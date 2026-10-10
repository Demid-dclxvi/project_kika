cmake_minimum_required(VERSION 3.25)

project(kika
  VERSION 0.1.0
  DESCRIPTION "Strength analysis of FDM 3D-printed parts from print toolpaths"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Все исполняемые файлы — в build/<preset>/bin, библиотеки — в build/<preset>/lib
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(KIKA_BUILD_TESTS "Собирать тесты" ON)
option(KIKA_WERROR "Считать предупреждения компилятора ошибками (включено в CI)" OFF)
option(KIKA_BUILD_GUI "Собирать окно kika-gui (нужен Qt 6: Widgets, OpenGLWidgets)" OFF)

include(cmake/CompilerOptions.cmake)

add_subdirectory(src/core)
add_subdirectory(src/report)
add_subdirectory(src/geometry)
add_subdirectory(src/slicer)
add_subdirectory(src/app)
add_subdirectory(apps/cli)
if(KIKA_BUILD_GUI)
  add_subdirectory(apps/gui)
endif()

if(KIKA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
