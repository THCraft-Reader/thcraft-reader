#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <NativeAllocator.h>
#include <NativeTextEngine.h>
#include <NativeUtf8.h>
#include <fontIds.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "../../lib/Epub/Epub/ParsedText.h"
#include "../../lib/Epub/Epub/hyphenation/Hyphenator.h"

namespace {
using Blocks = std::vector<std::unique_ptr<TextBlock>>;
size_t scalarCount(const std::string_view text) {
  size_t offset = 0, count = 0;
  uint32_t cp;
  while (native_text::nextUtf8(text, offset, cp)) ++count;
  return count;
}
std::string joined(const Blocks& lines) {
  std::string result;
  for (const auto& line : lines) result += line->nativeLine()->logicalText();
  return result;
}
struct Snapshot {
  std::string text;
  uint32_t start, end;
  int32_t alignment;
  int8_t paragraphLevel;
  std::vector<std::string> words;
  bool operator==(const Snapshot&) const = default;
};
class NativeTextParsedTextTest : public testing::Test {
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
    Hyphenator::setPreferredLanguage("");
  }
  void TearDown() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    renderer.setNativeTextEngine(nullptr);
    engine.shutdown();
    EXPECT_LE(native_text::allocationStats().peak, native_text::MEMORY_LIMIT);
  }
  bool layout(ParsedText& input, Blocks& output, uint16_t width = 240, bool final = true, int8_t characterSpacing = 0,
              uint8_t wordSpacingPercent = 100) {
    return input.layoutAndExtractLines(
        renderer, NOTOSANS_14_FONT_ID, width,
        [&](std::unique_ptr<TextBlock> block, uint32_t source) {
          EXPECT_EQ(source, block->sourceStartOffset());
          output.push_back(std::move(block));
        },
        final, characterSpacing, wordSpacingPercent);
  }
  std::vector<uint8_t> pixels(const TextBlock& block) {
    renderer.clearScreen(0xff);
    block.render(renderer, NOTOSANS_14_FONT_ID, 20, 20);
    EXPECT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
    return {renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize()};
  }
};

TEST_F(NativeTextParsedTextTest, AttachedMarkupPreservesLogicalNfdThaiAndRenderedClusters) {
  const std::string text = "e\xcc\x81 กี่ ภาษาไทยประเทศไทย กู้";
  ParsedText whole(true), fragments(true);
  whole.addWord(text, EpdFontFamily::REGULAR, false, false, 70);
  size_t offset = 0;
  uint32_t source = 70, cp;
  while (offset < text.size()) {
    const size_t start = offset;
    ASSERT_TRUE(native_text::nextUtf8(text, offset, cp));
    const auto style = cp == 0x0301 || cp == 0x0e48 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    fragments.addWord(std::string_view(text).substr(start, offset - start), style, false, true, source++);
  }
  ASSERT_EQ(fragments.size(), scalarCount(text));
  Blocks a, b;
  ASSERT_TRUE(layout(whole, a, 170, true, -1, 150));
  ASSERT_TRUE(layout(fragments, b, 170, true, -1, 150));
  ASSERT_EQ(a.size(), b.size());
  EXPECT_EQ(joined(a), text);
  EXPECT_EQ(joined(b), text);
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i]->nativeLine()->logicalText(), b[i]->nativeLine()->logicalText());
    EXPECT_EQ(a[i]->sourceStartOffset(), b[i]->sourceStartOffset());
    EXPECT_EQ(a[i]->sourceEndOffset(), b[i]->sourceEndOffset());
    EXPECT_EQ(pixels(*a[i]), pixels(*b[i]));
  }
  ASSERT_FALSE(b.empty());
  EXPECT_EQ(b.front()->sourceStartOffset(), 70u);
  EXPECT_EQ(b.back()->sourceEndOffset(), 70u + scalarCount(text));
  EXPECT_TRUE(fragments.isEmpty());
}

TEST_F(NativeTextParsedTextTest, CollapsedWhitespaceAndSyntheticMarkersKeepOriginalSourceCoordinates) {
  ParsedText input(true);
  input.addSyntheticText("10.", EpdFontFamily::REGULAR, 100);
  input.addWord("e\xcc\x81", EpdFontFamily::REGULAR, false, false, 100);
  // Three original spaces have become one displayed HTML space.
  input.addWord("กี่", EpdFontFamily::REGULAR, false, false, 105);
  Blocks lines;
  ASSERT_TRUE(layout(input, lines, 420));
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(joined(lines), "10. e\xcc\x81 กี่");
  EXPECT_EQ(lines[0]->sourceStartOffset(), 100u);
  EXPECT_EQ(lines[0]->sourceEndOffset(), 108u);
  EXPECT_EQ(lines[0]->nativeLine()->syntheticSuffixCp, 0u);
}

TEST_F(NativeTextParsedTextTest, StreamingRetainsSuffixAndIsIndependentOfFragmentSize) {
  std::string text = "e\xcc\x81 ";
  for (int i = 0; i < 610; ++i) text += "ภาษาไทยกี่";
  const auto stream = [&](size_t chunkScalars, std::vector<Snapshot>& output) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    style.textIndent = 12;
    ParsedText input(false, false, false, style);
    size_t offset = 0;
    uint32_t source = 1000;
    bool extracted = false;
    const auto collect = [&](std::unique_ptr<TextBlock> block, uint32_t start) {
      Snapshot value{std::string(block->nativeLine()->logicalText()),
                     start,
                     block->sourceEndOffset(),
                     block->nativeLine()->alignmentX26,
                     block->nativeLine()->paragraphLevel,
                     {}};
      for (uint16_t i = 0; i < block->wordCount(); ++i) value.words.emplace_back(block->wordText(i));
      output.push_back(std::move(value));
    };
    while (offset < text.size()) {
      const size_t start = offset;
      size_t count = 0;
      uint32_t cp;
      while (count < chunkScalars && offset < text.size()) {
        ASSERT_TRUE(native_text::nextUtf8(text, offset, cp));
        ++count;
      }
      input.addWord(std::string_view(text).substr(start, offset - start), EpdFontFamily::REGULAR, false, true, source);
      source += static_cast<uint32_t>(count);
      ASSERT_EQ(input.lastTextStatus(), TextStatus::Ok);
      const bool full = input.nativeNeedsLayout();
      const size_t oldLines = output.size();
      ASSERT_TRUE(input.layoutAndExtractLines(renderer, NOTOSANS_14_FONT_ID, 180, collect, false));
      if (full) {
        extracted = true;
        EXPECT_GE(input.nativePendingScalars(), 512u);
        EXPECT_LT(input.nativePendingScalars(), 4096u);
      } else
        EXPECT_EQ(output.size(), oldLines);
    }
    EXPECT_TRUE(extracted);
    ASSERT_TRUE(input.layoutAndExtractLines(renderer, NOTOSANS_14_FONT_ID, 180, collect, true));
    EXPECT_TRUE(input.isEmpty());
    EXPECT_EQ(input.nativePendingScalars(), 0u);
  };
  std::vector<Snapshot> scalarFragments, parserChunks;
  stream(1, scalarFragments);
  stream(63, parserChunks);  // 189 Thai bytes, crossing the exact 4096-scalar cut.
  ASSERT_EQ(scalarFragments, parserChunks);
  ASSERT_GT(parserChunks.size(), 1u);
  std::string reconstructed;
  uint32_t next = 1000;
  for (size_t i = 0; i < parserChunks.size(); ++i) {
    const auto& line = parserChunks[i];
    EXPECT_EQ(line.start, next);
    EXPECT_EQ(line.end - line.start, scalarCount(line.text));
    EXPECT_EQ(line.alignment, i ? 0 : 12 * 64);
    next = line.end;
    reconstructed += line.text;
  }
  EXPECT_EQ(reconstructed, text);
  EXPECT_EQ(next, 1000u + scalarCount(text));
}

TEST_F(NativeTextParsedTextTest, RetainedRubyFragmentHandlesAndLinksSurvivePrefixConsumption) {
  ParsedText input(true);
  std::string prefix, suffix;
  for (int i = 0; i < 540; ++i) prefix += "ภาษาไทย";  // 3780 scalars: ruby belongs to the retained suffix.
  for (int i = 0; i < 49; ++i) suffix += "ภาษาไทย";
  input.addWord(prefix, EpdFontFamily::REGULAR, false, true, 0);
  const auto id = input.addLinkTarget("chapter.xhtml#note");
  ASSERT_NE(id, 0);
  input.addWord("ก", EpdFontFamily::REGULAR, false, true, 3780, id);
  input.addWord("ี่", EpdFontFamily::BOLD, false, true, 3781, id);
  input.setRubyGroupAt(1, 2, "ไทย");
  input.setRubyForWordAt(1, "ภาษาไทย");
  input.addWord(suffix, EpdFontFamily::REGULAR, false, true, 3783);
  ASSERT_TRUE(input.nativeNeedsLayout());
  Blocks lines;
  ASSERT_TRUE(layout(input, lines, 220, false));
  EXPECT_GE(input.nativePendingScalars(), 512u);
  size_t leader = input.size();
  for (size_t i = 0; i < input.size(); ++i)
    if (input.getRubyTextAt(i) == "ภาษาไทย") leader = i;
  ASSERT_LT(leader + 1, input.size());
  EXPECT_NE(input.getWordStyleAt(leader + 1) & EpdFontFamily::RUBY_CONTINUE, 0);
  EXPECT_TRUE(input.linkTargetMatches(id, "chapter.xhtml#note"));
  ASSERT_TRUE(layout(input, lines, 220));
  EXPECT_EQ(joined(lines), prefix + "กี่" + suffix);
  size_t annotations = 0, linked = 0;
  for (auto& block : lines) {
    for (size_t r = 0; r < block->getNativeRuby().size(); ++r) {
      ++annotations;
      EXPECT_EQ(block->nativeRubyText(r), "ภาษาไทย");
      const auto& ruby = block->getNativeRuby()[r];
      EXPECT_EQ(block->nativeLine()->logicalText().substr(ruby.baseStartByte, ruby.baseEndByte - ruby.baseStartByte),
                "กี่");
      EXPECT_LE(block->sourceStartOffset(), 3780u);
      EXPECT_GT(block->sourceEndOffset(), 3782u);
    }
    auto links = block->takeLinkSpans();
    for (const auto& link : links.span()) {
      ++linked;
      EXPECT_STREQ(link.href, "chapter.xhtml#note");
      EXPECT_GT(link.width, 0);
      EXPECT_GT(link.height, 0);
      EXPECT_GE(link.top, 0);
      EXPECT_LE(link.top + link.height, block->layoutHeight(renderer, NOTOSANS_14_FONT_ID, 0.7f));
    }
  }
  EXPECT_EQ(annotations, 1u);
  EXPECT_GE(linked, 1u);
}

TEST_F(NativeTextParsedTextTest, RubyBoundaryMakesRoomWithoutFinalizingTheRetainedLine) {
  ParsedText input(true);
  std::string prefix, base;
  for (int i = 0; i < 571; ++i) prefix += "ภาษาไทย";
  prefix += "กี่";
  for (int i = 0; i < 200; ++i) base += "กี่";
  ASSERT_EQ(scalarCount(prefix), 4000u);
  ASSERT_EQ(scalarCount(base), 600u);
  input.addWord(prefix, EpdFontFamily::REGULAR, false, true, 0);
  Blocks lines;
  ASSERT_TRUE(input.layoutBeforeRuby(renderer, NOTOSANS_14_FONT_ID, 220,
                                     [&](std::unique_ptr<TextBlock> block, uint32_t source) {
                                       EXPECT_EQ(source, block->sourceStartOffset());
                                       lines.push_back(std::move(block));
                                     }));
  ASSERT_FALSE(lines.empty());
  EXPECT_GE(input.nativePendingScalars(), 512u);
  EXPECT_LT(input.nativePendingScalars(), 1000u);
  const size_t rubyIndex = input.size();
  input.addWord(base, EpdFontFamily::REGULAR, false, true, 4000);
  input.setRubyGroupAt(rubyIndex, 1, "คำอ่าน");
  ASSERT_EQ(input.lastTextStatus(), TextStatus::Ok);
  ASSERT_TRUE(layout(input, lines, 220));
  EXPECT_EQ(joined(lines), prefix + base);
  size_t groups = 0;
  for (const auto& block : lines) {
    for (const auto& ruby : block->getNativeRuby()) {
      ++groups;
      EXPECT_EQ(block->nativeLine()->logicalText().substr(ruby.baseStartByte, ruby.baseEndByte - ruby.baseStartByte),
                base);
      EXPECT_LE(block->sourceStartOffset(), 4000u);
      EXPECT_EQ(block->sourceEndOffset(), 4600u);
    }
  }
  EXPECT_EQ(groups, 1u);

  ParsedText shortInput(true);
  shortInput.addWord("ภาษา", EpdFontFamily::REGULAR);
  size_t callbacks = 0;
  ASSERT_TRUE(shortInput.layoutBeforeRuby(renderer, NOTOSANS_14_FONT_ID, 220,
                                          [&](std::unique_ptr<TextBlock>, uint32_t) { ++callbacks; }));
  EXPECT_EQ(callbacks, 0u);
  EXPECT_EQ(shortInput.nativePendingScalars(), 4u);
  const size_t index = shortInput.size();
  shortInput.addWord("ไทย", EpdFontFamily::REGULAR, false, true, 4);
  shortInput.setRubyForWordAt(index, "ไทย");
  Blocks shortLines;
  ASSERT_TRUE(layout(shortInput, shortLines, 220));
  ASSERT_EQ(shortLines.size(), 1u);
  EXPECT_EQ(joined(shortLines), "ภาษาไทย");
}

TEST_F(NativeTextParsedTextTest, ReplacingRubyFromABorrowedAnnotationPreservesBothGroups) {
  ParsedText input(true);
  input.addWord("กี่", EpdFontFamily::REGULAR, false, false, 0);
  input.addWord("ไทย", EpdFontFamily::REGULAR, false, false, 4);
  input.setRubyForWordAt(0, "x");
  input.setRubyForWordAt(1, "ภาษาไทยกู้");
  input.setRubyForWordAt(0, input.getRubyTextAt(1));
  EXPECT_EQ(input.getRubyTextAt(0), "ภาษาไทยกู้");
  EXPECT_EQ(input.getRubyTextAt(1), "ภาษาไทยกู้");
  Blocks lines;
  ASSERT_TRUE(layout(input, lines, 420));
  ASSERT_EQ(lines.size(), 1u);
  ASSERT_EQ(lines[0]->getNativeRuby().size(), 2u);
  EXPECT_EQ(lines[0]->nativeRubyText(0), "ภาษาไทยกู้");
  EXPECT_EQ(lines[0]->nativeRubyText(1), "ภาษาไทยกู้");
}

TEST_F(NativeTextParsedTextTest, FocusAndLinkedSuperscriptUseFinalClusterHitGeometry) {
  ParsedText input(true, false, true);
  const auto linkId = input.addLinkTarget("#note");
  input.addWord(
      "e\xcc\x81"
      "abcdef",
      EpdFontFamily::REGULAR, false, false, 10);
  input.addWord("กู้", EpdFontFamily::SUP, false, false, 19, linkId);
  Blocks lines;
  ASSERT_TRUE(layout(input, lines, 420));
  ASSERT_EQ(lines.size(), 1u);
  auto& block = *lines[0];
  const auto& line = *block.nativeLine();
  EXPECT_EQ(line.logicalText(),
            "e\xcc\x81"
            "abcdef กู้");
  bool bold = false;
  for (const auto& span : line.spans.span()) {
    EXPECT_NE(span.startByte, 1u);
    EXPECT_NE(span.endByte, 1u);  // The NFD accent cannot be detached by focus.
    bold |= (span.style & EpdFontFamily::BOLD) != 0;
  }
  EXPECT_TRUE(bold);
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(line.input(NOTOSANS_14_FONT_ID), run), TextStatus::Ok);
  auto links = block.takeLinkSpans();
  ASSERT_EQ(links.size(), 1u);
  EXPECT_STREQ(links[0].href, "#note");
  for (const auto& cluster : run.clusters.span()) {
    if (!(cluster.style & EpdFontFamily::SUP)) continue;
    const auto& box = links[0];
    EXPECT_LE(box.x * 64, line.alignmentX26 + cluster.x26);
    EXPECT_GE((box.x + box.width) * 64, line.alignmentX26 + cluster.x26 + cluster.advance26);
    EXPECT_LE(box.top * 64, line.baseline * 64 + cluster.top26);
    EXPECT_GE((box.top + box.height) * 64, line.baseline * 64 + cluster.bottom26);
  }
  EXPECT_GE(block.layoutHeight(renderer, NOTOSANS_14_FONT_ID, 0.5f), line.lineHeight);
}

TEST_F(NativeTextParsedTextTest, IngestionFailuresLatchAndNeverPaintOrPublishPartialOutput) {
  renderer.clearScreen(0x5a);
  const std::vector<uint8_t> before(renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize());
  ParsedText invalid(true);
  invalid.addWord("valid", EpdFontFamily::REGULAR);
  invalid.addWord(std::string_view("\xe0\x80\x80", 3), EpdFontFamily::REGULAR, false, true, 5);
  EXPECT_EQ(invalid.lastTextStatus(), TextStatus::InvalidText);
  Blocks lines;
  EXPECT_FALSE(layout(invalid, lines));
  EXPECT_TRUE(lines.empty());
  EXPECT_TRUE(invalid.isEmpty());
  EXPECT_EQ(renderer.lastTextStatus(), TextStatus::InvalidText);
  invalid.addWord("ignored", EpdFontFamily::REGULAR);
  EXPECT_FALSE(layout(invalid, lines));
  EXPECT_EQ(before,
            std::vector<uint8_t>(renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize()));

  ParsedText capacity(true);
  capacity.addWord(std::string(4297, 'a'), EpdFontFamily::REGULAR);
  EXPECT_EQ(capacity.lastTextStatus(), TextStatus::CapacityExceeded);
  EXPECT_FALSE(layout(capacity, lines));
  EXPECT_TRUE(lines.empty());
  EXPECT_EQ(renderer.lastTextStatus(), TextStatus::InvalidText);  // First failure remains latched.
  renderer.clearTextStatus();

  ParsedText allocation(true);
  native_text::failAllocationsAfter(0);
  allocation.addWord("ภาษาไทย", EpdFontFamily::REGULAR);
  EXPECT_EQ(allocation.lastTextStatus(), TextStatus::OutOfMemory);
  EXPECT_FALSE(layout(allocation, lines));
  EXPECT_EQ(renderer.lastTextStatus(), TextStatus::OutOfMemory);
  EXPECT_TRUE(lines.empty());
  native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
}

TEST_F(NativeTextParsedTextTest, LayoutAllocationFailureDiscardsStagedLinesAndEmptyInputSucceeds) {
  const std::string text = "ภาษาไทย ประเทศไทย กี่ ภาษาไทย ประเทศไทย กู้ ภาษาไทย ประเทศไทย";
  size_t failures = 0;
  for (size_t allowance = 0; allowance < 150; allowance += 13) {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    ParsedText input(true);
    input.addWord(text, EpdFontFamily::REGULAR, false, false, 100);
    ASSERT_EQ(input.lastTextStatus(), TextStatus::Ok);
    native_text::failAllocationsAfter(allowance);
    Blocks lines;
    const bool ok = layout(input, lines, 95);
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    if (ok)
      EXPECT_EQ(joined(lines), text);
    else {
      ++failures;
      EXPECT_EQ(input.lastTextStatus(), TextStatus::OutOfMemory);
      EXPECT_TRUE(lines.empty());
      EXPECT_TRUE(input.isEmpty());
    }
  }
  EXPECT_GT(failures, 0u);
  ParsedText empty(true);
  Blocks lines;
  EXPECT_TRUE(layout(empty, lines));
  EXPECT_TRUE(lines.empty());
  EXPECT_EQ(empty.lastTextStatus(), TextStatus::Ok);
}
}  // namespace
