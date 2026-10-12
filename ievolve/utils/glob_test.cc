#include "ievolve/utils/glob.h"

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "ievolve/utils/file.h"

namespace ievolve::utils {
namespace {
namespace fs = std::filesystem;
using Paths = std::vector<std::string>;

TEST(GlobTest, ValidateAcceptsRelativePatterns) {
  for (const char* pattern : {"a.py", "src/*.py", "**/*.cc", "src/**/x?.h", "**"}) {
    EXPECT_TRUE(ValidateGlob(pattern).ok()) << pattern;
  }
}

TEST(GlobTest, ValidateRejectsMalformedPatterns) {
  for (const char* pattern : {"", "/abs/*.py", "../x.py", "a/../b", "a//b", "./a", "a/", "a**/b", "a/**b", "x\\y"}) {
    EXPECT_EQ(ValidateGlob(pattern).code(), absl::StatusCode::kInvalidArgument) << pattern;
  }
}

TEST(GlobTest, StarAndQuestionMarkStayWithinOneSegment) {
  EXPECT_TRUE(MatchGlob("*.py", "a.py"));
  EXPECT_FALSE(MatchGlob("*.py", "src/a.py"));
  EXPECT_TRUE(MatchGlob("src/?.py", "src/a.py"));
  EXPECT_FALSE(MatchGlob("src/?.py", "src/ab.py"));
  EXPECT_FALSE(MatchGlob("a?b", "a/b"));
  EXPECT_TRUE(MatchGlob("*", "file"));
  EXPECT_FALSE(MatchGlob("*", "dir/file"));
}

TEST(GlobTest, DoubleStarMatchesZeroOrMoreDirectories) {
  EXPECT_TRUE(MatchGlob("**/*.py", "a.py"));
  EXPECT_TRUE(MatchGlob("**/*.py", "x/y/a.py"));
  EXPECT_TRUE(MatchGlob("src/**/a.py", "src/a.py"));
  EXPECT_TRUE(MatchGlob("src/**/a.py", "src/x/y/a.py"));
  EXPECT_FALSE(MatchGlob("src/**/a.py", "lib/a.py"));
  EXPECT_TRUE(MatchGlob("src/**", "src/x/y"));
  EXPECT_TRUE(MatchGlob("**", "x/y"));
}

class GlobExpandTest : public testing::Test {
 protected:
  void SetUp() override {
    std::random_device random;
    root_ = fs::temp_directory_path() / ("ievolve-glob-test-" + std::to_string(random()));
    ASSERT_TRUE(fs::create_directory(root_));
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  void Touch(const std::string& path) {
    fs::create_directories((root_ / path).parent_path());
    ASSERT_TRUE(WriteFile(root_ / path, "x").ok());
  }

  fs::path root_;
};

TEST_F(GlobExpandTest, ReturnsSortedUniqueRegularFiles) {
  for (const char* path : {"main.py", "src/b.py", "src/a.py", "src/deep/c.py", "src/notes.txt"}) Touch(path);

  const auto files = ExpandGlobs(root_, {"src/**/*.py", "*.py", "src/a.py"});

  ASSERT_TRUE(files.ok()) << files.status();
  EXPECT_EQ(*files, (Paths{"main.py", "src/a.py", "src/b.py", "src/deep/c.py"}));
}

TEST_F(GlobExpandTest, SkipsGitDirectoryAndSymlinks) {
  Touch("a.py");
  Touch(".git/hooks/x.py");
  Touch("other/b.py");
  fs::create_symlink(root_ / "a.py", root_ / "link.py");
  fs::create_directory_symlink(root_ / "other", root_ / "linked");

  const auto files = ExpandGlobs(root_, {"**/*.py"});

  ASSERT_TRUE(files.ok()) << files.status();
  EXPECT_EQ(*files, (Paths{"a.py", "other/b.py"}));
}

TEST_F(GlobExpandTest, PatternWithoutMatchesIsAnError) {
  Touch("a.py");

  const auto files = ExpandGlobs(root_, {"*.py", "*.cc"});

  EXPECT_EQ(files.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(files.status().message().find("[1]"), std::string::npos) << files.status();
}

TEST_F(GlobExpandTest, RejectsInvalidPatternsAndMissingRoot) {
  Touch("a.py");

  EXPECT_EQ(ExpandGlobs(root_, {}).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(ExpandGlobs(root_, {"../a.py"}).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(ExpandGlobs(root_ / "missing", {"*.py"}).status().code(), absl::StatusCode::kNotFound);
}

}  // namespace
}  // namespace ievolve::utils
