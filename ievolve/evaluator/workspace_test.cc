#include "ievolve/evaluator/workspace.h"

#include <future>
#include <set>

#include "gtest/gtest.h"
#include "ievolve/utils/file.h"

namespace ievolve::evaluator {
namespace {
TEST(WorkspaceTest, WritesSourceAndCleansAllOutputs) {
  std::filesystem::path path;
  {
    auto workspace = EvaluationWorkspace::Create();
    ASSERT_TRUE(workspace.ok()) << workspace.status();
    path = (*workspace)->path();
    auto source = (*workspace)->WriteSource("int main() {}", ".cpp");
    ASSERT_TRUE(source.ok());
    EXPECT_EQ(*utils::ReadFile(*source), "int main() {}");
    EXPECT_TRUE(utils::WriteFile(path / "candidate", "build output").ok());
    EXPECT_FALSE((*workspace)->WriteSource("bad", "/../../escape").ok());
  }
  EXPECT_FALSE(std::filesystem::exists(path));
}
TEST(WorkspaceTest, ConcurrentWorkspacesRemainDistinct) {
  std::vector<std::future<absl::StatusOr<std::unique_ptr<EvaluationWorkspace>>>> futures;
  for (int i = 0; i < 8; ++i) futures.push_back(std::async(std::launch::async, EvaluationWorkspace::Create));
  std::set<std::filesystem::path> paths;
  std::vector<std::unique_ptr<EvaluationWorkspace>> owners;
  for (auto& future : futures) {
    auto workspace = future.get();
    ASSERT_TRUE(workspace.ok());
    EXPECT_TRUE(paths.insert((*workspace)->path()).second);
    owners.push_back(std::move(*workspace));
  }
}
}  // namespace
}  // namespace ievolve::evaluator
