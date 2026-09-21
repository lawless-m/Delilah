#Requires -Version 7
<#
  deploy.ps1 -- build and deploy the dbisam DuckDB extension, Windows and Linux.

  This is the /deploy procedure (~/.claude/commands/deploy.md) written down for a C++
  DuckDB extension, after the 2026-09-21 deploy had to be worked out by hand:

    * Nothing keeps the live copies in step -- DuckDB only fetches an extension on
      INSTALL / FORCE INSTALL. There are:
        - the web repo on vsprod (windows_amd64 + linux_amd64), which
          deploy/upgrade-dbisam.cmd|.sh install from;
        - the rivsts05 share, which is what duckdb.bat sessions actually LOAD, because
          the share's init.sql sets extension_directory to the share;
        - ~/.duckdb on each Linux host, refreshed from the web repo by upgrade-dbisam.sh.
      Updating some and not others leaves part of the estate on the old binary.
    * Scheduled tasks on another server load the extension from the share and hold it
      open at more or less any time of day, so "close all sessions first" never
      reliably works. A plain overwrite is tried first; if the file is in
      use it is swapped by rename instead (Windows allows renaming a loaded DLL).
      Holders keep the old code until they restart. To see who they are, from an
      ADMIN shell:
        Get-SmbOpenFile -CimSession rivsts05 | ? Path -like '*dbisam.duckdb_extension'
    * cmake / ninja / cl are not on PATH on the dev hosts; they come from Visual Studio.
    * Linux is built on vsprod from its own checkout, which pulls from GitHub -- so
      Linux can only ever ship a PUSHED commit from a CLEAN tree. -SkipLinux does a
      Windows-only deploy, and is the only mode -AllowDirty is compatible with.

  Both builds are unit-tested and smoke-tested against live sem01 straight from their
  build directories BEFORE any live copy is touched. The smoke query is the Top-N LIKE
  regression fixed in f81b389: read-only, and fails loudly if pushed filters are
  being dropped.

  Usage:
    pwsh -File deploy.ps1
    pwsh -File deploy.ps1 -Note "why this deploy happened"
    pwsh -File deploy.ps1 -SkipLinux -AllowDirty
#>
[CmdletBinding()]
param(
    [string]   $ShareRoot  = '\\rivsts05\Software\Data Warehouse\duckdb',
    [string]   $WebHost    = 'vsprod',                    # serves the web repo AND builds Linux
    [string]   $WebRoot    = '/var/www/html/duckdb-ext',
    [string]   $LinuxRepo  = 'Git/Delilah',               # checkout on $WebHost, relative to $HOME
    [string[]] $LinuxHosts = @('vsprod', 'beast'),        # hosts with dbisam installed in ~/.duckdb
    [string]   $Note       = '',
    [switch]   $SkipLinux,
    [switch]   $AllowDirty
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo     = $PSScriptRoot
$assembly = 'dbisam.duckdb_extension'
$build    = Join-Path $repo 'build'
$built    = Join-Path $build $assembly
$history  = 'R:\Outputs\Parquets\deploy\deploy_history.sqlite'
$recorder = 'R:\Scripts\Record-Deploy.ps1'
$stamp    = Get-Date -Format 'yyyyMMdd-HHmmss'
$scratch  = Join-Path ([IO.Path]::GetTempPath()) "dbisam-deploy-$stamp"

# Read the DuckDB version and the web repo URL out of the files that already define
# them, so this script can never disagree with the build or with upgrade-dbisam.cmd
# about where the binary is meant to land.
$m = [regex]::Match((Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw), 'set\(DUCKDB_VERSION_NORMALIZED\s+"([^"]+)"\)')
if (-not $m.Success) { throw 'No DUCKDB_VERSION_NORMALIZED in CMakeLists.txt -- the deploy destination is undefined.' }
$ver = $m.Groups[1].Value
$m = [regex]::Match((Get-Content (Join-Path $repo 'deploy\upgrade-dbisam.cmd') -Raw), "FORCE INSTALL dbisam FROM '([^']+)'")
if (-not $m.Success) { throw 'No FORCE INSTALL url in deploy\upgrade-dbisam.cmd -- the web repo is undefined.' }
$webBase = $m.Groups[1].Value
$webUrl  = "$webBase/$ver/windows_amd64/$assembly.gz"
$webDir  = "$WebRoot/$ver/windows_amd64"
$linUrl  = "$webBase/$ver/linux_amd64/$assembly.gz"
$linDir  = "$WebRoot/$ver/linux_amd64"
$linBuilt     = "`$HOME/$LinuxRepo/build/$assembly"
$linInstalled = "`$HOME/.duckdb/extensions/$ver/linux_amd64/$assembly"

$duck        = Join-Path $ShareRoot 'duckdb.exe'
$shareInit   = Join-Path $ShareRoot 'init.sql'
$shareTarget = Join-Path $ShareRoot "$ver\windows_amd64\$assembly"

$smokeSql = "SELECT count(*), count(*) FILTER (WHERE code LIKE 'BPC-%') FROM " +
            "(SELECT code FROM sem01.customer WHERE code LIKE 'BPC-%' ORDER BY code DESC LIMIT 1000);"

function Step($n, $text) { Write-Host "`n[$n] $text" -ForegroundColor Cyan }

function Get-Sha($path) { (Get-FileHash $path -Algorithm SHA256).Hash }

# SHA-256 of the DECOMPRESSED body -- gzip output isn't byte-stable, the payload is.
function Get-GzBodySha($gzPath) {
    $in = [IO.File]::OpenRead($gzPath)
    try {
        $gz = [IO.Compression.GZipStream]::new($in, [IO.Compression.CompressionMode]::Decompress)
        try { (Get-FileHash -InputStream $gz -Algorithm SHA256).Hash } finally { $gz.Dispose() }
    } finally { $in.Dispose() }
}

function Get-WebSha($url) {
    $tmp = Join-Path $scratch "web-$([guid]::NewGuid().ToString('n')).gz"
    Invoke-WebRequest $url -OutFile $tmp -TimeoutSec 300 | Out-Null
    try { Get-GzBodySha $tmp } finally { Remove-Item $tmp -Force }
}

# Run SQL through the share's duckdb.exe and return the last stdout line. $sql goes via
# a scratch file: it can hold the sem01 ATTACH line (credentials) and must not be echoed.
function Invoke-Duck([string] $init, [string] $sql) {
    $file = Join-Path $scratch "q-$([guid]::NewGuid().ToString('n')).sql"
    Set-Content $file $sql
    try {
        $out = & $duck -unsigned -init $init -csv -noheader -f $file
        if ($LASTEXITCODE -ne 0) { throw "duckdb exited $LASTEXITCODE" }
        ($out | Select-Object -Last 1)
    } finally { Remove-Item $file -Force }
}

function Assert-Smoke([string] $line, [string] $what) {
    $n = $line -split ','
    if ($n.Count -ne 2 -or [int]$n[0] -le 0 -or $n[0] -ne $n[1]) {
        throw "$what smoke test failed: Top-N LIKE returned '$line' (rows,matching) -- pushed filter dropped?"
    }
    Write-Host "  $what`: $($n[0]) Top-N rows, all match the LIKE filter"
}

# Overwrite, or swap by rename when a holder (see header) has the live file loaded.
function Install-ShareFile($src) {
    try { Copy-Item $src $shareTarget -Force; return }
    catch { Write-Warning "  overwrite refused ($($_.Exception.Message.Trim())) -- swapping by rename" }
    $inuse = "$assembly.inuse-$stamp"
    Rename-Item $shareTarget $inuse
    try { Copy-Item $src $shareTarget }
    catch { Rename-Item (Join-Path (Split-Path $shareTarget) $inuse) $assembly; throw }
}

# Run a bash script on a remote host in a LOGIN shell (duckdb is only on that PATH) and
# return its stdout lines. The script travels on stdin but is read in full before it
# runs, so a command inside it that reads stdin cannot swallow the rest; tr strips the
# CRs PowerShell adds. Remote stderr (build progress) goes straight to the console.
function Invoke-Ssh([string] $sshHost, [string] $script) {
    $out = "set -e`n$script`n" | ssh -o BatchMode=yes $sshHost 'bash -l -c "$(tr -d ''\r'')"'
    if ($LASTEXITCODE -ne 0) { throw "ssh $sshHost failed ($LASTEXITCODE)" }
    @($out) -replace '\x1b\[[0-9;]*m', ''
}

function Get-RemoteSha([string] $sshHost, [string] $path, [switch] $Gz) {
    $cmd = $Gz ? "gunzip -c `"$path`" | sha256sum | cut -c1-64" : "sha256sum `"$path`" | cut -c1-64"
    (Invoke-Ssh $sshHost $cmd | Select-Object -Last 1).ToUpper()
}

# Same smoke query, remotely. $prelude emits any SQL that must come first (LOAD/ATTACH --
# the ATTACH line is grepped out of ~/.duckdbrc on the host and never leaves it).
function Assert-LinuxSmoke([string] $sshHost, [string] $prelude, [string] $init, [string] $pathLike, [string] $what) {
    $out = Invoke-Ssh $sshHost (@'
umask 077; Q=$(mktemp /tmp/dbisam-smoke.XXXXXX); trap 'rm -f "$Q"' EXIT
{ {PRELUDE}
cat <<'SQL'
SELECT install_path FROM duckdb_extensions() WHERE extension_name='dbisam';
{SQL}
SQL
} > "$Q"
duckdb -init {INIT} -csv -noheader -f "$Q" | tail -2
'@).Replace('{PRELUDE}', $prelude).Replace('{SQL}', $smokeSql).Replace('{INIT}', $init)
    if ($out.Count -ne 2 -or $out[0] -notlike $pathLike) { throw "$what loaded dbisam from '$($out[0])', expected $pathLike." }
    Assert-Smoke $out[1] $what
}

Step 1 'Working tree'
$dirty = git -C $repo status --porcelain
if ($dirty -and -not $AllowDirty) {
    throw "Working tree is dirty -- deployed bytes must trace back to a commit. Commit first, or pass -AllowDirty.`n$dirty"
}
$full    = (git -C $repo rev-parse HEAD).Trim()
$sha     = $full.Substring(0, 7)
$subject = (git -C $repo log -1 --pretty=%s).Trim()
if ($dirty) { Write-Warning "Deploying uncommitted work -- what ships is NOT $sha." }
Write-Host "  $sha  $subject"
if (-not $SkipLinux) {
    if ($dirty) { throw 'Linux is built from the pushed commit, so it cannot ship a dirty tree. Add -SkipLinux.' }
    git -C $repo fetch -q origin
    if (-not (git -C $repo branch -r --contains $full)) {
        throw "$sha is not on origin -- $WebHost builds Linux from GitHub. Push first, or pass -SkipLinux."
    }
}

New-Item -ItemType Directory $scratch | Out-Null
$shareBackup = $null; $shareChanged = $false; $webChanged = $false; $linWebChanged = $false
$bak = "bak-$stamp"; $linHostsChanged = @(); $linFrom = $null; $linHash = $null
try {
    Step 2 'Currently deployed'
    $exeVer = (& $duck -version)
    if ($exeVer -notlike "$ver *") { throw "Share duckdb.exe is '$exeVer' but the extension builds against $ver -- it would not load." }
    $fromHash = Get-Sha $shareTarget
    $webFrom  = Get-WebSha $webUrl
    Write-Host ("  share        {0}  ({1:n0} bytes)" -f $fromHash, (Get-Item $shareTarget).Length)
    Write-Host  "  web windows  $webFrom"
    if ($webFrom -ne $fromHash) { Write-Warning '  Share and web repo were ALREADY out of step.' }
    if (-not $SkipLinux) {
        $linFrom = Get-WebSha $linUrl
        Write-Host "  web linux    $linFrom"
        foreach ($h in $LinuxHosts) {
            $s = Get-RemoteSha $h $linInstalled
            Write-Host "  $($h.PadRight(12)) $s"
            if ($s -ne $linFrom) { Write-Warning "  $h and the web repo were ALREADY out of step." }
        }
    }

    Step 3 'Build + unit tests (Windows)'
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio install with the C++ tools found (vswhere).' }
    $cmakeRoot = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    $env:PATH  = "$cmakeRoot\CMake\bin;$cmakeRoot\Ninja;$env:PATH"
    # vcvars grumbles on stderr about its own vswhere lookup; if it truly fails, cmake does.
    cmd /c ("call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1" +
            " && cmake -S `"$repo`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release" +
            " && cmake --build `"$build`"" +
            " && ctest --test-dir `"$build`" --output-on-failure") | Select-Object -Last 4 | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "build/ctest exited $LASTEXITCODE" }
    $toHash = Get-Sha $built
    $toSize = (Get-Item $built).Length
    Write-Host ("  built  {0:n0} bytes  {1}" -f $toSize, $toHash)
    # A healthy build statically links DuckDB (~22 MB). Anything tiny linked the wrong thing.
    if ($toSize -lt 10MB) { throw "$built is only $toSize bytes -- not a statically linked extension." }
    if ($toHash -eq $fromHash -and $toHash -eq $webFrom) { Write-Warning '  Bytes identical to what is already deployed.' }

    if (-not $SkipLinux) {
        Step 4 "Build + unit tests (Linux, on $WebHost)"
        $r = Invoke-Ssh $WebHost (@'
cd "$HOME/{REPO}"
test -z "$(git status --porcelain)" || { echo "remote checkout is dirty" >&2; exit 1; }
git pull -q --ff-only
test "$(git rev-parse HEAD)" = "{FULL}" || { echo "remote HEAD is $(git rev-parse --short HEAD), not {FULL}" >&2; exit 1; }
L=$(mktemp /tmp/dbisam-build.XXXXXX); trap 'rm -f "$L"' EXIT
# -j3 of 4 cores: this is a production box.
nice -n 10 cmake --build build -j3 >"$L" 2>&1 || { tail -25 "$L" >&2; exit 1; }
(cd build && ctest --output-on-failure) >"$L" 2>&1 || { tail -40 "$L" >&2; exit 1; }
grep 'tests passed' "$L" >&2
echo "$(sha256sum build/{ASM} | cut -c1-64) $(stat -c %s build/{ASM})"
'@).Replace('{REPO}', $LinuxRepo).Replace('{FULL}', $full).Replace('{ASM}', $assembly)
        $linHash, $linSize = ($r | Select-Object -Last 1) -split ' '
        $linHash = $linHash.ToUpper()
        Write-Host ("  built  {0:n0} bytes  {1}" -f [long]$linSize, $linHash)
        if ([long]$linSize -lt 10MB) { throw "Linux build is only $linSize bytes -- not a statically linked extension." }
        if ($linHash -eq $linFrom) { Write-Warning '  Bytes identical to what is already deployed.' }
    }

    Step 5 'Smoke test the new builds against sem01, before touching anything live'
    $attach = (Select-String -Path $shareInit -Pattern '\bAS sem01\b' | Select-Object -First 1).Line
    if (-not $attach) { throw "No sem01 ATTACH line in $shareInit." }
    Assert-Smoke (Invoke-Duck 'NUL' "LOAD '$($built -replace '\\','/')';`n$attach`n$smokeSql") 'windows build\'
    if (-not $SkipLinux) {
        # No path assertion here ('*'): install_path reports the INSTALLED copy's .info
        # metadata even after an explicit-path LOAD, which -init /dev/null makes unambiguous.
        Assert-LinuxSmoke $WebHost "echo `"LOAD '$linBuilt';`"; grep 'AS sem01' ~/.duckdbrc" '/dev/null' '*' 'linux build/'
    }

    Step 6 'Backup'
    $shareBackup = "$shareTarget.$bak"
    Copy-Item $shareTarget $shareBackup
    if ((Get-Sha $shareBackup) -ne $fromHash) { throw "Backup $shareBackup does not match the live extension." }
    Write-Host "  $shareBackup"
    Invoke-Ssh $WebHost "cp -p '$webDir/$assembly.gz' '$webDir/$assembly.gz.$bak'" | Out-Null
    Write-Host "  ${WebHost}:$webDir/$assembly.gz.$bak"
    if (-not $SkipLinux) {
        Invoke-Ssh $WebHost "cp -p '$linDir/$assembly.gz' '$linDir/$assembly.gz.$bak'" | Out-Null
        Write-Host "  ${WebHost}:$linDir/$assembly.gz.$bak"
        foreach ($h in $LinuxHosts) {
            Invoke-Ssh $h "cp -p `"$linInstalled`" `"$linInstalled.$bak`"" | Out-Null
            Write-Host "  ${h}:~/.duckdb/.../$assembly.$bak"
        }
    }

    Step 7 "Deploy -> $shareTarget"
    $shareChanged = $true
    Install-ShareFile $built
    if ((Get-Sha $shareTarget) -ne $toHash) { throw 'Share copy does not match the build.' }
    Write-Host "  verified $toHash"

    Step 8 "Deploy -> $webUrl"
    $gzLocal = Join-Path $scratch "$assembly.gz"
    $in = [IO.File]::OpenRead($built); $out = [IO.File]::Create($gzLocal)
    try {
        $gz = [IO.Compression.GZipStream]::new($out, [IO.Compression.CompressionLevel]::SmallestSize)
        try { $in.CopyTo($gz) } finally { $gz.Dispose() }
    } finally { $in.Dispose(); $out.Dispose() }
    if ((Get-GzBodySha $gzLocal) -ne $toHash) { throw 'Local gzip does not round-trip to the build.' }
    $webChanged = $true
    scp -q -o BatchMode=yes $gzLocal "${WebHost}:$webDir/$assembly.gz.upload-tmp"
    if ($LASTEXITCODE -ne 0) { throw "scp to $WebHost failed ($LASTEXITCODE)" }
    Invoke-Ssh $WebHost "cd '$webDir'; chmod 644 $assembly.gz.upload-tmp; mv -f $assembly.gz.upload-tmp $assembly.gz" | Out-Null
    if ((Get-WebSha $webUrl) -ne $toHash) { throw "Body served at $webUrl does not match the build." }
    Write-Host "  verified over HTTPS $toHash"

    if (-not $SkipLinux) {
        Step 9 "Deploy -> $linUrl"
        $linWebChanged = $true
        Invoke-Ssh $WebHost ("cd '$linDir'; gzip -9 -c `"$linBuilt`" > $assembly.gz.upload-tmp; chmod 644 $assembly.gz.upload-tmp; " +
                             "mv -f $assembly.gz.upload-tmp $assembly.gz") | Out-Null
        if ((Get-WebSha $linUrl) -ne $linHash) { throw "Body served at $linUrl does not match the Linux build." }
        Write-Host "  verified over HTTPS $linHash"

        Step 10 "Install on $($LinuxHosts -join ', ') (deploy/upgrade-dbisam.sh, from the web repo)"
        $upgrade = Get-Content (Join-Path $repo 'deploy\upgrade-dbisam.sh') -Raw
        foreach ($h in $LinuxHosts) {
            $linHostsChanged += $h
            Invoke-Ssh $h $upgrade | Out-Null
            if ((Get-RemoteSha $h $linInstalled) -ne $linHash) { throw "$h installed copy does not match the Linux build." }
            Write-Host "  $($h.PadRight(12)) verified $linHash"
        }
    }

    Step 11 'Smoke test the real entry points'
    $loaded = Invoke-Duck $shareInit "SELECT install_path FROM duckdb_extensions() WHERE extension_name='dbisam';"
    if ($loaded -ne $shareTarget) { throw "Launcher loaded dbisam from '$loaded', not $shareTarget." }
    Assert-Smoke (Invoke-Duck $shareInit $smokeSql) 'share init.sql'
    if (-not $SkipLinux) {
        foreach ($h in $LinuxHosts) { Assert-LinuxSmoke $h ':' '~/.duckdbrc' "*/.duckdb/extensions/$ver/linux_amd64/$assembly" "$h ~/.duckdbrc" }
    }
}
catch {
    Write-Host "`nDEPLOY FAILED: $_" -ForegroundColor Red
    function Report($what, $now, $want, $kept) {
        if ($now -eq $want) { Write-Host "  $what rollback verified: $now" -ForegroundColor Yellow }
        else { Write-Host "  $what ROLLBACK FAILED -- is $now, expected $want. Backup kept at $kept" -ForegroundColor Red }
    }
    if ($shareChanged) {
        try { Install-ShareFile $shareBackup } catch { Write-Host "  $_" -ForegroundColor Red }
        Report 'share' (Get-Sha $shareTarget) $fromHash $shareBackup
    }
    if ($webChanged) {
        try { Invoke-Ssh $WebHost "cp -p '$webDir/$assembly.gz.$bak' '$webDir/$assembly.gz'" | Out-Null; Report 'web windows' (Get-WebSha $webUrl) $webFrom "$webDir/$assembly.gz.$bak" }
        catch { Write-Host "  web windows ROLLBACK FAILED -- $_. Backup kept at $webDir/$assembly.gz.$bak" -ForegroundColor Red }
    }
    if ($linWebChanged) {
        try { Invoke-Ssh $WebHost "cp -p '$linDir/$assembly.gz.$bak' '$linDir/$assembly.gz'" | Out-Null; Report 'web linux' (Get-WebSha $linUrl) $linFrom "$linDir/$assembly.gz.$bak" }
        catch { Write-Host "  web linux ROLLBACK FAILED -- $_. Backup kept at $linDir/$assembly.gz.$bak" -ForegroundColor Red }
    }
    foreach ($h in $linHostsChanged) {
        # Restores the host's OWN previous copy, which may differ from the old web body.
        try { Invoke-Ssh $h "cp -p `"$linInstalled.$bak`" `"$linInstalled`"" | Out-Null; Write-Host "  $h restored from .$bak ($(Get-RemoteSha $h $linInstalled))" -ForegroundColor Yellow }
        catch { Write-Host "  $h ROLLBACK FAILED -- $_. Backup kept at ~/.duckdb/.../$assembly.$bak" -ForegroundColor Red }
    }
    Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
    throw
}

Step 12 'Record'
$fullNote = "Commit ${sha}: $subject. Share + web repo ($ver/windows_amd64); web was $webFrom. Smoke: Top-N LIKE on sem01 OK from build\ and via share init.sql."
if ($Note) { $fullNote = "$Note $fullNote" }
& $recorder -SqliteOut $history -Assembly $assembly -Project 'Delilah' `
    -FromHash $fromHash -ToHash $toHash -DeployedBy $env:USERNAME -Note $fullNote
if (-not $SkipLinux) {
    $linNote = "Commit ${sha}: $subject. Built on $WebHost; web repo ($ver/linux_amd64) + ~/.duckdb on $($LinuxHosts -join ', '). Smoke: Top-N LIKE on sem01 OK from build/ and via ~/.duckdbrc on each host."
    if ($Note) { $linNote = "$Note $linNote" }
    & $recorder -SqliteOut $history -Assembly "$assembly.linux_amd64" -Project 'Delilah' `
        -FromHash $linFrom -ToHash $linHash -DeployedBy $env:USERNAME -Note $linNote
}

Step 13 'Clean up'
Remove-Item $shareBackup -Force; Write-Host "  removed $shareBackup"
Invoke-Ssh $WebHost "rm -f '$webDir/$assembly.gz.$bak'" | Out-Null; Write-Host "  removed ${WebHost}:$webDir/$assembly.gz.$bak"
if (-not $SkipLinux) {
    Invoke-Ssh $WebHost "rm -f '$linDir/$assembly.gz.$bak'" | Out-Null; Write-Host "  removed ${WebHost}:$linDir/$assembly.gz.$bak"
    foreach ($h in $LinuxHosts) { Invoke-Ssh $h "rm -f `"$linInstalled.$bak`"" | Out-Null; Write-Host "  removed ${h}:~/.duckdb/.../$assembly.$bak" }
}
# Leftovers from earlier rename swaps; still-loaded ones refuse and are left for next time.
Get-ChildItem (Split-Path $shareTarget) -Filter "$assembly.inuse-*" | ForEach-Object {
    try { Remove-Item $_.FullName -Force; Write-Host "  removed $($_.Name)" } catch { Write-Host "  $($_.Name) still in use -- left" }
}
Remove-Item $scratch -Recurse -Force

Write-Host "`nDeployed $assembly  windows $fromHash -> $toHash" -ForegroundColor Green
if ($SkipLinux) { Write-Host 'linux_amd64 was SKIPPED (-SkipLinux).' -ForegroundColor Yellow }
else { Write-Host "Deployed $assembly  linux   $linFrom -> $linHash" -ForegroundColor Green }
