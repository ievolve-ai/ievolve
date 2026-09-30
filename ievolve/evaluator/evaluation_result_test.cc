#include "ievolve/evaluator/evaluation_result.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include "gtest/gtest.h"
#include "ievolve/database/artifact_store.h"

namespace ievolve::evaluator {
namespace {

Metrics ReadFixture() {
  std::ifstream stream(std::filesystem::path(IEVOLVE_EVALUATOR_TEST_DATA_DIR) / "evaluation_result.json");
  return Metrics::parse(stream);
}

TEST(EvaluationResultTest, PreservesDirectScalarMetricsAndTypedArtifacts) {
  const Metrics wire = {
      {"metrics",
       {{"score", 0.75},
        {"count", -2},
        {"large", std::numeric_limits<std::uint64_t>::max()},
        {"passed", true},
        {"note", "é中🙂"},
        {"missing", nullptr}}},
      {"artifacts", {{"text", "é中🙂"}, {"empty", {{"__bytes__", ""}}}, {"bytes", {{"__bytes__", "AP+A"}}}}}};

  auto result = EvaluationResult::FromJson(wire);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->metrics, wire["metrics"]);
  EXPECT_TRUE(result->metrics["passed"].is_boolean());
  EXPECT_TRUE(result->metrics["count"].is_number_integer());
  EXPECT_TRUE(result->metrics["large"].is_number_unsigned());
  EXPECT_EQ(std::get<std::string>(result->artifacts.at("text")), "é中🙂");
  EXPECT_EQ(std::get<ArtifactBytes>(result->artifacts.at("bytes")), (ArtifactBytes{0, 255, 128}));
  EXPECT_TRUE(std::get<ArtifactBytes>(result->artifacts.at("empty")).empty());

  auto encoded = result->ToJson();
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  EXPECT_EQ((*encoded)["metrics"], wire["metrics"]);
  EXPECT_EQ((*encoded)["artifacts"]["text"], "é中🙂");
  EXPECT_EQ((*encoded)["artifacts"]["bytes"], wire["artifacts"]["bytes"]);
  EXPECT_EQ((*encoded)["artifacts"]["empty"], wire["artifacts"]["empty"]);
}

TEST(EvaluationResultTest, AllowsEmptyMetricsAndOmittedArtifacts) {
  const auto result = EvaluationResult::FromJson({{"metrics", Metrics::object()}});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_TRUE(result->artifacts.empty());
  EXPECT_TRUE(result->Validate(0).ok());
  EXPECT_EQ(result->GetArtifactSize("missing"), 0);
  EXPECT_EQ(result->GetTotalArtifactSize(), 0);
}

TEST(EvaluationResultTest, RejectsMalformedResultAndMetricTypes) {
  for (const auto& wire :
       {Metrics(nullptr), Metrics::array(), Metrics::object(), Metrics{{"metrics", nullptr}},
        Metrics{{"metrics", Metrics::array()}}, Metrics{{"metrics", {{"nested", Metrics::object()}}}},
        Metrics{{"metrics", {{"nested", Metrics::array()}}}},
        Metrics{{"metrics", Metrics::object()}, {"artifacts", nullptr}},
        Metrics{{"metrics", Metrics::object()}, {"artifacts", "text"}}}) {
    SCOPED_TRACE(wire.dump());
    EXPECT_EQ(EvaluationResult::FromJson(wire).status().code(), absl::StatusCode::kInvalidArgument);
  }

  for (const auto& metric :
       {Metrics(std::numeric_limits<double>::infinity()), Metrics(-std::numeric_limits<double>::infinity()),
        Metrics(std::numeric_limits<double>::quiet_NaN()), Metrics::binary({1, 2})}) {
    EvaluationResult result;
    result.metrics["bad"] = metric;

    EXPECT_EQ(result.Validate().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(result.ToJson().status().code(), absl::StatusCode::kInvalidArgument);
  }
}

TEST(EvaluationResultTest, RejectsNoncanonicalBinaryMarkers) {
  for (const auto& value :
       {Metrics(1), Metrics(false), Metrics(nullptr), Metrics::array(), Metrics::object(), Metrics{{"__bytes__", 5}},
        Metrics{{"__bytes__", "AA=="}, {"extra", "value"}}, Metrics{{"__bytes__", "AA"}},
        Metrics{{"__bytes__", "AB=="}}, Metrics{{"__bytes__", "AA==\n"}}, Metrics{{"__bytes__", "_w=="}},
        Metrics{{"__bytes__", "!!!!"}}}) {
    SCOPED_TRACE(value.dump());
    const Metrics wire = {{"metrics", Metrics::object()}, {"artifacts", {{"bad", value}}}};

    EXPECT_EQ(EvaluationResult::FromJson(wire).status().code(), absl::StatusCode::kInvalidArgument);
  }
}

TEST(EvaluationResultTest, ValidatesUtf8InAllKeysAndTextValues) {
  const std::string invalid_utf8("\xff", 1);
  for (int field = 0; field < 4; ++field) {
    EvaluationResult result;
    if (field == 0) result.metrics[invalid_utf8] = 1;
    if (field == 1) result.metrics["text"] = invalid_utf8;
    if (field == 2) result.artifacts[invalid_utf8] = std::string("text");
    if (field == 3) result.artifacts["text"] = invalid_utf8;

    EXPECT_EQ(result.Validate().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(result.ToJson().status().code(), absl::StatusCode::kInvalidArgument);
  }

  EXPECT_EQ(EvaluationResult::FromJson({{"metrics", {{"bad", invalid_utf8}}}}).status().code(),
            absl::StatusCode::kInvalidArgument);

  EvaluationResult binary;
  binary.artifacts["binary"] = ArtifactBytes{255};
  EXPECT_TRUE(binary.Validate().ok());
}

TEST(EvaluationResultTest, EnforcesDecodedPayloadLimitIncludingUtf8Bytes) {
  const Metrics wire = {{"metrics", {{"score", 1}}},
                        {"artifacts", {{"text", "中"}, {"binary", {{"__bytes__", "AQIDBA=="}}}}}};

  auto result = EvaluationResult::FromJson(wire, 7);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->GetArtifactSize("text"), 3);
  EXPECT_EQ(result->GetArtifactSize("binary"), 4);
  EXPECT_EQ(result->GetTotalArtifactSize(), 7);

  EXPECT_TRUE(result->Validate(7).ok());
  EXPECT_EQ(result->Validate(6).code(), absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(EvaluationResult::FromJson(wire, 6).status().code(), absl::StatusCode::kResourceExhausted);
}

class ArtifactRoundTripTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ =
        std::filesystem::temp_directory_path() /
        ("ievolve-artifact-roundtrip-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  std::filesystem::path path_;
};

TEST_F(ArtifactRoundTripTest, EvaluationArtifactsSurviveInlineAndDiskStorage) {
  const Metrics wire = {{"metrics", {{"score", 1}}},
                        {"artifacts",
                         {{"text", "é中"},
                          {"binary", {{"__bytes__", "AP+A"}}},
                          {"binary-utf8", {{"__bytes__", "QUI="}}},
                          {"empty", {{"__bytes__", ""}}}}}};

  const auto evaluated = EvaluationResult::FromJson(wire, 10);
  ASSERT_TRUE(evaluated.ok()) << evaluated.status();

  for (const int threshold : {100, 0}) {
    SCOPED_TRACE(threshold);
    DatabaseConfig config;
    config.artifacts_base_path = path_.string();
    config.artifact_size_threshold = threshold;
    const auto store = ArtifactStore::Create(config);
    ASSERT_TRUE(store.ok()) << store.status();

    Program program;
    program.id = "evaluated";

    const auto stored = store->Store(program, evaluated->artifacts);
    ASSERT_TRUE(stored.ok()) << stored.status();
    EXPECT_EQ(stored->artifact_dir.has_value(), threshold == 0);

    const auto restored = ArtifactStore::Load(*stored);
    ASSERT_TRUE(restored.ok()) << restored.status();
    EXPECT_EQ(*restored, evaluated->artifacts);

    const EvaluationResult result{evaluated->metrics, *restored};
    EXPECT_TRUE(result.Validate(10).ok());
    EXPECT_EQ(result.Validate(9).code(), absl::StatusCode::kResourceExhausted);

    const auto encoded = result.ToJson();
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    EXPECT_EQ((*encoded)["metrics"], wire["metrics"]);
    for (const auto& [key, value] : wire["artifacts"].items()) EXPECT_EQ((*encoded)["artifacts"][key], value);
  }
}

TEST(EvaluationResultTest, MatchesPythonArtifactSizes) {
  const auto fixture = ReadFixture();
  for (const auto& test_case : fixture["results"]) {
    SCOPED_TRACE(test_case["name"].get<std::string>());
    auto result = EvaluationResult::FromJson(test_case["wire"]);
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_EQ(result->metrics, test_case["metrics"]);
    for (const auto& [key, size] : test_case["artifact_sizes"].items())
      EXPECT_EQ(result->GetArtifactSize(key), size.get<std::size_t>());
    EXPECT_EQ(result->GetArtifactSize("missing"), 0);
    EXPECT_EQ(result->GetTotalArtifactSize(), test_case["total_artifact_size"].get<std::size_t>());
  }
}

TEST(EvaluationResultTest, MatchesPythonThresholdCases) {
  const auto fixture = ReadFixture();
  for (const auto& test_case : fixture["thresholds"]) {
    SCOPED_TRACE(test_case["name"].get<std::string>());
    EvaluationResult result;
    result.metrics = test_case["metrics"];

    EXPECT_EQ(result.PassesThreshold(test_case["threshold"].get<double>()), test_case["expected"].get<bool>());
  }
}

TEST(EvaluationResultTest, MatchesPythonCascadeMergeCases) {
  const auto fixture = ReadFixture();
  for (const auto& test_case : fixture["merges"]) {
    SCOPED_TRACE(test_case["name"].get<std::string>());
    auto earlier = EvaluationResult::FromJson(test_case["earlier"]);
    auto later = EvaluationResult::FromJson(test_case["later"]);
    auto expected = EvaluationResult::FromJson(test_case["expected"]);
    ASSERT_TRUE(earlier.ok()) << earlier.status();
    ASSERT_TRUE(later.ok()) << later.status();
    ASSERT_TRUE(expected.ok()) << expected.status();

    const auto merged = EvaluationResult::Merge(*earlier, *later);
    EXPECT_EQ(merged.metrics, expected->metrics);
    EXPECT_EQ(merged.artifacts, expected->artifacts);
    for (const auto& metric : merged.metrics) EXPECT_TRUE(metric.is_number_float());

    EXPECT_EQ(earlier->metrics, test_case["earlier"]["metrics"]);
    EXPECT_EQ(later->metrics, test_case["later"]["metrics"]);
  }
}

}  // namespace
}  // namespace ievolve::evaluator
