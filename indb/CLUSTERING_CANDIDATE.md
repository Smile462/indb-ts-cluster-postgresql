<!--
Licensed to the Apache Software Foundation (ASF) under one or more
contributor license agreements. See the NOTICE file distributed with
this work for additional information regarding copyright ownership.
The ASF licenses this file to you under the Apache License, Version 2.0
(the "License"); you may not use this file except in compliance with
the License. You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Clustering implementation candidate - 2026-11-08

This snapshot is a candidate for a new round of experiments. Persisted Chunk
metadata and coverage-threshold selection remain the core method. Page-M and
Chunk-M share the platform's Page subtraction correction when usable Page
metadata falls below the Page coverage threshold. Unusable or conflicting
metadata still takes the existing raw recovery path.

The original Page correction removes wholly outside subsequences, so a
subsequence crossing a query edge can retain a contribution. This is not a
strict query-intersection refit. K queries retain their existing boundary path.
Historical measurements remain results of their recorded implementation;
they should not be relabeled as measurements of this candidate.

## PostgreSQL add-on changes

- Page-M and Chunk-M call the same sampler. Keep adding whole pages; the first
  page exceeding remaining capacity contributes its chronological prefix,
  fills the pool, and stops sampling. Capacity remains `max(k*32, 128)`.
- Query-local page candidate caches avoid repeated SQL reads and extraction.
  Duplicate candidates are retained. Caches do not survive between queries.
- Page weights, sampling rate and RNG are retained, while the final-page
  acceptance rule intentionally changes the candidate pool. Candidate content,
  representatives and quality can therefore differ from the preceding version.
- Extraction errors are reported instead of being treated as empty samples.
- Chunk-M reuses Page correction, with M-weight bookkeeping. The shared Chunk
  merge also guards non-finite distances caused by zero running K centers;
  normal distances and original Page merge arithmetic are unchanged.

`indb/sql/install.sql` loads `$libdir/indb_tsc`, matching the build script's
`-Install` option, rather than embedding a local experiment directory.
Public SQL function signatures and the persisted metadata format are unchanged.

## Validation

The extension builds with MSVC against the PostgreSQL server headers/library.
Sampler behavior was checked against an independent ordered-candidate oracle,
including oversized pages, repeats, short/empty pages and fresh per-query
caches. The source-built DLL passed synthetic integration queries through SPI.
The numerical core matches the RocksDB add-on. No performance ranking or
quality-equivalence claim is made for the changed candidate policy.
