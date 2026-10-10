#pragma once
// Чтение ZIP-архива целиком из памяти: 3MF и .gcode.3mf — это ZIP.
// Поддерживаются способы хранения «без сжатия» и deflate, ZIP64.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kika::geometry {

class ZipError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class ZipArchive {
 public:
  // data должна жить дольше архива.
  explicit ZipArchive(std::string_view data);

  struct Entry {
    std::string name;
    std::uint16_t method = 0;
    std::uint64_t compressed = 0;
    std::uint64_t size = 0;
    std::uint64_t local_offset = 0;
  };
  const std::vector<Entry>& entries() const { return entries_; }
  // Файл по имени (без учёта регистра и ведущего «/»); пусто — нет такого.
  const Entry* find(std::string_view name) const;
  std::string read(const Entry& e) const;
  std::optional<std::string> read(std::string_view name) const;

 private:
  std::string_view data_;
  std::vector<Entry> entries_;
};

// Похоже ли на ZIP (сигнатура PK\3\4).
bool looks_like_zip(std::string_view data);

}  // namespace kika::geometry
