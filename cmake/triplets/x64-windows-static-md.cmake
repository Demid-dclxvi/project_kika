# Триплет vcpkg для сборки с окном (пресеты win-gui-*): библиотеки статически, среда выполнения
# C++ — DLL (/MD), как у Qt. Заменяет встроенный триплет с тем же именем (VCPKG_OVERLAY_TRIPLETS).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# OpenCascade (LGPL-2.1 с исключением) — только динамически: TK*.dll лежат рядом с программой
# отдельными файлами и заменяемы. Собирается только Release (вдвое быстрее; отладочная сборка
# kika с /MDd работает с ним через границу DLL — объекты среды выполнения C++ её не пересекают).
if(PORT STREQUAL "opencascade")
  set(VCPKG_LIBRARY_LINKAGE dynamic)
  set(VCPKG_BUILD_TYPE release)
endif()
