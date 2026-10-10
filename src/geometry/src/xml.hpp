#pragma once
// Небольшой потоковый разбор XML для файлов 3MF: элементы, атрибуты, сущности.
// Без проверки по схеме и без DOM — у сеток бывают миллионы вершин.

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kika::geometry::xml {

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct Attribute {
  std::string_view name;   // как в файле, с префиксом пространства имён
  std::string_view value;  // сырое значение, без замены сущностей
};

class Reader {
 public:
  explicit Reader(std::string_view text) : s_(text) {}

  enum class Event { Start, End, Eof };
  // Следующий элемент. Для <a/> выдаются Start и сразу End.
  Event next();

  // Имя текущего элемента без префикса пространства имён.
  std::string_view name() const { return local(name_); }
  const std::vector<Attribute>& attributes() const { return attrs_; }
  // Значение атрибута по имени без префикса (сущности заменены); пусто — нет атрибута.
  bool attr(std::string_view local_name, std::string& out) const;
  // Номер строки текущего места — для сообщений.
  int line() const;

  static std::string_view local(std::string_view qname) {
    const auto p = qname.find(':');
    return p == std::string_view::npos ? qname : qname.substr(p + 1);
  }
  static std::string decode(std::string_view raw);

 private:
  std::string_view s_;
  std::size_t p_ = 0;
  std::string_view name_;
  std::vector<Attribute> attrs_;
  bool pending_end_ = false;
};

}  // namespace kika::geometry::xml
