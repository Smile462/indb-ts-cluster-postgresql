//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Shared clustering kernel for the RocksDB implementation.
//
// The kernel is storage agnostic: adapters hand it raw points or previously
// persisted page metadata and it returns cluster centres. This is the
// authoritative copy; the PostgreSQL repository vendors an identical snapshot.
//
// Semantics follow the IoTDB reference implementation:
//   - subsequences are aligned to the absolute time grid [i*l, (i+1)*l)
//   - a page stores head/tail partial subsequences so that adjacent pages can
//     be stitched back together
//   - page metadata holds per-cluster summed outer products, which are merged
//     at query time and reduced to one eigenvector per cluster
// Deviations from the reference are listed in docs/algorithm-notes.md.

#pragma once

#include <cstdint>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace indb {

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------

struct Mat {
  int rows = 0;
  int cols = 0;
  std::vector<double> v;

  Mat() = default;
  Mat(int r, int c) : rows(r), cols(c), v(static_cast<size_t>(r) * c, 0.0) {}
  double& at(int r, int c) { return v[static_cast<size_t>(r) * cols + c]; }
  double at(int r, int c) const { return v[static_cast<size_t>(r) * cols + c]; }
  double* row(int r) { return v.data() + static_cast<size_t>(r) * cols; }
  const double* row(int r) const { return v.data() + static_cast<size_t>(r) * cols; }
};

using Vec = std::vector<double>;

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

struct Config {
  int k = 3;                 // number of clusters
  int l = 166;               // subsequence length
  int edk = 0;               // approximate clusters for Medoid-Shape, 0 => k + 2
  double r = 0.2;            // Medoid sampling rate
  int iter_build = 30;       // iterations when building persisted metadata
  int iter_raw = 100;        // iterations for from-scratch clustering
  int page_points = 10240;   // logical page capacity in points
  int chunk_pages = 8;       // logical chunk size in pages
  double theta_chunk = 0.8;  // chunk coverage threshold
  double theta_page = 0.8;   // page coverage threshold
  uint64_t seed = 0x5EED;    // base seed for deterministic replay

  int edk_effective() const { return edk > 0 ? edk : k + 2; }
};

// ---------------------------------------------------------------------------
// Deterministic randomness
// ---------------------------------------------------------------------------

class Rng {
 public:
  explicit Rng(uint64_t seed) : gen_(seed) {}
  int next_int(int bound);
  double next_double();

 private:
  std::mt19937_64 gen_;
};

class AlgorithmError : public std::runtime_error {
 public:
  explicit AlgorithmError(const std::string& what) : std::runtime_error(what) {}
};

// ---------------------------------------------------------------------------
// Basic numerics
// ---------------------------------------------------------------------------

Vec zscore(const double* x, int l);
inline Vec zscore(const Vec& x) { return zscore(x.data(), static_cast<int>(x.size())); }
bool all_finite(const Vec& x);

int fft_size_for(int l);
void fft(const double* re, const double* im, int n, double* out_re, double* out_im);
void ifft(const double* re, const double* im, int n, double* out_re, double* out_im);

// Cross correlation already divided by |x|*|y|, laid out like the reference.
Vec ncc(const double* x, const double* y, int l);

// Distance used when comparing stored centroids (reference divides twice).
double sbd(const double* x, const double* y, int l);
inline double sbd(const Vec& x, const Vec& y) {
  return sbd(x.data(), y.data(), static_cast<int>(x.size()));
}
// Distance used inside the K-Shape assignment loop (single division).
double sbd_assignment(const double* x, const double* y, int l);

double norm2(const double* x, int l);
double l2_squared(const double* x, const double* y, int l);

struct EigenResult {
  Vec values;   // ascending
  Mat vectors;  // one eigenvector per column
  bool ties_detected = false;
};
EigenResult eigen_symmetric(const Mat& m);

Vec extract_shape(const Mat& sum_matrix, const Vec& reference);
Vec extract_shape_population(const Mat& sum_matrix, const std::vector<Vec>& population);

// ---------------------------------------------------------------------------
// Clustering
// ---------------------------------------------------------------------------

struct KShapeResult {
  Mat centroids;
  std::vector<Mat> sum_matrices;
  Vec deltas;
  std::vector<int> idx;
  int iterations_run = 0;
  int empty_clusters = 0;
};
KShapeResult kshape_fit(const std::vector<Vec>& x, int k, int max_iter, Rng& rng);

struct KMeansResult {
  Mat centroids;
  std::vector<int> idx;
  Vec deltas;
  bool empty_cluster_filled = false;
};
KMeansResult kmeans(int k, int iterations, const std::vector<Vec>& points, Rng& rng);

// ---------------------------------------------------------------------------
// Persisted metadata
// ---------------------------------------------------------------------------

struct Points {
  std::vector<int64_t> ts;
  std::vector<double> values;
  size_t size() const { return ts.size(); }
};

constexpr int32_t kMetaVersion = 1;

enum class Level { Page, Chunk };

struct PageMeta {
  int32_t version = kMetaVersion;
  int32_t k = 0;
  int32_t l = 0;
  int32_t edk = 0;
  int64_t t_start = 0;
  int64_t t_end = 0;
  int64_t count = 0;
  int32_t n_seq = 0;

  std::vector<double> sum_matrices;  // k * l * l
  std::vector<double> centroids;     // k * l
  std::vector<double> deltas;        // k
  std::vector<int32_t> idx;          // n_seq

  std::vector<double> ed_centroids;  // edk * l
  std::vector<double> ed_deltas;     // edk
  std::vector<int32_t> ed_counts;    // edk
  std::vector<int32_t> ed_idx;       // n_seq

  std::vector<double> head_extra;  // l, left aligned
  std::vector<double> tail_extra;  // l, right aligned

  bool is_chunk = false;

  double sum_matrix(int c, int r, int q) const {
    return sum_matrices[(static_cast<size_t>(c) * l + r) * l + q];
  }
  void set_sum_matrix(int c, int r, int q, double value) {
    sum_matrices[(static_cast<size_t>(c) * l + r) * l + q] = value;
  }
  double centroid(int c, int pos) const { return centroids[static_cast<size_t>(c) * l + pos]; }
  void set_centroid(int c, int pos, double value) {
    centroids[static_cast<size_t>(c) * l + pos] = value;
  }
  double ed_centroid(int c, int pos) const {
    return ed_centroids[static_cast<size_t>(c) * l + pos];
  }
  void set_ed_centroid(int c, int pos, double value) {
    ed_centroids[static_cast<size_t>(c) * l + pos] = value;
  }
};

PageMeta build_page_meta(const Points& points, const Config& config, uint64_t seed);
PageMeta build_chunk_meta(const std::vector<PageMeta>& pages, const Config& config);

std::string encode_meta(const PageMeta& meta);
PageMeta decode_meta(const std::string& bytes);
void validate_meta(const PageMeta& meta, const Config& config);
double overlap_ratio(const PageMeta& meta, int64_t t0, int64_t t1);

// ---------------------------------------------------------------------------
// Query paths
// ---------------------------------------------------------------------------

struct PageRef {
  int64_t page_no = 0;
  PageMeta meta;
};
struct ChunkRef {
  int64_t chunk_no = 0;
  int64_t page_from = 0;
  int64_t page_to = 0;
  PageMeta meta;
};

using RawPageLoader = std::function<bool(int64_t page_no, Points* out)>;
using SampledSource = std::function<bool(int max_sequences, std::vector<Vec>* out)>;

struct QueryStats {
  int64_t pages_considered = 0;
  int64_t pages_meta_used = 0;
  int64_t pages_corrected = 0;
  int64_t chunks_meta_used = 0;
  int64_t chunks_descended = 0;
  int64_t units_merged = 0;
  int64_t raw_points_read = 0;
  int64_t meta_bytes_read = 0;
};

struct QueryResult {
  std::vector<Vec> centers;
  QueryStats stats;
};

QueryResult kshape_raw(const Points& points, const Config& config);
QueryResult mshape_raw(const Points& points, const Config& config);

QueryResult kshape_query(const std::vector<ChunkRef>& chunks, const std::vector<PageRef>& pages,
                         int64_t t0, int64_t t1, const Config& config, Level level,
                         const RawPageLoader& loader);

QueryResult mshape_query(const std::vector<ChunkRef>& chunks, const std::vector<PageRef>& pages,
                         int64_t t0, int64_t t1, const Config& config, Level level,
                         const RawPageLoader& loader, const SampledSource& sampler);

std::vector<Vec> grid_subsequences(const Points& points, int l);
PageMeta correct_page(const PageMeta& meta, const Points& raw, int64_t t0, int64_t t1,
                      const Config& config, QueryStats* stats);

// Merge helpers for adapters that perform page selection themselves and then
// hand the chosen metadata records to the kernel. Both apply the same merge
// rules as the built-in query paths.
QueryResult kshape_centers(const std::vector<PageMeta>& units, const Config& config);
QueryResult medoid_centers(const std::vector<PageMeta>& units, const std::vector<Vec>& candidates,
                           const Config& config);

}  // namespace indb
