#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

class ChapterHtmlSlimParserTest : public ::testing::TestWithParam<const char*> {
 protected:
  std::string filepath = "unused.xhtml";
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{nullptr,
                               filepath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               0,
                               static_cast<uint16_t>(renderer.getScreenWidth()),
                               static_cast<uint16_t>(renderer.getScreenHeight()),
                               false,
                               false,
                               {},
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser};

  void SetUp() override {
    filepath = (std::filesystem::temp_directory_path() /
                ("crosspoint-parser-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 ".xhtml"))
                   .string();
    HalFile file;
    ASSERT_TRUE(file.open(filepath.c_str(), "wb"));
    static constexpr char xml[] = "<html><body></body></html>";
    ASSERT_EQ(file.write(xml, sizeof(xml) - 1), sizeof(xml) - 1);
    parser.currentTextBlock = std::make_unique<ParsedText>(false);
  }
  void TearDown() override {
    parser.abortParse();
    std::filesystem::remove(filepath);
  }
};

TEST_F(ChapterHtmlSlimParserTest, RubySurvivesPartialParagraphExtraction) {
  ParsedText text(false);
  text.addWord("a", EpdFontFamily::REGULAR);
  text.addWord("b", EpdFontFamily::REGULAR);
  text.addWord("c", EpdFontFamily::REGULAR);
  text.setRubyForWordAt(2, "c");
  size_t lines = 0;
  ASSERT_TRUE(text.layoutAndExtractLines(
      renderer, 0, 20,
      [&](std::unique_ptr<TextBlock> line, auto) {
        ++lines;
        EXPECT_TRUE(line->getRubyTexts().empty());
      },
      false));
  EXPECT_EQ(lines, 1u);
  const size_t retainedWords = text.size();
  ASSERT_GT(retainedWords, 0u);
  ASSERT_LT(retainedWords, 3u);
  ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) {
    ++lines;
    ASSERT_EQ(line->getRubyTexts().size(), retainedWords);
    EXPECT_EQ(line->getRubyTexts().back(), "c");
    for (size_t i = 0; i + 1 < retainedWords; ++i) EXPECT_TRUE(line->getRubyTexts()[i].empty());
  }));
  EXPECT_EQ(lines, 2u);
}

TEST_F(ChapterHtmlSlimParserTest, EmptyLayoutSucceedsWithoutEmittingLines) {
  ParsedText text(false);
  EXPECT_TRUE(
      text.layoutAndExtractLines(renderer, 0, 200, [](auto, auto) { ADD_FAILURE() << "Empty input emitted a line"; }));
}

TEST_F(ChapterHtmlSlimParserTest, LayoutReportsRejectedTextBlockWithoutEmittingIt) {
  ParsedText text(false);
  // The line's text plus its NUL exceeds TextBlock's uint16_t arena limit.
  text.addWord(std::string(UINT16_MAX, 'a'), EpdFontFamily::REGULAR);
  EXPECT_FALSE(text.layoutAndExtractLines(renderer, 0, UINT16_MAX,
                                          [](auto, auto) { ADD_FAILURE() << "A rejected line was emitted"; }));
}

TEST_F(ChapterHtmlSlimParserTest, LayoutFailureOverridesHtmlEndAndStopsPagePublication) {
  parser.viewportWidth = UINT16_MAX;
  parser.currentTextBlock->addWord(std::string(UINT16_MAX, 'a'), EpdFontFamily::REGULAR);
  size_t publishedPages = 0;
  parser.completePageFn = [&](auto, auto, auto, auto) { ++publishedPages; };
  parser.makePages();
  parser.htmlEnded_ = true;

  EXPECT_EQ(parser.parseStep(), ChapterHtmlSlimParser::ParseStatus::Error);
  ChapterHtmlSlimParser::startElement(&parser, "hr", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "later ", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  EXPECT_FALSE(parser.finishParse());
  EXPECT_EQ(publishedPages, 0u);
  EXPECT_EQ(parser.currentPage, nullptr);
}

TEST_F(ChapterHtmlSlimParserTest, FinalLayoutFailureDoesNotPublishTrailingPage) {
  parser.viewportWidth = UINT16_MAX;
  parser.currentTextBlock->addWord(std::string(UINT16_MAX, 'a'), EpdFontFamily::REGULAR);
  size_t publishedPages = 0;
  parser.completePageFn = [&](auto, auto, auto, auto) { ++publishedPages; };

  EXPECT_FALSE(parser.finishParse());
  EXPECT_EQ(publishedPages, 0u);
  EXPECT_EQ(parser.currentPage, nullptr);
}

TEST_F(ChapterHtmlSlimParserTest, UnequalTableCellsAndRubySurvivePageBreaks) {
  parser.viewportWidth = 240;
  parser.viewportHeight = 32;
  parser.tableRowCells.reserve(2);
  std::multiset<std::string> expected;
  for (int column = 0; column < 2; ++column) {
    auto cell = std::make_unique<ParsedText>(false);
    for (int index = 0; index < (column == 0 ? 30 : 3); ++index) {
      const auto word = std::string(column == 0 ? "left" : "right") + std::to_string(index);
      expected.insert(word);
      cell->addWord(word, EpdFontFamily::REGULAR);
    }
    if (column == 0) cell->setRubyGroupAt(0, 2, "reading");
    parser.tableRowCells.push_back(std::move(cell));
  }
  std::multiset<std::string> actual;
  unsigned pages = 0;
  unsigned rubyLines = 0;
  auto inspect = [&](std::unique_ptr<Page> page, auto, auto, auto) {
    ++pages;
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = *line.getBlock();
      ASSERT_TRUE(block.valid());
      EXPECT_LE(element->yPos + 16 + block.getRubyShift(12), parser.viewportHeight);
      rubyLines += block.hasRuby();
      for (uint16_t word = 0; word < block.wordCount(); ++word) actual.insert(block.wordText(word));
    }
  };
  parser.completePageFn = inspect;
  parser.finishTableRow();
  ASSERT_NE(parser.currentPage, nullptr);
  inspect(std::move(parser.currentPage), 0, 0, 0);
  EXPECT_GT(pages, 2u);
  EXPECT_EQ(rubyLines, 1u);
  EXPECT_EQ(actual, expected);
  for (const auto& lines : parser.tableCellLines) EXPECT_TRUE(lines.empty());
}

TEST_F(ChapterHtmlSlimParserTest, PageImageDeserializeRejectsMissingImageBlock) {
  const auto path = std::filesystem::temp_directory_path() / "crosspoint-missing-image-cache.bin";
  {
    HalFile output;
    ASSERT_TRUE(output.open(path.string().c_str(), "wb"));
    const int16_t coordinates[] = {0, 0};
    output.write(coordinates, sizeof(coordinates));
  }
  HalFile input;
  ASSERT_TRUE(input.open(path.string().c_str(), "rb"));
  serialization::BoundedFileReader bounded(input);
  EXPECT_EQ(PageImage::deserialize(bounded), nullptr);
}

TEST_P(ChapterHtmlSlimParserTest, KeepsCssVerticalAlignAndInternalLinkMetadata) {
  const char* verticalAlign = GetParam();
  const char* expectedHref = "#note-target";
  const XML_Char* attributes[] = {"href", expectedHref, "style", verticalAlign, nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "a", attributes);
  const uint8_t linkId = parser.currentFootnoteLinkId;
  ASSERT_NE(linkId, 0u);
  ChapterHtmlSlimParser::characterData(&parser, "1", 1);
  ChapterHtmlSlimParser::endElement(&parser, "a");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  const auto expectedStyle =
      std::string(verticalAlign).find("super") != std::string::npos ? EpdFontFamily::SUP : EpdFontFamily::SUB;
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(expectedStyle), 0u);

  ASSERT_EQ(parser.pendingFootnotes.size(), 1u);
  const FootnoteEntry& footnote = parser.pendingFootnotes.front().second;
  EXPECT_STREQ(footnote.href, expectedHref);
  ASSERT_EQ(parser.currentTextBlock->wordLinkIds.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->wordLinkIds.front(), linkId);
  EXPECT_TRUE(parser.currentTextBlock->linkTargetMatches(linkId, expectedHref));
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

TEST_F(ChapterHtmlSlimParserTest, ParagraphWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  ASSERT_TRUE(parser.beginParse());
  ChapterHtmlSlimParser::startElement(&parser, "p", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, HeaderWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  ASSERT_TRUE(parser.beginParse());
  ChapterHtmlSlimParser::startElement(&parser, "h1", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, SpanWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  ASSERT_TRUE(parser.beginParse());
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before ", 7);
  ChapterHtmlSlimParser::startElement(&parser, "span", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, " After ", 7);

  std::vector<std::string> visible;
  visible.reserve(2);
  ASSERT_TRUE(
      parser.currentTextBlock->layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, uint32_t) {
        for (uint16_t i = 0; i < line->wordCount(); ++i) visible.emplace_back(line->wordText(i));
      }));
  EXPECT_EQ(visible, (std::vector<std::string>{"Before", "After"}));
}

TEST_F(ChapterHtmlSlimParserTest, DivWithHiddenAttributeContentShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  ASSERT_TRUE(parser.beginParse());
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

}  // namespace
