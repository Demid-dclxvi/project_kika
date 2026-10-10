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

if(KIKA_WITH_OCCT)
  # OpenCascade 8 (vcpkg): STEP — в TKDESTEP; 7.x (пакеты Linux): в TKSTEP
  find_package(OpenCASCADE CONFIG REQUIRED)
  if(TARGET TKDESTEP)
    set(kika_occt_step TKDESTEP)
  else()
    set(kika_occt_step TKSTEP TKSTEPBase)
  endif()
  if(DEFINED OpenCASCADE_BUILD_SHARED_LIBS AND NOT OpenCASCADE_BUILD_SHARED_LIBS)
    message(FATAL_ERROR "OpenCascade (LGPL-2.1) подключается только динамически, а найдена статическая сборка. "
                        "Для vcpkg — триплет из cmake/triplets (VCPKG_OVERLAY_TRIPLETS).")
  endif()
  target_sources(kika_geometry PRIVATE src/step.cpp)
  target_compile_definitions(kika_geometry PRIVATE KIKA_HAS_STEP)
  # заголовки OpenCascade — как системные: их предупреждения не наши
  target_include_directories(kika_geometry SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
  target_link_libraries(kika_geometry PRIVATE ${kika_occt_step} TKXSBase TKMesh TKTopAlgo TKBRep TKGeomBase TKG3d TKMath
                                              TKernel)
  message(STATUS "Чтение STEP: OpenCascade ${OpenCASCADE_VERSION}")
endif()
