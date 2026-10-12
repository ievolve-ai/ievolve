#include "ievolve/code/project_editor.h"

#include <string>

#include "gtest/gtest.h"

namespace ievolve {
namespace {

const SourceFiles kTwoFiles = {{"src/a.py", "x = 1\ny = 2\n"}, {"src/b.py", "z = 3\n"}};

std::string Block(const std::string& search, const std::string& replace) {
  return "<<<<<<< SEARCH\n" + search + "\n=======\n" + replace + "\n>>>>>>> REPLACE\n";
}

absl::StatusOr<FileDiffs> Group(const std::string& response, const SourceFiles& files,
                                std::optional<std::string_view> description = std::nullopt) {
  auto blocks = CodeParser::ExtractDiffs(response);
  if (!blocks.ok()) return blocks.status();
  return ProjectEditor::GroupDiffs(*blocks, response, files, description);
}

TEST(ProjectEditorTest, ExtractDiffsRecordsBlockOffsets) {
  const std::string response = "intro\n" + Block("a", "b");

  const auto blocks = CodeParser::ExtractDiffs(response);

  ASSERT_TRUE(blocks.ok());
  ASSERT_EQ(blocks->size(), 1u);
  EXPECT_EQ((*blocks)[0].begin, 6u);
  EXPECT_EQ((*blocks)[0].end, response.size() - 1);
}

TEST(ProjectEditorTest, PathLinePrecedingEachBlockSelectsTheFile) {
  const std::string response = "First:\nsrc/b.py\n" + Block("z = 3", "z = 4") + "\n### File: `src/a.py`\n" +
                               Block("x = 1", "x = 10") + "**src/a.py**:\n" + Block("y = 2", "y = 20");

  const auto grouped = Group(response, kTwoFiles);

  ASSERT_TRUE(grouped.ok()) << grouped.status();
  ASSERT_EQ(grouped->files.size(), 2u);
  ASSERT_EQ(grouped->files.at("src/a.py").size(), 2u);
  EXPECT_EQ(grouped->files.at("src/a.py")[0].search, "x = 1");
  EXPECT_EQ(grouped->files.at("src/a.py")[1].search, "y = 2");
  EXPECT_EQ(grouped->files.at("src/b.py")[0].replacement, "z = 4");
}

TEST(ProjectEditorTest, FenceLinesBetweenPathAndBlockAreSkipped) {
  const std::string response = "src/b.py\n```python\n" + Block("z = 3", "z = 4") + "```\n";

  const auto grouped = Group(response, kTwoFiles);

  ASSERT_TRUE(grouped.ok()) << grouped.status();
  EXPECT_EQ(grouped->files.count("src/b.py"), 1u);
}

TEST(ProjectEditorTest, AdjacentBlocksInheritThePreviousPath) {
  const std::string response = "src/a.py\n" + Block("x = 1", "x = 10") + "\n" + Block("y = 2", "y = 20");

  const auto grouped = Group(response, kTwoFiles);

  ASSERT_TRUE(grouped.ok()) << grouped.status();
  EXPECT_EQ(grouped->files.at("src/a.py").size(), 2u);
}

TEST(ProjectEditorTest, UnknownOrMissingPathIsRejectedForSeveralFiles) {
  EXPECT_EQ(Group("src/c.py\n" + Block("x", "y"), kTwoFiles).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(Group("Change this:\n" + Block("x", "y"), kTwoFiles).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(Group(Block("x", "y"), kTwoFiles).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ProjectEditorTest, SingleFileAcceptsUnnamedBlocks) {
  const SourceFiles one = {{"main.py", "x = 1\n"}};

  const auto grouped = Group("Here is my change:\n" + Block("x = 1", "x = 2"), one);

  ASSERT_TRUE(grouped.ok()) << grouped.status();
  EXPECT_EQ(grouped->files.at("main.py").size(), 1u);
}

TEST(ProjectEditorTest, ChangesDescriptionPathTargetsTheDescription) {
  const std::string response =
      "CHANGES_DESCRIPTION\n" + Block("old idea", "new idea") + "src/a.py\n" + Block("x = 1", "x = 2");

  const auto grouped = Group(response, kTwoFiles, std::string_view("old idea\n"));

  ASSERT_TRUE(grouped.ok()) << grouped.status();
  ASSERT_EQ(grouped->changes_description.size(), 1u);
  EXPECT_EQ(grouped->changes_description[0].replacement, "new idea");
  EXPECT_EQ(grouped->files.at("src/a.py").size(), 1u);
}

TEST(ProjectEditorTest, ChangesDescriptionPathWithoutDescriptionIsUnknown) {
  EXPECT_EQ(Group("CHANGES_DESCRIPTION\n" + Block("a", "b"), kTwoFiles).status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(ProjectEditorTest, SingleFileUnnamedBlocksAreRoutedByContentWithDescription) {
  const SourceFiles one = {{"main.py", "x = 1\n"}};
  const std::string response = Block("x = 1", "x = 2") + Block("old idea", "new idea") + Block("nowhere", "z");

  const auto grouped = Group(response, one, std::string_view("old idea\n"));

  ASSERT_TRUE(grouped.ok()) << grouped.status();
  ASSERT_EQ(grouped->files.at("main.py").size(), 1u);
  EXPECT_EQ(grouped->files.at("main.py")[0].replacement, "x = 2");
  ASSERT_EQ(grouped->changes_description.size(), 1u);
  EXPECT_EQ(grouped->changes_description[0].replacement, "new idea");
}

TEST(ProjectEditorTest, ApplyDiffsEditsOnlyTargetedFiles) {
  FileDiffs diffs;
  diffs.files["src/a.py"] = {{"x = 1", "x = 10"}, {"missing", "nothing"}};

  const auto applied = ProjectEditor::ApplyDiffs(kTwoFiles, diffs);

  EXPECT_EQ(applied.applied_count, 1u);
  EXPECT_EQ(applied.files.at("src/a.py"), "x = 10\ny = 2\n");
  EXPECT_EQ(applied.files.at("src/b.py"), "z = 3\n");
  EXPECT_EQ(kTwoFiles.at("src/a.py"), "x = 1\ny = 2\n");
}

TEST(ProjectEditorTest, RewriteReplacesNamedFilesExactly) {
  const std::string response = "Updated files:\n\nsrc/b.py\n```python\nz = 30\n\n```\nAnd unchanged a.\n";

  const auto files = ProjectEditor::ParseRewrite(response, kTwoFiles);

  ASSERT_TRUE(files.ok()) << files.status();
  EXPECT_EQ(files->at("src/a.py"), kTwoFiles.at("src/a.py"));
  EXPECT_EQ(files->at("src/b.py"), "z = 30\n\n");
}

TEST(ProjectEditorTest, RewriteRejectsUnknownUnnamedDuplicateOrMissingBlocks) {
  const auto code = [&](const std::string& response) {
    return ProjectEditor::ParseRewrite(response, kTwoFiles).status().code();
  };

  EXPECT_EQ(code("src/c.py\n```python\nx\n```\n"), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(code("```python\nx\n```\n"), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(code("src/a.py\n```\nx\n```\nsrc/a.py\n```\ny\n```\n"), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(code("src/a.py\n```\nunterminated\n"), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(code("no code at all"), absl::StatusCode::kInvalidArgument);
}

TEST(ProjectEditorTest, SingleFileRewriteMatchesParseFullRewrite) {
  const SourceFiles one = {{"main.py", "old\n"}};
  const std::string response = "Sure:\n```python\nnew code\n```\n";

  const auto files = ProjectEditor::ParseRewrite(response, one);

  ASSERT_TRUE(files.ok()) << files.status();
  EXPECT_EQ(files->at("main.py"), CodeParser::ParseFullRewrite(response, "python"));
}

TEST(ProjectEditorTest, EnforceEditableBlocksRevertsEditsOutsideBlocksPerFile) {
  const SourceFiles original = {
      {"a.py", "head\n# EVOLVE-BLOCK-START\nbody\n# EVOLVE-BLOCK-END\ntail\n"},
      {"b.py", "free\n"},
  };
  const SourceFiles proposed = {
      {"a.py", "HEAD\n# EVOLVE-BLOCK-START\nBODY\n# EVOLVE-BLOCK-END\ntail\n"},
      {"b.py", "FREE\n"},
  };

  const auto enforced = ProjectEditor::EnforceEditableBlocks(original, proposed);

  ASSERT_TRUE(enforced.ok()) << enforced.status();
  EXPECT_EQ(enforced->at("a.py"), "head\n# EVOLVE-BLOCK-START\nBODY\n# EVOLVE-BLOCK-END\ntail\n");
  EXPECT_EQ(enforced->at("b.py"), "FREE\n");
}

TEST(ProjectEditorTest, EnforceEditableBlocksRequiresTheSamePaths) {
  EXPECT_EQ(ProjectEditor::EnforceEditableBlocks({{"a.py", "x"}}, {{"b.py", "x"}}).status().code(),
            absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace ievolve
