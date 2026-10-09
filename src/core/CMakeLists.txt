# Ядро: библиотека без интерфейса. Её используют консольная программа, тесты,
# позже — приложение на Qt и серверные расчёты.

add_library(kika_core
  src/analysis/analysis.cpp
  src/analysis/job.cpp
  src/analysis/loads.cpp
  src/gcode/parser.cpp
  src/gcode/roles.cpp
  src/gcode/toolpaths.cpp
  src/fem/assembly.cpp
  src/fem/element.cpp
  src/fem/mesh.cpp
  src/linalg/amg.cpp
  src/linalg/dense.cpp
  src/linalg/sparse.cpp
  src/material/builtin.cpp
  src/material/material.cpp
  src/util/json.cpp
  src/util/parallel.cpp
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
find_package(Threads REQUIRED)
target_link_libraries(kika_core PUBLIC Threads::Threads)
target_compile_features(kika_core PUBLIC cxx_std_20)
kika_set_compile_options(kika_core)
