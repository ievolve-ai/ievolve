find_package(Threads REQUIRED)

add_library(ievolve_llm STATIC base.cc cli_client.cc cli_common.cc claude_code.cc
  codex.cc ensemble.cc no_generation.cc)
add_library(ievolve::llm ALIAS ievolve_llm)

target_link_libraries(ievolve_llm PUBLIC ievolve::config_types ievolve::process ievolve::utils absl::statusor
  PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

if(MSVC)
  target_compile_options(ievolve_llm PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_llm PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IEVOLVE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ievolve_llm PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(ievolve_llm PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  add_executable(ievolve_llm_fake_cli utest_helpers/fake_cli.cc)
  target_link_libraries(ievolve_llm_fake_cli PRIVATE nlohmann_json::nlohmann_json)

  foreach(unit cli_client ensemble)
    add_executable(${unit}_test ${unit}_test.cc)
    target_link_libraries(${unit}_test PRIVATE ievolve::llm GTest::gtest_main Threads::Threads)

    gtest_discover_tests(${unit}_test DISCOVERY_MODE PRE_TEST
      PROPERTIES LABELS "llm")
  endforeach()

  target_link_libraries(cli_client_test PRIVATE ievolve::config ievolve::prompt)
  target_compile_definitions(cli_client_test PRIVATE
    IEVOLVE_LLM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/utest_data"
    IEVOLVE_FAKE_CLI_PATH="$<TARGET_FILE:ievolve_llm_fake_cli>")
  add_dependencies(cli_client_test ievolve_llm_fake_cli)
endif()
