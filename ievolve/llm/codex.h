#ifndef IEVOLVE_LLM_CODEX_H_
#define IEVOLVE_LLM_CODEX_H_

#include "ievolve/llm/cli_client.h"

namespace ievolve {

class CodexCLILLM final : public LLMInterface {
 public:
  explicit CodexCLILLM(LLMModelConfig config, CLIOptions options = {});
  using LLMInterface::Generate;
  absl::StatusOr<LLMResponse> Generate(const LLMRequest& request) const override;

 private:
  LLMModelConfig config_;
  CLIOptions options_;
};

}  // namespace ievolve
#endif  // IEVOLVE_LLM_CODEX_H_
