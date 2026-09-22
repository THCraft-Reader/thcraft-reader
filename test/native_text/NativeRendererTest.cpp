#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <NativeAllocator.h>
#include <NativeParagraphLayout.h>
#include <NativeTextEngine.h>
#include <fontIds.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
class NativeTextRendererTest : public testing::Test {
 protected:
  NativeTextEngine engine;
  HalDisplay display;
  GfxRenderer renderer{display};
  void SetUp() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    ASSERT_EQ(engine.initialize(), TextStatus::Ok);
    display.begin();
    renderer.begin();
    renderer.setNativeTextEngine(&engine);
  }
  void TearDown() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    renderer.setNativeTextEngine(nullptr);
    engine.shutdown();
    EXPECT_LE(native_text::allocationStats().peak, native_text::MEMORY_LIMIT);
  }
  std::vector<uint8_t> pixels() const {
    return {renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize()};
  }
  static int pixel(int32_t value) { return (value + 32) >> 6; }
  NativeLineInput input(const char* text) const {
    NativeLineInput result;
    result.fontId = NOTOSANS_14_FONT_ID;
    result.text = text;
    result.readerFeatures = false;
    return result;
  }
  void referenceRun(GfxRenderer& target, const NativeGlyphRun& run, int x, int y, bool black, bool rotated) {
    const int baseline = (std::max(run.ascender26, -run.ink.top26) + 63) >> 6;
    for (const auto& glyph : run.glyphs.span()) {
      NativeBitmapView bitmap;
      ASSERT_EQ(engine.rasterize(glyph, bitmap), TextStatus::Ok);
      for (int row = 0; row < bitmap.height; ++row) {
        const int sourceRow = bitmap.pitch < 0 ? bitmap.height - row - 1 : row;
        for (int col = 0; col < bitmap.width; ++col) {
          const uint8_t bucket = bitmap.data[sourceRow * std::abs(bitmap.pitch) + col] / 64;
          if (!bucket) continue;
          const int dx = pixel(glyph.x26) + bitmap.left + col;
          const int dy = baseline + pixel(glyph.y26) - bitmap.top + row;
          const int sx = x + (rotated ? dy : dx), sy = y + (rotated ? -dx : dy);
          const auto mode = target.grayPlanesAreAbsolute() ? GfxRenderer::BW : target.getRenderMode();
          if (mode == GfxRenderer::BW)
            target.drawPixel(sx, sy, black);
          else if (mode == GfxRenderer::GRAYSCALE_MSB && bucket != 3)
            target.drawPixel(sx, sy, false);
          else if (mode == GfxRenderer::GRAYSCALE_LSB && bucket == (black ? 2 : 1))
            target.drawPixel(sx, sy, false);
        }
      }
    }
  }
};

TEST_F(NativeTextRendererTest, RasterCoverageMatchesNativeGlyphsAcrossPlanesOrientationAndInversion) {
  constexpr const char* text = "กี่ น้ำ กู้ เก่ง AV";
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(input(text), run), TextStatus::Ok);
  HalDisplay expectedDisplay;
  expectedDisplay.begin();
  GfxRenderer expected(expectedDisplay);
  expected.begin();
  for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                                 GfxRenderer::LandscapeCounterClockwise}) {
    renderer.setOrientation(orientation);
    expected.setOrientation(orientation);
    for (const auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_MSB, GfxRenderer::GRAYSCALE_LSB}) {
      renderer.setRenderMode(mode);
      expected.setRenderMode(mode);
      for (const bool black : {true, false}) {
        for (const bool rotated : {false, true}) {
          SCOPED_TRACE(testing::Message() << orientation << '/' << mode << '/' << black << '/' << rotated);
          const uint8_t background = mode == GfxRenderer::BW && black ? 0xff : 0;
          renderer.clearScreen(background);
          expected.clearScreen(background);
          if (rotated)
            renderer.drawTextRotated90CW(NOTOSANS_14_FONT_ID, 80, 350, text, black);
          else
            renderer.drawText(NOTOSANS_14_FONT_ID, 80, 90, text, black);
          referenceRun(expected, run, 80, rotated ? 350 : 90, black, rotated);
          ASSERT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
          EXPECT_EQ(pixels(), std::vector<uint8_t>(expected.getFrameBuffer(),
                                                   expected.getFrameBuffer() + expected.getBufferSize()));
        }
      }
    }
  }
}

TEST_F(NativeTextRendererTest, ClipAndStripTargetsMatchWholePlaneWithoutTouchingFrameBuffer) {
  constexpr const char* text = "กี่ น้ำ กู้ เก่ง AV";
  for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise, GfxRenderer::PortraitInverted,
                                 GfxRenderer::LandscapeCounterClockwise}) {
    renderer.setOrientation(orientation);
    renderer.setClipRect(87, 90, 155, 40);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.clearScreen(0);
    renderer.drawText(NOTOSANS_14_FONT_ID, 80, 90, text);
    const auto whole = pixels();
    renderer.clearScreen(0x5a);
    const auto untouched = pixels();
    std::vector<uint8_t> assembled(renderer.getBufferSize(), 0);
    const int rowBytes = renderer.getDisplayWidthBytes();
    for (int y = 0; y < renderer.getDisplayHeight(); y += 17) {
      const int rows = std::min(17, int(renderer.getDisplayHeight()) - y);
      std::vector<uint8_t> scratch(rows * rowBytes, 0);
      renderer.beginStripTarget(scratch.data(), y, rows);
      renderer.drawText(NOTOSANS_14_FONT_ID, 80, 90, text);
      renderer.endStripTarget();
      std::copy(scratch.begin(), scratch.end(), assembled.begin() + y * rowBytes);
    }
    EXPECT_EQ(assembled, whole);
    EXPECT_EQ(pixels(), untouched);
  }
}

TEST_F(NativeTextRendererTest, MeasuresInkAndAdvanceAndCentersActualInk) {
  constexpr const char* text = "กี่ ไทย AV ";
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(input(text), run), TextStatus::Ok);
  EXPECT_EQ(renderer.getTextAdvanceX(NOTOSANS_14_FONT_ID, text, EpdFontFamily::REGULAR), pixel(run.advance26));
  const int width = ((run.ink.right26 + 63) >> 6) - (run.ink.left26 >> 6);
  EXPECT_EQ(renderer.getTextWidth(NOTOSANS_14_FONT_ID, text), width);
  renderer.clearScreen();
  renderer.drawCenteredText(NOTOSANS_14_FONT_ID, 50, text);
  const auto centered = pixels();
  renderer.clearScreen();
  renderer.drawText(NOTOSANS_14_FONT_ID, (renderer.getScreenWidth() - width) / 2 - (run.ink.left26 >> 6), 50, text);
  EXPECT_EQ(pixels(), centered);
}

TEST_F(NativeTextRendererTest, TruncationRetainsOnlySafeSourceClustersAndRemeasuresEllipsis) {
  constexpr const char* text = "เก่งกี่น้ำภาษาไทยประเทศไทย";
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(input(text), run), TextStatus::Ok);
  const int ellipsisWidth = renderer.getTextWidth(NOTOSANS_14_FONT_ID, "…");
  EXPECT_EQ(renderer.truncatedText(NOTOSANS_14_FONT_ID, text, ellipsisWidth - 1), "");
  for (int width = ellipsisWidth; width < 220; width += 13) {
    const auto shortened = renderer.truncatedText(NOTOSANS_14_FONT_ID, text, width);
    ASSERT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
    ASSERT_LE(renderer.getTextWidth(NOTOSANS_14_FONT_ID, shortened.c_str()), width);
    if (shortened == text) continue;
    ASSERT_GE(shortened.size(), size_t(3));
    ASSERT_EQ(shortened.substr(shortened.size() - 3), "…");
    const auto prefix = shortened.substr(0, shortened.size() - 3);
    EXPECT_EQ(std::string(text).substr(0, prefix.size()), prefix);
    EXPECT_TRUE(prefix.empty() ||
                std::any_of(run.clusters.span().begin(), run.clusters.span().end(), [&](const NativeCluster& cluster) {
                  return cluster.startByte == prefix.size() && !cluster.unsafeToBreak;
                }));
  }
}

TEST_F(NativeTextRendererTest, WrappingUsesParagraphFitterAndLastLineEllipsis) {
  constexpr const char* text = "ภาษาไทยประเทศไทย เก่งกี่ น้ำ EPUB 123 ภาษาไทยประเทศไทย";
  NativeParagraphLayout fitter(engine);
  NativeParagraphView paragraph;
  paragraph.text = text;
  NativeLayoutOptions options;
  options.fontId = NOTOSANS_14_FONT_ID;
  options.width = 155;
  options.readerFeatures = false;
  std::vector<std::string> expected;
  size_t consumed;
  int8_t level;
  const auto emit = [](void* context, NativeLayoutEmission&& emission) -> TextStatus {
    static_cast<std::vector<std::string>*>(context)->emplace_back(emission.line.logicalText());
    return TextStatus::Ok;
  };
  ASSERT_EQ(fitter.layout(paragraph, options, emit, &expected, consumed, level), TextStatus::Ok);
  ASSERT_EQ(consumed, std::strlen(text));
  EXPECT_EQ(renderer.wrappedText(NOTOSANS_14_FONT_ID, text, options.width, 100), expected);
  const auto limited = renderer.wrappedText(NOTOSANS_14_FONT_ID, text, options.width, 2);
  ASSERT_EQ(limited.size(), size_t(2));
  EXPECT_EQ(limited.front(), expected.front());
  EXPECT_EQ(limited.back().substr(limited.back().size() - 3), "…");
  EXPECT_LE(renderer.getTextWidth(NOTOSANS_14_FONT_ID, limited.back().c_str()), options.width);
}

TEST_F(NativeTextRendererTest, WarmAndFailedMeasurementNeverPaintAndLoanRemainsUntouched) {
  renderer.clearScreen(0x5a);
  const auto before = pixels();
  renderer.prewarmFallbackText(NOTOSANS_14_FONT_ID, "กี่ น้ำ เก่ง");
  renderer.ensureSdCardFontReady(NOTOSANS_14_FONT_ID, "ภาษาไทย", 1);
  EXPECT_EQ(pixels(), before);
  EXPECT_TRUE(renderer.getFontMap().empty());
  uint8_t* lentStorage = renderer.getFrameBuffer();
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    EXPECT_EQ(renderer.getTextWidth(NOTOSANS_14_FONT_ID, "\xff"), 0);
    EXPECT_EQ(renderer.lastTextStatus(), TextStatus::InvalidText);
    renderer.drawText(NOTOSANS_14_FONT_ID, 0, 0, "\xff");
    EXPECT_EQ(std::vector<uint8_t>(lentStorage, lentStorage + before.size()), before);
  }
  renderer.clearScreen();
  renderer.drawText(NOTOSANS_14_FONT_ID, 20, 20, "\xff");
  const auto diagnostic = pixels();
  renderer.drawText(NOTOSANS_14_FONT_ID, 20, 80, "\xff");
  EXPECT_EQ(pixels(), diagnostic);
  EXPECT_EQ(renderer.lastTextStatus(), TextStatus::InvalidText);
  renderer.clearTextStatus();
  EXPECT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
}

TEST_F(NativeTextRendererTest, NativeLineRubyAndDecorationsStayWithinReservedHeightAcrossCacheEviction) {
  constexpr const char* text = "กี่ น้ำ เก่ง";
  NativeParagraphView paragraph;
  paragraph.text = text;
  NativeStyleSpan span{0, static_cast<uint32_t>(std::strlen(text)),
                       EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH, 0};
  paragraph.spans = {&span, 1};
  NativeRubyInput ruby{0, 9, "ภาษาไทย", EpdFontFamily::REGULAR};
  paragraph.ruby = {&ruby, 1};
  NativeLayoutOptions options;
  options.fontId = NOTOSANS_14_FONT_ID;
  options.width = 220;
  options.alignment = NativeAlignment::Center;
  NativeLineData line;
  NativeParagraphLayout fitter(engine);
  size_t consumed;
  int8_t level;
  const auto emit = [](void* context, NativeLayoutEmission&& emission) -> TextStatus {
    *static_cast<NativeLineData*>(context) = std::move(emission.line);
    return TextStatus::Ok;
  };
  ASSERT_EQ(fitter.layout(paragraph, options, emit, &line, consumed, level), TextStatus::Ok);
  ASSERT_EQ(consumed, std::strlen(text));
  renderer.clearScreen();
  renderer.fillRect(0, 60, renderer.getScreenWidth(), line.lineHeight);
  const auto outsideLine = pixels();
  renderer.clearScreen();
  const auto white = pixels();
  ASSERT_TRUE(renderer.warmNativeLine(options.fontId, line));
  EXPECT_EQ(pixels(), white);
  ASSERT_TRUE(renderer.drawNativeLine(options.fontId, line, 20, 60));
  const auto first = pixels();
  for (size_t i = 0; i < first.size(); ++i) EXPECT_EQ(first[i] & outsideLine[i], outsideLine[i]);
  engine.clearCaches();
  renderer.clearScreen();
  ASSERT_TRUE(renderer.drawNativeLine(options.fontId, line, 20, 60));
  EXPECT_EQ(pixels(), first);
}

TEST_F(NativeTextRendererTest, ForcedOversizeConsumesClusterOnceAndClipsPaintAndHitBoxes) {
  NativeParagraphView paragraph;
  paragraph.text = "Mii";
  NativeLinkRange link{0, 3, 7};
  paragraph.links = {&link, 1};
  NativeLayoutOptions options;
  options.fontId = NOTOSANS_14_FONT_ID;
  options.width = 8;
  NativeParagraphLayout fitter(engine);
  std::vector<NativeLayoutEmission> lines;
  const auto emit = [](void* context, NativeLayoutEmission&& emission) {
    static_cast<std::vector<NativeLayoutEmission>*>(context)->push_back(std::move(emission));
    return TextStatus::Ok;
  };
  size_t consumed = 0;
  int8_t level = 0;
  ASSERT_EQ(fitter.layout(paragraph, options, emit, &lines, consumed, level), TextStatus::Ok);
  ASSERT_EQ(consumed, paragraph.text.size());
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines.front().line.logicalText(), "M");
  std::string retained;
  uint32_t source = 0;
  for (const auto& emission : lines) {
    retained += emission.line.logicalText();
    EXPECT_EQ(emission.sourceStart, source);
    source = emission.sourceEnd;
  }
  EXPECT_EQ(retained, paragraph.text);
  EXPECT_EQ(source, 3u);
  const auto& first = lines.front();
  ASSERT_FALSE(first.line.words.empty());
  for (const auto& word : first.line.words.span()) {
    EXPECT_GE(first.line.alignmentX26 + word.x26, 0);
    EXPECT_LE(first.line.alignmentX26 + word.x26 + word.width26, options.width * 64);
  }
  ASSERT_FALSE(first.links.empty());
  for (const auto& box : first.links.span()) {
    EXPECT_GE(box.x26, 0);
    EXPECT_LE(box.x26 + box.width26, options.width * 64);
  }
  for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise, GfxRenderer::PortraitInverted,
                                 GfxRenderer::LandscapeCounterClockwise}) {
    renderer.setOrientation(orientation);
    // The active clip is narrower than the overflow box: both must apply.
    renderer.setClipRect(42, 20, 5, 100);
    renderer.clearScreen();
    renderer.fillRect(42, 20, 5, 100);
    const auto outside = pixels();
    renderer.clearScreen();
    const auto blank = pixels();
    ASSERT_TRUE(renderer.drawNativeLine(options.fontId, first.line, 40, 30));
    const auto painted = pixels();
    EXPECT_NE(painted, blank);
    for (size_t i = 0; i < painted.size(); ++i) EXPECT_EQ(painted[i] & outside[i], outside[i]);
    int x, y, width, height;
    renderer.getClipRect(x, y, width, height);
    EXPECT_EQ(x, 42);
    EXPECT_EQ(y, 20);
    EXPECT_EQ(width, 5);
    EXPECT_EQ(height, 100);
    // Without a narrower caller clip, the content box itself still constrains M.
    renderer.setClipRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
    renderer.clearScreen();
    renderer.fillRect(40, 30, options.width, first.line.lineHeight);
    const auto contentOutside = pixels();
    renderer.clearScreen();
    ASSERT_TRUE(renderer.drawNativeLine(options.fontId, first.line, 40, 30));
    const auto contentPainted = pixels();
    for (size_t i = 0; i < contentPainted.size(); ++i)
      EXPECT_EQ(contentPainted[i] & contentOutside[i], contentOutside[i]);
  }
}
TEST_F(NativeTextRendererTest, NormalItalicOverhangAndNegativeIndentKeepUnclippedPixels) {
  NativeParagraphView paragraph;
  paragraph.text = "f";
  NativeStyleSpan style{0, 1, EpdFontFamily::ITALIC, 0};
  paragraph.spans = {&style, 1};
  NativeLineInput source;
  source.text = paragraph.text;
  source.fontId = NOTOSANS_14_FONT_ID;
  source.spans = paragraph.spans;
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(source, run), TextStatus::Ok);
  NativeLayoutOptions options;
  options.fontId = source.fontId;
  options.width = static_cast<uint16_t>((run.advance26 + 63) / 64);
  NativeParagraphLayout fitter(engine);
  HalDisplay expectedDisplay;
  expectedDisplay.begin();
  GfxRenderer expected(expectedDisplay);
  expected.begin();
  for (const int16_t indent : {int16_t{0}, int16_t{-12}}) {
    options.firstLineIndent = indent;
    NativeLineData line;
    const auto emit = [](void* context, NativeLayoutEmission&& emission) {
      *static_cast<NativeLineData*>(context) = std::move(emission.line);
      return TextStatus::Ok;
    };
    size_t consumed = 0;
    int8_t level = 0;
    ASSERT_EQ(fitter.layout(paragraph, options, emit, &line, consumed, level), TextStatus::Ok);
    ASSERT_EQ(consumed, paragraph.text.size());
    ASSERT_EQ(engine.shapeLine(line.input(options.fontId), run), TextStatus::Ok);
    renderer.clearScreen();
    expected.clearScreen();
    ASSERT_TRUE(renderer.drawNativeLine(options.fontId, line, 40, 30));
    const int referenceBaseline = (std::max(run.ascender26, -run.ink.top26) + 63) / 64;
    referenceRun(expected, run, 40 + pixel(line.alignmentX26), 30 + line.baseline - referenceBaseline, true, false);
    EXPECT_EQ(pixels(),
              std::vector<uint8_t>(expected.getFrameBuffer(), expected.getFrameBuffer() + expected.getBufferSize()));
    const auto painted = pixels();
    renderer.clearScreen();
    renderer.fillRect(40, 0, options.width, renderer.getScreenHeight());
    const auto outside = pixels();
    bool hasOutsideInk = false;
    for (size_t i = 0; i < painted.size(); ++i) hasOutsideInk |= (painted[i] & outside[i]) != outside[i];
    EXPECT_TRUE(hasOutsideInk);
  }
}

TEST_F(NativeTextRendererTest, ForcedRubyGroupClipsAnnotationAndRetainsFollowingText) {
  NativeParagraphView paragraph;
  paragraph.text = "MiiZ";
  NativeRubyInput ruby{0, 3, "annotation", EpdFontFamily::REGULAR};
  paragraph.ruby = {&ruby, 1};
  NativeLayoutOptions options;
  options.fontId = NOTOSANS_14_FONT_ID;
  options.width = 8;
  NativeParagraphLayout fitter(engine);
  std::vector<NativeLayoutEmission> lines;
  const auto emit = [](void* context, NativeLayoutEmission&& emission) {
    static_cast<std::vector<NativeLayoutEmission>*>(context)->push_back(std::move(emission));
    return TextStatus::Ok;
  };
  size_t consumed = 0;
  int8_t level = 0;
  ASSERT_EQ(fitter.layout(paragraph, options, emit, &lines, consumed, level), TextStatus::Ok);
  ASSERT_EQ(consumed, paragraph.text.size());
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].line.logicalText(), "Mii");
  EXPECT_EQ(lines[1].line.logicalText(), "Z");
  EXPECT_EQ(lines[0].sourceEnd, lines[1].sourceStart);
  ASSERT_EQ(lines[0].line.ruby.size(), 1u);
  renderer.clearScreen();
  renderer.fillRect(40, 30, options.width, lines[0].line.lineHeight);
  const auto outside = pixels();
  renderer.clearScreen();
  const auto blank = pixels();
  ASSERT_TRUE(renderer.drawNativeLine(options.fontId, lines[0].line, 40, 30));
  const auto painted = pixels();
  EXPECT_NE(painted, blank);
  for (size_t i = 0; i < painted.size(); ++i) EXPECT_EQ(painted[i] & outside[i], outside[i]);
}

TEST_F(NativeTextRendererTest, FailingRubyDiscardsStagedBasePixelsBeforeDrawingDiagnostic) {
  NativeLineData line;
  const std::string text = "กี่ น้ำ เก่ง ภาษาไทย";
  ASSERT_TRUE(line.text.assign({text.data(), text.size()}));
  line.baseline = renderer.getFontAscenderSize(NOTOSANS_14_FONT_ID);
  ASSERT_TRUE(line.ruby.resize(1));
  line.ruby[0] = {};
  line.ruby[0].textOffset = 1;  // Outside the empty ruby text arena.
  renderer.clearScreen();
  EXPECT_FALSE(renderer.drawNativeLine(NOTOSANS_14_FONT_ID, line, 20, 20));
  EXPECT_EQ(renderer.lastTextStatus(), TextStatus::InvalidText);
  const auto failedLine = pixels();
  renderer.clearTextStatus();
  renderer.clearScreen();
  renderer.drawText(NOTOSANS_14_FONT_ID, 20, 20, "\xff");
  EXPECT_EQ(pixels(), failedLine);
  line.overflowClipWidth = 8;
  renderer.clearTextStatus();
  renderer.setClipRect(5, 10, 80, 100);
  EXPECT_FALSE(renderer.drawNativeLine(NOTOSANS_14_FONT_ID, line, 20, 20));
  int x, y, width, height;
  renderer.getClipRect(x, y, width, height);
  EXPECT_EQ(x, 5);
  EXPECT_EQ(y, 10);
  EXPECT_EQ(width, 80);
  EXPECT_EQ(height, 100);
}
}  // namespace
