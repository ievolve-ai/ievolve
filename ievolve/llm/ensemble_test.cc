#include "ievolve/llm/ensemble.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "gtest/gtest.h"

namespace ievolve {
namespace {

using Handler = std::function<absl::StatusOr<LLMResponse>(const LLMRequest& request)>;

class MemoryClient final : public LLMInterface {
 public:
  explicit MemoryClient(Handler handler) : handler_(std::move(handler)) {}
  absl::StatusOr<LLMResponse> Generate(const LLMRequest& request) const override { return handler_(request); }

 private:
  Handler handler_;
};

LLMModelConfig Model(std::string name, double weight = 1.0) {
  LLMModelConfig config;
  config.name = std::move(name);
  config.provider = "memory";
  config.weight = weight;
  config.random_seed = 1234;

  return config;
}

LLMRequest Request(std::string text) {
  LLMRequest request;
  request.messages.push_back({"user", std::move(text)});
  return request;
}

LLMResponse Echo(const LLMRequest& request, std::string model = "memory") {
  LLMResponse response;
  response.text = request.messages.empty() ? "" : request.messages.back().content;
  response.model = std::move(model);
  response.provider = "memory";
  response.usage = LLMUsage{};
  response.usage->input_tokens = response.text.size();
  response.usage->output_tokens = response.model.size();

  return response;
}

LLMFactory MemoryFactory() {
  return [](const LLMModelConfig& config) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    return std::make_shared<MemoryClient>(
        [name = config.name.value_or("default")](const LLMRequest& request) { return Echo(request, name); });
  };
}

LLMFactory FactoryWithHandler(Handler handler) {
  return [handler = std::move(handler)](const LLMModelConfig&) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    return std::make_shared<MemoryClient>(handler);
  };
}

TEST(LLMEnsembleTest, RejectsEmptyModels) {
  const auto ensemble = LLMEnsemble::Create({}, MemoryFactory());
  EXPECT_EQ(ensemble.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LLMEnsembleTest, RejectsInvalidWeightsBeforeCreatingClients) {
  int factory_calls = 0;
  const LLMFactory factory = [&factory_calls](const LLMModelConfig& config) {
    ++factory_calls;
    return MemoryFactory()(config);
  };

  for (double weight : {-1.0, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
    const auto ensemble = LLMEnsemble::Create({Model("valid"), Model("invalid", weight)}, factory);
    EXPECT_EQ(ensemble.status().code(), absl::StatusCode::kInvalidArgument);
  }

  EXPECT_EQ(factory_calls, 0);
}

TEST(LLMEnsembleTest, RejectsAllZeroWeights) {
  const auto ensemble = LLMEnsemble::Create({Model("first", 0.0), Model("second", -0.0)}, MemoryFactory());
  EXPECT_EQ(ensemble.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LLMEnsembleTest, RejectsZeroConcurrency) {
  const auto ensemble = LLMEnsemble::Create({Model("one")}, MemoryFactory(), 0);
  EXPECT_EQ(ensemble.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LLMEnsembleTest, PropagatesFactoryFailure) {
  const auto ensemble =
      LLMEnsemble::Create({Model("one")}, [](const LLMModelConfig&) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
        return absl::NotFoundError("missing model");
      });
  EXPECT_EQ(ensemble.status().code(), absl::StatusCode::kNotFound);
}

TEST(LLMEnsembleTest, RejectsNullFactoryResult) {
  const auto ensemble =
      LLMEnsemble::Create({Model("one")}, [](const LLMModelConfig&) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
        return std::shared_ptr<LLMInterface>{};
      });
  EXPECT_EQ(ensemble.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LLMEnsembleTest, ConvertsFactoryExceptionsToStatus) {
  for (bool standard_exception : {false, true}) {
    const auto ensemble = LLMEnsemble::Create(
        {Model("one")}, [standard_exception](const LLMModelConfig&) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
          if (standard_exception) throw std::runtime_error("sensitive detail");
          throw 7;
        });
    EXPECT_EQ(ensemble.status().code(), absl::StatusCode::kInternal);

    EXPECT_EQ(ensemble.status().message().find("sensitive detail"), std::string::npos);
  }
}

TEST(LLMEnsembleTest, DefaultFactoryRejectsUnsupportedProvider) {
  const auto ensemble = LLMEnsemble::Create({Model("one")});
  EXPECT_EQ(ensemble.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LLMEnsembleTest, PreservesFullRequestAndPerResponseUsage) {
  LLMRequest request;
  request.system_message = "system";
  request.messages = {{"user", "first"}, {"assistant", "reply"}, {"user", "followup"}};
  request.options.timeout = 19;
  request.options.retries = 2;
  request.options.retry_delay = 0;
  request.options.reasoning_effort = "high";

  auto ensemble = LLMEnsemble::Create(
      {Model("one")}, FactoryWithHandler([](const LLMRequest& received) -> absl::StatusOr<LLMResponse> {
        if (received.system_message != "system" || received.messages.size() != 3 ||
            received.messages[0].content != "first" || received.messages[1].role != "assistant" ||
            received.messages[1].content != "reply" || received.options.timeout != 19 ||
            received.options.retries != 2 || received.options.retry_delay != 0 ||
            received.options.reasoning_effort != "high") {
          return absl::InvalidArgumentError("request changed");
        }

        return Echo(received);
      }));
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  const auto result = (*ensemble)->Generate(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->text, "followup");
  EXPECT_EQ(result->provider, "memory");

  ASSERT_TRUE(result->usage.has_value());
  EXPECT_EQ(result->usage->input_tokens, 8);
}

TEST(LLMEnsembleTest, NeverSamplesZeroWeightsAndGenerateAllIncludesThem) {
  auto ensemble =
      LLMEnsemble::Create({Model("zero-first", 0), Model("chosen", 1), Model("zero-last", 0)}, MemoryFactory());
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  auto selected = (*ensemble)->GenerateMultiple(Request("prompt"), 128);
  ASSERT_TRUE(selected.ok()) << selected.status();

  ASSERT_EQ(selected->size(), 128);
  for (const auto& response : *selected) EXPECT_EQ(response.model, "chosen");

  const auto all = (*ensemble)->GenerateAll(Request("prompt"));
  ASSERT_TRUE(all.ok()) << all.status();

  ASSERT_EQ(all->size(), 3);
  EXPECT_EQ((*all)[0].model, "zero-first");
  EXPECT_EQ((*all)[1].model, "chosen");
  EXPECT_EQ((*all)[2].model, "zero-last");
}

TEST(LLMEnsembleTest, SamplesInProportionToWeights) {
  auto ensemble = LLMEnsemble::Create({Model("small", 1), Model("large", 9)}, MemoryFactory());
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  const auto results = (*ensemble)->GenerateMultiple(Request("prompt"), 2000);
  ASSERT_TRUE(results.ok()) << results.status();

  const auto large_count = std::count_if(results->begin(), results->end(),
                                         [](const LLMResponse& response) { return response.model == "large"; });
  EXPECT_GT(large_count, 1600);
  EXPECT_LT(large_count, 1950);
}

TEST(LLMEnsembleTest, NormalizesLargeFiniteWeightsWithoutOverflow) {
  auto ensemble = LLMEnsemble::Create({Model("first", 1e308), Model("second", 1e308)}, MemoryFactory());
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  const auto results = (*ensemble)->GenerateMultiple(Request("prompt"), 128);
  ASSERT_TRUE(results.ok()) << results.status();

  const auto first_count = std::count_if(results->begin(), results->end(),
                                         [](const LLMResponse& response) { return response.model == "first"; });
  EXPECT_GT(first_count, 20);
  EXPECT_LT(first_count, 108);
}

TEST(LLMEnsembleTest, AcceptsTinyPositiveWeights) {
  auto ensemble = LLMEnsemble::Create({Model("tiny", std::numeric_limits<double>::denorm_min()), Model("zero", 0)},
                                      MemoryFactory());
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  const auto response = (*ensemble)->Generate(Request("prompt"));
  ASSERT_TRUE(response.ok()) << response.status();

  EXPECT_EQ(response->model, "tiny");
}

TEST(LLMEnsembleTest, ReproducesSeededSelectionAcrossSingleAndBatchCalls) {
  std::vector<LLMModelConfig> configs = {Model("a", 2), Model("b", 3), Model("c", 5)};
  auto serial = LLMEnsemble::Create(configs, MemoryFactory());

  // Only the first model's seed controls the ensemble.
  configs[1].random_seed = 765;
  configs[2].random_seed.reset();
  auto repeated = LLMEnsemble::Create(configs, MemoryFactory(), 1);
  auto parallel = LLMEnsemble::Create(configs, MemoryFactory(), 7);
  ASSERT_TRUE(serial.ok()) << serial.status();
  ASSERT_TRUE(repeated.ok()) << repeated.status();
  ASSERT_TRUE(parallel.ok()) << parallel.status();

  const auto repeated_results = (*repeated)->GenerateMultiple(Request("prompt"), 80);
  const auto parallel_results = (*parallel)->ParallelGenerate(std::vector<LLMRequest>(80, Request("prompt")));
  ASSERT_TRUE(repeated_results.ok()) << repeated_results.status();
  ASSERT_TRUE(parallel_results.ok()) << parallel_results.status();

  ASSERT_EQ(repeated_results->size(), 80);
  ASSERT_EQ(parallel_results->size(), 80);

  for (std::size_t i = 0; i < 80; ++i) {
    const auto result = (*serial)->Generate(Request("prompt"));
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->model, (*repeated_results)[i].model);
    EXPECT_EQ(result->model, (*parallel_results)[i].model);
  }
}

TEST(LLMEnsembleTest, EmptyBatchesMakeNoCallsAndDoNotConsumeRandomSelections) {
  int calls = 0;
  const auto factory = FactoryWithHandler([&calls](const LLMRequest& request) {
    ++calls;
    return Echo(request);
  });
  auto ensemble = LLMEnsemble::Create({Model("a"), Model("b")}, factory);
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  const auto multiple = (*ensemble)->GenerateMultiple(Request("prompt"), 0);
  const auto parallel = (*ensemble)->ParallelGenerate({});
  ASSERT_TRUE(multiple.ok()) << multiple.status();
  ASSERT_TRUE(parallel.ok()) << parallel.status();

  EXPECT_TRUE(multiple->empty());
  EXPECT_TRUE(parallel->empty());
  EXPECT_EQ(calls, 0);

  auto with_empty = LLMEnsemble::Create({Model("a"), Model("b")}, MemoryFactory());
  auto untouched = LLMEnsemble::Create({Model("a"), Model("b")}, MemoryFactory());
  ASSERT_TRUE(with_empty.ok()) << with_empty.status();
  ASSERT_TRUE(untouched.ok()) << untouched.status();

  ASSERT_TRUE((*with_empty)->ParallelGenerate({}).ok());
  const auto first = (*with_empty)->GenerateMultiple(Request("prompt"), 20);
  const auto second = (*untouched)->GenerateMultiple(Request("prompt"), 20);
  ASSERT_TRUE(first.ok()) << first.status();
  ASSERT_TRUE(second.ok()) << second.status();

  ASSERT_EQ(first->size(), second->size());
  for (std::size_t i = 0; i < first->size(); ++i) {
    EXPECT_EQ((*first)[i].model, (*second)[i].model);
  }
}

TEST(LLMEnsembleTest, BoundsConcurrencyWhileAllowingOverlappingCalls) {
  std::mutex mutex;
  std::condition_variable changed;
  int active = 0;
  int maximum = 0;
  bool released = false;

  auto ensemble = LLMEnsemble::Create(
      {Model("one")}, FactoryWithHandler([&](const LLMRequest& request) -> absl::StatusOr<LLMResponse> {
        std::unique_lock<std::mutex> lock(mutex);
        ++active;
        maximum = std::max(maximum, active);
        if (active == 3) {
          released = true;
          changed.notify_all();
        }

        const bool overlapped = changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; });
        --active;
        if (!overlapped) return absl::DeadlineExceededError("calls serialized");

        return Echo(request);
      }),
      3);
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  const auto results = (*ensemble)->GenerateMultiple(Request("prompt"), 30);
  ASSERT_TRUE(results.ok()) << results.status();

  EXPECT_EQ(results->size(), 30);
  EXPECT_EQ(maximum, 3);
  EXPECT_EQ(active, 0);
}

TEST(LLMEnsembleTest, PreservesInputOrderAndUsageDespiteReversedCompletion) {
  std::mutex mutex;
  std::condition_variable changed;
  int completed_others = 0;

  auto ensemble = LLMEnsemble::Create(
      {Model("one")}, FactoryWithHandler([&](const LLMRequest& request) -> absl::StatusOr<LLMResponse> {
        std::unique_lock<std::mutex> lock(mutex);
        if (request.messages.back().content == "first") {
          if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return completed_others == 2; })) {
            return absl::DeadlineExceededError("other requests not completed");
          }
        } else {
          ++completed_others;
          changed.notify_all();
        }

        return Echo(request);
      }));
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  const auto results = (*ensemble)->ParallelGenerate({Request("first"), Request("second"), Request("third-longer")});
  ASSERT_TRUE(results.ok()) << results.status();

  ASSERT_EQ(results->size(), 3);
  EXPECT_EQ((*results)[0].text, "first");
  EXPECT_EQ((*results)[1].text, "second");
  EXPECT_EQ((*results)[2].text, "third-longer");

  ASSERT_TRUE((*results)[0].usage.has_value());
  ASSERT_TRUE((*results)[1].usage.has_value());
  ASSERT_TRUE((*results)[2].usage.has_value());
  EXPECT_EQ((*results)[0].usage->input_tokens, 5);
  EXPECT_EQ((*results)[1].usage->input_tokens, 6);
  EXPECT_EQ((*results)[2].usage->input_tokens, 12);
}

TEST(LLMEnsembleTest, ReturnsFirstInputErrorAfterJoiningAllCalls) {
  std::mutex mutex;
  std::condition_variable changed;
  int completed = 0;

  auto ensemble = LLMEnsemble::Create(
      {Model("one")}, FactoryWithHandler([&](const LLMRequest& request) -> absl::StatusOr<LLMResponse> {
        std::unique_lock<std::mutex> lock(mutex);
        const auto& text = request.messages.back().content;
        if (text == "first") {
          if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return completed == 9; })) {
            return absl::DeadlineExceededError("remaining calls not completed");
          }

          ++completed;
          return absl::InvalidArgumentError("first input failed");
        }

        ++completed;
        changed.notify_all();
        if (text == "second") return absl::NotFoundError("second input failed");

        return Echo(request);
      }));
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  std::vector<LLMRequest> requests(10, Request("other"));
  requests[0] = Request("first");
  requests[1] = Request("second");

  const auto result = (*ensemble)->ParallelGenerate(requests);
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(completed, 10);
}

TEST(LLMEnsembleTest, ConvertsClientExceptionsAndCompletesRemainingCalls) {
  for (bool standard_exception : {false, true}) {
    std::atomic<int> calls{0};
    auto ensemble =
        LLMEnsemble::Create({Model("one")}, FactoryWithHandler([&](const LLMRequest&) -> absl::StatusOr<LLMResponse> {
                              ++calls;
                              if (standard_exception) throw std::runtime_error("sensitive detail");
                              throw 7;
                            }));
    ASSERT_TRUE(ensemble.ok()) << ensemble.status();

    const auto single = (*ensemble)->Generate(Request("prompt"));
    EXPECT_EQ(single.status().code(), absl::StatusCode::kInternal);

    EXPECT_EQ(single.status().message().find("sensitive detail"), std::string::npos);

    const auto multiple = (*ensemble)->GenerateMultiple(Request("prompt"), 9);
    EXPECT_EQ(multiple.status().code(), absl::StatusCode::kInternal);

    EXPECT_EQ(calls, 10);
  }
}

TEST(LLMEnsembleTest, CapsWorkersToInputCountForHugeConcurrencySetting) {
  auto ensemble = LLMEnsemble::Create({Model("one")}, MemoryFactory(), std::numeric_limits<std::size_t>::max());
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  const auto result = (*ensemble)->GenerateMultiple(Request("prompt"), 1);
  ASSERT_TRUE(result.ok()) << result.status();

  ASSERT_EQ(result->size(), 1);
  EXPECT_EQ(result->front().text, "prompt");
}

}  // namespace
}  // namespace ievolve
