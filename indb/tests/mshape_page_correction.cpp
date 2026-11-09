// Copyright (c) 2026 The in-database time-series clustering authors.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "indb/core.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <utility>

int main() {
  indb::Config page_config;
  page_config.l = 16;
  page_config.k = 3;
  indb::Config chunk_config = page_config;
  chunk_config.chunk_path = true;
  chunk_config.medoid_page_correction = true;
  int checks = 0;
  for (const auto span : {std::pair{0, 129}, std::pair{130, 259}, std::pair{128, 135}}) {
    indb::Points raw;
    for (int time = span.first; time <= span.second; ++time) {
      raw.ts.push_back(time);
      raw.values.push_back(std::sin(time * 0.2) + 0.02 * std::cos(time));
    }
    const auto stored = indb::build_page_meta(raw, page_config, 24301);
    const auto original = indb::encode_meta(stored);
    for (const auto query : {std::pair{span.first + 1, span.second - 1},
                             std::pair{span.first, span.second - 2},
                             std::pair{span.first + 2, span.second}}) {
      indb::QueryStats expected_stats, actual_stats;
      const auto expected = indb::correct_page(stored, raw, query.first, query.second,
                                                page_config, &expected_stats);
      const auto actual = indb::correct_page(stored, raw, query.first, query.second,
                                              chunk_config, &actual_stats);
      assert(expected.ed_counts == actual.ed_counts);
      assert(expected.ed_centroids == actual.ed_centroids);
      assert(expected.sum_matrices == actual.sum_matrices);
      assert(expected.n_seq == actual.n_seq);
      assert(expected.count == actual.count);
      assert(actual.t_start == query.first && actual.t_end == query.second);
      assert(actual.grid_fragments);
      assert(expected_stats.raw_points_read == actual_stats.raw_points_read);
      assert(indb::encode_meta(stored) == original);
      const auto decoded = indb::decode_meta(indb::encode_meta(actual));
      assert(decoded.grid_fragments && decoded.ed_counts == actual.ed_counts);
      for (size_t i = 0; i < raw.ts.size(); ++i) {
        if (raw.ts[i] < query.first || raw.ts[i] > query.second) continue;
        const int slot = static_cast<int>(raw.ts[i] % page_config.l);
        if (raw.ts[i] < ((query.first + 15) / 16) * 16)
          assert(actual.head_extra[slot] == raw.values[i]);
        if (raw.ts[i] >= ((query.second + 1) / 16) * 16)
          assert(actual.tail_extra[slot] == raw.values[i]);
      }
      indb::Config k_config = chunk_config;
      k_config.medoid_page_correction = false;
      const auto actual_k = indb::correct_page(stored, raw, query.first, query.second, k_config, nullptr);
      assert(expected.sum_matrices == actual_k.sum_matrices);
      assert(expected.centroids == actual_k.centroids);
      assert(expected.n_seq == actual_k.n_seq);
      assert(actual_k.idx.size() == static_cast<size_t>(expected.n_seq));
      for (int label : actual_k.idx) assert(label >= 0 && label < page_config.k);
      assert(actual_k.grid_fragments);
      assert(actual_k.t_start == query.first && actual_k.t_end == query.second);
      assert(indb::encode_meta(stored) == original);
      const auto decoded_k = indb::decode_meta(indb::encode_meta(actual_k));
      assert(decoded_k.idx == actual_k.idx);
      const auto centers = indb::kshape_centers({actual_k}, k_config);
      assert(centers.stats.represented_subsequences == expected.n_seq);
      assert(centers.centers.size() == static_cast<size_t>(page_config.k));
      for (const auto& center : centers.centers) assert(indb::all_finite(center));
      // An unusable membership payload must retain the raw recovery route.
      auto unusable = stored;
      unusable.idx.assign(unusable.idx.size(), -1);
      indb::Points restricted;
      for (size_t i = 0; i < raw.ts.size(); ++i) {
        if (raw.ts[i] >= query.first && raw.ts[i] <= query.second) {
          restricted.ts.push_back(raw.ts[i]);
          restricted.values.push_back(raw.values[i]);
        }
      }
      if (!unusable.idx.empty()) {
        const auto recovered = indb::correct_page(unusable, raw, query.first, query.second, k_config, nullptr);
        const auto refit = indb::build_page_meta(restricted, k_config,
            k_config.seed ^ static_cast<uint64_t>(stored.t_start));
        assert(indb::encode_meta(recovered) == indb::encode_meta(refit));
      }
      ++checks;
    }
  }
  std::cout << "PASS " << checks << " shared Page-K/Page-M correction and retained Chunk-K weights\n";
}
