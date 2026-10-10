# Консольная программа kika: расчёт по файлам, без интерфейса.
# Параметры разбираются вручную, JSON — из ядра, отчёт — kika::report.

add_executable(kika main.cpp)
target_link_libraries(kika PRIVATE kika::core kika::report kika::geometry kika::slicer)
if(WIN32)
  # открыть отчёт в браузере: ShellExecuteW, CoInitializeEx
  target_link_libraries(kika PRIVATE shell32 ole32)
endif()
kika_set_compile_options(kika)
