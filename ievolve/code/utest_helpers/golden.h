#ifndef IEVOLVE_CODE_UTEST_HELPERS_GOLDEN_H_
#define IEVOLVE_CODE_UTEST_HELPERS_GOLDEN_H_

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ievolve/code/code_parser.h"
#include "nlohmann/json.hpp"

namespace ievolve::code_test {
using Json = nlohmann::json;

inline std::vector<Json> Cases(const std::vector<std::string>& operations) {
  std::ifstream input(std::string(IEVOLVE_CODE_TEST_DATA_DIR) + "/python_golden.json");
  if (!input) throw std::runtime_error("Cannot open code golden fixtures");

  Json fixtures;
  input >> fixtures;

  std::vector<Json> result;
  for (const auto& item : fixtures.at("cases")) {
    for (const auto& operation : operations) {
      if (item.at("operation") == operation) result.push_back(item);
    }
  }

  if (result.empty()) throw std::runtime_error("No matching code golden fixtures");

  return result;
}

inline std::vector<DiffBlock> Blocks(const Json& value) {
  std::vector<DiffBlock> result;
  for (const auto& pair : value) result.push_back({pair.at(0), pair.at(1)});
  return result;
}

inline Json Pairs(const std::vector<DiffBlock>& blocks) {
  Json result = Json::array();
  for (const auto& block : blocks) result.push_back({block.search, block.replacement});

  return result;
}
}  // namespace ievolve::code_test

#endif  // IEVOLVE_CODE_UTEST_HELPERS_GOLDEN_H_
