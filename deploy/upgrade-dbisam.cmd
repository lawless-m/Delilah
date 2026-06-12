@echo off
setlocal
set "EXE=\\rivsts05\Software\Data Warehouse\duckdb\duckdb.exe"

rem A running session has the extension mapped as a DLL, so the install's
rem final move fails with "Access is denied" - bail out early instead.
tasklist /fi "imagename eq duckdb.exe" 2>nul | find /i "duckdb.exe" >nul && (
    echo ERROR: duckdb.exe is running - close all DuckDB sessions first.
    exit /b 1
)

rem Clean up leftover tmp downloads from previously failed installs
for /d %%V in ("%USERPROFILE%\.duckdb\extensions\*") do del /q "%%V\windows_amd64\dbisam.duckdb_extension.tmp-*" 2>nul

rem -init NUL skips the shared init.sql so dbisam is not loaded during install
"%EXE%" -unsigned -init NUL -c "FORCE INSTALL dbisam FROM 'https://dw.ramsden-international.com/duckdb-ext';" || exit /b 1
"%EXE%" -unsigned -init NUL -c "LOAD dbisam; SELECT extension_version AS dbisam_version FROM duckdb_extensions() WHERE extension_name='dbisam';"
