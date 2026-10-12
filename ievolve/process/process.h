#ifndef IEVOLVE_PROCESS_PROCESS_H_
#define IEVOLVE_PROCESS_PROCESS_H_

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace ievolve::process {

struct ProcessRequest {
  std::vector<std::string> argv;
  std::string stdin_text;
  std::optional<std::filesystem::path> working_directory;
  std::chrono::milliseconds timeout{60000};
  std::size_t max_output_bytes = 8 * 1024 * 1024;
  // Inherit the caller's group for nested CLI calls. Errors kill only this child.
  bool inherit_process_group = false;
  // Also terminate remaining group members after the direct child exits.
  // Requires an owned group (inherit_process_group must be false).
  bool cleanup_group_on_exit = false;
};

struct ProcessResult {
  int exit_code = 0;
  std::string stdout_text;
  std::string stderr_text;
};

// Safe to call concurrently. Uses argv directly, not a shell. Timeout/error
// terminates the owned group (or inherited direct child) and reaps the child.
// Output limit is combined. Deliberately detached descendants are not managed.
absl::StatusOr<ProcessResult> RunProcess(const ProcessRequest& request);
using ProcessRunner = std::function<absl::StatusOr<ProcessResult>(const ProcessRequest&)>;

}  // namespace ievolve::process
#endif  // IEVOLVE_PROCESS_PROCESS_H_
