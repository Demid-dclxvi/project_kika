find_package(Catch2 3 CONFIG REQUIRED)

add_executable(kika_tests
  analysis/test_analysis.cpp
  app/test_app.cpp
  analysis/test_job.cpp
  fem/test_fem.cpp
  gcode/test_roles.cpp
  gcode/test_parser.cpp
  gcode/test_examples.cpp
  material/test_material.cpp
  report/test_report.cpp
  util/test_json.cpp
  voxel/test_voxel.cpp
)
target_link_libraries(kika_tests PRIVATE kika::core kika::report kika::app Catch2::Catch2WithMain)
# Тесты читают эталонные G-code из папки прототипа
target_compile_definitions(kika_tests PRIVATE KIKA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
kika_set_compile_options(kika_tests)

# Одна запись CTest на все тесты: названия тестов русские, а автоматический поиск
# тестов Catch2 на Windows путается в кодировках. Подробности — в выводе kika_tests.
add_test(NAME kika_tests COMMAND kika_tests --order rand --warn NoAssertions)
