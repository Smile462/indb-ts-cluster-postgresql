# Vendored numerical kernel

This directory is a verbatim copy of `indb/core` from the sibling repository
[`Smile462/indb-ts-cluster-rocksdb`](https://github.com/Smile462/indb-ts-cluster-rocksdb).

- Source commit: `c436790e21079bfb4eda2f7cb232ba9f6fe86a94`
  ("Expose merge helpers so adapters can drive page selection themselves")
- Copied: 2026-10-24

| File | SHA-256 |
|---|---|
| `include/indb/core.h` | `3a2b11d9389cd5f4f70840fe86b0fe56c1adc2986406341c0df5a3445de40205` |
| `src/math.cpp` | `53d975634b7ce88e0e7793872de84528aee59ad1e58c5b60a5ebc017d5f34d7e` |
| `src/meta.cpp` | `83a198b9f019f4771f6b11b4a7f257930ef8b1a192d6d9491623eb8ac2eeba02` |
| `src/query.cpp` | `73195a8594cfea36662eeb1af97ec1e07d3c5de20a569af1c41c70c50b7186b9` |

Rules:

1. Do not edit these files here. Change them in the RocksDB repository, then
   copy the directory over and update this file.
2. `scripts/build.ps1` compiles exactly these sources, so the PostgreSQL
   extension always runs the same arithmetic as the RocksDB one.
3. The deliberate deviations from the IoTDB reference are documented in the
   RocksDB repository under `indb/docs/algorithm-notes.md`.
