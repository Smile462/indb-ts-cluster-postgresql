/*  Copyright (c) 2026 The in-database time-series clustering authors.
 *  This source code is licensed under both the GPLv2 (found in the
 *  COPYING file in the root directory) and Apache 2.0 License
 *  (found in the LICENSE.Apache file in the root directory).
 *
 * PostgreSQL server side entry points.
 *
 * Every entry point runs inside the backend and reads its inputs through SPI,
 * so the clustering paths never bypass SQL access to the tables. The numerical
 * work is delegated to the shared C++ kernel through bridge.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include <math.h>

#include "bridge.h"

PG_MODULE_MAGIC;

#define IMPORT_BATCH 256

static indb_stats g_last_stats;
static char *journal_query_memory=NULL;
static int journal_candidate_count=0;
static unsigned long long journal_candidate_hash=0xcbf29ce484222325ULL;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void CheckSpi(int rc, const char *what)
{
    if (rc < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("%s failed: %s", what, SPI_result_code_string(rc))));
}

static ArrayType *DoublesToArray(const double *values, int n)
{
    Datum *datums = (Datum *) palloc(sizeof(Datum) * (n > 0 ? n : 1));
    for (int i = 0; i < n; i++)
        datums[i] = Float8GetDatum(values[i]);
    return construct_array(datums, n, FLOAT8OID, sizeof(float8), FLOAT8PASSBYVAL, 'd');
}

static Datum ByteaDatum(const char *buffer, size_t length)
{
    bytea *value = (bytea *) palloc(VARHDRSZ + length);
    SET_VARSIZE(value, VARHDRSZ + length);
    if (length > 0)
        memcpy(VARDATA(value), buffer, length);
    return PointerGetDatum(value);
}

/* centers is allocated with SPI_palloc in the caller context, so it remains
 * valid after SPI_finish frees the SPI procedure context. */
static ArrayType *FinishQueryResult(double *centers, int n)
{
    ArrayType *result;
    SPI_finish();
    result = DoublesToArray(centers, n);
    pfree(centers);
    return result;
}

/* Loads the parameters recorded for a dataset. Returns false if absent. */
static bool LoadParams(int ds, indb_params *params, bool *found)
{
    Oid types[1] = {INT4OID};
    Datum values[1] = {Int32GetDatum(ds)};
    int rc = SPI_execute_with_args(
        "SELECT k, l, edk, iter_build, iter_raw, page_points, chunk_pages, seed "
        "FROM ts_dataset WHERE ds = $1",
        1, types, values, NULL, true, 1);
    CheckSpi(rc, "reading dataset parameters");
    if (rc != SPI_OK_SELECT || SPI_processed == 0) {
        *found = false;
        return true;
    }

    bool isnull = false;
    HeapTuple tuple = SPI_tuptable->vals[0];
    TupleDesc desc = SPI_tuptable->tupdesc;

    params->k = DatumGetInt32(SPI_getbinval(tuple, desc, 1, &isnull));
    params->l = DatumGetInt32(SPI_getbinval(tuple, desc, 2, &isnull));
    params->edk = DatumGetInt32(SPI_getbinval(tuple, desc, 3, &isnull));
    params->iter_build = DatumGetInt32(SPI_getbinval(tuple, desc, 4, &isnull));
    params->iter_raw = DatumGetInt32(SPI_getbinval(tuple, desc, 5, &isnull));
    params->page_points = DatumGetInt32(SPI_getbinval(tuple, desc, 6, &isnull));
    params->chunk_pages = DatumGetInt32(SPI_getbinval(tuple, desc, 7, &isnull));
    params->seed = (unsigned long long) DatumGetInt64(SPI_getbinval(tuple, desc, 8, &isnull));
    params->r = 0.2;
    params->theta_page = 0.8;
    params->theta_chunk = 0.8;
    *found = true;
    return true;
}

/* Reads points of one series inside a closed time range. Caller frees both
 * arrays with pfree. */
static void FetchRange(int ds, long long t0, long long t1, long long **out_ts,
                       double **out_values, int *out_count)
{
    Oid types[3] = {INT4OID, INT8OID, INT8OID};
    Datum values[3] = {Int32GetDatum(ds), Int64GetDatum(t0), Int64GetDatum(t1)};
    int rc = SPI_execute_with_args(
        "SELECT ts, v FROM ts_point WHERE ds = $1 AND ts >= $2 AND ts <= $3 ORDER BY ts",
        3, types, values, NULL, true, 0);
    CheckSpi(rc, "reading points");

    const uint64 count = SPI_processed;
    long long *ts = (long long *) palloc(sizeof(long long) * (count > 0 ? count : 1));
    double *vals = (double *) palloc(sizeof(double) * (count > 0 ? count : 1));
    TupleDesc desc = SPI_tuptable->tupdesc;
    for (uint64 i = 0; i < count; i++) {
        bool isnull = false;
        HeapTuple tuple = SPI_tuptable->vals[i];
        ts[i] = DatumGetInt64(SPI_getbinval(tuple, desc, 1, &isnull));
        vals[i] = DatumGetFloat8(SPI_getbinval(tuple, desc, 2, &isnull));
    }
    *out_ts = ts;
    *out_values = vals;
    *out_count = (int) count;
}

/* ------------------------------------------------------------------------- */
/* Import                                                                    */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(indb_tsc_import);

PGDLLEXPORT Datum indb_tsc_import(PG_FUNCTION_ARGS)
{
    int32 ds = PG_GETARG_INT32(0);
    char *path = text_to_cstring(PG_GETARG_TEXT_PP(1));
    bool skip_header = PG_GETARG_BOOL(2);

    FILE *file = AllocateFile(path, "r");
    if (file == NULL)
        ereport(ERROR, (errcode_for_file_access(), errmsg("cannot open \"%s\"", path)));

    SPI_connect();

    StringInfoData sql;
    initStringInfo(&sql);
    appendStringInfoString(&sql, "INSERT INTO ts_point (ds, ts, v) VALUES ");
    for (int i = 0; i < IMPORT_BATCH; i++) {
        if (i > 0)
            appendStringInfoChar(&sql, ',');
        appendStringInfo(&sql, "($%d,$%d,$%d)", 3 * i + 1, 3 * i + 2, 3 * i + 3);
    }

    Oid *types = (Oid *) palloc(sizeof(Oid) * 3 * IMPORT_BATCH);
    Datum *values = (Datum *) palloc(sizeof(Datum) * 3 * IMPORT_BATCH);
    for (int i = 0; i < IMPORT_BATCH; i++) {
        types[3 * i + 0] = INT4OID;
        types[3 * i + 1] = INT8OID;
        types[3 * i + 2] = FLOAT8OID;
    }

    long long total = 0;
    int pending = 0;
    long long row = 0;
    char line[512];
    bool header_skipped = !skip_header;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *first = line;
        while (*first == ' ' || *first == '\t')
            first++;
        if (*first == '\0' || *first == '\n' || *first == '\r' || *first == '#')
            continue;
        if (!((*first >= '0' && *first <= '9') || *first == '-' || *first == '+' || *first == '.'))
            continue; /* header line */

        char *comma = strchr(first, ',');
        long long ts = row;
        double value = 0.0;
        if (comma != NULL) {
            *comma = '\0';
            ts = strtoll(first, NULL, 10);
            value = strtod(comma + 1, NULL);
            header_skipped = true;
        } else {
            value = strtod(first, NULL);
        }
        if (!header_skipped) {
            header_skipped = true;
            continue;
        }

        values[3 * pending + 0] = Int32GetDatum(ds);
        values[3 * pending + 1] = Int64GetDatum(ts);
        values[3 * pending + 2] = Float8GetDatum(value);
        pending++;
        row++;

        if (pending == IMPORT_BATCH) {
            int rc = SPI_execute_with_args(sql.data, 3 * pending, types, values, NULL, false, 0);
            CheckSpi(rc, "inserting points");
            total += pending;
            pending = 0;
            CHECK_FOR_INTERRUPTS();
        }
    }

    if (pending > 0) {
        StringInfoData partial;
        initStringInfo(&partial);
        appendStringInfoString(&partial, "INSERT INTO ts_point (ds, ts, v) VALUES ");
        for (int i = 0; i < pending; i++) {
            if (i > 0)
                appendStringInfoChar(&partial, ',');
            appendStringInfo(&partial, "($%d,$%d,$%d)", 3 * i + 1, 3 * i + 2, 3 * i + 3);
        }
        int rc = SPI_execute_with_args(partial.data, 3 * pending, types, values, NULL, false, 0);
        CheckSpi(rc, "inserting trailing points");
        total += pending;
        pfree(partial.data);
    }

    FreeFile(file);
    SPI_finish();
    PG_RETURN_INT64(total);
}

/* ------------------------------------------------------------------------- */
/* Metadata build                                                            */
/* ------------------------------------------------------------------------- */

static void InsertPageRow(int ds, long long page_no, const char *meta, size_t meta_len,
                          const indb_params *params, long long t_start, long long t_end,
                          long long points)
{
    Oid types[10] = {INT4OID, INT8OID, INT8OID, INT8OID, INT8OID,
                     INT4OID, INT4OID, INT4OID, INT4OID, BYTEAOID};
    Datum values[10];
    values[0] = Int32GetDatum(ds);
    values[1] = Int64GetDatum(page_no);
    values[2] = Int64GetDatum(t_start);
    values[3] = Int64GetDatum(t_end);
    values[4] = Int64GetDatum(points);
    values[5] = Int32GetDatum(params->k);
    values[6] = Int32GetDatum(params->l);
    values[7] = Int32GetDatum(params->edk > 0 ? params->edk : params->k + 2);
    values[8] = Int32GetDatum(1);
    values[9] = ByteaDatum(meta, meta_len);

    int rc = SPI_execute_with_args(
        "INSERT INTO ts_page (ds,page_no,t_start,t_end,n_points,k,l,edk,version,meta) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
        10, types, values, NULL, false, 0);
    CheckSpi(rc, "inserting page metadata");
}

static void InsertChunkRow(int ds, long long chunk_no, long long page_from, long long page_to,
                           const char *meta, size_t meta_len, long long t_start, long long t_end)
{
    Oid types[8] = {INT4OID, INT8OID, INT8OID, INT8OID, INT8OID, INT8OID, INT4OID, BYTEAOID};
    Datum values[8];
    values[0] = Int32GetDatum(ds);
    values[1] = Int64GetDatum(chunk_no);
    values[2] = Int64GetDatum(page_from);
    values[3] = Int64GetDatum(page_to);
    values[4] = Int64GetDatum(t_start);
    values[5] = Int64GetDatum(t_end);
    values[6] = Int32GetDatum(1);
    values[7] = ByteaDatum(meta, meta_len);

    int rc = SPI_execute_with_args(
        "INSERT INTO ts_chunk (ds,chunk_no,page_from,page_to,t_start,t_end,version,meta) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8)",
        8, types, values, NULL, false, 0);
    CheckSpi(rc, "inserting chunk metadata");
}

PG_FUNCTION_INFO_V1(indb_tsc_build);

PGDLLEXPORT Datum indb_tsc_build(PG_FUNCTION_ARGS)
{
    int32 ds = PG_GETARG_INT32(0);
    indb_params params;
    memset(&params, 0, sizeof(params));
    params.k = PG_GETARG_INT32(1);
    params.l = PG_GETARG_INT32(2);
    params.page_points = PG_GETARG_INT32(3);
    params.chunk_pages = PG_GETARG_INT32(4);
    params.iter_build = PG_GETARG_INT32(5);
    params.iter_raw = 100;
    params.edk = 0;
    params.seed = (unsigned long long) PG_GETARG_INT64(6);
    params.r = 0.2;
    params.theta_page = 0.8;
    params.theta_chunk = 0.8;

    if (params.k <= 0 || params.l < 2 || params.page_points <= 0 || params.chunk_pages <= 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid build parameters")));

    SPI_connect();
    const char *storage_state=GetConfigOptionByName("journal.storage_state",NULL,true);
    const bool page_only=storage_state && strcmp(storage_state,"SP")==0;

    {
        Oid types[1] = {INT4OID};
        Datum values[1] = {Int32GetDatum(ds)};
        CheckSpi(SPI_execute_with_args("DELETE FROM ts_page WHERE ds = $1", 1, types, values, NULL,
                                       false, 0),
                 "clearing page metadata");
        CheckSpi(SPI_execute_with_args("DELETE FROM ts_chunk WHERE ds = $1", 1, types, values, NULL,
                                       false, 0),
                 "clearing chunk metadata");
    }

    SPIPlanPtr plan = SPI_prepare("SELECT ts, v FROM ts_point WHERE ds = $1 ORDER BY ts", 1,
                                  (Oid[]){INT4OID});
    if (plan == NULL)
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("cannot prepare point cursor")));
    Datum cursor_values[1] = {Int32GetDatum(ds)};
    Portal portal = SPI_cursor_open(NULL, plan, cursor_values, NULL, true);

    long long *page_ts = (long long *) palloc(sizeof(long long) * params.page_points);
    double *page_values = (double *) palloc(sizeof(double) * params.page_points);
    int page_count = 0;

    long long page_no = 0;
    long long chunk_no = 0;
    double journal_page_seconds=0,journal_chunk_seconds=0;
    long long total_points = 0;
    long long first_ts = 0;
    long long last_ts = 0;
    bool have_any = false;

    char **chunk_metas = (char **) palloc(sizeof(char *) * params.chunk_pages);
    size_t *chunk_lens = (size_t *) palloc(sizeof(size_t) * params.chunk_pages);
    int chunk_fill = 0;
    long long chunk_page_from = 0;
    long long chunk_t_start = 0;
    long long chunk_t_end = 0;

    for (;;) {
        SPI_cursor_fetch(portal, true, params.page_points);
        if (SPI_processed == 0)
            break;

        TupleDesc desc = SPI_tuptable->tupdesc;
        for (uint64 i = 0; i < SPI_processed; i++) {
            bool isnull = false;
            HeapTuple tuple = SPI_tuptable->vals[i];
            page_ts[page_count] = DatumGetInt64(SPI_getbinval(tuple, desc, 1, &isnull));
            page_values[page_count] = DatumGetFloat8(SPI_getbinval(tuple, desc, 2, &isnull));
            page_count++;
            total_points++;
            if (!have_any) {
                first_ts = page_ts[0];
                have_any = true;
            }
            last_ts = page_ts[page_count - 1];

            if (page_count == params.page_points) {
                TimestampTz journal_page_begin=GetCurrentTimestamp();
                char *meta = NULL;
                size_t meta_len = 0;
                char err[512] = {0};
                const unsigned long long page_seed =
                    params.seed ^ ((unsigned long long) (page_no + 1) * 0x9E3779B97F4A7C15ULL);
                if (indb_build_page_meta(page_ts, page_values, page_count, &params, page_seed,
                                         &meta, &meta_len, err, sizeof(err)) != 0)
                    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                    errmsg("page metadata failed: %s", err)));

                journal_page_seconds+=(GetCurrentTimestamp()-journal_page_begin)/1000000.0;
                InsertPageRow(ds, page_no, meta, meta_len, &params, page_ts[0],
                              page_ts[page_count - 1], page_count);
        if(!page_only && page_count<params.l) {
            TimestampTz short_begin=GetCurrentTimestamp();
            indb_params chunk_page_params=params;chunk_page_params.chunk_path=1;
            indb_free(meta);meta=NULL;meta_len=0;
            if(indb_build_page_meta(page_ts,page_values,page_count,&chunk_page_params,page_seed,&meta,&meta_len,err,sizeof(err))!=0)
                ereport(ERROR,(errmsg("Short Chunk page failed: %s",err)));
            journal_chunk_seconds+=(GetCurrentTimestamp()-short_begin)/1000000.0;
        }

                if (chunk_fill == 0)
                    chunk_t_start = page_ts[0];
                chunk_t_end = page_ts[page_count - 1];
                chunk_metas[chunk_fill] = meta;
                chunk_lens[chunk_fill] = meta_len;
                chunk_fill++;
                page_no++;
                page_count = 0;

                if (chunk_fill == params.chunk_pages) {
                    if(!page_only) {

                    TimestampTz journal_chunk_begin=GetCurrentTimestamp();
                    char *chunk_meta = NULL;
                    size_t chunk_meta_len = 0;
                    if (indb_build_chunk_meta((const char *const *) chunk_metas, chunk_lens,
                                              chunk_fill, &params, &chunk_meta, &chunk_meta_len,
                                              err, sizeof(err)) != 0)
                        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                        errmsg("chunk metadata failed: %s", err)));
                    journal_chunk_seconds+=(GetCurrentTimestamp()-journal_chunk_begin)/1000000.0;
                    InsertChunkRow(ds, chunk_no, page_no - chunk_fill, page_no - 1, chunk_meta,
                                   chunk_meta_len, chunk_t_start, chunk_t_end);
                    chunk_no++;
                    }
                    for (int c = 0; c < chunk_fill; c++)
                        indb_free(chunk_metas[c]);
                    chunk_fill = 0;
                    chunk_page_from = page_no;
                }
            }
        }
        SPI_freetuptable(SPI_tuptable);
        CHECK_FOR_INTERRUPTS();
    }
    SPI_cursor_close(portal);

    /* Trailing partial page. */
    if (page_count > 0) {
        TimestampTz journal_page_begin=GetCurrentTimestamp();
        char *meta = NULL;
        size_t meta_len = 0;
        char err[512] = {0};
        const unsigned long long page_seed =
            params.seed ^ ((unsigned long long) (page_no + 1) * 0x9E3779B97F4A7C15ULL);
        if (indb_build_page_meta(page_ts, page_values, page_count, &params, page_seed, &meta,
                                 &meta_len, err, sizeof(err)) != 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR), errmsg("page metadata failed: %s", err)));
        journal_page_seconds+=(GetCurrentTimestamp()-journal_page_begin)/1000000.0;
        InsertPageRow(ds, page_no, meta, meta_len, &params, page_ts[0],
                      page_ts[page_count - 1], page_count);
        if(!page_only && page_count<params.l) {
            TimestampTz short_begin=GetCurrentTimestamp();
            indb_params chunk_page_params=params;chunk_page_params.chunk_path=1;
            indb_free(meta);meta=NULL;meta_len=0;
            if(indb_build_page_meta(page_ts,page_values,page_count,&chunk_page_params,page_seed,&meta,&meta_len,err,sizeof(err))!=0)
                ereport(ERROR,(errmsg("Short Chunk page failed: %s",err)));
            journal_chunk_seconds+=(GetCurrentTimestamp()-short_begin)/1000000.0;
        }

        if (chunk_fill == 0)
            chunk_t_start = page_ts[0];
        chunk_t_end = page_ts[page_count - 1];
        chunk_metas[chunk_fill] = meta;
        chunk_lens[chunk_fill] = meta_len;
        chunk_fill++;
        page_no++;
    }

    if (chunk_fill > 0) {
                    if(!page_only) {

        TimestampTz journal_chunk_begin=GetCurrentTimestamp();
                    char *chunk_meta = NULL;
        size_t chunk_meta_len = 0;
        char err[512] = {0};
        if (indb_build_chunk_meta((const char *const *) chunk_metas, chunk_lens, chunk_fill,
                                  &params, &chunk_meta, &chunk_meta_len, err, sizeof(err)) != 0)
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("chunk metadata failed: %s", err)));
        journal_chunk_seconds+=(GetCurrentTimestamp()-journal_chunk_begin)/1000000.0;
        InsertChunkRow(ds, chunk_no, page_no - chunk_fill, page_no - 1, chunk_meta, chunk_meta_len,
                       chunk_t_start, chunk_t_end);
        chunk_no++;
                    }
        for (int c = 0; c < chunk_fill; c++)
            indb_free(chunk_metas[c]);
        chunk_fill = 0;
    }
    (void) chunk_page_from;

    {
        Oid types[14] = {INT4OID, INT4OID, INT4OID, INT4OID, INT4OID, INT4OID, INT4OID, INT4OID,
                         INT8OID,  INT8OID, INT8OID, INT8OID, INT8OID, INT8OID};
        Datum values[14];
        values[0] = Int32GetDatum(ds);
        values[1] = Int32GetDatum(params.k);
        values[2] = Int32GetDatum(params.l);
        values[3] = Int32GetDatum(params.edk > 0 ? params.edk : params.k + 2);
        values[4] = Int32GetDatum(params.iter_build);
        values[5] = Int32GetDatum(params.iter_raw);
        values[6] = Int32GetDatum(params.page_points);
        values[7] = Int32GetDatum(params.chunk_pages);
        values[8] = Int64GetDatum((int64) params.seed);
        values[9] = Int64GetDatum(total_points);
        values[10] = Int64GetDatum(page_no);
        values[11] = Int64GetDatum(chunk_no);
        values[12] = Int64GetDatum(first_ts);
        values[13] = Int64GetDatum(last_ts);
        CheckSpi(SPI_execute_with_args(
                     "INSERT INTO ts_dataset (ds,k,l,edk,iter_build,iter_raw,page_points,"
                     "chunk_pages,seed,n_points,n_pages,n_chunks,first_ts,last_ts,version) "
                     "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,1) "
                     "ON CONFLICT (ds) DO UPDATE SET k=EXCLUDED.k, l=EXCLUDED.l, "
                     "edk=EXCLUDED.edk, iter_build=EXCLUDED.iter_build, "
                     "iter_raw=EXCLUDED.iter_raw, page_points=EXCLUDED.page_points, "
                     "chunk_pages=EXCLUDED.chunk_pages, seed=EXCLUDED.seed, "
                     "n_points=EXCLUDED.n_points, n_pages=EXCLUDED.n_pages, "
                     "n_chunks=EXCLUDED.n_chunks, first_ts=EXCLUDED.first_ts, "
                     "last_ts=EXCLUDED.last_ts, version=EXCLUDED.version",
                     14, types, values, NULL, false, 0),
                 "writing dataset row");
    }

    SPI_finish();
    ereport(NOTICE,(errmsg("JOURNAL_BUILD page_seconds=%.9f chunk_seconds=%.9f",journal_page_seconds,journal_chunk_seconds)));
    PG_RETURN_INT64(total_points);
}

/* ------------------------------------------------------------------------- */
/* Query paths                                                               */
/* ------------------------------------------------------------------------- */

typedef struct PageRow {
    long long page_no;
    long long t_start;
    long long t_end;
    char *meta;
    size_t meta_len;
} PageRow;

typedef struct ChunkRow {
    long long chunk_no;
    long long page_from;
    long long page_to;
    long long t_start;
    long long t_end;
    char *meta;
    size_t meta_len;
} ChunkRow;

static double OverlapRatio(long long t_start, long long t_end, long long t0, long long t1)
{
    long long start = t_start > t0 ? t_start : t0;
    long long end = t_end < t1 ? t_end : t1;
    if (start > end)
        return 0.0;
    double total = (double) (t_end - t_start + 1);
    return total <= 0.0 ? 0.0 : (double) (end - start + 1) / total;
}

static int LoadPages(int ds, long long t0, long long t1, PageRow **out, bool headers_only)
{
    Oid types[3] = {INT4OID, INT8OID, INT8OID};
    Datum values[3] = {Int32GetDatum(ds), Int64GetDatum(t0), Int64GetDatum(t1)};
    const char *sql=headers_only?
        "SELECT page_no,t_start,t_end,NULL::bytea FROM ts_page WHERE ds=$1 AND t_end>=$2 AND t_start<=$3 ORDER BY page_no":
        "SELECT page_no,t_start,t_end,meta FROM ts_page WHERE ds=$1 AND t_end>=$2 AND t_start<=$3 ORDER BY page_no";
    int rc = SPI_execute_with_args(sql,
        3, types, values, NULL, true, 0);
    CheckSpi(rc, "reading page metadata");

    const uint64 count = SPI_processed;
    PageRow *rows = (PageRow *) palloc(sizeof(PageRow) * (count > 0 ? count : 1));
    TupleDesc desc = SPI_tuptable->tupdesc;
    for (uint64 i = 0; i < count; i++) {
        bool isnull = false;
        HeapTuple tuple = SPI_tuptable->vals[i];
        rows[i].page_no = DatumGetInt64(SPI_getbinval(tuple, desc, 1, &isnull));
        rows[i].t_start = DatumGetInt64(SPI_getbinval(tuple, desc, 2, &isnull));
        rows[i].t_end = DatumGetInt64(SPI_getbinval(tuple, desc, 3, &isnull));
        rows[i].meta_len = 0;
        rows[i].meta = NULL;
        if(!headers_only) {
            bytea *blob=(bytea*)PG_DETOAST_DATUM_PACKED(SPI_getbinval(tuple,desc,4,&isnull));
            rows[i].meta_len=VARSIZE_ANY_EXHDR(blob);rows[i].meta=(char*)palloc(rows[i].meta_len);
            memcpy(rows[i].meta,VARDATA_ANY(blob),rows[i].meta_len);g_last_stats.meta_bytes+=rows[i].meta_len;
        }
    }
    *out = rows;
    return (int) count;
}

static int LoadChunks(int ds, long long t0, long long t1, ChunkRow **out)
{
    Oid types[3] = {INT4OID, INT8OID, INT8OID};
    Datum values[3] = {Int32GetDatum(ds), Int64GetDatum(t0), Int64GetDatum(t1)};
    const char *ablation=GetConfigOptionByName("journal.ablation",NULL,true);
    const char *sql=(ablation && strcmp(ablation,"dynamic")==0)?
        "SELECT page_no/(SELECT chunk_pages FROM ts_dataset WHERE ds=$1),min(page_no),max(page_no),min(t_start),max(t_end),NULL::bytea FROM ts_page WHERE ds=$1 GROUP BY 1 HAVING max(t_end)>=$2 AND min(t_start)<=$3 ORDER BY 1":
        "SELECT chunk_no, page_from, page_to, t_start, t_end, meta FROM ts_chunk WHERE ds=$1 AND t_end>=$2 AND t_start<=$3 ORDER BY chunk_no";
    int rc = SPI_execute_with_args(sql,
        3, types, values, NULL, true, 0);
    CheckSpi(rc, "reading chunk metadata");

    const uint64 count = SPI_processed;
    ChunkRow *rows = (ChunkRow *) palloc(sizeof(ChunkRow) * (count > 0 ? count : 1));
    TupleDesc desc = SPI_tuptable->tupdesc;
    for (uint64 i = 0; i < count; i++) {
        bool isnull = false;
        HeapTuple tuple = SPI_tuptable->vals[i];
        rows[i].chunk_no = DatumGetInt64(SPI_getbinval(tuple, desc, 1, &isnull));
        rows[i].page_from = DatumGetInt64(SPI_getbinval(tuple, desc, 2, &isnull));
        rows[i].page_to = DatumGetInt64(SPI_getbinval(tuple, desc, 3, &isnull));
        rows[i].t_start = DatumGetInt64(SPI_getbinval(tuple, desc, 4, &isnull));
        rows[i].t_end = DatumGetInt64(SPI_getbinval(tuple, desc, 5, &isnull));
        Datum blobvalue=SPI_getbinval(tuple,desc,6,&isnull);
        if(isnull){rows[i].meta=NULL;rows[i].meta_len=0;continue;}
        bytea *blob=(bytea*)PG_DETOAST_DATUM_PACKED(blobvalue);
        rows[i].meta_len = VARSIZE_ANY_EXHDR(blob);
        g_last_stats.meta_bytes += rows[i].meta_len;
        rows[i].meta = (char *) palloc(rows[i].meta_len);
        memcpy(rows[i].meta, VARDATA_ANY(blob), rows[i].meta_len);
    }
    *out = rows;
    return (int) count;
}

static void EnsurePageMeta(int ds, PageRow *page,const indb_params *params) {
    if(page->meta) return;
    Oid types[2]={INT4OID,INT8OID};
    Datum vals[2]={Int32GetDatum(ds),Int64GetDatum(page->page_no)};
    CheckSpi(SPI_execute_with_args("SELECT meta FROM ts_page WHERE ds=$1 AND page_no=$2",2,types,vals,NULL,true,1),"page payload");
    if(SPI_processed!=1) ereport(ERROR,(errmsg("Missing page payload")));
    bool isnull=false;
    bytea *blob=(bytea*)PG_DETOAST_DATUM_PACKED(SPI_getbinval(SPI_tuptable->vals[0],SPI_tuptable->tupdesc,1,&isnull));
    page->meta_len=VARSIZE_ANY_EXHDR(blob);page->meta=(char*)palloc(page->meta_len);
    memcpy(page->meta,VARDATA_ANY(blob),page->meta_len);
    g_last_stats.meta_bytes+=page->meta_len;
    if(params->chunk_path && page->t_end-page->t_start+1<params->l) {
        long long *ts=NULL;double *values=NULL;int count=0;char err[512]={0};
        FetchRange(ds,page->t_start,page->t_end,&ts,&values,&count);g_last_stats.raw_points+=count;
        char *built=NULL;size_t length=0;
        if(indb_build_page_meta(ts,values,count,params,params->seed,&built,&length,err,sizeof(err))!=0)
            ereport(ERROR,(errmsg("Short Chunk page failed: %s",err)));
        pfree(page->meta);page->meta=(char*)palloc(length);page->meta_len=length;
        memcpy(page->meta,built,length);indb_free(built);pfree(ts);pfree(values);
    }
}
/* Collects the medoid candidate subsequences by sampling pages. */
static int SampleCandidates(int ds, const indb_params *params, const PageRow *pages, int page_count,
                            int max_sequences, double **out)
{
    if (page_count <= 0 || max_sequences <= 0)
        return 0;

    long long total_weight = 0;
    for (int i = 0; i < page_count; i++)
        total_weight += (pages[i].t_end - pages[i].t_start + 1);
    if (total_weight <= 0)
        return 0;

    double *buffer = (double *) palloc(sizeof(double) * (size_t) max_sequences * params->l);
    int collected = 0;
    uint32 state = (uint32) (params->seed ^ 0x9E3779B9U);

    for (int attempt = 0; attempt < max_sequences * 8 && collected < max_sequences; attempt++) {
        /* Deterministic xorshift so repeated runs agree. */
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        long long pick = (long long) (state % (unsigned long long) total_weight);
        int index = 0;
        long long acc = 0;
        for (int i = 0; i < page_count; i++) {
            acc += (pages[i].t_end - pages[i].t_start + 1);
            if (pick < acc) {
                index = i;
                break;
            }
        }

        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        if ((double) (state % 1000) / 1000.0 > params->r)
            continue;

        long long *ts = NULL;
        double *values = NULL;
        int count = 0;
        FetchRange(ds, pages[index].t_start, pages[index].t_end, &ts, &values, &count);
        g_last_stats.raw_points+=count;
        if (count >= params->l) {
            const int capacity = max_sequences * params->l;
            int produced = 0;
            char err[512] = {0};
            if (indb_grid_subsequences(ts, values, count, params->l,
                                       buffer + (size_t) collected * params->l,
                                       capacity - collected * params->l, &produced, err,
                                       sizeof(err)) != 0)
                produced = 0;
            collected += produced / params->l;
        }
        pfree(ts);
        pfree(values);
        CHECK_FOR_INTERRUPTS();
    }

    *out = buffer;
    return collected;
}

static ArrayType *RunQuery(int32 ds, int64 t0, int64 t1, const char *level, bool medoid, bool raw)
{
    const char *profile=GetConfigOptionByName("journal.profile",NULL,true);
    indb_query_memory_start(profile && strcmp(profile,"true")==0);
    indb_params params;
    memset(&params, 0, sizeof(params));
    bool found = false;
    if (!LoadParams(ds, &params, &found) || !found)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("dataset %d has no metadata; run indb_tsc_build first", ds)));

    const char *tp=GetConfigOptionByName("journal.theta_page",NULL,true);
    const char *tc=GetConfigOptionByName("journal.theta_chunk",NULL,true);
    if(tp && *tp) params.theta_page=atof(tp);
    if(tc && *tc) params.theta_chunk=atof(tc);
    const char *ablation=GetConfigOptionByName("journal.ablation",NULL,true);
    if(ablation && strcmp(ablation,"coarse")==0)params.theta_chunk=0.0;
    const int capacity = params.k * params.l;
    double *centers = (double *) SPI_palloc(sizeof(double) * (size_t) capacity);
    int produced = 0;
    char err[1024] = {0};
    memset(&g_last_stats, 0, sizeof(g_last_stats));
    journal_candidate_count=0;journal_candidate_hash=0xcbf29ce484222325ULL;

    if (raw) {
        long long *ts = NULL;
        double *values = NULL;
        int count = 0;
        FetchRange(ds, t0, t1, &ts, &values, &count);
        g_last_stats.raw_points = count;
        if (indb_raw(medoid ? 1 : 0, ts, values, count, &params, centers, capacity, &produced, err,
                     sizeof(err)) != 0)
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", err)));
        pfree(ts);
        pfree(values);
    } else {
        const bool chunk_level = level != NULL && strcmp(level, "chunk") == 0;
        params.chunk_path=chunk_level?1:0;
        PageRow *pages = NULL;
        int page_count = LoadPages(ds, t0, t1, &pages,chunk_level);
        g_last_stats.pages_considered = page_count;


        const char **unit_metas = (const char **) palloc(sizeof(char *) * (page_count + 1));
        size_t *unit_lens = (size_t *) palloc(sizeof(size_t) * (page_count + 1));
        int unit_count = 0;

        if (chunk_level) {
            ChunkRow *chunks = NULL;
            int chunk_count = LoadChunks(ds, t0, t1, &chunks);
            for (int c = 0; c < chunk_count; c++) {
                double ratio = OverlapRatio(chunks[c].t_start, chunks[c].t_end, t0, t1);
                if (ratio <= 0.0)
                    continue;
                if (ratio >= params.theta_chunk) {
                    if(ablation && strcmp(ablation,"dynamic")==0) {
                      PageRow *all=NULL;int np=LoadPages(ds,chunks[c].t_start,chunks[c].t_end,&all,true);
                      const char **metas=(const char**)palloc(sizeof(char*)*np);size_t *lens=(size_t*)palloc(sizeof(size_t)*np);
                      for(int i=0;i<np;i++){EnsurePageMeta(ds,&all[i],&params);metas[i]=all[i].meta;lens[i]=all[i].meta_len;}
                      char *built=NULL;size_t built_len=0;
                      if(indb_build_chunk_meta(metas,lens,np,&params,&built,&built_len,err,sizeof(err))!=0)ereport(ERROR,(errmsg("%s",err)));
                      unit_metas[unit_count]=built;unit_lens[unit_count]=built_len;
                    }else {
                      unit_metas[unit_count] = chunks[c].meta;
                      unit_lens[unit_count] = chunks[c].meta_len;
                    }
                    unit_count++;
                    g_last_stats.chunks_meta++;
                    continue;
                }
                g_last_stats.chunks_descended++;
                for (int p = 0; p < page_count; p++) {
                    if (pages[p].page_no < chunks[c].page_from ||
                        pages[p].page_no > chunks[c].page_to)
                        continue;
                    double page_ratio = OverlapRatio(pages[p].t_start, pages[p].t_end, t0, t1);
                    if (page_ratio <= 0.0)
                        continue;
                    EnsurePageMeta(ds,&pages[p],&params);
                    if (page_ratio >= params.theta_page) {
                        unit_metas[unit_count] = pages[p].meta;
                        unit_lens[unit_count] = pages[p].meta_len;
                        unit_count++;
                        g_last_stats.pages_meta++;
                    } else {
                        long long *ts = NULL;
                        double *values = NULL;
                        int count = 0;
                        FetchRange(ds, pages[p].t_start, pages[p].t_end, &ts, &values, &count);
                        char *corrected = NULL;
                        size_t corrected_len = 0;
                        if (indb_correct_page(pages[p].meta, pages[p].meta_len, ts, values, count,
                                              t0, t1, &params, &corrected, &corrected_len,
                                              &g_last_stats, err, sizeof(err)) != 0)
                            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                            errmsg("%s", err)));
                        unit_metas[unit_count] = corrected;
                        unit_lens[unit_count] = corrected_len;
                        unit_count++;
                        g_last_stats.pages_corrected++;
                        pfree(ts);
                        pfree(values);
                    }
                }
            }
        } else {
            for (int p = 0; p < page_count; p++) {
                double ratio = OverlapRatio(pages[p].t_start, pages[p].t_end, t0, t1);
                if (ratio <= 0.0)
                    continue;
                EnsurePageMeta(ds,&pages[p],&params);
                if (ratio >= params.theta_page) {
                    unit_metas[unit_count] = pages[p].meta;
                    unit_lens[unit_count] = pages[p].meta_len;
                    unit_count++;
                    g_last_stats.pages_meta++;
                } else {
                    long long *ts = NULL;
                    double *values = NULL;
                    int count = 0;
                    FetchRange(ds, pages[p].t_start, pages[p].t_end, &ts, &values, &count);
                    char *corrected = NULL;
                    size_t corrected_len = 0;
                    if (indb_correct_page(pages[p].meta, pages[p].meta_len, ts, values, count, t0,
                                          t1, &params, &corrected, &corrected_len, &g_last_stats,
                                          err, sizeof(err)) != 0)
                        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", err)));
                    unit_metas[unit_count] = corrected;
                    unit_lens[unit_count] = corrected_len;
                    unit_count++;
                    g_last_stats.pages_corrected++;
                    pfree(ts);
                    pfree(values);
                }
            }
        }

        g_last_stats.units_merged = unit_count;
        if (unit_count == 0) {
            /* No overlap: return an empty array. */
            return FinishQueryResult(centers, 0);
        }

        if (medoid) {
            double *candidates = NULL;
            const int max_sequences = params.k * 32 > 128 ? params.k * 32 : 128;
            int candidate_count = SampleCandidates(ds, &params, pages, page_count, max_sequences,
                                                   &candidates);
            journal_candidate_count=candidate_count;
            if (candidate_count <= 0) {
                return FinishQueryResult(centers, 0);
            }
            if (indb_merge_medoid(unit_metas, unit_lens, unit_count, candidates, candidate_count,
                                  &params, centers, capacity, &produced, err, sizeof(err)) != 0)
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", err)));
            pfree(candidates);
        } else {
            if (indb_merge(unit_metas, unit_lens, unit_count, &params, centers, capacity, &produced,
                           err, sizeof(err)) != 0)
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", err)));
        }
    }

    return FinishQueryResult(centers, produced);
}

PG_FUNCTION_INFO_V1(indb_tsc_kshape);
PGDLLEXPORT Datum indb_tsc_kshape(PG_FUNCTION_ARGS)
{
    int32 ds = PG_GETARG_INT32(0);
    int64 t0 = PG_GETARG_INT64(1);
    int64 t1 = PG_GETARG_INT64(2);
    char *level = PG_ARGISNULL(3) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(3));
    SPI_connect();
    ArrayType *result = RunQuery(ds, t0, t1, level, false, false);
    journal_query_memory=MemoryContextStrdup(TopMemoryContext,indb_query_memory_stop());
    PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(indb_tsc_mshape);
PGDLLEXPORT Datum indb_tsc_mshape(PG_FUNCTION_ARGS)
{
    int32 ds = PG_GETARG_INT32(0);
    int64 t0 = PG_GETARG_INT64(1);
    int64 t1 = PG_GETARG_INT64(2);
    char *level = PG_ARGISNULL(3) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(3));
    SPI_connect();
    ArrayType *result = RunQuery(ds, t0, t1, level, true, false);
    journal_query_memory=MemoryContextStrdup(TopMemoryContext,indb_query_memory_stop());
    PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(indb_tsc_kshape_raw);
PGDLLEXPORT Datum indb_tsc_kshape_raw(PG_FUNCTION_ARGS)
{
    int32 ds = PG_GETARG_INT32(0);
    int64 t0 = PG_GETARG_INT64(1);
    int64 t1 = PG_GETARG_INT64(2);
    SPI_connect();
    ArrayType *result = RunQuery(ds, t0, t1, NULL, false, true);
    journal_query_memory=MemoryContextStrdup(TopMemoryContext,indb_query_memory_stop());
    PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(indb_tsc_mshape_raw);
PGDLLEXPORT Datum indb_tsc_mshape_raw(PG_FUNCTION_ARGS)
{
    int32 ds = PG_GETARG_INT32(0);
    int64 t0 = PG_GETARG_INT64(1);
    int64 t2 = PG_GETARG_INT64(2);
    SPI_connect();
    ArrayType *result = RunQuery(ds, t0, t2, NULL, true, true);
    journal_query_memory=MemoryContextStrdup(TopMemoryContext,indb_query_memory_stop());
    PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(indb_tsc_last_stats);
PGDLLEXPORT Datum indb_tsc_last_stats(PG_FUNCTION_ARGS)
{
    double values[10];
    values[0] = (double) g_last_stats.pages_considered;
    values[1] = (double) g_last_stats.pages_meta;
    values[2] = (double) g_last_stats.pages_corrected;
    values[3] = (double) g_last_stats.chunks_meta;
    values[4] = (double) g_last_stats.chunks_descended;
    values[5] = (double) g_last_stats.units_merged;
    values[6] = (double) g_last_stats.raw_points;
    values[7] = (double) g_last_stats.meta_bytes;
    values[8]=(double)indb_last_merged_count();values[9]=journal_candidate_count;
    PG_RETURN_ARRAYTYPE_P(DoublesToArray(values, 10));
}

PG_FUNCTION_INFO_V1(indb_tsc_journal_candidate_hash);
PGDLLEXPORT Datum indb_tsc_journal_candidate_hash(PG_FUNCTION_ARGS) {
  char value[32];snprintf(value,sizeof(value),"%016llx",journal_candidate_hash);PG_RETURN_TEXT_P(cstring_to_text(value));
}

PG_FUNCTION_INFO_V1(indb_tsc_journal_memory);
PGDLLEXPORT Datum indb_tsc_journal_memory(PG_FUNCTION_ARGS){PG_RETURN_TEXT_P(cstring_to_text(journal_query_memory?journal_query_memory:"null"));}
