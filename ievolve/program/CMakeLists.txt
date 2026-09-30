add_library(ievolve_program STATIC program.cc metrics.cc artifact.cc)
add_library(ievolve::program ALIAS ievolve_program)

target_compile_features(ievolve_program PUBLIC cxx_std_17)
target_include_directories(ievolve_program PUBLIC "${PROJECT_SOURCE_DIR}")

target_link_libraries(ievolve_program PUBLIC absl::statusor nlohmann_json::nlohmann_json
  PRIVATE absl::strings)

if(MSVC)
  target_compile_options(ievolve_program PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_program PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IEVOLVE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ievolve_program PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(ievolve_program PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  add_executable(program_test program_test.cc)
  target_link_libraries(program_test PRIVATE ievolve::program GTest::gtest_main)
  target_compile_definitions(program_test PRIVATE
    IEVOLVE_PROGRAM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/utest_data")

  gtest_discover_tests(program_test DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES LABELS "program")
endif()
