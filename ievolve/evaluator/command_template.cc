#include "ievolve/evaluator/command_template.h"

#include <string_view>

namespace ievolve::evaluator {
namespace {
absl::StatusOr<std::string> ExpandArgument(std::string_view text, const CommandContext& context) {
  if (text.find('\0') != std::string_view::npos) return absl::InvalidArgumentError("Command argument contains NUL");
  std::string result;
  for (std::size_t i = 0; i < text.size();) {
    const char ch = text[i++];
    if (ch != '{' && ch != '}') {
      result += ch;
      continue;
    }
    if (i < text.size() && text[i] == ch) {
      result += ch;
      ++i;
      continue;
    }
    if (ch == '}') return absl::InvalidArgumentError("Unmatched command template brace");
    const auto end = text.find('}', i);
    if (end == std::string_view::npos) return absl::InvalidArgumentError("Unmatched command template brace");
    const auto token = text.substr(i, end - i);
    if (token == "source") {
      if (!context.source) return absl::InvalidArgumentError("Command references {source} without a source file");
      result += context.source->string();
    } else if (token == "workspace") {
      result += context.workspace.string();
    } else if (token == "base_dir") {
      result += context.base_directory.string();
    } else {
      return absl::InvalidArgumentError("Unknown command template placeholder");
    }
    i = end + 1;
  }
  if (result.find('\0') != std::string::npos) return absl::InvalidArgumentError("Expanded argument contains NUL");
  return result;
}
}  // namespace

absl::Status ValidateCommandTemplate(const std::vector<std::string>& argv, bool source_available) {
  CommandContext context{"/base", "/workspace", std::nullopt};
  if (source_available) context.source = "/source";
  return ExpandCommand(argv, context).status();
}

absl::StatusOr<std::vector<std::string>> ExpandCommand(const std::vector<std::string>& argv,
                                                       const CommandContext& context) {
  if (argv.empty() || argv.front().empty()) return absl::InvalidArgumentError("Command argv[0] must be nonempty");
  std::vector<std::string> result;
  for (const auto& argument : argv) {
    auto expanded = ExpandArgument(argument, context);
    if (!expanded.ok()) return expanded.status();
    result.push_back(std::move(*expanded));
  }
  if (result.front().empty()) return absl::InvalidArgumentError("Expanded command argv[0] must be nonempty");
  const std::filesystem::path executable(result.front());
  if (executable.is_relative() && executable.has_parent_path()) {
    result.front() = (context.base_directory / executable).lexically_normal().string();
  }
  return result;
}
}  // namespace ievolve::evaluator
