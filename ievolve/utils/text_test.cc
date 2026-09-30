#include "ievolve/utils/text.h"

#include <string>

#include "gtest/gtest.h"

namespace ievolve::utils {
namespace {
using Lines = std::vector<std::string_view>;

TEST(TextTest, IsValidUtf8AcceptsWellFormedTextIncludingEmpty) {
  EXPECT_TRUE(IsValidUtf8(""));
  EXPECT_TRUE(IsValidUtf8("plain ascii"));
  EXPECT_TRUE(IsValidUtf8("héllo 世界 🙂"));

  // Embedded NUL is valid UTF-8; rejecting it is a separate concern that only
  // the argv and stdin callers care about.
  EXPECT_TRUE(IsValidUtf8(std::string("a\0b", 3)));
}

TEST(TextTest, IsValidUtf8RejectsMalformedSequences) {
  EXPECT_FALSE(IsValidUtf8("\xff"));              // never a lead byte
  EXPECT_FALSE(IsValidUtf8("\x80"));              // lone continuation
  EXPECT_FALSE(IsValidUtf8("\xc0\xaf"));          // overlong
  EXPECT_FALSE(IsValidUtf8("\xe4\xb8"));          // truncated three-byte
  EXPECT_FALSE(IsValidUtf8("\xed\xa0\x80"));      // surrogate
  EXPECT_FALSE(IsValidUtf8("\xf5\x80\x80\x80"));  // beyond U+10FFFF
}

TEST(TextTest, IsCodePointStartRejectsOnlyContinuationBytes) {
  EXPECT_TRUE(IsCodePointStart('a'));
  EXPECT_TRUE(IsCodePointStart(0xc2));  // two-byte lead
  EXPECT_TRUE(IsCodePointStart(0xe4));  // three-byte lead
  EXPECT_TRUE(IsCodePointStart(0xf0));  // four-byte lead

  EXPECT_FALSE(IsCodePointStart(0x80));
  EXPECT_FALSE(IsCodePointStart(0xbf));
}

TEST(TextTest, Utf8LengthCountsCodePointsOnWellFormedInput) {
  EXPECT_EQ(Utf8Length(""), 0u);
  EXPECT_EQ(Utf8Length("abc"), 3u);

  // Two three-byte characters, one four-byte emoji, three ASCII.
  EXPECT_EQ(Utf8Length("你好🙂abc"), 6u);
  EXPECT_EQ(Utf8Length("héllo"), 5u);
}

TEST(TextTest, Utf8LengthAgreesWithUtf8PrefixOnWellFormedInput) {
  // The two are used interchangeably to enforce length limits, so they must
  // agree on every input a caller can actually reach.
  for (const std::string text : {"", "a", "héllo", "你好🙂abc"}) {
    const auto length = Utf8Length(text);
    EXPECT_EQ(Utf8Prefix(text, length).size(), text.size()) << "text: " << text;
    if (length > 0) {
      EXPECT_LT(Utf8Prefix(text, length - 1).size(), text.size()) << "text: " << text;
    }
  }
}

TEST(TextTest, Utf8LengthCountsStartBytesOnMalformedInput) {
  // Pins current behavior rather than endorsing it: counting start bytes does
  // not reproduce Python's replacement-character count for malformed input.
  // Unreachable in practice because callers validate UTF-8 before measuring.
  EXPECT_EQ(Utf8Length("\xc0\xaf"), 1u);      // Python would report 2
  EXPECT_EQ(Utf8Length("\x80"), 0u);          // Python would report 1
  EXPECT_EQ(Utf8Length("\xe4\xb8"), 1u);      // Python would report 1
  EXPECT_EQ(Utf8Length("\xed\xa0\x80"), 1u);  // Python would report 3
}

TEST(TextTest, SplitLinesAlwaysReturnsAtLeastOneField) {
  EXPECT_EQ(SplitLines(""), Lines{""});
  EXPECT_EQ(SplitLines("a"), Lines{"a"});
}

TEST(TextTest, SplitLinesKeepsTheFieldAfterATrailingNewline) {
  // Splitting on separators, not stripping terminators: "a\n" is two fields.
  EXPECT_EQ(SplitLines("a\n"), (Lines{"a", ""}));
  EXPECT_EQ(SplitLines("a\nb"), (Lines{"a", "b"}));
  EXPECT_EQ(SplitLines("\n"), (Lines{"", ""}));
  EXPECT_EQ(SplitLines("a\n\nb"), (Lines{"a", "", "b"}));
}

TEST(TextTest, SplitLinesDoesNotTreatCarriageReturnAsASeparator) {
  EXPECT_EQ(SplitLines("a\r\nb"), (Lines{"a\r", "b"}));
}

TEST(TextTest, JoinLinesInsertsSeparatorsWithoutATerminator) {
  EXPECT_EQ(JoinLines({}), "");
  EXPECT_EQ(JoinLines({"a"}), "a");
  EXPECT_EQ(JoinLines({"a", "b"}), "a\nb");
  EXPECT_EQ(JoinLines({"a", ""}), "a\n");
}

TEST(TextTest, JoinLinesRoundTripsSplitLines) {
  for (const std::string text : {"", "a", "a\n", "\n", "a\nb", "a\n\nb\n", "\n\n"}) {
    EXPECT_EQ(JoinLines(SplitLines(text)), text) << "text: " << text;
  }
}

TEST(TextTest, TrimRemovesUnicodeWhitespaceFromBothEnds) {
  EXPECT_EQ(Trim(" \t\r\n a \t\r\n "), "a");
  EXPECT_EQ(Trim(""), "");
  EXPECT_EQ(Trim("   "), "");
  EXPECT_EQ(Trim("a"), "a");

  // Non-ASCII whitespace: U+00A0, U+2003, U+3000.
  EXPECT_EQ(Trim("  x　"), "x");

  // Interior whitespace is preserved.
  EXPECT_EQ(Trim("  a b  "), "a b");
}

TEST(TextTest, TrimTreatsAsciiInformationSeparatorsAsWhitespace) {
  // Python str.isspace() reports true for U+001C through U+001F.
  EXPECT_EQ(Trim("\x1c\x1d\x1e\x1f"
                 "a"
                 "\x1c"),
            "a");
}

TEST(TextTest, TrimRightLeavesLeadingWhitespaceAlone) {
  EXPECT_EQ(TrimRight("  a  "), "  a");
  EXPECT_EQ(TrimRight("a"), "a");
  EXPECT_EQ(TrimRight("   "), "");
  EXPECT_EQ(TrimRight(""), "");
}

TEST(TextTest, TrimKeepsInvalidUtf8BecauseItIsNotWhitespace) {
  // A malformed byte decodes to U+FFFD, which is not whitespace, so it bounds
  // the trim instead of being consumed by it.
  EXPECT_EQ(Trim(" \xff "), "\xff");
}

TEST(TextTest, Utf8PrefixCountsCodePointsNotBytes) {
  // "he" plus a two-byte e-acute: three code points, four bytes.
  const std::string text = "héllo";

  EXPECT_EQ(Utf8Prefix(text, 0), "");
  EXPECT_EQ(Utf8Prefix(text, 1), "h");
  EXPECT_EQ(Utf8Prefix(text, 2), "hé");
  EXPECT_EQ(Utf8Prefix(text, 3), "hél");
}

TEST(TextTest, Utf8PrefixStopsAtTheEndWhenCountExceedsLength) {
  EXPECT_EQ(Utf8Prefix("ab", 99), "ab");
  EXPECT_EQ(Utf8Prefix("", 99), "");
}

TEST(TextTest, Utf8PrefixAdvancesOneByteThroughInvalidSequences) {
  // Truncated three-byte lead, a lone continuation byte, an overlong encoding,
  // a surrogate and an out-of-range code point each count as one unit.
  for (const std::string invalid : {"\xe4\xb8", "\x80", "\xc0\xaf", "\xed\xa0\x80", "\xf5\x80\x80\x80"}) {
    EXPECT_EQ(Utf8Prefix(invalid, 1).size(), 1u) << "input size: " << invalid.size();
  }
}

TEST(TextTest, Utf8PrefixNeverSplitsAValidCodePoint) {
  // U+4E16 is three bytes; taking one unit takes all three.
  EXPECT_EQ(Utf8Prefix("世界", 1), "世");
  EXPECT_EQ(Utf8Prefix("世界", 2), "世界");
}

}  // namespace
}  // namespace ievolve::utils
