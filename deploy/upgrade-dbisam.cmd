@echo off
setlocal
set "DIR=\\rivsts05\Software\Data Warehouse\duckdb"
set "EXE=%DIR%\duckdb.exe"

rem The share's init.sql sets extension_directory to the share, so that is
rem where duckdb.bat sessions LOAD dbisam from. Installing into the default
rem %USERPROFILE%\.duckdb (what this script did until 2026-09-22) updates a
rem copy nothing reads. Install into the share instead - same setting.

rem A running session has the extension mapped as a DLL, so the install's
rem final move fails with "Access is denied" - bail out early instead.
rem Sessions on OTHER machines (scheduled tasks included) hold the share
rem copy too and cause the same failure; an admin can list them with
rem   Get-SmbOpenFile -CimSession rivsts05 ^| ? Path -like '*dbisam.duckdb_extension'
rem Replacing the live file also needs Modify on it (it is admin-owned).
tasklist /fi "imagename eq duckdb.exe" 2>nul | find /i "duckdb.exe" >nul && (
    echo ERROR: duckdb.exe is running - close all DuckDB sessions first.
    exit /b 1
)

rem Clean up leftover tmp downloads from previously failed installs
for /d %%V in ("%DIR%\v*") do del /q "%%V\windows_amd64\dbisam.duckdb_extension.tmp-*" 2>nul

rem -init NUL skips the shared init.sql so dbisam is not loaded during install
"%EXE%" -unsigned -init NUL -c "SET extension_directory = '%DIR%'; FORCE INSTALL dbisam FROM 'https://dw.ramsden-international.com/duckdb-ext';" || exit /b 1
"%EXE%" -unsigned -init NUL -c "SET extension_directory = '%DIR%'; LOAD dbisam; SELECT extension_version AS dbisam_version, install_path FROM duckdb_extensions() WHERE extension_name='dbisam';"
