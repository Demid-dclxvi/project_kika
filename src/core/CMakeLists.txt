# Ядро: библиотека без интерфейса. Её используют консольная программа, тесты,
# позже — приложение на Qt и серверные расчёты.

add_library(kika_core
  src/gcode/parser.cpp
  src/gcode/roles.cpp
  src/gcode/toolpaths.cpp
  src/linalg/dense.cpp
  src/linalg/sparse.cpp
  src/material/builtin.cpp
  src/material/material.cpp
  src/util/text.cpp
  src/voxel/voxelize.cpp
)
add_library(kika::core ALIAS kika_core)

configure_file(include/kika/version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/kika/version.hpp @ONLY)

target_include_directories(kika_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(kika_core PUBLIC cxx_std_20)
kika_set_compile_options(kika_core)
