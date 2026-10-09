# Консольная программа kika: расчёт по файлам, без интерфейса.
# Сторонних библиотек не нужно: параметры разбираются вручную, JSON — из ядра.

add_executable(kika main.cpp)
target_link_libraries(kika PRIVATE kika::core)
kika_set_compile_options(kika)
