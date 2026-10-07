#include "ievolve/evaluator/workspace.h"

#include <algorithm>
#include <cstdlib>

#include "absl/strings/ascii.h"
#include "ievolve/utils/file.h"

namespace ievolve::evaluator {
absl::StatusOr<std::unique_ptr<EvaluationWorkspace>> EvaluationWorkspace::Create() {
#ifdef _WIN32
  return absl::UnimplementedError("Evaluation workspaces require POSIX");
#else
  std::error_code error;
  const auto root = std::filesystem::temp_directory_path(error);
  if (error) return absl::UnavailableError("Cannot locate temporary directory");
  std::string pattern = (root / "ievolve-evaluation-XXXXXX").string();
  if (!mkdtemp(pattern.data())) return absl::UnavailableError("Cannot create evaluation workspace");
  return std::unique_ptr<EvaluationWorkspace>(new EvaluationWorkspace(pattern));
#endif
}

EvaluationWorkspace::~EvaluationWorkspace() {
  std::error_code ignored;
  std::filesystem::remove_all(path_, ignored);
}

absl::StatusOr<std::filesystem::path> EvaluationWorkspace::WriteSource(std::string_view code, std::string_view suffix) {
  if (suffix.size() < 2 || suffix.size() > 32 || suffix.front() != '.' ||
      !std::all_of(suffix.begin() + 1, suffix.end(),
                   [](unsigned char c) { return absl::ascii_isalnum(c) || c == '_'; })) {
    return absl::InvalidArgumentError("Invalid candidate source suffix");
  }
  const auto source = path_ / ("candidate" + std::string(suffix));
  auto status = utils::WriteFile(source, code);
  if (!status.ok()) return absl::UnavailableError("Cannot write candidate source: " + std::string(status.message()));
  return source;
}
}  // namespace ievolve::evaluator
