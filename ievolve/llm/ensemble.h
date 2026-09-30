#ifndef IEVOLVE_LLM_ENSEMBLE_H_
#define IEVOLVE_LLM_ENSEMBLE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

#include "ievolve/llm/base.h"

namespace ievolve {

class LLMEnsemble final : public LLMInterface {
 public:
  // All models are constructed, including zero-weight models. The first
  // model's seed controls sampling; its sequence need not match Python's RNG.
  static absl::StatusOr<std::unique_ptr<LLMEnsemble>> Create(const std::vector<LLMModelConfig>& configs,
                                                             LLMFactory factory = {}, std::size_t max_concurrency = 4);

  using LLMInterface::Generate;
  absl::StatusOr<LLMResponse> Generate(const LLMRequest& request) const override;
  // Batch calls select models in input order before starting workers. Each
  // batch runs at most max_concurrency calls at once and returns in input
  // order. All calls finish before the first input-order error is returned.
  absl::StatusOr<std::vector<LLMResponse>> GenerateMultiple(const LLMRequest& request, std::size_t count) const;
  absl::StatusOr<std::vector<LLMResponse>> ParallelGenerate(const std::vector<LLMRequest>& requests) const;
  // Calls every model in configuration order, including zero-weight models.
  // Does not consume random selections.
  absl::StatusOr<std::vector<LLMResponse>> GenerateAll(const LLMRequest& request) const;

 private:
  LLMEnsemble(std::vector<std::shared_ptr<LLMInterface>> models, std::vector<std::size_t> sampled_models,
              const std::vector<double>& weights, std::uint64_t seed, std::size_t max_concurrency);

  std::vector<std::size_t> SelectModels(std::size_t count) const;
  absl::StatusOr<std::vector<LLMResponse>> GenerateBatch(const std::vector<const LLMRequest*>& requests,
                                                         const std::vector<std::size_t>& selected_models) const;

  const std::vector<std::shared_ptr<LLMInterface>> models_;
  const std::vector<std::size_t> sampled_models_;
  const std::size_t max_concurrency_;
  mutable std::mutex random_mutex_;
  mutable std::mt19937_64 random_;
  mutable std::discrete_distribution<std::size_t> distribution_;
};

}  // namespace ievolve

#endif  // IEVOLVE_LLM_ENSEMBLE_H_
