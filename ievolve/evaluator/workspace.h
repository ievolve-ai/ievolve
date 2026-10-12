#ifndef IEVOLVE_EVALUATOR_WORKSPACE_H_
#define IEVOLVE_EVALUATOR_WORKSPACE_H_

#include <filesystem>
#include <memory>

#include "absl/status/statusor.h"
#include "ievolve/program/source_files.h"

namespace ievolve::evaluator {
class EvaluationWorkspace {
 public:
  static absl::StatusOr<std::unique_ptr<EvaluationWorkspace>> Create();
  ~EvaluationWorkspace();
  EvaluationWorkspace(const EvaluationWorkspace&) = delete;
  EvaluationWorkspace& operator=(const EvaluationWorkspace&) = delete;
  const std::filesystem::path& path() const { return path_; }
  // Copies project_dir into <workspace>/project, skipping .git and copying
  // symlinks as links, then writes files over the copy; returns that root.
  // An empty project_dir yields a project holding only files.
  // Once per workspace. A file path must not pass through a symlink, so a
  // candidate cannot write outside the copy.
  //   InvalidArgument    invalid file paths or a path through a symlink
  //   FailedPrecondition project_dir is not a directory, or already called
  //   Unavailable        copying or writing failed
  absl::StatusOr<std::filesystem::path> MaterializeProject(const std::filesystem::path& project_dir,
                                                           const SourceFiles& files);

 private:
  explicit EvaluationWorkspace(std::filesystem::path path) : path_(std::move(path)) {}
  std::filesystem::path path_;
};
}  // namespace ievolve::evaluator
#endif  // IEVOLVE_EVALUATOR_WORKSPACE_H_
