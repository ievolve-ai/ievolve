#include "ievolve/prompt/prompt_program.h"

#include "gtest/gtest.h"

namespace ievolve {
namespace {

TEST(PromptProgramTest, RejectsMalformedTypedMetadata) {
  for (const auto& metadata :
       {Metrics{{"changes", 3}}, Metrics{{"parent_metrics", {1, 2}}}, Metrics{{"parent_metrics", {{"nested", {1, 2}}}}},
        Metrics{{"diverse", "true"}}, Metrics{{"migrant", 1}}, Metrics{{"random", nullptr}},
        Metrics{{"key_features", {1, "x"}}}}) {
    Program program;
    program.id = "a";
    program.metrics = Metrics::object();
    program.metadata = metadata;

    EXPECT_EQ(PromptProgram::FromProgram(program).status().code(), absl::StatusCode::kInvalidArgument) << metadata;
  }
}

}  // namespace
}  // namespace ievolve
