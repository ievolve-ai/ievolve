add_library(ievolve_config_types INTERFACE)
add_library(ievolve::config_types ALIAS ievolve_config_types)

target_compile_features(ievolve_config_types INTERFACE cxx_std_17)
target_include_directories(ievolve_config_types INTERFACE "${PROJECT_SOURCE_DIR}")

target_link_libraries(ievolve_config_types INTERFACE nlohmann_json::nlohmann_json)

add_library(ievolve_config STATIC config.cc)
add_library(ievolve::config ALIAS ievolve_config)

target_link_libraries(ievolve_config PUBLIC ievolve::config_types absl::statusor
  nlohmann_json::nlohmann_json PRIVATE ievolve::utils_yaml)

if(MSVC)
  target_compile_options(ievolve_config PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_config PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IEVOLVE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ievolve_config PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(ievolve_config PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  add_executable(config_test config_test.cc)
  target_link_libraries(config_test PRIVATE ievolve::config ievolve::prompt GTest::gtest_main)
  target_compile_definitions(config_test PRIVATE
    IEVOLVE_CONFIG_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/utest_data")

  gtest_discover_tests(config_test DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "config" ENVIRONMENT "IEVOLVE_CONFIG_TEST_KEY=fixture-key")
endif()
