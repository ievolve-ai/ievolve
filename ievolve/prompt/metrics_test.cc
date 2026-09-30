#include "ievolve/prompt/metrics.h"

#include <limits>

#include "gtest/gtest.h"

namespace ievolve {
namespace {

TEST(MetricsTest, SeparatesFitnessFromFeaturesAndFlags) {
  const Metrics metrics = {{"quality", 0.6}, {"speed", 0.8}, {"complexity", 90}, {"timeout", true}, {"message", "ok"}};

  EXPECT_DOUBLE_EQ(GetFitnessScore(metrics, {"complexity"}), 0.7);
  EXPECT_EQ(FormatFeatureCoordinates(metrics, {"complexity", "absent"}), "complexity=90.00");
}

TEST(MetricsTest, PrefersNumericCombinedScoreAndIgnoresStrings) {
  EXPECT_DOUBLE_EQ(GetFitnessScore({{"combined_score", 0.25}, {"score", 1}}), 0.25);
  EXPECT_DOUBLE_EQ(GetFitnessScore({{"combined_score", true}, {"score", 0.5}}), 1.0);

  EXPECT_DOUBLE_EQ(GetFitnessScore({{"combined_score", "0.25"}, {"score", 1}}), 1.0);
  EXPECT_DOUBLE_EQ(GetFitnessScore({{"combined_score", "invalid"}}), 0.0);
}

TEST(MetricsTest, IgnoresNanAndFallsBackToFeatureOnlyMetrics) {
  EXPECT_DOUBLE_EQ(GetFitnessScore({{"nan", std::numeric_limits<double>::quiet_NaN()}, {"score", 0.4}, {"flag", true}}),
                   0.4);

  EXPECT_DOUBLE_EQ(GetFitnessScore({{"size", 2}}, {"size"}), 2.0);
  EXPECT_DOUBLE_EQ(GetFitnessScore({{"flag", true}}), 0.0);
  EXPECT_DOUBLE_EQ(GetFitnessScore(Metrics::object()), 0.0);
}

TEST(MetricsTest, FormatsMixedValuesInInsertionOrder) {
  const Metrics metrics = {{"z", 0.2}, {"a", "ok"}, {"flag", true}, {"missing", nullptr}};

  EXPECT_EQ(FormatMetrics(metrics), "- z: 0.2000\n- a: ok\n- flag: 1.0000\n- missing: None");
  EXPECT_EQ(FormatMetrics(metrics, false), "z: 0.2000, a: ok, flag: 1.0000, missing: None");

  EXPECT_EQ(FormatFeatureCoordinates(metrics, {"a", "flag"}), "a=ok, flag=1.00");
}

}  // namespace
}  // namespace ievolve
