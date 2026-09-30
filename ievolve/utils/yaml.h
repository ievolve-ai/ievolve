#ifndef IEVOLVE_UTILS_YAML_H_
#define IEVOLVE_UTILS_YAML_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

namespace ievolve::utils {

// YAML text and file conversion to and from ordered JSON. Normalization,
// validation and schema knowledge stay with the caller; this layer only moves
// between the two representations and reports where a document went wrong.
//
// Built as ievolve::utils_yaml rather than part of ievolve::utils, because
// yaml-cpp is a real static library and every component links ievolve::utils.
absl::StatusOr<nlohmann::ordered_json> ParseYaml(std::string_view yaml);
absl::StatusOr<nlohmann::ordered_json> ReadYaml(const std::filesystem::path& path);
absl::StatusOr<std::string> EmitYaml(const nlohmann::ordered_json& input);
absl::Status WriteYaml(std::string_view yaml, const std::filesystem::path& path);

}  // namespace ievolve::utils

#endif  // IEVOLVE_UTILS_YAML_H_
