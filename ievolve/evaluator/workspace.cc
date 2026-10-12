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
namespace {
namespace fs = std::filesystem;

absl::Status CopyProject(const fs::path& from, const fs::path& to) {
  std::error_code error;
  if (!fs::create_directory(to, error)) return absl::UnavailableError("Cannot create project copy");
  fs::recursive_directory_iterator it(from, error), end;
  if (error) return absl::UnavailableError("Cannot list project directory");
  for (; it != end; it.increment(error)) {
    if (error) return absl::UnavailableError("Cannot list project directory");
    const auto target = to / it->path().lexically_relative(from);
    const auto status = it->symlink_status(error);
    if (error) return absl::UnavailableError("Cannot inspect project entry");
    if (fs::is_symlink(status)) {
      fs::copy_symlink(it->path(), target, error);
    } else if (fs::is_directory(status)) {
      if (it->path().filename() == ".git") {
        it.disable_recursion_pending();
        continue;
      }
      fs::create_directory(target, error);
    } else if (fs::is_regular_file(status)) {
      fs::copy_file(it->path(), target, error);
    }
    if (error) return absl::UnavailableError("Cannot copy project entry");
  }
  if (error) return absl::UnavailableError("Cannot list project directory");
  return absl::OkStatus();
}

// Every existing prefix of path below root must be a real directory or file.
bool PassesThroughSymlink(const fs::path& root, const std::string& path) {
  auto current = root;
  for (const auto& part : fs::path(path)) {
    current /= part;
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(current, error))) return true;
  }
  return false;
}
}  // namespace

absl::StatusOr<std::filesystem::path> EvaluationWorkspace::MaterializeProject(const std::filesystem::path& project_dir,
                                                                              const SourceFiles& files) {
  auto status = ValidateSourceFiles(files);
  if (!status.ok()) return status;
  std::error_code error;
  if (!fs::is_directory(project_dir, error)) return absl::FailedPreconditionError("Project directory is missing");
  const auto root = path_ / "project";
  if (fs::exists(fs::symlink_status(root, error))) {
    return absl::FailedPreconditionError("Project is already materialized");
  }

  status = CopyProject(project_dir, root);
  if (!status.ok()) return status;
  for (const auto& [path, content] : files) {
    if (PassesThroughSymlink(root, path)) {
      return absl::InvalidArgumentError("Candidate file " + path + " passes through a symlink");
    }
    const auto target = root / path;
    fs::create_directories(target.parent_path(), error);
    if (error) return absl::UnavailableError("Cannot create directory for " + path);
    status = utils::WriteFile(target, content);
    if (!status.ok()) return absl::UnavailableError("Cannot write candidate file " + path);
  }
  return root;
}
}  // namespace ievolve::evaluator
