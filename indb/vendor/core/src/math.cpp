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

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinDenominator = 1e-9;
// Java's Double.MIN_VALUE is the smallest positive subnormal, and the
// reference uses it as the initial maximum in two places.
constexpr double kJavaDoubleMin = 4.9406564584124654e-324;

Mat mat_mul(const Mat& a, const Mat& b) {
  if (a.cols != b.rows) throw AlgorithmError("matrix dimension mismatch");
  Mat out(a.rows, b.cols);
  for (int i = 0; i < a.rows; ++i) {
    for (int j = 0; j < b.cols; ++j) {
      double sum = 0.0;
      for (int t = 0; t < a.cols; ++t) sum += a.at(i, t) * b.at(t, j);
      out.at(i, j) = sum;
    }
  }
  return out;
}

Mat shape_matrix(const Mat& sum_matrix) {
  const int l = sum_matrix.rows;
  if (l != sum_matrix.cols) throw AlgorithmError("sum matrix must be square");
  if (l < 2) throw AlgorithmError("subsequence length must be at least 2");

  Mat p(l, l);
  const double fill = 1.0 / static_cast<double>(l - 1);
  for (int i = 0; i < l; ++i) {
    for (int j = 0; j < l; ++j) p.at(i, j) = (i == j ? 1.0 : 0.0) - fill;
  }
  return mat_mul(mat_mul(p, sum_matrix), p);
}

double max_of(const Vec& v) {
  double best = kJavaDoubleMin;
  for (double x : v) {
    if (x > best) best = x;
  }
  return best;
}

}  // namespace

int Rng::next_int(int bound) {
  if (bound <= 0) throw AlgorithmError("next_int bound must be positive");
  std::uniform_int_distribution<int> dist(0, bound - 1);
  return dist(gen_);
}

double Rng::next_double() {
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  return dist(gen_);
}

bool all_finite(const Vec& x) {
  for (double v : x) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}

Vec zscore(const double* x, int l) {
  if (l < 2) throw AlgorithmError("zscore needs l >= 2");
  double sum = 0.0;
  for (int i = 0; i < l; ++i) sum += x[i];
  const double mean = sum / l;

  sum = 0.0;
  for (int i = 0; i < l; ++i) {
    const double d = x[i] - mean;
    sum += d * d;
  }
  const double std_dev = std::sqrt(sum / (l - 1));

  Vec out(static_cast<size_t>(l));
  for (int i = 0; i < l; ++i) out[static_cast<size_t>(i)] = (x[i] - mean) / std_dev;
  return out;
}

int fft_size_for(int l) {
  if (l < 2) throw AlgorithmError("fft_size_for needs l >= 2");
  const int v = 2 * l - 1;
  int bits = 0;
  while (bits < 31 && (1LL << bits) <= static_cast<long long>(v)) ++bits;
  return 1 << bits;
}

void fft(const double* re, const double* im, int n, double* out_re, double* out_im) {
  if (n <= 0 || (n & (n - 1)) != 0) throw AlgorithmError("fft size must be a power of two");
  for (int i = 0; i < n; ++i) {
    out_re[i] = re[i];
    out_im[i] = im[i];
  }
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; (j & bit) != 0; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(out_re[i], out_re[j]);
      std::swap(out_im[i], out_im[j]);
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double angle = -2.0 * kPi / len;
    const double wlen_re = std::cos(angle);
    const double wlen_im = std::sin(angle);
    for (int i = 0; i < n; i += len) {
      double w_re = 1.0;
      double w_im = 0.0;
      for (int k = 0; k < len / 2; ++k) {
        const int u = i + k;
        const int v = i + k + len / 2;
        const double vr = out_re[v] * w_re - out_im[v] * w_im;
        const double vi = out_re[v] * w_im + out_im[v] * w_re;
        out_re[v] = out_re[u] - vr;
        out_im[v] = out_im[u] - vi;
        out_re[u] += vr;
        out_im[u] += vi;
        const double next_re = w_re * wlen_re - w_im * wlen_im;
        const double next_im = w_re * wlen_im + w_im * wlen_re;
        w_re = next_re;
        w_im = next_im;
      }
    }
  }
}

void ifft(const double* re, const double* im, int n, double* out_re, double* out_im) {
  std::vector<double> cr(static_cast<size_t>(n));
  std::vector<double> ci(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    cr[static_cast<size_t>(i)] = re[i];
    ci[static_cast<size_t>(i)] = -im[i];
  }
  fft(cr.data(), ci.data(), n, out_re, out_im);
  const double scale = 1.0 / n;
  for (int i = 0; i < n; ++i) {
    out_re[i] *= scale;
    out_im[i] = -out_im[i] * scale;
  }
}

Vec ncc(const double* x, const double* y, int l) {
  const int fft_size = fft_size_for(l);
  const double norms = norm2(x, l) * norm2(y, l);
  const double den = norms < kMinDenominator ? std::numeric_limits<double>::max() : norms;

  std::vector<double> xr(static_cast<size_t>(fft_size), 0.0);
  std::vector<double> xi(static_cast<size_t>(fft_size), 0.0);
  std::vector<double> yr(static_cast<size_t>(fft_size), 0.0);
  std::vector<double> yi(static_cast<size_t>(fft_size), 0.0);
  for (int i = 0; i < l; ++i) {
    xr[static_cast<size_t>(i)] = x[i];
    yr[static_cast<size_t>(i)] = y[i];
  }
  std::vector<double> fxr(static_cast<size_t>(fft_size));
  std::vector<double> fxi(static_cast<size_t>(fft_size));
  std::vector<double> fyr(static_cast<size_t>(fft_size));
  std::vector<double> fyi(static_cast<size_t>(fft_size));
  fft(xr.data(), xi.data(), fft_size, fxr.data(), fxi.data());
  fft(yr.data(), yi.data(), fft_size, fyr.data(), fyi.data());

  std::vector<double> pr(static_cast<size_t>(fft_size));
  std::vector<double> pi(static_cast<size_t>(fft_size));
  for (int i = 0; i < fft_size; ++i) {
    pr[static_cast<size_t>(i)] = fxr[static_cast<size_t>(i)] * fyr[static_cast<size_t>(i)] +
                                 fxi[static_cast<size_t>(i)] * fyi[static_cast<size_t>(i)];
    pi[static_cast<size_t>(i)] = fxi[static_cast<size_t>(i)] * fyr[static_cast<size_t>(i)] -
                                 fxr[static_cast<size_t>(i)] * fyi[static_cast<size_t>(i)];
  }
  std::vector<double> ccr(static_cast<size_t>(fft_size));
  std::vector<double> cci(static_cast<size_t>(fft_size));
  ifft(pr.data(), pi.data(), fft_size, ccr.data(), cci.data());

  Vec out(static_cast<size_t>(fft_size - 1));
  for (int i = 0; i < fft_size - 1; ++i) {
    const double value = i < l - 1 ? ccr[static_cast<size_t>(fft_size - l + 1 + i)]
                                   : ccr[static_cast<size_t>(i - l + 1)];
    out[static_cast<size_t>(i)] = value / den;
  }
  return out;
}

double sbd(const double* x, const double* y, int l) {
  const Vec corr = ncc(x, y, l);
  return 1.0 - max_of(corr) / norm2(x, l) / norm2(y, l);
}

double sbd_assignment(const double* x, const double* y, int l) {
  const Vec corr = ncc(x, y, l);
  return 1.0 - max_of(corr);
}

double norm2(const double* x, int l) {
  double res = 0.0;
  for (int i = 0; i < l; ++i) res += x[i] * x[i];
  return std::sqrt(res);
}

double l2_squared(const double* x, const double* y, int l) {
  double res = 0.0;
  for (int i = 0; i < l; ++i) {
    const double d = x[i] - y[i];
    res += d * d;
  }
  return res;
}

EigenResult eigen_symmetric(const Mat& m) {
  if (m.rows != m.cols) throw AlgorithmError("eigen requires a square matrix");
  const int n = m.rows;
  Mat a = m;
  Mat v(n, n);
  for (int i = 0; i < n; ++i) v.at(i, i) = 1.0;

  for (int sweep = 0; sweep < 100; ++sweep) {
    double off = 0.0;
    double diag = 0.0;
    for (int i = 0; i < n; ++i) {
      diag += a.at(i, i) * a.at(i, i);
      for (int j = i + 1; j < n; ++j) off += a.at(i, j) * a.at(i, j);
    }
    if (off <= 1e-30 * (diag + 1e-30)) break;

    for (int p = 0; p < n; ++p) {
      for (int q = p + 1; q < n; ++q) {
        const double apq = a.at(p, q);
        if (std::fabs(apq) < 1e-300) continue;
        const double theta = (a.at(q, q) - a.at(p, p)) / (2.0 * apq);
        const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                         (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        for (int k = 0; k < n; ++k) {
          const double akp = a.at(k, p);
          const double akq = a.at(k, q);
          a.at(k, p) = c * akp - s * akq;
          a.at(k, q) = s * akp + c * akq;
        }
        for (int k = 0; k < n; ++k) {
          const double apk = a.at(p, k);
          const double aqk = a.at(q, k);
          a.at(p, k) = c * apk - s * aqk;
          a.at(q, k) = s * apk + c * aqk;
        }
        for (int k = 0; k < n; ++k) {
          const double vkp = v.at(k, p);
          const double vkq = v.at(k, q);
          v.at(k, p) = c * vkp - s * vkq;
          v.at(k, q) = s * vkp + c * vkq;
        }
      }
    }
  }

  std::vector<int> order(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) order[static_cast<size_t>(i)] = i;
  std::sort(order.begin(), order.end(), [&](int lhs, int rhs) {
    if (a.at(lhs, lhs) != a.at(rhs, rhs)) return a.at(lhs, lhs) < a.at(rhs, rhs);
    return lhs < rhs;
  });

  EigenResult result;
  result.values.resize(static_cast<size_t>(n));
  result.vectors = Mat(n, n);
  for (int i = 0; i < n; ++i) {
    const int src = order[static_cast<size_t>(i)];
    result.values[static_cast<size_t>(i)] = a.at(src, src);
    int pivot = 0;
    for (int k = 1; k < n; ++k) {
      if (std::fabs(v.at(k, src)) > std::fabs(v.at(pivot, src))) pivot = k;
    }
    const double sign = v.at(pivot, src) < 0.0 ? -1.0 : 1.0;
    for (int k = 0; k < n; ++k) result.vectors.at(k, i) = v.at(k, src) * sign;
  }
  const double scale = std::fabs(result.values.back()) + 1.0;
  for (int i = 0; i + 1 < n; ++i) {
    if (std::fabs(result.values[static_cast<size_t>(i)] -
                 result.values[static_cast<size_t>(i + 1)]) <= 1e-12 * scale) {
      result.ties_detected = true;
      break;
    }
  }
  return result;
}

Vec extract_shape(const Mat& sum_matrix, const Vec& reference) {
  const EigenResult eig = eigen_symmetric(shape_matrix(sum_matrix));
  const int l = static_cast<int>(eig.values.size());
  Vec candidate(static_cast<size_t>(l));
  for (int i = 0; i < l; ++i) candidate[static_cast<size_t>(i)] = eig.vectors.at(i, l - 1);

  double towards = 0.0;
  double away = 0.0;
  for (int i = 0; i < l; ++i) {
    const double plus = reference[static_cast<size_t>(i)] - candidate[static_cast<size_t>(i)];
    const double minus = reference[static_cast<size_t>(i)] + candidate[static_cast<size_t>(i)];
    towards += plus * plus;
    away += minus * minus;
  }
  if (away < towards) {
    for (int i = 0; i < l; ++i) candidate[static_cast<size_t>(i)] = -candidate[static_cast<size_t>(i)];
  }
  return zscore(candidate);
}

Vec extract_shape_population(const Mat& sum_matrix, const std::vector<Vec>& population) {
  const EigenResult eig = eigen_symmetric(shape_matrix(sum_matrix));
  const int l = static_cast<int>(eig.values.size());
  Vec candidate(static_cast<size_t>(l));
  for (int i = 0; i < l; ++i) candidate[static_cast<size_t>(i)] = eig.vectors.at(i, l - 1);

  double towards = 0.0;
  double away = 0.0;
  for (const Vec& x : population) {
    for (int i = 0; i < l; ++i) {
      const double plus = x[static_cast<size_t>(i)] - candidate[static_cast<size_t>(i)];
      const double minus = x[static_cast<size_t>(i)] + candidate[static_cast<size_t>(i)];
      towards += plus * plus;
      away += minus * minus;
    }
  }
  if (away < towards) {
    for (int i = 0; i < l; ++i) candidate[static_cast<size_t>(i)] = -candidate[static_cast<size_t>(i)];
  }
  return zscore(candidate);
}

KMeansResult kmeans(int k, int iterations, const std::vector<Vec>& points, Rng& rng) {
  if (k <= 0) throw AlgorithmError("kmeans needs k > 0");
  if (points.empty()) throw AlgorithmError("kmeans needs points");
  const int n = static_cast<int>(points.size());
  const int d = static_cast<int>(points[0].size());

  KMeansResult result;
  result.centroids = Mat(k, d);
  result.idx.assign(static_cast<size_t>(n), 0);
  result.deltas.assign(static_cast<size_t>(k), 0.0);

  if (n < k) {
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j < d; ++j) result.centroids.at(i, j) = points[static_cast<size_t>(i)][static_cast<size_t>(j)];
      result.idx[static_cast<size_t>(i)] = i;
    }
    return result;
  }

  std::vector<double> dist(static_cast<size_t>(n), 0.0);
  std::vector<double> weighted(static_cast<size_t>(n), 0.0);
  for (int c = 0; c < k; ++c) {
    int choose = 0;
    if (c == 0) {
      choose = rng.next_int(n);
    } else {
      const double* prev = result.centroids.row(c - 1);
      for (int p = 0; p < n; ++p) {
        const double temp = l2_squared(points[static_cast<size_t>(p)].data(), prev, d);
        if (c == 1) {
          dist[static_cast<size_t>(p)] = temp;
        } else if (temp < dist[static_cast<size_t>(p)]) {
          dist[static_cast<size_t>(p)] = temp;
        }
        weighted[static_cast<size_t>(p)] =
            p == 0 ? dist[0] : weighted[static_cast<size_t>(p - 1)] + dist[static_cast<size_t>(p)];
      }
      const double total = weighted[static_cast<size_t>(n - 1)];
      const double rnd = rng.next_double();
      choose = 0;
      for (int j = n - 1; j > 0; --j) {
        if (total != 0.0 && rnd > weighted[static_cast<size_t>(j - 1)] / total) {
          choose = j;
          break;
        }
        choose = 0;
      }
    }
    for (int j = 0; j < d; ++j) result.centroids.at(c, j) = points[static_cast<size_t>(choose)][static_cast<size_t>(j)];
  }

  std::vector<std::vector<double>> sums(static_cast<size_t>(k), std::vector<double>(static_cast<size_t>(d)));
  std::vector<int> counts(static_cast<size_t>(k), 0);
  for (int iter = 0; iter < iterations; ++iter) {
    for (int i = 0; i < n; ++i) {
      int best = 0;
      double best_distance = std::numeric_limits<double>::max();
      for (int j = 0; j < k; ++j) {
        const double value = l2_squared(points[static_cast<size_t>(i)].data(), result.centroids.row(j), d);
        if (value < best_distance) {
          best_distance = value;
          best = j;
        }
      }
      result.idx[static_cast<size_t>(i)] = best;
    }
    for (int j = 0; j < k; ++j) {
      std::fill(sums[static_cast<size_t>(j)].begin(), sums[static_cast<size_t>(j)].end(), 0.0);
      counts[static_cast<size_t>(j)] = 0;
    }
    for (int i = 0; i < n; ++i) {
      const int label = result.idx[static_cast<size_t>(i)];
      ++counts[static_cast<size_t>(label)];
      for (int j = 0; j < d; ++j) {
        sums[static_cast<size_t>(label)][static_cast<size_t>(j)] += points[static_cast<size_t>(i)][static_cast<size_t>(j)];
      }
    }
    for (int j = 0; j < k; ++j) {
      if (counts[static_cast<size_t>(j)] == 0) continue;
      for (int t = 0; t < d; ++t) {
        result.centroids.at(j, t) = sums[static_cast<size_t>(j)][static_cast<size_t>(t)] / counts[static_cast<size_t>(j)];
      }
    }
    for (int j = 0; j < k; ++j) {
      if (counts[static_cast<size_t>(j)] != 0) continue;
      const int pick = rng.next_int(n);
      for (int t = 0; t < d; ++t) result.centroids.at(j, t) = points[static_cast<size_t>(pick)][static_cast<size_t>(t)];
      counts[static_cast<size_t>(j)] = 1;
      result.empty_cluster_filled = true;
    }
  }

  std::vector<double> acc(static_cast<size_t>(k), 0.0);
  std::vector<int> cnt(static_cast<size_t>(k), 0);
  for (int i = 0; i < n; ++i) {
    const int label = result.idx[static_cast<size_t>(i)];
    acc[static_cast<size_t>(label)] +=
        l2_squared(points[static_cast<size_t>(i)].data(), result.centroids.row(label), d);
    ++cnt[static_cast<size_t>(label)];
  }
  for (int j = 0; j < k; ++j) {
    result.deltas[static_cast<size_t>(j)] =
        cnt[static_cast<size_t>(j)] == 0 ? -1.0 : acc[static_cast<size_t>(j)] / cnt[static_cast<size_t>(j)];
  }
  return result;
}

KShapeResult kshape_fit(const std::vector<Vec>& x, int k, int max_iter, Rng& rng) {
  if (k <= 0) throw AlgorithmError("kshape needs k > 0");
  if (x.empty()) throw AlgorithmError("kshape needs subsequences");
  const int n = static_cast<int>(x.size());
  const int l = static_cast<int>(x[0].size());
  if (l < 2) throw AlgorithmError("kshape needs l >= 2");

  KShapeResult result;
  result.centroids = Mat(k, l);
  result.sum_matrices.assign(static_cast<size_t>(k), Mat(l, l));
  result.deltas.assign(static_cast<size_t>(k), 0.0);
  result.idx.assign(static_cast<size_t>(n), 0);

  if (n < k) {
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j < l; ++j) result.centroids.at(i, j) = x[static_cast<size_t>(i)][static_cast<size_t>(j)];
      result.idx[static_cast<size_t>(i)] = i;
      result.deltas[static_cast<size_t>(i)] = std::numeric_limits<double>::quiet_NaN();
      for (int p = 0; p < l; ++p) {
        for (int q = 0; q < l; ++q) {
          result.sum_matrices[static_cast<size_t>(i)].at(p, q) =
              x[static_cast<size_t>(i)][static_cast<size_t>(p)] * x[static_cast<size_t>(i)][static_cast<size_t>(q)];
        }
      }
    }
    return result;
  }

  // The reference draws labels from [0, k], so label k exists during the first
  // round even though it has no members. Preserved for parity.
  std::vector<int> idx(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) idx[static_cast<size_t>(i)] = rng.next_int(k + 1);

  std::vector<std::vector<int>> members(static_cast<size_t>(k));
  std::vector<Vec> centroid(k, Vec(static_cast<size_t>(l), 0.0));

  for (int iter = 0; iter < max_iter; ++iter) {
    const std::vector<int> old_idx = idx;
    ++result.iterations_run;

    for (int j = 0; j < k; ++j) members[static_cast<size_t>(j)].clear();
    for (int i = 0; i < n; ++i) {
      const int label = idx[static_cast<size_t>(i)];
      if (label >= 0 && label < k) members[static_cast<size_t>(label)].push_back(i);
    }

    for (int j = 0; j < k; ++j) {
      if (members[static_cast<size_t>(j)].empty()) {
        const int pick = rng.next_int(n);
        centroid[static_cast<size_t>(j)] = x[static_cast<size_t>(pick)];
        ++result.empty_clusters;
        continue;
      }
      std::vector<Vec> rows;
      rows.reserve(members[static_cast<size_t>(j)].size());
      for (int i : members[static_cast<size_t>(j)]) rows.push_back(zscore(x[static_cast<size_t>(i)]));

      Mat s(l, l);
      for (const Vec& row : rows) {
        for (int p = 0; p < l; ++p) {
          for (int q = 0; q < l; ++q) s.at(p, q) += row[static_cast<size_t>(p)] * row[static_cast<size_t>(q)];
        }
      }
      result.sum_matrices[static_cast<size_t>(j)] = s;
      centroid[static_cast<size_t>(j)] = extract_shape_population(s, x);
    }

    for (int i = 0; i < n; ++i) {
      int best = 0;
      double best_distance = std::numeric_limits<double>::max();
      for (int j = 0; j < k; ++j) {
        const double value =
            sbd_assignment(x[static_cast<size_t>(i)].data(), centroid[static_cast<size_t>(j)].data(), l);
        if (value < best_distance) {
          best_distance = value;
          best = j;
        }
      }
      idx[static_cast<size_t>(i)] = best;
    }
    if (idx == old_idx) break;
  }

  result.idx = idx;
  for (int j = 0; j < k; ++j) {
    for (int t = 0; t < l; ++t) result.centroids.at(j, t) = centroid[static_cast<size_t>(j)][static_cast<size_t>(t)];
  }

  Vec acc(static_cast<size_t>(k), 0.0);
  std::vector<int> cnt(static_cast<size_t>(k), 0);
  for (int i = 0; i < n; ++i) {
    const int label = idx[static_cast<size_t>(i)];
    acc[static_cast<size_t>(label)] +=
        sbd(x[static_cast<size_t>(i)].data(), centroid[static_cast<size_t>(label)].data(), l);
    ++cnt[static_cast<size_t>(label)];
  }
  for (int j = 0; j < k; ++j) {
    result.deltas[static_cast<size_t>(j)] =
        cnt[static_cast<size_t>(j)] == 0 ? -1.0 : acc[static_cast<size_t>(j)] / cnt[static_cast<size_t>(j)];
  }
  return result;
}

}  // namespace indb
