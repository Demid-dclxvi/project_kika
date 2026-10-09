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

}  // namespace kika::util
