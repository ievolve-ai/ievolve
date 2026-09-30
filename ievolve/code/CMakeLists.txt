add_library(ievolve_code STATIC code_parser.cc code_distance.cc)
add_library(ievolve::code ALIAS ievolve_code)

target_compile_features(ievolve_code PUBLIC cxx_std_17)
target_include_directories(ievolve_code PUBLIC "${PROJECT_SOURCE_DIR}")

target_link_libraries(ievolve_code PUBLIC ievolve::utils absl::statusor)

if(MSVC)
  target_compile_options(ievolve_code PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_code PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IEVOLVE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ievolve_code PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(ievolve_code PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  foreach(unit code_parser code_distance)
    add_executable(${unit}_test ${unit}_test.cc)
    target_link_libraries(${unit}_test PRIVATE ievolve::code GTest::gtest_main
      nlohmann_json::nlohmann_json)
    target_compile_definitions(${unit}_test PRIVATE
      IEVOLVE_CODE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/utest_data")

    gtest_discover_tests(${unit}_test DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES
      PROPERTIES LABELS "code")
  endforeach()

  target_link_libraries(code_parser_test PRIVATE ievolve::config)
endif()
