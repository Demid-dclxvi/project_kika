# Логика окна без Qt: сцена (видимые грани, цвета, выбор граней) и задание из окна.
# Проверяется тестами без графики; окно (apps/gui) только рисует и показывает.

add_library(kika_app src/scene.cpp src/job_model.cpp)
add_library(kika::app ALIAS kika_app)
target_include_directories(kika_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kika_app PUBLIC kika::core)
kika_set_compile_options(kika_app)
