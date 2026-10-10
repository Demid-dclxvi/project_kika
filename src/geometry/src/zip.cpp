// Чтение ZIP из памяти (центральный каталог, deflate через zlib, ZIP64).

#include "kika/geometry/zip.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace kika::geometry {

namespace {

std::uint32_t u16(std::string_view d, std::size_t p) {
  if (p + 2 > d.size()) throw ZipError("архив обрезан");
  return static_cast<std::uint32_t>(static_cast<unsigned char>(d[p])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(d[p + 1])) << 8);
}

std::uint32_t u32(std::string_view d, std::size_t p) { return u16(d, p) | (u16(d, p + 2) << 16); }

std::uint64_t u64(std::string_view d, std::size_t p) {
  return static_cast<std::uint64_t>(u32(d, p)) | (static_cast<std::uint64_t>(u32(d, p + 4)) << 32);
}

std::string normalize(std::string_view name) {
  while (!name.empty() && (name.front() == '/' || name.front() == '\\')) name.remove_prefix(1);
  std::string s(name);
  for (char& c : s) {
    if (c == '\\') c = '/';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

}  // namespace

bool looks_like_zip(std::string_view data) { return data.size() >= 4 && data.substr(0, 4) == std::string_view("PK\3\4", 4); }

ZipArchive::ZipArchive(std::string_view data) : data_(data) {
  if (data.size() < 22) throw ZipError("это не ZIP-архив");
  // конец центрального каталога ищем с конца (после него может быть комментарий до 64 КБ)
  std::size_t eocd = std::string_view::npos;
  const std::size_t stop = data.size() > 65557 ? data.size() - 65557 : 0;
  for (std::size_t p = data.size() - 22 + 1; p-- > stop;)
    if (u32(data, p) == 0x06054b50u) {
      eocd = p;
      break;
    }
  if (eocd == std::string_view::npos) throw ZipError("это не ZIP-архив или он повреждён (нет каталога)");
  std::uint64_t count = u16(data, eocd + 10);
  std::uint64_t cd_size = u32(data, eocd + 12);
  std::uint64_t cd_off = u32(data, eocd + 16);
  if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
    // ZIP64: перед концом каталога — указатель на расширенную запись
    if (eocd < 20 || u32(data, eocd - 20) != 0x07064b50u) throw ZipError("повреждён каталог ZIP64");
    const std::uint64_t rec = u64(data, eocd - 20 + 8);
    if (rec + 56 > data.size() || u32(data, static_cast<std::size_t>(rec)) != 0x06064b50u)
      throw ZipError("повреждён каталог ZIP64");
    count = u64(data, static_cast<std::size_t>(rec) + 32);
    cd_size = u64(data, static_cast<std::size_t>(rec) + 40);
    cd_off = u64(data, static_cast<std::size_t>(rec) + 48);
  }
  if (cd_off + cd_size > data.size()) throw ZipError("каталог ZIP выходит за конец файла — архив обрезан");
  std::size_t p = static_cast<std::size_t>(cd_off);
  entries_.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, 1u << 20)));
  for (std::uint64_t i = 0; i < count; ++i) {
    if (u32(data, p) != 0x02014b50u) throw ZipError("повреждён каталог ZIP");
    Entry e;
    const std::uint32_t flags = u16(data, p + 8);
    e.method = static_cast<std::uint16_t>(u16(data, p + 10));
    e.compressed = u32(data, p + 20);
    e.size = u32(data, p + 24);
    const std::size_t name_len = u16(data, p + 28), extra_len = u16(data, p + 30), comment_len = u16(data, p + 32);
    e.local_offset = u32(data, p + 42);
    if (p + 46 + name_len + extra_len > data.size()) throw ZipError("повреждён каталог ZIP");
    e.name = std::string(data.substr(p + 46, name_len));
    // расширенные размеры ZIP64 — в поле 0x0001, только для тех, что равны 0xFFFFFFFF
    std::size_t x = p + 46 + name_len;
    const std::size_t xend = x + extra_len;
    while (x + 4 <= xend) {
      const std::uint32_t id = u16(data, x), len = u16(data, x + 2);
      std::size_t q = x + 4;
      if (id == 0x0001) {
        if (e.size == 0xFFFFFFFFu && q + 8 <= x + 4 + len) e.size = u64(data, q), q += 8;
        if (e.compressed == 0xFFFFFFFFu && q + 8 <= x + 4 + len) e.compressed = u64(data, q), q += 8;
        if (e.local_offset == 0xFFFFFFFFu && q + 8 <= x + 4 + len) e.local_offset = u64(data, q);
      }
      x += 4 + len;
    }
    if (flags & 1u) throw ZipError("архив зашифрован: файл «" + e.name + "»");
    entries_.push_back(std::move(e));
    p += 46 + name_len + extra_len + comment_len;
  }
}

const ZipArchive::Entry* ZipArchive::find(std::string_view name) const {
  const std::string want = normalize(name);
  for (const auto& e : entries_)
    if (normalize(e.name) == want) return &e;
  return nullptr;
}

std::string ZipArchive::read(const Entry& e) const {
  const auto lo = static_cast<std::size_t>(e.local_offset);
  if (lo + 30 > data_.size() || u32(data_, lo) != 0x04034b50u) throw ZipError("повреждён файл «" + e.name + "» в архиве");
  const std::size_t start = lo + 30 + u16(data_, lo + 26) + u16(data_, lo + 28);
  if (start + e.compressed > data_.size()) throw ZipError("файл «" + e.name + "» обрезан");
  const std::string_view src = data_.substr(start, static_cast<std::size_t>(e.compressed));
  if (e.method == 0) return std::string(src);
  if (e.method != 8) throw ZipError("файл «" + e.name + "» сжат неподдерживаемым способом " + std::to_string(e.method));
  std::string out;
  out.resize(static_cast<std::size_t>(e.size));
  z_stream zs{};
  if (inflateInit2(&zs, -15) != Z_OK) throw ZipError("не удалось начать распаковку");
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(src.data()));
  std::size_t done_in = 0, done_out = 0;
  int rc = Z_OK;
  // zlib берёт размеры в uInt: большие файлы — кусками
  constexpr std::size_t kChunk = 1u << 30;
  while (rc != Z_STREAM_END) {
    if (zs.avail_in == 0 && done_in < src.size()) {
      const std::size_t n = std::min(kChunk, src.size() - done_in);
      zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(src.data() + done_in));
      zs.avail_in = static_cast<uInt>(n);
      done_in += n;
    }
    if (done_out == out.size()) out.resize(out.size() + std::max<std::size_t>(out.size() / 2, 1 << 16));
    const std::size_t room = std::min(kChunk, out.size() - done_out);
    zs.next_out = reinterpret_cast<Bytef*>(out.data() + done_out);
    zs.avail_out = static_cast<uInt>(room);
    rc = inflate(&zs, Z_NO_FLUSH);
    done_out += room - zs.avail_out;
    if (rc != Z_OK && rc != Z_STREAM_END) {
      inflateEnd(&zs);
      throw ZipError("файл «" + e.name + "» в архиве повреждён");
    }
    if (rc == Z_OK && zs.avail_in == 0 && done_in >= src.size() && zs.avail_out != 0) {
      inflateEnd(&zs);
      throw ZipError("файл «" + e.name + "» в архиве обрезан");
    }
  }
  inflateEnd(&zs);
  out.resize(done_out);
  return out;
}

std::optional<std::string> ZipArchive::read(std::string_view name) const {
  const Entry* e = find(name);
  if (!e) return std::nullopt;
  return read(*e);
}

}  // namespace kika::geometry
