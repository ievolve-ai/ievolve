#include "ievolve/llm/base.h"

namespace ievolve {
absl::StatusOr<LLMResponse> LLMInterface::Generate(std::string_view prompt, const GenerationOptions& options) const {
  return Generate(LLMRequest{std::nullopt, {{"user", std::string(prompt)}}, options});
}
absl::StatusOr<LLMResponse> LLMInterface::GenerateWithContext(std::string_view system_message,
                                                              const std::vector<LLMMessage>& messages,
                                                              const GenerationOptions& options) const {
  return Generate(LLMRequest{std::string(system_message), messages, options});
}
}  // namespace ievolve
