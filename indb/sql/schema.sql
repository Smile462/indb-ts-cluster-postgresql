--  Copyright (c) 2026 The in-database time-series clustering authors.
--  This source code is licensed under both the GPLv2 (found in the
--  COPYING file in the root directory) and Apache 2.0 License
--  (found in the LICENSE.Apache file in the root directory).
--
-- Tables for the in-database clustering add-on.
-- ts_point holds the raw series; ts_page and ts_chunk hold the precomputed
-- clustering metadata; ts_dataset records the parameters that produced them.

CREATE TABLE IF NOT EXISTS ts_point (
  ds  int4   NOT NULL,
  ts  int8   NOT NULL,
  v   float8 NOT NULL,
  PRIMARY KEY (ds, ts)
) WITH (fillfactor = 100);

CREATE TABLE IF NOT EXISTS ts_page (
  ds       int4  NOT NULL,
  page_no  int8  NOT NULL,
  t_start  int8  NOT NULL,
  t_end    int8  NOT NULL,
  n_points int8  NOT NULL,
  k        int4  NOT NULL,
  l        int4  NOT NULL,
  edk      int4  NOT NULL,
  version  int4  NOT NULL,
  meta     bytea NOT NULL,
  PRIMARY KEY (ds, page_no)
);

CREATE INDEX IF NOT EXISTS ts_page_range_idx ON ts_page (ds, t_end, t_start);

CREATE TABLE IF NOT EXISTS ts_chunk (
  ds        int4  NOT NULL,
  chunk_no  int8  NOT NULL,
  page_from int8  NOT NULL,
  page_to   int8  NOT NULL,
  t_start   int8  NOT NULL,
  t_end     int8  NOT NULL,
  version   int4  NOT NULL,
  meta      bytea NOT NULL,
  PRIMARY KEY (ds, chunk_no)
);

CREATE INDEX IF NOT EXISTS ts_chunk_range_idx ON ts_chunk (ds, t_end, t_start);

-- Metadata payloads are large and opaque: storing them externally keeps them
-- out of the compression path, which matters for the measured query cost.
ALTER TABLE ts_page ALTER COLUMN meta SET STORAGE EXTERNAL;
ALTER TABLE ts_chunk ALTER COLUMN meta SET STORAGE EXTERNAL;

CREATE TABLE IF NOT EXISTS ts_dataset (
  ds           int4 PRIMARY KEY,
  k            int4 NOT NULL,
  l            int4 NOT NULL,
  edk          int4 NOT NULL,
  iter_build   int4 NOT NULL,
  iter_raw     int4 NOT NULL,
  page_points  int4 NOT NULL,
  chunk_pages  int4 NOT NULL,
  seed         int8 NOT NULL,
  n_points     int8 NOT NULL,
  n_pages      int8 NOT NULL,
  n_chunks     int8 NOT NULL,
  first_ts     int8 NOT NULL,
  last_ts      int8 NOT NULL,
  version      int4 NOT NULL
);
