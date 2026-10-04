#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <GfxRenderer.h>
#include <ThaiLayoutId.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// The reused parser host adapter has no language dictionaries for generic hyphenation.
void Hyphenator::setPreferredLanguage(const std::string&) {}

class SectionThaiCacheTestPeer {
 public:
  static ChapterHtmlSlimParser& parser(Section& section) { return *section.build_->parser; }
};

namespace {
const std::string sentence = "ประเทศไทยมีประชากรจำนวนมาก";
const std::vector<std::string> dictionaryWords = {"ประเทศไทย", "มี", "ประชากร", "จำนวนมาก"};
constexpr int paragraphCount = 600;

std::string readBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeBytes(const std::filesystem::path& path, const std::string& data) {
  std::ofstream output(path, std::ios::binary);
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::vector<std::string> firstLineWords(const Page& page) {
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine&>(*element).getBlock();
    std::vector<std::string> result;
    for (uint16_t i = 0; i < block->wordCount(); ++i) result.emplace_back(block->wordText(i));
    return result;
  }
  return {};
}

class ThaiSectionCacheTest : public testing::TestWithParam<bool> {
 protected:
  std::filesystem::path root;
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;
  ReaderRenderSpec spec;
  std::string html;
  const std::string progress = "spine=0;visibleOffset=321;fraction=0.125";
  const std::string metadata = "host-only publication metadata fixture";

  void SetUp() override {
    // Each discovered test has its own directory, including parallel CTest processes.
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    std::string name = std::string(info->test_suite_name()) + "_" + info->name();
    for (char& c : name)
      if (c == '/' || c == '\\') c = '_';
    root = std::filesystem::temp_directory_path() / ("crosspoint_thai_cache_" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "html");
    html = "<html><body>";
    for (int i = 0; i < paragraphCount; ++i) {
      html += "<p";
      if (i == 0) html += " id=\"start\"";
      html += ">" + sentence + "</p>";
    }
    html += "</body></html>";
    writeBytes(root / "html/0.html", html);
    writeBytes(root / "progress.bin", progress);
    writeBytes(root / "book.bin", metadata);
    epub = std::make_shared<Epub>(root.generic_string());
    spec.viewportWidth = 480;
    spec.viewportHeight = 64;
    spec.embeddedStyle = false;
    spec.thaiLayoutId = thai::layoutId();
  }

  void TearDown() override { std::filesystem::remove_all(root); }

  void completeOrSuspend(Section& section) {
    if (GetParam()) {
      ASSERT_TRUE(section.buildSomeMore(1));
      ASSERT_TRUE(section.isBuilding());
      ASSERT_GT(section.pageCount, 0);
      section.suspendBuild();
      ASSERT_TRUE(section.isPartial());
    } else {
      ASSERT_TRUE(section.buildSomeMore(0));
      ASSERT_TRUE(section.isBuildComplete());
    }
    ASSERT_FALSE(section.isBuilding());
    ASSERT_TRUE(section.loadPage(0));
  }

  void expectPublicationPreserved() {
    EXPECT_EQ(readBytes(root / "html/0.html"), html);
    EXPECT_EQ(readBytes(root / "progress.bin"), progress);
    EXPECT_EQ(readBytes(root / "book.bin"), metadata);
  }

  void expectHealthyReflow() {
    Section healthy(epub, 0, renderer);
    ASSERT_FALSE(healthy.loadSectionFile(spec));
    EXPECT_FALSE(std::filesystem::exists(root / "sections/0.bin"));
    expectPublicationPreserved();
    ASSERT_TRUE(healthy.createSectionFile(spec));
    auto first = healthy.loadPage(0);
    ASSERT_TRUE(first);
    EXPECT_EQ(firstLineWords(*first), dictionaryWords);
    std::string allText;
    uint32_t previousOffset = 0;
    for (uint16_t i = 0; i < healthy.pageCount; ++i) {
      auto page = healthy.loadPage(i);
      ASSERT_TRUE(page);
      EXPECT_GE(page->visibleTextOffset, previousOffset);
      EXPECT_EQ(healthy.getPageForVisibleTextOffset(page->visibleTextOffset), i);
      previousOffset = page->visibleTextOffset;
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const auto* block = static_cast<const PageLine&>(*element).getBlock();
        for (uint16_t word = 0; word < block->wordCount(); ++word) allText += block->wordText(word);
      }
    }
    std::string expected;
    for (int i = 0; i < paragraphCount; ++i) expected += sentence;
    EXPECT_EQ(allText, expected);
    Section next(epub, 0, renderer);
    ASSERT_TRUE(next.loadSectionFile(spec));
    EXPECT_FALSE(next.isPartial());
    EXPECT_EQ(next.pageCount, healthy.pageCount);
    const auto progressPage = next.getPageForVisibleTextOffset(321);
    ASSERT_TRUE(progressPage);
    EXPECT_EQ(progressPage, healthy.getPageForVisibleTextOffset(321));
    expectPublicationPreserved();
  }
};

TEST_P(ThaiSectionCacheTest, SameIdentityReusesCommittedPagesAndOffsets) {
  Section built(epub, 0, renderer);
  ASSERT_TRUE(built.startBuild(spec));
  completeOrSuspend(built);
  ASSERT_FALSE(HasFatalFailure());
  const auto original = readBytes(root / "sections/0.bin");
  Section reopened(epub, 0, renderer);
  ASSERT_TRUE(reopened.loadSectionFile(spec));
  EXPECT_EQ(reopened.isPartial(), GetParam());
  ASSERT_EQ(reopened.pageCount, built.pageCount);
  EXPECT_EQ(reopened.getPageForAnchor("start"), 0);
  uint32_t expectedOffset = 0;
  for (uint16_t i = 0; i < built.pageCount; ++i) {
    auto actual = reopened.loadPage(i);
    ASSERT_TRUE(actual);
    EXPECT_EQ(actual->visibleTextOffset, expectedOffset);
    for (const auto& element : actual->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto* block = static_cast<const PageLine&>(*element).getBlock();
      std::string text;
      for (uint16_t word = 0; word < block->wordCount(); ++word) text += block->wordText(word);
      EXPECT_EQ(text, sentence);
      expectedOffset += static_cast<uint32_t>(text.size() / 3);  // This fixture contains only Thai scalars.
    }
  }
  EXPECT_EQ(readBytes(root / "sections/0.bin"), original);
  expectPublicationPreserved();
}

TEST_P(ThaiSectionCacheTest, ChangedIdentityRejectsOnlyRenderedCache) {
  Section built(epub, 0, renderer);
  ASSERT_TRUE(built.startBuild(spec));
  completeOrSuspend(built);
  ASSERT_FALSE(HasFatalFailure());
  auto changed = spec;
  changed.thaiLayoutId ^= 0x100;
  Section reopened(epub, 0, renderer);
  EXPECT_FALSE(reopened.loadSectionFile(changed));
  EXPECT_FALSE(std::filesystem::exists(root / "sections/0.bin"));
  expectPublicationPreserved();
  ASSERT_TRUE(reopened.createSectionFile(changed));
  Section next(epub, 0, renderer);
  EXPECT_TRUE(next.loadSectionFile(changed));
  EXPECT_TRUE(next.loadPage(0));
}

TEST_P(ThaiSectionCacheTest, ChangedIndentationRejectsOnlyRenderedCache) {
  spec.paragraphIndentSpaces = 2;
  Section built(epub, 0, renderer);
  ASSERT_TRUE(built.startBuild(spec));
  completeOrSuspend(built);
  ASSERT_FALSE(HasFatalFailure());
  Section matching(epub, 0, renderer);
  ASSERT_TRUE(matching.loadSectionFile(spec));
  auto changed = spec;
  changed.paragraphIndentSpaces = 4;
  Section reopened(epub, 0, renderer);
  EXPECT_FALSE(reopened.loadSectionFile(changed));
  EXPECT_FALSE(std::filesystem::exists(root / "sections/0.bin"));
  expectPublicationPreserved();
  ASSERT_TRUE(reopened.createSectionFile(changed));
  Section next(epub, 0, renderer);
  EXPECT_TRUE(next.loadSectionFile(changed));
  EXPECT_TRUE(next.loadPage(0));
}

TEST_P(ThaiSectionCacheTest, PriorFinalAndPartialVersionsAreRejected) {
  Section built(epub, 0, renderer);
  ASSERT_TRUE(built.startBuild(spec));
  completeOrSuspend(built);
  ASSERT_FALSE(HasFatalFailure());
  auto bytes = readBytes(root / "sections/0.bin");
  ASSERT_FALSE(bytes.empty());
  for (const uint8_t version : {48, 49, 50, 51, 52}) {
    SCOPED_TRACE(version);
    bytes[0] = static_cast<char>(GetParam() ? 0xFE - (version - 28) : version);
    writeBytes(root / "sections/0.bin", bytes);
    Section reopened(epub, 0, renderer);
    EXPECT_FALSE(reopened.loadSectionFile(spec));
    EXPECT_FALSE(std::filesystem::exists(root / "sections/0.bin"));
    expectPublicationPreserved();
    ASSERT_TRUE(reopened.createSectionFile(spec));
    Section next(epub, 0, renderer);
    EXPECT_TRUE(next.loadSectionFile(spec));
  }
}

TEST_P(ThaiSectionCacheTest, ThaiJustifyReusesExpandedGeometryAndPageLinks) {
  spec.paragraphAlignment = 5;
  spec.paragraphIndentSpaces = 0;
  spec.viewportWidth = 100;
  html = "<html><body>";
  for (int i = 0; i < paragraphCount; ++i) {
    html += "<p><a href=\"#target\">ประเทศไทยประเทศไทย</a></p>";
  }
  html += "<p id=\"target\">ประเทศไทย</p></body></html>";
  writeBytes(root / "html/0.html", html);
  Section built(epub, 0, renderer);
  ASSERT_TRUE(built.startBuild(spec));
  completeOrSuspend(built);
  ASSERT_FALSE(HasFatalFailure());
  const auto original = readBytes(root / "sections/0.bin");
  Section reopened(epub, 0, renderer);
  ASSERT_TRUE(reopened.loadSectionFile(spec));
  EXPECT_EQ(reopened.isPartial(), GetParam());
  ASSERT_EQ(reopened.pageCount, built.pageCount);
  size_t lineIndex = 0;
  size_t expandedLines = 0;
  size_t finalLines = 0;
  for (uint16_t i = 0; i < built.pageCount; ++i) {
    auto before = built.loadPage(i);
    auto after = reopened.loadPage(i);
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->visibleTextOffset, before->visibleTextOffset);
    EXPECT_EQ(after->visibleTextOffset, lineIndex * 9);
    ASSERT_EQ(after->elements.size(), before->elements.size());
    ASSERT_EQ(after->links.size(), before->links.size());
    size_t linkedLines = 0;
    for (size_t j = 0; j < after->elements.size(); ++j) {
      const auto& element = after->elements[j];
      ASSERT_EQ(element->getTag(), TAG_PageLine);
      EXPECT_EQ(element->xPos, before->elements[j]->xPos);
      EXPECT_EQ(element->yPos, before->elements[j]->yPos);
      const auto* block = static_cast<const PageLine&>(*element).getBlock();
      const auto* old = static_cast<const PageLine&>(*before->elements[j]).getBlock();
      ASSERT_EQ(block->wordCount(), 1);
      ASSERT_EQ(old->wordCount(), block->wordCount());
      EXPECT_STREQ(block->wordText(0), "ประเทศไทย");
      EXPECT_STREQ(old->wordText(0), block->wordText(0));
      EXPECT_EQ(block->wordXpos(0), 0);
      EXPECT_EQ(old->wordXpos(0), block->wordXpos(0));
      const bool linked = lineIndex < static_cast<size_t>(paragraphCount) * 2;
      const int natural = renderer.getTextAdvanceX(spec.fontId, block->wordText(0), block->wordStyle(0));
      const int capacity = renderer.countThaiJustificationGaps(spec.fontId, block->wordText(0), block->wordStyle(0)) *
                           renderer.getThaiJustificationGapLimit(spec.fontId, block->wordText(0), block->wordStyle(0));
      const uint16_t budget = linked && lineIndex % 2 == 0 ? std::min(spec.viewportWidth - natural, capacity) : 0;
      EXPECT_EQ(block->thaiExpansion(0), budget);
      EXPECT_EQ(old->thaiExpansion(0), budget);
      EXPECT_EQ(block->getBlockStyle().alignment, CssTextAlign::ThaiJustify);
      const int extent = renderer.getTextAdvanceX(spec.fontId, block->wordText(0), block->wordStyle(0), 0,
                                                  BidiUtils::BidiBaseDir::AUTO, GfxRenderer::TextMeasureMode::Rendered,
                                                  block->thaiExpansion(0));
      EXPECT_EQ(extent, natural + budget);
      EXPECT_LT(extent, spec.viewportWidth);
      if (budget)
        ++expandedLines;
      else
        ++finalLines;
      if (linked) {
        ASSERT_LT(linkedLines, after->links.size());
        const auto& link = after->links[linkedLines];
        const auto& oldLink = before->links[linkedLines++];
        EXPECT_STREQ(link.href, "#target");
        EXPECT_STREQ(link.href, oldLink.href);
        EXPECT_EQ(link.x, element->xPos);
        EXPECT_EQ(link.width, extent);
        EXPECT_EQ(link.x, oldLink.x);
        EXPECT_EQ(link.y, oldLink.y);
        EXPECT_EQ(link.width, oldLink.width);
        EXPECT_EQ(link.height, oldLink.height);
        EXPECT_GT(link.height, 0);
      }
      ++lineIndex;
    }
    EXPECT_EQ(linkedLines, after->links.size());
  }
  EXPECT_GT(expandedLines, 0);
  EXPECT_GT(finalLines, 0);
  if (!GetParam()) EXPECT_EQ(lineIndex, static_cast<size_t>(paragraphCount) * 2 + 1);
  EXPECT_EQ(readBytes(root / "sections/0.bin"), original);
  expectPublicationPreserved();
}

TEST_P(ThaiSectionCacheTest, SwitchingBetweenJustifyAndThaiJustifyInvalidatesOnlyRenderedCache) {
  for (const uint8_t initial : {0, 5}) {
    SCOPED_TRACE(initial);
    spec.paragraphAlignment = initial;
    Section built(epub, 0, renderer);
    ASSERT_TRUE(built.startBuild(spec));
    completeOrSuspend(built);
    ASSERT_FALSE(HasFatalFailure());
    auto changed = spec;
    changed.paragraphAlignment = initial == 0 ? 5 : 0;
    Section reopened(epub, 0, renderer);
    EXPECT_FALSE(reopened.loadSectionFile(changed));
    EXPECT_FALSE(std::filesystem::exists(root / "sections/0.bin"));
    expectPublicationPreserved();
    ASSERT_TRUE(reopened.createSectionFile(changed));
    const auto rebuilt = readBytes(root / "sections/0.bin");
    Section matching(epub, 0, renderer);
    ASSERT_TRUE(matching.loadSectionFile(changed));
    ASSERT_TRUE(matching.loadPage(0));
    EXPECT_EQ(readBytes(root / "sections/0.bin"), rebuilt);
    expectPublicationPreserved();
  }
}

TEST_P(ThaiSectionCacheTest, TextBlockExpansionRoundTripsAlongsideOptionalFocusArrays) {
  const std::vector<std::string> words = {"ก", "ขค", "reading"};
  const std::vector<int16_t> positions = {0, 11, 33};
  const std::vector<EpdFontFamily::Style> styles(3, EpdFontFamily::REGULAR);
  const std::vector<uint8_t> focus = GetParam() ? std::vector<uint8_t>{0, 0, 3} : std::vector<uint8_t>{};
  const std::vector<uint16_t> suffix = GetParam() ? std::vector<uint16_t>{0, 0, 24} : std::vector<uint16_t>{};
  std::vector<uint16_t> budgets = {0, 2, 0};
  BlockStyle style;
  style.alignment = CssTextAlign::ThaiJustify;
  TextBlock block(words, positions, styles, focus, suffix, style, {}, {}, budgets);
  ASSERT_TRUE(block.valid());
  budgets[1] = 99;  // The arena owns its metadata, not a borrowed layout scratch span.
  EXPECT_EQ(block.thaiExpansion(1), 2);
  const auto path = root / "block.bin";
  HalFile file;
  ASSERT_TRUE(file.open(path.c_str(), "w+b"));
  ASSERT_TRUE(block.serialize(file));
  ASSERT_TRUE(file.seek(0));
  auto replayed = TextBlock::deserialize(file);
  ASSERT_TRUE(replayed);
  ASSERT_TRUE(file.close());
  ASSERT_EQ(replayed->wordCount(), words.size());
  for (uint16_t i = 0; i < replayed->wordCount(); ++i) {
    EXPECT_STREQ(replayed->wordText(i), words[i].c_str());
    EXPECT_EQ(replayed->wordXpos(i), positions[i]);
    EXPECT_EQ(replayed->wordStyle(i), styles[i]);
    EXPECT_EQ(replayed->thaiExpansion(i), i == 1 ? 2 : 0);
    EXPECT_EQ(replayed->focusBoundary(i), GetParam() ? focus[i] : 0);
    EXPECT_EQ(replayed->focusSuffixX(i), GetParam() ? suffix[i] : 0);
  }
  EXPECT_EQ(replayed->wordXpos(1) + renderer.getTextAdvanceX(0, replayed->wordText(1), replayed->wordStyle(1), 0,
                                                             BidiUtils::BidiBaseDir::AUTO,
                                                             GfxRenderer::TextMeasureMode::Rendered,
                                                             replayed->thaiExpansion(1)),
            29);
  EXPECT_EQ(replayed->getBlockStyle().alignment, CssTextAlign::ThaiJustify);
  const auto bytes = readBytes(path);
  ASSERT_GT(bytes.size(), 5);
  EXPECT_EQ(static_cast<uint8_t>(bytes[2]), GetParam() ? 3 : 2);

  // Unknown flags are rejected even when the complete otherwise-valid payload follows.
  for (const uint8_t unknown : {4, 8, 128, 255}) {
    SCOPED_TRACE(unknown);
    auto corrupt = bytes;
    corrupt[2] = static_cast<char>(static_cast<uint8_t>(bytes[2]) | unknown);
    writeBytes(path, corrupt);
    ASSERT_TRUE(file.open(path.c_str(), "rb"));
    EXPECT_FALSE(TextBlock::deserialize(file));
    file.close();
  }
  // Cut at every byte of the optional uint16 array, with/without the preceding focus array.
  const size_t expansionStart = 5 + words.size() * (GetParam() ? 6 : 4);
  for (size_t end = expansionStart; end < expansionStart + words.size() * 2; ++end) {
    SCOPED_TRACE(end);
    writeBytes(path, bytes.substr(0, end));
    ASSERT_TRUE(file.open(path.c_str(), "rb"));
    EXPECT_FALSE(TextBlock::deserialize(file));
    file.close();
  }
}

TEST_P(ThaiSectionCacheTest, PageRoundTripPreservesExpandedWordsAndLinkHitExtent) {
  const std::vector<std::string> words = {"ก", "ขค"};
  const std::vector<int16_t> positions = {0, 11};
  const std::vector<EpdFontFamily::Style> styles(2, EpdFontFamily::REGULAR);
  const std::vector<uint16_t> budgets = {0, 2};
  BlockStyle style;
  style.alignment = CssTextAlign::ThaiJustify;
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(
      std::make_unique<TextBlock>(words, positions, styles, std::vector<uint8_t>{}, std::vector<uint16_t>{}, style,
                                  std::vector<std::string>{}, std::vector<TextBlock::LinkSpan>{}, budgets),
      7, 13));
  ASSERT_TRUE(page.addLink("#target", 18, 13, 18, 16));
  HalFile file;
  ASSERT_TRUE(file.open((root / "page.bin").c_str(), "w+b"));
  ASSERT_TRUE(page.serialize(file));
  ASSERT_TRUE(file.seek(0));
  auto replayed = Page::deserialize(file);
  ASSERT_TRUE(replayed);
  ASSERT_EQ(replayed->elements.size(), 1);
  ASSERT_EQ(replayed->elements[0]->getTag(), TAG_PageLine);
  const auto& line = static_cast<const PageLine&>(*replayed->elements[0]);
  EXPECT_EQ(line.xPos, 7);
  EXPECT_EQ(line.yPos, 13);
  const auto* block = line.getBlock();
  ASSERT_EQ(block->wordCount(), 2);
  EXPECT_STREQ(block->wordText(0), "ก");
  EXPECT_STREQ(block->wordText(1), "ขค");
  EXPECT_EQ(block->wordXpos(0), 0);
  EXPECT_EQ(block->wordXpos(1), 11);
  EXPECT_EQ(block->thaiExpansion(0), 0);
  EXPECT_EQ(block->thaiExpansion(1), 2);
  ASSERT_EQ(replayed->links.size(), 1);
  const auto& link = replayed->links[0];
  EXPECT_STREQ(link.href, "#target");
  EXPECT_EQ(link.x, line.xPos + block->wordXpos(1));
  EXPECT_EQ(link.y, 13);
  EXPECT_EQ(link.height, 16);
  EXPECT_EQ(link.width,
            renderer.getTextAdvanceX(0, block->wordText(1), block->wordStyle(1), 0, BidiUtils::BidiBaseDir::AUTO,
                                     GfxRenderer::TextMeasureMode::Rendered, block->thaiExpansion(1)));
  EXPECT_EQ(link.x + link.width, 36);
}

TEST_P(ThaiSectionCacheTest, TextBlockZeroExpansionIsAbsentAndMismatchedSpansCannotSerialize) {
  const std::vector<std::string> words = {"ก", "ขค"};
  const std::vector<int16_t> positions = {0, 8};
  const std::vector<EpdFontFamily::Style> styles(2, EpdFontFamily::REGULAR);
  const std::vector<uint16_t> zeros = {0, 0};
  TextBlock absent(words, positions, styles, {}, {});
  TextBlock zero(words, positions, styles, {}, {}, BlockStyle(), {}, {}, zeros);
  ASSERT_TRUE(absent.valid());
  ASSERT_TRUE(zero.valid());
  const auto absentPath = root / "absent.bin";
  const auto zeroPath = root / "zero.bin";
  HalFile file;
  ASSERT_TRUE(file.open(absentPath.c_str(), "w+b"));
  ASSERT_TRUE(absent.serialize(file));
  ASSERT_TRUE(file.close());
  ASSERT_TRUE(file.open(zeroPath.c_str(), "w+b"));
  ASSERT_TRUE(zero.serialize(file));
  ASSERT_TRUE(file.seek(0));
  auto replayed = TextBlock::deserialize(file);
  ASSERT_TRUE(replayed);
  ASSERT_TRUE(file.close());
  EXPECT_EQ(readBytes(absentPath), readBytes(zeroPath));
  ASSERT_EQ(replayed->wordCount(), 2);
  for (uint16_t i = 0; i < replayed->wordCount(); ++i) {
    EXPECT_STREQ(replayed->wordText(i), words[i].c_str());
    EXPECT_EQ(replayed->wordXpos(i), positions[i]);
    EXPECT_EQ(replayed->thaiExpansion(i), 0);
    EXPECT_EQ(zero.thaiExpansion(i), 0);
    EXPECT_EQ(absent.thaiExpansion(i), 0);
  }
  for (const auto& mismatch : {std::vector<uint16_t>{0}, std::vector<uint16_t>{2, 0, 0}}) {
    TextBlock invalid(words, positions, styles, {}, {}, BlockStyle(), {}, {}, mismatch);
    EXPECT_FALSE(invalid.valid());
    EXPECT_TRUE(invalid.isEmpty());
    ASSERT_TRUE(file.open((root / "invalid.bin").c_str(), "w+b"));
    EXPECT_FALSE(invalid.serialize(file));
    EXPECT_EQ(file.size(), 0);
    file.close();
  }
}

#if THAI_WORD_BREAKING && THAI_DICTIONARY
TEST_P(ThaiSectionCacheTest, ThaiAllocationFailureRemainsReadableThenHealthyOpenReflows) {
  Section fallback(epub, 0, renderer);
  ASSERT_TRUE(fallback.startBuild(spec));
  SectionThaiCacheTestPeer::parser(fallback).failThaiAllocation = true;
  completeOrSuspend(fallback);
  ASSERT_FALSE(HasFatalFailure());
  auto page = fallback.loadPage(0);
  ASSERT_TRUE(page);
  EXPECT_EQ(firstLineWords(*page), std::vector<std::string>{sentence});
  expectHealthyReflow();
}

TEST_P(ThaiSectionCacheTest, InvalidDictionaryRemainsReadableThenHealthyOpenReflows) {
  const uint8_t corruptData[] = {0, 1, 0x01};
  const uint32_t corruptOffsets[] = {0, 100};  // Out-of-range block end.
  const thai::DictionaryView corrupt{corruptData, sizeof(corruptData), corruptOffsets, 2, 1, 0};
  Section fallback(epub, 0, renderer);
  ASSERT_TRUE(fallback.startBuild(spec));
  SectionThaiCacheTestPeer::parser(fallback).thaiDictionaryOverride = &corrupt;
  completeOrSuspend(fallback);
  ASSERT_FALSE(HasFatalFailure());
  auto page = fallback.loadPage(0);
  ASSERT_TRUE(page);
  std::string text;
  for (const auto& word : firstLineWords(*page)) text += word;
  EXPECT_EQ(text, sentence);
  expectHealthyReflow();
}
#endif

INSTANTIATE_TEST_SUITE_P(FinalAndSuspended, ThaiSectionCacheTest, testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) { return info.param ? "Partial" : "Final"; });
}  // namespace
