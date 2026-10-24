//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Implementation of the C bridge declared in bridge.h.

#include "bridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "indb/core.h"

namespace {

indb::Config ToConfig(const indb_params* params) {
  indb::Config config;
  config.k = params->k;
  config.l = params->l;
  config.edk = params->edk;
  config.iter_build = params->iter_build;
  config.iter_raw = params->iter_raw;
  config.page_points = params->page_points;
  config.chunk_pages = params->chunk_pages;
  config.r = params->r;
  config.theta_page = params->theta_page;
  config.theta_chunk = params->theta_chunk;
  config.seed = params->seed;
  return config;
}

void WriteError(char* err, size_t err_len, const std::string& message) {
  if (err == nullptr || err_len == 0) return;
  std::snprintf(err, err_len, "%s", message.c_str());
}

indb::Points ToPoints(const long long* ts, const double* values, int n) {
  indb::Points points;
  points.ts.assign(ts, ts + n);
  points.values.assign(values, values + n);
  return points;
}

std::vector<indb::PageMeta> ToMetas(const char* const* metas, const size_t* lens, int n) {
  std::vector<indb::PageMeta> out;
  out.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    out.push_back(indb::decode_meta(std::string(metas[i], lens[i])));
  }
  return out;
}

int CopyMeta(const indb::PageMeta& meta, char** out_buf, size_t* out_len) {
  const std::string bytes = indb::encode_meta(meta);
  char* buffer = static_cast<char*>(std::malloc(bytes.size()));
  if (buffer == nullptr) return -1;
  std::memcpy(buffer, bytes.data(), bytes.size());
  *out_buf = buffer;
  *out_len = bytes.size();
  return 0;
}

int CopyCenters(const indb::QueryResult& result, double* out, int out_cap, int* out_n) {
  int total = 0;
  for (const auto& row : result.centers) total += static_cast<int>(row.size());
  if (total > out_cap) return -1;
  int position = 0;
  for (const auto& row : result.centers) {
    for (double value : row) out[position++] = value;
  }
  *out_n = total;
  return 0;
}

}  // namespace

extern "C" {

int indb_build_page_meta(const long long* ts, const double* values, int n,
                         const indb_params* params, unsigned long long page_seed,
                         char** out_buf, size_t* out_len, char* err, size_t err_len) {
  try {
    const indb::Config config = ToConfig(params);
    const indb::PageMeta meta = indb::build_page_meta(ToPoints(ts, values, n), config, page_seed);
    return CopyMeta(meta, out_buf, out_len);
  } catch (const std::exception& error) {
    WriteError(err, err_len, error.what());
    return -1;
  } catch (...) {
    WriteError(err, err_len, "unknown error in build_page_meta");
    return -1;
  }
}

int indb_build_chunk_meta(const char* const* metas, const size_t* lens, int n,
                          const indb_params* params, char** out_buf, size_t* out_len, char* err,
                          size_t err_len) {
  try {
    const indb::Config config = ToConfig(params);
    const indb::PageMeta chunk = indb::build_chunk_meta(ToMetas(metas, lens, n), config);
    return CopyMeta(chunk, out_buf, out_len);
  } catch (const std::exception& error) {
    WriteError(err, err_len, error.what());
    return -1;
  } catch (...) {
    WriteError(err, err_len, "unknown error in build_chunk_meta");
    return -1;
  }
}

int indb_correct_page(const char* meta, size_t meta_len, const long long* ts, const double* values,
                      int n, long long t0, long long t1, const indb_params* params,
                      char** out_buf, size_t* out_len, indb_stats* stats, char* err,
                      size_t err_len) {
  try {
    const indb::Config config = ToConfig(params);
    const indb::PageMeta original = indb::decode_meta(std::string(meta, meta_len));
    indb::QueryStats local;
    const indb::PageMeta corrected =
        indb::correct_page(original, ToPoints(ts, values, n), t0, t1, config, &local);
    if (stats != nullptr) {
      stats->raw_points += local.raw_points_read;
    }
    return CopyMeta(corrected, out_buf, out_len);
  } catch (const std::exception& error) {
    WriteError(err, err_len, error.what());
    return -1;
  } catch (...) {
    WriteError(err, err_len, "unknown error in correct_page");
    return -1;
  }
}

int indb_merge(const char* const* metas, const size_t* lens, int n, const indb_params* params,
               double* out, int out_cap, int* out_n, char* err, size_t err_len) {
  try {
    const indb::Config config = ToConfig(params);
    const indb::QueryResult result = indb::kshape_centers(ToMetas(metas, lens, n), config);
    return CopyCenters(result, out, out_cap, out_n);
  } catch (const std::exception& error) {
    WriteError(err, err_len, error.what());
    return -1;
  } catch (...) {
    WriteError(err, err_len, "unknown error in merge");
    return -1;
  }
}

int indb_merge_medoid(const char* const* metas, const size_t* lens, int n,
                      const double* candidates, int candidate_count, const indb_params* params,
                      double* out, int out_cap, int* out_n, char* err, size_t err_len) {
  try {
    const indb::Config config = ToConfig(params);
    std::vector<indb::Vec> sequences;
    sequences.reserve(static_cast<size_t>(candidate_count));
    for (int i = 0; i < candidate_count; ++i) {
      const double* begin = candidates + static_cast<size_t>(i) * config.l;
      sequences.emplace_back(begin, begin + config.l);
    }
    const indb::QueryResult result =
        indb::medoid_centers(ToMetas(metas, lens, n), sequences, config);
    return CopyCenters(result, out, out_cap, out_n);
  } catch (const std::exception& error) {
    WriteError(err, err_len, error.what());
    return -1;
  } catch (...) {
    WriteError(err, err_len, "unknown error in merge_medoid");
    return -1;
  }
}

int indb_raw(int medoid, const long long* ts, const double* values, int n,
             const indb_params* params, double* out, int out_cap, int* out_n, char* err,
             size_t err_len) {
  try {
    const indb::Config config = ToConfig(params);
    const indb::Points points = ToPoints(ts, values, n);
    const indb::QueryResult result =
        medoid ? indb::mshape_raw(points, config) : indb::kshape_raw(points, config);
    return CopyCenters(result, out, out_cap, out_n);
  } catch (const std::exception& error) {
    WriteError(err, err_len, error.what());
    return -1;
  } catch (...) {
    WriteError(err, err_len, "unknown error in raw clustering");
    return -1;
  }
}

int indb_grid_subsequences(const long long* ts, const double* values, int n, int l, double* out,
                           int out_cap, int* out_n, char* err, size_t err_len) {
  try {
    const std::vector<indb::Vec> sequences = indb::grid_subsequences(ToPoints(ts, values, n), l);
    const int total = static_cast<int>(sequences.size()) * l;
    if (total > out_cap) {
      WriteError(err, err_len, "grid_subsequences output buffer too small");
      return -1;
    }
    int position = 0;
    for (const auto& seq : sequences) {
      for (double value : seq) out[position++] = value;
    }
    *out_n = total;
    return 0;
  } catch (const std::exception& error) {
    WriteError(err, err_len, error.what());
    return -1;
  } catch (...) {
    WriteError(err, err_len, "unknown error in grid_subsequences");
    return -1;
  }
}

void indb_free(void* pointer) { std::free(pointer); }

}  // extern "C"
