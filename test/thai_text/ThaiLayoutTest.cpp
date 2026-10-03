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

void configure(ParsedText& text, CssTextAlign align = CssTextAlign::Left) { text.getBlockStyle().alignment = align; }
Lines layout(ParsedText& text, uint16_t width, bool final = true, int8_t tracking = 0, uint8_t spacing = 100) {
  GfxRenderer renderer;
  Lines lines;
  text.layoutAndExtractLines(
      renderer, 0, width,
      [&](std::unique_ptr<TextBlock> block, uint32_t offset) {
        EXPECT_TRUE(block->valid());
        lines.push_back({std::move(block), offset});
      },
      final, tracking, spacing);
  return lines;
}
std::vector<std::string> strings(const Lines& lines) {
  std::vector<std::string> result;
  for (const auto& line : lines) result.push_back(line.text());
  return result;
}
void add(ParsedText& text, std::string_view word, Kind rank, uint32_t offset = 0, EpdFontFamily::Style style = regular,
         uint8_t link = 0) {
  text.addAnalyzedToken(word, style, rank, offset, link);
}
void analyze(ParsedText& text, const std::string& source) {
  const thai::ThaiDictionary dictionary;
  size_t offset = 0;
  Kind previous = Kind::Space;
  thai::Segment segment{};
  while (thai::nextSegment(source, offset, segment, true, dictionary)) {
    ASSERT_TRUE(segment.valid);
    const Kind rank = previous == Kind::Prohibited || segment.before == Kind::Prohibited ? Kind::Prohibited : previous;
    add(text, std::string_view(source).substr(segment.begin, segment.end - segment.begin), rank);
    previous = segment.after;
  }
  ASSERT_EQ(offset, source.size());
}

TEST(ThaiLayoutTest, SourceSpaceDoesNotLeaveRoomForAnotherWholeThaiWord) {
  GfxRenderer renderer;
  const int space = renderer.getSpaceWidth(0, regular);
  const int width = renderer.getTextAdvanceX(0, "ภาษาไทยมีประชากร", regular) + space;
  for (const auto alignment : {CssTextAlign::Left, CssTextAlign::Justify}) {
    for (const bool hyphenation : {false, true}) {
      ParsedText text(hyphenation, false, BlockStyle(), 0);
      configure(text, alignment);
      add(text, "ภาษาไทย", Kind::Space, 0);
      add(text, "มี", Kind::Space, 8);
      add(text, "ประชากร", Kind::Word, 10);
      add(text, "จำนวนมาก", Kind::Word, 17);
      const auto lines = layout(text, width);
      EXPECT_EQ(strings(lines), (std::vector<std::string>{"ภาษาไทยมีประชากร", "จำนวนมาก"}));
      ASSERT_EQ(lines.size(), 2u);
      EXPECT_EQ(lines[1].offset, 17u);
      EXPECT_EQ(lines[0].block->wordXpos(1), renderer.getTextAdvanceX(0, "ภาษาไทย", regular) + space);
      EXPECT_EQ(lines[0].block->wordXpos(2), renderer.getTextAdvanceX(0, "ภาษาไทยมี", regular) + space);
    }
  }
}

TEST(ThaiLayoutTest, ClosingPunctuationCanFinishLineAfterEarlierWordBoundary) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text);
  analyze(text, "มีคนไทย...มาก");
  GfxRenderer renderer;
  const int width = renderer.getTextAdvanceX(0, "มีคนไทย...", regular);
  EXPECT_EQ(strings(layout(text, width)), (std::vector<std::string>{"มีคนไทย...", "มาก"}));
}

TEST(ThaiLayoutTest, WholeWordBoundaryPreventsEmergencyCutToFillLine) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text);
  add(text, "ก", Kind::Space);
  add(text, "ข", Kind::Word);
  add(text, "ค", Kind::Emergency);
  add(text, "ง", Kind::Emergency);
  EXPECT_EQ(strings(layout(text, 24)), (std::vector<std::string>{"ก", "ขคง"}));
}

TEST(ThaiLayoutTest, RightmostFittingWordBoundary) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text);
  for (const char* word : {"ก", "ข", "ค", "ง"}) add(text, word, Kind::Word);
  EXPECT_EQ(strings(layout(text, 24)), (std::vector<std::string>{"กขค", "ง"}));
}

TEST(ThaiLayoutTest, DictionaryCompoundPrefersKnownPrefixAndSuffixAcrossStyles) {
  GfxRenderer renderer;
  const int width = renderer.getTextAdvanceX(0, "จำนวน", regular);
  for (bool hyphenation : {false, true}) {
    for (bool styled : {false, true}) {
      ParsedText text(hyphenation, false, BlockStyle(), 0);
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
  ParsedText text(false, false, BlockStyle(), 0);
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
  for (const char* cluster : {"ก่", "ก้", "ก๊", "ก๋", "กิ", "กี", "กึ", "กื", "กุ", "กู", "กี่", "กุ่", "น้ำ", "นํ้า", "เรื่อ", "เพื่อ"}) {
    const std::string source = std::string(cluster) + "ก";
    const int full = renderer.getTextAdvanceX(0, source.c_str(), regular);
    for (bool hyphenation : {false, true}) {
      for (int width = 1; width <= full; ++width) {
        SCOPED_TRACE(cluster);
        SCOPED_TRACE(width);
        ParsedText text(hyphenation, false, BlockStyle(), 0);
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
    ParsedText text(hyphenation, false, BlockStyle(), 0);
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
        ParsedText text(hyphenation, false, BlockStyle(), 0);
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
            EXPECT_TRUE(std::any_of(lines.begin(), lines.end(),
                                    [&](const Line& line) { return line.text().find(unit) != std::string::npos; }));
          }
        }
      }
    }
  }
}

TEST(ThaiLayoutTest, RubyAndNbspRemainAtomicAtOverwideFinalAndPartialLines) {
  for (bool ruby : {false, true}) {
    for (bool hyphenation : {false, true}) {
      ParsedText text(hyphenation, false, BlockStyle(), 0);
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
    ParsedText text(hyphenation, false, BlockStyle(), 0);
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
    ParsedText text(false, false, BlockStyle(), 0);
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
  ParsedText text(false, false, BlockStyle(), 0);
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
  ParsedText text(false, true, BlockStyle(), 0);
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
  ParsedText text(false, false, BlockStyle(), 4);
  configure(text);
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
    ParsedText text(false, false, BlockStyle(), 0);
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
    ParsedText text(false, false, BlockStyle(), 0);
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
  ParsedText text(true, false, BlockStyle(), 0);
  configure(text);
  text.addWord("hospital", regular);
  text.setRubyForWordAt(0, "reading");
  add(text, "ก", Kind::Word);
  const auto lines = layout(text, 8);
  EXPECT_EQ(strings(lines), (std::vector<std::string>{"hospital", "ก"}));
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_TRUE(lines[0].block->hasRuby());
}

int expandedEnd(const TextBlock& block, uint16_t i) {
  GfxRenderer renderer;
  return block.wordXpos(i) + renderer.getTextAdvanceX(0, block.wordText(i), block.wordStyle(i),
                                                      block.getBlockStyle().characterSpacing,
                                                      BidiUtils::BidiBaseDir::AUTO,
                                                      GfxRenderer::TextMeasureMode::Rendered, block.thaiExpansion(i));
}

TEST(ThaiLayoutTest, ThaiJustifyAllocatesFivePixelsInsideDictionaryTokenOnlyOnNonfinalLine) {
  GfxRenderer renderer;
  for (const auto alignment : {CssTextAlign::Justify, CssTextAlign::ThaiJustify}) {
    ParsedText text(false, false, BlockStyle(), 0);
    configure(text, alignment);
    add(text, "กขค", Kind::Space, 100);
    add(text, "งจฉ", Kind::Word, 103);
    const auto lines = layout(text, 29);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(strings(lines), (std::vector<std::string>{"กขค", "งจฉ"}));
    EXPECT_EQ(lines[0].offset, 100u);
    EXPECT_EQ(lines[1].offset, 103u);
    ASSERT_EQ(lines[0].block->wordCount(), 1u);
    const auto& first = *lines[0].block;
    EXPECT_EQ(first.wordXpos(0), 0);
    EXPECT_EQ(first.thaiExpansion(0), alignment == CssTextAlign::ThaiJustify ? 5 : 0);
    EXPECT_EQ(expandedEnd(first, 0), alignment == CssTextAlign::ThaiJustify ? 29 : 24);
    EXPECT_EQ(lines[1].block->thaiExpansion(0), 0);
    EXPECT_EQ(expandedEnd(*lines[1].block, 0), 24);
    if (alignment == CssTextAlign::ThaiJustify) {
      // Fixed-metric origins implied by the cached budget's q/r contract.
      ASSERT_EQ(renderer.countThaiJustificationGaps(0, first.wordText(0), regular), 2u);
      EXPECT_EQ(renderer.getTextAdvanceX(0, "ก", regular) + 3, 11);
      EXPECT_EQ(renderer.getTextAdvanceX(0, "กข", regular) + first.thaiExpansion(0), 21);
    }
  }
}

TEST(ThaiLayoutTest, StyledProhibitedEdgeAndInternalSlotShareRemainderAndLinkExtent) {
  for (const auto boundary : {Kind::Word, Kind::Prohibited}) {
    ParsedText text(false, false, BlockStyle(), 0);
    configure(text, CssTextAlign::ThaiJustify);
    const uint8_t link = text.addLinkTarget("#expanded");
    add(text, "ก", Kind::Space, 100, EpdFontFamily::BOLD, link);
    add(text, "ขค", boundary, 101, EpdFontFamily::ITALIC, link);
    add(text, "งจฉ", Kind::Word, 103);
    const auto lines = layout(text, 29);
    ASSERT_EQ(lines.size(), 2u);
    const auto& first = *lines[0].block;
    ASSERT_EQ(first.wordCount(), 2u);
    EXPECT_EQ(lines[0].text(), "กขค");
    EXPECT_EQ(first.wordXpos(0), 0);
    EXPECT_EQ(first.wordXpos(1), 11);
    EXPECT_EQ(first.thaiExpansion(0), 0);
    EXPECT_EQ(first.thaiExpansion(1), 2);
    EXPECT_EQ(first.wordStyle(0), EpdFontFamily::BOLD);
    EXPECT_EQ(first.wordStyle(1), EpdFontFamily::ITALIC);
    EXPECT_EQ(expandedEnd(first, 1), 29);
    EXPECT_EQ(lines[1].offset, 103u);
    const auto spans = lines[0].block->takeLinkSpans();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_STREQ(spans[0].href, "#expanded");
    EXPECT_EQ(spans[0].x, 0);
    EXPECT_EQ(spans[0].width, 29);
  }
}

TEST(ThaiLayoutTest, GenericAttachmentsNeverManufactureAnalyzedThaiEdges) {
  for (bool genericFirst : {false, true}) {
    ParsedText text(false, false, BlockStyle(), 0);
    configure(text, CssTextAlign::ThaiJustify);
    if (genericFirst)
      text.addWordWithBoundary("ก", regular, false, Kind::Space, 50, 0);
    else
      add(text, "ก", Kind::Space, 50);
    if (genericFirst)
      add(text, "ขค", Kind::Prohibited, 51);
    else
      text.addWordWithBoundary("ขค", regular, false, Kind::Prohibited, 51, 0);
    add(text, "งจฉ", Kind::Word, 53);
    ASSERT_EQ(text.wordThaiAnalyzed.size(), 3u);
    EXPECT_FALSE(text.wordThaiAnalyzed[genericFirst ? 0 : 1]);
    const auto lines = layout(text, 29);
    ASSERT_EQ(lines.size(), 2u);
    ASSERT_EQ(lines[0].block->wordCount(), 2u);
    EXPECT_EQ(lines[0].block->wordXpos(1), 8);
    EXPECT_EQ(lines[0].block->thaiExpansion(1), 5);
    EXPECT_EQ(expandedEnd(*lines[0].block, 1), 29);
    EXPECT_EQ(lines[1].offset, 53u);
  }
}

TEST(ThaiLayoutTest, MarkedClustersRemainWholeAndReceiveOnlyInterclusterExpansion) {
  GfxRenderer renderer;
  for (const char* marked : {"กี่", "น้ำ", "นํ้า", "เพื่อ"}) {
    SCOPED_TRACE(marked);
    const std::string token = std::string(marked) + "ขค";
    const int natural = renderer.getTextAdvanceX(0, token.c_str(), regular);
    ParsedText text(false, false, BlockStyle(), 0);
    configure(text, CssTextAlign::ThaiJustify);
    add(text, token, Kind::Space, 100);
    add(text, token, Kind::Word, 200);
    const auto lines = layout(text, natural + 5);
    ASSERT_EQ(lines.size(), 2u);
    ASSERT_EQ(lines[0].block->wordCount(), 1u);
    EXPECT_EQ(lines[0].text(), token);
    EXPECT_EQ(lines[1].text(), token);
    EXPECT_EQ(lines[1].offset, 200u);
    EXPECT_EQ(lines[0].block->thaiExpansion(0), 5);
    EXPECT_EQ(lines[1].block->thaiExpansion(0), 0);
    EXPECT_EQ(expandedEnd(*lines[0].block, 0), natural + 5);
    thai::JustificationBoundaryCursor cursor(token);
    size_t boundary = 0;
    ASSERT_TRUE(cursor.next(boundary));
    EXPECT_EQ(boundary, std::string(marked).size());
    ASSERT_TRUE(cursor.next(boundary));
    EXPECT_EQ(boundary, std::string(marked).size() + std::string("ข").size());
    EXPECT_FALSE(cursor.next(boundary));
  }
}

TEST(ThaiLayoutTest, PunctuationDigitsMalformedAndSingletonLinesDoNotCreateSlots) {
  GfxRenderer renderer;
  const std::vector<std::string> samples = {"ก", "ก๑ข", "กฯข", "กๆข", "ก!ข", std::string("ก") + '\xff' + "ข"};
  for (const auto& sample : samples) {
    SCOPED_TRACE(sample);
    ParsedText text(false, false, BlockStyle(), 0);
    configure(text, CssTextAlign::ThaiJustify);
    text.addWordWithBoundary(sample, regular, false, Kind::Space, 10, 0);
    add(text, "งจฉชซฌญ", Kind::Word, 30);
    const int natural = renderer.getTextAdvanceX(0, sample.c_str(), regular);
    const auto lines = layout(text, natural + 5);
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0].text(), sample);
    ASSERT_EQ(lines[0].block->wordCount(), 1u);
    EXPECT_EQ(lines[0].block->wordXpos(0), 0);
    EXPECT_EQ(lines[0].block->thaiExpansion(0), 0);
    EXPECT_EQ(expandedEnd(*lines[0].block, 0), natural);
  }
  ParsedText overwide(false, false, BlockStyle(), 0);
  configure(overwide, CssTextAlign::ThaiJustify);
  add(overwide, "กี่", Kind::Space, 70000);
  add(overwide, "ข", Kind::Word, 70003);
  const auto lines = layout(overwide, 1);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(strings(lines), (std::vector<std::string>{"กี่", "ข"}));
  EXPECT_EQ(lines[1].offset, 70003u);
  for (const auto& line : lines) {
    EXPECT_EQ(line.block->wordXpos(0), 0);
    EXPECT_EQ(line.block->thaiExpansion(0), 0);
  }
  EXPECT_EQ(expandedEnd(*lines[0].block, 0), 24);  // Fixed metrics count all three Thai scalars.
  EXPECT_EQ(expandedEnd(*lines[1].block, 0), 8);
}

TEST(ThaiLayoutTest, RubyGroupIsInternallyAtomicButItsOutsideEdgeCanExpand) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text, CssTextAlign::ThaiJustify);
  add(text, "กข", Kind::Space, 0);
  add(text, "คง", Kind::Word, 2);
  text.setRubyGroupAt(0, 2, "a");
  add(text, "จฉ", Kind::Word, 4);
  add(text, "ชซฌญ", Kind::Word, 6);
  const auto lines = layout(text, 53);
  ASSERT_EQ(lines.size(), 2u);
  const auto& first = *lines[0].block;
  ASSERT_EQ(first.wordCount(), 3u);
  EXPECT_EQ(lines[0].text(), "กขคงจฉ");
  EXPECT_TRUE(first.hasRuby());
  EXPECT_EQ(first.wordXpos(0), 0);
  EXPECT_EQ(first.wordXpos(1), 16);
  EXPECT_EQ(first.wordXpos(2), 35);
  EXPECT_EQ(first.thaiExpansion(0), 0);
  EXPECT_EQ(first.thaiExpansion(1), 0);
  EXPECT_EQ(first.thaiExpansion(2), 2);
  EXPECT_EQ(expandedEnd(first, 2), 53);
  EXPECT_EQ(lines[1].offset, 6u);
}

TEST(ThaiLayoutTest, NbspUsesOneLegacySpaceSlotWithoutThaiEdgesAcrossIt) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text, CssTextAlign::ThaiJustify);
  add(text, "กข", Kind::Space, 0);
  text.addWord(" ", regular, false, true, 2);
  add(text, "คง", Kind::Prohibited, 3);
  add(text, "จฉชซ", Kind::Word, 5);
  const auto lines = layout(text, 41);
  ASSERT_EQ(lines.size(), 2u);
  const auto& first = *lines[0].block;
  ASSERT_EQ(first.wordCount(), 3u);
  EXPECT_EQ(lines[0].text(), "กข คง");
  EXPECT_EQ(first.wordXpos(0), 0);
  EXPECT_EQ(first.wordXpos(1), 18);
  EXPECT_EQ(first.wordXpos(2), 24);
  EXPECT_EQ(first.thaiExpansion(0), 2);
  EXPECT_EQ(first.thaiExpansion(1), 0);
  EXPECT_EQ(first.thaiExpansion(2), 1);
  EXPECT_EQ(expandedEnd(first, 2), 41);
  EXPECT_EQ(lines[1].offset, 5u);
}

TEST(ThaiLayoutTest, MixedFocusAndSpaceShareSlotsWithoutExpandingFocusTokenInternals) {
  ParsedText text(false, true, BlockStyle(), 0);
  configure(text, CssTextAlign::ThaiJustify);
  text.addWord("hello", regular, false, false, 0);
  add(text, "กข", Kind::Space, 6);
  add(text, "คงจฉ", Kind::Word, 8);
  const auto lines = layout(text, 65);
  ASSERT_EQ(lines.size(), 2u);
  const auto& first = *lines[0].block;
  ASSERT_EQ(first.wordCount(), 2u);
  EXPECT_STREQ(first.wordText(0), "hello");
  EXPECT_EQ(first.focusBoundary(0), 2);
  EXPECT_EQ(first.focusBoundary(1), 0);
  EXPECT_EQ(first.thaiExpansion(0), 0);
  EXPECT_EQ(first.wordXpos(1), 47);
  EXPECT_EQ(first.thaiExpansion(1), 2);
  EXPECT_EQ(expandedEnd(first, 1), 65);
  EXPECT_EQ(lines[1].offset, 8u);
}

TEST(ThaiLayoutTest, ExistingCjkSlotsAreCountedOnceInThaiUnion) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text, CssTextAlign::ThaiJustify);
  add(text, "กข", Kind::Space, 0);
  text.addWord("中文", regular, false, false, 3);
  add(text, "คงจฉ", Kind::Word, 5);
  const auto lines = layout(text, 41);
  ASSERT_EQ(lines.size(), 2u);
  const auto& first = *lines[0].block;
  ASSERT_EQ(first.wordCount(), 3u);
  EXPECT_EQ(lines[0].text(), "กข中文");
  EXPECT_EQ(first.thaiExpansion(0), 2);
  EXPECT_EQ(first.wordXpos(1), 24);
  EXPECT_EQ(first.wordXpos(2), 33);
  EXPECT_EQ(expandedEnd(first, 2), 41);
}

TEST(ThaiLayoutTest, RtlParagraphRetainsLtrThaiIdentityAndPhysicalRemainderOrder) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text, CssTextAlign::ThaiJustify);
  text.getBlockStyle().isRtl = true;
  text.getBlockStyle().directionDefined = true;
  text.addWord("אב", regular, false, false, 100);
  add(text, "กขค", Kind::Space, 103);
  add(text, "งจฉ", Kind::Word, 106);
  const auto lines = layout(text, 49);
  ASSERT_EQ(lines.size(), 2u);
  const auto& first = *lines[0].block;
  ASSERT_EQ(first.wordCount(), 2u);
  EXPECT_STREQ(first.wordText(0), "กขค");
  EXPECT_STREQ(first.wordText(1), "אב");
  EXPECT_EQ(first.wordXpos(0), 0);
  EXPECT_EQ(first.thaiExpansion(0), 4);
  EXPECT_EQ(first.wordXpos(1), 33);
  EXPECT_EQ(first.thaiExpansion(1), 0);
  EXPECT_EQ(expandedEnd(first, 1), 49);
  EXPECT_EQ(lines[0].offset, 100u);
  EXPECT_EQ(lines[1].offset, 106u);
  EXPECT_EQ(lines[1].block->thaiExpansion(0), 0);
}

TEST(ThaiLayoutTest, StreamingExtractionExpandsEmittedLinesAndRetainsProvenanceForHeldLine) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text, CssTextAlign::ThaiJustify);
  add(text, "ก", Kind::Space, 70000);
  add(text, "ขค", Kind::Prohibited, 70001);
  add(text, "ง", Kind::Word, 70003);
  add(text, "จฉ", Kind::Prohibited, 70004);
  const auto first = layout(text, 29, false);
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].text(), "กขค");
  EXPECT_EQ(first[0].block->wordXpos(1), 11);
  EXPECT_EQ(first[0].block->thaiExpansion(1), 2);
  EXPECT_EQ(expandedEnd(*first[0].block, 1), 29);
  ASSERT_EQ(text.size(), 2u);
  ASSERT_EQ(text.wordThaiAnalyzed.size(), 2u);
  EXPECT_TRUE(text.wordThaiAnalyzed[0]);
  EXPECT_TRUE(text.wordThaiAnalyzed[1]);
  add(text, "ชซฌ", Kind::Word, 70006);
  const auto second = layout(text, 29, false);
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0].text(), "งจฉ");
  EXPECT_EQ(second[0].offset, 70003u);
  EXPECT_EQ(second[0].block->wordXpos(1), 11);
  EXPECT_EQ(second[0].block->thaiExpansion(1), 2);
  EXPECT_EQ(expandedEnd(*second[0].block, 1), 29);
  const auto last = layout(text, 29);
  ASSERT_EQ(last.size(), 1u);
  EXPECT_EQ(last[0].text(), "ชซฌ");
  EXPECT_EQ(last[0].offset, 70006u);
  EXPECT_EQ(last[0].block->thaiExpansion(0), 0);
  EXPECT_EQ(expandedEnd(*last[0].block, 0), 24);
  EXPECT_TRUE(text.isEmpty());
  EXPECT_TRUE(text.wordThaiAnalyzed.empty());
}

TEST(ThaiLayoutTest, ThaiIndentAndEnglishOnlyLegacyRemainderRemainUnchanged) {
  ParsedText indented(false, false, BlockStyle(), 2);
  configure(indented, CssTextAlign::ThaiJustify);
  add(indented, "กขค", Kind::Space);
  add(indented, "งจฉ", Kind::Word);
  const auto lines = layout(indented, 37);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].block->wordXpos(0), 8);
  EXPECT_EQ(lines[0].block->thaiExpansion(0), 5);
  EXPECT_EQ(expandedEnd(*lines[0].block, 0), 37);
  EXPECT_EQ(lines[1].block->wordXpos(0), 0);
  for (auto alignment : {CssTextAlign::Justify, CssTextAlign::ThaiJustify}) {
    ParsedText english(false, false, BlockStyle(), 0);
    configure(english, alignment);
    for (const char* word : {"a", "b", "c", "long"}) english.addWord(word, regular);
    const auto control = layout(english, 35);
    ASSERT_EQ(control.size(), 2u);
    ASSERT_EQ(control[0].block->wordCount(), 3u);
    EXPECT_EQ(control[0].block->wordXpos(0), 0);
    EXPECT_EQ(control[0].block->wordXpos(1), 13);
    EXPECT_EQ(control[0].block->wordXpos(2), 26);
    EXPECT_EQ(expandedEnd(*control[0].block, 2), 34);
    EXPECT_EQ(control[0].block->thaiExpansion(0), 0);
    EXPECT_FALSE(english.thaiJustificationMetrics);
  }
}

TEST(ThaiLayoutTest, ReversedStyledThaiEdgesDoNotBecomeExpansionSlots) {
  const auto makeLines = [](CssTextAlign alignment) {
    ParsedText text(false, false, BlockStyle(), 0);
    configure(text, alignment);
    text.getBlockStyle().isRtl = true;
    text.getBlockStyle().directionDefined = true;
    text.addWord("אב", regular, false, false, 100);
    const uint8_t link = text.addLinkTarget("#rtl-thai");
    add(text, "ก", Kind::Space, 103, EpdFontFamily::BOLD, link);
    add(text, "ขค", Kind::Prohibited, 104, EpdFontFamily::ITALIC, link);
    add(text, "งจฉ", Kind::Word, 106);
    return layout(text, 49);
  };
  const auto legacy = makeLines(CssTextAlign::Justify);
  const auto lines = makeLines(CssTextAlign::ThaiJustify);
  ASSERT_EQ(lines.size(), 2u);
  ASSERT_EQ(legacy.size(), lines.size());
  const auto& first = *lines[0].block;
  ASSERT_EQ(first.wordCount(), 3u);
  for (uint16_t i = 0; i < first.wordCount(); ++i) {
    EXPECT_STREQ(first.wordText(i), legacy[0].block->wordText(i));
  }
  // Only the internal ข|ค edge and the existing space share the five spare pixels.
  EXPECT_EQ(first.wordXpos(0), 0);
  EXPECT_EQ(first.thaiExpansion(0), 3);
  EXPECT_EQ(first.wordXpos(1), expandedEnd(first, 0));
  EXPECT_EQ(first.thaiExpansion(1), 0);
  EXPECT_EQ(expandedEnd(first, 2), 49);
  const auto spans = lines[0].block->takeLinkSpans();
  ASSERT_EQ(spans.size(), 1u);
  EXPECT_EQ(spans[0].x, first.wordXpos(0));
  EXPECT_EQ(spans[0].width, expandedEnd(first, 1) - first.wordXpos(0));
  EXPECT_EQ(lines[1].offset, 106u);
}

TEST(ThaiLayoutTest, NoThaiSlotsFallBackToLegacySpacesWithSelectedRenderedMetrics) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text, CssTextAlign::ThaiJustify);
  add(text, "ก", Kind::Space, 0);
  add(text, "ข", Kind::Space, 2);
  add(text, "ค", Kind::Space, 4);
  add(text, "งจฉช", Kind::Space, 6);
  const auto lines = layout(text, 35);
  ASSERT_EQ(lines.size(), 2u);
  ASSERT_EQ(lines[0].block->wordCount(), 3u);
  EXPECT_TRUE(text.thaiJustificationMetrics);
  EXPECT_EQ(lines[0].block->wordXpos(0), 0);
  EXPECT_EQ(lines[0].block->wordXpos(1), 13);
  EXPECT_EQ(lines[0].block->wordXpos(2), 26);
  EXPECT_EQ(expandedEnd(*lines[0].block, 2), 34);
  for (uint16_t i = 0; i < 3; ++i) EXPECT_EQ(lines[0].block->thaiExpansion(i), 0);
}

TEST(ThaiLayoutTest, GenericThaiFocusSplitRemainsAtomicAlongsideExpandableAnalyzedText) {
  ParsedText text(false, true, BlockStyle(), 0);
  configure(text, CssTextAlign::ThaiJustify);
  text.addWord("กขค", regular, false, false, 0);
  add(text, "งจ", Kind::Space, 4);
  add(text, "ฉชซฌ", Kind::Word, 6);
  const auto lines = layout(text, 49);
  ASSERT_EQ(lines.size(), 2u);
  const auto& first = *lines[0].block;
  ASSERT_EQ(first.wordCount(), 2u);
  EXPECT_EQ(first.focusBoundary(0), 3);
  EXPECT_EQ(first.focusSuffixX(0), 8);
  EXPECT_EQ(first.thaiExpansion(0), 0);
  EXPECT_EQ(first.wordXpos(1), 31);
  EXPECT_EQ(first.thaiExpansion(1), 2);
  EXPECT_EQ(expandedEnd(first, 1), 49);
}

TEST(ThaiLayoutTest, IneligibleEndpointNeverCreatesStyledExpansionEdge) {
  GfxRenderer renderer;
  for (const std::string left : {"ก๑", "กฯ", "กๆ"}) {
    ParsedText text(false, false, BlockStyle(), 0);
    configure(text, CssTextAlign::ThaiJustify);
    add(text, left, Kind::Space, 0, EpdFontFamily::BOLD);
    add(text, "ขค", Kind::Prohibited, 2, EpdFontFamily::ITALIC);
    add(text, "งจฉช", Kind::Word, 4);
    const int naturalLeft = renderer.getTextAdvanceX(0, left.c_str(), EpdFontFamily::BOLD);
    const auto lines = layout(text, naturalLeft + 16 + 5);
    ASSERT_EQ(lines.size(), 2u);
    ASSERT_EQ(lines[0].block->wordCount(), 2u);
    EXPECT_EQ(lines[0].text(), left + "ขค");
    EXPECT_EQ(lines[0].block->wordXpos(1), naturalLeft);
    EXPECT_EQ(lines[0].block->thaiExpansion(0), 0);
    EXPECT_EQ(lines[0].block->thaiExpansion(1), 5);
    EXPECT_EQ(expandedEnd(*lines[0].block, 1), naturalLeft + 21);
  }
}

TEST(ThaiLayoutTest, RubyOverhangReservationIsKeptWhenOutsideThaiSlotsFillLine) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text, CssTextAlign::ThaiJustify);
  add(text, "กข", Kind::Space, 0);
  text.setRubyForWordAt(0, "abcd");
  add(text, "คง", Kind::Word, 2);
  add(text, "จฉชซ", Kind::Word, 4);
  const auto lines = layout(text, 45);
  ASSERT_EQ(lines.size(), 2u);
  const auto& first = *lines[0].block;
  ASSERT_EQ(first.wordCount(), 2u);
  EXPECT_EQ(first.wordXpos(0), 8);
  EXPECT_EQ(first.thaiExpansion(0), 0);
  EXPECT_EQ(first.wordXpos(1), 27);
  EXPECT_EQ(first.thaiExpansion(1), 2);
  EXPECT_EQ(expandedEnd(first, 1), 45);
  EXPECT_EQ(first.getRubyTexts()[0], "abcd");
  EXPECT_EQ(lines[1].offset, 4u);
}

TEST(ThaiLayoutTest, LongExactPrefixPreservesTextAndExpansionAcrossWrapping) {
  ParsedText text(false, false, BlockStyle(), 0);
  configure(text, CssTextAlign::ThaiJustify);
  std::string source;
  source.reserve(240);
  for (int i = 0; i < 80; ++i) source += "ก";
  add(text, source, Kind::Space, 100);
  add(text, "ขค", Kind::Word, 180);
  const auto lines = layout(text, 601);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].text(), source.substr(0, 225));
  EXPECT_EQ(lines[1].text(), source.substr(225) + "ขค");
  EXPECT_EQ(lines[0].offset, 100u);
  EXPECT_EQ(lines[1].offset, 175u);
  EXPECT_EQ(lines[0].block->thaiExpansion(0), 1);
  EXPECT_EQ(expandedEnd(*lines[0].block, 0), 601);
  EXPECT_EQ(lines[1].block->thaiExpansion(0), 0);
  EXPECT_EQ(expandedEnd(*lines[1].block, 1), 56);
}
}  // namespace
