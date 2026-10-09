#pragma once
// Разреженная матрица в формате CSR и операции, нужные МКЭ и многосеточному решателю.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kika::linalg {

struct CsrMatrix {
  std::int64_t rows = 0;
  std::int64_t cols = 0;
  std::vector<std::int64_t> row_ptr;  // rows + 1
  std::vector<std::int32_t> col;      // номера столбцов, по возрастанию внутри строки
  std::vector<double> val;

  std::int64_t nnz() const { return row_ptr.empty() ? 0 : row_ptr.back(); }

  // y = A·x
  void multiply(std::span<const double> x, std::span<double> y) const;
  std::vector<double> multiply(std::span<const double> x) const;

  std::vector<double> diagonal() const;
};

struct Triplet {
  std::int32_t row;
  std::int32_t col;
  double value;
};

// Сборка из троек (строка, столбец, значение); повторяющиеся позиции суммируются.
CsrMatrix from_triplets(std::int64_t rows, std::int64_t cols, std::vector<Triplet> triplets);

CsrMatrix transpose(const CsrMatrix& a);

// C = A·B
CsrMatrix multiply(const CsrMatrix& a, const CsrMatrix& b);

// C = alpha·A + beta·B (одинакового размера)
CsrMatrix add(const CsrMatrix& a, const CsrMatrix& b, double alpha = 1.0, double beta = 1.0);

// Строки A, умноженные на s[i]: diag(s)·A
void scale_rows(CsrMatrix& a, std::span<const double> s);

// Удаляет хранимые нули.
void eliminate_zeros(CsrMatrix& a);

// Подматрица A[keep, keep]; keep — номера строк/столбцов по возрастанию.
CsrMatrix submatrix(const CsrMatrix& a, std::span<const std::int64_t> keep);

double dot(std::span<const double> x, std::span<const double> y);
double norm2(std::span<const double> x);

}  // namespace kika::linalg
