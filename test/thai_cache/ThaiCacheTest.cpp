#include <gtest/gtest.h>

#include <Epub/Section.h>
#include <Epub/Page.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <GfxRenderer.h>
#include <ThaiLayoutId.h>

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
    for (char& c : name) if (c == '/' || c == '\\') c = '_';
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

TEST_P(ThaiSectionCacheTest, PriorFinalAndPartialVersionsAreRejected) {
  Section built(epub, 0, renderer);
  ASSERT_TRUE(built.startBuild(spec));
  completeOrSuspend(built);
  ASSERT_FALSE(HasFatalFailure());
  auto bytes = readBytes(root / "sections/0.bin");
  ASSERT_FALSE(bytes.empty());
  bytes[0] = static_cast<char>(GetParam() ? 0xFE - (48 - 28) : 48);
  writeBytes(root / "sections/0.bin", bytes);
  Section reopened(epub, 0, renderer);
  EXPECT_FALSE(reopened.loadSectionFile(spec));
  EXPECT_FALSE(std::filesystem::exists(root / "sections/0.bin"));
  expectPublicationPreserved();
  ASSERT_TRUE(reopened.createSectionFile(spec));
  Section next(epub, 0, renderer);
  EXPECT_TRUE(next.loadSectionFile(spec));
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
