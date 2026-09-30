#include "ievolve/prompt/artifact_renderer.h"

#include "gtest/gtest.h"
#include "ievolve/utils/text.h"

namespace ievolve {
namespace {

TEST(ArtifactRendererTest, RendersEveryArtifactAndPreservesOrder) {
  EXPECT_EQ(RenderArtifacts({{"stdout", "hello"}, {"stderr", "oops"}}, 100, false, "Output"),
            "## Output\n\n### stdout\n```\nhello\n```\n\n"
            "### stderr\n```\noops\n```");

  EXPECT_TRUE(RenderArtifacts({}, 100, true, "Output").empty());
}

TEST(ArtifactRendererTest, TruncatesAtUnicodeCharacterBoundaries) {
  EXPECT_EQ(RenderArtifacts({{"log", "你好🙂abc"}}, 3, false, "Output"),
            "## Output\n\n### log\n```\n你好🙂\n... (truncated)\n```");
  EXPECT_EQ(utils::Utf8Length("你好🙂abc"), 6);

  EXPECT_NE(RenderArtifacts({{"log", "x"}}, 0, false, "Output").find("\n... (truncated)"), std::string::npos);
}

TEST(ArtifactRendererTest, DecodesInvalidUtf8AndIncompleteSequences) {
  EXPECT_EQ(DecodeArtifact(std::string("a\xff\xfe", 3), false), "a��");
  EXPECT_EQ(DecodeArtifact(std::string("\xe2\x82", 2), false), "�");
  EXPECT_EQ(DecodeArtifact(std::string("\xe2\x82"
                                       "A",
                                       3),
                           false),
            "�A");

  EXPECT_EQ(DecodeArtifact(std::string("\xed\xa0\x80", 3), false), "���");

  EXPECT_EQ(DecodeArtifact(std::string("a\0b", 3), false), std::string("a\0b", 3));
}

TEST(ArtifactRendererTest, FiltersAnsiAndSecretsBeforeTruncation) {
  const std::string api_key = "sk-" + std::string(48, 'x');

  EXPECT_EQ(DecodeArtifact("\x1b[31mred\x1b[0m password=secret TOKEN: abc " + api_key),
            "red password=<REDACTED> token=<REDACTED> <REDACTED_API_KEY>");

  EXPECT_EQ(DecodeArtifact(std::string(32, 'a')), "<REDACTED_TOKEN>");

  EXPECT_EQ(DecodeArtifact("password=secret", false), "password=secret");
}

TEST(ArtifactRendererTest, HandlesLargeEvaluationOutput) {
  EXPECT_EQ(DecodeArtifact(std::string(256 * 1024, 'a')), "<REDACTED_TOKEN>");

  EXPECT_EQ(RenderArtifacts({{"log", std::string(256 * 1024, '!')}}, 3, true, "Output"),
            "## Output\n\n### log\n```\n!!!\n... (truncated)\n```");
}

}  // namespace
}  // namespace ievolve
