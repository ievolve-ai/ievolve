#include "ievolve/controller/staging_directory.h"

#include "gtest/gtest.h"
#include "ievolve/evaluator/workspace.h"
#include "ievolve/utils/file.h"

namespace ievolve::controller {
namespace {
namespace fs = std::filesystem;

TEST(StagingDirectoryTest, CreatesHiddenDirectoryUnderParentAndRemovesItOnDestruction) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  const auto parent = (*workspace)->path() / "missing/parent";
  fs::path path;
  {
    StagingDirectory staged(parent);
    path = staged.path();
    EXPECT_EQ(path.parent_path(), parent);
    EXPECT_EQ(path.filename().string().rfind(".controller-", 0), 0u);
    EXPECT_TRUE(fs::is_directory(path));
    ASSERT_TRUE(utils::WriteFile(path / "file", "content").ok());
  }
  EXPECT_FALSE(fs::exists(path));
}

TEST(StagingDirectoryTest, PublishRenamesTheWholeDirectoryAndKeepsIt) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  const auto destination = (*workspace)->path() / "published";
  fs::path path;
  {
    StagingDirectory staged((*workspace)->path());
    path = staged.path();
    ASSERT_TRUE(utils::WriteFile(path / "file", "content").ok());
    staged.Publish(destination);
  }
  EXPECT_FALSE(fs::exists(path));
  EXPECT_EQ(*utils::ReadFile(destination / "file"), "content");
}

TEST(StagingDirectoryTest, PublishContentsReplacesFilesInAnExistingDirectory) {
  auto workspace = evaluator::EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  const auto destination = (*workspace)->path() / "best";
  fs::create_directories(destination);
  ASSERT_TRUE(utils::WriteFile(destination / "a", "old").ok());
  ASSERT_TRUE(utils::WriteFile(destination / "keep", "untouched").ok());
  fs::path path;
  {
    StagingDirectory staged((*workspace)->path());
    path = staged.path();
    ASSERT_TRUE(utils::WriteFile(path / "a", "new").ok());
    ASSERT_TRUE(utils::WriteFile(path / "b", "added").ok());
    staged.PublishContents(destination);
  }
  EXPECT_FALSE(fs::exists(path));
  EXPECT_EQ(*utils::ReadFile(destination / "a"), "new");
  EXPECT_EQ(*utils::ReadFile(destination / "b"), "added");
  EXPECT_EQ(*utils::ReadFile(destination / "keep"), "untouched");
}

}  // namespace
}  // namespace ievolve::controller
