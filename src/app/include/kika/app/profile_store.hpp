#pragma once
// Свои профили принтера, пластика и печати — файлы JSON в папке настроек программы:
//   Windows — %APPDATA%\Kika, macOS — ~/Library/Application Support/Kika,
//   Linux — $XDG_CONFIG_HOME/kika (~/.config/kika); переопределить — KIKA_CONFIG_DIR.
// Файл: {"kika_profile": "filament", "name": "Мой PETG", "base": "PETG", "values": {…отличия…}}.
// base — встроенный профиль, от которого считаются отличия: ключ принтера, тип пластика, для
// печати — "default".

#include <filesystem>
#include <string>
#include <vector>

#include "kika/slicer/profiles.hpp"
#include "kika/slicer/slicer.hpp"

namespace kika::app {

std::filesystem::path config_dir();

struct ProfileRef {
  std::string name;
  std::filesystem::path path;
};

// Свои профили раздела по имени.
std::vector<ProfileRef> list_profiles(slicer::Section section);
// Загрузить профиль поверх s (раздел целиком: сначала встроенный base, потом отличия).
// Ошибка — analysis::JobError по-русски.
void load_profile(slicer::Section section, const std::filesystem::path& path, slicer::Settings& s);
// Сохранить раздел из s под именем name (заменяет такой же); возвращает путь файла.
std::filesystem::path save_profile(slicer::Section section, const std::string& name, const slicer::Settings& s);
void delete_profile(const std::filesystem::path& path);

}  // namespace kika::app
