#include "ievolve/controller/evaluation_report.h"

#include "gtest/gtest.h"
#include "ievolve/evaluator/workspace.h"
#include "ievolve/utils/file.h"

namespace ievolve::controller {
namespace {
TEST(EvaluationReportTest, PublishesSourceVerdictAndMetadataAtomically) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  RejectedEvaluation rejected;
  rejected.candidate.id = "iteration-1";
  rejected.candidate.code = "int main() {}";
  rejected.candidate.language = "cpp";
  rejected.candidate.parent_id = "initial";
  rejected.outcome.reason = "wrong_answer";
  rejected.outcome.result.artifacts["case"] = "input 1";
  ASSERT_TRUE(WriteEvaluationReport((*workspace)->path(), rejected).ok());
  auto text = utils::ReadFile((*workspace)->path() / "evaluations/iteration-1.json");
  ASSERT_TRUE(text.ok());
  auto json = Metrics::parse(*text);
  EXPECT_EQ(json["candidate"]["code"], rejected.candidate.code);
  EXPECT_EQ(json["candidate"]["parent_id"], "initial");
  EXPECT_EQ(json["outcome"]["reason"], "wrong_answer");
  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator((*workspace)->path() / "evaluations")) {
    EXPECT_EQ(entry.path().filename(), "iteration-1.json");
    ++count;
  }
  EXPECT_EQ(count, 1);
  rejected.outcome.passed = true;
  EXPECT_FALSE(WriteEvaluationReport((*workspace)->path(), rejected).ok());
  EXPECT_EQ(*utils::ReadFile((*workspace)->path() / "evaluations/iteration-1.json"), *text);
}
TEST(EvaluationReportTest, RejectsUnsafeIdentifiersAndReportsPublicationFailure) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  RejectedEvaluation rejected;
  rejected.outcome.reason = "wrong_answer";
  for (const char* id : {"../escape", "arbitrary", "iteration--1", "iteration-1/../x", "iteration-"}) {
    rejected.candidate.id = id;
    EXPECT_FALSE(WriteEvaluationReport((*workspace)->path(), rejected).ok());
  }
  rejected.candidate.id = "initial";
  ASSERT_TRUE(utils::WriteFile((*workspace)->path() / "evaluations", "block").ok());
  EXPECT_FALSE(WriteEvaluationReport((*workspace)->path(), rejected).ok());
}
}  // namespace
}  // namespace ievolve::controller
