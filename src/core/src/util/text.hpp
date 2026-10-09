#pragma once
// Внутренние утилиты для работы с текстом G-code (только ASCII-логика; UTF-8 проходит насквозь).

#include <optional>
#include <string>
#include <string_view>

namespace kika::util {

// Пробельные символы так же, как str.strip() в Python для ASCII.
constexpr bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
         (c >= '\x1c' && c <= '\x1f');
}

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

constexpr bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

constexpr char to_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

constexpr char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string_view trim(std::string_view s);
std::string to_upper(std::string_view s);
std::string to_lower(std::string_view s);

// Число как float() в Python: пробелы по краям, знак, десятичная/экспоненциальная запись, inf, nan.
// nullopt, если строка — не число целиком.
std::optional<double> parse_double(std::string_view s);

// Целое как int() в Python: пробелы по краям, знак, только цифры.
std::optional<long long> parse_int(std::string_view s);

// Обходит строки текста; разделители — \n, \r\n и \r (как str.splitlines() для G-code).
// Пустой хвост после последнего перевода строки строкой не считается.
template <class F>
void for_each_line(std::string_view text, F&& f) {
  std::size_t i = 0;
  const std::size_t n = text.size();
  while (i < n) {
    std::size_t j = i;
    while (j < n && text[j] != '\n' && text[j] != '\r') ++j;
    f(text.substr(i, j - i));
    if (j < n && text[j] == '\r' && j + 1 < n && text[j + 1] == '\n')
      i = j + 2;
    else
      i = j + 1;
  }
}

}  // namespace kika::util
