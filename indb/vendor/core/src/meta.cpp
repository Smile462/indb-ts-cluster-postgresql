//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "indb/core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace indb {
namespace {

constexpr uint32_t kMagic = 0x494E444DU;  // "INDM"

int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}
int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a, b); }

int count_members(const std::vector<int32_t>& idx, int label) {
  int count = 0;
  for (int32_t v : idx) {
    if (v == label) ++count;
  }
  return count;
}

int nearest_by_sbd(const double* probe, const PageMeta& meta) {
  int best = -1;
  double best_distance = std::numeric_limits<double>::max();
  for (int c = 0; c < meta.k; ++c) {
    const double d = sbd(probe, meta.centroids.data() + static_cast<size_t>(c) * meta.l, meta.l);
    if (d < best_distance) {
      best_distance = d;
      best = c;
    }
  }
  return best;
}

int nearest_by_l2(const double* probe, const std::vector<double>& centroids, int k, int l) {
  int best = -1;
  double best_distance = std::numeric_limits<double>::max();
  for (int c = 0; c < k; ++c) {
    const double d = l2_squared(probe, centroids.data() + static_cast<size_t>(c) * l, l);
    if (d < best_distance) {
      best_distance = d;
      best = c;
    }
  }
  return best;
}

void put_i32(std::string* out, int32_t value) {
  out->append(reinterpret_cast<const char*>(&value), sizeof(value));
}
void put_i64(std::string* out, int64_t value) {
  out->append(reinterpret_cast<const char*>(&value), sizeof(value));
}
void put_u8(std::string* out, uint8_t value) { out->push_back(static_cast<char>(value)); }
void put_doubles(std::string* out, const std::vector<double>& v) {
  if (!v.empty()) out->append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(double));
}
void put_i32s(std::string* out, const std::vector<int32_t>& v) {
  if (!v.empty()) out->append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(int32_t));
}

class Reader {
 public:
  explicit Reader(const std::string& bytes) : bytes_(bytes) {}

  int32_t i32() {
    const int32_t value = read<int32_t>();
    return value;
  }
  int64_t i64() { return read<int64_t>(); }
  uint32_t u32() { return read<uint32_t>(); }
  uint8_t u8() {
    require(1);
    return static_cast<uint8_t>(bytes_[offset_++]);
  }
  void doubles(std::vector<double>* out, size_t count) {
    require(count * sizeof(double));
    out->resize(count);
    if (count != 0) std::memcpy(out->data(), bytes_.data() + offset_, count * sizeof(double));
    offset_ += count * sizeof(double);
  }
  void i32s(std::vector<int32_t>* out, size_t count) {
    require(count * sizeof(int32_t));
    out->resize(count);
    if (count != 0) std::memcpy(out->data(), bytes_.data() + offset_, count * sizeof(int32_t));
    offset_ += count * sizeof(int32_t);
  }

 private:
  template <typename T>
  T read() {
    require(sizeof(T));
    T value{};
    std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
    offset_ += sizeof(T);
    return value;
  }
  void require(size_t n) const {
    if (offset_ + n > bytes_.size()) throw AlgorithmError("truncated metadata record");
  }
  const std::string& bytes_;
  size_t offset_ = 0;
};

}  // namespace

PageMeta build_page_meta(const Points& points, const Config& config, uint64_t seed) {
  if (points.ts.size() != points.values.size()) throw AlgorithmError("inconsistent page points");
  const int l = config.l;
  if (l < 2) throw AlgorithmError("seq length must be at least 2");

  PageMeta meta;
  meta.version = kMetaVersion;
  meta.k = config.k;
  meta.l = l;
  meta.edk = config.edk_effective();
  meta.sum_matrices.assign(static_cast<size_t>(config.k) * l * l, 0.0);
  meta.centroids.assign(static_cast<size_t>(config.k) * l, 0.0);
  meta.deltas.assign(static_cast<size_t>(config.k), 0.0);
  meta.ed_centroids.assign(static_cast<size_t>(meta.edk) * l, 0.0);
  meta.ed_deltas.assign(static_cast<size_t>(meta.edk), 0.0);
  meta.ed_counts.assign(static_cast<size_t>(meta.edk), 0);
  meta.head_extra.assign(static_cast<size_t>(l), 0.0);
  meta.tail_extra.assign(static_cast<size_t>(l), 0.0);

  if (points.ts.empty()) return meta;
  meta.t_start = points.ts.front();
  meta.t_end = points.ts.back();
  meta.count = static_cast<int64_t>(points.ts.size());
  if (static_cast<int>(points.ts.size()) < l) return meta;

  const int64_t seq_start = ceil_div(meta.t_start, l) * l;
  std::vector<Vec> sequences;
  Vec current;
  for (size_t i = 0; i < points.ts.size(); ++i) {
    if (points.ts[i] < seq_start) {
      if (static_cast<int>(i) < l) meta.head_extra[i] = points.values[i];
      continue;
    }
    current.push_back(points.values[i]);
    if (static_cast<int>(current.size()) == l) {
      sequences.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    const size_t begin = static_cast<size_t>(l) - current.size();
    for (size_t i = 0; i < current.size(); ++i) meta.tail_extra[begin + i] = current[i];
  }

  meta.n_seq = static_cast<int32_t>(sequences.size());
  // A short or badly aligned page can yield no complete subsequence. The
  // reference throws here; this implementation keeps an empty record.
  if (sequences.empty()) return meta;

  std::vector<Vec> normalized;
  normalized.reserve(sequences.size());
  for (const Vec& seq : sequences) normalized.push_back(zscore(seq));

  Rng rng(seed);
  // The reference runs K-Shape first and then K-Means over the same
  // subsequences, both with the build iteration count.
  const KShapeResult kshape = kshape_fit(normalized, config.k, config.iter_build, rng);
  for (int c = 0; c < config.k; ++c) {
    for (int r = 0; r < l; ++r) {
      for (int q = 0; q < l; ++q) meta.set_sum_matrix(c, r, q, kshape.sum_matrices[static_cast<size_t>(c)].at(r, q));
    }
    for (int r = 0; r < l; ++r) meta.set_centroid(c, r, kshape.centroids.at(c, r));
    meta.deltas[static_cast<size_t>(c)] = kshape.deltas[static_cast<size_t>(c)];
  }
  meta.idx.assign(kshape.idx.begin(), kshape.idx.end());

  const KMeansResult approx = kmeans(meta.edk, config.iter_build, normalized, rng);
  for (int c = 0; c < meta.edk; ++c) {
    for (int r = 0; r < l; ++r) meta.set_ed_centroid(c, r, approx.centroids.at(c, r));
    meta.ed_deltas[static_cast<size_t>(c)] = approx.deltas[static_cast<size_t>(c)];
  }
  meta.ed_idx.assign(approx.idx.begin(), approx.idx.end());
  for (int32_t label : meta.ed_idx) {
    if (label >= 0 && label < meta.edk) ++meta.ed_counts[static_cast<size_t>(label)];
  }
  return meta;
}

PageMeta build_chunk_meta(const std::vector<PageMeta>& pages, const Config& config) {
  if (pages.empty()) throw AlgorithmError("cannot merge an empty page list");
  const int l = config.l;
  const int k = config.k;
  const int edk = config.edk_effective();

  PageMeta chunk = pages.front();
  chunk.is_chunk = true;

  std::vector<double> ed_centroids = pages.front().ed_centroids;
  std::vector<int32_t> ed_counts = pages.front().ed_counts;

  std::vector<int> counts(static_cast<size_t>(k), 0);
  for (int c = 0; c < k; ++c) counts[static_cast<size_t>(c)] = count_members(chunk.idx, c);

  for (size_t i = 1; i < pages.size(); ++i) {
    const PageMeta& page = pages[i];

    for (int j = 0; j < page.k; ++j) {
      const int nearest = nearest_by_sbd(page.centroids.data() + static_cast<size_t>(j) * l, chunk);
      if (nearest < 0) continue;
      for (int r = 0; r < l; ++r) {
        for (int q = 0; q < l; ++q) {
          chunk.set_sum_matrix(nearest, r, q, chunk.sum_matrix(nearest, r, q) + page.sum_matrix(j, r, q));
        }
      }
      const int page_count = count_members(page.idx, j);
      const int total = counts[static_cast<size_t>(nearest)] + page_count;
      for (int r = 0; r < l; ++r) {
        const double weighted = chunk.centroid(nearest, r) * counts[static_cast<size_t>(nearest)] +
                                page.centroid(j, r) * page_count;
        chunk.set_centroid(nearest, r, total == 0 ? 0.0 : weighted / total);
      }
      counts[static_cast<size_t>(nearest)] += page_count;
    }

    for (int j = 0; j < page.edk; ++j) {
      if (page.ed_counts[static_cast<size_t>(j)] == 0) continue;
      const int nearest = nearest_by_l2(page.ed_centroids.data() + static_cast<size_t>(j) * l,
                                        ed_centroids, edk, l);
      if (nearest < 0) continue;
      const int total = ed_counts[static_cast<size_t>(nearest)] + page.ed_counts[static_cast<size_t>(j)];
      for (int r = 0; r < l; ++r) {
        const double weighted = ed_centroids[static_cast<size_t>(nearest) * l + r] *
                                    ed_counts[static_cast<size_t>(nearest)] +
                                page.ed_centroids[static_cast<size_t>(j) * l + r] *
                                    page.ed_counts[static_cast<size_t>(j)];
        ed_centroids[static_cast<size_t>(nearest) * l + r] = total == 0 ? 0.0 : weighted / total;
      }
      ed_counts[static_cast<size_t>(nearest)] += page.ed_counts[static_cast<size_t>(j)];
    }

    bool has_head = false;
    for (double v : page.head_extra) {
      if (v != 0.0) {
        has_head = true;
        break;
      }
    }
    if (has_head) {
      // head + tail reconstructs the subsequence that straddles the boundary.
      Vec merged(static_cast<size_t>(l));
      for (int r = 0; r < l; ++r) {
        merged[static_cast<size_t>(r)] =
            page.head_extra[static_cast<size_t>(r)] + pages[i - 1].tail_extra[static_cast<size_t>(r)];
      }
      merged = zscore(merged);

      const int nearest = nearest_by_sbd(merged.data(), chunk);
      if (nearest >= 0) {
        for (int r = 0; r < l; ++r) {
          for (int q = 0; q < l; ++q) {
            chunk.set_sum_matrix(nearest, r, q,
                                 chunk.sum_matrix(nearest, r, q) +
                                     merged[static_cast<size_t>(r)] * merged[static_cast<size_t>(q)]);
          }
        }
        ++counts[static_cast<size_t>(nearest)];
      }

      const int nearest_ed = nearest_by_l2(merged.data(), ed_centroids, edk, l);
      if (nearest_ed >= 0) {
        const int total = ed_counts[static_cast<size_t>(nearest_ed)] + 1;
        for (int r = 0; r < l; ++r) {
          const double weighted =
              ed_centroids[static_cast<size_t>(nearest_ed) * l + r] *
                  ed_counts[static_cast<size_t>(nearest_ed)] +
              merged[static_cast<size_t>(r)];
          ed_centroids[static_cast<size_t>(nearest_ed) * l + r] = weighted / total;
        }
        ++ed_counts[static_cast<size_t>(nearest_ed)];
      }
    }
  }

  chunk.ed_centroids = std::move(ed_centroids);
  chunk.ed_counts = std::move(ed_counts);
  chunk.head_extra = pages.front().head_extra;
  chunk.tail_extra = pages.back().tail_extra;
  chunk.t_start = pages.front().t_start;
  chunk.t_end = pages.back().t_end;
  chunk.count = 0;
  for (const PageMeta& page : pages) chunk.count += page.count;
  return chunk;
}

std::string encode_meta(const PageMeta& meta) {
  std::string out;
  out.reserve(256 + meta.sum_matrices.size() * sizeof(double) + meta.centroids.size() * sizeof(double));
  put_i32(&out, static_cast<int32_t>(kMagic));
  put_i32(&out, meta.version);
  put_i32(&out, meta.k);
  put_i32(&out, meta.l);
  put_i32(&out, meta.edk);
  put_i32(&out, meta.n_seq);
  put_i64(&out, meta.t_start);
  put_i64(&out, meta.t_end);
  put_i64(&out, meta.count);
  put_u8(&out, meta.is_chunk ? 1 : 0);
  put_doubles(&out, meta.sum_matrices);
  put_doubles(&out, meta.centroids);
  put_doubles(&out, meta.deltas);
  put_i32s(&out, meta.idx);
  put_doubles(&out, meta.ed_centroids);
  put_doubles(&out, meta.ed_deltas);
  put_i32s(&out, meta.ed_counts);
  put_i32s(&out, meta.ed_idx);
  put_doubles(&out, meta.head_extra);
  put_doubles(&out, meta.tail_extra);
  return out;
}

PageMeta decode_meta(const std::string& bytes) {
  Reader reader(bytes);
  if (reader.u32() != kMagic) throw AlgorithmError("metadata magic mismatch");

  PageMeta meta;
  meta.version = reader.i32();
  meta.k = reader.i32();
  meta.l = reader.i32();
  meta.edk = reader.i32();
  meta.n_seq = reader.i32();
  meta.t_start = reader.i64();
  meta.t_end = reader.i64();
  meta.count = reader.i64();
  meta.is_chunk = reader.u8() != 0;

  if (meta.version != kMetaVersion) {
    throw AlgorithmError("unsupported metadata version " + std::to_string(meta.version));
  }
  if (meta.k <= 0 || meta.l < 2 || meta.edk <= 0 || meta.n_seq < 0) {
    throw AlgorithmError("metadata header has invalid dimensions");
  }

  const size_t k = static_cast<size_t>(meta.k);
  const size_t l = static_cast<size_t>(meta.l);
  const size_t edk = static_cast<size_t>(meta.edk);
  const size_t n = static_cast<size_t>(meta.n_seq);

  reader.doubles(&meta.sum_matrices, k * l * l);
  reader.doubles(&meta.centroids, k * l);
  reader.doubles(&meta.deltas, k);
  reader.i32s(&meta.idx, n);
  reader.doubles(&meta.ed_centroids, edk * l);
  reader.doubles(&meta.ed_deltas, edk);
  reader.i32s(&meta.ed_counts, edk);
  reader.i32s(&meta.ed_idx, n);
  reader.doubles(&meta.head_extra, l);
  reader.doubles(&meta.tail_extra, l);
  return meta;
}

void validate_meta(const PageMeta& meta, const Config& config) {
  if (meta.version != kMetaVersion) throw AlgorithmError("metadata version mismatch");
  if (meta.k != config.k || meta.l != config.l || meta.edk != config.edk_effective()) {
    throw AlgorithmError("metadata parameters (k=" + std::to_string(meta.k) + ", l=" +
                         std::to_string(meta.l) + ", edk=" + std::to_string(meta.edk) +
                         ") do not match the query (k=" + std::to_string(config.k) + ", l=" +
                         std::to_string(config.l) + ", edk=" +
                         std::to_string(config.edk_effective()) + ")");
  }
}

double overlap_ratio(const PageMeta& meta, int64_t t0, int64_t t1) {
  const int64_t start = std::max(meta.t_start, t0);
  const int64_t end = std::min(meta.t_end, t1);
  if (start > end) return 0.0;
  const double total = static_cast<double>(meta.t_end - meta.t_start + 1);
  return total <= 0.0 ? 0.0 : static_cast<double>(end - start + 1) / total;
}

}  // namespace indb
