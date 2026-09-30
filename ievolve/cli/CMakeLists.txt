find_package(Threads REQUIRED)

add_library(ievolve_cli STATIC cli.cc stop.cc)
add_library(ievolve::cli ALIAS ievolve_cli)

target_link_libraries(ievolve_cli PUBLIC ievolve::controller PRIVATE Threads::Threads)
target_compile_definitions(ievolve_cli PRIVATE IEVOLVE_VERSION="${PROJECT_VERSION}")

add_executable(ievolve_command main.cc)
set_target_properties(ievolve_command PROPERTIES OUTPUT_NAME ievolve
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(ievolve_command PRIVATE ievolve::cli)

if(MSVC)
  target_compile_options(ievolve_cli PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
  find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

  add_executable(cli_test cli_test.cc)
  target_link_libraries(cli_test PRIVATE ievolve::cli GTest::gtest_main)
  target_compile_definitions(cli_test PRIVATE
    IEVOLVE_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
    IEVOLVE_COMMAND_PATH="$<TARGET_FILE:ievolve_command>")
  add_dependencies(cli_test ievolve_command)

  gtest_discover_tests(cli_test DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "cli" ENVIRONMENT "ENABLE_ARTIFACTS=true")
endif()
