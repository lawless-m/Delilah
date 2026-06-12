#!/bin/sh
# Linux counterpart of upgrade-dbisam.cmd. No running-session check is
# needed here: POSIX lets FORCE INSTALL replace the extension file even
# while a session has it mapped - running sessions keep the old code,
# new sessions pick up the new one.
set -e

command -v duckdb >/dev/null 2>&1 || { echo "ERROR: duckdb not on PATH" >&2; exit 1; }

# Clean up leftover tmp downloads from previously failed installs
rm -f "$HOME"/.duckdb/extensions/*/linux_*/dbisam.duckdb_extension.tmp-* 2>/dev/null || true

# -init /dev/null skips ~/.duckdbrc so dbisam is not loaded during install
duckdb -unsigned -init /dev/null -c "LOAD httpfs; FORCE INSTALL dbisam FROM 'https://dw.ramsden-international.com/duckdb-ext';"
duckdb -unsigned -init /dev/null -c "LOAD dbisam; SELECT extension_version AS dbisam_version FROM duckdb_extensions() WHERE extension_name='dbisam';"
