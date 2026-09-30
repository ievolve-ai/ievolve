# Shared helpers use the standard library and header-only dependencies.
# Platform threads support the logger; heavier dependencies such as yaml-cpp
# belong to separate targets. Component-specific helpers stay in their component.
find_package(Threads REQUIRED)

add_library(ievolve_utils STATIC log.cc text.cc)
add_library(ievolve::utils ALIAS ievolve_utils)

target_compile_features(ievolve_utils PUBLIC cxx_std_17)
target_include_directories(ievolve_utils PUBLIC "${PROJECT_SOURCE_DIR}")

target_link_libraries(ievolve_utils PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

if(MSVC)
  target_compile_options(ievolve_utils PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_utils PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IEVOLVE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ievolve_utils PUBLIC -fsanitize=address,undefined
    -fno-omit-frame-pointer)
  target_link_options(ievolve_utils PUBLIC -fsanitize=address,undefined)
endif()

# Separate target on purpose: yaml-cpp is a real static library, and every
# component links ievolve::utils. Keeping it apart means only the consumers that
# actually parse YAML pay for it.
add_library(ievolve_utils_yaml STATIC yaml.cc)
add_library(ievolve::utils_yaml ALIAS ievolve_utils_yaml)

target_compile_features(ievolve_utils_yaml PUBLIC cxx_std_17)
target_include_directories(ievolve_utils_yaml PUBLIC "${PROJECT_SOURCE_DIR}")

target_link_libraries(ievolve_utils_yaml PUBLIC absl::statusor
  nlohmann_json::nlohmann_json PRIVATE yaml-cpp::yaml-cpp)

if(MSVC)
  target_compile_options(ievolve_utils_yaml PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_utils_yaml PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IEVOLVE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ievolve_utils_yaml PUBLIC -fsanitize=address,undefined
    -fno-omit-frame-pointer)
  target_link_options(ievolve_utils_yaml PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  add_executable(log_test log_test.cc)
  target_link_libraries(log_test PRIVATE ievolve::utils GTest::gtest_main
    Threads::Threads)

  gtest_discover_tests(log_test DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "utils")

  add_executable(text_test text_test.cc)
  target_link_libraries(text_test PRIVATE ievolve::utils GTest::gtest_main)

  gtest_discover_tests(text_test DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "utils")

  # threads.h is header-only; the test creates the threads, so it links the
  # platform thread library itself, exactly as the other consumers do.
  add_executable(threads_test threads_test.cc)
  target_link_libraries(threads_test PRIVATE ievolve::utils GTest::gtest_main
    Threads::Threads)

  gtest_discover_tests(threads_test DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "utils")

  add_executable(yaml_test yaml_test.cc)
  target_link_libraries(yaml_test PRIVATE ievolve::utils_yaml GTest::gtest_main)

  gtest_discover_tests(yaml_test DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "utils")
endif()
