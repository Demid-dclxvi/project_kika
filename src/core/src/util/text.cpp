#include "util/text.hpp"

#include <charconv>
#include <system_error>

namespace kika::util {

std::string_view trim(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && is_space(s[b])) ++b;
  while (e > b && is_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::string to_upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = to_upper(c);
  return out;
}

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = to_lower(c);
  return out;
}

std::optional<double> parse_double(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  // std::from_chars не принимает ведущий '+', Python принимает.
  if (s.front() == '+') {
    s.remove_prefix(1);
    if (s.empty() || s.front() == '+' || s.front() == '-') return std::nullopt;
  }
  double value = 0.0;
  const char* first = s.data();
  const char* last = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(first, last, value, std::chars_format::general);
  if (ec != std::errc() || ptr != last) return std::nullopt;
  return value;
}

std::optional<long long> parse_int(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  bool negative = false;
  if (s.front() == '+' || s.front() == '-') {
    negative = s.front() == '-';
    s.remove_prefix(1);
  }
  if (s.empty()) return std::nullopt;
  for (char c : s)
    if (!is_digit(c)) return std::nullopt;
  long long value = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
  return negative ? -value : value;
}

std::string to_lower_utf8(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == 0xD0 && i + 1 < s.size()) {
      const auto d = static_cast<unsigned char>(s[i + 1]);
      if (d >= 0x90 && d <= 0x9F) {  // А–П → а–п
        out += static_cast<char>(0xD0);
        out += static_cast<char>(d + 0x20);
        ++i;
        continue;
      }
      if (d >= 0xA0 && d <= 0xAF) {  // Р–Я → р–я
        out += static_cast<char>(0xD1);
        out += static_cast<char>(d - 0x20);
        ++i;
        continue;
      }
      if (d == 0x81) {  // Ё → ё
        out += static_cast<char>(0xD1);
        out += static_cast<char>(0x91);
        ++i;
        continue;
      }
    }
    out += to_lower(static_cast<char>(c));
  }
  return out;
}

}  // namespace kika::util
