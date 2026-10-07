#include "ievolve/controller/checkpoint_state.h"

#include <limits>

#include "gtest/gtest.h"
#include "ievolve/evaluator/workspace.h"
#include "ievolve/utils/file.h"

namespace ievolve::controller {
namespace {
CheckpointState Example() {
  CheckpointState state;
  state.language = "cpp";
  state.file_suffix = ".cpp";
  state.best_id = "initial";
  state.progress.iteration = 3;
  state.progress.accepted = 2;
  state.progress.rejected = 1;
  state.progress.without_improvement = 1;
  state.progress.best_score = -10;
  state.stopping_policy = {{"patience", 3}, {"metric", "combined_score"}, {"threshold", 0.001}};
  return state;
}
TEST(CheckpointStateTest, RoundTripsCurrentState) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  ASSERT_TRUE(WriteCheckpointState((*workspace)->path(), Example()).ok());
  const auto restored = ReadCheckpointState((*workspace)->path());
  ASSERT_TRUE(restored.ok()) << restored.status();
  EXPECT_EQ(restored->language, "cpp");
  EXPECT_EQ(restored->file_suffix, ".cpp");
  EXPECT_EQ(restored->best_id, "initial");
  EXPECT_EQ(restored->progress.iteration, 3);
  EXPECT_EQ(restored->progress.accepted, 2);
  EXPECT_EQ(restored->progress.rejected, 1);
  EXPECT_EQ(restored->progress.best_score, -10);
  EXPECT_EQ(restored->stopping_policy, Example().stopping_policy);
}
TEST(CheckpointStateTest, RejectsInconsistentCountersAndMissingFields) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  ASSERT_TRUE(WriteCheckpointState((*workspace)->path(), Example()).ok());
  auto text = utils::ReadFile((*workspace)->path() / "checkpoint.json");
  ASSERT_TRUE(text.ok());
  const auto original = Metrics::parse(*text);
  for (const auto& pointer : {"iteration", "accepted", "rejected", "without_improvement"}) {
    for (const Metrics bad :
         {Metrics(-1), Metrics(true), Metrics(0.5), Metrics(std::numeric_limits<std::uint64_t>::max())}) {
      auto value = original;
      value["progress"][pointer] = bad;
      ASSERT_TRUE(utils::WriteFile((*workspace)->path() / "checkpoint.json", value.dump()).ok());
      EXPECT_EQ(ReadCheckpointState((*workspace)->path()).status().code(), absl::StatusCode::kDataLoss);
    }
  }
  for (const auto& item : original.items()) {
    auto value = original;
    value.erase(item.key());
    ASSERT_TRUE(utils::WriteFile((*workspace)->path() / "checkpoint.json", value.dump()).ok());
    EXPECT_FALSE(ReadCheckpointState((*workspace)->path()).ok());
  }
  auto state = Example();
  ++state.progress.rejected;
  EXPECT_FALSE(WriteCheckpointState((*workspace)->path(), state).ok());
}
TEST(CheckpointStateTest, RejectsDuplicateKeysUnknownSchemaAndTrailingContent) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  ASSERT_TRUE(WriteCheckpointState((*workspace)->path(), Example()).ok());
  const auto text = utils::ReadFile((*workspace)->path() / "checkpoint.json");
  ASSERT_TRUE(text.ok());
  auto value = Metrics::parse(*text);
  value["schema_version"] = 2;
  ASSERT_TRUE(utils::WriteFile((*workspace)->path() / "checkpoint.json", value.dump()).ok());
  EXPECT_FALSE(ReadCheckpointState((*workspace)->path()).ok());
  for (const std::string bad :
       std::vector<std::string>{"{\"schema_version\":1," + text->substr(1), *text + "{}", "{"}) {
    ASSERT_TRUE(utils::WriteFile((*workspace)->path() / "checkpoint.json", bad).ok());
    EXPECT_EQ(ReadCheckpointState((*workspace)->path()).status().code(), absl::StatusCode::kDataLoss);
  }
}
}  // namespace
}  // namespace ievolve::controller
