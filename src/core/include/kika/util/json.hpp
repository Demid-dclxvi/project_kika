#pragma once
// Небольшая библиотека JSON для заданий на расчёт и результатов.
// Объекты сохраняют порядок ключей (как dict в Python), числа различаются на целые и дробные.
// Ядро не зависит от сторонних библиотек, поэтому JSON здесь свой.

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kika::json {

class ParseError : public std::runtime_error {
 public:
  ParseError(const std::string& msg, int line, int column)
      : std::runtime_error(msg), line_(line), column_(column) {}
  int line() const { return line_; }
  int column() const { return column_; }

 private:
  int line_, column_;
};

class Value;
using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;

class Value {
 public:
  enum class Type { kNull, kBool, kInt, kDouble, kString, kArray, kObject };

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : v_(b) {}
  Value(int i) : v_(static_cast<std::int64_t>(i)) {}
  Value(long i) : v_(static_cast<std::int64_t>(i)) {}
  Value(long long i) : v_(static_cast<std::int64_t>(i)) {}
  Value(unsigned i) : v_(static_cast<std::int64_t>(i)) {}
  Value(unsigned long i) : v_(static_cast<std::int64_t>(i)) {}
  Value(unsigned long long i) : v_(static_cast<std::int64_t>(i)) {}
  Value(double d) : v_(d) {}
  Value(const char* s) : v_(std::string(s)) {}
  Value(std::string s) : v_(std::move(s)) {}
  Value(std::string_view s) : v_(std::string(s)) {}
  Value(Array a) : v_(std::move(a)) {}
  Value(Object o) : v_(std::move(o)) {}

  static Value array() { return Value(Array{}); }
  static Value object() { return Value(Object{}); }
  template <class T>
  static Value array_of(const T& range) {
    Array a;
    for (const auto& x : range) a.emplace_back(x);
    return Value(std::move(a));
  }

  Type type() const { return static_cast<Type>(v_.index()); }
  bool is_null() const { return type() == Type::kNull; }
  bool is_bool() const { return type() == Type::kBool; }
  bool is_int() const { return type() == Type::kInt; }
  bool is_number() const { return type() == Type::kInt || type() == Type::kDouble; }
  bool is_string() const { return type() == Type::kString; }
  bool is_array() const { return type() == Type::kArray; }
  bool is_object() const { return type() == Type::kObject; }

  // Доступ с проверкой типа; при несовпадении — std::invalid_argument.
  bool as_bool() const;
  double as_double() const;  // целые тоже
  std::int64_t as_int() const;
  const std::string& as_string() const;
  const Array& as_array() const;
  Array& as_array();
  const Object& as_object() const;
  Object& as_object();

  // «Истинность» как в Python: null, false, 0, "", [], {} — ложь.
  bool truthy() const;

  // Поиск ключа в объекте; nullptr — нет ключа или это не объект.
  const Value* find(std::string_view key) const;
  bool contains(std::string_view key) const { return find(key) != nullptr; }
  // Значение по ключу (ключ добавляется в конец, если его нет). Null превращается в объект.
  Value& operator[](std::string_view key);
  // Элемент массива (с проверкой границ).
  const Value& at(std::size_t i) const { return as_array().at(i); }
  std::size_t size() const;
  void push_back(Value v);

  friend bool operator==(const Value& a, const Value& b) { return a.v_ == b.v_; }

 private:
  std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> v_{nullptr};
};

// Разбор текста JSON. Ошибка — ParseError с номером строки и столбца (по-русски).
Value parse(std::string_view text);

// Запись в текст. indent < 0 — в одну строку; иначе с отступами.
// Нечисловые значения (NaN, бесконечность) записываются как null, не-ASCII — как есть (UTF-8).
std::string dump(const Value& v, int indent = -1);

// Название типа по-русски — для сообщений об ошибках в заданиях.
const char* type_name(Value::Type t);

}  // namespace kika::json
