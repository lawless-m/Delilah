#Requires -Version 7
<#
  deploy.ps1 -- build and deploy the Windows dbisam DuckDB extension.

  This is the /deploy procedure (~/.claude/commands/deploy.md) written down for a C++
  DuckDB extension, after the 2026-09-21 deploy had to be worked out by hand:

    * There are TWO live copies and nothing keeps them in step -- DuckDB only fetches
      an extension on INSTALL / FORCE INSTALL:
        - the web repo on vsprod, which deploy/upgrade-dbisam.cmd installs from;
        - the rivsts05 share, which is what duckdb.bat sessions actually LOAD, because
          the share's init.sql sets extension_directory to the share.
      Updating one and not the other leaves half the estate on the old binary.
    * Scheduled tasks on another server load the extension from the share and hold it
      open at more or less any time of day, so "close all sessions first" never
      reliably works. A plain overwrite is tried first; if the file is in
      use it is swapped by rename instead (Windows allows renaming a loaded DLL).
      Holders keep the old code until they restart. To see who they are, from an
      ADMIN shell:
        Get-SmbOpenFile -CimSession rivsts05 | ? Path -like '*dbisam.duckdb_extension'
    * cmake / ninja / cl are not on PATH on the dev hosts; they come from Visual Studio.

  The new build is smoke-tested against live sem01 straight from build\ BEFORE either
  live copy is touched. The smoke query is the Top-N LIKE regression fixed in f81b389:
  it is read-only and fails loudly if pushed filters are being dropped.

  NOT covered: the linux_amd64 build. That is built on vsprod (~/Git/Delilah, needs the
  commit pushed) and published to the same web repo by hand.

  Usage:
    pwsh -File deploy.ps1
    pwsh -File deploy.ps1 -Note "why this deploy happened"
#>
[CmdletBinding()]
param(
    [string] $ShareRoot = '\\rivsts05\Software\Data Warehouse\duckdb',
    [string] $WebHost   = 'vsprod',
    [string] $WebRoot   = '/var/www/html/duckdb-ext',
    [string] $Note      = '',
    [switch] $AllowDirty
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo     = $PSScriptRoot
$assembly = 'dbisam.duckdb_extension'
$platform = 'windows_amd64'
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
$webUrl = "$($m.Groups[1].Value)/$ver/$platform/$assembly.gz"
$webDir = "$WebRoot/$ver/$platform"

$duck        = Join-Path $ShareRoot 'duckdb.exe'
$shareInit   = Join-Path $ShareRoot 'init.sql'
$shareTarget = Join-Path $ShareRoot "$ver\$platform\$assembly"

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

function Get-WebSha {
    $tmp = Join-Path $scratch 'web-current.gz'
    Invoke-WebRequest $webUrl -OutFile $tmp -TimeoutSec 300 | Out-Null
    Get-GzBodySha $tmp
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

function Invoke-Remote([string] $command) {
    ssh -o BatchMode=yes $WebHost "set -e; cd '$webDir'; $command"
    if ($LASTEXITCODE -ne 0) { throw "ssh $WebHost failed ($LASTEXITCODE): $command" }
}

Step 1 'Working tree'
$dirty = git -C $repo status --porcelain
if ($dirty -and -not $AllowDirty) {
    throw "Working tree is dirty -- deployed bytes must trace back to a commit. Commit first, or pass -AllowDirty.`n$dirty"
}
$sha     = (git -C $repo rev-parse --short HEAD).Trim()
$subject = (git -C $repo log -1 --pretty=%s).Trim()
if ($dirty) { Write-Warning "Deploying uncommitted work -- what ships is NOT $sha." }
Write-Host "  $sha  $subject"

New-Item -ItemType Directory $scratch | Out-Null
$shareBackup = $null; $webBackup = $null; $shareChanged = $false; $webChanged = $false
try {
    Step 2 'Currently deployed'
    $exeVer = (& $duck -version)
    if ($exeVer -notlike "$ver *") { throw "Share duckdb.exe is '$exeVer' but the extension builds against $ver -- it would not load." }
    $fromHash = Get-Sha $shareTarget
    $webFrom  = Get-WebSha
    Write-Host ("  share  {0:n0} bytes  {1}" -f (Get-Item $shareTarget).Length, $fromHash)
    Write-Host  "  web    $webFrom"
    if ($webFrom -ne $fromHash) { Write-Warning '  Share and web repo were ALREADY out of step.' }

    Step 3 'Build + unit tests'
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio install with the C++ tools found (vswhere).' }
    $cmakeRoot = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    $env:PATH  = "$cmakeRoot\CMake\bin;$cmakeRoot\Ninja;$env:PATH"
    # vcvars grumbles on stderr about its own vswhere lookup; if it truly fails, cmake does.
    cmd /c ("call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1" +
            " && cmake -S `"$repo`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release" +
            " && cmake --build `"$build`"" +
            " && ctest --test-dir `"$build`" --output-on-failure") | Select-Object -Last 15 | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "build/ctest exited $LASTEXITCODE" }
    $toHash = Get-Sha $built
    $toSize = (Get-Item $built).Length
    Write-Host ("  built  {0:n0} bytes  {1}" -f $toSize, $toHash)
    # A healthy build statically links DuckDB (~22 MB). Anything tiny linked the wrong thing.
    if ($toSize -lt 10MB) { throw "$built is only $toSize bytes -- not a statically linked extension." }
    if ($toHash -eq $fromHash -and $toHash -eq $webFrom) { Write-Warning '  Bytes identical to what is already deployed.' }

    Step 4 'Smoke test the new build against sem01, before touching anything live'
    $attach = (Select-String -Path $shareInit -Pattern '\bAS sem01\b' | Select-Object -First 1).Line
    if (-not $attach) { throw "No sem01 ATTACH line in $shareInit." }
    Assert-Smoke (Invoke-Duck 'NUL' "LOAD '$($built -replace '\\','/')';`n$attach`n$smokeSql") 'build\'

    Step 5 'Backup'
    $shareBackup = "$shareTarget.bak-$stamp"
    Copy-Item $shareTarget $shareBackup
    if ((Get-Sha $shareBackup) -ne $fromHash) { throw "Backup $shareBackup does not match the live extension." }
    Write-Host "  $shareBackup"
    $webBackup = "$assembly.gz.bak-$stamp"
    Invoke-Remote "cp -p $assembly.gz $webBackup"
    Write-Host "  ${WebHost}:$webDir/$webBackup"

    Step 6 "Deploy -> $shareTarget"
    $shareChanged = $true
    Install-ShareFile $built
    if ((Get-Sha $shareTarget) -ne $toHash) { throw 'Share copy does not match the build.' }
    Write-Host "  verified $toHash"

    Step 7 "Deploy -> $webUrl"
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
    Invoke-Remote "chmod 644 $assembly.gz.upload-tmp; mv -f $assembly.gz.upload-tmp $assembly.gz"
    if ((Get-WebSha) -ne $toHash) { throw "Body served at $webUrl does not match the build." }
    Write-Host "  verified over HTTPS $toHash"

    Step 8 'Smoke test the real entry point (share duckdb.exe + init.sql)'
    $loaded = Invoke-Duck $shareInit "SELECT install_path FROM duckdb_extensions() WHERE extension_name='dbisam';"
    if ($loaded -ne $shareTarget) { throw "Launcher loaded dbisam from '$loaded', not $shareTarget." }
    Assert-Smoke (Invoke-Duck $shareInit $smokeSql) 'share'
}
catch {
    Write-Host "`nDEPLOY FAILED: $_" -ForegroundColor Red
    if ($shareChanged) {
        Write-Host "Rolling back share from $shareBackup ..." -ForegroundColor Yellow
        try { Install-ShareFile $shareBackup } catch { Write-Host "  $_" -ForegroundColor Red }
        $rolled = Get-Sha $shareTarget
        if ($rolled -eq $fromHash) { Write-Host "  Rollback verified: $rolled" -ForegroundColor Yellow }
        else { Write-Host "  ROLLBACK FAILED -- share is $rolled, expected $fromHash. Backup kept at $shareBackup" -ForegroundColor Red }
    }
    if ($webChanged) {
        Write-Host "Rolling back web repo from $webBackup ..." -ForegroundColor Yellow
        try {
            Invoke-Remote "cp -p $webBackup $assembly.gz"
            $rolled = Get-WebSha
            if ($rolled -eq $webFrom) { Write-Host "  Rollback verified: $rolled" -ForegroundColor Yellow }
            else { Write-Host "  ROLLBACK FAILED -- web is $rolled, expected $webFrom. Backup kept at $webDir/$webBackup" -ForegroundColor Red }
        } catch { Write-Host "  ROLLBACK FAILED -- $_. Backup kept at $webDir/$webBackup" -ForegroundColor Red }
    }
    Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
    throw
}

Step 9 'Record'
$fullNote = "Commit ${sha}: $subject. Share + web repo ($ver/$platform); web was $webFrom. Smoke: Top-N LIKE on sem01 OK from build\ and via share init.sql."
if ($Note) { $fullNote = "$Note $fullNote" }
& $recorder -SqliteOut $history -Assembly $assembly -Project 'Delilah' `
    -FromHash $fromHash -ToHash $toHash -DeployedBy $env:USERNAME -Note $fullNote

Step 10 'Clean up'
Remove-Item $shareBackup -Force; Write-Host "  removed $shareBackup"
Invoke-Remote "rm -f $webBackup"; Write-Host "  removed ${WebHost}:$webDir/$webBackup"
# Leftovers from earlier rename swaps; still-loaded ones refuse and are left for next time.
Get-ChildItem (Split-Path $shareTarget) -Filter "$assembly.inuse-*" | ForEach-Object {
    try { Remove-Item $_.FullName -Force; Write-Host "  removed $($_.Name)" } catch { Write-Host "  $($_.Name) still in use -- left" }
}
Remove-Item $scratch -Recurse -Force

Write-Host "`nDeployed $assembly  $fromHash -> $toHash" -ForegroundColor Green
Write-Host 'linux_amd64 is NOT covered by this script -- see the header.' -ForegroundColor Yellow
