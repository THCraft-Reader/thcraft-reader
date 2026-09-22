#include <Epub/Page.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <NativeAllocator.h>
#include <NativeTextEngine.h>
#include <fontIds.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using ParseStatus = ChapterHtmlSlimParser::ParseStatus;
using WordBox = std::tuple<std::string, int, int, int, int, unsigned>;
using LinkBox = std::tuple<std::string, int, int, int, int>;

struct LineObservation {
  std::string text;
  int x = 0, y = 0, height = 0;
  uint32_t start = 0, end = 0;
  std::vector<WordBox> words;
  std::vector<std::string> ruby;
  bool operator==(const LineObservation&) const = default;
};
struct PageObservation {
  std::vector<LineObservation> lines;
  std::vector<LinkBox> links;
  std::vector<std::pair<std::string, std::string>> footnotes;
  bool operator==(const PageObservation&) const = default;
};
struct Document {
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<uint32_t> offsets;
  ParseStatus status = ParseStatus::More;
  bool finished = false;
};
struct ParseOptions {
  uint16_t width = 260, height = 130;
  float compression = 1.0f;
  bool focus = false;
  CssTextAlign alignment = CssTextAlign::Left;
};

uint32_t scalars(std::string_view text) {
  return static_cast<uint32_t>(
      std::count_if(text.begin(), text.end(), [](unsigned char byte) { return (byte & 0xc0) != 0x80; }));
}
std::string repeat(std::string_view text, unsigned count) {
  std::string result;
  result.reserve(text.size() * count);
  while (count--) result += text;
  return result;
}
std::string paragraph(std::string_view body) {
  return "<p style=\"margin:0;padding:0;text-indent:0\">" + std::string(body) + "</p>";
}
std::string xhtml(std::string_view body) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
         "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Not visible</title></head><body>" +
         std::string(body) + "</body></html>";
}
std::vector<const PageLine*> lines(const Page& page) {
  std::vector<const PageLine*> result;
  for (const auto& element : page.elements)
    if (element->getTag() == TAG_PageLine) result.push_back(static_cast<const PageLine*>(element.get()));
  return result;
}
std::string joined(const Document& document) {
  std::string result;
  for (const auto& page : document.pages)
    for (const auto* line : lines(*page)) {
      const auto* native = line->getBlock()->nativeLine();
      EXPECT_NE(native, nullptr);
      if (native) result += native->logicalText();
    }
  return result;
}

class NativeTextEpubParserTest : public testing::Test {
 protected:
  static constexpr int FONT = NOTOSANS_14_FONT_ID;
  static constexpr size_t UNLIMITED = std::numeric_limits<size_t>::max();
  NativeTextEngine engine;
  HalDisplay display;
  GfxRenderer renderer{display};
  std::filesystem::path root, previousRoot;
  const std::string chapterPath = "/chapter.xhtml";
  size_t initialNativeBytes = 0;

  static void SetUpTestSuite() {
    NativeTextEngine warm;
    ASSERT_EQ(warm.initialize(), TextStatus::Ok);
    warm.shutdown();
  }

  void SetUp() override {
    native_text::failAllocationsAfter(UNLIMITED);
    initialNativeBytes = native_text::allocationStats().used;
    ASSERT_EQ(Storage.openHandles(), 0u);
    previousRoot = Storage.root();
    root = std::filesystem::temp_directory_path() /
           ("thcraft-native-epub-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Storage.setRoot(root);
    ASSERT_EQ(engine.initialize(), TextStatus::Ok);
    renderer.begin();
    renderer.setNativeTextEngine(&engine);
  }
  void TearDown() override {
    native_text::failAllocationsAfter(UNLIMITED);
    Storage.resetFaults();
    renderer.setNativeTextEngine(nullptr);
    engine.shutdown();
    EXPECT_EQ(native_text::allocationStats().used, initialNativeBytes);
    EXPECT_EQ(Storage.openHandles(), 0u);
    EXPECT_LE(native_text::allocationStats().peak, native_text::MEMORY_LIMIT);
    if (Storage.openHandles() == 0) Storage.setRoot(previousRoot);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error) << error.message();
  }
  void writeChapter(std::string_view xml) {
    HalFile file;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", chapterPath, file));
    ASSERT_EQ(file.write(xml.data(), xml.size()), xml.size());
    ASSERT_TRUE(file.close());
  }
  std::unique_ptr<ChapterHtmlSlimParser> parser(Document& output, const ParseOptions& options) {
    return std::make_unique<ChapterHtmlSlimParser>(
        nullptr, chapterPath, renderer, FONT, options.compression, false, static_cast<uint8_t>(options.alignment),
        options.width, options.height, false, options.focus,
        [&output](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t sourceOffset) {
          EXPECT_NE(page, nullptr);
          if (!page) return;
          EXPECT_FALSE(page->elements.empty()) << "An empty page was published";
          // This is the Section callback's visible-offset LUT, not serialized Page body data.
          output.offsets.push_back(sourceOffset);
          output.pages.push_back(std::move(page));
        },
        true, "", "");
  }
  ParseStatus drain(ChapterHtmlSlimParser& parser, size_t maxSteps) {
    for (size_t step = 0; step < maxSteps; ++step) {
      const auto status = parser.parseStep();
      if (status != ParseStatus::More) return status;
    }
    ADD_FAILURE() << "Parser did not terminate after more steps than source bytes";
    return ParseStatus::Error;
  }
  Document parse(std::string_view body, size_t maximumRead = UNLIMITED, ParseOptions options = {}) {
    Storage.resetFaults();
    const auto xml = xhtml(body);
    writeChapter(xml);
    Storage.faults().maximumRead = maximumRead;
    renderer.clearTextStatus();
    Document output;
    auto instance = parser(output, options);
    if (!instance->beginParse()) {
      ADD_FAILURE() << "beginParse rejected a readable XHTML fixture";
      output.status = ParseStatus::Error;
      return output;
    }
    output.status = drain(*instance, xml.size() + 2);
    output.finished = instance->finishParse();
    instance.reset();
    Storage.resetFaults();
    EXPECT_EQ(Storage.openHandles(), 0u);
    return output;
  }
  PageObservation observe(const Page& page, bool includeSource = true, float compression = 1.0f) {
    PageObservation result;
    for (const auto* line : lines(page)) {
      const auto& block = *line->getBlock();
      EXPECT_TRUE(block.valid());
      const auto* native = block.nativeLine();
      EXPECT_NE(native, nullptr);
      if (!native) continue;
      LineObservation value;
      value.text = native->logicalText();
      value.x = line->xPos;
      value.y = line->yPos;
      value.height = block.layoutHeight(renderer, FONT, compression);
      if (includeSource) {
        value.start = block.sourceStartOffset();
        value.end = block.sourceEndOffset();
      }
      for (uint16_t i = 0; i < block.wordCount(); ++i)
        value.words.emplace_back(block.wordText(i), line->xPos + block.wordXpos(i), line->yPos + block.wordTop(i),
                                 block.wordWidth(i), block.wordHeight(i), static_cast<unsigned>(block.wordStyle(i)));
      for (const auto& annotation : native->ruby.span())
        value.ruby.emplace_back(native->rubyText.data() + annotation.textOffset, annotation.textBytes);
      result.lines.push_back(std::move(value));
    }
    for (const auto& link : page.links) result.links.emplace_back(link.href, link.x, link.y, link.width, link.height);
    for (const auto& note : page.footnotes) result.footnotes.emplace_back(note.number, note.href);
    return result;
  }
  void expectSame(const Document& first, const Document& second, float compression = 1.0f) {
    ASSERT_EQ(first.status, ParseStatus::Done);
    ASSERT_EQ(second.status, ParseStatus::Done);
    ASSERT_TRUE(first.finished);
    ASSERT_TRUE(second.finished);
    ASSERT_EQ(first.pages.size(), second.pages.size());
    EXPECT_EQ(first.offsets, second.offsets);
    for (size_t page = 0; page < first.pages.size(); ++page) {
      SCOPED_TRACE(page);
      EXPECT_EQ(observe(*first.pages[page], true, compression), observe(*second.pages[page], true, compression));
    }
  }
  void expectSourceCoverage(const Document& document, std::string_view original) {
    ASSERT_FALSE(document.pages.empty());
    ASSERT_EQ(document.pages.size(), document.offsets.size());
    uint32_t source = 0;
    for (size_t page = 0; page < document.pages.size(); ++page) {
      const auto pageLines = lines(*document.pages[page]);
      ASSERT_FALSE(pageLines.empty());
      EXPECT_EQ(document.offsets[page], source);
      for (const auto* line : pageLines) {
        const auto& block = *line->getBlock();
        ASSERT_NE(block.nativeLine(), nullptr);
        EXPECT_EQ(block.sourceStartOffset(), source);
        source += scalars(block.nativeLine()->logicalText());
        EXPECT_EQ(block.sourceEndOffset(), source);
      }
    }
    EXPECT_EQ(source, scalars(original));
    EXPECT_EQ(joined(document), original);
  }
  std::vector<uint8_t> paint(const Page& page, GfxRenderer::RenderMode mode = GfxRenderer::BW) {
    renderer.setRenderMode(mode);
    renderer.clearTextStatus();
    renderer.clearScreen(mode == GfxRenderer::BW ? 0xff : 0);
    page.render(renderer, FONT, 20, 20);
    EXPECT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
    return {renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize()};
  }
  std::unique_ptr<Page> reload(const Page& page) {
    HalFile file;
    EXPECT_TRUE(Storage.openFileForWrite("TEST", "/page.bin", file));
    EXPECT_TRUE(page.serialize(file));
    file.close();
    if (!Storage.openFileForRead("TEST", "/page.bin", file)) return nullptr;
    auto result = Page::deserialize(file);
    EXPECT_EQ(file.position(), file.size());
    return result;
  }
};

TEST_F(NativeTextEpubParserTest, InlineMarkTagsAndForcedWordBufferCutsPreserveSourceAndFocus) {
  const std::string rest = " กิ กึ กุ กู้ น้ำ กำ ปี่ ญู ฐู เก่ง " + repeat("ภาษาไทยประเทศไทย", 24);
  const std::string logical = "e\xcc\x81 กี่" + rest;
  const std::string tagged = "e<span>\xcc\x81</span> ก<b>ี่</b>" + rest;
  ParseOptions options;
  options.focus = true;
  const auto plain = parse(paragraph(logical), UNLIMITED, options);
  const auto marked = parse(paragraph(tagged), UNLIMITED, options);
  expectSame(plain, marked);
  expectSourceCoverage(marked, logical);
  ASSERT_GT(marked.pages.size(), 1u);
  ASSERT_EQ(plain.pages.size(), marked.pages.size());
  for (size_t page = 0; page < plain.pages.size(); ++page)
    EXPECT_EQ(paint(*plain.pages[page]), paint(*marked.pages[page]));

  // maximumRead controls filesystem bytes, not characterData calls: Expat keeps
  // UTF-8 scalars intact. Tags force callback/style boundaries before the marks,
  // and the >200-byte uninterrupted suffix forces partWordBuffer's own split.
  for (const size_t cap : {size_t{1}, size_t{200}}) {
    SCOPED_TRACE(cap);
    const auto chunked = parse(paragraph(tagged), cap, options);
    expectSame(marked, chunked);
  }
}

TEST_F(NativeTextEpubParserTest, DictionarySelectionsCrossParserChunksWithoutInventedThaiGaps) {
  const std::string text = repeat("ภาษาไทยประเทศไทย", 50);
  ParseOptions options;
  options.width = 280;
  options.alignment = CssTextAlign::Justify;
  const auto document = parse(paragraph(text), 200, options);
  ASSERT_EQ(document.status, ParseStatus::Done);
  ASSERT_TRUE(document.finished);
  expectSourceCoverage(document, text);
  std::vector<uint32_t> boundaries(scalars(text) + 1);
  size_t count = 0;
  ASSERT_EQ(engine.findThaiBreaks(text, boundaries, count), TextStatus::Ok);
  boundaries.resize(count);
  if (boundaries.empty() || boundaries.back() != text.size()) boundaries.push_back(static_cast<uint32_t>(text.size()));
  std::vector<std::string> expectedWords;
  uint32_t previous = 0;
  for (const uint32_t end : boundaries) {
    if (end > previous) expectedWords.emplace_back(text.substr(previous, end - previous));
    previous = end;
  }
  std::vector<std::string> selectedWords;
  size_t lineEnd = 0;
  for (const auto& page : document.pages) {
    for (const auto* line : lines(*page)) {
      const auto& block = *line->getBlock();
      const auto& native = *block.nativeLine();
      EXPECT_TRUE(native.gaps.empty());
      EXPECT_EQ(native.syntheticSuffixCp, 0u);
      lineEnd += native.text.size();
      EXPECT_NE(std::find(boundaries.begin(), boundaries.end(), lineEnd), boundaries.end());
      NativeGlyphRun run;
      ASSERT_EQ(engine.shapeLine(native.input(FONT), run), TextStatus::Ok);
      EXPECT_LE(run.advance26 + native.alignmentX26, options.width * 64);
      for (uint16_t word = 0; word < block.wordCount(); ++word) selectedWords.emplace_back(block.wordText(word));
    }
  }
  EXPECT_EQ(selectedWords, expectedWords);
  // At least one dictionary word really straddles the parser's 198-byte safe
  // UTF-8 prefix, rather than this fixture coincidentally aligning every cut.
  previous = 0;
  bool crossesChunk = false;
  for (const auto end : boundaries) {
    crossesChunk |= previous < 198 && end > 198;
    previous = end;
  }
  EXPECT_TRUE(crossesChunk);
  const auto oneByte = parse(paragraph(text), 1, options);
  const auto wholeRead = parse(paragraph(text), UNLIMITED, options);
  expectSame(document, oneByte);
  expectSame(document, wholeRead);
}

TEST_F(NativeTextEpubParserTest, ModerateRubyAtWindowEdgeFlushesOnlyAtItsSemanticBoundary) {
  const auto prefix = repeat("ภาษาไทย", 560);
  const auto base = repeat("ภาษาไทย", 90);
  const std::string logical = prefix + base + " เก่ง";
  const std::string body = prefix + "<ruby>" + base + "<rt>คำอ่าน</rt></ruby> เก่ง";
  ASSERT_LT(scalars(prefix), 4096u);
  ASSERT_GT(scalars(prefix + base), 4296u);
  ParseOptions options;
  options.width = 560;
  options.height = 220;
  const auto whole = parse(paragraph(body), UNLIMITED, options);
  const auto bytes = parse(paragraph(body), 1, options);
  expectSame(whole, bytes);
  EXPECT_EQ(joined(whole), logical);
  ASSERT_FALSE(whole.pages.empty());
  ASSERT_FALSE(lines(*whole.pages.back()).empty());
  EXPECT_EQ(lines(*whole.pages.back()).back()->getBlock()->sourceEndOffset(), scalars(logical) + scalars("คำอ่าน"));
  size_t annotations = 0;
  for (const auto& page : whole.pages) {
    for (const auto* line : lines(*page)) annotations += line->getBlock()->getNativeRuby().size();
  }
  EXPECT_EQ(annotations, 1u);
}

TEST_F(NativeTextEpubParserTest, LongParagraphSourceWindowsIgnoreReadAndInlineFragmentBoundaries) {
  const std::string unit = "ภาษาไทยประเทศไทย";
  const std::string logical = "e\xcc\x81" + repeat(unit, 310);
  ASSERT_GT(scalars(logical), 4096u);
  std::string fragments = "e<span>\xcc\x81</span>";
  // More than the old CSS word-count threshold of 320 attached fragments.
  // These tags must not turn fragments into artificial dictionary words/windows.
  for (unsigned i = 0; i < 310; ++i) fragments += "<span>ภาษา</span><span>ไทยประเทศไทย</span>";
  ParseOptions options;
  options.width = 400;
  options.height = 220;
  const auto reference = parse(paragraph(logical), UNLIMITED, options);
  ASSERT_EQ(reference.status, ParseStatus::Done);
  ASSERT_TRUE(reference.finished);
  expectSourceCoverage(reference, logical);
  for (const size_t cap : {size_t{1}, size_t{200}, UNLIMITED}) {
    SCOPED_TRACE(cap);
    const auto fragmented = parse(paragraph(fragments), cap, options);
    expectSame(reference, fragmented);
    expectSourceCoverage(fragmented, logical);
  }
}

TEST_F(NativeTextEpubParserTest, NbspRemainsLogicalAndOnlyRealSpacesCanExpand) {
  const std::string glued = "one\xc2\xa0two";
  const std::string text = "lead " + glued + " tail ภาษาไทย ภาษาไทย ภาษาไทย ภาษาไทย ภาษาไทย";
  ParseOptions options;
  options.width = 230;
  options.alignment = CssTextAlign::Justify;
  const auto document = parse(paragraph(text), 1, options);
  ASSERT_EQ(document.status, ParseStatus::Done);
  ASSERT_TRUE(document.finished);
  expectSourceCoverage(document, text);
  bool sawGlued = false, expandedSpace = false;
  for (const auto& page : document.pages)
    for (const auto* line : lines(*page)) {
      const auto& native = *line->getBlock()->nativeLine();
      sawGlued |= native.logicalText().find(glued) != std::string_view::npos;
      for (const auto& gap : native.gaps.span()) {
        ASSERT_GT(gap.byteOffset, 0u);
        ASSERT_LE(gap.byteOffset, native.text.size());
        EXPECT_EQ(native.text[gap.byteOffset - 1], ' ');
        expandedSpace |= gap.extraAdvance26 > 0;
      }
    }
  EXPECT_TRUE(sawGlued);
  EXPECT_TRUE(expandedSpace);
  std::string entityText = text;
  entityText.replace(entityText.find("\xc2\xa0"), 2, "&#xA0;");
  const auto entity = parse(paragraph(entityText), 200, options);
  expectSame(document, entity);
}

TEST_F(NativeTextEpubParserTest, LinksSupSubRubyAndCachedPagesKeepActualGeometryAndPixels) {
  const std::string body = paragraph(
      "กิ กี่ กึ กุ กู้ น้ำ กำ ปี่ ญู ฐู เก่ง "
      "<a href=\"#base\">BASE</a> <sup><a href=\"#up\">UP</a></sup> "
      "<sub><a href=\"#down\">DOWN</a></sub> "
      "<ruby>ภาษาไทย<rt>กี่น้ำเก่ง</rt></ruby> "
      "<u>ไทย Arabic العربية Hebrew עברית</u> <s>tail</s>");
  ParseOptions options;
  options.width = 330;
  options.height = 110;
  options.compression = 0.65f;
  options.focus = true;
  const auto document = parse(body, UNLIMITED, options);
  const auto shortReads = parse(body, 1, options);
  expectSame(document, shortReads, options.compression);
  unsigned rubyCount = 0, checkedLinks = 0;
  for (const auto& page : document.pages) {
    int previousBottom = 0;
    for (const auto* line : lines(*page)) {
      const auto& block = *line->getBlock();
      const auto& native = *block.nativeLine();
      const int height = block.layoutHeight(renderer, FONT, options.compression);
      EXPECT_GE(line->yPos, previousBottom);
      EXPECT_GE(height, native.lineHeight);
      EXPECT_LE(line->yPos + height, options.height);
      previousBottom = line->yPos + height;
      NativeGlyphRun run;
      ASSERT_EQ(engine.shapeLine(native.input(FONT), run), TextStatus::Ok);
      EXPECT_GE(native.baseline * 64 + run.ink.top26, 0);
      EXPECT_LE(native.baseline * 64 + run.ink.bottom26, height * 64);
      for (size_t i = 0; i < native.ruby.size(); ++i) {
        ++rubyCount;
        EXPECT_EQ(block.nativeRubyText(i), "กี่น้ำเก่ง");
      }
      // Each link covers a complete Latin word, so its tappable rectangle must
      // be exactly the consumer's selection rectangle, including SUP/SUB lift.
      for (uint16_t word = 0; word < block.wordCount(); ++word) {
        const std::string_view text = block.wordText(word);
        const char* href = text == "BASE" ? "#base" : text == "UP" ? "#up" : text == "DOWN" ? "#down" : nullptr;
        if (!href) continue;
        const auto found = std::find_if(page->links.begin(), page->links.end(),
                                        [&](const auto& link) { return std::string_view(link.href) == href; });
        ASSERT_NE(found, page->links.end());
        EXPECT_EQ(found->x, line->xPos + block.wordXpos(word));
        EXPECT_EQ(found->y, line->yPos + block.wordTop(word));
        EXPECT_EQ(found->width, block.wordWidth(word));
        EXPECT_EQ(found->height, block.wordHeight(word));
        EXPECT_GT(found->width, 0);
        EXPECT_GT(found->height, 0);
        ++checkedLinks;
      }
      // Render a real line against a filled band mask. This catches ruby or
      // stacked-mark pixels outside the height reserved by compressed layout.
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.clearScreen();
      renderer.fillRect(0, 30, renderer.getScreenWidth(), height);
      const std::vector<uint8_t> outside(renderer.getFrameBuffer(),
                                         renderer.getFrameBuffer() + renderer.getBufferSize());
      renderer.clearScreen();
      renderer.clearTextStatus();
      block.render(renderer, FONT, 20 + line->xPos, 30);
      ASSERT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
      for (size_t byte = 0; byte < outside.size(); ++byte)
        ASSERT_EQ(renderer.getFrameBuffer()[byte] & outside[byte], outside[byte]) << "Ink escaped reserved line height";
    }
    auto cached = reload(*page);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(observe(*page, false, options.compression), observe(*cached, false, options.compression));
    for (const auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_MSB, GfxRenderer::GRAYSCALE_LSB}) {
      const auto original = paint(*page, mode);
      if (mode == GfxRenderer::BW)
        EXPECT_TRUE(std::any_of(original.begin(), original.end(), [](uint8_t byte) { return byte != 0xff; }));
      engine.clearCaches();
      EXPECT_EQ(paint(*cached, mode), original);
    }
  }
  EXPECT_EQ(rubyCount, 1u);
  EXPECT_EQ(checkedLinks, 3u);
}

TEST_F(NativeTextEpubParserTest, TwoColumnRowFootnotesStayWithTheirOriginalSourceAnchorPage) {
  const std::string left = repeat("ภาษาไทย", 18);
  const std::string right = "ขวา";
  const uint32_t leftAnchor = scalars(left), rightAnchor = scalars(left + "1" + right);
  const std::string body = "<table><tr><td>" + left + "<a href=\"#left-note\">1</a></td><td>" + right +
                           "<a href=\"#right-note\">2</a></td></tr></table>";
  ParseOptions options;
  options.width = 440;
  options.height = 85;
  const auto document = parse(body, 200, options);
  ASSERT_EQ(document.status, ParseStatus::Done);
  ASSERT_TRUE(document.finished);
  ASSERT_GT(document.pages.size(), 1u);
  const auto singleBytes = parse(body, 1, options);
  expectSame(document, singleBytes);
  std::array<int, 2> anchorPage{-1, -1}, notePage{-1, -1};
  std::vector<std::pair<uint32_t, std::string>> sourcePieces;
  bool sawParallelColumns = false;
  for (size_t page = 0; page < document.pages.size(); ++page) {
    const auto pageLines = lines(*document.pages[page]);
    for (const auto* line : pageLines) {
      const auto& block = *line->getBlock();
      sourcePieces.emplace_back(block.sourceStartOffset(), block.nativeLine()->logicalText());
      for (size_t i = 0; i < 2; ++i) {
        const auto anchor = i == 0 ? leftAnchor : rightAnchor;
        if (block.sourceStartOffset() <= anchor && anchor < block.sourceEndOffset()) {
          EXPECT_EQ(anchorPage[i], -1);
          anchorPage[i] = static_cast<int>(page);
        }
      }
      for (const auto* other : pageLines) sawParallelColumns |= line->yPos == other->yPos && line->xPos < other->xPos;
    }
    for (const auto& note : document.pages[page]->footnotes) {
      const int i = std::string_view(note.href) == "#left-note"    ? 0
                    : std::string_view(note.href) == "#right-note" ? 1
                                                                   : -1;
      ASSERT_GE(i, 0);
      EXPECT_EQ(notePage[i], -1) << "Footnote duplicated across pages";
      notePage[i] = static_cast<int>(page);
      EXPECT_STREQ(note.number, i == 0 ? "1" : "2");
      EXPECT_TRUE(std::any_of(document.pages[page]->links.begin(), document.pages[page]->links.end(),
                              [&](const auto& link) { return std::string_view(link.href) == note.href; }));
    }
  }
  EXPECT_TRUE(sawParallelColumns) << "Fixture must exercise grid rows, not stacked fallback";
  EXPECT_GT(anchorPage[0], 0);
  EXPECT_EQ(anchorPage[1], 0);
  EXPECT_EQ(notePage, anchorPage);
  std::sort(sourcePieces.begin(), sourcePieces.end());
  std::string reconstructed;
  for (const auto& [start, text] : sourcePieces) reconstructed += text;
  EXPECT_EQ(reconstructed, left + "1" + right + "2");
}

TEST_F(NativeTextEpubParserTest, NativeOomAfterPartialPageRejectsFinishAndCanRecover) {
  const auto prefix = xhtml(paragraph("first กี่"));
  const auto split = prefix.find("</body>");
  const std::string xml = prefix.substr(0, split) + paragraph(repeat("ภาษาไทย", 30)) + "</body></html>";
  writeChapter(xml);
  Storage.faults().maximumRead = split;
  ParseOptions options;
  options.height = 700;
  Document failed;
  const auto beforePixels =
      std::vector<uint8_t>(renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize());
  {
    auto instance = parser(failed, options);
    ASSERT_TRUE(instance->beginParse());
    ASSERT_EQ(instance->parseStep(), ParseStatus::More);
    ASSERT_TRUE(failed.pages.empty());
    native_text::failAllocationsAfter(0);
    EXPECT_EQ(drain(*instance, xml.size() + 2), ParseStatus::Error);
    EXPECT_EQ(renderer.lastTextStatus(), TextStatus::OutOfMemory);
    EXPECT_FALSE(instance->finishParse());
    EXPECT_TRUE(failed.pages.empty());
  }
  native_text::failAllocationsAfter(UNLIMITED);
  Storage.resetFaults();
  EXPECT_EQ(Storage.openHandles(), 0u);
  EXPECT_EQ(std::vector<uint8_t>(renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize()),
            beforePixels);
  engine.clearCaches();
  const auto recovered = parse(paragraph("first กี่") + paragraph("ภาษาไทย"), 1, options);
  ASSERT_EQ(recovered.status, ParseStatus::Done);
  ASSERT_TRUE(recovered.finished);
  EXPECT_EQ(joined(recovered), "first กี่ภาษาไทย");
}

TEST_F(NativeTextEpubParserTest, SourceReadFailureRejectsTrailingPartialPageAndClosesStorage) {
  const auto prefix = xhtml(paragraph("already laid out กี่"));
  const auto split = prefix.find("</body>");
  const std::string xml = prefix.substr(0, split) + paragraph(repeat("ภาษาไทย", 30)) + "</body></html>";
  writeChapter(xml);
  Storage.faults().maximumRead = split;
  Storage.faults().readPath = "chapter.xhtml";
  Storage.faults().readBytes = split;
  ParseOptions options;
  options.height = 700;
  Document failed;
  {
    auto instance = parser(failed, options);
    ASSERT_TRUE(instance->beginParse());
    ASSERT_EQ(instance->parseStep(), ParseStatus::More);
    ASSERT_TRUE(failed.pages.empty());
    EXPECT_EQ(instance->parseStep(), ParseStatus::Error);
    EXPECT_FALSE(instance->finishParse());
    EXPECT_TRUE(failed.pages.empty());
  }
  EXPECT_EQ(Storage.openHandles(), 0u);
  Storage.resetFaults();
  ASSERT_TRUE(Storage.beginUsbDrive());
  Storage.endUsbDrive();
  const auto recovered = parse(paragraph("กี่ recover"), 200, options);
  ASSERT_EQ(recovered.status, ParseStatus::Done);
  ASSERT_TRUE(recovered.finished);
  EXPECT_EQ(joined(recovered), "กี่ recover");
}

TEST_F(NativeTextEpubParserTest, GeneratedListNumbersNeverAdvanceOriginalCodepointPositions) {
  const std::string first = "e\xcc\x81กี่" + repeat("ภาษาไทย", 20);
  const std::string second = "น้ำ" + repeat("ประเทศไทย", 12);
  const std::string body = "<ol><li>" + first + "</li><li>" + second + "</li></ol>";
  ParseOptions options;
  options.height = 90;
  const auto document = parse(body, 1, options);
  ASSERT_EQ(document.status, ParseStatus::Done);
  ASSERT_TRUE(document.finished);
  const auto fullRead = parse(body, UNLIMITED, options);
  const auto chunks = parse(body, 200, options);
  expectSame(document, fullRead);
  expectSame(document, chunks);
  uint32_t source = 0;
  unsigned markers = 0;
  std::string restored;
  for (size_t page = 0; page < document.pages.size(); ++page) {
    EXPECT_EQ(document.offsets[page], source);
    for (const auto* line : lines(*document.pages[page])) {
      const auto& block = *line->getBlock();
      ASSERT_NE(block.nativeLine(), nullptr);
      std::string text(block.nativeLine()->logicalText());
      if (source == 0 || source == scalars(first)) {
        const std::string marker = source == 0 ? "1. " : "2. ";
        ASSERT_EQ(text.find(marker), 0u);
        text.erase(0, marker.size());
        ++markers;
      }
      EXPECT_EQ(block.sourceStartOffset(), source);
      source += scalars(text);
      EXPECT_EQ(block.sourceEndOffset(), source);
      restored += text;
    }
  }
  EXPECT_EQ(markers, 2u);
  EXPECT_EQ(restored, first + second);
  EXPECT_EQ(source, scalars(first + second));
}

TEST_F(NativeTextEpubParserTest, OversizedOpenRubyFailsCapacityInsteadOfPublishingAnIncompleteGroup) {
  const std::string body = paragraph("<ruby>" + repeat("ก", 4300) + "<rt>กี่</rt></ruby>");
  const auto document = parse(body, 200);
  EXPECT_EQ(document.status, ParseStatus::Error);
  EXPECT_FALSE(document.finished);
  EXPECT_EQ(renderer.lastTextStatus(), TextStatus::CapacityExceeded);
  EXPECT_TRUE(document.pages.empty());
  const auto recovered = parse(paragraph("<ruby>ก<rt>กี่</rt></ruby>"), 1);
  ASSERT_EQ(recovered.status, ParseStatus::Done);
  ASSERT_TRUE(recovered.finished);
  EXPECT_EQ(joined(recovered), "ก");
}
}  // namespace
