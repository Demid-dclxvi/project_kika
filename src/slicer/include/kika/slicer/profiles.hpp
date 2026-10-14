#pragma once
// Описание всех настроек нарезки одной таблицей: ключ JSON, название по-русски, единицы, пределы.
// По таблице строятся профили (принтер, пластик, печать) в JSON, поле "print" задания и окно
// настроек — поэтому настройка, добавленная сюда, сразу появляется везде.
//
// Доли 0…1 (заполнение, обдув) показываются и пишутся в JSON в процентах (scale = 100).

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "kika/slicer/slicer.hpp"
#include "kika/util/json.hpp"

namespace kika::slicer {

// Чей это параметр: профиль принтера, профиль пластика или настройки печати.
enum class Section { Printer, Filament, Process };
std::string_view section_key(Section s);   // printer, filament, process
std::string_view section_name(Section s);  // «Принтер», «Пластик», «Печать»

enum class FieldKind {
  Number,   // число с пределами
  Integer,  // целое
  Bool,     // да/нет
  Text,     // строка в одну строку
  Code,     // многострочный G-code
  Choice,   // одно из списка (ключи — в choices)
};

struct FieldChoice {
  std::string key;
  std::string name;
};

struct Field {
  Section section;
  std::string key;     // ключ JSON
  std::string page;    // страница в окне, как в OrcaSlicer: «Качество», «Прочность», «Скорость»…
  std::string group;   // раздел страницы: «Высота слоя», «Ширина линий»…
  std::string label;   // по-русски
  std::string unit;    // «мм», «°C», «%»…
  std::string tip;     // подсказка, может быть пустой
  FieldKind kind = FieldKind::Number;
  double lo = 0, hi = 0;  // пределы в единицах показа
  double scale = 1;       // показ = значение × scale
  bool advanced = false;  // тонкая настройка: в окне — только в режиме «Все настройки»
  std::vector<FieldChoice> choices;
  // Проверка строки (Text): ошибка по-русски или пусто; не задана — любая строка.
  std::function<std::string(std::string_view)> check_text;
  // Вариант: ключ из JSON (с синонимами, например zig-zag) → ключ из choices; пусто — нет такого.
  std::function<std::string(std::string_view)> canonical;
  // Значение в единицах показа (числа), строка или ключ варианта.
  std::function<json::Value(const Settings&)> get;
  // Записать значение (уже проверенное check).
  std::function<void(Settings&, const json::Value&)> set;
};

const std::vector<Field>& fields();
const Field* find_field(Section section, std::string_view key);
// Страницы раздела по порядку (как в таблице).
std::vector<std::string> pages(Section section);

// Проверка значения поля; ошибка — текст по-русски (where — откуда, для сообщения), иначе пусто.
std::string check_field(const Field& f, const json::Value& v, std::string_view where);

// Профиль раздела в JSON: все поля (base == nullptr) или только отличающиеся от base.
json::Value section_to_json(const Settings& s, Section section, const Settings* base = nullptr);
// Значения раздела из JSON поверх s. Неизвестные ключи и неверные значения — исключение
// analysis::JobError с текстом по-русски; ключи из skip пропускаются молча.
void section_from_json(Settings& s, Section section, const json::Value& o, std::string_view where,
                       const std::vector<std::string>& skip = {});

// Значение поля для показа человеку: «0,2 мм», «да», «Гироид».
std::string field_text(const Field& f, const Settings& s);

}  // namespace kika::slicer
