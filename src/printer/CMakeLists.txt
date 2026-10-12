# Принтер по сети: свой HTTP-клиент на сокетах и Moonraker (Klipper). Без сторонних библиотек.

find_package(Threads REQUIRED)

add_library(kika_printer src/http.cpp src/moonraker.cpp)
add_library(kika::printer ALIAS kika_printer)
target_include_directories(kika_printer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kika_printer PUBLIC kika::core PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(kika_printer PRIVATE ws2_32)
endif()
kika_set_compile_options(kika_printer)
