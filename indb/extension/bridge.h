/*  Copyright (c) 2026 The in-database time-series clustering authors.
 *  This source code is licensed under both the GPLv2 (found in the
 *  COPYING file in the root directory) and Apache 2.0 License
 *  (found in the LICENSE.Apache file in the root directory).
 *
 * C interface to the C++ clustering kernel. PostgreSQL's extension entry
 * points are plain C, so every call into the kernel goes through these
 * functions: they translate plain arrays into the kernel's containers, catch
 * every C++ exception, and hand errors back as text.
 *
 * Buffers returned through out_buf are allocated with malloc and must be
 * released with indb_free.
 */

#ifndef INDB_BRIDGE_H
#define INDB_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct indb_params {
  int k;
  int l;
  int edk; /* 0 means k + 2 */
  int iter_build;
  int iter_raw;
  int page_points;
  int chunk_pages;
  double r;
  double theta_page;
  double theta_chunk;
  unsigned long long seed;
} indb_params;

typedef struct indb_stats {
  long long pages_considered;
  long long pages_meta;
  long long pages_corrected;
  long long chunks_meta;
  long long chunks_descended;
  long long units_merged;
  long long raw_points;
  long long meta_bytes;
} indb_stats;

/* Encodes the page metadata for one page of points. */
int indb_build_page_meta(const long long *ts, const double *values, int n,
                         const indb_params *params, unsigned long long page_seed,
                         char **out_buf, size_t *out_len, char *err,
                         size_t err_len);

/* Merges page metadata records into a chunk record. */
int indb_build_chunk_meta(const char *const *metas, const size_t *lens, int n,
                          const indb_params *params, char **out_buf,
                          size_t *out_len, char *err, size_t err_len);

/* Restricts a page record to [t0, t1] using the page's raw points. */
int indb_correct_page(const char *meta, size_t meta_len, const long long *ts,
                      const double *values, int n, long long t0, long long t1,
                      const indb_params *params, char **out_buf,
                      size_t *out_len, indb_stats *stats, char *err,
                      size_t err_len);

/* Merges records and extracts one eigenvector per cluster. */
int indb_merge(const char *const *metas, const size_t *lens, int n,
               const indb_params *params, double *out, int out_cap, int *out_n,
               char *err, size_t err_len);

/* Merges records and selects medoids from the supplied subsequences. */
int indb_merge_medoid(const char *const *metas, const size_t *lens, int n,
                      const double *candidates, int candidate_count,
                      const indb_params *params, double *out, int out_cap,
                      int *out_n, char *err, size_t err_len);

/* From-scratch clustering over raw points. medoid selects the method. */
int indb_raw(int medoid, const long long *ts, const double *values, int n,
             const indb_params *params, double *out, int out_cap, int *out_n,
             char *err, size_t err_len);

/* Grid aligned subsequences of a page, used by the medoid sampler. */
int indb_grid_subsequences(const long long *ts, const double *values, int n,
                           int l, double *out, int out_cap, int *out_n,
                           char *err, size_t err_len);

void indb_free(void *pointer);

#ifdef __cplusplus
}
#endif

#endif /* INDB_BRIDGE_H */
