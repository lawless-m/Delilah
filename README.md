# Delilah

DuckDB extension that reads from DBISAM 4 databases over the native
Exportmaster TCP protocol — no ODBC driver required.

```sql
LOAD 'dbisam';
ATTACH 'em://user:pw@host/CATALOG' AS em (TYPE dbisam, READ_ONLY);
SELECT CODE, CPYNAME, NOTES FROM em.CUSTOMER WHERE EVCUSTOMER LIMIT 10;
COPY (SELECT * FROM em.ORDERH) TO 'orderh.parquet';
```

## Why

The DBISAM ODBC driver shipped by Elevate Software is Windows-only and
has four compounding bugs in its bulk-fetch path that cause silent row
loss (see `KNOWN_BUGS.md §B1` in [MrsFlow][mrsflow]). Delilah speaks
the wire protocol directly: cross-platform, correct, with the
ergonomics of DuckDB SQL on top.

Sibling implementations of the same protocol live in
[MrsFlow][mrsflow] (Rust) and [ExportKing][exportking] (.NET / ADO.NET).
The wire protocol itself was reverse-engineered separately and
documented in [Derek][derek].

## Build

Requires CMake ≥ 3.21, GCC ≥ 10 (or equivalent), and a working network.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The build pulls DuckDB v1.5.2 via FetchContent the first time
(~150 MB source, several minutes). The resulting extension is at
`build/dbisam.duckdb_extension`.

DuckDB itself must be **unsigned-extension-aware** to load it during
development. Either start `duckdb -unsigned` or use a wrapper script
that adds the flag.

```sh
duckdb -unsigned -c "LOAD '$PWD/build/dbisam.duckdb_extension'; SELECT dbisam_version();"
```

## Two ways in

### `dbisam_scan(host, user, pw, catalog, sql)` — one-shot table function

For ad-hoc queries that don't need DuckDB's catalog plumbing.

```sql
SELECT * FROM dbisam_scan(
    'YOURHOST', 'YOURUSER', 'YOURPASSWORD', 'YOURCATALOG',
    'SELECT CODE, CPYNAME FROM CUSTOMER WHERE EVCUSTOMER = TRUE');
```

Named parameters:
- `encrypt_password` (default `'elevatesoft'`)
- `port` (default `12005`)
- `compression` (default `TRUE`)
- `top` — append a DBISAM `TOP n` clause to the SQL, so the server
  applies the cap instead of us streaming-and-stopping.

### `ATTACH 'em://...' AS x (TYPE dbisam, READ_ONLY)` — full catalog

Tables show up under a DuckDB schema; you can query them naturally.

```sql
ATTACH 'em://YOURUSER:YOURPASSWORD@YOURHOST:12005/YOURCATALOG'
    AS em (TYPE dbisam, READ_ONLY);

SHOW TABLES FROM em;
SELECT count(*) FROM em.CUSTOMER WHERE NOTES IS NOT NULL;
SELECT CODE, CPYNAME FROM em.CUSTOMER
   WHERE CODE LIKE '1-3%' OR CODE IN ('400017', '38-1114');
```

Connection-string options can also be passed via the `ATTACH (...)` clause:

```sql
ATTACH 'YOURHOST/YOURCATALOG' AS em
    (TYPE dbisam, READ_ONLY,
     USER 'YOURUSER', PASSWORD 'YOURPASSWORD',
     ENCRYPT_PASSWORD 'elevatesoft', PORT 12005, COMPRESSION TRUE);
```

### Schema loading: lazy (default) vs `EAGER_SCHEMA`

Listing the catalog (`SHOW TABLES`) only needs table *names*, so by default
Delilah does **not** probe each table's columns up front — enumeration is
instant even on catalogs with hundreds of tables, and column schemas are
fetched on demand when you actually query a table.

The trade-off: catalog-*wide* column introspection
(`information_schema.columns` across all tables, `SHOW ALL TABLES`, and the
JDBC `getColumns` metadata a GUI like **DBeaver** uses to populate its
column browser) reports a table's columns as empty until that table has
been queried directly. If you browse the catalog in such a tool, attach
with `EAGER_SCHEMA true`:

```sql
ATTACH 'YOURHOST/YOURCATALOG' AS em (TYPE dbisam, READ_ONLY, EAGER_SCHEMA true);
```

This probes every table's schema once on first catalog access (serial — the
server rejects concurrent login storms) and caches it for the session, so
all column metadata is complete. Expect a one-off delay proportional to the
table count (~15 s for ~600 tables); subsequent access is instant.

## What works

| Capability               | Where                                                                       |
|--------------------------|-----------------------------------------------------------------------------|
| Projection pushdown      | Only the requested columns travel over the wire.                            |
| `WHERE` filter pushdown  | `=`, `<>`, `<`, `>`, `<=`, `>=`, `IN`, `IS [NOT] NULL`, AND/OR, LIKE-prefix.|
| `LIMIT n` pushdown       | Emits a trailing DBISAM `TOP n` so the server caps early (no full prepare); falls back to first-batch sizing when a non-pushable filter sits above the scan. |
| Streaming scan           | Bounded memory; no whole-table materialisation in the extension.            |
| BLOB / Memo columns      | Auto-resolved per row via `OpenBlob` / `FreeBlob`, PK auto-injected.        |
| `COPY (…) TO 'x.parquet'`| Win-1252 → UTF-8 transcoded at the boundary; parquet readback is clean.     |
| Case-insensitive idents  | DBISAM-compatible; double-quoted form also accepted per the DCG grammar.    |

DDL/DML throw cleanly with "DBISAM catalogs are read-only". This is
deliberate — Delilah is SELECT-only by design.

## DBISAM quirks worth knowing

DBISAM 4's SQL dialect diverges from ANSI in a few specific places.
Delilah compensates where needed; if you're writing the SQL yourself
(via `dbisam_scan`), keep these in mind:

- **`TOP n` is a *trailing* clause**: `SELECT * FROM CUSTOMER TOP 5`,
  not `SELECT TOP 5 * FROM CUSTOMER`. Use the `top` named parameter
  if you want Delilah to append it for you.
- **`WHERE col <> x` includes NULL rows** (DBISAM treats `NULL <> x` as
  TRUE). Delilah's pushdown wraps this as `(col <> x AND col IS NOT NULL)`
  to enforce ANSI semantics — if you're writing the SQL yourself, do
  the same.
- **Other comparisons are ANSI-safe**: `=`, `<`, `>`, `<=`, `>=`, `IN`,
  `NOT IN`, `IS [NOT] NULL` all exclude NULLs the same way ANSI does.

The authoritative grammar is the Prolog DCG in [Dibdog][dibdog].

## Tests

```sh
cd build && ctest
```

Seven unit-test executables (wire/framing/crypto, message builders,
schema/row decoders, blob slot/bookmark/response codecs,
Windows-1252→UTF-8 transcoder, ATTACH parser). The filter renderer and
storage-layer subclasses are exercised against the live server.

For live tests, set `DBISAM_USER` and `DBISAM_PASSWORD`:

```sh
DBISAM_USER=YOURUSER DBISAM_PASSWORD=YOURPASSWORD build/test_phase4_live
DBISAM_USER=YOURUSER DBISAM_PASSWORD=YOURPASSWORD build/test_phase5_live
```

(Defaults assume `YOURHOST` / `YOURCATALOG`; override via `DBISAM_HOST`,
`DBISAM_CATALOG`.)

## Layout

```
src/
  dbisam_extension.cpp        # entry point + scalar dbisam_version()
  dbisam_scan.cpp             # dbisam_scan(host, user, ...) table function
  dbisam_storage.cpp          # ATTACH TYPE dbisam StorageExtension
  protocol/                   # native DBISAM wire protocol (C++ port of
    wire / framing / crypto       MrsFlow's exportmaster/*.rs)
    msg / response / row
    cursor / cursor_info
    schema / blob / client / text
  storage/                    # DuckDB-side ATTACH plumbing
    dbisam_catalog              Catalog + SchemaEntry + TableEntry
    dbisam_schema_entry         + TransactionManager subclasses
    dbisam_table_entry        # GetScanFunction with proj + filter pushdown
    dbisam_transaction        # Per-context cached table entries
    dbisam_filter_render      # TableFilter → DBISAM WHERE
    dbisam_optimizer          # OptimizerExtension for LIMIT pushdown
    dbisam_attach_options     # em://...  connection-string parser
third_party/
  blowfish/                   # Schneier reference, encrypt-only
  md5/                        # RFC 1321 clean-room
test/                         # 7 unit-test executables + 2 live drivers
```

## Debug switches

Several env vars surface diagnostics without rebuilding:

- `DBISAM_SQL_DEBUG=1` — log every SQL string sent to the server.
- `DBISAM_FILTER_DEBUG=1` — log each TableFilter type + rendered fragment.
- `DBISAM_ATTACH_DEBUG=1` — log catalog lookup / schema probe activity.
- `DBISAM_BLOB_DEBUG=1` — log per-blob OpenBlob/FreeBlob round-trips.

## License

The DuckDB extension code is original. Bundled third-party:

- Blowfish: Bruce Schneier reference, public domain (`third_party/blowfish/`)
- MD5: clean-room RFC 1321 (`third_party/md5/`)
- DuckDB itself: MIT, fetched at build time
- miniz, mbedtls: bundled with DuckDB

[mrsflow]: https://github.com/lawless-m/MrsFlow
[exportking]: https://github.com/lawless-m/ExportKing
[derek]: file:///nonreplicated/Git/Derek
[dibdog]: https://github.com/lawless-m/Dibdog
