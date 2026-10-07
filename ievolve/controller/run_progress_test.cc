#include "ievolve/controller/run_progress.h"

#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

namespace ievolve::controller {
namespace {

StoppingPolicy Policy(std::optional<int> patience, double threshold = 0.001, std::string metric = "combined_score") {
  return {patience, std::move(metric), threshold};
}

Metrics Score(const Metrics& value) { return {{"combined_score", value}}; }

TEST(RunProgressTest, CountsAcceptedAndRejectedAttempts) {
  RunProgress progress(Policy(std::nullopt));
  progress.RecordAccepted(Score(1));
  progress.RecordRejected();
  progress.RecordAccepted(Score(2));

  EXPECT_EQ(progress.iteration(), 3);
  EXPECT_EQ(progress.accepted(), 2);
  EXPECT_EQ(progress.rejected(), 1);
  EXPECT_FALSE(progress.early_stopped());
  // Without patience, no stopping score is tracked.
  EXPECT_FALSE(progress.counters().best_score);
}

TEST(RunProgressTest, PositivePatienceStopsAfterThatManyNonImprovingChildren) {
  RunProgress progress(Policy(2, 0.5));
  progress.RecordAccepted(Score(1.0));
  progress.RecordAccepted(Score(1.4));  // Below threshold: no improvement.
  EXPECT_FALSE(progress.early_stopped());
  progress.RecordRejected();  // Rejections do not count against patience.
  EXPECT_FALSE(progress.early_stopped());
  progress.RecordAccepted(Score(1.2));
  EXPECT_TRUE(progress.early_stopped());
  EXPECT_EQ(progress.counters().without_improvement, 2);
  EXPECT_EQ(progress.counters().best_score, Metrics(1.0));
}

TEST(RunProgressTest, ImprovementResetsPatience) {
  RunProgress progress(Policy(2, 0.5));
  progress.RecordAccepted(Score(1.0));
  progress.RecordAccepted(Score(1.0));
  progress.RecordAccepted(Score(1.5));
  EXPECT_FALSE(progress.early_stopped());
  EXPECT_EQ(progress.counters().without_improvement, 0);
  EXPECT_EQ(progress.counters().best_score, Metrics(1.5));
}

TEST(RunProgressTest, NonPositivePatienceStopsOnlyAtExactThreshold) {
  RunProgress progress(Policy(0, 3));
  progress.RecordAccepted(Score(2));
  EXPECT_FALSE(progress.early_stopped());
  EXPECT_FALSE(progress.counters().best_score);
  progress.RecordAccepted(Score(3));
  EXPECT_TRUE(progress.early_stopped());
  EXPECT_EQ(progress.counters().best_score, Metrics(3));
}

TEST(RunProgressTest, IntegerDeltasDoNotLosePrecision) {
  RunProgress progress(Policy(1, 1));
  progress.RecordAccepted(Score(std::numeric_limits<std::int64_t>::min()));
  progress.RecordAccepted(Score(std::numeric_limits<std::uint64_t>::max()));
  EXPECT_FALSE(progress.early_stopped());
  progress.RecordAccepted(Score(std::numeric_limits<std::uint64_t>::max()));
  EXPECT_TRUE(progress.early_stopped());
}

TEST(RunProgressTest, MissingMetricFallsBackToNumericMeanAndIgnoresEmptyMetrics) {
  RunProgress progress(Policy(1, 0, "missing"));
  progress.RecordAccepted({{"a", 1}, {"b", 3}, {"label", "text"}});
  EXPECT_EQ(progress.counters().best_score, Metrics(2.0));
  progress.RecordAccepted({{"label", "text"}});  // No numeric metric: unchanged.
  progress.RecordAccepted(Metrics::object());
  EXPECT_FALSE(progress.early_stopped());
  progress.RecordAccepted({{"missing", "not a number"}});  // Present but non-numeric: unchanged.
  EXPECT_FALSE(progress.early_stopped());
}

TEST(RunProgressTest, RestoreRoundTripsCounters) {
  RunProgress progress(Policy(3));
  progress.RecordAccepted(Score(1));
  progress.RecordRejected();
  progress.RecordAccepted(Score(1));

  const auto restored = RunProgress::Restore(Policy(3), progress.counters());
  ASSERT_TRUE(restored.ok()) << restored.status();
  EXPECT_EQ(restored->counters(), progress.counters());
  EXPECT_EQ(restored->policy(), Policy(3));
}

TEST(RunProgressTest, RestoreRejectsStateNoRunCouldProduce) {
  RunProgress::Counters valid{3, 2, 1, 1, Metrics(-10), false};
  ASSERT_TRUE(RunProgress::Restore(Policy(3), valid).ok());

  const auto rejects = [&](StoppingPolicy policy, auto mutate) {
    auto counters = valid;
    mutate(counters);
    return RunProgress::Restore(std::move(policy), counters).status().code() == absl::StatusCode::kDataLoss;
  };
  using C = RunProgress::Counters;
  EXPECT_TRUE(rejects(Policy(3), [](C& c) { c.iteration = -1; }));
  EXPECT_TRUE(rejects(Policy(3), [](C& c) { c.accepted = 4; }));
  EXPECT_TRUE(rejects(Policy(3), [](C& c) { c.rejected = 2; }));
  EXPECT_TRUE(rejects(Policy(3), [](C& c) { c.without_improvement = 3; }));
  EXPECT_TRUE(rejects(Policy(3), [](C& c) { c.best_score.reset(); }));
  EXPECT_TRUE(rejects(Policy(3), [](C& c) { c.best_score = "1.0"; }));
  EXPECT_TRUE(rejects(Policy(3), [](C& c) { c.best_score = std::numeric_limits<double>::infinity(); }));
  EXPECT_TRUE(rejects(Policy(3), [](C& c) { c.early_stopped = true; }));
  EXPECT_TRUE(rejects(Policy(1), [](C&) {}));  // without_improvement 1 >= patience 1 must be stopped.
  EXPECT_TRUE(rejects(Policy(std::nullopt), [](C&) {}));
  EXPECT_TRUE(rejects(Policy(0, -10), [](C&) {}));  // Non-positive patience never counts.
  EXPECT_TRUE(rejects(Policy(0, -9), [](C& c) {
    c.without_improvement = 0;
    c.early_stopped = true;
  }));
  EXPECT_FALSE(rejects(Policy(0, -10), [](C& c) {
    c.without_improvement = 0;
    c.early_stopped = true;
  }));
}

TEST(StoppingPolicyTest, RoundTripsAndRejectsInvalidJson) {
  const auto policy = Policy(3, 0.25, "score");
  const auto json = policy.ToJson();
  EXPECT_EQ(json.dump(), R"({"patience":3,"metric":"score","threshold":0.25})");
  const auto parsed = StoppingPolicy::FromJson(json);
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(*parsed, policy);
  EXPECT_EQ(StoppingPolicy::FromJson(Policy(std::nullopt).ToJson())->patience, std::nullopt);

  for (const auto& [key, bad] :
       std::vector<std::pair<std::string, Metrics>>{{"patience", 1.5},
                                                    {"patience", std::int64_t{std::numeric_limits<int>::max()} + 1},
                                                    {"patience", std::numeric_limits<std::uint64_t>::max()},
                                                    {"metric", ""},
                                                    {"metric", 1},
                                                    {"threshold", "0.1"},
                                                    {"threshold", true}}) {
    SCOPED_TRACE(key + ": " + bad.dump());
    auto value = json;
    value[key] = bad;
    EXPECT_EQ(StoppingPolicy::FromJson(value).status().code(), absl::StatusCode::kDataLoss);
  }
  auto extra = json;
  extra["other"] = 1;
  EXPECT_FALSE(StoppingPolicy::FromJson(extra).ok());
  EXPECT_FALSE(StoppingPolicy::FromJson(Metrics::array()).ok());
}

}  // namespace
}  // namespace ievolve::controller
