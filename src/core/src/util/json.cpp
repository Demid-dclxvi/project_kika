#include "kika/util/json.hpp"

#include <charconv>
#include <cmath>
#include <system_error>

namespace kika::json {

namespace {

[[noreturn]] void type_error(Value::Type want, Value::Type got) {
  throw std::invalid_argument(std::string("ожидается ") + type_name(want) + ", а записано " + type_name(got));
}

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

class Parser {
 public:
  explicit Parser(std::string_view t) : t_(t) {}

  Value document() {
    // метка порядка байтов UTF-8 (так сохраняет Блокнот)
    if (t_.substr(0, 3) == "\xEF\xBB\xBF") p_ = 3;
    skip_ws();
    Value v = value(0);
    skip_ws();
    if (p_ != t_.size()) fail("лишние символы после конца JSON");
    return v;
  }

 private:
  [[noreturn]] void fail(const std::string& msg) const {
    int line = 1, col = 1;
    for (std::size_t i = 0; i < p_ && i < t_.size(); ++i) {
      if (t_[i] == '\n') {
        ++line;
        col = 1;
      } else if ((static_cast<unsigned char>(t_[i]) & 0xC0) != 0x80) {
        ++col;
      }
    }
    throw ParseError("Ошибка в JSON (строка " + std::to_string(line) + ", столбец " + std::to_string(col) +
                         "): " + msg,
                     line, col);
  }

  void skip_ws() {
    while (p_ < t_.size()) {
      const char c = t_[p_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++p_;
      } else {
        break;
      }
    }
  }

  bool eat(char c) {
    if (p_ < t_.size() && t_[p_] == c) {
      ++p_;
      return true;
    }
    return false;
  }

  void expect_word(std::string_view w) {
    if (t_.substr(p_, w.size()) != w) fail("непонятное значение");
    p_ += w.size();
  }

  Value value(int depth) {
    if (depth > 200) fail("слишком глубокая вложенность");
    if (p_ >= t_.size()) fail("неожиданный конец текста");
    const char c = t_[p_];
    switch (c) {
      case '{':
        return object(depth);
      case '[':
        return array(depth);
      case '"':
        return Value(string());
      case 't':
        expect_word("true");
        return Value(true);
      case 'f':
        expect_word("false");
        return Value(false);
      case 'n':
        expect_word("null");
        return Value(nullptr);
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        if (c == '\'') fail("строки в JSON пишутся в двойных кавычках");
        fail("непонятное значение");
    }
  }

  Value object(int depth) {
    ++p_;  // {
    Object o;
    skip_ws();
    if (eat('}')) return Value(std::move(o));
    for (;;) {
      skip_ws();
      if (p_ >= t_.size() || t_[p_] != '"') fail("ожидается название поля в двойных кавычках");
      std::string key = string();
      skip_ws();
      if (!eat(':')) fail("после названия поля ожидается двоеточие");
      skip_ws();
      Value v = value(depth + 1);
      bool replaced = false;  // повтор ключа: побеждает последнее значение, как в Python
      for (auto& m : o)
        if (m.first == key) {
          m.second = std::move(v);
          replaced = true;
          break;
        }
      if (!replaced) o.emplace_back(std::move(key), std::move(v));
      skip_ws();
      if (eat('}')) break;
      if (!eat(',')) fail("ожидается запятая или закрывающая скобка }");
      skip_ws();
      if (p_ < t_.size() && t_[p_] == '}') fail("лишняя запятая перед }");
    }
    return Value(std::move(o));
  }

  Value array(int depth) {
    ++p_;  // [
    Array a;
    skip_ws();
    if (eat(']')) return Value(std::move(a));
    for (;;) {
      skip_ws();
      a.push_back(value(depth + 1));
      skip_ws();
      if (eat(']')) break;
      if (!eat(',')) fail("ожидается запятая или закрывающая скобка ]");
      skip_ws();
      if (p_ < t_.size() && t_[p_] == ']') fail("лишняя запятая перед ]");
    }
    return Value(std::move(a));
  }

  std::uint32_t hex4() {
    if (p_ + 4 > t_.size()) fail("неполная последовательность \\u");
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = t_[p_++];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        fail("неверная последовательность \\u");
      }
    }
    return v;
  }

  std::string string() {
    ++p_;  // "
    std::string out;
    for (;;) {
      if (p_ >= t_.size()) fail("строка не закрыта кавычкой");
      const char c = t_[p_++];
      if (c == '"') break;
      if (static_cast<unsigned char>(c) < 0x20) fail("перевод строки или управляющий символ внутри строки");
      if (c != '\\') {
        out += c;
        continue;
      }
      if (p_ >= t_.size()) fail("строка не закрыта кавычкой");
      const char e = t_[p_++];
      switch (e) {
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case '/':
          out += '/';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          std::uint32_t cp = hex4();
          if (cp >= 0xD800 && cp < 0xDC00 && t_.substr(p_, 2) == "\\u") {
            p_ += 2;
            const std::uint32_t lo = hex4();
            if (lo >= 0xDC00 && lo < 0xE000) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              append_utf8(out, 0xFFFD);
              cp = lo;
            }
          }
          if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
          append_utf8(out, cp);
          break;
        }
        default:
          fail(std::string("неизвестная escape-последовательность \\") + e);
      }
    }
    return out;
  }

  Value number() {
    const std::size_t start = p_;
    bool is_float = false;
    eat('-');
    if (eat('0')) {
    } else if (p_ < t_.size() && t_[p_] >= '1' && t_[p_] <= '9') {
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
    } else {
      fail("неверное число");
    }
    if (eat('.')) {
      is_float = true;
      if (!(p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9')) fail("после точки в числе нужны цифры");
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
    }
    if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
      is_float = true;
      ++p_;
      if (!eat('+')) eat('-');
      if (!(p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9')) fail("неверная степень в числе");
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
    }
    const char* b = t_.data() + start;
    const char* e = t_.data() + p_;
    if (!is_float) {
      std::int64_t i = 0;
      const auto r = std::from_chars(b, e, i);
      if (r.ec == std::errc() && r.ptr == e) return Value(static_cast<long long>(i));
      // слишком большое целое — как дробное
    }
    double d = 0;
    const auto r = std::from_chars(b, e, d);
    if (r.ec == std::errc::result_out_of_range) {
      // переполнение: как в Python — бесконечность или ноль
      d = (*b == '-') ? -HUGE_VAL : HUGE_VAL;
      for (const char* q = b; q < e; ++q)
        if (*q == 'e' || *q == 'E') {
          if (q[1] == '-') d = 0.0;
          break;
        }
    } else if (r.ec != std::errc() || r.ptr != e) {
      fail("неверное число");
    }
    return Value(d);
  }

  std::string_view t_;
  std::size_t p_ = 0;
};

void dump_string(std::string& out, const std::string& s) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (c < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += ch;
        }
    }
  }
  out += '"';
}

void dump_double(std::string& out, double d) {
  if (!std::isfinite(d)) {
    out += "null";
    return;
  }
  // как repr() в Python: кратчайшая запись, которая читается обратно в то же число;
  // от 1e-4 до 1e16 — без степени
  char buf[400];
  const double a = std::abs(d);
  const auto fmt = (a == 0.0 || (a >= 1e-4 && a < 1e16)) ? std::chars_format::fixed : std::chars_format::scientific;
  const auto r = std::to_chars(buf, buf + sizeof buf, d, fmt);
  std::string_view sv(buf, static_cast<std::size_t>(r.ptr - buf));
  out += sv;
  // целое значение дробного числа помечаем «.0», чтобы тип сохранялся при обратном чтении
  if (sv.find_first_of(".eE") == std::string_view::npos) out += ".0";
}

void dump_value(std::string& out, const Value& v, int indent, int level) {
  const char* item_sep = indent == kCompact ? "," : (indent < 0 ? ", " : ",");
  const char* key_sep = indent == kCompact ? ":" : ": ";
  auto newline = [&](int lv) {
    if (indent < 0) return;
    out += '\n';
    out.append(static_cast<std::size_t>(indent * lv), ' ');
  };
  switch (v.type()) {
    case Value::Type::kNull:
      out += "null";
      break;
    case Value::Type::kBool:
      out += v.as_bool() ? "true" : "false";
      break;
    case Value::Type::kInt:
      out += std::to_string(v.as_int());
      break;
    case Value::Type::kDouble:
      dump_double(out, v.as_double());
      break;
    case Value::Type::kString:
      dump_string(out, v.as_string());
      break;
    case Value::Type::kArray: {
      const auto& a = v.as_array();
      if (a.empty()) {
        out += "[]";
        break;
      }
      out += '[';
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (i) out += item_sep;
        newline(level + 1);
        dump_value(out, a[i], indent, level + 1);
      }
      newline(level);
      out += ']';
      break;
    }
    case Value::Type::kObject: {
      const auto& o = v.as_object();
      if (o.empty()) {
        out += "{}";
        break;
      }
      out += '{';
      for (std::size_t i = 0; i < o.size(); ++i) {
        if (i) out += item_sep;
        newline(level + 1);
        dump_string(out, o[i].first);
        out += key_sep;
        dump_value(out, o[i].second, indent, level + 1);
      }
      newline(level);
      out += '}';
      break;
    }
  }
}

}  // namespace

const char* type_name(Value::Type t) {
  switch (t) {
    case Value::Type::kNull:
      return "null";
    case Value::Type::kBool:
      return "true/false";
    case Value::Type::kInt:
    case Value::Type::kDouble:
      return "число";
    case Value::Type::kString:
      return "строка";
    case Value::Type::kArray:
      return "список [..]";
    case Value::Type::kObject:
      return "объект {..}";
  }
  return "?";
}

bool Value::as_bool() const {
  if (!is_bool()) type_error(Type::kBool, type());
  return std::get<bool>(v_);
}

double Value::as_double() const {
  if (type() == Type::kInt) return static_cast<double>(std::get<std::int64_t>(v_));
  if (type() == Type::kDouble) return std::get<double>(v_);
  // как float() в Python: true/false — 1/0
  if (type() == Type::kBool) return std::get<bool>(v_) ? 1.0 : 0.0;
  type_error(Type::kDouble, type());
}

std::int64_t Value::as_int() const {
  if (type() == Type::kInt) return std::get<std::int64_t>(v_);
  if (type() == Type::kDouble) {
    const double d = std::get<double>(v_);
    if (std::isfinite(d) && std::abs(d) < 9.2e18) return static_cast<std::int64_t>(d);
  }
  if (type() == Type::kBool) return std::get<bool>(v_) ? 1 : 0;
  type_error(Type::kInt, type());
}

const std::string& Value::as_string() const {
  if (!is_string()) type_error(Type::kString, type());
  return std::get<std::string>(v_);
}

const Array& Value::as_array() const {
  if (!is_array()) type_error(Type::kArray, type());
  return std::get<Array>(v_);
}

Array& Value::as_array() {
  if (!is_array()) type_error(Type::kArray, type());
  return std::get<Array>(v_);
}

const Object& Value::as_object() const {
  if (!is_object()) type_error(Type::kObject, type());
  return std::get<Object>(v_);
}

Object& Value::as_object() {
  if (!is_object()) type_error(Type::kObject, type());
  return std::get<Object>(v_);
}

bool Value::truthy() const {
  switch (type()) {
    case Type::kNull:
      return false;
    case Type::kBool:
      return std::get<bool>(v_);
    case Type::kInt:
      return std::get<std::int64_t>(v_) != 0;
    case Type::kDouble:
      return std::get<double>(v_) != 0.0;
    case Type::kString:
      return !std::get<std::string>(v_).empty();
    case Type::kArray:
      return !std::get<Array>(v_).empty();
    case Type::kObject:
      return !std::get<Object>(v_).empty();
  }
  return false;
}

const Value* Value::find(std::string_view key) const {
  if (!is_object()) return nullptr;
  for (const auto& m : std::get<Object>(v_))
    if (m.first == key) return &m.second;
  return nullptr;
}

Value& Value::operator[](std::string_view key) {
  if (is_null()) v_ = Object{};
  auto& o = as_object();
  for (auto& m : o)
    if (m.first == key) return m.second;
  o.emplace_back(std::string(key), Value());
  return o.back().second;
}

std::size_t Value::size() const {
  if (is_array()) return std::get<Array>(v_).size();
  if (is_object()) return std::get<Object>(v_).size();
  return 0;
}

void Value::push_back(Value v) {
  if (is_null()) v_ = Array{};
  as_array().push_back(std::move(v));
}

Value parse(std::string_view text) { return Parser(text).document(); }

std::string dump(const Value& v, int indent) {
  std::string out;
  dump_value(out, v, indent, 0);
  return out;
}

}  // namespace kika::json
