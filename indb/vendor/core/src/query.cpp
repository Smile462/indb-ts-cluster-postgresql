//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "indb/core.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace indb {
namespace {

int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}
int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a, b); }

bool on_grid_boundary(int64_t ts, int l) {
  const int64_t anchor = ts + 1;
  return ((anchor % l) + l) % l == 0;
}

void subtract_outer(std::vector<double>* matrix, const Vec& seq, int l) {
  for (int r = 0; r < l; ++r) {
    for (int q = 0; q < l; ++q) {
      (*matrix)[static_cast<size_t>(r) * l + q] -= seq[static_cast<size_t>(r)] * seq[static_cast<size_t>(q)];
    }
  }
}

void remove_from_mean(std::vector<double>* centroids, std::vector<int32_t>* counts, int cluster,
                      const Vec& seq, int l) {
  const int count = (*counts)[static_cast<size_t>(cluster)];
  if (count <= 1) {
    for (int r = 0; r < l; ++r) (*centroids)[static_cast<size_t>(cluster) * l + r] = 0.0;
    (*counts)[static_cast<size_t>(cluster)] = 0;
    return;
  }
  for (int r = 0; r < l; ++r) {
    double& value = (*centroids)[static_cast<size_t>(cluster) * l + r];
    value = (value * count - seq[static_cast<size_t>(r)]) / (count - 1);
  }
  (*counts)[static_cast<size_t>(cluster)] = count - 1;
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

double max_correlation(const double* a, const double* b, int l) {
  const Vec corr = ncc(a, b, l);
  double best = std::numeric_limits<double>::lowest();
  for (double v : corr) best = std::max(best, v);
  return best;
}

// Greedy maximum coverage used by Medoid-Shape: repeatedly add the sampled
// subsequence that maximises sum_i max_j ncc(ed_centroid_i, coreset_j) * count_i.
std::vector<Vec> greedy_coreset(const std::vector<Vec>& candidates,
                                const std::vector<double>& ed_centroids,
                                const std::vector<int32_t>& ed_counts, int k, int l, double r,
                                Rng* rng) {
  std::vector<Vec> coreset;
  if (candidates.empty()) return coreset;
  const int n = static_cast<int>(candidates.size());
  const int edk = static_cast<int>(ed_counts.size());

  while (static_cast<int>(coreset.size()) < k) {
    const int draw = std::max(1, static_cast<int>(static_cast<double>(n) * r));
    std::vector<int> pool(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) pool[static_cast<size_t>(i)] = i;
    std::vector<int> sampled;
    sampled.reserve(static_cast<size_t>(draw));
    for (int i = 0; i < draw && !pool.empty(); ++i) {
      const int pos = rng->next_int(static_cast<int>(pool.size()));
      sampled.push_back(pool[static_cast<size_t>(pos)]);
      pool.erase(pool.begin() + pos);
    }

    double best_gain = -std::numeric_limits<double>::max();
    int best_index = -1;
    for (int candidate : sampled) {
      const double* probe = candidates[static_cast<size_t>(candidate)].data();
      double gain = 0.0;
      for (int c = 0; c < edk; ++c) {
        if (ed_counts[static_cast<size_t>(c)] == 0) continue;
        const double* centre = ed_centroids.data() + static_cast<size_t>(c) * l;
        double best_corr = max_correlation(probe, centre, l);
        for (const Vec& chosen : coreset) {
          best_corr = std::max(best_corr, max_correlation(probe, chosen.data(), l));
        }
        gain += best_corr * static_cast<double>(ed_counts[static_cast<size_t>(c)]);
      }
      if (gain > best_gain) {
        best_gain = gain;
        best_index = candidate;
      }
    }
    if (best_index < 0) break;
    coreset.push_back(candidates[static_cast<size_t>(best_index)]);
  }
  return coreset;
}

QueryResult merge_units(const std::vector<PageMeta>& units, const Config& config, bool medoid,
                        const SampledSource& sampler) {
  if (!medoid) return kshape_centers(units, config);
  std::vector<Vec> candidates;
  if (!sampler || !sampler(std::max(config.k * 32, 128), &candidates) || candidates.empty()) {
    return QueryResult();
  }
  return medoid_centers(units, candidates, config);
}

}  // namespace

QueryResult kshape_centers(const std::vector<PageMeta>& units, const Config& config) {
  QueryResult result;
  if (units.empty()) return result;
  const PageMeta merged = build_chunk_meta(units, config);
  result.centers.resize(static_cast<size_t>(config.k));
  for (int c = 0; c < config.k; ++c) {
    Mat sum(config.l, config.l);
    for (int r = 0; r < config.l; ++r) {
      for (int q = 0; q < config.l; ++q) sum.at(r, q) = merged.sum_matrix(c, r, q);
    }
    Vec reference(static_cast<size_t>(config.l));
    for (int r = 0; r < config.l; ++r) reference[static_cast<size_t>(r)] = merged.centroid(c, r);
    result.centers[static_cast<size_t>(c)] = extract_shape(sum, reference);
  }
  return result;
}

QueryResult medoid_centers(const std::vector<PageMeta>& units, const std::vector<Vec>& candidates,
                           const Config& config) {
  QueryResult result;
  if (units.empty() || candidates.empty()) return result;
  const PageMeta merged = build_chunk_meta(units, config);

  std::vector<Vec> normalized;
  normalized.reserve(candidates.size());
  for (const Vec& seq : candidates) {
    if (static_cast<int>(seq.size()) != config.l) continue;
    normalized.push_back(zscore(seq));
  }
  if (normalized.empty()) return result;

  Rng rng(config.seed);
  result.centers = greedy_coreset(normalized, merged.ed_centroids, merged.ed_counts, config.k,
                                  config.l, config.r, &rng);
  return result;
}

std::vector<Vec> grid_subsequences(const Points& points, int l) {
  std::vector<Vec> out;
  Vec current;
  current.reserve(static_cast<size_t>(l));
  for (size_t i = 0; i < points.ts.size(); ++i) {
    current.push_back(points.values[i]);
    if (on_grid_boundary(points.ts[i], l)) {
      if (static_cast<int>(current.size()) == l) out.push_back(current);
      current.clear();
    }
  }
  return out;
}

QueryResult kshape_raw(const Points& points, const Config& config) {
  QueryResult result;
  const std::vector<Vec> sequences = grid_subsequences(points, config.l);
  if (sequences.empty()) return result;
  Rng rng(config.seed);
  const KShapeResult fit = kshape_fit(sequences, config.k, config.iter_raw, rng);
  result.centers.resize(static_cast<size_t>(config.k));
  for (int c = 0; c < config.k; ++c) {
    result.centers[static_cast<size_t>(c)].assign(fit.centroids.row(c), fit.centroids.row(c) + config.l);
  }
  return result;
}

QueryResult mshape_raw(const Points& points, const Config& config) {
  QueryResult result;
  const std::vector<Vec> sequences = grid_subsequences(points, config.l);
  if (sequences.empty()) return result;

  std::vector<Vec> normalized;
  normalized.reserve(sequences.size());
  for (const Vec& seq : sequences) normalized.push_back(zscore(seq));

  Rng rng(config.seed);
  const KMeansResult approx = kmeans(config.edk_effective(), config.iter_raw, normalized, rng);
  std::vector<double> ed_centroids(static_cast<size_t>(approx.centroids.rows) * config.l);
  for (int c = 0; c < approx.centroids.rows; ++c) {
    for (int r = 0; r < config.l; ++r) ed_centroids[static_cast<size_t>(c) * config.l + r] = approx.centroids.at(c, r);
  }
  std::vector<int32_t> ed_counts(static_cast<size_t>(config.edk_effective()), 0);
  for (int label : approx.idx) {
    if (label >= 0 && label < static_cast<int>(ed_counts.size())) ++ed_counts[static_cast<size_t>(label)];
  }
  result.centers = greedy_coreset(normalized, ed_centroids, ed_counts, config.k, config.l, config.r, &rng);
  return result;
}

PageMeta correct_page(const PageMeta& meta, const Points& raw, int64_t t0, int64_t t1,
                      const Config& config, QueryStats* stats) {
  const int l = config.l;
  if (raw.ts.empty()) return meta;
  if (stats != nullptr) stats->raw_points_read += static_cast<int64_t>(raw.ts.size());

  const int64_t new_start = std::max(raw.ts.front(), t0);
  const int64_t new_end = std::min(raw.ts.back(), t1);
  if (new_start > new_end) return meta;

  // Collect the grid aligned subsequences that lie entirely outside the range.
  std::vector<Vec> removed;
  for (int pass = 0; pass < 2; ++pass) {
    const bool before = pass == 0;
    Vec run;
    for (size_t i = 0; i < raw.ts.size(); ++i) {
      const bool outside = before ? raw.ts[i] < new_start : raw.ts[i] > new_end;
      if (!outside) continue;
      run.push_back(raw.values[i]);
      if (on_grid_boundary(raw.ts[i], l)) {
        if (static_cast<int>(run.size()) == l) removed.push_back(zscore(run));
        run.clear();
      }
    }
  }

  std::vector<double> k_sum = meta.sum_matrices;
  std::vector<double> ed_centroids = meta.ed_centroids;
  std::vector<int32_t> ed_counts = meta.ed_counts;

  for (const Vec& seq : removed) {
    const int cluster = nearest_by_l2(seq.data(), meta.centroids, meta.k, l);
    if (cluster >= 0) subtract_outer(&k_sum, seq, l);
    const int ed_cluster = nearest_by_l2(seq.data(), ed_centroids, meta.edk, l);
    if (ed_cluster >= 0) remove_from_mean(&ed_centroids, &ed_counts, ed_cluster, seq, l);
  }

  PageMeta corrected = meta;
  corrected.sum_matrices = std::move(k_sum);
  corrected.ed_centroids = std::move(ed_centroids);
  corrected.ed_counts = std::move(ed_counts);
  corrected.n_seq = static_cast<int32_t>(
      std::max<int64_t>(0, static_cast<int64_t>(meta.n_seq) - static_cast<int64_t>(removed.size())));
  corrected.idx.assign(static_cast<size_t>(corrected.n_seq), -1);

  // Rebuild centroids from the corrected matrices so the stored centroid and
  // its matrix stay consistent.
  for (int c = 0; c < meta.k; ++c) {
    Mat sum(l, l);
    for (int r = 0; r < l; ++r) {
      for (int q = 0; q < l; ++q) sum.at(r, q) = corrected.sum_matrix(c, r, q);
    }
    Vec reference(static_cast<size_t>(l));
    for (int r = 0; r < l; ++r) reference[static_cast<size_t>(r)] = meta.centroid(c, r);
    const Vec shape = extract_shape(sum, reference);
    for (int r = 0; r < l; ++r) corrected.set_centroid(c, r, shape[static_cast<size_t>(r)]);
  }

  // Refresh the trailing partial subsequence.
  Vec tail(static_cast<size_t>(l), 0.0);
  const int64_t tail_start = floor_div(new_end, l) * l;
  int tail_size = 0;
  for (size_t i = 0; i < raw.ts.size(); ++i) {
    if (raw.ts[i] >= tail_start && raw.ts[i] <= new_end) ++tail_size;
  }
  if (tail_size > 0 && tail_size < l) {
    int position = l - tail_size;
    for (size_t i = 0; i < raw.ts.size(); ++i) {
      if (raw.ts[i] >= tail_start && raw.ts[i] <= new_end) tail[static_cast<size_t>(position++)] = raw.values[i];
    }
  }
  corrected.tail_extra = tail;
  corrected.t_start = ceil_div(new_start, l) * l;
  corrected.t_end = floor_div(new_end, l) * l;
  corrected.count = new_end - new_start + 1;
  return corrected;
}

namespace {

QueryResult run_query(const std::vector<ChunkRef>& chunks, const std::vector<PageRef>& pages,
                      int64_t t0, int64_t t1, const Config& config, Level level,
                      const RawPageLoader& loader, bool medoid, const SampledSource& sampler) {
  std::vector<PageMeta> units;
  QueryStats stats;

  auto consider_page = [&](const PageRef& page) {
    ++stats.pages_considered;
    const double ratio = overlap_ratio(page.meta, t0, t1);
    if (ratio <= 0.0) return;
    if (ratio >= config.theta_page) {
      units.push_back(page.meta);
      ++stats.pages_meta_used;
      return;
    }
    if (!loader) throw AlgorithmError("partial page needs a raw loader");
    Points raw;
    if (!loader(page.page_no, &raw)) throw AlgorithmError("cannot load raw page " + std::to_string(page.page_no));
    units.push_back(correct_page(page.meta, raw, t0, t1, config, &stats));
    ++stats.pages_corrected;
  };

  if (level == Level::Chunk && !chunks.empty()) {
    for (const ChunkRef& chunk : chunks) {
      const double ratio = overlap_ratio(chunk.meta, t0, t1);
      if (ratio <= 0.0) continue;
      if (ratio >= config.theta_chunk) {
        units.push_back(chunk.meta);
        ++stats.chunks_meta_used;
        continue;
      }
      ++stats.chunks_descended;
      for (const PageRef& page : pages) {
        if (page.page_no < chunk.page_from || page.page_no > chunk.page_to) continue;
        consider_page(page);
      }
    }
  } else {
    for (const PageRef& page : pages) consider_page(page);
  }

  QueryResult result;
  if (!units.empty()) {
    result = merge_units(units, config, medoid, sampler);
  }
  result.stats = stats;
  result.stats.units_merged = static_cast<int64_t>(units.size());
  return result;
}

}  // namespace

QueryResult kshape_query(const std::vector<ChunkRef>& chunks, const std::vector<PageRef>& pages,
                         int64_t t0, int64_t t1, const Config& config, Level level,
                         const RawPageLoader& loader) {
  return run_query(chunks, pages, t0, t1, config, level, loader, false, nullptr);
}

QueryResult mshape_query(const std::vector<ChunkRef>& chunks, const std::vector<PageRef>& pages,
                         int64_t t0, int64_t t1, const Config& config, Level level,
                         const RawPageLoader& loader, const SampledSource& sampler) {
  return run_query(chunks, pages, t0, t1, config, level, loader, true, sampler);
}

}  // namespace indb
