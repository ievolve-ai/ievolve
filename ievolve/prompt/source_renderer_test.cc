#include "ievolve/prompt/source_renderer.h"

#include "gtest/gtest.h"

namespace ievolve {
namespace {

TEST(SourceRendererTest, RendersEachFileUnderItsPathWithItsLanguage) {
  const SourceFiles files = {{"src/a.py", "x = 1\n"}, {"k.cu", "kernel()"}};

  EXPECT_EQ(RenderSourceFiles(files),
            "### File: k.cu\n```cuda\nkernel()\n```\n\n"
            "### File: src/a.py\n```python\nx = 1\n```");
}

TEST(SourceRendererTest, ChangedFilesOmitThoseEqualToTheReference) {
  const SourceFiles reference = {{"a.py", "same\n"}, {"b.py", "old\n"}};
  const SourceFiles program = {{"a.py", "same\n"}, {"b.py", "new\n"}};

  EXPECT_EQ(RenderChangedFiles(program, reference), "### File: b.py\n```python\nnew\n```");
}

TEST(SourceRendererTest, IdenticalProgramsRenderAPlaceholder) {
  const SourceFiles files = {{"a.py", "x\n"}};

  EXPECT_EQ(RenderChangedFiles(files, files), "(identical to the current program)");
}

TEST(SourceRendererTest, FilesMissingFromTheReferenceCountAsChanged) {
  EXPECT_EQ(RenderChangedFiles({{"a.txt", "x"}}, {}), "### File: a.txt\n```text\nx\n```");
}

}  // namespace
}  // namespace ievolve
