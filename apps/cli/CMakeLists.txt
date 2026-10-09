# Консольная программа kika: расчёт по файлам, без интерфейса.

find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(kika main.cpp)
target_link_libraries(kika PRIVATE kika::core CLI11::CLI11 nlohmann_json::nlohmann_json)
kika_set_compile_options(kika)
