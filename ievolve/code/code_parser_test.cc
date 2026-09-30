#include "ievolve/code/code_parser.h"

#include "gtest/gtest.h"
#include "ievolve/code/utest_helpers/golden.h"
#include "ievolve/config/config.h"

namespace ievolve {
namespace {
using code_test::Blocks;
using code_test::Json;
using code_test::Pairs;

class DiffGoldenTest : public testing::TestWithParam<Json> {};
TEST_P(DiffGoldenTest, MatchesPython) {
  const auto& test = GetParam();
  const auto& input = test.at("input");
  const auto op = test.at("operation").get<std::string>();
  Json actual;
  absl::Status status;

  if (op == "extract_diffs") {
    auto result = CodeParser::ExtractDiffs(input.at("diff_text").get<std::string>(),
                                           input.value("diff_pattern", std::string(CodeParser::kDefaultDiffPattern)));
    status = result.status();
    if (result.ok()) actual = Pairs(*result);
  } else if (op == "apply_diff") {
    auto result =
        CodeParser::ApplyDiff(input.at("original_code").get<std::string>(), input.at("diff_text").get<std::string>());
    status = result.status();
    if (result.ok()) actual = result->text;
  } else if (op == "apply_diff_blocks") {
    auto result =
        CodeParser::ApplyDiffBlocks(input.at("original_text").get<std::string>(), Blocks(input.at("diff_blocks")));
    actual = Json::array({result.text, result.applied_count});
  } else {
    auto result =
        CodeParser::SplitDiffsByTarget(Blocks(input.at("diff_blocks")), input.at("code_text").get<std::string>(),
                                       input.at("changes_description_text").get<std::string>());
    status = result.status();
    if (result.ok()) {
      actual = Json::array({Pairs(result->code), Pairs(result->changes_description), Pairs(result->unmatched)});
    }
  }

  if (test.contains("error")) {
    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument) << status;
  } else {
    ASSERT_TRUE(status.ok()) << status;
    EXPECT_EQ(actual, test.at("expected"));
  }
}
INSTANTIATE_TEST_SUITE_P(Python, DiffGoldenTest,
                         testing::ValuesIn(code_test::Cases({"extract_diffs", "apply_diff", "apply_diff_blocks",
                                                             "split_diffs_by_target"})),
                         [](const auto& info) {
                           return info.param.at("operation").template get<std::string>() + "_" +
                                  info.param.at("name").template get<std::string>();
                         });

class RewriteGoldenTest : public testing::TestWithParam<Json> {};
TEST_P(RewriteGoldenTest, MatchesPython) {
  const auto& input = GetParam().at("input");
  EXPECT_EQ(CodeParser::ParseFullRewrite(input.at("llm_response").get<std::string>(),
                                         input.at("language").get<std::string>()),
            GetParam().at("expected"));
}
INSTANTIATE_TEST_SUITE_P(Python, RewriteGoldenTest, testing::ValuesIn(code_test::Cases({"parse_full_rewrite"})),
                         [](const auto& info) { return info.param.at("name").template get<std::string>(); });

class SummaryGoldenTest : public testing::TestWithParam<Json> {};
TEST_P(SummaryGoldenTest, MatchesPython) {
  const auto& input = GetParam().at("input");
  auto result =
      CodeParser::FormatDiffSummary(Blocks(input.at("diff_blocks")), input.at("max_line_len"), input.at("max_lines"));
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(*result, GetParam().at("expected"));
}
INSTANTIATE_TEST_SUITE_P(Python, SummaryGoldenTest, testing::ValuesIn(code_test::Cases({"format_diff_summary"})),
                         [](const auto& info) { return info.param.at("name").template get<std::string>(); });

TEST(CodeParserTest, ConfigDefaultValidatesBeforeApplyingAnyBlock) {
  const Config config;
  auto result = CodeParser::ApplyDiff("x\ny",
                                      "<<<<<<< SEARCH\nx\n=======\nX\n>>>>>>> REPLACE\n"
                                      "<<<<<<< SEARCH\ny\n=======\nY\n=======\n>>>>>>> REPLACE",
                                      config.diff_pattern);
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);

  auto valid = CodeParser::ApplyDiff("x\n", "<<<<<<< SEARCH\nx\n=======\nX\n>>>>>>> REPLACE", config.diff_pattern);
  ASSERT_TRUE(valid.ok()) << valid.status();

  EXPECT_EQ(valid->text, "X\n");
  EXPECT_EQ(valid->applied_count, 1);
}

TEST(CodeParserTest, RejectsInvalidCustomPatternsAndMissingCaptures) {
  for (const auto& pattern : {"[", "abc", "(abc)", "(a)?(b)"}) {
    auto result = CodeParser::ExtractDiffs("b", pattern);
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument) << pattern;
  }
}

TEST(CodeParserTest, CustomDotAllPreservesEscapedDotsAndCharacterClasses) {
  auto result = CodeParser::ExtractDiffs("a.\nb|xy\nz", R"((a\..*?)\|([xyz\n.]*))");
  ASSERT_TRUE(result.ok()) << result.status();

  ASSERT_EQ(result->size(), 1);
  EXPECT_EQ(result->front().search, "a.\nb");
  EXPECT_EQ(result->front().replacement, "xy\nz");
}

TEST(CodeParserTest, HandlesLargeStandardDiffWithoutRecursiveRegex) {
  const std::string text(1024 * 1024, 'x');

  auto result = CodeParser::ApplyDiff(text, "<<<<<<< SEARCH\n" + text + "\n=======\nnew\n>>>>>>> REPLACE");
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->text, "new");
  EXPECT_EQ(result->applied_count, 1);
}

TEST(CodeParserTest, CustomDotAllPreservesPosixBracketExpressions) {
  for (const auto& pattern : {R"(([[:alpha:].]+):(.*))", R"(([[.a.].]+):(.*))", R"(([[=a=].]+):(.*))"}) {
    auto result = CodeParser::ExtractDiffs("a.a:new\nline", pattern);
    ASSERT_TRUE(result.ok()) << pattern << ": " << result.status();

    ASSERT_EQ(result->size(), 1);
    EXPECT_EQ(result->front().search, "a.a");
    EXPECT_EQ(result->front().replacement, "new\nline");
  }
}

TEST(CodeParserTest, TreatsRewriteLanguageAsLiteral) {
  EXPECT_EQ(CodeParser::ParseFullRewrite("```cc\nwrong```\n```c++\nint main() {}\n```", "c++"), "int main() {}");
  EXPECT_EQ(CodeParser::ParseFullRewrite("```[\nx\n```", "["), "x");
}

TEST(CodeParserTest, RejectsInvalidSummaryLimits) {
  EXPECT_EQ(CodeParser::FormatDiffSummary({}, 2, 30).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(CodeParser::FormatDiffSummary({}, 100, -1).status().code(), absl::StatusCode::kInvalidArgument);
}

class EditableBlocksGoldenTest : public testing::TestWithParam<Json> {};
TEST_P(EditableBlocksGoldenTest, MatchesPython) {
  const auto& test = GetParam();
  const auto& input = test.at("input");

  if (test.at("operation") == "parse_evolve_blocks") {
    Json actual = Json::array();
    for (const auto& block : CodeParser::ParseEditableBlocks(input.at("code").get<std::string>())) {
      actual.push_back({block.start_line, block.end_line, block.content});
    }

    EXPECT_EQ(actual, test.at("expected"));
  } else {
    auto result = CodeParser::EnforceEditableBlocks(input.at("original_code").get<std::string>(),
                                                    input.at("new_code").get<std::string>());

    if (test.contains("error")) {
      EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    } else {
      ASSERT_TRUE(result.ok()) << result.status();
      EXPECT_EQ(*result, test.at("expected"));
    }
  }
}
INSTANTIATE_TEST_SUITE_P(Python, EditableBlocksGoldenTest,
                         testing::ValuesIn(code_test::Cases({"parse_evolve_blocks", "enforce_evolve_blocks"})),
                         [](const auto& info) {
                           return info.param.at("operation").template get<std::string>() + "_" +
                                  info.param.at("name").template get<std::string>();
                         });

TEST(EditableBlocksTest, EnforcingRestoredCodeIsIdempotent) {
  const std::string original = "head\n# EVOLVE-BLOCK-START\na\n# EVOLVE-BLOCK-END\ntail\n";

  auto first = CodeParser::EnforceEditableBlocks(original, "HEAD\n//EVOLVE-BLOCK-START\nb\n//EVOLVE-BLOCK-END\nTAIL");
  ASSERT_TRUE(first.ok()) << first.status();

  auto second = CodeParser::EnforceEditableBlocks(original, *first);
  ASSERT_TRUE(second.ok()) << second.status();

  EXPECT_EQ(*second, "head\n# EVOLVE-BLOCK-START\nb\n# EVOLVE-BLOCK-END\ntail\n");
}

}  // namespace
}  // namespace ievolve
