#include <CrossPointSettings.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <NativeAllocator.h>
#include <NativeTextEngine.h>
#include <NativeTxtCache.h>
#include <NativeTxtPaginator.h>
#include <Txt.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
class NativeTextTxtCacheTest : public testing::Test {
 protected:
  using Result = NativeTxtCache::Result;
  static constexpr uint8_t ALIGN = CrossPointSettings::LEFT_ALIGN;
  static constexpr uint16_t WIDTH = 230, HEIGHT = 90;
  NativeTextEngine engine;
  HalDisplay display;
  GfxRenderer renderer{display};
  std::filesystem::path root, previousRoot;
  std::unique_ptr<Txt> txt;
  std::string source;

  struct Snapshot {
    uint32_t start = 0, end = 0;
    std::string text;
    std::vector<uint8_t> pixels;
    bool operator==(const Snapshot&) const = default;
  };

  void SetUp() override {
    ASSERT_EQ(Storage.openHandles(), 0u);
    previousRoot = Storage.root();
    root = std::filesystem::temp_directory_path() /
           ("native-txt-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Storage.setRoot(root);
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    SETTINGS.fontFamily = CrossPointSettings::NOTOSANS;
    SETTINGS.fontPointSize = 14;
    SETTINGS.sdFontFamilyName[0] = 0;
    SETTINGS.sdFontIdResolver = nullptr;
    SETTINGS.sdFontResolverCtx = nullptr;
    ASSERT_EQ(engine.initialize(), TextStatus::Ok);
    renderer.begin();
    renderer.setNativeTextEngine(&engine);
    source = "\xef\xbb\xbf";
    for (int i = 0; i < 12; ++i) source += "ภาษาไทย กี่ น้ำ ABC العربية\r\n\n";
    source += "สุดท้าย ไม่มี newline";
    makeBook(source);
  }
  void TearDown() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    renderer.setNativeTextEngine(nullptr);
    engine.shutdown();
    SETTINGS.sdFontFamilyName[0] = 0;
    SETTINGS.sdFontIdResolver = nullptr;
    SETTINGS.sdFontResolverCtx = nullptr;
    EXPECT_EQ(Storage.openHandles(), 0u);
    Storage.endUsbDrive();
    Storage.resetFaults();
    Storage.setRoot(previousRoot);
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  void write(const std::string& path, std::string_view bytes) {
    const auto physical = Storage.resolve(path.c_str());
    std::filesystem::create_directories(physical.parent_path());
    std::ofstream file(physical, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(file.good());
  }
  std::string read(const std::string& path) const {
    std::ifstream file(Storage.resolve(path.c_str()), std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
  void makeBook(std::string_view content) {
    write("/book.txt", content);
    txt = std::make_unique<Txt>("/book.txt", "/.cache");
    ASSERT_TRUE(txt->load());
  }
  std::string directory() const { return txt->getCachePath() + "/native"; }
  std::string indexPath() const { return directory() + "/index.bin"; }
  std::string progressPath() const { return directory() + "/progress.bin"; }
  static void put(std::string& bytes, size_t offset, uint64_t value, size_t count) {
    ASSERT_LE(offset + count, bytes.size());
    for (size_t i = 0; i < count; ++i) bytes[offset + i] = static_cast<char>(value >> (i * 8));
  }
  static uint64_t get(const std::string& bytes, size_t offset, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= uint64_t(uint8_t(bytes[offset + i])) << (i * 8);
    return value;
  }
  static uint64_t hash(std::string_view bytes) {
    uint64_t value = 14695981039346656037ULL;
    for (unsigned char byte : bytes) value = (value ^ byte) * 1099511628211ULL;
    return value;
  }
  void build(NativeTxtCache& cache, uint16_t width = WIDTH, uint16_t height = HEIGHT, uint8_t align = ALIGN) {
    ASSERT_EQ(cache.open(*txt, renderer, width, height, align), Result::Rebuild);
    ASSERT_TRUE(cache.build(renderer));
    EXPECT_EQ(cache.lastStatus(), TextStatus::Ok);
  }
  Snapshot render(const Page& page, uint32_t start, uint32_t end) {
    Snapshot result;
    result.start = start;
    result.end = end;
    for (const auto& element : page.elements) {
      const auto* block = static_cast<const PageLine&>(*element).getBlock();
      EXPECT_TRUE(block->nativeLine());
      if (block->nativeLine()) result.text += std::string(block->nativeLine()->logicalText()) + '\n';
    }
    renderer.clearScreen();
    EXPECT_TRUE(page.warmNativeText(renderer, SETTINGS.getReaderFontId()));
    page.render(renderer, SETTINGS.getReaderFontId(), 0, 0);
    result.pixels.assign(renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize());
    return result;
  }
  Snapshot load(NativeTxtCache& cache, uint32_t number) {
    std::unique_ptr<Page> page;
    uint32_t start = 0, end = 0;
    EXPECT_EQ(cache.loadPage(number, page, start, end), Result::Ready);
    return page ? render(*page, start, end) : Snapshot{};
  }
  void legacy(uint32_t page, const std::vector<uint32_t>& offsets, bool valid = true) {
    std::string progress(4, '\0');
    put(progress, 0, page, 4);
    write(txt->getCachePath() + "/progress.bin", progress);
    std::string index(30 + offsets.size() * 4, '\0');
    put(index, 0, 0x54585449, 4);
    index[4] = 3;
    put(index, 5, source.size(), 4);
    // Deliberately unrelated legacy metrics: migration only uses source bytes.
    put(index, 9, 999, 4);
    put(index, 13, 99, 4);
    put(index, 17, 1, 4);
    put(index, 26, offsets.size(), 4);
    for (size_t i = 0; i < offsets.size(); ++i) put(index, 30 + i * 4, offsets[i], 4);
    if (!valid) index.resize(29);
    write(txt->getCachePath() + "/index.bin", index);
  }
};

TEST_F(NativeTextTxtCacheTest, ColdAndCachedForwardBackwardSkippedPagesPreserveTextPixelsAndByteRanges) {
  std::vector<Snapshot> expected;
  NativeTxtPaginator paginator;
  ASSERT_TRUE(paginator.begin(*txt, renderer, WIDTH, HEIGHT, ALIGN));
  for (;;) {
    std::unique_ptr<Page> page;
    uint32_t start = 0, end = 0;
    const auto result = paginator.nextPage(page, start, end);
    ASSERT_NE(result, NativeTxtPaginator::Result::Error);
    if (result == NativeTxtPaginator::Result::End) break;
    ASSERT_TRUE(page);
    expected.push_back(render(*page, start, end));
  }
  paginator.close();
  ASSERT_GE(expected.size(), 3u);
  NativeTxtCache cache;
  build(cache);
  ASSERT_EQ(cache.pageCount(), expected.size());
  EXPECT_EQ(cache.sourceHash(), hash(source));
  EXPECT_EQ(cache.sourceSize(), source.size());
  uint32_t consumed = 0;
  for (uint32_t i = 0; i < cache.pageCount(); ++i) {
    const auto actual = load(cache, i);
    EXPECT_EQ(actual, expected[i]);
    EXPECT_EQ(actual.start, consumed);
    consumed = actual.end;
  }
  EXPECT_EQ(consumed, source.size());
  cache.close();
  ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Ready);
  for (uint32_t page : {cache.pageCount() - 1, 0u, 2u, 1u}) EXPECT_EQ(load(cache, page), expected[page]);
  cache.close();
  EXPECT_EQ(Storage.openHandles(), 0u);
}

TEST_F(NativeTextTxtCacheTest, ReflowFontViewportAlignmentRestoresContainingSourceByteRatherThanOldPageNumber) {
  NativeTxtCache cache;
  build(cache);
  ASSERT_GE(cache.pageCount(), 3u);
  const auto original = load(cache, cache.pageCount() / 2);
  ASSERT_TRUE(cache.saveProgress(original.start));
  const auto progress = read(progressPath());
  for (int change = 0; change < 4; ++change) {
    SCOPED_TRACE(change);
    SETTINGS.fontPointSize = change == 0 ? 18 : 14;
    const uint16_t width = change == 1 ? 170 : WIDTH;
    const uint16_t height = change == 2 ? 150 : HEIGHT;
    const uint8_t alignment = change == 3 ? CrossPointSettings::RIGHT_ALIGN : ALIGN;
    EXPECT_FALSE(cache.matches(renderer, width, height, alignment));
    ASSERT_EQ(cache.open(*txt, renderer, width, height, alignment), Result::Rebuild);
    ASSERT_TRUE(cache.build(renderer));
    uint32_t restored = UINT32_MAX;
    ASSERT_TRUE(cache.restorePage(restored));
    const auto page = load(cache, restored);
    EXPECT_LE(page.start, original.start);
    EXPECT_GT(page.end, original.start);
    EXPECT_EQ(read(progressPath()), progress);
  }
}

TEST_F(NativeTextTxtCacheTest, ChangedFontContentWithStableFontIdInvalidatesAndRestoresSource) {
  constexpr int FONT = 78291;
  SETTINGS.sdFontFamilyName[0] = 'X';
  SETTINGS.sdFontFamilyName[1] = 0;
  SETTINGS.sdFontIdResolver = [](void*, const char*, uint8_t) { return FONT; };
  const NativeVariation light{0x77676874u, 100}, heavy{0x77676874u, 900};
  const NativeFontFile first{NATIVE_VARIABLE_FONT_PATH, 0, {&light, 1}};
  const NativeFontFile second{NATIVE_VARIABLE_FONT_PATH, 0, {&heavy, 1}};
  ASSERT_EQ(engine.registerCustomFont(FONT, 14, {&first, 1}, false), TextStatus::Ok);
  NativeTxtCache cache;
  build(cache);
  ASSERT_GE(cache.pageCount(), 2u);
  const uint32_t saved = load(cache, 1).start;
  ASSERT_TRUE(cache.saveProgress(saved));
  ASSERT_EQ(engine.registerCustomFont(FONT, 14, {&second, 1}, false), TextStatus::Ok);
  EXPECT_FALSE(cache.matches(renderer, WIDTH, HEIGHT, ALIGN));
  ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Rebuild);
  ASSERT_TRUE(cache.build(renderer));
  uint32_t page = 0;
  ASSERT_TRUE(cache.restorePage(page));
  const auto resumed = load(cache, page);
  EXPECT_LE(resumed.start, saved);
  EXPECT_GT(resumed.end, saved);
}

TEST_F(NativeTextTxtCacheTest, SameLengthSourceEditReplacesCachedTextAndClampsChangedSourceResume) {
  NativeTxtCache cache;
  build(cache);
  const auto at = source.find("ABC");
  ASSERT_NE(at, std::string::npos);
  uint32_t changedPage = 0;
  ASSERT_TRUE(cache.pageForSource(static_cast<uint32_t>(at), changedPage));
  const auto original = load(cache, changedPage);
  const auto saved = load(cache, cache.pageCount() - 1).start;
  ASSERT_TRUE(cache.saveProgress(saved));
  const uint64_t originalHash = cache.sourceHash();
  source.replace(at, 3, "XYZ");
  write("/book.txt", source);
  ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Rebuild);
  ASSERT_TRUE(cache.build(renderer));
  EXPECT_NE(cache.sourceHash(), originalHash);
  ASSERT_TRUE(cache.pageForSource(static_cast<uint32_t>(at), changedPage));
  const auto changed = load(cache, changedPage);
  EXPECT_NE(changed.text, original.text);
  EXPECT_NE(changed.pixels, original.pixels);
  source = "กี่";
  write("/book.txt", source);
  ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Rebuild);
  ASSERT_TRUE(cache.build(renderer));
  uint32_t restored = UINT32_MAX;
  ASSERT_TRUE(cache.restorePage(restored));
  EXPECT_EQ(restored, cache.pageCount() - 1);
  EXPECT_EQ(load(cache, restored).end, source.size());
}

TEST_F(NativeTextTxtCacheTest, CorruptHeadersLutRangesAndTruncationAreRebuildableWithoutTouchingProgress) {
  NativeTxtCache cache;
  build(cache);
  ASSERT_TRUE(cache.saveProgress(0));
  cache.close();
  const auto complete = read(indexPath()), progress = read(progressPath());
  const size_t lut = static_cast<size_t>(get(complete, 38, 4));
  ASSERT_GT(get(complete, 34, 4), 1u);
  for (int corruption = 0; corruption < 9; ++corruption) {
    SCOPED_TRACE(corruption);
    auto bytes = complete;
    switch (corruption) {
      case 0:
        bytes.resize(17);
        break;
      case 1:
        bytes[4] = 0;
        break;
      case 2:
        put(bytes, 34, UINT32_MAX, 4);
        break;
      case 3:
        put(bytes, 38, 41, 4);
        break;
      case 4:
        put(bytes, lut + 16, 0, 4);
        break;
      case 5:
        put(bytes, lut + 8, 0, 4);
        break;
      case 6:
        put(bytes, lut + 12, UINT32_MAX, 4);
        break;
      case 7:
        bytes.pop_back();
        break;
      case 8:
        bytes[4] = 1;  // Old native TextBlock payload lacks spacing fields.
        break;
    }
    write(indexPath(), bytes);
    EXPECT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Rebuild);
    EXPECT_EQ(read(progressPath()), progress);
    cache.close();
  }
  write(indexPath(), complete);
  EXPECT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Ready);
}

TEST_F(NativeTextTxtCacheTest, MalformedPageCannotReadIntoNextPageAndReturnsItsValidResumeRange) {
  NativeTxtCache cache;
  build(cache);
  cache.close();
  const auto complete = read(indexPath());
  const size_t lut = static_cast<size_t>(get(complete, 38, 4));
  for (int corruption = 0; corruption < 3; ++corruption) {
    SCOPED_TRACE(corruption);
    auto bytes = complete;
    if (corruption == 0) bytes.at(44) = static_cast<char>(0xff);  // Unknown Page element tag.
    if (corruption == 1) put(bytes, 42, 1024, 2);                 // Count exceeds this page's body.
    if (corruption == 2) {
      // Reassign one byte to the following page while keeping LUT coverage valid.
      const uint32_t firstSize = static_cast<uint32_t>(get(bytes, lut + 12, 4));
      put(bytes, lut + 12, firstSize - 1, 4);
      put(bytes, lut + 16 + 8, get(bytes, lut + 16 + 8, 4) - 1, 4);
      put(bytes, lut + 16 + 12, get(bytes, lut + 16 + 12, 4) + 1, 4);
    }
    write(indexPath(), bytes);
    ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Ready);
    std::unique_ptr<Page> page;
    uint32_t start = UINT32_MAX, end = 0;
    EXPECT_EQ(cache.loadPage(0, page, start, end), Result::Rebuild);
    EXPECT_FALSE(page);
    EXPECT_EQ(start, 0u);
    EXPECT_EQ(end, get(complete, lut + 4, 4));
    EXPECT_EQ(cache.lastStatus(), TextStatus::InvalidText);
    cache.close();
  }
}

TEST_F(NativeTextTxtCacheTest, BuildWriteReadRenameAndAllocationFailuresKeepPreviousCompleteIndexAndProgress) {
  NativeTxtCache cache;
  build(cache);
  ASSERT_TRUE(cache.saveProgress(0));
  cache.close();
  const auto index = read(indexPath()), progress = read(progressPath());
  SETTINGS.fontPointSize = 18;
  for (int failure = 0; failure < 5; ++failure) {
    SCOPED_TRACE(failure);
    ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Rebuild);
    auto& faults = Storage.faults();
    if (failure == 0) {
      faults.writePath = "index.bin.tmp";
      faults.writeBytes = 60;
    }
    if (failure == 1) {
      faults.readPath = "/book.txt";
      faults.readBytes = 11;
    }
    if (failure == 2) {
      faults.renamePath = "index.bin.tmp";
      faults.renameFailures = 1;
    }
    if (failure == 3) native_text::failAllocationsAfter(0);
    if (failure == 4) {
      faults.readPath = "index.bin.tmp";
      faults.readBytes = 3;
    }
    EXPECT_FALSE(cache.build(renderer));
    EXPECT_EQ(cache.lastStatus(), failure == 3 ? TextStatus::OutOfMemory : TextStatus::StorageError);
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    Storage.resetFaults();
    EXPECT_EQ(read(indexPath()), index);
    EXPECT_EQ(read(progressPath()), progress);
    EXPECT_FALSE(Storage.exists((directory() + "/index.bin.tmp").c_str()));
    EXPECT_FALSE(Storage.exists((directory() + "/index.lut.tmp").c_str()));
    cache.close();
    EXPECT_EQ(Storage.openHandles(), 0u);
  }
  SETTINGS.fontPointSize = 14;
  ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Ready);
  EXPECT_EQ(load(cache, 0).start, 0u);
}

TEST_F(NativeTextTxtCacheTest, CachedSourceAndPageIoAndAllocationFailuresAreErrorsNotEmptyOrCorruptBooks) {
  NativeTxtCache cache;
  build(cache);
  ASSERT_TRUE(cache.saveProgress(0));
  const auto progress = read(progressPath());
  cache.close();
  Storage.faults().readPath = "/book.txt";
  Storage.faults().readBytes = 0;
  EXPECT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Error);
  EXPECT_EQ(cache.lastStatus(), TextStatus::StorageError);
  Storage.resetFaults();
  ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Ready);
  Storage.faults().readPath = "index.bin";
  Storage.faults().readBytes = 17;  // A valid LUT record, then a failed body read.
  std::unique_ptr<Page> page;
  uint32_t start = 0, end = 0;
  EXPECT_EQ(cache.loadPage(0, page, start, end), Result::Error);
  EXPECT_FALSE(page);
  EXPECT_EQ(cache.lastStatus(), TextStatus::StorageError);
  Storage.resetFaults();
  native_text::failAllocationsAfter(0);
  EXPECT_EQ(cache.loadPage(0, page, start, end), Result::Error);
  EXPECT_FALSE(page);
  EXPECT_EQ(cache.lastStatus(), TextStatus::OutOfMemory);
  native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
  EXPECT_EQ(read(progressPath()), progress);
}

TEST_F(NativeTextTxtCacheTest, ProgressUnchangedGuardAndFailedWritesKeepOldPositionUntilSuccessfulRetry) {
  NativeTxtCache cache;
  build(cache);
  ASSERT_GE(cache.pageCount(), 2u);
  const uint32_t nextByte = load(cache, 1).start;
  ASSERT_TRUE(cache.saveProgress(0));
  const auto previous = read(progressPath());
  ASSERT_EQ(previous.size(), 21u);
  EXPECT_EQ(get(previous, 0, 4), 0x52505854u);
  for (int failure = 0; failure < 2; ++failure) {
    SCOPED_TRACE(failure);
    auto& faults = Storage.faults();
    if (failure == 0) {
      faults.writePath = "progress.bin.tmp";
      faults.writeBytes = 8;
    } else {
      faults.renamePath = "progress.bin.tmp";
      faults.renameFailures = 1;
    }
    EXPECT_TRUE(cache.saveProgress(0));  // Must not enter the failing storage path.
    EXPECT_FALSE(cache.saveProgress(nextByte));
    EXPECT_EQ(read(progressPath()), previous);
    Storage.resetFaults();
    uint32_t restored = UINT32_MAX;
    ASSERT_TRUE(cache.restorePage(restored));
    EXPECT_EQ(restored, 0u);
  }
  ASSERT_TRUE(cache.saveProgress(nextByte));
  cache.close();
  ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Ready);
  uint32_t restored = UINT32_MAX;
  ASSERT_TRUE(cache.restorePage(restored));
  EXPECT_EQ(restored, 1u);
}

TEST_F(NativeTextTxtCacheTest, InterruptedPublicationRecoversOwnedIndexAndProgressBackups) {
  NativeTxtCache cache;
  build(cache);
  ASSERT_GE(cache.pageCount(), 2u);
  ASSERT_TRUE(cache.saveProgress(load(cache, 1).start));
  cache.close();
  const auto index = read(indexPath()), progress = read(progressPath());
  ASSERT_TRUE(Storage.rename(indexPath().c_str(), (indexPath() + ".bak").c_str()));
  ASSERT_TRUE(Storage.rename(progressPath().c_str(), (progressPath() + ".bak").c_str()));
  write(indexPath() + ".tmp", "incomplete");
  write(progressPath() + ".tmp", "incomplete");
  ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Ready);
  EXPECT_EQ(read(indexPath()), index);
  EXPECT_EQ(read(progressPath()), progress);
  uint32_t restored = 0;
  ASSERT_TRUE(cache.restorePage(restored));
  EXPECT_EQ(restored, 1u);
}

TEST_F(NativeTextTxtCacheTest, EmptyAndBomOnlyHaveZeroPagesWhilePhysicalBlankLinesRemainPages) {
  NativeTxtCache cache;
  for (const std::string& text : {std::string(), std::string("\xef\xbb\xbf"), std::string("\n\r\n\n")}) {
    cache.close();
    makeBook(text);
    const auto opened = cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN);
    ASSERT_EQ(opened, Result::Rebuild);
    ASSERT_TRUE(cache.build(renderer));
    EXPECT_EQ(cache.sourceHash(), hash(text));
    if (text.empty() || text == "\xef\xbb\xbf")
      EXPECT_EQ(cache.pageCount(), 0u);
    else {
      EXPECT_GT(cache.pageCount(), 0u);
      uint32_t end = 0;
      for (uint32_t i = 0; i < cache.pageCount(); ++i) {
        const auto page = load(cache, i);
        EXPECT_EQ(page.start, end);
        end = page.end;
        EXPECT_EQ(page.text.find_first_not_of('\n'), std::string::npos);
      }
      EXPECT_EQ(end, text.size());
    }
    cache.close();
    EXPECT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Ready);
  }
}

TEST_F(NativeTextTxtCacheTest, ShortPositiveSourceReadsSucceedButMalformedUtf8NeverPublishes) {
  NativeTxtCache cache;
  Storage.faults().maximumRead = 2;
  Storage.faults().readPath = "/book.txt";
  build(cache);
  EXPECT_EQ(cache.sourceHash(), hash(source));
  Storage.resetFaults();
  ASSERT_TRUE(cache.saveProgress(0));
  const auto index = read(indexPath()), progress = read(progressPath());
  cache.close();
  write("/book.txt", std::string("valid ") + char(0xe0) + char(0x80));
  ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Rebuild);
  EXPECT_FALSE(cache.build(renderer));
  EXPECT_EQ(cache.lastStatus(), TextStatus::InvalidText);
  EXPECT_EQ(read(indexPath()), index);
  EXPECT_EQ(read(progressPath()), progress);
}

TEST_F(NativeTextTxtCacheTest, LegacyV3MigrationUsesOriginalByteOffsetsWithoutMatchingOldMetrics) {
  NativeTxtCache cache;
  const uint32_t wanted = static_cast<uint32_t>(source.find("ABC", source.size() / 2));
  ASSERT_LT(wanted, source.size());
  legacy(1, {0, wanted});
  const auto oldProgress = read(txt->getCachePath() + "/progress.bin");
  const auto oldIndex = read(txt->getCachePath() + "/index.bin");
  build(cache);
  uint32_t restored = 0;
  ASSERT_TRUE(cache.restorePage(restored));
  const auto page = load(cache, restored);
  EXPECT_LE(page.start, wanted);
  EXPECT_GT(page.end, wanted);
  ASSERT_TRUE(cache.saveProgress(page.start));
  EXPECT_EQ(read(txt->getCachePath() + "/progress.bin"), oldProgress);
  EXPECT_EQ(read(txt->getCachePath() + "/index.bin"), oldIndex);
  cache.close();
  ASSERT_EQ(cache.open(*txt, renderer, WIDTH, HEIGHT, ALIGN), Result::Ready);
  uint32_t native = 0;
  ASSERT_TRUE(cache.restorePage(native));
  EXPECT_EQ(native, restored);
}

TEST_F(NativeTextTxtCacheTest, MissingInvalidLegacyIndexFallsBackOnceThenNativeByteProgressSurvivesReflow) {
  NativeTxtCache cache;
  for (int invalid = 0; invalid < 3; ++invalid) {
    SCOPED_TRACE(invalid);
    cache.close();
    Storage.remove(progressPath().c_str());
    Storage.remove(indexPath().c_str());
    legacy(2, {0, static_cast<uint32_t>(source.size() + 1)}, invalid != 1);
    if (invalid == 0) Storage.remove((txt->getCachePath() + "/index.bin").c_str());
    build(cache);
    uint32_t restored = 0;
    ASSERT_TRUE(cache.restorePage(restored));
    EXPECT_EQ(restored, std::min(2u, cache.pageCount() - 1));
    const auto page = load(cache, restored);
    ASSERT_TRUE(cache.saveProgress(page.start));
    ASSERT_EQ(cache.open(*txt, renderer, 170, 130, ALIGN), Result::Rebuild);
    ASSERT_TRUE(cache.build(renderer));
    ASSERT_TRUE(cache.restorePage(restored));
    const auto reflow = load(cache, restored);
    EXPECT_LE(reflow.start, page.start);
    EXPECT_GT(reflow.end, page.start);
  }
}
}  // namespace
