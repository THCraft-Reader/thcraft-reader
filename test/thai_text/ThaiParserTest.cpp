#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <ThaiCluster.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "Epub/parsers/ChapterHtmlSlimParser.h"

namespace {
struct ParsedToken {
  std::string text;
  EpdFontFamily::Style style;
  uint32_t offset;
  uint8_t link;
  uint8_t focus;
  bool continues;
  bool noSpace;
  std::string ruby;
  bool operator==(const ParsedToken&) const = default;
};

void PrintTo(const ParsedToken& token, std::ostream* out) {
  *out << token.text << " style=" << unsigned(token.style) << " offset=" << token.offset
       << " link=" << unsigned(token.link) << " focus=" << unsigned(token.focus) << " continues=" << token.continues
       << " noSpace=" << token.noSpace << " ruby=" << token.ruby;
}

// Real Expat callbacks and parser state, without a storage fixture. The parser
// owns/tears down this Expat instance exactly as it does during a book parse.
class ParserSession {
 public:
  GfxRenderer renderer;
  std::string filename;
  CssParser css{""};
  std::vector<std::unique_ptr<Page>> pages;
  ChapterHtmlSlimParser parser;

  explicit ParserSession(uint16_t width = 480, uint16_t height = 800, bool focus = false)
      : parser(
            nullptr, filename, renderer, 0, 1.0f, false, static_cast<uint8_t>(CssTextAlign::Left), width, height, false,
            focus,
            [this](std::unique_ptr<Page> page, auto, auto, uint32_t offset) {
              if (page) {
                page->visibleTextOffset = offset;
                pages.push_back(std::move(page));
              }
            },
            true, "", "", 0, {}, nullptr, &css) {
    BlockStyle root;
    root.alignment = CssTextAlign::Left;
    root.textAlignDefined = true;
    parser.blockStyleStack.push_back(root);
    parser.xmlParser_ = XML_ParserCreate(nullptr);
    XML_SetUserData(parser.xmlParser_, &parser);
    XML_SetElementHandler(parser.xmlParser_, ChapterHtmlSlimParser::startElement, ChapterHtmlSlimParser::endElement);
    XML_SetCharacterDataHandler(parser.xmlParser_, ChapterHtmlSlimParser::characterData);
    XML_SetDefaultHandlerExpand(parser.xmlParser_, ChapterHtmlSlimParser::defaultHandlerExpand);
    feed("<html><body>");
  }

  void feed(std::string_view text, size_t chunk = 0) {
    if (!chunk) chunk = text.size();
    for (size_t i = 0; i < text.size(); i += chunk) {
      const size_t count = std::min(chunk, text.size() - i);
      ASSERT_EQ(XML_Parse(parser.xmlParser_, text.data() + i, static_cast<int>(count), false), XML_STATUS_OK)
          << XML_ErrorString(XML_GetErrorCode(parser.xmlParser_));
    }
  }

  void callbacks(std::string_view text, size_t chunk) {
    for (size_t i = 0; i < text.size(); i += chunk) {
      ChapterHtmlSlimParser::characterData(&parser, text.data() + i,
                                           static_cast<int>(std::min(chunk, text.size() - i)));
    }
  }

  std::vector<ParsedToken> tokens() {
    parser.endTextRun();
    std::vector<ParsedToken> result;
    const auto& block = parser.currentTextBlock;
    if (!block) return result;
    for (size_t i = 0; i < block->size(); ++i) {
      result.push_back({std::string(block->wordAt(i)), block->wordStyles[i], block->visibleOffsetAt(i),
                        block->wordLinkIds[i], block->wordFocusBoundary[i], block->wordContinues[i],
                        block->wordNoSpaceBefore[i], block->getRubyTextAt(i)});
    }
    return result;
  }

  void finish() {
    feed("</body></html>");
    EXPECT_EQ(XML_Parse(parser.xmlParser_, "", 0, true), XML_STATUS_OK);
    EXPECT_TRUE(parser.finishParse());
  }

  std::vector<std::string> lines() const {
    std::vector<std::string> result;
    for (const auto& page : pages) {
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const auto* line = static_cast<const PageLine&>(*element).getBlock();
        auto& text = result.emplace_back();
        for (uint16_t i = 0; i < line->wordCount(); ++i) text += line->wordText(i);
      }
    }
    return result;
  }
};

std::string joined(const std::vector<ParsedToken>& tokens) {
  std::string result;
  for (const auto& token : tokens) result += token.text;
  return result;
}
std::string joined(const std::vector<std::string>& lines) {
  std::string result;
  for (const auto& line : lines) result += line;
  return result;
}
std::vector<std::string> textOnly(const std::vector<ParsedToken>& tokens) {
  std::vector<std::string> result;
  for (const auto& token : tokens) result.push_back(token.text);
  return result;
}

TEST(ThaiParserTest, ProductionDictionarySurvivesEveryCallbackAndRawWindowBoundary) {
  std::string source;
  for (int i = 0; i < 9; ++i) source += "ประเทศไทยมีประชากรจำนวนมาก";
  source += "กี่น้ำเรื่อง";
  ParserSession whole;
  whole.feed("<p>");
  whole.callbacks(source, source.size());
  const auto expected = whole.tokens();
  ASSERT_EQ(joined(expected), source);
  ASSERT_GE(expected.size(), 4u);
  EXPECT_EQ(textOnly({expected.begin(), expected.begin() + 4}),
            (std::vector<std::string>{"ประเทศไทย", "มี", "ประชากร", "จำนวนมาก"}));
  for (size_t chunk : {1u, 2u, 3u, 7u, 198u, 199u, 200u, 201u, 202u, 767u, 768u, 769u}) {
    SCOPED_TRACE(chunk);
    ParserSession split;
    split.feed("<p>");
    split.callbacks(source, chunk);
    EXPECT_EQ(split.tokens(), expected);
    EXPECT_EQ(split.parser.visibleTextOffset, whole.parser.visibleTextOffset);
    ASSERT_TRUE(split.parser.thaiRun);
    EXPECT_LE(split.parser.thaiRun->highWater, 768u);
  }
}

TEST(ThaiParserTest, ExpatByteChunksPreserveStylesLinksAndOffsets) {
  // A markup delimiter makes Expat deliver the final text token before inspection.
  const std::string markup = "<p><b>ก</b>ี่ประเทศ<b>ไทย</b>มี<a href=\"#note\">ประชากร</a>จำนวนมาก<!--complete-text-->";
  ParserSession whole;
  whole.feed(markup);
  const auto expected = whole.tokens();
  for (size_t chunk : {1u, 2u, 3u, 5u, 199u, 200u, 201u, 768u}) {
    SCOPED_TRACE(chunk);
    ParserSession split;
    split.feed(markup, chunk);
    EXPECT_EQ(split.tokens(), expected);
  }
}

TEST(ThaiParserTest, ProductionMakRemainsOneWordAcrossInlineStyle) {
  ParserSession plain;
  plain.feed("<p>ยิ่งได้มากเท่าไหร่<!--complete-text-->");
  EXPECT_EQ(textOnly(plain.tokens()), (std::vector<std::string>{"ยิ่ง", "ได้", "มาก", "เท่าไหร่"}));

  const std::string markup = "<p>ยิ่งได้<b>มา</b>กเท่าไหร่<!--complete-text-->";
  ParserSession whole;
  whole.feed(markup);
  const auto expected = whole.tokens();
  ASSERT_EQ(textOnly(expected), (std::vector<std::string>{"ยิ่ง", "ได้", "มา", "ก", "เท่าไหร่"}));
  EXPECT_EQ(expected[2].style, EpdFontFamily::BOLD);
  EXPECT_TRUE(expected[3].continues);
  EXPECT_FALSE(expected[3].noSpace);
  EXPECT_EQ(expected[3].offset, 9u);
  for (size_t chunk = 1; chunk <= markup.size(); ++chunk) {
    SCOPED_TRACE(chunk);
    ParserSession split;
    split.feed(markup, chunk);
    EXPECT_EQ(split.tokens(), expected);
  }
}

TEST(ThaiParserTest, RepairsGreedySingletonTailAcrossInlineStyle) {
  // Front-compressed entries: กข, กขค, คง, ง.
  const std::array<uint8_t, 14> data{0, 2, 1, 2, 2, 1, 4, 0, 2, 4, 7, 0, 1, 7};
  const std::array<uint32_t, 2> offsets{0, 14};
  const thai::DictionaryView dictionary{data.data(), data.size(), offsets.data(), offsets.size(), 4, 0};
  const std::string markup = "<p>กข<b>คง</b><!--complete-text-->";
  for (size_t chunk = 1; chunk <= markup.size(); ++chunk) {
    SCOPED_TRACE(chunk);
    ParserSession session;
    session.parser.thaiDictionaryOverride = &dictionary;
    session.feed(markup, chunk);
    const auto tokens = session.tokens();
    ASSERT_EQ(textOnly(tokens), (std::vector<std::string>{"กข", "คง"}));
    EXPECT_EQ(tokens[1].style, EpdFontFamily::BOLD);
    EXPECT_EQ(tokens[1].offset, 2u);
  }
}

TEST(ThaiParserTest, ClusterUsesFirstBaseStyleAcrossMarkup) {
  ParserSession marked;
  marked.feed("<p><b>ก</b>ี่");
  const auto tokens = marked.tokens();
  ASSERT_EQ(tokens.size(), 1u);
  EXPECT_EQ(tokens[0].text, "กี่");
  EXPECT_EQ(tokens[0].style, EpdFontFamily::BOLD);
  EXPECT_EQ(tokens[0].offset, 0u);

  ParserSession leading;
  leading.feed("<p>เ<b>ก</b>่ง");
  const auto vowel = leading.tokens();
  ASSERT_EQ(joined(vowel), "เก่ง");
  ASSERT_GE(vowel.size(), 2u);
  EXPECT_EQ(vowel[0].text, "เก่");
  EXPECT_EQ(vowel[0].style, EpdFontFamily::BOLD);
  EXPECT_TRUE(vowel[1].continues);
  EXPECT_FALSE(vowel[1].noSpace);
}

TEST(ThaiParserTest, StyleAndLinkPiecesDoNotCreateWordBreaks) {
  ParserSession session;
  session.feed("<p>ประเทศ<b>ไทย</b> ประเทศ<a href=\"#note\">ไทย</a>");
  const auto tokens = session.tokens();
  ASSERT_EQ(textOnly(tokens), (std::vector<std::string>{"ประเทศ", "ไทย", "ประเทศ", "ไทย"}));
  EXPECT_EQ(tokens[1].style, EpdFontFamily::BOLD);
  EXPECT_TRUE(tokens[1].continues);
  EXPECT_FALSE(tokens[1].noSpace);
  EXPECT_TRUE(tokens[3].continues);
  EXPECT_FALSE(tokens[3].noSpace);
  EXPECT_NE(tokens[3].link, 0u);
  ASSERT_EQ(session.parser.pendingFootnotes.size(), 1u);
  EXPECT_EQ(session.parser.pendingFootnotes[0].wordIndex, 4);
}

TEST(ThaiParserTest, LongestLexicalEntryCrossesOldRawBufferLimit) {
  std::array<uint8_t, 72> data{};
  data[1] = 70;
  std::fill(data.begin() + 2, data.end(), 1);
  const std::array<uint32_t, 2> offsets{0, 72};
  const thai::DictionaryView dictionary{data.data(), data.size(), offsets.data(), offsets.size(), 1, 0};
  std::string word;
  for (int i = 0; i < 70; ++i) word += "ก";
  for (size_t chunk : {1u, 3u, 199u, 200u, 201u, 210u}) {
    SCOPED_TRACE(chunk);
    ParserSession session;
    session.parser.thaiDictionaryOverride = &dictionary;
    session.feed("<p>");
    session.callbacks(word, chunk);
    EXPECT_EQ(textOnly(session.tokens()), (std::vector<std::string>{word}));
  }
}

TEST(ThaiParserTest, IgnoredCharactersAndZeroWidthBreakKeepSourceOffsets) {
  ParserSession session;
  session.feed("<p>ภาษา\xEF\xBB\xBFไทย&#x200B;EPUB");
  const auto tokens = session.tokens();
  ASSERT_EQ(textOnly(tokens), (std::vector<std::string>{"ภาษา", "ไทย", "EPUB"}));
  EXPECT_EQ(tokens[0].offset, 0u);
  EXPECT_EQ(tokens[1].offset, 5u);
  EXPECT_EQ(tokens[2].offset, 9u);
  EXPECT_EQ(session.parser.visibleTextOffset, 13u);
  EXPECT_TRUE(tokens[2].continues);
  EXPECT_TRUE(tokens[2].noSpace);

  ParserSession initial;
  initial.feed("<p>EPUB&#x200B;ไทย");
  const auto firstThai = initial.tokens();
  EXPECT_EQ(joined(firstThai), "EPUBไทย");
  ASSERT_GE(firstThai.size(), 2u);
  EXPECT_FALSE(firstThai[0].continues);
  EXPECT_TRUE(firstThai[1].continues);
  EXPECT_TRUE(firstThai[1].noSpace);

  ParserSession insideCluster;
  insideCluster.feed("<p>เก&#xFEFF;่ง");
  const auto gap = insideCluster.tokens();
  ASSERT_EQ(textOnly(gap), (std::vector<std::string>{"เก่", "ง"}));
  EXPECT_EQ(gap[0].offset, 0u);
  EXPECT_EQ(gap[1].offset, 4u);
  EXPECT_TRUE(gap[1].continues);
  EXPECT_FALSE(gap[1].noSpace);
}

TEST(ThaiParserTest, GenericTransitionsRetainFocusVersionsTimesAndNbspGlue) {
  ParserSession session(480, 800, true);
  session.feed("<p>ภาษาไทยEPUB 4.2.0 02:40 ไทย&#xA0;EPUB ไทย&#x202F;12");
  const auto tokens = session.tokens();
  auto find = [&](const std::string& text) {
    return std::find_if(tokens.begin(), tokens.end(), [&](const auto& token) { return token.text == text; });
  };
  const auto latin = find("EPUB");
  ASSERT_NE(latin, tokens.end());
  EXPECT_NE(latin->focus, 0u);
  EXPECT_TRUE(latin->continues);
  EXPECT_TRUE(latin->noSpace);
  EXPECT_NE(find("4.2.0"), tokens.end());
  EXPECT_NE(find("02:40"), tokens.end());
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].text != " ") continue;
    ASSERT_LT(i + 1, tokens.size());
    EXPECT_TRUE(tokens[i].continues);
    EXPECT_FALSE(tokens[i].noSpace);
    EXPECT_TRUE(tokens[i + 1].continues);
    EXPECT_FALSE(tokens[i + 1].noSpace);
  }
}

TEST(ThaiParserTest, UrlModeSurvivesGenericOverflowAndInlineStyleChanges) {
  for (const char* scheme : {"http://", "https://", "www."}) {
    const std::string prefix = std::string(scheme) + std::string(760, 'x');
    const std::string suffix = "/ภาษาไทย?q=12";
    ParserSession session;
    session.feed("<p>" + prefix + "<b>" + suffix + "</b>", 1);
    const auto tokens = session.tokens();
    EXPECT_EQ(joined(tokens), prefix + suffix);
    EXPECT_FALSE(session.parser.thaiRun);
    ASSERT_GE(tokens.size(), 2u);
    for (size_t i = 1; i < tokens.size(); ++i) {
      EXPECT_TRUE(tokens[i].continues);
      EXPECT_FALSE(tokens[i].noSpace);
    }
  }
}

TEST(ThaiParserTest, RubyBaseFlushesBeforeAnnotationAndKeepsGroupProtection) {
  ParserSession session;
  session.feed("<p>ก่อน<ruby>ภาษาไทย<rt>คำอ่าน</rt></ruby>หลัง");
  const auto tokens = session.tokens();
  EXPECT_EQ(joined(tokens), "ก่อนภาษาไทยหลัง");
  const auto ruby = std::find_if(tokens.begin(), tokens.end(), [](const auto& token) { return !token.ruby.empty(); });
  ASSERT_NE(ruby, tokens.end());
  EXPECT_EQ(ruby->ruby, "คำอ่าน");
  std::string base = ruby->text;
  for (auto next = ruby + 1; next != tokens.end() && (next->style & EpdFontFamily::RUBY_CONTINUE); ++next) {
    EXPECT_TRUE(next->continues);
    EXPECT_FALSE(next->noSpace);
    base += next->text;
  }
  EXPECT_EQ(base, "ภาษาไทย");
  EXPECT_EQ(session.parser.thaiRun->bytes, 0u);
}

TEST(ThaiParserTest, BlockTableBreakAndImageTransitionsDoNotLeakPendingText) {
  ParserSession session(480, 800);
  session.feed(
      "<p>ภาษา<br/>ไทย</p><table><tr><td>ประเทศ</td><td>ไทย</td></tr></table>"
      "<p>ก่อน<img alt=\"x\"/>หลัง</p>",
      1);
  session.finish();
  const auto lines = session.lines();
  ASSERT_GE(lines.size(), 4u);
  EXPECT_EQ(lines[0], "ภาษา");
  EXPECT_EQ(lines[1], "ไทย");
  const auto rendered = joined(lines);
  EXPECT_EQ(rendered.find("ภาษาไทยประเทศไทยก่อน"), 0u);
  EXPECT_NE(rendered.find("หลัง"), std::string::npos);
  EXPECT_EQ(session.parser.thaiRun->bytes, 0u);
}

TEST(ThaiParserTest, DeferredThaiFootnoteFollowsItsLinkedLabelToLaterPage) {
  ParserSession session(80, 16);
  session.feed("<p>one two three four <a href=\"#note\">ประเทศไทย</a>", 1);
  ASSERT_EQ(session.parser.pendingFootnotes.size(), 1u);
  EXPECT_EQ(session.parser.pendingFootnotes[0].wordIndex, -1);
  session.feed("</p>");
  session.finish();
  size_t footnotePage = session.pages.size();
  size_t linkPage = session.pages.size();
  for (size_t i = 0; i < session.pages.size(); ++i) {
    for (const auto& entry : session.pages[i]->footnotes) {
      if (std::string(entry.href) == "#note") {
        footnotePage = i;
        EXPECT_STREQ(entry.number, "ประเทศไทย");
      }
    }
    for (const auto& link : session.pages[i]->links) {
      if (std::string(link.href) == "#note") linkPage = i;
    }
  }
  ASSERT_LT(footnotePage, session.pages.size());
  EXPECT_GT(footnotePage, 0u);
  EXPECT_EQ(footnotePage, linkPage);
  EXPECT_TRUE(session.parser.pendingFootnotes.empty());
}

TEST(ThaiParserTest, Unknown64KiBCallbackRetainsBoundedAnalyzerAndPageOffsets) {
  const thai::DictionaryView empty;
  std::string source;
  source.reserve(65536);
  for (int i = 0; i < 21845; ++i) source += "ฃ";
  source += "Z";
  ParserSession session(240, 160);
  session.parser.thaiDictionaryOverride = &empty;
  session.feed("<p>");
  session.callbacks(source, source.size());
  ASSERT_TRUE(session.parser.thaiRun);
  EXPECT_LE(session.parser.thaiRun->highWater, 768u);
  EXPECT_LE(session.parser.thaiRun->count, 256u);
  EXPECT_LE(session.parser.currentTextBlock->size(), 320u);
  session.feed("</p>");
  session.finish();
  EXPECT_EQ(joined(session.lines()), source);
  for (size_t i = 1; i < session.pages.size(); ++i) {
    EXPECT_GT(session.pages[i]->visibleTextOffset, session.pages[i - 1]->visibleTextOffset);
  }
  EXPECT_FALSE(session.parser.thaiAnalysisUnavailable());
}

TEST(ThaiParserTest, RepeatedSignsFallBackWithoutDroppingBytesThenResumeDictionary) {
  std::string source = "ก";
  for (int i = 0; i < 1000; ++i) source += "่";
  source += "ประเทศไทย";
  ParserSession session;
  session.feed("<p>");
  session.callbacks(source, 1);
  const auto tokens = session.tokens();
  EXPECT_EQ(joined(tokens), source);
  ASSERT_FALSE(tokens.empty());
  EXPECT_EQ(tokens.back().text, "ประเทศไทย");
  EXPECT_LE(session.parser.thaiRun->highWater, 768u);
  EXPECT_FALSE(session.parser.thaiRun->malformed);
  EXPECT_FALSE(session.parser.thaiAnalysisUnavailable());
}

TEST(ThaiParserTest, AllocationFailureIsLifetimeLegacyFallbackWithoutByteLoss) {
  ParserSession session;
  session.parser.failThaiAllocation = true;
  session.feed("<p>ประเทศไทยกี่ ");
  session.parser.failThaiAllocation = false;
  session.feed("ภาษาไทย");
  const auto tokens = session.tokens();
  EXPECT_EQ(joined(tokens), "ประเทศไทยกี่ภาษาไทย");
  EXPECT_FALSE(session.parser.thaiRun);
  EXPECT_TRUE(session.parser.thaiAnalysisUnavailable());
  EXPECT_TRUE(session.parser.thaiAllocationAttempted);
}

TEST(ThaiParserTest, InvalidDictionaryPreservesClustersAndLatchesUnavailable) {
  const std::array<uint8_t, 3> bad{1, 1, 1};
  const std::array<uint32_t, 2> offsets{0, 3};
  const thai::DictionaryView invalid{bad.data(), bad.size(), offsets.data(), offsets.size(), 1, 0};
  ParserSession session;
  session.parser.thaiDictionaryOverride = &invalid;
  session.feed("<p>กี่กุ่ประเทศไทย");
  const auto tokens = session.tokens();
  EXPECT_EQ(joined(tokens), "กี่กุ่ประเทศไทย");
  ASSERT_GE(tokens.size(), 2u);
  EXPECT_EQ(tokens[0].text, "กี่");
  EXPECT_EQ(tokens[1].text, "กุ่");
  EXPECT_TRUE(session.parser.thaiAnalysisUnavailable());
}

TEST(ThaiParserTest, LinkedBaseOwnsMarksOutsideAnchorAndResolvesItsFootnote) {
  ParserSession session;
  session.feed("<p><a href=\"#note\">ก</a>ี่");
  const auto tokens = session.tokens();
  ASSERT_EQ(tokens.size(), 1u);
  EXPECT_EQ(tokens[0].text, "กี่");
  EXPECT_NE(tokens[0].link, 0u);
  EXPECT_EQ(tokens[0].style, EpdFontFamily::UNDERLINE);
  ASSERT_EQ(session.parser.pendingFootnotes.size(), 1u);
  EXPECT_EQ(session.parser.pendingFootnotes[0].wordIndex, 1);
  EXPECT_EQ(session.parser.pendingFootnotes[0].visibleEnd, 1u);
}

TEST(ThaiParserTest, LinkForcingStackedTableDoesNotDrainIntoPreviousCell) {
  ParserSession session;
  session.feed("<table><tr><td>ก่อน</td><td>ประเทศ<a href=\"#note\">ไทย</a></td></tr></table>", 1);
  session.finish();
  EXPECT_EQ(joined(session.lines()), "ก่อนประเทศไทย");
  size_t footnotes = 0;
  size_t links = 0;
  for (const auto& page : session.pages) {
    footnotes += page->footnotes.size();
    links += page->links.size();
    if (!page->footnotes.empty()) {
      ASSERT_FALSE(page->links.empty());
      EXPECT_STREQ(page->footnotes[0].href, page->links[0].href);
    }
  }
  EXPECT_EQ(footnotes, 1u);
  EXPECT_EQ(links, 1u);
}

TEST(ThaiParserTest, ClosingPunctuationAndThaiSuffixUnitsRemainAttached) {
  ParserSession session;
  session.feed("<p>ประเทศไทย...แล้ว ประเทศไทยๆ ประเทศไทยฯลฯ");
  const auto tokens = session.tokens();
  EXPECT_EQ(joined(tokens), "ประเทศไทย...แล้วประเทศไทยๆประเทศไทยฯลฯ");
  for (const char* unit : {"...", "ๆ", "ฯลฯ"}) {
    const auto found =
        std::find_if(tokens.begin(), tokens.end(), [&](const auto& token) { return token.text == unit; });
    ASSERT_NE(found, tokens.end()) << unit;
    EXPECT_TRUE(found->continues);
    EXPECT_FALSE(found->noSpace);
  }
}

TEST(ThaiParserTest, NonThaiControlsNeverAllocateThaiState) {
  ParserSession session;
  session.feed("<p>English <b>bold</b> Ti&#x65;&#x302;&#x301;ng 中文 한글");
  EXPECT_EQ(joined(session.tokens()), "EnglishboldTiếng中文한글");
  EXPECT_FALSE(session.parser.thaiRun);
  EXPECT_FALSE(session.parser.thaiAllocationAttempted);
}
}  // namespace
