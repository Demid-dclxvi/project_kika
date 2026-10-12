# Подбор положения детали и настроек печати по двум целям из трёх: свой слайсер + расчёт на
# грубой сетке. Без Qt — окно и командная строка вызывают одно и то же.

add_library(kika_advisor src/advisor.cpp)
add_library(kika::advisor ALIAS kika_advisor)
target_include_directories(kika_advisor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kika_advisor PUBLIC kika::app kika::slicer kika::geometry kika::core)
kika_set_compile_options(kika_advisor)
