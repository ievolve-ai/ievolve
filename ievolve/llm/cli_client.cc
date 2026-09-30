#include "ievolve/llm/cli_client.h"

#include "ievolve/llm/claude_code.h"
#include "ievolve/llm/cli_common.h"
#include "ievolve/llm/codex.h"

namespace ievolve {
absl::StatusOr<std::shared_ptr<LLMInterface>> CreateLLM(const LLMModelConfig& config, const CLIOptions& options) {
  const auto provider = config.provider.value_or("");
  if (provider != "claude_code" && provider != "codex") {
    return absl::InvalidArgumentError("llm.provider must be claude_code or codex");
  }

  const auto status = llm_internal::ValidateModel(config, options, provider);
  if (!status.ok()) return status;

  if (provider == "claude_code") return std::shared_ptr<LLMInterface>(std::make_shared<ClaudeCodeLLM>(config, options));

  return std::shared_ptr<LLMInterface>(std::make_shared<CodexCLILLM>(config, options));
}
}  // namespace ievolve
