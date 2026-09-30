#ifndef IEVOLVE_LLM_CLI_CLIENT_H_
#define IEVOLVE_LLM_CLI_CLIENT_H_

#include "ievolve/llm/base.h"
#include "ievolve/llm/process.h"

namespace ievolve {

struct CLIOptions {
  std::string claude_executable = "claude";
  std::string codex_executable = "codex";
  // Missing uses a fresh empty temporary directory per call.
  std::optional<std::filesystem::path> working_directory;
  std::size_t max_output_bytes = 8 * 1024 * 1024;
  // Injected runners must support concurrent calls.
  process::ProcessRunner runner = process::RunProcess;
};

// Only explicit claude_code/codex providers are accepted.
absl::StatusOr<std::shared_ptr<LLMInterface>> CreateLLM(const LLMModelConfig& config, const CLIOptions& options = {});

}  // namespace ievolve
#endif  // IEVOLVE_LLM_CLI_CLIENT_H_
