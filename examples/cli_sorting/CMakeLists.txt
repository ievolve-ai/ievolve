add_executable(ievolve_cli_sorting_evaluator evaluator.cpp)
target_link_libraries(ievolve_cli_sorting_evaluator PRIVATE ievolve::process nlohmann_json::nlohmann_json)
set_target_properties(ievolve_cli_sorting_evaluator PROPERTIES
  OUTPUT_NAME evaluator RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
