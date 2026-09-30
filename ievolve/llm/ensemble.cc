#include "ievolve/llm/ensemble.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numeric>
#include <system_error>
#include <thread>
#include <utility>

#include "absl/status/status.h"
#include "ievolve/llm/cli_client.h"
#include "ievolve/utils/threads.h"

namespace ievolve {
namespace {

absl::StatusOr<LLMResponse> CallClient(const LLMInterface& client, const LLMRequest& request) {
  try {
    return client.Generate(request);
  } catch (...) {
    // An injected client may throw; never include its exception text, which
    // could contain prompts or credentials, in the diagnostic.
    return absl::InternalError("LLM client threw an exception");
  }
}

}  // namespace

LLMEnsemble::LLMEnsemble(std::vector<std::shared_ptr<LLMInterface>> models, std::vector<std::size_t> sampled_models,
                         const std::vector<double>& weights, std::uint64_t seed, std::size_t max_concurrency)
    : models_(std::move(models)),
      sampled_models_(std::move(sampled_models)),
      max_concurrency_(max_concurrency),
      random_(seed),
      distribution_(weights.begin(), weights.end()) {}

absl::StatusOr<std::unique_ptr<LLMEnsemble>> LLMEnsemble::Create(const std::vector<LLMModelConfig>& configs,
                                                                 LLMFactory factory, std::size_t max_concurrency) {
  if (configs.empty()) {
    return absl::InvalidArgumentError("LLM ensemble requires at least one model");
  }
  if (max_concurrency == 0) {
    return absl::InvalidArgumentError("LLM ensemble concurrency must be positive");
  }

  double max_weight = 0;
  for (const auto& config : configs) {
    if (!std::isfinite(config.weight) || config.weight < 0) {
      return absl::InvalidArgumentError("LLM ensemble weights must be finite and nonnegative");
    }
    max_weight = std::max(max_weight, config.weight);
  }
  if (max_weight == 0) {
    return absl::InvalidArgumentError("LLM ensemble requires at least one positive weight");
  }

  if (!factory) {
    factory = [](const LLMModelConfig& config) { return CreateLLM(config); };
  }

  try {
    std::vector<std::shared_ptr<LLMInterface>> models;
    std::vector<std::size_t> sampled_models;
    std::vector<double> weights;
    models.reserve(configs.size());

    for (std::size_t i = 0; i < configs.size(); ++i) {
      auto model = factory(configs[i]);
      if (!model.ok()) return model.status();
      if (*model == nullptr) {
        return absl::InvalidArgumentError("LLM factory returned a null client");
      }

      models.push_back(std::move(*model));
      if (configs[i].weight > 0) {
        sampled_models.push_back(i);
        // Scaling first avoids an infinite sum for large finite weights.
        // Zero-weight models are excluded entirely from the distribution.
        weights.push_back(configs[i].weight / max_weight);
      }
    }

    const std::uint64_t seed = configs.front().random_seed ? static_cast<std::uint64_t>(*configs.front().random_seed)
                                                           : static_cast<std::uint64_t>(std::random_device{}());
    return std::unique_ptr<LLMEnsemble>(
        new LLMEnsemble(std::move(models), std::move(sampled_models), weights, seed, max_concurrency));
  } catch (...) {
    return absl::InternalError("LLM ensemble creation threw an exception");
  }
}

std::vector<std::size_t> LLMEnsemble::SelectModels(std::size_t count) const {
  std::vector<std::size_t> selected;
  selected.reserve(count);

  std::lock_guard<std::mutex> lock(random_mutex_);
  for (std::size_t i = 0; i < count; ++i) {
    selected.push_back(sampled_models_[distribution_(random_)]);
  }

  return selected;
}

absl::StatusOr<LLMResponse> LLMEnsemble::Generate(const LLMRequest& request) const {
  const auto selected = SelectModels(1);
  return CallClient(*models_[selected.front()], request);
}

absl::StatusOr<std::vector<LLMResponse>> LLMEnsemble::GenerateMultiple(const LLMRequest& request,
                                                                       std::size_t count) const {
  return GenerateBatch(std::vector<const LLMRequest*>(count, &request), SelectModels(count));
}

absl::StatusOr<std::vector<LLMResponse>> LLMEnsemble::ParallelGenerate(const std::vector<LLMRequest>& requests) const {
  std::vector<const LLMRequest*> inputs;
  inputs.reserve(requests.size());
  for (const auto& request : requests) inputs.push_back(&request);

  return GenerateBatch(inputs, SelectModels(requests.size()));
}

absl::StatusOr<std::vector<LLMResponse>> LLMEnsemble::GenerateAll(const LLMRequest& request) const {
  std::vector<std::size_t> selected(models_.size());
  std::iota(selected.begin(), selected.end(), std::size_t{0});

  return GenerateBatch(std::vector<const LLMRequest*>(models_.size(), &request), selected);
}

absl::StatusOr<std::vector<LLMResponse>> LLMEnsemble::GenerateBatch(
    const std::vector<const LLMRequest*>& requests, const std::vector<std::size_t>& selected_models) const {
  if (requests.empty()) return std::vector<LLMResponse>{};

  std::vector<absl::StatusOr<LLMResponse>> results(requests.size());
  std::atomic<std::size_t> next{0};
  const auto run = [&] {
    while (true) {
      const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
      if (index >= requests.size()) return;

      results[index] = CallClient(*models_[selected_models[index]], *requests[index]);
    }
  };

  const std::size_t worker_count = std::min(max_concurrency_, requests.size());
  {
    std::vector<std::thread> threads;
    threads.reserve(worker_count - 1);
    utils::JoinThreads join(threads);

    for (std::size_t i = 1; i < worker_count; ++i) {
      try {
        threads.emplace_back(run);
      } catch (const std::system_error&) {
        // Resource pressure may prevent more threads. The caller and any
        // existing workers can still complete the batch at lower concurrency.
        break;
      }
    }

    run();
  }

  std::vector<LLMResponse> responses;
  responses.reserve(results.size());
  for (auto& result : results) {
    if (!result.ok()) return result.status();
    responses.push_back(std::move(*result));
  }

  return responses;
}

}  // namespace ievolve
