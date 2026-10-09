# Встраивание файлов в программу как массивов байтов.
#
#   cmake -DOUT=файл.cpp -DBASE=папка -DFILES=a.html,b.js -DNAMESPACE=kika::report::assets -P EmbedFiles.cmake
#
# Создаёт .cpp с функцией std::string_view get(std::string_view name): содержимое файла
# по его пути относительно BASE (пустая строка — нет такого файла).

if(NOT OUT OR NOT BASE OR NOT FILES OR NOT NAMESPACE)
  message(FATAL_ERROR "EmbedFiles: нужны OUT, BASE, FILES и NAMESPACE")
endif()

string(REPLACE "," ";" _files "${FILES}")
string(REPEAT "[0-9a-f]" 64 _line)

set(_arrays "")
set(_lookup "")
set(_index 0)
foreach(_name IN LISTS _files)
  file(READ "${BASE}/${_name}" _hex HEX)
  string(LENGTH "${_hex}" _len)
  if(_len EQUAL 0)
    message(FATAL_ERROR "EmbedFiles: пустой файл ${_name}")
  endif()
  # по 32 байта в строке, иначе у компиляторов бывают слишком длинные строки
  string(REGEX REPLACE "(${_line})" "\\1\n" _hex "${_hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
  string(APPEND _arrays "// ${_name}\nconst unsigned char d${_index}[] = {\n${_hex}\n};\n\n")
  string(APPEND _lookup "  if (name == \"${_name}\") return {reinterpret_cast<const char*>(d${_index}), sizeof(d${_index})};\n")
  math(EXPR _index "${_index} + 1")
endforeach()

set(_content "// Файл создан cmake/EmbedFiles.cmake из папки ${BASE} — не редактировать.\n\n#include <string_view>\n\nnamespace ${NAMESPACE} {\n\nnamespace {\n\n${_arrays}}  // namespace\n\nstd::string_view get(std::string_view name) {\n${_lookup}  return {};\n}\n\n}  // namespace ${NAMESPACE}\n")

# не переписываем файл без изменений — иначе он будет каждый раз пересобираться
if(EXISTS "${OUT}")
  file(READ "${OUT}" _old)
  if(_old STREQUAL _content)
    return()
  endif()
endif()
file(WRITE "${OUT}" "${_content}")
