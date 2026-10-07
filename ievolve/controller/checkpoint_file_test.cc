#include "ievolve/controller/checkpoint_file.h"

#include <limits>

#include "gtest/gtest.h"
#include "ievolve/evaluator/workspace.h"
#include "ievolve/utils/file.h"

namespace ievolve::controller {
namespace {
CheckpointFile Example() {
  auto progress = RunProgress::Restore({3, "combined_score", 0.001}, {3, 2, 1, 1, Metrics(-10), false});
  EXPECT_TRUE(progress.ok()) << progress.status();
  return {"cpp", ".cpp", "initial", *progress};
}
TEST(CheckpointFileTest, RoundTripsCurrentState) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  ASSERT_TRUE(Example().Write((*workspace)->path()).ok());
  const auto restored = CheckpointFile::Read((*workspace)->path());
  ASSERT_TRUE(restored.ok()) << restored.status();
  EXPECT_EQ(restored->language, "cpp");
  EXPECT_EQ(restored->file_suffix, ".cpp");
  EXPECT_EQ(restored->best_id, "initial");
  EXPECT_EQ(restored->progress.iteration(), 3);
  EXPECT_EQ(restored->progress.accepted(), 2);
  EXPECT_EQ(restored->progress.rejected(), 1);
  EXPECT_EQ(restored->progress.counters().best_score, -10);
  EXPECT_EQ(restored->progress.policy(), Example().progress.policy());
}
TEST(CheckpointFileTest, KeepsTheOnDiskLayout) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  ASSERT_TRUE(Example().Write((*workspace)->path()).ok());
  const auto text = utils::ReadFile((*workspace)->path() / "checkpoint.json");
  ASSERT_TRUE(text.ok());
  EXPECT_EQ(Metrics::parse(*text).dump(),
            R"({"format":"ievolve.cli-checkpoint","schema_version":1,"evaluation_protocol_version":1,)"
            R"("program":{"language":"cpp","file_suffix":".cpp","best_id":"initial"},)"
            R"("progress":{"iteration":3,"accepted":2,"rejected":1,"without_improvement":1,"best_score":-10,)"
            R"("early_stopped":false},"stopping_policy":{"patience":3,"metric":"combined_score","threshold":0.001}})");
}
TEST(CheckpointFileTest, RejectsInconsistentCountersAndMissingFields) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  ASSERT_TRUE(Example().Write((*workspace)->path()).ok());
  auto text = utils::ReadFile((*workspace)->path() / "checkpoint.json");
  ASSERT_TRUE(text.ok());
  const auto original = Metrics::parse(*text);
  for (const auto& pointer : {"iteration", "accepted", "rejected", "without_improvement"}) {
    for (const Metrics bad :
         {Metrics(-1), Metrics(true), Metrics(0.5), Metrics(std::numeric_limits<std::uint64_t>::max())}) {
      auto value = original;
      value["progress"][pointer] = bad;
      ASSERT_TRUE(utils::WriteFile((*workspace)->path() / "checkpoint.json", value.dump()).ok());
      EXPECT_EQ(CheckpointFile::Read((*workspace)->path()).status().code(), absl::StatusCode::kDataLoss);
    }
  }
  for (const auto& item : original.items()) {
    auto value = original;
    value.erase(item.key());
    ASSERT_TRUE(utils::WriteFile((*workspace)->path() / "checkpoint.json", value.dump()).ok());
    EXPECT_FALSE(CheckpointFile::Read((*workspace)->path()).ok());
  }
  auto value = original;
  value["progress"]["rejected"] = 2;
  ASSERT_TRUE(utils::WriteFile((*workspace)->path() / "checkpoint.json", value.dump()).ok());
  const auto inconsistent = CheckpointFile::Read((*workspace)->path());
  EXPECT_EQ(inconsistent.status().code(), absl::StatusCode::kDataLoss);
  EXPECT_NE(inconsistent.status().message().find("progress"), std::string::npos) << inconsistent.status();
}
TEST(CheckpointFileTest, RejectsInvalidProgramFieldsOnWrite) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  auto file = Example();
  file.file_suffix = "cpp";
  const auto status = file.Write((*workspace)->path());
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("program.file_suffix"), std::string::npos) << status;
  EXPECT_FALSE(std::filesystem::exists((*workspace)->path() / "checkpoint.json"));
}
TEST(CheckpointFileTest, RejectsDuplicateKeysUnknownSchemaAndTrailingContent) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  ASSERT_TRUE(Example().Write((*workspace)->path()).ok());
  const auto text = utils::ReadFile((*workspace)->path() / "checkpoint.json");
  ASSERT_TRUE(text.ok());
  auto value = Metrics::parse(*text);
  value["schema_version"] = 2;
  ASSERT_TRUE(utils::WriteFile((*workspace)->path() / "checkpoint.json", value.dump()).ok());
  EXPECT_FALSE(CheckpointFile::Read((*workspace)->path()).ok());
  for (const std::string bad :
       std::vector<std::string>{"{\"schema_version\":1," + text->substr(1), *text + "{}", "{"}) {
    ASSERT_TRUE(utils::WriteFile((*workspace)->path() / "checkpoint.json", bad).ok());
    EXPECT_EQ(CheckpointFile::Read((*workspace)->path()).status().code(), absl::StatusCode::kDataLoss);
  }
}
}  // namespace
}  // namespace ievolve::controller
