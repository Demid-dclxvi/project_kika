# HTML-отчёт с 3D-видом. Отдельно от ядра: ему нужна zlib (сжатие данных отчёта),
# а ядро остаётся без сторонних библиотек.

find_package(ZLIB REQUIRED)

# Файлы просмотрщика встраиваются в программу: отчёт — один файл, без папок рядом.
set(KIKA_REPORT_WEB report.html report.js viewer.js viewer.css vendor/three.min.js)
set(_web_paths "")
foreach(_f IN LISTS KIKA_REPORT_WEB)
  list(APPEND _web_paths ${CMAKE_CURRENT_SOURCE_DIR}/web/${_f})
endforeach()
list(JOIN KIKA_REPORT_WEB "," _web_list)
set(_assets_cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/report_assets.cpp)
add_custom_command(
  OUTPUT ${_assets_cpp}
  COMMAND ${CMAKE_COMMAND} -DOUT=${_assets_cpp} -DBASE=${CMAKE_CURRENT_SOURCE_DIR}/web
          -DFILES=${_web_list} -DNAMESPACE=kika::report::assets
          -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
  DEPENDS ${_web_paths} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
  COMMENT "Встраиваю файлы просмотрщика в отчёт"
  VERBATIM)

add_library(kika_report src/report.cpp ${_assets_cpp})
add_library(kika::report ALIAS kika_report)
target_include_directories(kika_report PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kika_report PUBLIC kika::core PRIVATE ZLIB::ZLIB)
kika_set_compile_options(kika_report)
