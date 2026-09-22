#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Memory.h>
#include <NativeAllocator.h>
#include <NativeParagraphLayout.h>
#include <NativeTextEngine.h>
#include <fontIds.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {
class NativeTextPayloadTest : public testing::Test {
 protected:
  NativeTextEngine engine;
  HalDisplay display;
  GfxRenderer renderer{display};
  std::filesystem::path root;
  static constexpr int FONT = NOTOSANS_14_FONT_ID;

  void SetUp() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    ASSERT_EQ(engine.initialize(), TextStatus::Ok);
    renderer.begin();
    renderer.setNativeTextEngine(&engine);
    root = std::filesystem::temp_directory_path() /
           ("thcraft-native-payload-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
  }
  void TearDown() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    renderer.setNativeTextEngine(nullptr);
    engine.shutdown();
    std::filesystem::remove_all(root);
  }
  std::vector<uint8_t> pixels() const {
    return {renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize()};
  }
  std::unique_ptr<Page> paragraph(std::string_view text, bool ruby = false, int fontId = FONT) {
    auto page = std::make_unique<Page>();
    NativeParagraphLayout layout(engine);
    NativeParagraphView view;
    view.text = text;
    NativeRubyInput annotation{0, 12, "กี่", 0};
    if (ruby) view.ruby = {&annotation, 1};
    NativeLayoutOptions options;
    options.fontId = fontId;
    options.width = 420;
    options.focus = true;
    options.alignment = NativeAlignment::Center;
    size_t consumed = 0;
    int8_t level = 0;
    const auto emit = [](void* context, NativeLayoutEmission&& emission) {
      auto* page = static_cast<Page*>(context);
      auto block = makeUniqueNoThrow<TextBlock>(std::move(emission.line), BlockStyle{});
      if (!block) return TextStatus::OutOfMemory;
      if (!block->valid()) return TextStatus::InvalidText;
      int y = 0;
      if (!page->elements.empty()) {
        const auto& previous = static_cast<const PageLine&>(*page->elements.back());
        y = previous.yPos + previous.getBlock()->nativeLine()->lineHeight;
      }
      if (!page->addElement(makeUniqueNoThrow<PageLine>(std::move(block), 0, static_cast<int16_t>(y))))
        return TextStatus::OutOfMemory;
      return TextStatus::Ok;
    };
    const auto status = layout.layout(view, options, emit, page.get(), consumed, level);
    EXPECT_EQ(status, TextStatus::Ok);
    EXPECT_EQ(consumed, text.size());
    return status == TextStatus::Ok ? std::move(page) : nullptr;
  }
  std::vector<uint8_t> encode(const TextBlock& block) {
    const auto path = (root / "block.bin").string();
    HalFile file;
    EXPECT_TRUE(file.open(path.c_str(), "w+b"));
    EXPECT_TRUE(block.serialize(file));
    file.flush();
    std::vector<uint8_t> bytes(file.size());
    EXPECT_TRUE(file.seek(0));
    EXPECT_EQ(file.read(bytes.data(), bytes.size()), bytes.size());
    return bytes;
  }
  std::unique_ptr<TextBlock> decode(const std::vector<uint8_t>& bytes) {
    const auto path = (root / "decode.bin").string();
    HalFile file;
    if (!file.open(path.c_str(), "w+b") || file.write(bytes.data(), bytes.size()) != bytes.size()) return nullptr;
    file.flush();
    if (!file.seek(0)) return nullptr;
    serialization::BoundedFileReader input(file);
    return TextBlock::deserialize(input);
  }
  template <class T>
  static void patch(std::vector<uint8_t>& bytes, size_t offset, T value) {
    ASSERT_LE(offset + sizeof(value), bytes.size());
    memcpy(bytes.data() + offset, &value, sizeof(value));
  }
  template <class T>
  static void put(HalFile& file, T value) {
    ASSERT_EQ(file.write(&value, sizeof(value)), sizeof(value));
  }
  void writeSection(const std::string& path, const Page& page, const ReaderRenderSpec& spec, uint8_t version,
                    uint64_t fingerprint) {
    HalFile file;
    ASSERT_TRUE(file.open(path.c_str(), "w+b"));
    put(file, version);
    put(file, static_cast<int32_t>(spec.fontId));
    put(file, spec.lineCompression);
    put(file, static_cast<uint8_t>(spec.extraParagraphSpacing));
    put(file, spec.paragraphAlignment);
    put(file, spec.viewportWidth);
    put(file, spec.viewportHeight);
    put(file, static_cast<uint8_t>(spec.hyphenationEnabled));
    put(file, static_cast<uint8_t>(spec.embeddedStyle));
    put(file, spec.imageRendering);
    put(file, static_cast<uint8_t>(spec.focusReadingEnabled));
    put(file, fingerprint);
    put(file, uint16_t{1});
    for (int i = 0; i < 5; ++i) put(file, uint32_t{0});
    const uint32_t pageStart = static_cast<uint32_t>(file.position());
    ASSERT_TRUE(page.serialize(file));
    const uint32_t lut = static_cast<uint32_t>(file.position());
    put(file, pageStart);
    const uint32_t anchors = static_cast<uint32_t>(file.position());
    put(file, uint16_t{0});
    const uint32_t paragraphs = static_cast<uint32_t>(file.position());
    put(file, uint16_t{1});
    put(file, uint16_t{1});
    const uint32_t items = static_cast<uint32_t>(file.position());
    put(file, uint16_t{0});
    const uint32_t visible = static_cast<uint32_t>(file.position());
    put(file, uint32_t{37});
    if (version == 0xeb) {
      put(file, uint32_t{200});
      put(file, uint32_t{1000});
    }
    ASSERT_TRUE(file.seek(29));
    for (const uint32_t offset : {lut, anchors, paragraphs, items, visible}) put(file, offset);
    file.flush();
  }
  static std::string fileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
};

TEST_F(NativeTextPayloadTest, PageReloadPreservesOriginalThaiSelectionRubyAndPixels) {
  constexpr std::string_view text = "ภาษาไทย น้ำ ABC";
  auto page = paragraph(text, true);
  ASSERT_TRUE(page);
  ASSERT_EQ(page->elements.size(), 1u);
  const auto& original = *static_cast<const PageLine&>(*page->elements[0]).getBlock();
  ASSERT_EQ(original.getRubyTexts().size(), 1u);
  EXPECT_EQ(original.getRubyTexts()[0], "กี่");
  EXPECT_GE(original.layoutHeight(renderer, FONT, 0.1f), original.nativeLine()->lineHeight);
  renderer.clearScreen();
  const auto blank = pixels();
  ASSERT_TRUE(page->warmNativeText(renderer, FONT));
  EXPECT_EQ(pixels(), blank);
  page->render(renderer, FONT, 12, 30);
  const auto expected = pixels();
  ASSERT_NE(expected, blank);

  const auto path = (root / "page.bin").string();
  HalFile file;
  ASSERT_TRUE(file.open(path.c_str(), "w+b"));
  ASSERT_TRUE(page->serialize(file));
  file.flush();
  ASSERT_TRUE(file.seek(0));
  auto loaded = Page::deserialize(file);
  ASSERT_TRUE(loaded);
  ASSERT_EQ(loaded->elements.size(), 1u);
  const auto& restored = *static_cast<const PageLine&>(*loaded->elements[0]).getBlock();
  ASSERT_TRUE(restored.nativeLine());
  EXPECT_EQ(restored.nativeLine()->logicalText(), text);
  EXPECT_EQ(restored.getRubyTexts().back(), "กี่");
  ASSERT_EQ(restored.wordCount(), original.wordCount());
  std::string selected;
  for (uint16_t i = 0; i < restored.wordCount(); ++i) {
    EXPECT_STREQ(restored.wordText(i), original.wordText(i));
    EXPECT_EQ(restored.wordTextLen(i), strlen(restored.wordText(i)));
    EXPECT_EQ(restored.wordXpos(i), original.wordXpos(i));
    EXPECT_EQ(restored.wordWidth(i), original.wordWidth(i));
    EXPECT_EQ(restored.wordTop(i), original.wordTop(i));
    EXPECT_EQ(restored.wordHeight(i), original.wordHeight(i));
    EXPECT_LE(restored.wordTop(i) + restored.wordHeight(i), restored.layoutHeight(renderer, FONT, 0.1f));
    selected += restored.wordText(i);
  }
  EXPECT_NE(selected.find("ภาษา"), std::string::npos);
  EXPECT_NE(selected.find("ไทย"), std::string::npos);
  renderer.clearScreen();
  engine.clearCaches();
  ASSERT_TRUE(loaded->warmNativeText(renderer, FONT));
  EXPECT_EQ(pixels(), blank);
  loaded->render(renderer, FONT, 12, 30);
  EXPECT_EQ(pixels(), expected);
}

TEST_F(NativeTextPayloadTest, PunctuationOnlyLineIsNotEmptyAndLegacyArenaStillRoundTrips) {
  NativeLineData punctuation;
  ASSERT_TRUE(punctuation.text.assign(std::span<const char>("...", 3)));
  punctuation.lineHeight = 30;
  punctuation.baseline = 24;
  TextBlock native(std::move(punctuation), BlockStyle{});
  ASSERT_TRUE(native.valid());
  EXPECT_FALSE(native.isEmpty());
  EXPECT_EQ(native.wordCount(), 0);
  auto restoredNative = decode(encode(native));
  ASSERT_TRUE(restoredNative);
  EXPECT_FALSE(restoredNative->isEmpty());
  EXPECT_EQ(restoredNative->nativeLine()->logicalText(), "...");

  TextBlock legacy({"Latin", "ruby"}, {2, 71}, {EpdFontFamily::REGULAR, EpdFontFamily::BOLD}, {2, 0}, {17, 0},
                   BlockStyle{}, {"", "annotation"});
  ASSERT_TRUE(legacy.valid());
  auto restoredLegacy = decode(encode(legacy));
  ASSERT_TRUE(restoredLegacy);
  EXPECT_EQ(restoredLegacy->nativeLine(), nullptr);
  EXPECT_STREQ(restoredLegacy->wordText(1), "ruby");
  EXPECT_EQ(restoredLegacy->wordXpos(1), 71);
  EXPECT_EQ(restoredLegacy->focusBoundary(0), 2);
  EXPECT_EQ(restoredLegacy->focusSuffixX(0), 17);
  EXPECT_EQ(restoredLegacy->getRubyTexts().back(), "annotation");
  TextBlock smallLegacy({"a"}, {0}, {EpdFontFamily::REGULAR}, {}, {}, BlockStyle{}, {"b"});
  auto badLegacy = encode(smallLegacy);
  // representation/header (6), one legacy word's arrays (5), text + NUL (2).
  patch(badLegacy, 13, uint32_t{0xffffffff});
  EXPECT_FALSE(decode(badLegacy));
  badLegacy = encode(smallLegacy);
  badLegacy.pop_back();
  EXPECT_FALSE(decode(badLegacy));
}
TEST_F(NativeTextPayloadTest, TextBlockControlIsBudgetedAndAllocationFailureDoesNotConsumeLine) {
  const size_t initialUsage = native_text::allocationStats().used;
  NativeLineData line;
  ASSERT_TRUE(line.text.assign(std::span<const char>("M", 1)));
  line.lineHeight = 30;
  line.baseline = 24;
  const size_t baseline = native_text::allocationStats().used;
  native_text::failAllocationsAfter(0);
  auto failed = makeUniqueNoThrow<TextBlock>(std::move(line), BlockStyle{});
  native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
  EXPECT_FALSE(failed);
  EXPECT_EQ(line.logicalText(), "M");
  EXPECT_EQ(native_text::allocationStats().used, baseline);
  {
    auto block = makeUniqueNoThrow<TextBlock>(std::move(line), BlockStyle{});
    ASSERT_TRUE(block);
    ASSERT_TRUE(block->valid());
    EXPECT_EQ(block->nativeLine()->logicalText(), "M");
    const size_t charged = native_text::allocationStats().used - baseline;
    EXPECT_GE(charged, sizeof(TextBlock));
    RecordProperty("controlObjectBytes", static_cast<int>(sizeof(TextBlock)));
    RecordProperty("budgetedControlBytes", static_cast<int>(charged));
    // Destruction must release both the control and its owned text.
  }
  const size_t afterBlock = native_text::allocationStats().used;
  EXPECT_EQ(afterBlock, initialUsage);
  {
    NativeBuffer<uint8_t> fill;
    const size_t remaining = native_text::MEMORY_LIMIT - afterBlock;
    ASSERT_GT(remaining, sizeof(TextBlock));
    // Native allocations include an aligned header, leaving less than one
    // TextBlock control allocation while still permitting the fill itself.
    ASSERT_TRUE(fill.resize(remaining - sizeof(TextBlock)));
    auto capped = makeUniqueNoThrow<TextBlock>(NativeLineData{}, BlockStyle{});
    EXPECT_FALSE(capped);
  }
  EXPECT_EQ(native_text::allocationStats().used, afterBlock);
}

TEST_F(NativeTextPayloadTest, ForcedOverflowReloadKeepsPixelsSelectionsAndPageLinksInsideContent) {
  NativeParagraphView view;
  view.text = "Mii";
  NativeLinkRange link{0, 3, 0};
  view.links = {&link, 1};
  NativeLayoutOptions options;
  options.fontId = FONT;
  options.width = 8;
  NativeParagraphLayout fitter(engine);
  auto page = std::make_unique<Page>();
  const auto emit = [](void* context, NativeLayoutEmission&& emission) {
    auto& page = *static_cast<Page*>(context);
    int y = 0;
    if (!page.elements.empty()) {
      const auto& previous = static_cast<const PageLine&>(*page.elements.back());
      y = previous.yPos + previous.getBlock()->nativeLine()->lineHeight;
    }
    for (const auto& box : emission.links.span()) {
      const int x = box.x26 / 64;
      const int right = (box.x26 + box.width26 + 63) / 64;
      if (!page.addLink("#target", static_cast<int16_t>(x), static_cast<int16_t>(y + box.top),
                        static_cast<int16_t>(right - x), box.height))
        return TextStatus::CapacityExceeded;
    }
    auto block = makeUniqueNoThrow<TextBlock>(std::move(emission.line), BlockStyle{});
    if (!block) return TextStatus::OutOfMemory;
    if (!block->valid()) return TextStatus::InvalidText;
    if (!page.addElement(makeUniqueNoThrow<PageLine>(std::move(block), 0, static_cast<int16_t>(y))))
      return TextStatus::OutOfMemory;
    return TextStatus::Ok;
  };
  size_t consumed = 0;
  int8_t level = 0;
  ASSERT_EQ(fitter.layout(view, options, emit, page.get(), consumed, level), TextStatus::Ok);
  ASSERT_EQ(consumed, view.text.size());
  const auto path = (root / "overflow-page.bin").string();
  HalFile file;
  ASSERT_TRUE(file.open(path.c_str(), "w+b"));
  ASSERT_TRUE(page->serialize(file));
  file.flush();
  ASSERT_TRUE(file.seek(0));
  auto restored = Page::deserialize(file);
  ASSERT_TRUE(restored);
  std::vector<uint8_t> expectedPixels;
  for (const Page* candidate : {page.get(), restored.get()}) {
    std::string retained;
    for (const auto& element : candidate->elements) {
      const auto& block = *static_cast<const PageLine&>(*element).getBlock();
      retained += block.nativeLine()->logicalText();
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        EXPECT_GE(block.wordXpos(i), 0);
        EXPECT_LE(block.wordXpos(i) + block.wordWidth(i), options.width);
      }
    }
    EXPECT_EQ(retained, view.text);
    ASSERT_FALSE(candidate->links.empty());
    for (const auto& box : candidate->links) {
      EXPECT_GE(box.x, 0);
      EXPECT_LE(box.x + box.width, options.width);
    }
    renderer.clearScreen();
    renderer.fillRect(40, 0, options.width, renderer.getScreenHeight());
    const auto outside = pixels();
    renderer.clearScreen();
    const auto blank = pixels();
    engine.clearCaches();
    candidate->render(renderer, FONT, 40, 30);
    ASSERT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
    const auto painted = pixels();
    EXPECT_NE(painted, blank);
    for (size_t i = 0; i < painted.size(); ++i) EXPECT_EQ(painted[i] & outside[i], outside[i]);
    if (candidate == page.get())
      expectedPixels = painted;
    else
      EXPECT_EQ(painted, expectedPixels);
  }
  const auto& first = *static_cast<const PageLine&>(*restored->elements.front()).getBlock();
  auto bad = encode(first);
  patch(bad, 26, uint16_t{32768});
  EXPECT_FALSE(decode(bad));
  bad = encode(first);
  const size_t wordStart = 28 + first.nativeLine()->text.size() + first.nativeLine()->spans.size() * 6;
  patch(bad, wordStart + 8, int32_t{9 * 64});
  EXPECT_FALSE(decode(bad));
}

TEST_F(NativeTextPayloadTest, RejectsTruncationUnsafeRangesAndGeometryWithoutLeakingNativeStorage) {
  auto page = paragraph("ภาษาไทย น้ำ", true);
  ASSERT_TRUE(page);
  const auto& block = *static_cast<const PageLine&>(*page->elements[0]).getBlock();
  const auto& line = *block.nativeLine();
  ASSERT_GT(line.words.size(), 0u);
  const auto valid = encode(block);
  const size_t baseline = native_text::allocationStats().used;
  for (size_t length = 0; length < valid.size(); ++length) {
    SCOPED_TRACE(length);
    EXPECT_FALSE(decode({valid.begin(), valid.begin() + length}));
  }
  const size_t spanStart = 28 + line.text.size();
  const size_t wordStart = spanStart + line.spans.size() * 6;
  const size_t rubyStart = wordStart + line.words.size() * 16 + line.gaps.size() * 6;
  auto bad = valid;
  patch(bad, 3, uint16_t{4097});
  EXPECT_FALSE(decode(bad));
  bad = valid;
  bad[28] = 0xff;
  EXPECT_FALSE(decode(bad));
  bad = valid;
  patch(bad, wordStart, uint16_t{1});  // Inside the first three-byte Thai scalar.
  EXPECT_FALSE(decode(bad));
  bad = valid;
  patch(bad, wordStart + 8, int32_t{-1});
  EXPECT_FALSE(decode(bad));
  bad = valid;
  patch(bad, wordStart + 4, std::numeric_limits<int32_t>::max());
  EXPECT_FALSE(decode(bad));
  bad = valid;
  patch(bad, rubyStart + 4, uint16_t{65535});
  EXPECT_FALSE(decode(bad));
  EXPECT_EQ(native_text::allocationStats().used, baseline);
  native_text::failAllocationsAfter(0);
  EXPECT_FALSE(decode(valid));
  native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
  EXPECT_EQ(native_text::allocationStats().used, baseline);
}

TEST_F(NativeTextPayloadTest, RejectsMalformedPageCountsTagsAndShortMetadata) {
  const auto path = (root / "bad-page.bin").string();
  for (const std::vector<uint8_t>& bytes :
       {std::vector<uint8_t>{1}, std::vector<uint8_t>{0xff, 0xff}, std::vector<uint8_t>{0, 0, 17, 0},
        std::vector<uint8_t>{0, 0, 0, 0, 33, 0}, std::vector<uint8_t>{1, 0, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        // One image: malicious first path length, followed by enough bytes
        // to pass the page's minimum-record preflight.
        std::vector<uint8_t>{1, 0, 2, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f, 0, 0, 0}}) {
    HalFile file;
    ASSERT_TRUE(file.open(path.c_str(), "w+b"));
    ASSERT_EQ(file.write(bytes.data(), bytes.size()), bytes.size());
    file.flush();
    ASSERT_TRUE(file.seek(0));
    EXPECT_FALSE(Page::deserialize(file));
  }
}

TEST_F(NativeTextPayloadTest, BoundedPageDecodeStopsBeforeAdjacentRecordsAndClassifiesFailures) {
  auto original = paragraph("ภาษาไทย กี่ น้ำ ABC");
  ASSERT_TRUE(original);
  HalFile file;
  const auto path = (root / "bounded-pages.bin").string();
  ASSERT_TRUE(file.open(path.c_str(), "w+b"));
  ASSERT_TRUE(original->serialize(file));
  const auto bytes = static_cast<uint32_t>(file.position());
  ASSERT_TRUE(original->serialize(file));
  file.flush();
  const size_t baseline = native_text::allocationStats().used;
  TextStatus status = TextStatus::Ok;

  ASSERT_TRUE(file.seek(0));
  EXPECT_FALSE(Page::deserialize(file, bytes - 1, status));
  EXPECT_EQ(status, TextStatus::InvalidText);
  EXPECT_LT(file.position(), bytes);
  EXPECT_EQ(native_text::allocationStats().used, baseline);

  ASSERT_TRUE(file.seek(0));
  auto decoded = Page::deserialize(file, bytes, status);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(status, TextStatus::Ok);
  EXPECT_EQ(file.position(), bytes);
  decoded.reset();

  ASSERT_TRUE(file.seek(0));
  Storage.faults().readPath = path;
  Storage.faults().readBytes = 1;
  EXPECT_FALSE(Page::deserialize(file, bytes, status));
  EXPECT_EQ(status, TextStatus::StorageError);
  EXPECT_EQ(file.position(), 1u);
  Storage.resetFaults();

  ASSERT_TRUE(file.seek(0));
  native_text::failAllocationsAfter(0);
  EXPECT_FALSE(Page::deserialize(file, bytes, status));
  EXPECT_EQ(status, TextStatus::OutOfMemory);
  native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
  EXPECT_EQ(native_text::allocationStats().used, baseline);
}

TEST_F(NativeTextPayloadTest, FailedPageElementGrowthPreservesPreviouslyRenderableContent) {
  auto page = paragraph("ภาษาไทย กี่ น้ำ");
  ASSERT_TRUE(page);
  renderer.clearScreen();
  page->render(renderer, FONT, 0, 0);
  ASSERT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
  const auto before = pixels();
  native_text::failAllocationsAfter(0);
  EXPECT_FALSE(page->reserveElements(Page::MAX_ELEMENTS_PER_PAGE));
  native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
  renderer.clearScreen();
  page->render(renderer, FONT, 0, 0);
  ASSERT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
  EXPECT_EQ(pixels(), before);
}

TEST_F(NativeTextPayloadTest, FinalAndPartialSectionsRejectStaleFingerprintButKeepProgressAndMetadata) {
  auto page = paragraph("ภาษาไทย");
  ASSERT_TRUE(page);
  auto epub = std::make_shared<Epub>("fixture.epub", root.string());
  const auto directory = std::filesystem::path(epub->getCachePath());
  std::filesystem::create_directories(directory / "sections");
  const std::string path = (directory / "sections/0.bin").string();
  const auto metadata = directory / "book.bin";
  const auto progress = directory / "progress.bin";
  std::ofstream(metadata, std::ios::binary) << "metadata";
  std::ofstream(progress, std::ios::binary) << "source-position";
  ReaderRenderSpec spec;
  spec.fontId = FONT;
  spec.viewportWidth = 420;
  spec.viewportHeight = 700;
  const uint64_t fingerprint = renderer.textLayoutFingerprint(FONT);
  for (uint8_t version : {uint8_t{47}, uint8_t{0xeb}}) {
    writeSection(path, *page, spec, version, fingerprint);
    {
      Section section(epub, 0, renderer);
      ASSERT_TRUE(section.loadSectionFile(spec));
      EXPECT_EQ(section.isPartial(), version == 0xeb);
      ASSERT_TRUE(section.loadPage(0));
      EXPECT_EQ(section.getTextFromSectionFile(), "ภาษาไทย");
      EXPECT_EQ(section.getVisibleTextOffsetForPage(0), 37u);
    }
    writeSection(path, *page, spec, version, fingerprint ^ 1);
    {
      Section stale(epub, 0, renderer);
      EXPECT_FALSE(stale.loadSectionFile(spec));
      EXPECT_FALSE(stale.loadPage(0));
    }
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(std::filesystem::file_size(metadata), 8u);
    EXPECT_EQ(std::filesystem::file_size(progress), 15u);
  }
}

TEST_F(NativeTextPayloadTest, ResidentFinalAndPartialSectionsRejectLiveFontIdentityChanges) {
  constexpr int CUSTOM = 71237, ALIAS = 71238, OTHER = 71239;
  auto epub = std::make_shared<Epub>("fixture.epub", root.string());
  const auto directory = std::filesystem::path(epub->getCachePath());
  std::filesystem::create_directories(directory / "sections");
  const auto path = (directory / "sections/0.bin").string();
  const auto progress = directory / "progress.bin";
  const auto metadata = directory / "book.bin";
  std::ofstream(progress, std::ios::binary) << "source-position";
  std::ofstream(metadata, std::ios::binary) << "metadata";
  const auto customPath = (root / "Family-Regular.ttf").string();
  const NativeFontFile custom{customPath, 0, {}};
  const NativeFontFile other{NATIVE_SUBSET_FONT_PATH, 0, {}};
  const NativeVariation light{0x77676874u, 100};  // wght
  const NativeVariation heavy{0x77676874u, 900};
  const NativeFontFile variableLight{NATIVE_VARIABLE_FONT_PATH, 0, {&light, 1}};
  const NativeFontFile variableHeavy{NATIVE_VARIABLE_FONT_PATH, 0, {&heavy, 1}};

  for (uint8_t version : {uint8_t{47}, uint8_t{0xeb}}) {
    for (int change = 0; change < 4; ++change) {
      SCOPED_TRACE(testing::Message() << "version=" << unsigned(version) << " change=" << change);
      engine.clearCustomFonts();
      std::filesystem::copy_file(NATIVE_FONT_PATH, customPath, std::filesystem::copy_options::overwrite_existing);
      ASSERT_EQ(engine.registerCustomFont(CUSTOM, 14, {&custom, 1}, false), TextStatus::Ok);
      ASSERT_EQ(engine.registerCustomFont(OTHER, 14, {&other, 1}, false), TextStatus::Ok);
      ReaderRenderSpec spec;
      spec.fontId = CUSTOM;
      spec.viewportWidth = 420;
      spec.viewportHeight = 700;
      if (change == 1) {
        ASSERT_EQ(engine.registerFontAlias(ALIAS, CUSTOM, 14), TextStatus::Ok);
        spec.fontId = ALIAS;
      } else if (change == 2) {
        ASSERT_EQ(engine.setUiFallback(CUSTOM), TextStatus::Ok);
        spec.fontId = UI_12_FONT_ID;
      } else if (change == 3) {
        ASSERT_EQ(engine.registerCustomFont(CUSTOM, 14, {&variableLight, 1}, false), TextStatus::Ok);
      }
      auto page = paragraph("ภาษาไทย abc", false, spec.fontId);
      ASSERT_TRUE(page);
      const auto fingerprint = renderer.textLayoutFingerprint(spec.fontId);
      writeSection(path, *page, spec, version, fingerprint);
      const auto original = fileBytes(path);
      Section section(epub, 0, renderer);
      ASSERT_TRUE(section.loadSectionFile(spec));
      ASSERT_TRUE(section.textLayoutMatches());
      auto resident = section.loadPage(0);
      ASSERT_TRUE(resident);
      ASSERT_EQ(resident->visibleTextOffset, 37u);

      if (change == 0) {
        engine.releaseSdFaces();
        std::filesystem::copy_file(NATIVE_SUBSET_FONT_PATH, customPath,
                                   std::filesystem::copy_options::overwrite_existing);
        ASSERT_EQ(engine.registerCustomFont(CUSTOM, 14, {&custom, 1}, false), TextStatus::Ok);
      } else if (change == 1) {
        ASSERT_EQ(engine.registerFontAlias(ALIAS, OTHER, 14), TextStatus::Ok);
      } else if (change == 2) {
        ASSERT_EQ(engine.setUiFallback(OTHER), TextStatus::Ok);
      } else {
        ASSERT_EQ(engine.registerCustomFont(CUSTOM, 14, {&variableHeavy, 1}, false), TextStatus::Ok);
      }
      ASSERT_NE(renderer.textLayoutFingerprint(spec.fontId), fingerprint);
      EXPECT_FALSE(section.textLayoutMatches());  // Reader must not paint its resident page.
      EXPECT_FALSE(section.loadPage(0));
      EXPECT_EQ(section.getVisibleTextOffsetForPage(0), resident->visibleTextOffset);
      EXPECT_EQ(section.getParagraphIndexForPage(0), 1u);
      section.suspendBuild();
      Section reopened(epub, 0, renderer);
      EXPECT_FALSE(reopened.loadSectionFile(spec));
      EXPECT_FALSE(reopened.loadPage(0));
      EXPECT_EQ(fileBytes(path), original);
      EXPECT_EQ(fileBytes(progress), "source-position");
      EXPECT_EQ(fileBytes(metadata), "metadata");
    }
  }
}

TEST_F(NativeTextPayloadTest, LiveIdentityChangeAbandonsBuildWithoutReplacingPriorCache) {
  constexpr int CUSTOM = 71237;
  const NativeFontFile originalFont{NATIVE_FONT_PATH, 0, {}};
  const NativeFontFile replacementFont{NATIVE_SUBSET_FONT_PATH, 0, {}};
  auto epub = std::make_shared<Epub>("fixture.epub", root.string());
  const auto directory = std::filesystem::path(epub->getCachePath());
  std::filesystem::create_directories(directory / "sections");
  std::filesystem::create_directories(directory / "html");
  const auto path = (directory / "sections/0.bin").string();
  std::string html = "<html><body>";
  for (int i = 0; i < 1000; ++i) html += "<p>ภาษาไทย น้ำ abc กี่ เก่ง</p>";
  html += "</body></html>";
  std::ofstream(directory / "html/0.html", std::ios::binary) << html;
  ReaderRenderSpec spec;
  spec.fontId = CUSTOM;
  spec.viewportWidth = 420;
  spec.viewportHeight = 130;
  spec.embeddedStyle = false;

  for (uint8_t version : {uint8_t{47}, uint8_t{0xeb}}) {
    for (int stop = 0; stop < 3; ++stop) {
      SCOPED_TRACE(testing::Message() << "version=" << unsigned(version) << " stop=" << stop);
      ASSERT_EQ(engine.registerCustomFont(CUSTOM, 14, {&originalFont, 1}, false), TextStatus::Ok);
      auto page = paragraph("ภาษาไทย", false, CUSTOM);
      ASSERT_TRUE(page);
      writeSection(path, *page, spec, version, renderer.textLayoutFingerprint(CUSTOM));
      const auto original = fileBytes(path);
      Section section(epub, 0, renderer);
      ASSERT_TRUE(section.loadSectionFile(spec));
      ASSERT_TRUE(section.startBuild(spec));
      ASSERT_TRUE(section.buildSomeMore(1));
      ASSERT_TRUE(section.isBuilding());
      auto built = section.loadPage(0);
      ASSERT_TRUE(built);
      const auto restoreOffset = section.getVisibleTextOffsetForPage(0);
      ASSERT_EQ(restoreOffset, built->visibleTextOffset);
      ASSERT_EQ(engine.registerCustomFont(CUSTOM, 14, {&replacementFont, 1}, false), TextStatus::Ok);
      ASSERT_FALSE(section.textLayoutMatches());
      if (stop == 0)
        EXPECT_FALSE(section.buildSomeMore(0));
      else if (stop == 1)
        EXPECT_FALSE(section.loadPage(0));
      else
        section.suspendBuild();
      EXPECT_FALSE(section.isBuilding());
      EXPECT_FALSE(section.isBuildComplete());
      EXPECT_FALSE(std::filesystem::exists(path + ".part"));
      EXPECT_EQ(fileBytes(path), original);
      EXPECT_EQ(built->visibleTextOffset, restoreOffset);

      // The same object can rebuild, but may not expose the old partial before
      // it has produced pages under the replacement identity.
      ASSERT_TRUE(section.startBuild(spec));
      EXPECT_TRUE(section.textLayoutMatches());
      EXPECT_FALSE(section.loadPage(0));
      section.suspendBuild();
      EXPECT_EQ(fileBytes(path), original);
    }
  }
}
}  // namespace
