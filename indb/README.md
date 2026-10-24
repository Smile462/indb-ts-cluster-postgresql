# In-database time-series clustering on PostgreSQL

Server-side implementation of the K-Shape / Medoid-Shape pipeline on PostgreSQL
18, ported from the IoTDB reference implementation and from the sibling
RocksDB port.

The repository root is an unmodified PostgreSQL 18.6 source tree. Everything in
this directory (`indb/`) is the add-on: a C extension plus the vendored copy of
the shared numerical kernel.

## What is implemented

Four query paths, each available at page and chunk granularity, exactly as in
the RocksDB repository:

| Path | Input | Work at query time |
|---|---|---|
| `indb_tsc_kshape_raw` | raw points of the range | full K-Shape from scratch |
| `indb_tsc_mshape_raw` | raw points of the range | full Medoid-Shape from scratch |
| `indb_tsc_kshape` | persisted page or chunk metadata (edge pages re-read) | merge, correct boundaries, extract eigenvectors |
| `indb_tsc_mshape` | persisted metadata plus sampled subsequences | merge, greedy medoid selection |

Everything runs inside the backend: the entry points read their inputs through
SPI, so no path bypasses SQL access to the tables.

## Layout

```
indb/
  vendor/core/          identical snapshot of indb-ts-cluster-rocksdb:indb/core
  extension/bridge.h    C interface to the C++ kernel
  extension/bridge.cpp  exception safe wrappers, buffer and array conversion
  extension/indb_tsc.c  PostgreSQL entry points: import, build, four queries
  sql/schema.sql        tables and indexes
  sql/install.sql       function registration
  scripts/build.ps1     MSVC build and install
```

## Build

Prerequisites: MSVC 2022, PostgreSQL 18 binaries with `include/` and
`lib/postgres.lib` (the EDB Windows package provides both).

```powershell
powershell -ExecutionPolicy Bypass -File indb/scripts/build.ps1 -Install
```

The script compiles the kernel, the bridge and the extension with `cl`, links
against `postgres.lib`, and copies `indb_tsc.dll` into `E:\pgsql\lib`.

## Schema

| Table | Purpose |
|---|---|
| `ts_point` | raw series: `ds`, `ts`, `v`, primary key `(ds, ts)` |
| `ts_page` | one row per logical page: time range, point count, parameters, encoded metadata |
| `ts_chunk` | one row per logical chunk: page range, time range, encoded metadata |
| `ts_dataset` | parameters and counts for each dataset id |

`meta` columns use `STORAGE EXTERNAL`: the payloads are large and opaque, so
compressing them would add cost to every read without helping.

## Usage

```sql
-- once
\i indb/sql/schema.sql
\i indb/sql/install.sql

-- load a two column CSV (ts,value); a single column file is accepted too
SELECT indb_tsc_import(1, 'E:/data/air.csv', false);

-- build page and chunk metadata (k, l, page points, chunk pages, iterations, seed)
SELECT indb_tsc_build(1, 3, 166, 10240, 8, 30, 24301);

-- queries
SELECT indb_tsc_kshape(1, 0, 714961, 'page');
SELECT indb_tsc_kshape(1, 0, 714961, 'chunk');
SELECT indb_tsc_mshape(1, 0, 714961, 'page');
SELECT indb_tsc_kshape_raw(1, 0, 714961);
SELECT indb_tsc_last_stats();
```

Results are `float8[]` with `k * l` entries in row major order. The counters
returned by `indb_tsc_last_stats` describe the most recent query **in the same
session**; they are backend local.

## Parity with the RocksDB implementation

The two implementations share one kernel, so the deterministic paths produce
bit identical results. Measured on a 20000 point synthetic series
(`k=3, l=50`, page 2000 points, chunk 4 pages, seed 24301):

| Path | Max absolute difference |
|---|---|
| `kshape` page | 0.0 |
| `kshape` chunk | 0.0 |
| `kshape_raw` | 0.0 |
| `mshape_raw` | 0.0 |

Diagnostics also match: a partially covered range over the same data reports
`pages_considered=7, pages_meta=5, pages_corrected=2, raw_points=4000` in both
implementations.

`mshape` (the precomputed Medoid path) samples candidate subsequences, and the
sampling stream differs between the two adapters, so those centres agree
statistically but not bit for bit. This matches the reference implementation,
which also uses unseeded sampling.

## Current limitations

- Import is a row-oriented loop; it is fine for setup but not tuned for very
  large files. Wrapping the call in an explicit transaction is recommended.
- Metadata is built by `indb_tsc_build` from the current contents of
  `ts_point`. There is no automatic invalidation on updates; treat the series
  as static, and re-run the build after changes.
- Compaction and vacuum are left at PostgreSQL defaults.
- One series per dataset id.

## Provenance

The vendored kernel is a verbatim copy of
`indb-ts-cluster-rocksdb:indb/core` at commit
`c436790e21079bfb4eda2f7cb232ba9f6fe86a94`. See `vendor/README.md` for the
per-file hashes.
