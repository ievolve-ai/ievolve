#include "ievolve/llm/no_generation.h"

namespace ievolve {

absl::StatusOr<LLMResponse> NoGeneration::Generate(const LLMRequest&) const {
  return absl::FailedPreconditionError("Generation disabled for zero iterations");
}

}  // namespace ievolve
