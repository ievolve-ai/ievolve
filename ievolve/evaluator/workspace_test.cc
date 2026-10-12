#include "ievolve/evaluator/workspace.h"

#include <future>
#include <iterator>
#include <random>
#include <set>

#include "gtest/gtest.h"
#include "ievolve/utils/file.h"

namespace ievolve::evaluator {
namespace {
TEST(WorkspaceTest, EmptyProjectDirMaterializesOnlyFilesAndCleansAllOutputs) {
  std::filesystem::path path;
  {
    auto workspace = EvaluationWorkspace::Create();
    ASSERT_TRUE(workspace.ok()) << workspace.status();
    path = (*workspace)->path();
    auto root = (*workspace)->MaterializeProject({}, {{"src/main.cpp", "int main() {}"}});
    ASSERT_TRUE(root.ok()) << root.status();
    EXPECT_EQ(root->parent_path(), path);
    EXPECT_EQ(*utils::ReadFile(*root / "src/main.cpp"), "int main() {}");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(*root), {}), 1);
    EXPECT_TRUE(utils::WriteFile(path / "candidate", "build output").ok());
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

class MaterializeProjectTest : public testing::Test {
 protected:
  void SetUp() override {
    std::random_device random;
    project_ = std::filesystem::temp_directory_path() / ("ievolve-project-test-" + std::to_string(random()));
    ASSERT_TRUE(std::filesystem::create_directories(project_ / "src"));
    ASSERT_TRUE(std::filesystem::create_directories(project_ / ".git"));
    ASSERT_TRUE(utils::WriteFile(project_ / "src/a.py", "original").ok());
    ASSERT_TRUE(utils::WriteFile(project_ / "helper.py", "helper").ok());
    ASSERT_TRUE(utils::WriteFile(project_ / ".git/HEAD", "ref").ok());
    std::filesystem::create_symlink("helper.py", project_ / "link.py");
    auto workspace = EvaluationWorkspace::Create();
    ASSERT_TRUE(workspace.ok());
    workspace_ = std::move(*workspace);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(project_, ignored);
  }

  std::filesystem::path project_;
  std::unique_ptr<EvaluationWorkspace> workspace_;
};

TEST_F(MaterializeProjectTest, CopiesTheProjectAndOverlaysCandidateFiles) {
  auto root = workspace_->MaterializeProject(project_, {{"src/a.py", "evolved"}, {"new/dir/b.py", "added"}});

  ASSERT_TRUE(root.ok()) << root.status();
  EXPECT_EQ(root->parent_path(), workspace_->path());
  EXPECT_EQ(*utils::ReadFile(*root / "src/a.py"), "evolved");
  EXPECT_EQ(*utils::ReadFile(*root / "new/dir/b.py"), "added");
  EXPECT_EQ(*utils::ReadFile(*root / "helper.py"), "helper");
  EXPECT_FALSE(std::filesystem::exists(*root / ".git"));
  ASSERT_TRUE(std::filesystem::is_symlink(*root / "link.py"));
  EXPECT_EQ(std::filesystem::read_symlink(*root / "link.py"), "helper.py");
  EXPECT_EQ(*utils::ReadFile(project_ / "src/a.py"), "original");
}

TEST_F(MaterializeProjectTest, RejectsInvalidPathsMissingProjectAndRepeatedCalls) {
  EXPECT_EQ(workspace_->MaterializeProject(project_, {{"../escape.py", "x"}}).status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(workspace_->MaterializeProject(project_ / "missing", {{"a.py", "x"}}).status().code(),
            absl::StatusCode::kFailedPrecondition);

  ASSERT_TRUE(workspace_->MaterializeProject(project_, {{"src/a.py", "x"}}).ok());
  EXPECT_EQ(workspace_->MaterializeProject(project_, {{"src/a.py", "x"}}).status().code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST_F(MaterializeProjectTest, CandidateFileCannotReplaceASymlinkedPath) {
  EXPECT_EQ(workspace_->MaterializeProject(project_, {{"link.py", "x"}}).status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(*utils::ReadFile(project_ / "helper.py"), "helper");
}
}  // namespace
}  // namespace ievolve::evaluator
