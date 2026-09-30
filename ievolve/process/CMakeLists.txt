find_package(Threads REQUIRED)

add_library(ievolve_process STATIC process.cc)
add_library(ievolve::process ALIAS ievolve_process)

target_compile_features(ievolve_process PUBLIC cxx_std_17)
target_include_directories(ievolve_process PUBLIC "${PROJECT_SOURCE_DIR}")

target_link_libraries(ievolve_process PUBLIC absl::statusor
  PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(ievolve_process PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_process PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IEVOLVE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ievolve_process PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(ievolve_process PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  add_executable(ievolve_process_test_child utest_helpers/process_test_child.cc)
  target_link_libraries(ievolve_process_test_child PRIVATE Threads::Threads)

  add_executable(process_test process_test.cc)
  target_link_libraries(process_test PRIVATE ievolve::process GTest::gtest_main Threads::Threads)

  gtest_discover_tests(process_test DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "process")

  target_compile_definitions(process_test PRIVATE
    IEVOLVE_PROCESS_TEST_CHILD_PATH="$<TARGET_FILE:ievolve_process_test_child>")
  add_dependencies(process_test ievolve_process_test_child)
endif()
