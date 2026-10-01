add_library(ievolve_database STATIC program_database.cc program_store.cc population.cc database_codec.cc selection.cc feature_mapper.cc checkpoint.cc artifact_store.cc)
add_library(ievolve::database ALIAS ievolve_database)

target_link_libraries(ievolve_database PUBLIC ievolve::program ievolve::config_types ievolve::code ievolve::utils
  PRIVATE absl::strings)

if(MSVC)
  target_compile_options(ievolve_database PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_database PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
  foreach(unit population program_store database_codec selection feature_mapper artifact_store checkpoint)
    add_executable(${unit}_test ${unit}_test.cc)
    target_link_libraries(${unit}_test PRIVATE ievolve::database GTest::gtest_main)
    target_compile_definitions(${unit}_test PRIVATE
      IEVOLVE_DATABASE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/utest_data")

    gtest_discover_tests(${unit}_test DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES LABELS "database")
  endforeach()

  add_executable(program_database_test program_database_test.cc)
  target_link_libraries(program_database_test PRIVATE ievolve::database ievolve::prompt GTest::gtest_main)
  target_compile_definitions(program_database_test PRIVATE
    IEVOLVE_DATABASE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/utest_data")

  gtest_discover_tests(program_database_test DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES LABELS "database")
endif()
