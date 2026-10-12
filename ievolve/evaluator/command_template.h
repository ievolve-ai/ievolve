#ifndef IEVOLVE_EVALUATOR_COMMAND_TEMPLATE_H_
#define IEVOLVE_EVALUATOR_COMMAND_TEMPLATE_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace ievolve::evaluator {
struct CommandContext {
  std::filesystem::path base_directory;
  std::filesystem::path workspace;
  std::optional<std::filesystem::path> source;
};

absl::Status ValidateCommandTemplate(const std::vector<std::string>& argv, bool source_available);
absl::StatusOr<std::vector<std::string>> ExpandCommand(const std::vector<std::string>& argv,
                                                       const CommandContext& context);
}  // namespace ievolve::evaluator
#endif  // IEVOLVE_EVALUATOR_COMMAND_TEMPLATE_H_
