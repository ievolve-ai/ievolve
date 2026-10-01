#include "ievolve/utils/file.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "gtest/gtest.h"

namespace ievolve::utils {
namespace {
namespace fs = std::filesystem;

class FileTest : public testing::Test {
 protected:
  void SetUp() override {
    std::random_device random;
    directory_ = fs::temp_directory_path() / ("ievolve-file-test-" + std::to_string(random()));
    ASSERT_TRUE(fs::create_directory(directory_));
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(directory_, ignored);
  }

  fs::path directory_;
};

TEST_F(FileTest, WriteThenReadRoundTripsBinaryContent) {
  const std::string content("a\0b\r\n\xff", 6);

  ASSERT_TRUE(WriteFile(directory_ / "data", content).ok());

  const auto read = ReadFile(directory_ / "data");
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(*read, content);
}

TEST_F(FileTest, WriteTruncatesExistingFile) {
  ASSERT_TRUE(WriteFile(directory_ / "data", "longer content").ok());

  ASSERT_TRUE(WriteFile(directory_ / "data", "short").ok());

  const auto read = ReadFile(directory_ / "data");
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(*read, "short");
}

TEST_F(FileTest, ReadsEmptyFile) {
  ASSERT_TRUE(WriteFile(directory_ / "empty", "").ok());

  const auto read = ReadFile(directory_ / "empty");

  ASSERT_TRUE(read.ok());
  EXPECT_TRUE(read->empty());
}

TEST_F(FileTest, ReadFollowsSymlinks) {
  ASSERT_TRUE(WriteFile(directory_ / "target", "linked").ok());
  fs::create_symlink(directory_ / "target", directory_ / "link");

  const auto read = ReadFile(directory_ / "link");

  ASSERT_TRUE(read.ok());
  EXPECT_EQ(*read, "linked");
}

TEST_F(FileTest, ReadMissingFileIsNotFound) {
  const auto read = ReadFile(directory_ / "missing");

  EXPECT_EQ(read.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(FileTest, ReadDirectoryIsFailedPrecondition) {
  const auto read = ReadFile(directory_);

  EXPECT_EQ(read.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(FileTest, WriteIntoMissingDirectoryIsPermissionDenied) {
  const auto status = WriteFile(directory_ / "missing" / "data", "x");

  EXPECT_EQ(status.code(), absl::StatusCode::kPermissionDenied);
}

}  // namespace
}  // namespace ievolve::utils
