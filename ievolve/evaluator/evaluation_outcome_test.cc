#include "ievolve/evaluator/evaluation_outcome.h"

#include <limits>

#include "gtest/gtest.h"

namespace ievolve::evaluator {
namespace {
TEST(EvaluationOutcomeTest, RejectionCannotCarryFitness) {
  EvaluationOutcome value;
  value.reason = "wrong_answer";
  value.result.metrics = {{"combined_score", 0}};
  EXPECT_FALSE(value.Validate().ok());
  value.result.metrics.clear();
  EXPECT_TRUE(value.Validate().ok());
  value.reason.clear();
  EXPECT_FALSE(value.Validate().ok());
  value.reason = std::string(4097, 'x');
  EXPECT_FALSE(value.Validate().ok());
  value.reason = std::string(1, '\xff');
  EXPECT_FALSE(value.Validate().ok());
}
TEST(EvaluationOutcomeTest, PassRequiresFiniteNumericScoreAndNoReason) {
  EvaluationOutcome value;
  value.passed = true;
  EXPECT_FALSE(value.Validate().ok());
  for (const auto& score : std::vector<Metrics>{true, "1", nullptr, std::numeric_limits<double>::quiet_NaN(),
                                                std::numeric_limits<double>::infinity()}) {
    value.result.metrics = {{"combined_score", score}};
    EXPECT_FALSE(value.Validate().ok());
  }
  value.result.metrics = {{"combined_score", -10.0}};
  EXPECT_TRUE(value.Validate().ok());
  value.reason = "wrong";
  EXPECT_FALSE(value.Validate().ok());
}
TEST(EvaluationOutcomeTest, RoundTripsVerdictsAndBinaryArtifactsWithDecodedLimit) {
  for (bool passed : {false, true}) {
    EvaluationOutcome value;
    value.passed = passed;
    if (passed)
      value.result.metrics["combined_score"] = -1;
    else
      value.reason = "wrong_answer";
    value.result.artifacts = {{"bytes", ArtifactBytes{0, 255}}, {"text", "abc"}};
    auto json = value.ToJson();
    ASSERT_TRUE(json.ok()) << json.status();
    auto result = EvaluationOutcome::Parse(json->dump(), true, 5);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->result.artifacts, value.result.artifacts);
    EXPECT_EQ(result->passed, passed);
    EXPECT_FALSE(EvaluationOutcome::Parse(json->dump(), true, 4).ok());
    auto disabled = EvaluationOutcome::Parse(json->dump(), false, 0);
    ASSERT_TRUE(disabled.ok());
    EXPECT_TRUE(disabled->result.artifacts.empty());
  }
}
TEST(EvaluationOutcomeTest, RejectsMalformedEnvelopesDuplicatesAndExcessiveDepth) {
  for (const std::string text :
       {R"({"protocol_version":1,"passed":true,"metrics":{"combined_score":1},"reason":""})",
        R"({"protocol_version":1,"passed":false,"metrics":{},"reason":"x","extra":0})",
        R"({"protocol_version":1.0,"passed":false,"metrics":{},"reason":"x"})",
        R"({"protocol_version":2,"passed":false,"metrics":{},"reason":"x"})",
        R"({"protocol_version":1,"passed":0,"metrics":{},"reason":"x"})",
        R"({"protocol_version":1,"passed":false,"metrics":{},"reason":"x","reason":"y"})",
        R"({"protocol_version":1,"passed":true,"metrics":{"combined_score":1,"combined_score":2}})",
        R"({"protocol_version":1,"passed":false,"metrics":{},"reason":"x"} trailing)",
        R"({"protocol_version":1,"passed":false,"metrics":{},"reason":"x"} {})"}) {
    SCOPED_TRACE(text);
    EXPECT_FALSE(EvaluationOutcome::Parse(text, true, 100).ok());
  }
  const std::string nested = R"({"protocol_version":1,"passed":false,"metrics":{},"reason":"x","artifacts":)";
  EXPECT_FALSE(
      EvaluationOutcome::Parse(nested + std::string(64, '[') + "0" + std::string(64, ']') + "}", false, 0).ok());
}
}  // namespace
}  // namespace ievolve::evaluator
