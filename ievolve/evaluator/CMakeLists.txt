set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/python_worker.py")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/python_worker.py" PYTHON_EVALUATOR_WORKER)
configure_file(python_worker.inc.in "${CMAKE_CURRENT_BINARY_DIR}/python_worker.inc" @ONLY)

add_library(ievolve_evaluator STATIC evaluation_result.cc python_evaluator.cc evaluator.cc)
add_library(ievolve::evaluator ALIAS ievolve_evaluator)

target_compile_features(ievolve_evaluator PUBLIC cxx_std_17)
target_include_directories(ievolve_evaluator PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(ievolve_evaluator PUBLIC ievolve::program ievolve::config_types
  ievolve::process ievolve::prompt ievolve::llm ievolve::utils PRIVATE absl::strings Threads::Threads)

if(MSVC)
  target_compile_options(ievolve_evaluator PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_evaluator PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
  find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

  foreach(unit evaluation_result python_evaluator evaluator)
    add_executable(${unit}_test ${unit}_test.cc)
    target_link_libraries(${unit}_test PRIVATE ievolve::evaluator GTest::gtest_main)
    target_compile_definitions(${unit}_test PRIVATE
      IEVOLVE_EVALUATOR_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/utest_data"
      IEVOLVE_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}")

    gtest_discover_tests(${unit}_test DISCOVERY_MODE PRE_TEST
      PROPERTIES LABELS "evaluator" ENVIRONMENT "ENABLE_ARTIFACTS=true")
  endforeach()

  target_link_libraries(evaluation_result_test PRIVATE ievolve::database)
endif()
