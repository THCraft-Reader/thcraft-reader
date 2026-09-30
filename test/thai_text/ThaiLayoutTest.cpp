#include <Epub/ParsedText.h>
#include <GfxRenderer.h>
#include <ThaiCluster.h>
#include <ThaiSegmenter.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {
using Kind = thai::BreakKind;
constexpr auto regular = EpdFontFamily::REGULAR;
struct Line {
  std::unique_ptr<TextBlock> block;
  uint32_t offset;
  std::string text() const {
    std::string result;
    for (uint16_t i = 0; i < block->wordCount(); ++i) result += block->wordText(i);
    return result;
  }
};
using Lines = std::vector<Line>;

void configure(ParsedText& text, CssTextAlign align = CssTextAlign::Left) {
  text.getBlockStyle().alignment = align;
  text.getBlockStyle().textIndentDefined = true;
}
Lines layout(ParsedText& text, uint16_t width, bool final = true, int8_t tracking = 0, uint8_t spacing = 100) {
  GfxRenderer renderer;
  Lines lines;
  text.layoutAndExtractLines(renderer, 0, width, [&](std::unique_ptr<TextBlock> block, uint32_t offset) {
    EXPECT_TRUE(block->valid());
    lines.push_back({std::move(block), offset});
  }, final, tracking, spacing);
  return lines;
}
std::vector<std::string> strings(const Lines& lines) {
  std::vector<std::string> result;
  for (const auto& line : lines) result.push_back(line.text());
  return result;
}
void add(ParsedText& text, std::string_view word, Kind rank, uint32_t offset = 0,
         EpdFontFamily::Style style = regular, uint8_t link = 0) {
  text.addAnalyzedToken(word, style, rank, offset, link);
}
void analyze(ParsedText& text, const std::string& source) {
  const thai::ThaiDictionary dictionary;
  size_t offset = 0;
  Kind previous = Kind::Space;
  thai::Segment segment{};
  while (thai::nextSegment(source, offset, segment, true, dictionary)) {
    ASSERT_TRUE(segment.valid);
    const Kind rank = previous == Kind::Prohibited || segment.before == Kind::Prohibited
                          ? Kind::Prohibited : previous;
    add(text, std::string_view(source).substr(segment.begin, segment.end - segment.begin), rank);
    previous = segment.after;
  }
  ASSERT_EQ(offset, source.size());
}

TEST(ThaiLayoutTest, RealSpaceOutranksLaterDictionaryBoundary) {
  ParsedText text(false);
  configure(text);
  add(text, "ก", Kind::Space);
  add(text, "ข", Kind::Space);
  add(text, "ค", Kind::Word);
  add(text, "ง", Kind::Word);
  EXPECT_EQ(strings(layout(text, 28)), (std::vector<std::string>{"ก", "ขคง"}));
}

TEST(ThaiLayoutTest, DictionaryOutranksPunctuationAndPunctuationOutranksEmergency) {
  for (const Kind priority : {Kind::Word, Kind::Punctuation}) {
    ParsedText text(false);
    configure(text);
    add(text, "ก", Kind::Space);
    add(text, "ข", priority);
    add(text, "ค", priority == Kind::Word ? Kind::Punctuation : Kind::Emergency);
    add(text, "ง", Kind::Emergency);
    EXPECT_EQ(strings(layout(text, 24)), (std::vector<std::string>{"ก", "ขคง"}));
  }
}

TEST(ThaiLayoutTest, RightmostBoundaryWithinWinningRank) {
  ParsedText text(false);
  configure(text);
  for (const char* word : {"ก", "ข", "ค", "ง"}) add(text, word, Kind::Word);
  EXPECT_EQ(strings(layout(text, 24)), (std::vector<std::string>{"กขค", "ง"}));
}

TEST(ThaiLayoutTest, DictionaryCompoundPrefersKnownPrefixAndSuffixAcrossStyles) {
  GfxRenderer renderer;
  const int width = renderer.getTextAdvanceX(0, "จำนวน", regular);
  for (bool hyphenation : {false, true}) {
    for (bool styled : {false, true}) {
      ParsedText text(false, hyphenation);
      configure(text);
      if (styled) {
        add(text, "จำ", Kind::Space, 100, EpdFontFamily::BOLD);
        add(text, "นวนมาก", Kind::Prohibited, 102, EpdFontFamily::ITALIC);
      } else {
        add(text, "จำนวนมาก", Kind::Space, 100);
      }
      const auto lines = layout(text, width);
      EXPECT_EQ(strings(lines), (std::vector<std::string>{"จำนวน", "มาก"}));
      ASSERT_EQ(lines.size(), 2u);
      EXPECT_EQ(lines[0].offset, 100u);
      EXPECT_EQ(lines[1].offset, 105u);
      if (styled) EXPECT_EQ(lines[1].block->wordStyle(0), EpdFontFamily::ITALIC);
    }
  }
}

TEST(ThaiLayoutTest, FittingStyledWordMovesWholeInsteadOfBreakingAtMarkup) {
  ParsedText text(false);
  configure(text);
  add(text, "ก", Kind::Space);
  add(text, "โรง", Kind::Word, 1, EpdFontFamily::BOLD);
  add(text, "พยาบาล", Kind::Prohibited, 4, EpdFontFamily::ITALIC);
  GfxRenderer renderer;
  const auto lines = layout(text, renderer.getTextAdvanceX(0, "โรงพยาบาล", regular));
  EXPECT_EQ(strings(lines), (std::vector<std::string>{"ก", "โรงพยาบาล"}));
}

TEST(ThaiLayoutTest, EveryViewportPreservesAllMarkedClustersAndMakesProgress) {
  GfxRenderer renderer;
  for (const char* cluster : {"ก่", "ก้", "ก๊", "ก๋", "กิ", "กี", "กึ", "กื", "กุ", "กู", "กี่", "กุ่",
                              "น้ำ", "นํ้า", "เรื่อ", "เพื่อ"}) {
    const std::string source = std::string(cluster) + "ก";
    const int full = renderer.getTextAdvanceX(0, source.c_str(), regular);
    for (bool hyphenation : {false, true}) {
      for (int width = 1; width <= full; ++width) {
        SCOPED_TRACE(cluster);
        SCOPED_TRACE(width);
        ParsedText text(false, hyphenation);
        configure(text);
        add(text, source, Kind::Space);
        const auto lines = layout(text, width);
        ASSERT_FALSE(lines.empty());
        EXPECT_EQ(lines.front().text().find(cluster), 0u);
        std::string output;
        for (const auto& line : lines) output += line.text();
        EXPECT_EQ(output, source);
        EXPECT_TRUE(text.isEmpty());
      }
    }
  }
}

TEST(ThaiLayoutTest, PartialExtractionPreservesRemainderOffsetsStylesAndLinks) {
  for (bool hyphenation : {false, true}) {
    ParsedText text(false, hyphenation);
    configure(text);
    const auto link = text.addLinkTarget("#linked");
    add(text, "กี่ขค", Kind::Space, 70000, EpdFontFamily::BOLD, link);
    const auto first = layout(text, 1, false);
    EXPECT_EQ(strings(first), (std::vector<std::string>{"กี่", "ข"}));
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[1].offset, 70003u);
    add(text, "ง", Kind::Word, 70005);
    const auto last = layout(text, 16);
    EXPECT_EQ(strings(last), (std::vector<std::string>{"คง"}));
    ASSERT_EQ(last.size(), 1u);
    EXPECT_EQ(last[0].offset, 70004u);
    EXPECT_EQ(last[0].block->wordStyle(0), EpdFontFamily::BOLD);
    const auto spans = last[0].block->takeLinkSpans();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_STREQ(spans[0].href, "#linked");
    EXPECT_TRUE(text.isEmpty());
  }
}

TEST(ThaiLayoutTest, ClosingUnitsStayLeftAndOpeningQuoteStaysRight) {
  for (const std::string source : {"“ภาษาไทย”ก", "ภาษาไทยฯลฯก", "ภาษาไทย...ก", "ภาษาไทยๆก", "ภาษาไทยฯก"}) {
    for (const bool hyphenation : {false, true}) {
      for (const int width : {1, 24, 48, 72}) {
        ParsedText text(false, hyphenation);
        configure(text);
        analyze(text, source);
        const auto lines = layout(text, width);
        std::string output;
        for (const auto& line : lines) {
          const auto value = line.text();
          output += value;
          EXPECT_NE(value, "“");
          EXPECT_NE(value.find("”"), 0u);
          EXPECT_NE(value.find("."), 0u);
          EXPECT_NE(value.find("ฯ"), 0u);
          EXPECT_NE(value.find("ๆ"), 0u);
        }
        EXPECT_EQ(output, source);
        for (const std::string unit : {"ฯลฯ", "..."}) {
          if (source.find(unit) != std::string::npos) {
            EXPECT_TRUE(std::any_of(lines.begin(), lines.end(), [&](const Line& line) {
              return line.text().find(unit) != std::string::npos;
            }));
          }
        }
      }
    }
  }
}

TEST(ThaiLayoutTest, RubyAndNbspRemainAtomicAtOverwideFinalAndPartialLines) {
  for (bool ruby : {false, true}) {
    for (bool hyphenation : {false, true}) {
      ParsedText text(false, hyphenation);
      configure(text);
      add(text, "ก", Kind::Space);
      if (!ruby) text.addWord(" ", regular, false, true);
      add(text, "ข", Kind::Prohibited);
      if (ruby) text.setRubyGroupAt(0, 2, "reading");
      add(text, "ค", Kind::Word);
      const auto first = layout(text, 1, false);
      EXPECT_EQ(strings(first), (std::vector<std::string>{ruby ? "กข" : "ก ข"}));
      EXPECT_EQ(strings(layout(text, 1)), (std::vector<std::string>{"ค"}));
      EXPECT_TRUE(text.isEmpty());
    }
  }
}

TEST(ThaiLayoutTest, UnknownRunBeyond64KiBPreservesEveryClusterWithBoundedLineProgress) {
  for (bool hyphenation : {false, true}) {
    ParsedText text(false, hyphenation);
    configure(text);
    constexpr size_t count = 22000;
    for (size_t i = 0; i < count; ++i) add(text, "ฃ", Kind::Emergency, i);
    const auto lines = layout(text, 80);
    ASSERT_EQ(lines.size(), count / 10);
    for (size_t i = 0; i < lines.size(); ++i) {
      EXPECT_EQ(lines[i].offset, i * 10);
      EXPECT_EQ(lines[i].text(), "ฃฃฃฃฃฃฃฃฃฃ");
    }
    EXPECT_TRUE(text.isEmpty());
  }
}

TEST(ThaiLayoutTest, TrackingAndJustificationNeverStretchThaiDictionaryGaps) {
  for (const auto alignment : {CssTextAlign::Left, CssTextAlign::Justify}) {
    ParsedText text(false);
    configure(text, alignment);
    for (const char* word : {"ก", "ข", "ค", "ง"}) add(text, word, Kind::Word);
    const auto lines = layout(text, 35, true, 2, 125);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].text(), "กขค");
    EXPECT_EQ(lines[0].block->wordXpos(0), 0);
    EXPECT_EQ(lines[0].block->wordXpos(1), 10);
    EXPECT_EQ(lines[0].block->wordXpos(2), 20);
  }
}

TEST(ThaiLayoutTest, WordSpacingAndCjkInMixedBlockReuseExistingGapPositions) {
  ParsedText text(false);
  configure(text);
  add(text, "ก", Kind::Space);
  text.addWord("中文", regular);
  add(text, "ข", Kind::Word);
  const auto lines = layout(text, 80, true, 2, 125);
  ASSERT_EQ(lines.size(), 1u);
  ASSERT_EQ(lines[0].block->wordCount(), 4u);
  EXPECT_EQ(lines[0].block->wordXpos(1), 13);
  EXPECT_EQ(lines[0].block->wordXpos(2), 23);
  EXPECT_EQ(lines[0].block->wordXpos(3), 33);
}

TEST(ThaiLayoutTest, EnglishOnlyControlKeepsExistingWordSpacingAndFocus) {
  ParsedText text(false, false, true);
  configure(text);
  text.addWord("hello", regular);
  text.addWord("world", regular);
  const auto lines = layout(text, 200, true, 2, 125);
  ASSERT_EQ(lines.size(), 1u);
  ASSERT_EQ(lines[0].block->wordCount(), 2u);
  EXPECT_STREQ(lines[0].block->wordText(0), "hello");
  EXPECT_EQ(lines[0].block->wordXpos(1), 53);
  EXPECT_EQ(lines[0].block->focusBoundary(0), 2);
}

TEST(ThaiLayoutTest, IndentReducesOnlyFirstEffectiveLineAndNeverCutsCluster) {
  ParsedText text(false);
  configure(text);
  text.getBlockStyle().textIndent = 16;
  add(text, "กี่", Kind::Space);
  add(text, "ข", Kind::Word);
  add(text, "ค", Kind::Word);
  const auto lines = layout(text, 32);
  EXPECT_EQ(strings(lines), (std::vector<std::string>{"กี่", "ขค"}));
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].block->wordXpos(0), 16);
  EXPECT_EQ(lines[1].block->wordXpos(0), 0);
}

TEST(ThaiLayoutTest, RubyAddedBeforeThaiInitializationAndCurrentFlagsBothProtectGroups) {
  for (bool ruby : {false, true}) {
    ParsedText text(false);
    configure(text);
    text.addWord("中文", regular);
    if (ruby) text.setRubyGroupAt(0, 2, "reading");
    add(text, "ก", Kind::Word);
    if (!ruby) {
      // A later attachment mutation must override its earlier Word rank.
      text.wordContinues[1] = true;
      text.wordNoSpaceBefore[1] = false;
    }
    const auto lines = layout(text, 8);
    EXPECT_EQ(strings(lines), (std::vector<std::string>{"中文", "ก"}));
  }
}

TEST(ThaiLayoutTest, PrefixesBeyondLegacyBufferLimitKeepBytesAndOffsets) {
  for (const size_t prefixCount : {70u, 80u}) {
    ParsedText text(false);
    configure(text);
    std::string source;
    for (size_t i = 0; i < 100; ++i) source += "ฃ";
    add(text, source, Kind::Space, 200);
    const auto lines = layout(text, prefixCount * 8);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].text(), source.substr(0, prefixCount * 3));
    EXPECT_EQ(lines[1].text(), source.substr(prefixCount * 3));
    EXPECT_EQ(lines[1].offset, 200 + prefixCount);
  }
}

TEST(ThaiLayoutTest, SingleTokenRubyCannotBeSplitByGenericHyphenationInMixedBlock) {
  ParsedText text(false, true);
  configure(text);
  text.addWord("hospital", regular);
  text.setRubyForWordAt(0, "reading");
  add(text, "ก", Kind::Word);
  const auto lines = layout(text, 8);
  EXPECT_EQ(strings(lines), (std::vector<std::string>{"hospital", "ก"}));
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_TRUE(lines[0].block->hasRuby());
}
}  // namespace
