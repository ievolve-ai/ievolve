#include "ievolve/program/source_files.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "gtest/gtest.h"

namespace ievolve {
namespace {
namespace fs = std::filesystem;

TEST(SourceFilesTest, ValidPathsAreNormalizedRelativePaths) {
  for (const char* path : {"a.py", "src/a.py", "src/deep/b.cc", ".hidden/x"}) {
    EXPECT_TRUE(ValidateSourcePath(path).ok()) << path;
  }
  for (const char* path : {"", "/a.py", "a//b", "./a", "a/../b", "..", "a/", "a\\b"}) {
    EXPECT_EQ(ValidateSourcePath(path).code(), absl::StatusCode::kInvalidArgument) << path;
  }
  EXPECT_EQ(ValidateSourcePath(std::string("a\0b", 3)).code(), absl::StatusCode::kInvalidArgument);
}

TEST(SourceFilesTest, ValidateRequiresNonemptyUtf8Files) {
  EXPECT_TRUE(ValidateSourceFiles({{"a.py", "x"}, {"b/c.py", ""}}).ok());
  EXPECT_EQ(ValidateSourceFiles({}).code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(ValidateSourceFiles({{"../a.py", "x"}}).code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(ValidateSourceFiles({{"a.py", "\xff"}}).code(), absl::StatusCode::kInvalidArgument);
}

TEST(SourceFilesTest, TotalLengthCountsCodePointsAcrossFiles) {
  EXPECT_EQ(TotalLength({}), 0u);
  EXPECT_EQ(TotalLength({{"a.py", "abc"}, {"b.py", "\xc3\xa9"}}), 4u);
}

TEST(SourceFilesTest, LanguageFollowsTheExtensionCaseInsensitively) {
  EXPECT_EQ(LanguageOf("src/a.py"), "python");
  EXPECT_EQ(LanguageOf("A.CPP"), "cpp");
  EXPECT_EQ(LanguageOf("k.h"), "cpp");
  EXPECT_EQ(LanguageOf("k.cu"), "cuda");
  EXPECT_EQ(LanguageOf("lib.rs"), "rust");
  EXPECT_EQ(LanguageOf("Makefile"), "text");
  EXPECT_EQ(LanguageOf("notes.unknown"), "text");
  EXPECT_EQ(LanguageOf(".py/readme"), "text");
}

class ReadSourceFilesTest : public testing::Test {
 protected:
  void SetUp() override {
    std::random_device random;
    root_ = fs::temp_directory_path() / ("ievolve-source-files-test-" + std::to_string(random()));
    ASSERT_TRUE(fs::create_directories(root_ / "src"));
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  void Write(const std::string& path, const std::string& content) {
    std::ofstream(root_ / path, std::ios::binary) << content;
  }

  fs::path root_;
};

TEST_F(ReadSourceFilesTest, ReadsEveryListedFile) {
  Write("a.py", "print(1)\n");
  Write("src/b.py", "x = 2\n");

  const auto files = ReadSourceFiles(root_, {"src/b.py", "a.py"});

  ASSERT_TRUE(files.ok()) << files.status();
  EXPECT_EQ(*files, (SourceFiles{{"a.py", "print(1)\n"}, {"src/b.py", "x = 2\n"}}));
}

TEST_F(ReadSourceFilesTest, MissingInvalidOrNonUtf8FilesFail) {
  Write("bad.py", "\xff");

  EXPECT_EQ(ReadSourceFiles(root_, {"missing.py"}).status().code(), absl::StatusCode::kNotFound);
  EXPECT_EQ(ReadSourceFiles(root_, {"../x.py"}).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(ReadSourceFiles(root_, {"bad.py"}).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(ReadSourceFiles(root_, {}).status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace ievolve
