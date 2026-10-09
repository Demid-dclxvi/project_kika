#include "kika/linalg/sparse.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/parallel.hpp"

namespace kika::linalg {

namespace {

// Порог распараллеливания: меньше ~200 тыс. ненулевых — накладные расходы больше выигрыша.
constexpr std::int64_t kParallelNnz = 200000;

std::size_t row_grain(const CsrMatrix& a) {
  if (a.nnz() < kParallelNnz) return static_cast<std::size_t>(a.rows) + 1;
  return 1024;
}

}  // namespace

void CsrMatrix::multiply(std::span<const double> x, std::span<double> y) const {
  util::parallel_for(0, static_cast<std::size_t>(rows), row_grain(*this), [&](std::size_t lo, std::size_t hi) {
    for (auto i = static_cast<std::int64_t>(lo); i < static_cast<std::int64_t>(hi); ++i) {
      double s = 0.0;
      for (std::int64_t p = row_ptr[i]; p < row_ptr[i + 1]; ++p) s += val[p] * x[col[p]];
      y[i] = s;
    }
  });
}

std::vector<double> CsrMatrix::multiply(std::span<const double> x) const {
  std::vector<double> y(static_cast<std::size_t>(rows));
  multiply(x, y);
  return y;
}

std::vector<double> CsrMatrix::diagonal() const {
  std::vector<double> d(static_cast<std::size_t>(std::min(rows, cols)), 0.0);
  for (std::int64_t i = 0; i < static_cast<std::int64_t>(d.size()); ++i) {
    const auto b = col.begin() + row_ptr[i];
    const auto e = col.begin() + row_ptr[i + 1];
    const auto it = std::lower_bound(b, e, static_cast<std::int32_t>(i));
    if (it != e && *it == i) d[i] = val[it - col.begin()];
  }
  return d;
}

CsrMatrix from_triplets(std::int64_t rows, std::int64_t cols, std::vector<Triplet> t) {
  std::sort(t.begin(), t.end(), [](const Triplet& a, const Triplet& b) {
    return a.row != b.row ? a.row < b.row : a.col < b.col;
  });
  CsrMatrix m;
  m.rows = rows;
  m.cols = cols;
  m.row_ptr.assign(static_cast<std::size_t>(rows) + 1, 0);
  m.col.reserve(t.size());
  m.val.reserve(t.size());
  std::size_t k = 0;
  for (std::int64_t i = 0; i < rows; ++i) {
    while (k < t.size() && t[k].row == i) {
      const std::int32_t c = t[k].col;
      double s = 0.0;
      while (k < t.size() && t[k].row == i && t[k].col == c) s += t[k++].value;
      m.col.push_back(c);
      m.val.push_back(s);
    }
    m.row_ptr[i + 1] = static_cast<std::int64_t>(m.col.size());
  }
  return m;
}

CsrMatrix transpose(const CsrMatrix& a) {
  CsrMatrix t;
  t.rows = a.cols;
  t.cols = a.rows;
  t.row_ptr.assign(static_cast<std::size_t>(a.cols) + 1, 0);
  for (std::int64_t p = 0; p < a.nnz(); ++p) ++t.row_ptr[a.col[p] + 1];
  for (std::int64_t i = 0; i < a.cols; ++i) t.row_ptr[i + 1] += t.row_ptr[i];
  t.col.resize(static_cast<std::size_t>(a.nnz()));
  t.val.resize(static_cast<std::size_t>(a.nnz()));
  std::vector<std::int64_t> next(t.row_ptr.begin(), t.row_ptr.end() - 1);
  for (std::int64_t i = 0; i < a.rows; ++i)
    for (std::int64_t p = a.row_ptr[i]; p < a.row_ptr[i + 1]; ++p) {
      const std::int64_t q = next[a.col[p]]++;
      t.col[q] = static_cast<std::int32_t>(i);
      t.val[q] = a.val[p];
    }
  return t;
}

CsrMatrix multiply(const CsrMatrix& a, const CsrMatrix& b) {
  if (a.cols != b.rows) throw std::invalid_argument("multiply: несовместимые размеры");
  CsrMatrix c;
  c.rows = a.rows;
  c.cols = b.cols;
  c.row_ptr.assign(static_cast<std::size_t>(a.rows) + 1, 0);
  const auto n = static_cast<std::size_t>(a.rows);
  // строки делятся на куски; каждый кусок считается независимо в свои буферы,
  // затем куски склеиваются по порядку — результат не зависит от числа потоков
  const std::size_t threads = static_cast<std::size_t>(thread_count());
  const std::size_t n_chunks =
      (a.nnz() < kParallelNnz / 4 || threads <= 1 || n < 2) ? 1 : std::min(n, threads * 4);
  const std::size_t step = (n + n_chunks - 1) / std::max<std::size_t>(n_chunks, 1);
  std::vector<std::vector<std::int32_t>> ccol(n_chunks);
  std::vector<std::vector<double>> cval(n_chunks);
  util::run_tasks(n_chunks, [&](std::size_t chunk) {
    const std::size_t lo = chunk * step;
    const std::size_t hi = std::min(n, lo + step);
    std::vector<double> acc(static_cast<std::size_t>(b.cols), 0.0);
    std::vector<std::int64_t> mark(static_cast<std::size_t>(b.cols), -1);
    std::vector<std::int32_t> touched;
    auto& cc = ccol[chunk];
    auto& cv = cval[chunk];
    for (auto i = static_cast<std::int64_t>(lo); i < static_cast<std::int64_t>(hi); ++i) {
      touched.clear();
      for (std::int64_t p = a.row_ptr[i]; p < a.row_ptr[i + 1]; ++p) {
        const std::int32_t k = a.col[p];
        const double av = a.val[p];
        for (std::int64_t q = b.row_ptr[k]; q < b.row_ptr[k + 1]; ++q) {
          const std::int32_t j = b.col[q];
          if (mark[j] != i) {
            mark[j] = i;
            acc[j] = 0.0;
            touched.push_back(j);
          }
          acc[j] += av * b.val[q];
        }
      }
      std::sort(touched.begin(), touched.end());
      for (std::int32_t j : touched) {
        cc.push_back(j);
        cv.push_back(acc[j]);
      }
      c.row_ptr[i + 1] = static_cast<std::int64_t>(touched.size());
    }
  });
  for (std::size_t i = 0; i < n; ++i) c.row_ptr[i + 1] += c.row_ptr[i];
  c.col.reserve(static_cast<std::size_t>(c.row_ptr[n]));
  c.val.reserve(static_cast<std::size_t>(c.row_ptr[n]));
  for (std::size_t k = 0; k < n_chunks; ++k) {
    c.col.insert(c.col.end(), ccol[k].begin(), ccol[k].end());
    c.val.insert(c.val.end(), cval[k].begin(), cval[k].end());
    std::vector<std::int32_t>().swap(ccol[k]);
    std::vector<double>().swap(cval[k]);
  }
  return c;
}

CsrMatrix add(const CsrMatrix& a, const CsrMatrix& b, double alpha, double beta) {
  if (a.rows != b.rows || a.cols != b.cols) throw std::invalid_argument("add: разные размеры");
  CsrMatrix c;
  c.rows = a.rows;
  c.cols = a.cols;
  c.row_ptr.assign(static_cast<std::size_t>(a.rows) + 1, 0);
  c.col.reserve(static_cast<std::size_t>(a.nnz() + b.nnz()));
  c.val.reserve(static_cast<std::size_t>(a.nnz() + b.nnz()));
  for (std::int64_t i = 0; i < a.rows; ++i) {
    std::int64_t p = a.row_ptr[i], pe = a.row_ptr[i + 1];
    std::int64_t q = b.row_ptr[i], qe = b.row_ptr[i + 1];
    while (p < pe || q < qe) {
      if (q >= qe || (p < pe && a.col[p] < b.col[q])) {
        c.col.push_back(a.col[p]);
        c.val.push_back(alpha * a.val[p++]);
      } else if (p >= pe || b.col[q] < a.col[p]) {
        c.col.push_back(b.col[q]);
        c.val.push_back(beta * b.val[q++]);
      } else {
        c.col.push_back(a.col[p]);
        c.val.push_back(alpha * a.val[p++] + beta * b.val[q++]);
      }
    }
    c.row_ptr[i + 1] = static_cast<std::int64_t>(c.col.size());
  }
  return c;
}

void scale_rows(CsrMatrix& a, std::span<const double> s) {
  for (std::int64_t i = 0; i < a.rows; ++i)
    for (std::int64_t p = a.row_ptr[i]; p < a.row_ptr[i + 1]; ++p) a.val[p] *= s[i];
}

void eliminate_zeros(CsrMatrix& a) {
  std::int64_t w = 0;
  std::int64_t start = 0;
  for (std::int64_t i = 0; i < a.rows; ++i) {
    const std::int64_t end = a.row_ptr[i + 1];
    for (std::int64_t p = start; p < end; ++p)
      if (a.val[p] != 0.0) {
        a.col[w] = a.col[p];
        a.val[w] = a.val[p];
        ++w;
      }
    start = end;
    a.row_ptr[i + 1] = w;
  }
  a.col.resize(static_cast<std::size_t>(w));
  a.val.resize(static_cast<std::size_t>(w));
}

CsrMatrix submatrix(const CsrMatrix& a, std::span<const std::int64_t> keep) {
  std::vector<std::int32_t> map(static_cast<std::size_t>(a.cols), -1);
  for (std::size_t k = 0; k < keep.size(); ++k) map[keep[k]] = static_cast<std::int32_t>(k);
  CsrMatrix s;
  s.rows = static_cast<std::int64_t>(keep.size());
  s.cols = static_cast<std::int64_t>(keep.size());
  s.row_ptr.assign(keep.size() + 1, 0);
  for (std::size_t k = 0; k < keep.size(); ++k) {
    const std::int64_t i = keep[k];
    for (std::int64_t p = a.row_ptr[i]; p < a.row_ptr[i + 1]; ++p) {
      const std::int32_t j = map[a.col[p]];
      if (j >= 0) {
        s.col.push_back(j);
        s.val.push_back(a.val[p]);
      }
    }
    s.row_ptr[k + 1] = static_cast<std::int64_t>(s.col.size());
  }
  return s;
}

double dot(std::span<const double> x, std::span<const double> y) {
  // четыре независимые суммы: быстрее одной цепочки, результат детерминирован
  double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
  const std::size_t n = x.size();
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    s0 += x[i] * y[i];
    s1 += x[i + 1] * y[i + 1];
    s2 += x[i + 2] * y[i + 2];
    s3 += x[i + 3] * y[i + 3];
  }
  for (; i < n; ++i) s0 += x[i] * y[i];
  return (s0 + s1) + (s2 + s3);
}

double norm2(std::span<const double> x) { return std::sqrt(dot(x, x)); }

}  // namespace kika::linalg
