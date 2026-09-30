#include "ievolve/program/program.h"

#include <chrono>
#include <fstream>
#include <limits>

#include "gtest/gtest.h"
#include "ievolve/program/artifact.h"

namespace ievolve {
namespace {

TEST(ArtifactTest, RejectsInvalidUtf8InBothCodecDirections) {
  for (const auto& text : {std::string(1, '\xff'), std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3)}) {
    EXPECT_EQ(EncodeArtifactValue(ArtifactValue(text)).status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(DecodeArtifactValue(Metrics(text)).status().code(), absl::StatusCode::kInvalidArgument);
  }
}

TEST(ProgramTest, MatchesCompletePythonRoundTrips) {
  std::ifstream stream(std::string(IEVOLVE_PROGRAM_TEST_DATA_DIR) + "/python_golden.json");
  ASSERT_TRUE(stream.good());

  const auto fixtures = Metrics::parse(stream);
  ASSERT_EQ(fixtures.at("cases").size(), 9);

  for (const auto& item : fixtures.at("cases")) {
    SCOPED_TRACE(item.at("name").get<std::string>());

    auto program = Program::FromJson(item.at("input"));
    ASSERT_TRUE(program.ok()) << program.status();

    auto json = program->ToJson();
    ASSERT_TRUE(json.ok()) << json.status();
    EXPECT_EQ(*json, item.at("expected"));

    auto again = Program::FromJson(*json);
    ASSERT_TRUE(again.ok()) << again.status();

    auto roundtrip = again->ToJson();
    ASSERT_TRUE(roundtrip.ok()) << roundtrip.status();
    EXPECT_EQ(*roundtrip, *json);
  }
}

TEST(ProgramTest, MissingTimestampUsesCurrentTime) {
  const auto before = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  auto program = Program::FromJson({{"id", "a"}, {"code", ""}});
  const auto after = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  ASSERT_TRUE(program.ok()) << program.status();

  EXPECT_GE(program->timestamp, before);
  EXPECT_LE(program->timestamp, after);
}

TEST(ProgramTest, RejectsInvalidRequiredAndTypedFields) {
  for (const auto& value :
       {Metrics::array(), Metrics{{"id", "a"}}, Metrics{{"code", "x"}}, Metrics{{"id", ""}, {"code", "x"}}}) {
    EXPECT_EQ(Program::FromJson(value).status().code(), absl::StatusCode::kInvalidArgument);
  }

  const std::vector<Metrics> invalid = {{{"id", 1}},
                                        {{"code", nullptr}},
                                        {{"language", false}},
                                        {{"changes_description", nullptr}},
                                        {{"parent_id", 1}},
                                        {{"generation", -1}},
                                        {{"generation", 1.5}},
                                        {{"generation", true}},
                                        {{"generation", std::numeric_limits<std::uint64_t>::max()}},
                                        {{"iteration_found", -1}},
                                        {{"timestamp", true}},
                                        {{"complexity", "1"}},
                                        {{"diversity", false}},
                                        {{"metadata", nullptr}},
                                        {{"metadata", Metrics::array()}},
                                        {{"metrics", Metrics::array()}},
                                        {{"metrics", {{"nested", Metrics::array()}}}},
                                        {{"prompts", 3}},
                                        {{"artifact_dir", false}},
                                        {{"artifacts_json", Metrics::object()}},
                                        {{"embedding", "x"}},
                                        {{"embedding", {1, true}}}};

  for (const auto& field : invalid) {
    Metrics input = {{"id", "a"}, {"code", "x"}};
    input.update(field);

    auto result = Program::FromJson(input);
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument) << field;
  }
}

TEST(ProgramTest, ValidatesMutableValuesBeforeSerializing) {
  Program program;
  program.id = "a";
  ASSERT_TRUE(program.Validate().ok());

  program.generation = -1;
  EXPECT_EQ(program.ToJson().status().code(), absl::StatusCode::kInvalidArgument);

  program.generation = 0;
  program.metadata = {{"nested", {{"value", std::numeric_limits<double>::infinity()}}}};
  EXPECT_FALSE(program.Validate().ok());

  program.metadata = Metrics::object();
  program.metrics["score"] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(program.ToJson().ok());

  program.metrics.clear();
  program.embedding = std::vector<double>{std::numeric_limits<double>::infinity()};
  EXPECT_FALSE(program.Validate().ok());
}

TEST(ProgramTest, RejectsInvalidUtf8AndBinaryWithoutEchoingValues) {
  Program program;
  program.id = "a";
  program.code = std::string("private-secret") + char(0xff);

  auto result = program.ToJson();
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(std::string(result.status().message()).find("private-secret"), std::string::npos);

  program.code = "";
  program.metadata["bytes"] = Metrics::binary({1, 2, 3});
  EXPECT_FALSE(program.ToJson().ok());
}

TEST(ProgramTest, PreservesMetricInsertionOrderAndIntegerPrecision) {
  auto program = Program::FromJson({{"id", "a"}, {"code", "x"}, {"metrics", {{"z", 9007199254740993LL}, {"a", 1}}}});
  ASSERT_TRUE(program.ok());

  auto json = program->ToJson();
  ASSERT_TRUE(json.ok());

  EXPECT_EQ(json->at("metrics").begin().key(), "z");
  EXPECT_EQ(json->at("metrics").at("z").get<std::int64_t>(), 9007199254740993LL);
}

TEST(ProgramTest, RejectsMalformedLegacyDescriptionButIgnoresUnknownFields) {
  auto result = Program::FromJson({{"id", "a"}, {"code", "x"}, {"metadata", {{"changes", 7}}}});
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);

  result = Program::FromJson({{"id", "a"}, {"code", "x"}, {"unknown", {1, 2}}});
  ASSERT_TRUE(result.ok());

  auto json = result->ToJson();
  ASSERT_TRUE(json.ok());
  EXPECT_FALSE(json->contains("unknown"));
}

TEST(MetricsTest, AsCounterAcceptsNonnegativeIntegersThatFitInInt64) {
  EXPECT_EQ(AsCounter(Metrics(0)), 0);
  EXPECT_EQ(AsCounter(Metrics(42)), 42);

  const auto limit = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(AsCounter(Metrics(limit)), limit);
}

TEST(MetricsTest, AsCounterRejectsEverythingElse) {
  EXPECT_FALSE(AsCounter(Metrics(-1)).has_value());
  EXPECT_FALSE(AsCounter(Metrics(1.5)).has_value());
  EXPECT_FALSE(AsCounter(Metrics(true)).has_value());
  EXPECT_FALSE(AsCounter(Metrics("7")).has_value());
  EXPECT_FALSE(AsCounter(Metrics(nullptr)).has_value());
  EXPECT_FALSE(AsCounter(Metrics::object()).has_value());

  // Unsigned values above int64 max do not fit and are refused rather than
  // wrapping to a negative counter.
  const auto above = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
  EXPECT_FALSE(AsCounter(Metrics(above)).has_value());
}

TEST(MetricsTest, IsNumericTreatsBooleansAsNumbersAndNumberConvertsThem) {
  EXPECT_TRUE(IsNumeric(Metrics(1)));
  EXPECT_TRUE(IsNumeric(Metrics(1.5)));
  EXPECT_TRUE(IsNumeric(Metrics(true)));
  EXPECT_FALSE(IsNumeric(Metrics("1")));
  EXPECT_FALSE(IsNumeric(Metrics(nullptr)));

  EXPECT_DOUBLE_EQ(Number(Metrics(true)), 1.0);
  EXPECT_DOUBLE_EQ(Number(Metrics(false)), 0.0);
  EXPECT_DOUBLE_EQ(Number(Metrics(2.5)), 2.5);

  // The widening overload shares the boolean handling.
  EXPECT_EQ(NumberAs<long double>(Metrics(true)), 1.0L);
  EXPECT_EQ(NumberAs<long double>(Metrics(-3)), -3.0L);
}

}  // namespace
}  // namespace ievolve
