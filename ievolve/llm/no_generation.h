#ifndef IEVOLVE_LLM_NO_GENERATION_H_
#define IEVOLVE_LLM_NO_GENERATION_H_

#include "ievolve/llm/base.h"

namespace ievolve {

// Disables generation without constructing or launching a model client.
class NoGeneration final : public LLMInterface {
 public:
  using LLMInterface::Generate;
  absl::StatusOr<LLMResponse> Generate(const LLMRequest& request) const override;
};

}  // namespace ievolve
#endif  // IEVOLVE_LLM_NO_GENERATION_H_
