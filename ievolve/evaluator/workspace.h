#ifndef IEVOLVE_EVALUATOR_WORKSPACE_H_
#define IEVOLVE_EVALUATOR_WORKSPACE_H_

#include <filesystem>
#include <memory>
#include <string_view>

#include "absl/status/statusor.h"

namespace ievolve::evaluator {
class EvaluationWorkspace {
 public:
  static absl::StatusOr<std::unique_ptr<EvaluationWorkspace>> Create();
  ~EvaluationWorkspace();
  EvaluationWorkspace(const EvaluationWorkspace&) = delete;
  EvaluationWorkspace& operator=(const EvaluationWorkspace&) = delete;
  const std::filesystem::path& path() const { return path_; }
  absl::StatusOr<std::filesystem::path> WriteSource(std::string_view code, std::string_view suffix);

 private:
  explicit EvaluationWorkspace(std::filesystem::path path) : path_(std::move(path)) {}
  std::filesystem::path path_;
};
}  // namespace ievolve::evaluator
#endif  // IEVOLVE_EVALUATOR_WORKSPACE_H_
