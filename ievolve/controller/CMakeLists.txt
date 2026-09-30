add_library(ievolve_controller STATIC iteration.cc controller.cc)
add_library(ievolve::controller ALIAS ievolve_controller)

target_link_libraries(ievolve_controller PUBLIC ievolve::config ievolve::code
  ievolve::evaluator ievolve::llm ievolve::prompt ievolve::database
  ievolve::utils)

if(MSVC)
  target_compile_options(ievolve_controller PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_controller PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
  find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

  foreach(unit iteration controller)
    add_executable(${unit}_test ${unit}_test.cc)
    target_link_libraries(${unit}_test PRIVATE ievolve::controller GTest::gtest_main)
    target_compile_definitions(${unit}_test PRIVATE
      IEVOLVE_CONTROLLER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/utest_data"
      IEVOLVE_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}")

    gtest_discover_tests(${unit}_test DISCOVERY_MODE PRE_TEST
      PROPERTIES LABELS "controller" ENVIRONMENT "ENABLE_ARTIFACTS=true")
  endforeach()
endif()
