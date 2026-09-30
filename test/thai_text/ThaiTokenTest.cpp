#include <Epub/ParsedText.h>
#include <Epub/TokenBoundary.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
thread_local bool failTokenArrayAllocation = false;

class FailTokenArrays {
 public:
  FailTokenArrays() { failTokenArrayAllocation = true; }
  ~FailTokenArrays() { failTokenArrayAllocation = false; }
};
}  // namespace

// WordStore uses nothrow arrays for both its chunk table and text chunks. Keep
// failure scoped to this thread and only these tests, not unrelated STL storage.
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (failTokenArrayAllocation) return nullptr;
  try {
    return ::operator new[](size);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

namespace {
using Kind = thai::BreakKind;
constexpr auto REGULAR = EpdFontFamily::REGULAR;

void expectAligned(const ParsedText& text) {
  const auto size = text.size();
  EXPECT_EQ(text.wordStyles.size(), size);
  EXPECT_EQ(text.wordContinues.size(), size);
  EXPECT_EQ(text.wordNoSpaceBefore.size(), size);
  EXPECT_EQ(text.wordFocusBoundary.size(), size);
  EXPECT_EQ(text.wordLinkIds.size(), size);
  EXPECT_EQ(text.wordVisibleOffsetDeltas.size(), size);
  EXPECT_EQ(text.wordBreakRanks.size(), text.hasThaiTokens ? size : 0u);
}

std::vector<std::unique_ptr<TextBlock>> extract(ParsedText& text, uint16_t width, bool last = true) {
  GfxRenderer renderer;
  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(renderer, 0, width,
                            [&](std::unique_ptr<TextBlock> line, uint32_t) {
                              EXPECT_TRUE(line->valid());
                              lines.push_back(std::move(line));
                            }, last);
  return lines;
}

TEST(ThaiTokenTest, GenericControlsKeepNfcFocusAndCjkWithoutThaiStorage) {
  ParsedText text(false, false, true);
  text.addWord("EpubCraft", REGULAR);
  text.addWord("4.2.0", REGULAR);
  text.addWord("Tie\xCC\x82\xCC\x81ng", EpdFontFamily::ITALIC, true);
  text.addWord("中文", REGULAR);
  ASSERT_EQ(text.size(), 5u);
  EXPECT_EQ(text.wordAt(0), "EpubCraft");
  EXPECT_EQ(text.wordFocusBoundary[0], 4);
  EXPECT_EQ(text.wordAt(1), "4.2.0");
  EXPECT_EQ(text.wordFocusBoundary[1], 0);
  EXPECT_EQ(text.wordAt(2), "Tiếng");
  EXPECT_EQ(text.wordFocusBoundary[2], 2);
  EXPECT_EQ(text.wordStyles[2], (EpdFontFamily::ITALIC | EpdFontFamily::UNDERLINE));
  EXPECT_EQ(text.wordAt(3), "中");
  EXPECT_EQ(text.wordAt(4), "文");
  EXPECT_FALSE(text.wordContinues[4]);
  EXPECT_TRUE(text.wordNoSpaceBefore[4]);
  EXPECT_FALSE(text.hasThaiTokens);
  EXPECT_EQ(text.wordBreakRanks.capacity(), 0u);
  expectAligned(text);
}

TEST(ThaiTokenTest, AnalyzedBoundariesKeepSpacesAttachmentsAndRanksDistinct) {
  ParsedText text(false, false, true);
  const std::vector<Kind> kinds{Kind::Space, Kind::Word, Kind::Prohibited, Kind::Punctuation, Kind::Emergency};
  const std::vector<std::string> words{"ภาษา", "ไทย", "กี่", "น้ำ", "ฃ"};
  for (size_t i = 0; i < kinds.size(); ++i) {
    text.addAnalyzedToken(words[i], REGULAR, kinds[i], static_cast<uint32_t>(i * 10), 0);
  }
  ASSERT_EQ(text.size(), kinds.size());
  EXPECT_EQ(text.wordBreakRanks, kinds);
  for (size_t i = 0; i < kinds.size(); ++i) {
    EXPECT_EQ(text.wordAt(i), words[i]);
    EXPECT_EQ(text.wordFocusBoundary[i], 0);
    EXPECT_EQ(text.wordStyles[i], REGULAR);
    EXPECT_EQ(text.wordContinues[i], kinds[i] != Kind::Space);
    EXPECT_EQ(text.wordNoSpaceBefore[i], kinds[i] != Kind::Space && kinds[i] != Kind::Prohibited);
    EXPECT_EQ(TokenBoundary::allowsBreak(text.wordContinues[i], text.wordNoSpaceBefore[i]),
              kinds[i] != Kind::Prohibited);
    EXPECT_EQ(text.visibleOffsetAt(i), i * 10);
  }
  expectAligned(text);
}

TEST(ThaiTokenTest, GenericAfterThaiPreservesFocusNfcAndNoInsertedSpace) {
  ParsedText text(false, false, true);
  text.getBlockStyle().alignment = CssTextAlign::Left;
  const auto link = text.addLinkTarget("#note");
  text.addAnalyzedToken("ไทย", REGULAR, Kind::Space, 4, 0);
  text.addWordWithBoundary("Tie\xCC\x82\xCC\x81ng!", EpdFontFamily::ITALIC, true, Kind::Word, 7, link);
  ASSERT_EQ(text.size(), 3u);
  EXPECT_EQ(text.wordAt(1), "Tiếng");
  EXPECT_EQ(text.wordAt(2), "!");
  EXPECT_EQ(text.wordBreakRanks, (std::vector<Kind>{Kind::Space, Kind::Word, Kind::Prohibited}));
  EXPECT_EQ(text.wordFocusBoundary[1], 2);
  EXPECT_EQ(text.visibleOffsetAt(1), 7u);
  EXPECT_EQ(text.wordLinkIds[1], link);
  EXPECT_EQ(text.wordLinkIds[2], link);
  const auto lines = extract(text, 480);
  ASSERT_EQ(lines.size(), 1u);
  ASSERT_EQ(lines[0]->wordCount(), 3);
  EXPECT_EQ(lines[0]->wordXpos(1) - lines[0]->wordXpos(0), GfxRenderer{}.getTextAdvanceX(0, "ไทย", REGULAR));
  EXPECT_EQ(lines[0]->wordXpos(2) - lines[0]->wordXpos(1), GfxRenderer{}.getTextAdvanceX(0, "Tiếng", REGULAR));
  EXPECT_EQ(lines[0]->focusBoundary(1), 2);
  EXPECT_EQ(lines[0]->wordStyle(1), (EpdFontFamily::ITALIC | EpdFontFamily::UNDERLINE));
  expectAligned(text);
}

TEST(ThaiTokenTest, BoundaryOverrideAffectsOnlyFirstCjkToken) {
  ParsedText text(false);
  text.addAnalyzedToken("ไทย", REGULAR, Kind::Space, 0, 0);
  text.addWordWithBoundary("中文", REGULAR, false, Kind::Prohibited, 3, 0);
  ASSERT_EQ(text.size(), 3u);
  EXPECT_EQ(text.wordBreakRanks, (std::vector<Kind>{Kind::Space, Kind::Prohibited, Kind::Word}));
  EXPECT_TRUE(text.wordContinues[1]);
  EXPECT_FALSE(text.wordNoSpaceBefore[1]);
  EXPECT_FALSE(text.wordContinues[2]);
  EXPECT_TRUE(text.wordNoSpaceBefore[2]);
  EXPECT_EQ(text.visibleOffsetAt(2), 4u);
  expectAligned(text);
}

TEST(ThaiTokenTest, LazyRanksPreserveExistingRubyAndOrdinaryAttachments) {
  ParsedText text(false);
  text.addWord("中文", REGULAR);
  text.setRubyGroupAt(0, 2, "zhongwen");
  text.addWord("left", REGULAR);
  text.addWord("x", REGULAR, false, true);
  text.addWord("next", REGULAR);
  EXPECT_EQ(text.wordBreakRanks.capacity(), 0u);
  text.addAnalyzedToken("ไทย", REGULAR, Kind::Word, 8, 0);
  ASSERT_EQ(text.size(), 6u);
  EXPECT_EQ(text.wordBreakRanks,
            (std::vector<Kind>{Kind::Space, Kind::Prohibited, Kind::Space, Kind::Prohibited, Kind::Space, Kind::Word}));
  EXPECT_EQ(text.rubyTexts.size(), text.size());
  EXPECT_EQ(text.getRubyTextAt(0), "zhongwen");
  expectAligned(text);
}

TEST(ThaiTokenTest, RubyAppliedAfterThaiProhibitsInternalAnalyzedBoundary) {
  ParsedText text(false);
  text.addWord("x", REGULAR);
  text.addAnalyzedToken("ภาษา", REGULAR, Kind::Space, 1, 0);
  text.addAnalyzedToken("ไทย", REGULAR, Kind::Word, 5, 0);
  text.setRubyGroupAt(1, 2, "reading");
  EXPECT_EQ(text.wordBreakRanks[2], Kind::Prohibited);
  EXPECT_FALSE(TokenBoundary::allowsBreak(text.wordContinues[2], text.wordNoSpaceBefore[2]));
  const auto lines = extract(text, 56);
  ASSERT_EQ(lines.size(), 2u);
  ASSERT_EQ(lines[0]->wordCount(), 1);
  EXPECT_STREQ(lines[0]->wordText(0), "x");
  ASSERT_EQ(lines[1]->wordCount(), 2);
  EXPECT_STREQ(lines[1]->wordText(0), "ภาษา");
  EXPECT_STREQ(lines[1]->wordText(1), "ไทย");
  expectAligned(text);
}

TEST(ThaiTokenTest, RubyPaddingStaysAtOriginalIndexAcrossFocusPieces) {
  ParsedText text(false, false, true);
  text.addWord("x", REGULAR);
  text.setRubyForWordAt(0, "ex");
  text.addWord("hello!", REGULAR);
  text.addAnalyzedToken("ไทย", REGULAR, Kind::Space, 7, 0);
  ASSERT_EQ(text.size(), 4u);
  EXPECT_EQ(text.rubyTexts.size(), 4u);
  EXPECT_EQ(text.getRubyTextAt(0), "ex");
  EXPECT_TRUE(text.getRubyTextAt(1).empty());
  EXPECT_TRUE(text.getRubyTextAt(2).empty());
  EXPECT_TRUE(text.getRubyTextAt(3).empty());
  expectAligned(text);
}

TEST(ThaiTokenTest, PartialExtractionRetainsRankStyleLinkAndRebasedOffset) {
  ParsedText text(false);
  text.getBlockStyle().alignment = CssTextAlign::Left;
  text.getBlockStyle().textIndentDefined = true;
  const auto link = text.addLinkTarget("#last");
  text.addAnalyzedToken("ก", REGULAR, Kind::Space, 10, 0);
  text.addAnalyzedToken("ข", REGULAR, Kind::Word, 70000, 0);
  text.addAnalyzedToken("ค", EpdFontFamily::BOLD, Kind::Emergency, 70003, link);
  const auto first = extract(text, 8, false);
  ASSERT_EQ(first.size(), 2u);
  EXPECT_STREQ(first[0]->wordText(0), "ก");
  EXPECT_STREQ(first[1]->wordText(0), "ข");
  ASSERT_EQ(text.size(), 1u);
  EXPECT_EQ(text.wordAt(0), "ค");
  EXPECT_EQ(text.wordBreakRanks[0], Kind::Emergency);
  EXPECT_EQ(text.wordStyles[0], EpdFontFamily::BOLD);
  EXPECT_EQ(text.wordLinkIds[0], link);
  EXPECT_EQ(text.visibleOffsetAt(0), 70003u);
  expectAligned(text);
  text.addAnalyzedToken("ง", REGULAR, Kind::Word, 70004, 0);
  EXPECT_EQ(text.wordBreakRanks, (std::vector<Kind>{Kind::Emergency, Kind::Word}));
  const auto last = extract(text, 16);
  ASSERT_EQ(last.size(), 1u);
  EXPECT_STREQ(last[0]->wordText(0), "ค");
  EXPECT_STREQ(last[0]->wordText(1), "ง");
  EXPECT_TRUE(text.isEmpty());
  expectAligned(text);
}

TEST(ThaiTokenTest, GenericSplitInThaiBlockPreservesSurroundingRanksAndOffsets) {
  ParsedText text(false, true);
  const auto link = text.addLinkTarget("#korean");
  text.addAnalyzedToken("ไทย", REGULAR, Kind::Space, 0, 0);
  text.addWordWithBoundary("한국어", REGULAR, false, Kind::Word, 3, link);
  text.addAnalyzedToken("ก", REGULAR, Kind::Emergency, 6, 0);
  GfxRenderer renderer;
  auto widths = text.calculateWordWidths(renderer, 0);
  ASSERT_TRUE(text.hyphenateWordAtIndex(1, 16, renderer, 0, widths, false));
  ASSERT_EQ(text.size(), 4u);
  EXPECT_EQ(text.wordAt(1), "한국");
  EXPECT_EQ(text.wordAt(2), "어");
  EXPECT_EQ(text.wordBreakRanks, (std::vector<Kind>{Kind::Space, Kind::Word, Kind::Space, Kind::Emergency}));
  EXPECT_EQ(text.wordLinkIds[1], link);
  EXPECT_EQ(text.wordLinkIds[2], link);
  EXPECT_EQ(text.visibleOffsetAt(1), 3u);
  EXPECT_EQ(text.visibleOffsetAt(2), 5u);
  EXPECT_EQ(text.visibleOffsetAt(3), 6u);
  EXPECT_EQ(widths, (std::vector<uint16_t>{24, 16, 8, 8}));
  expectAligned(text);
}

TEST(ThaiTokenTest, FailedInitialAnalyzedAppendDoesNotCreateParallelEntries) {
  ParsedText text(false);
  {
    FailTokenArrays failure;
    text.addAnalyzedToken("ไทย", REGULAR, Kind::Word, 0, 0);
  }
  EXPECT_TRUE(text.hadDroppedWords());
  EXPECT_TRUE(text.isEmpty());
  EXPECT_FALSE(text.hasThaiTokens);
  EXPECT_EQ(text.wordBreakRanks.capacity(), 0u);
  expectAligned(text);
  text.addAnalyzedToken("ภาษา", REGULAR, Kind::Space, 3, 0);
  ASSERT_EQ(text.size(), 1u);
  EXPECT_EQ(text.wordAt(0), "ภาษา");
  EXPECT_EQ(text.visibleOffsetAt(0), 3u);
  EXPECT_TRUE(text.hadDroppedWords());
  expectAligned(text);
}

TEST(ThaiTokenTest, FailedGenericAppendRetainsThaiAndRubyArrayAlignment) {
  ParsedText text(false);
  text.addAnalyzedToken("ไทย", REGULAR, Kind::Space, 0, 0);
  text.setRubyForWordAt(0, "thai");
  const std::string large(2048, 'a');
  {
    FailTokenArrays failure;
    text.addWordWithBoundary(large, REGULAR, false, Kind::Word, 3, 0);
  }
  EXPECT_TRUE(text.hadDroppedWords());
  ASSERT_EQ(text.size(), 1u);
  EXPECT_EQ(text.wordAt(0), "ไทย");
  EXPECT_EQ(text.wordBreakRanks[0], Kind::Space);
  EXPECT_EQ(text.getRubyTextAt(0), "thai");
  EXPECT_EQ(text.rubyTexts.size(), 1u);
  expectAligned(text);
  text.addWordWithBoundary("EPUB", REGULAR, false, Kind::Word, 2051, 0);
  ASSERT_EQ(text.size(), 2u);
  EXPECT_EQ(text.wordAt(1), "EPUB");
  EXPECT_EQ(text.wordBreakRanks[1], Kind::Word);
  EXPECT_EQ(text.visibleOffsetAt(1), 2051u);
  EXPECT_EQ(text.rubyTexts.size(), 2u);
  expectAligned(text);
}
}  // namespace
