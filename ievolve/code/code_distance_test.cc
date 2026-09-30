#include "ievolve/code/code_distance.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace ievolve {
namespace {

TEST(CodeDistanceTest, CountsInsertionDeletionAndSubstitution) {
  struct Case {
    std::string left;
    std::string right;
    std::size_t expected;
  };

  const std::vector<Case> cases = {
      {"", "", 0},        {"same", "same", 0}, {"", "abc", 3},  {"kitten", "sitting", 3},
      {"abcd", "acd", 1}, {"abc", "axc", 1},   {"ab", "ba", 2}, {std::string("a\0b", 3), "ab", 1}};

  for (const auto& item : cases) {
    SCOPED_TRACE(item.left + " -> " + item.right);

    const auto forward = CodeDistance::Levenshtein(item.left, item.right);
    const auto reverse = CodeDistance::Levenshtein(item.right, item.left);
    ASSERT_TRUE(forward.ok()) << forward.status();
    ASSERT_TRUE(reverse.ok()) << reverse.status();

    EXPECT_EQ(*forward, item.expected);
    EXPECT_EQ(*reverse, item.expected);
  }
}

TEST(CodeDistanceTest, CountsCodePointsWithoutUnicodeNormalization) {
  struct Case {
    std::string left;
    std::string right;
    std::size_t expected;
  };

  const std::vector<Case> cases = {{"你好🙂", "你🙂", 1}, {"é", "e\u0301", 2}, {"", "é中🙂", 3}, {"🙂", "🙃", 1}};

  for (const auto& item : cases) {
    const auto result = CodeDistance::Levenshtein(item.left, item.right);
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_EQ(*result, item.expected);
  }
}

TEST(CodeDistanceTest, RejectsMalformedUtf8EvenForIdenticalInputs) {
  const std::vector<std::string> malformed = {
      "\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf0\x80\x80\xaf", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
      "\xff", "\xc2",     "\xe2\x82",     "\xf0\x9f\x99", "\xe2(\xa1"};

  for (const auto& input : malformed) {
    EXPECT_EQ(CodeDistance::Levenshtein(input, "").status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(CodeDistance::Levenshtein("", input).status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(CodeDistance::Levenshtein(input, input).status().code(), absl::StatusCode::kInvalidArgument);
  }
}

TEST(CodeDistanceTest, HandlesLongUnequalInputs) {
  const std::string input(100000, 'x');

  const auto result = CodeDistance::Levenshtein(input, "x");
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(*result, 99999);
}

}  // namespace
}  // namespace ievolve
