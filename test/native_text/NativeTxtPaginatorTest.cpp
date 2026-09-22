#include <CrossPointSettings.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Memory.h>
#include <NativeAllocator.h>
#include <NativeParagraphLayout.h>
#include <NativeTextEngine.h>
#include <NativeTxtPaginator.h>
#include <NativeUtf8.h>
#include <Txt.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr size_t UNLIMITED = std::numeric_limits<size_t>::max();
uint64_t fnv(std::string_view bytes) {
  uint64_t value = 14695981039346656037ULL;
  for (const unsigned char byte : bytes) value = (value ^ byte) * 1099511628211ULL;
  return value;
}
std::string drawn(std::string_view bytes, bool first = true) {
  if (first && bytes.starts_with("\xef\xbb\xbf")) bytes.remove_prefix(3);
  std::string result;
  for (const char byte : bytes)
    if (byte != '\r' && byte != '\n') result += byte;
  return result;
}

class NativeTextTxtPaginatorTest : public testing::Test {
 protected:
  struct Book {
    std::vector<std::pair<uint32_t, uint32_t>> pages;
    std::vector<std::string> lines;
    std::vector<std::vector<std::string>> selections;
    std::vector<uint64_t> linePixels, pagePixels;
    std::vector<int> heights, levels;
    uint64_t hash = 0;
    uint32_t size = 0;
    bool empty = false;
    NativeTxtPaginator::Result result = NativeTxtPaginator::Result::Error;
    std::string text() const {
      std::string text;
      for (const auto& line : lines) text += line;
      return text;
    }
  };
  NativeTextEngine engine;
  HalDisplay display;
  GfxRenderer renderer{display};
  std::filesystem::path root, previousRoot;
  size_t initialNativeBytes = 0;
  int font = 0;

  static void SetUpTestSuite() {
    NativeTextEngine warm;
    ASSERT_EQ(warm.initialize(), TextStatus::Ok);
    warm.shutdown();
  }

  void SetUp() override {
    native_text::failAllocationsAfter(UNLIMITED);
    initialNativeBytes = native_text::allocationStats().used;
    previousRoot = Storage.root();
    root = std::filesystem::temp_directory_path() /
           ("native-txt-paginator-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Storage.setRoot(root);
    SETTINGS.fontFamily = CrossPointSettings::NOTOSANS;
    SETTINGS.fontPointSize = 14;
    SETTINGS.sdFontFamilyName[0] = 0;
    SETTINGS.sdFontIdResolver = nullptr;
    SETTINGS.sdFontResolverCtx = nullptr;
    font = SETTINGS.getReaderFontId();
    ASSERT_EQ(engine.initialize(), TextStatus::Ok);
    renderer.begin();
    renderer.setNativeTextEngine(&engine);
  }
  void TearDown() override {
    native_text::failAllocationsAfter(UNLIMITED);
    Storage.resetFaults();
    renderer.setNativeTextEngine(nullptr);
    engine.shutdown();
    EXPECT_EQ(Storage.openHandles(), 0u);
    EXPECT_EQ(native_text::allocationStats().used, initialNativeBytes);
    EXPECT_LE(native_text::allocationStats().peak, native_text::MEMORY_LIMIT);
    Storage.setRoot(previousRoot);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error) << error.message();
  }
  void write(std::string_view bytes) {
    std::ofstream output(root / "book.txt", std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output);
  }
  uint64_t pixels() const {
    return fnv({reinterpret_cast<const char*>(renderer.getFrameBuffer()), renderer.getBufferSize()});
  }
  Book paginate(std::string_view bytes, uint16_t width = 220, uint16_t height = 180,
                uint8_t alignment = CrossPointSettings::LEFT_ALIGN, size_t maximumRead = UNLIMITED) {
    write(bytes);
    Storage.resetFaults();
    Storage.faults().maximumRead = maximumRead;
    renderer.clearTextStatus();
    Txt txt("/book.txt", "/cache");
    NativeTxtPaginator paginator;
    Book book;
    if (!paginator.begin(txt, renderer, width, height, alignment)) {
      ADD_FAILURE() << "begin status " << static_cast<int>(paginator.lastStatus());
      return book;
    }
    std::unique_ptr<Page> page;
    uint32_t start = 0, end = 0, previous = 0;
    // Every nonempty page consumes at least one original byte, even if the
    // content viewport is smaller than a single glyph or line.
    for (size_t attempts = 0; attempts <= bytes.size() + 1; ++attempts) {
      book.result = paginator.nextPage(page, start, end);
      if (book.result != NativeTxtPaginator::Result::PageReady) break;
      if (!page || page->elements.empty()) {
        ADD_FAILURE() << "empty ready page";
        break;
      }
      EXPECT_EQ(start, previous);
      EXPECT_GT(end, start);
      EXPECT_LE(end, bytes.size());
      previous = end;
      book.pages.emplace_back(start, end);
      std::string text;
      int y = 0;
      for (const auto& element : page->elements) {
        EXPECT_EQ(element->getTag(), TAG_PageLine);
        const auto& line = static_cast<const PageLine&>(*element);
        const auto* block = line.getBlock();
        if (!block || !block->nativeLine()) {
          ADD_FAILURE() << "non-native TXT line";
          continue;
        }
        EXPECT_EQ(line.xPos, 0);
        EXPECT_EQ(line.yPos, y);
        const int lineHeight = block->layoutHeight(renderer, font, 1.0f);
        EXPECT_GT(lineHeight, 0);
        EXPECT_TRUE(y == 0 || y + lineHeight <= height);
        y += lineHeight;
        const auto logical = block->nativeLine()->logicalText();
        text += logical;
        book.lines.emplace_back(logical);
        book.heights.push_back(lineHeight);
        book.levels.push_back(block->nativeLine()->paragraphLevel);
        book.selections.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          book.selections.back().emplace_back(block->wordText(i));
          EXPECT_GE(block->wordTop(i), 0);
          EXPECT_LE(block->wordTop(i) + block->wordHeight(i), lineHeight);
          EXPECT_EQ(block->wordStyle(i), EpdFontFamily::REGULAR);
        }
        EXPECT_TRUE(block->nativeLine()->gaps.empty());
        EXPECT_EQ(block->nativeLine()->syntheticSuffixCp, 0u);
        renderer.setClipRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
        renderer.clearScreen();
        block->render(renderer, font, 0, 0);
        EXPECT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
        book.linePixels.push_back(pixels());
      }
      if (end <= bytes.size()) EXPECT_EQ(text, drawn(bytes.substr(start, end - start), start == 0));
      renderer.setClipRect(0, 0, width, height);
      renderer.clearScreen();
      const auto beforeWarm = pixels();
      EXPECT_TRUE(page->warmNativeText(renderer, font));
      EXPECT_EQ(pixels(), beforeWarm);
      page->render(renderer, font, 0, 0);
      EXPECT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
      book.pagePixels.push_back(pixels());
    }
    EXPECT_EQ(book.result, NativeTxtPaginator::Result::End) << static_cast<int>(paginator.lastStatus());
    EXPECT_FALSE(page);
    book.hash = paginator.sourceHash();
    book.size = paginator.sourceSize();
    book.empty = paginator.emptySource();
    EXPECT_EQ(book.hash, fnv(bytes));
    EXPECT_EQ(book.size, bytes.size());
    if (!book.empty) EXPECT_EQ(previous, bytes.size());
    // End is stable and must not turn a successful close into a storage error.
    EXPECT_EQ(paginator.nextPage(page, start, end), NativeTxtPaginator::Result::End);
    paginator.close();
    EXPECT_EQ(Storage.openHandles(), 0u);
    Storage.resetFaults();
    return book;
  }
  std::vector<uint64_t> reference(std::string_view text, uint16_t width, NativeAlignment alignment,
                                  std::vector<std::string>& lines) {
    NativeParagraphLayout layout(engine);
    NativeParagraphView view;
    view.text = text;
    view.sourceUnit = NativeSourceUnit::Byte;
    NativeLayoutOptions options;
    options.fontId = font;
    options.width = width;
    options.alignment = alignment;
    std::vector<NativeLayoutEmission> output;
    const auto emit = [](void* context, NativeLayoutEmission&& emission) {
      static_cast<std::vector<NativeLayoutEmission>*>(context)->push_back(std::move(emission));
      return TextStatus::Ok;
    };
    size_t consumed = 0;
    int8_t level = -1;
    EXPECT_EQ(layout.layout(view, options, emit, &output, consumed, level), TextStatus::Ok);
    EXPECT_EQ(consumed, text.size());
    std::vector<uint64_t> image;
    for (auto& emission : output) {
      lines.emplace_back(emission.line.logicalText());
      auto block = makeUniqueNoThrow<TextBlock>(std::move(emission.line), BlockStyle{});
      EXPECT_TRUE(block);
      if (!block) continue;
      renderer.setClipRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
      renderer.clearScreen();
      block->render(renderer, font, 0, 0);
      image.push_back(pixels());
    }
    return image;
  }
};

TEST_F(NativeTextTxtPaginatorTest, BomCrlfBlankLinesSpacesAndFinalUnterminatedTextCoverSourceExactly) {
  const std::string bytes =
      "\xef\xbb\xbf"
      "  กิ กี่ กึ กุ กู้ น้ำ กำ ปี่ ญู ฐู เก่ง  \r\n\r\n\n"
      "ภาษาไทย EPUB 123 العربية\n  last line  ";
  const auto book = paginate(bytes, 180, 79);
  EXPECT_EQ(book.text(), drawn(bytes));
  EXPECT_EQ(std::count(book.lines.begin(), book.lines.end(), ""), 2);
  EXPECT_FALSE(book.empty);
  const auto shortReads = paginate(bytes, 180, 79, CrossPointSettings::LEFT_ALIGN, 1);
  EXPECT_EQ(shortReads.pages, book.pages);
  EXPECT_EQ(shortReads.lines, book.lines);
  EXPECT_EQ(shortReads.pagePixels, book.pagePixels);
  EXPECT_EQ(shortReads.selections, book.selections);
}

TEST_F(NativeTextTxtPaginatorTest, EmptyAndBomOnlyHaveNoPagesButBlankPhysicalLinesRemain) {
  for (const std::string bytes : {std::string{}, std::string("\xef\xbb\xbf")}) {
    const auto book = paginate(bytes, 180, 80, CrossPointSettings::LEFT_ALIGN, 1);
    EXPECT_TRUE(book.empty);
    EXPECT_TRUE(book.pages.empty());
  }
  renderer.setClipRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
  renderer.clearScreen();
  const uint64_t white = pixels();
  const auto blanks = paginate("\r\n\n\r\n", 180, 1);
  EXPECT_FALSE(blanks.empty);
  EXPECT_EQ(blanks.pages, (std::vector<std::pair<uint32_t, uint32_t>>{{0, 2}, {2, 3}, {3, 5}}));
  EXPECT_EQ(blanks.lines, (std::vector<std::string>{"", "", ""}));
  for (const auto image : blanks.pagePixels) EXPECT_EQ(image, white);
}

TEST_F(NativeTextTxtPaginatorTest, LongParagraphKeepsThaiWordsMarksAndBidiAcrossReadWindowsAndPages) {
  // The base ก occupies bytes 8189..8191, its tone mark starts at 8192.
  // A later 8192-byte boundary cuts the multibyte Thai source itself. The
  // physical paragraph also exceeds the fitter's 4096-scalar source window.
  std::string bytes = "العربية ";
  while (bytes.size() + std::string("ภาษาไทย").size() <= 8189) bytes += "ภาษาไทย";
  bytes.append(8189 - bytes.size(), ' ');
  bytes += "กี่ประเทศไทย";
  for (int i = 0; i < 550; ++i) bytes += "ภาษาไทย";
  bytes += " END";
  ASSERT_GT(bytes.size(), 16384u);
  const auto full = paginate(bytes, 240, 160);
  const auto shortReads = paginate(bytes, 240, 160, CrossPointSettings::LEFT_ALIGN, 7);
  EXPECT_EQ(full.text(), bytes);
  EXPECT_EQ(shortReads.pages, full.pages);
  EXPECT_EQ(shortReads.lines, full.lines);
  EXPECT_EQ(shortReads.selections, full.selections);
  EXPECT_EQ(shortReads.pagePixels, full.pagePixels);
  for (const int level : full.levels) EXPECT_EQ(level, 1);
  for (const auto& line : full.lines) {
    size_t offset = 0;
    uint32_t first = 0;
    ASSERT_TRUE(native_text::nextUtf8(line, offset, first));
    EXPECT_FALSE(first >= 0x0e48 && first <= 0x0e4b);
  }
  // Page height is not a paragraph or dictionary boundary.
  const auto oneLinePages = paginate(bytes, 240, 1);
  EXPECT_EQ(oneLinePages.lines, full.lines);
  EXPECT_EQ(oneLinePages.linePixels, full.linePixels);
  EXPECT_EQ(oneLinePages.selections, full.selections);
}

TEST_F(NativeTextTxtPaginatorTest, SharedParagraphFitterProducesIdenticalTextAndPixelsWithoutTxtHyphens) {
  const std::string text = "ภาษาไทยประเทศไทย กี่ น้ำ EPUB 123 العربية עברית  e\xcc\x81 last";
  for (const auto alignment :
       {CrossPointSettings::LEFT_ALIGN, CrossPointSettings::CENTER_ALIGN, CrossPointSettings::RIGHT_ALIGN,
        CrossPointSettings::JUSTIFIED, CrossPointSettings::BOOK_STYLE}) {
    const auto nativeAlignment = alignment == CrossPointSettings::CENTER_ALIGN  ? NativeAlignment::Center
                                 : alignment == CrossPointSettings::RIGHT_ALIGN ? NativeAlignment::Right
                                                                                : NativeAlignment::Start;
    std::vector<std::string> referenceLines;
    const auto referencePixels = reference(text, 190, nativeAlignment, referenceLines);
    const auto book = paginate(text, 190, 1, alignment);
    EXPECT_EQ(book.lines, referenceLines);
    EXPECT_EQ(book.linePixels, referencePixels);
  }
}

TEST_F(NativeTextTxtPaginatorTest, OversizedViewportLinesAreConsumedOnceAndActualHeightsPackPages) {
  const std::string text = "M\nกี่\n!";
  const auto tiny = paginate(text, 1, 1);
  EXPECT_EQ(tiny.text(), "Mกี่!");
  EXPECT_EQ(tiny.pages.size(), 3u);
  EXPECT_EQ(tiny.lines, (std::vector<std::string>{"M", "กี่", "!"}));
  const int firstHeight = tiny.heights[0];
  const int thaiHeight = tiny.heights[1];
  const auto fit = paginate(text, 180, static_cast<uint16_t>(firstHeight + thaiHeight));
  ASSERT_EQ(fit.pages.size(), 2u);
  EXPECT_EQ(fit.pages.front(), std::make_pair(uint32_t{0}, uint32_t{12}));
  const auto noFit = paginate(text, 180, static_cast<uint16_t>(firstHeight + thaiHeight - 1));
  EXPECT_EQ(noFit.pages.front(), std::make_pair(uint32_t{0}, uint32_t{2}));
}

TEST_F(NativeTextTxtPaginatorTest, MalformedUtf8DiscardsPartialPageAndNeverReportsEnd) {
  for (const std::string bad :
       {std::string("\xe0\x80\x80", 3), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
        std::string("\xe0\xb8", 2), std::string("\x80", 1), std::string("\xe0X", 2), std::string("\0", 1)}) {
    write("valid prefix\n" + bad);
    Storage.faults().maximumRead = 1;
    Txt txt("/book.txt", "/cache");
    NativeTxtPaginator paginator;
    ASSERT_TRUE(paginator.begin(txt, renderer, 400, 400, CrossPointSettings::LEFT_ALIGN));
    std::unique_ptr<Page> page;
    uint32_t start = 0, end = 0;
    EXPECT_EQ(paginator.nextPage(page, start, end), NativeTxtPaginator::Result::Error);
    EXPECT_EQ(paginator.lastStatus(), TextStatus::InvalidText);
    EXPECT_FALSE(page);
    EXPECT_EQ(Storage.openHandles(), 0u);
    Storage.resetFaults();
  }
}

TEST_F(NativeTextTxtPaginatorTest, SourceReadFailureAndTruncationAreNotSuccessfulEof) {
  const std::string bytes = "กี่ประเทศไทย English source continues beyond injected short read";
  for (const bool truncate : {false, true}) {
    write(bytes);
    Txt txt("/book.txt", "/cache");
    NativeTxtPaginator paginator;
    ASSERT_TRUE(paginator.begin(txt, renderer, 220, 160, CrossPointSettings::LEFT_ALIGN));
    if (truncate)
      std::filesystem::resize_file(root / "book.txt", 4);
    else {
      Storage.faults().readPath = "book.txt";
      Storage.faults().readBytes = 11;
    }
    std::unique_ptr<Page> page;
    uint32_t start = 0, end = 0;
    EXPECT_EQ(paginator.nextPage(page, start, end), NativeTxtPaginator::Result::Error);
    EXPECT_EQ(paginator.lastStatus(), TextStatus::StorageError);
    EXPECT_FALSE(page);
    EXPECT_EQ(Storage.openHandles(), 0u);
    Storage.resetFaults();
  }
}

TEST_F(NativeTextTxtPaginatorTest, AllocationFailureClosesSourceAndReleasesPendingNativeStorage) {
  write("ภาษาไทยกี่ประเทศไทย\nsecond physical line");
  Txt txt("/book.txt", "/cache");
  NativeTxtPaginator paginator;
  native_text::failAllocationsAfter(0);
  EXPECT_FALSE(paginator.begin(txt, renderer, 180, 160, CrossPointSettings::LEFT_ALIGN));
  EXPECT_EQ(paginator.lastStatus(), TextStatus::OutOfMemory);
  native_text::failAllocationsAfter(UNLIMITED);
  ASSERT_TRUE(paginator.begin(txt, renderer, 180, 160, CrossPointSettings::LEFT_ALIGN));
  native_text::failAllocationsAfter(0);
  std::unique_ptr<Page> page;
  uint32_t start = 0, end = 0;
  EXPECT_EQ(paginator.nextPage(page, start, end), NativeTxtPaginator::Result::Error);
  EXPECT_EQ(paginator.lastStatus(), TextStatus::OutOfMemory);
  EXPECT_FALSE(page);
  EXPECT_EQ(Storage.openHandles(), 0u);
  native_text::failAllocationsAfter(UNLIMITED);
  paginator.close();
  EXPECT_EQ(paginate("กี่ recovered").text(), "กี่ recovered");
}

TEST_F(NativeTextTxtPaginatorTest, HashScanUsesRawBytesAndRejectsReadFailureBeforePublishingHash) {
  const std::string bytes =
      "\xef\xbb\xbf"
      "กี่\r\n\nlast";
  write(bytes);
  Txt txt("/book.txt", "/cache");
  NativeTxtPaginator paginator;
  Storage.faults().maximumRead = 1;
  uint64_t hash = 0;
  ASSERT_EQ(paginator.hashSource(txt, hash), TextStatus::Ok);
  EXPECT_EQ(hash, fnv(bytes));
  EXPECT_EQ(paginator.sourceSize(), bytes.size());
  EXPECT_FALSE(paginator.emptySource());
  EXPECT_EQ(Storage.openHandles(), 0u);
  Storage.faults().readPath = "book.txt";
  Storage.faults().readBytes = 5;
  hash = 123;
  EXPECT_EQ(paginator.hashSource(txt, hash), TextStatus::StorageError);
  EXPECT_EQ(hash, 0u);
  EXPECT_EQ(Storage.openHandles(), 0u);
  Storage.resetFaults();
  write("\xef\xbb\xbf");
  EXPECT_EQ(paginator.hashSource(txt, hash), TextStatus::Ok);
  EXPECT_TRUE(paginator.emptySource());
  paginator.close();
}
}  // namespace
