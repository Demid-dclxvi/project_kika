# Модели для своего слайсера: треугольная сетка, чтение STL и 3MF (ZIP + XML — свои),
# STEP — через OpenCascade, только если KIKA_WITH_OCCT (см. корневой CMakeLists.txt).

find_package(ZLIB REQUIRED)

add_library(kika_geometry
  src/mesh.cpp
  src/zip.cpp
  src/xml.cpp
  src/stl.cpp
  src/threemf.cpp
  src/import.cpp
)
add_library(kika::geometry ALIAS kika_geometry)
target_include_directories(kika_geometry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kika_geometry PRIVATE ZLIB::ZLIB)
kika_set_compile_options(kika_geometry)
