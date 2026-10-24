--  Copyright (c) 2026 The in-database time-series clustering authors.
--  This source code is licensed under both the GPLv2 (found in the
--  COPYING file in the root directory) and Apache 2.0 License
--  (found in the LICENSE.Apache file in the root directory).
--
-- Registers the C entry points. Run after the shared library is installed in
-- the PostgreSQL library directory (or use an absolute path below).

CREATE OR REPLACE FUNCTION indb_tsc_import(ds int4, path text, skip_header bool DEFAULT false)
RETURNS int8 AS 'indb_tsc', 'indb_tsc_import'
LANGUAGE C STRICT VOLATILE;

CREATE OR REPLACE FUNCTION indb_tsc_build(ds int4, k int4, l int4, page_points int4,
                                          chunk_pages int4, iter_build int4, seed int8)
RETURNS int8 AS 'indb_tsc', 'indb_tsc_build'
LANGUAGE C STRICT VOLATILE;

CREATE OR REPLACE FUNCTION indb_tsc_kshape(ds int4, t0 int8, t1 int8, level text DEFAULT 'page')
RETURNS float8[] AS 'indb_tsc', 'indb_tsc_kshape'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION indb_tsc_mshape(ds int4, t0 int8, t1 int8, level text DEFAULT 'page')
RETURNS float8[] AS 'indb_tsc', 'indb_tsc_mshape'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION indb_tsc_kshape_raw(ds int4, t0 int8, t1 int8)
RETURNS float8[] AS 'indb_tsc', 'indb_tsc_kshape_raw'
LANGUAGE C STRICT VOLATILE;

CREATE OR REPLACE FUNCTION indb_tsc_mshape_raw(ds int4, t0 int8, t1 int8)
RETURNS float8[] AS 'indb_tsc', 'indb_tsc_mshape_raw'
LANGUAGE C STRICT VOLATILE;

CREATE OR REPLACE FUNCTION indb_tsc_last_stats()
RETURNS float8[] AS 'indb_tsc', 'indb_tsc_last_stats'
LANGUAGE C STRICT VOLATILE;
