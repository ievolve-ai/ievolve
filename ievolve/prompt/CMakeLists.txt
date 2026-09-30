set(prompt_units metrics template_manager artifact_renderer prompt_sampler prompt_program)

# Resources are compiled into the library; custom directories remain runtime
# overrides. CONFIGURE_DEPENDS also notices newly added template files.
file(GLOB template_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/defaults/*.txt")
set(template_entries "")

foreach(template_file IN LISTS template_files)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${template_file}")
  get_filename_component(template_name "${template_file}" NAME_WE)
  file(READ "${template_file}" template_content)

  string(APPEND template_entries
    "{\"${template_name}\", R\"ievolve(${template_content})ievolve\"},\n")
endforeach()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/defaults/fragments.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/defaults/fragments.json" fragment_content)

configure_file(embedded_templates.inc.in generated/embedded_templates.inc @ONLY)

add_library(ievolve_prompt STATIC)
add_library(ievolve::prompt ALIAS ievolve_prompt)

foreach(unit IN LISTS prompt_units)
  target_sources(ievolve_prompt PRIVATE "${unit}.cc")
endforeach()

target_compile_features(ievolve_prompt PUBLIC cxx_std_17)
target_include_directories(ievolve_prompt PUBLIC "${PROJECT_SOURCE_DIR}"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(ievolve_prompt PUBLIC ievolve::program ievolve::config_types ievolve::utils absl::statusor nlohmann_json::nlohmann_json
  PRIVATE absl::strings)

if(MSVC)
  target_compile_options(ievolve_prompt PRIVATE /W4 /utf-8)
else()
  target_compile_options(ievolve_prompt PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IEVOLVE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ievolve_prompt PUBLIC -fsanitize=address,undefined
    -fno-omit-frame-pointer)
  target_link_options(ievolve_prompt PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  foreach(unit IN LISTS prompt_units)
    add_executable(${unit}_test "${unit}_test.cc")
    target_link_libraries(${unit}_test PRIVATE ievolve::prompt GTest::gtest_main)
    target_compile_definitions(${unit}_test PRIVATE
      IEVOLVE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/utest_data")

    gtest_discover_tests(${unit}_test DISCOVERY_MODE PRE_TEST
      PROPERTIES LABELS "prompt")
  endforeach()
endif()
