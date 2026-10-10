# Свой слайсер: сечения, стенки, заполнение, G-code. Контуры — Clipper2 (BSL-1.0, через vcpkg).

find_package(Clipper2 CONFIG REQUIRED)

add_library(kika_slicer src/presets.cpp src/slicer.cpp src/print_job.cpp)
add_library(kika::slicer ALIAS kika_slicer)
target_include_directories(kika_slicer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kika_slicer PUBLIC kika::geometry kika::core PRIVATE Clipper2::Clipper2)
kika_set_compile_options(kika_slicer)
