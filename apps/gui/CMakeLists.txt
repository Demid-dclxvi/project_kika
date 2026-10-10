# Окно kika-gui: Qt 6 Widgets и свой 3D-вид на OpenGL 2 (работает и на программном рендеринге).
# Qt подключается динамически (LGPL-3.0): библиотеки Qt лежат рядом с kika-gui.exe отдельными
# файлами, их можно заменить своими. Сигналы — лямбдами, moc не нужен.
# Логика окна без Qt — в src/app (kika::app), она проверяется тестами.

if(DEFINED ENV{QT_ROOT_DIR})
  list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_ROOT_DIR}")
endif()
find_package(Qt6 6.4 COMPONENTS Widgets OpenGLWidgets)
if(NOT Qt6_FOUND)
  message(FATAL_ERROR
    "Для окна (KIKA_BUILD_GUI) нужен Qt 6.4 или новее с модулями Widgets и OpenGLWidgets. "
    "Укажите папку Qt в переменной окружения QT_ROOT_DIR или в CMAKE_PREFIX_PATH, "
    "например C:/Qt/6.8.3/msvc2022_64. Без окна: -DKIKA_BUILD_GUI=OFF.")
endif()

add_executable(kika-gui WIN32 main.cpp main_window.cpp viewer.cpp)
target_link_libraries(kika-gui PRIVATE kika::app kika::report Qt6::Widgets Qt6::OpenGLWidgets)
# пример (кронштейн) ищется рядом с программой, а при запуске из папки сборки — в prototype/examples
target_compile_definitions(kika-gui PRIVATE KIKA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
kika_set_compile_options(kika-gui)
