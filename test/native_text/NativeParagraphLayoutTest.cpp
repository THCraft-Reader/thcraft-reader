#include <NativeAllocator.h>
#include <NativeParagraphLayout.h>
#include <NativeTextEngine.h>
#include <NativeUtf8.h>
#include <fontIds.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

#include "../../lib/Epub/Epub/hyphenation/Hyphenator.h"

namespace {
using Lines = std::vector<NativeLayoutEmission>;
TextStatus collect(void* context, NativeLayoutEmission&& value) {
  static_cast<Lines*>(context)->push_back(std::move(value));
  return TextStatus::Ok;
}
std::string joined(const Lines& lines) {
  std::string result;
  for (const auto& value : lines) result += value.line.logicalText();
  return result;
}
std::vector<uint32_t> ends(const Lines& lines) {
  std::vector<uint32_t> result;
  for (const auto& value : lines) result.push_back(value.sourceEnd);
  return result;
}
class NativeTextParagraphTest : public testing::Test {
 protected:
  NativeTextEngine engine;
  NativeLayoutOptions options;
  void SetUp() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    ASSERT_EQ(engine.initialize(), TextStatus::Ok);
    Hyphenator::setPreferredLanguage("");
    options.fontId = NOTOSANS_14_FONT_ID;
    options.width = 240;
  }
  void TearDown() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    engine.shutdown();
    EXPECT_LE(native_text::allocationStats().peak, native_text::MEMORY_LIMIT);
  }
  TextStatus fit(NativeParagraphView view, Lines& lines, size_t& consumed) {
    NativeParagraphLayout layout(engine);
    int8_t level = -1;
    return layout.layout(view, options, collect, &lines, consumed, level);
  }
  int32_t width(std::string_view text) {
    NativeGlyphRun run;
    NativeLineInput input;
    input.text = text;
    input.fontId = options.fontId;
    EXPECT_EQ(engine.shapeLine(input, run), TextStatus::Ok);
    return run.advance26;
  }
  void expectFits(const Lines& lines) {
    for (const auto& value : lines) {
      NativeGlyphRun run;
      ASSERT_EQ(engine.shapeLine(value.line.input(options.fontId), run), TextStatus::Ok);
      EXPECT_LE(run.advance26 + value.line.alignmentX26, options.width * 64);
      EXPECT_GE(value.line.baseline * 64 + run.ink.top26, 0);
      EXPECT_LE(value.line.baseline * 64 + run.ink.bottom26, value.line.lineHeight * 64);
    }
  }
};

TEST_F(NativeTextParagraphTest, ThaiDictionaryWordsPreserveNoSpaceSourceAndNeverStretch) {
  const std::string text = "ภาษาไทยประเทศไทยภาษาไทยประเทศไทย";
  NativeParagraphView view;
  view.text = text;
  options.width = 150;
  options.alignment = NativeAlignment::Justify;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_GT(lines.size(), 1u);
  EXPECT_EQ(consumed, text.size());
  EXPECT_EQ(joined(lines), text);
  std::array<uint32_t, 64> boundaries{};
  size_t count = 0;
  ASSERT_EQ(engine.findThaiBreaks(text, boundaries, count), TextStatus::Ok);
  for (const auto& value : lines) {
    EXPECT_TRUE(value.line.gaps.empty());
    EXPECT_EQ(value.line.syntheticSuffixCp, 0u);
    if (value.endByte != text.size())
      EXPECT_NE(std::find(boundaries.begin(), boundaries.begin() + count, value.endByte), boundaries.begin() + count);
    for (const auto& word : value.line.words.span()) {
      EXPECT_EQ(std::string_view(value.line.selectionText.data() + word.selectionTextOffset),
                value.line.logicalText().substr(word.startByte, word.endByte - word.startByte));
    }
  }
  expectFits(lines);
}

TEST_F(NativeTextParagraphTest, RealSpacesJustifyButZeroWidthThaiBoundariesDoNot) {
  const std::string text = "ภาษาไทย ภาษาไทย ภาษาไทย ภาษาไทย ภาษาไทย";
  NativeParagraphView view;
  view.text = text;
  options.width = static_cast<uint16_t>((width("ภาษาไทย ภาษาไทย ") + 63) / 64 + 18);
  options.alignment = NativeAlignment::Justify;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_GT(lines.size(), 1u);
  bool expanded = false;
  for (size_t i = 0; i + 1 < lines.size(); ++i) {
    for (const auto& gap : lines[i].line.gaps.span()) {
      ASSERT_GT(gap.byteOffset, 0u);
      EXPECT_EQ(lines[i].line.text[gap.byteOffset - 1], ' ');
      expanded |= gap.extraAdvance26 > 0;
    }
  }
  EXPECT_TRUE(expanded);
  EXPECT_EQ(joined(lines), text);
  expectFits(lines);
}

TEST_F(NativeTextParagraphTest, SpacingChangesFittingAndPersistsTheSameRenderedGeometry) {
  NativeParagraphView view;
  view.text = "AB CD";
  options.width = static_cast<uint16_t>((width(view.text) + 63) / 64);
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_EQ(lines.size(), 1u);
  lines.clear();
  options.characterSpacing = 2;
  options.wordSpacingPercent = 200;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].line.logicalText(), "AB ");
  EXPECT_EQ(lines[1].line.logicalText(), "CD");
  EXPECT_EQ(consumed, view.text.size());
  EXPECT_EQ(joined(lines), view.text);
  expectFits(lines);
  for (const auto& value : lines) {
    NativeLineInput expected;
    expected.text = value.line.logicalText();
    expected.fontId = options.fontId;
    expected.characterSpacing = options.characterSpacing;
    expected.wordSpacingPercent = options.wordSpacingPercent;
    NativeGlyphRun measured, rendered;
    ASSERT_EQ(engine.shapeLine(expected, measured), TextStatus::Ok);
    ASSERT_EQ(engine.shapeLine(value.line.input(options.fontId), rendered), TextStatus::Ok);
    EXPECT_EQ(rendered.advance26, measured.advance26);
    ASSERT_EQ(rendered.glyphs.size(), measured.glyphs.size());
    for (size_t i = 0; i < measured.glyphs.size(); ++i) EXPECT_EQ(rendered.glyphs[i].x26, measured.glyphs[i].x26);
  }
  lines.clear();
  options.characterSpacing = -2;
  options.wordSpacingPercent = 50;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_EQ(lines.size(), 1u);
  expectFits(lines);
  NativeGlyphRun compact;
  ASSERT_EQ(engine.shapeLine(lines[0].line.input(options.fontId), compact), TextStatus::Ok);
  EXPECT_LT(compact.advance26, width(view.text));
}

TEST_F(NativeTextParagraphTest, SpacedThaiCellsAndRtlWordsRetainSelectionAndJustificationGeometry) {
  NativeParagraphView view;
  view.text = "กี่กู้ אב AB กี่กู้ אב AB กี่กู้ אב AB";
  options.characterSpacing = -2;
  options.wordSpacingPercent = 50;
  options.alignment = NativeAlignment::Justify;
  options.width = 170;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_GT(lines.size(), 1u);
  EXPECT_EQ(joined(lines), view.text);
  EXPECT_EQ(consumed, view.text.size());
  expectFits(lines);
  for (const auto& value : lines) {
    NativeGlyphRun run;
    ASSERT_EQ(engine.shapeLine(value.line.input(options.fontId), run), TextStatus::Ok);
    for (const auto& gap : value.line.gaps.span()) EXPECT_GE(gap.extraAdvance26, 0);
    for (const auto& word : value.line.words.span()) {
      int32_t left = INT32_MAX, right = INT32_MIN;
      for (const auto& cluster : run.clusters.span()) {
        if (cluster.startByte >= word.endByte || cluster.endByte <= word.startByte) continue;
        left = std::min(left, cluster.x26);
        right = std::max(right, cluster.x26 + cluster.advance26);
      }
      EXPECT_EQ(word.x26, left);
      EXPECT_EQ(word.width26, right - left);
      EXPECT_EQ(std::string_view(value.line.selectionText.data() + word.selectionTextOffset),
                value.line.logicalText().substr(word.startByte, word.endByte - word.startByte));
    }
  }
}

TEST_F(NativeTextParagraphTest, NbspStaysJoinedAndZwspIsPreservedButBreakable) {
  const std::string glued = "one\xc2\xa0two";
  const std::string text = "a " + glued + " three";
  NativeParagraphView view;
  view.text = text;
  options.width = static_cast<uint16_t>((width(glued + " ") + 63) / 64 + 2);
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  EXPECT_EQ(joined(lines), text);
  EXPECT_TRUE(std::any_of(lines.begin(), lines.end(), [&](const auto& line) {
    return line.line.logicalText().find(glued) != std::string_view::npos;
  }));
  lines.clear();
  const std::string separated = "ภาษา\xe2\x80\x8bไทย";
  view.text = separated;
  options.width = static_cast<uint16_t>((width("ภาษา") + 63) / 64 + 1);
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_GE(lines.size(), 2u);
  EXPECT_EQ(joined(lines), separated);
  EXPECT_EQ(lines.front().endByte, std::string("ภาษา\xe2\x80\x8b").size());
}

TEST_F(NativeTextParagraphTest, OriginalNfdAndSourceAnchorsSurviveLayout) {
  const std::string text = "e\xcc\x81กี่ test";
  const std::array<NativeSourceAnchor, 3> anchors{{{0, 100}, {3, 500}, {12, 503}}};
  NativeParagraphView view;
  view.text = text;
  view.sourceAnchors = anchors;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines.front().sourceStart, 100u);
  EXPECT_EQ(lines.back().sourceEnd, 508u);
  EXPECT_EQ(joined(lines), text);
  EXPECT_EQ(consumed, text.size());
}

TEST_F(NativeTextParagraphTest, LeadingVowelCellsAndMarksRemainWholeAtNarrowWidths) {
  const std::string text = "เก่งเก่งกี่กู้ญูฐูเก่งเก่งกี่กู้";
  NativeParagraphView view;
  view.text = text;
  NativeGlyphRun paragraph;
  NativeLineInput input;
  input.text = text;
  input.fontId = options.fontId;
  ASSERT_EQ(engine.shapeLine(input, paragraph), TextStatus::Ok);
  options.width = 60;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_GT(lines.size(), 1u);
  EXPECT_EQ(joined(lines), text);
  for (const auto& line : lines) {
    if (line.endByte == text.size()) continue;
    EXPECT_TRUE(std::any_of(
        paragraph.clusters.span().begin(), paragraph.clusters.span().end(),
        [&](const NativeCluster& cluster) { return cluster.startByte == line.endByte && !cluster.unsafeToBreak; }));
  }
  expectFits(lines);
}

TEST_F(NativeTextParagraphTest, MarkMarkupAndFocusCannotSplitBaseOwnedCluster) {
  const std::string text = "เก่งเก่ง ภาษาไทย";
  const std::array<NativeStyleSpan, 3> spans{
      {{0, 6, 0, 0}, {6, 9, 1, 0}, {9, static_cast<uint32_t>(text.size()), 0, 0}}};
  NativeParagraphView view;
  view.text = text;
  view.spans = spans;
  options.focus = true;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  EXPECT_EQ(joined(lines), text);
  for (const auto& line : lines) {
    for (const auto& span : line.line.spans.span()) {
      EXPECT_NE(span.startByte + line.startByte, 3u);
      EXPECT_NE(span.startByte + line.startByte, 6u);
      EXPECT_NE(span.endByte + line.startByte, 3u);
      EXPECT_NE(span.endByte + line.startByte, 6u);
    }
  }
  expectFits(lines);
}

TEST_F(NativeTextParagraphTest, RubyIsUnbreakableAndInkFitsReservedExtent) {
  const std::string text = "กี่ ไทย ภาษา";
  const std::string annotation = "ภาษาไทย กี่กู้";
  const std::array<NativeRubyInput, 1> ruby{{{0, 6, annotation, 0}}};
  NativeParagraphView view;
  view.text = text;
  view.ruby = ruby;
  options.width = 220;
  options.characterSpacing = 2;
  options.wordSpacingPercent = 150;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_FALSE(lines.empty());
  ASSERT_EQ(lines.front().line.ruby.size(), 1u);
  const auto& line = lines.front().line;
  const auto& value = line.ruby[0];
  NativeStyleSpan style{0, value.textBytes, 16, 0};
  NativeLineInput input;
  input.text = annotation;
  input.fontId = options.fontId;
  input.spans = {&style, 1};
  input.characterSpacing = options.characterSpacing;
  input.wordSpacingPercent = options.wordSpacingPercent;
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(input, run), TextStatus::Ok);
  EXPECT_GE(value.x26, 0);
  EXPECT_LE(value.x26 + run.advance26, options.width * 64);
  EXPECT_GE(value.y26 + run.ink.top26, 0);
  EXPECT_LE(value.y26 + run.ink.bottom26, line.lineHeight * 64);
  EXPECT_GT(line.rubyLift, 0);
  for (const auto& item : lines) EXPECT_FALSE(item.endByte > 0 && item.endByte < 6);
  EXPECT_EQ(joined(lines), text);
  expectFits(lines);
}

TEST_F(NativeTextParagraphTest, LinkAndWordBoxesFollowFinalVisualClusters) {
  const std::string text = "אב ภาษาไทย A";
  const uint32_t start = static_cast<uint32_t>(text.find("ภาษา"));
  const uint32_t stop = start + static_cast<uint32_t>(std::string("ภาษาไทย").size());
  const std::array<NativeLinkRange, 1> links{{{start, stop, 73}}};
  NativeParagraphView view;
  view.text = text;
  view.links = links;
  options.width = 400;
  options.alignment = NativeAlignment::Center;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_EQ(lines.size(), 1u);
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(lines[0].line.input(options.fontId), run), TextStatus::Ok);
  for (const auto& cluster : run.clusters.span()) {
    if (cluster.startByte >= stop || cluster.endByte <= start) continue;
    const int32_t left = lines[0].line.alignmentX26 + cluster.x26;
    EXPECT_TRUE(std::any_of(lines[0].links.span().begin(), lines[0].links.span().end(), [&](const auto& box) {
      return box.id == 73 && box.x26 <= left && box.x26 + box.width26 >= left + cluster.advance26 &&
             box.top <= lines[0].line.baseline + cluster.top26 / 64 &&
             (box.top + box.height) * 64 >= lines[0].line.baseline * 64 + cluster.bottom26;
    }));
  }
  for (const auto& word : lines[0].line.words.span()) {
    EXPECT_GE(word.width26, 0);
    EXPECT_GE(word.top, 0);
    EXPECT_LE(word.top + word.height, lines[0].line.lineHeight);
  }
}

TEST_F(NativeTextParagraphTest, StreamingWindowsAreIndependentOfArrivalChunks) {
  std::string text;
  for (int i = 0; i < 450; ++i) text += "ภาษาไทย เก่งกี่ ";
  auto stream = [&](size_t chunk, Lines& output) {
    NativeParagraphLayout layout(engine);
    std::string pending;
    size_t input = 0;
    uint32_t source = 0;
    int8_t level = -1;
    NativeLayoutOptions opts = options;
    while (input < text.size() || !pending.empty()) {
      const size_t take = std::min(chunk, text.size() - input);
      pending.append(text, input, take);
      input += take;
      NativeParagraphView view;
      view.text = pending;
      view.sourceStart = source;
      view.sourceUnit = NativeSourceUnit::Byte;
      view.paragraphLevel = level;
      view.final = input == text.size();
      size_t consumed = 0;
      const auto status = layout.layout(view, opts, collect, &output, consumed, level);
      ASSERT_EQ(status, TextStatus::Ok);
      if (!consumed && view.final) {
        FAIL() << "final window made no progress";
        return;
      }
      source += static_cast<uint32_t>(consumed);
      pending.erase(0, consumed);
      if (consumed) opts.firstLine = false;
    }
    EXPECT_EQ(source, text.size());
  };
  Lines whole, chunked;
  stream(text.size(), whole);
  stream(199, chunked);
  EXPECT_EQ(joined(whole), text);
  EXPECT_EQ(joined(chunked), text);
  EXPECT_EQ(ends(whole), ends(chunked));
}

TEST_F(NativeTextParagraphTest, NonfinalWindowRetainsSuffixAndUncommittedLine) {
  std::string text;
  for (int i = 0; i < 800; ++i) text += "ภาษาไทย";
  size_t prefix = 0;
  ASSERT_EQ(NativeParagraphLayout::windowPrefix(text, prefix, false), TextStatus::Ok);
  NativeParagraphView view;
  view.text = std::string_view(text).substr(0, prefix);
  view.final = false;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_GT(consumed, 0u);
  EXPECT_EQ(joined(lines), text.substr(0, consumed));
  size_t retained = 0, offset = consumed;
  uint32_t cp;
  while (offset < prefix) {
    ASSERT_TRUE(native_text::nextUtf8(text, offset, cp));
    ++retained;
  }
  EXPECT_GE(retained, 512u);
  EXPECT_LT(consumed, prefix);
}

TEST_F(NativeTextParagraphTest, DynamicProgrammingUsesSquaredSlackAndGreedyUsesLongestFit) {
  const std::string text = "aaa bb cc ddddd";
  std::vector<uint32_t> boundaries{0};
  for (size_t i = 0; i < text.size(); ++i)
    if (text[i] == ' ') boundaries.push_back(static_cast<uint32_t>(i + 1));
  boundaries.push_back(static_cast<uint32_t>(text.size()));
  const size_t count = boundaries.size();
  std::vector<std::vector<int32_t>> widths(count, std::vector<int32_t>(count));
  int32_t minimum = 0;
  for (size_t i = 0; i + 1 < count; ++i)
    for (size_t j = i + 1; j < count; ++j) {
      widths[i][j] = width(std::string_view(text).substr(boundaries[i], boundaries[j] - boundaries[i]));
      if (j == i + 1) minimum = std::max(minimum, widths[i][j]);
    }
  std::vector<uint32_t> optimal, greedy;
  auto path = [&](const std::vector<size_t>& next) {
    std::vector<uint32_t> result;
    for (size_t at = 0; at + 1 < count; at = next[at]) result.push_back(boundaries[next[at]]);
    return result;
  };
  for (int pixels = (minimum + 63) / 64; pixels < 400; ++pixels) {
    std::vector<uint64_t> cost(count, UINT64_MAX / 4);
    std::vector<size_t> dp(count), gr(count);
    cost.back() = 0;
    for (size_t i = count - 1; i-- > 0;) {
      for (size_t j = i + 1; j < count && widths[i][j] <= pixels * 64; ++j) {
        gr[i] = j;
        const uint64_t slack = pixels * 64 - widths[i][j];
        const uint64_t candidate = j + 1 == count ? 0 : slack * slack + cost[j];
        if (candidate <= cost[i]) {
          cost[i] = candidate;
          dp[i] = j;
        }
      }
    }
    optimal = path(dp);
    greedy = path(gr);
    if (optimal != greedy) {
      options.width = static_cast<uint16_t>(pixels);
      break;
    }
  }
  ASSERT_NE(optimal, greedy);
  NativeParagraphView view;
  view.text = text;
  view.sourceUnit = NativeSourceUnit::Byte;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  EXPECT_EQ(ends(lines), optimal);
  lines.clear();
  options.hyphenation = true;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  EXPECT_EQ(ends(lines), greedy);
}

TEST_F(NativeTextParagraphTest, MaximumLinesConsumesOnlyEmittedPrefix) {
  const std::string text = "ภาษาไทย ภาษาไทย ภาษาไทย ภาษาไทย";
  NativeParagraphView view;
  view.text = text;
  options.width = 120;
  options.maxLines = 1;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(consumed, lines.front().endByte);
  EXPECT_LT(consumed, text.size());
  EXPECT_EQ(joined(lines), text.substr(0, consumed));
}

TEST_F(NativeTextParagraphTest, OversizedProtectedClusterIsConsumedOnce) {
  const std::string text = "เก่งกี่";
  NativeParagraphView view;
  view.text = text;
  options.width = 1;
  Lines lines;
  size_t consumed = 0;
  ASSERT_EQ(fit(view, lines, consumed), TextStatus::Ok);
  EXPECT_EQ(consumed, text.size());
  EXPECT_EQ(joined(lines), text);
  for (const auto& line : lines) EXPECT_GT(line.endByte, line.startByte);
}

TEST_F(NativeTextParagraphTest, CallbackFailureCannotReportSuccessfulConsumption) {
  NativeParagraphLayout layout(engine);
  NativeParagraphView view;
  view.text = "ภาษาไทย ภาษาไทย";
  int calls = 0;
  auto reject = [](void* context, NativeLayoutEmission&&) {
    ++*static_cast<int*>(context);
    return TextStatus::StorageError;
  };
  size_t consumed = 99;
  int8_t level = -1;
  EXPECT_EQ(layout.layout(view, options, reject, &calls, consumed, level), TextStatus::StorageError);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(consumed, 0u);
}

TEST_F(NativeTextParagraphTest, EmptyAndIncompleteInputHaveDistinctContracts) {
  NativeParagraphLayout layout(engine);
  NativeParagraphView view;
  size_t consumed = 9;
  int8_t level = -1;
  EXPECT_EQ(layout.layout(view, options, nullptr, nullptr, consumed, level), TextStatus::Ok);
  EXPECT_EQ(consumed, 0u);
  size_t prefix = 0;
  EXPECT_EQ(NativeParagraphLayout::windowPrefix("A\xe0\xb8", prefix, false), TextStatus::Ok);
  EXPECT_EQ(prefix, 1u);
  EXPECT_EQ(NativeParagraphLayout::windowPrefix("A\xe0\xb8", prefix, true), TextStatus::InvalidText);
  EXPECT_EQ(NativeParagraphLayout::windowPrefix("A\xe0\x80", prefix, false), TextStatus::InvalidText);
  EXPECT_EQ(NativeParagraphLayout::windowPrefix("A\xff", prefix, false), TextStatus::InvalidText);
}

TEST_F(NativeTextParagraphTest, OversizedSourceClusterFailsInsteadOfDroppingItsMarks) {
  std::string text = "a";
  for (size_t i = 0; i < 4200; ++i) text += "\xcc\x81";
  NativeParagraphView view;
  view.text = text;
  Lines lines;
  size_t consumed = 0;
  EXPECT_EQ(fit(view, lines, consumed), TextStatus::CapacityExceeded);
  EXPECT_EQ(consumed, 0u);
  EXPECT_TRUE(lines.empty());
}

TEST_F(NativeTextParagraphTest, NeutralCallbackFragmentDoesNotPrematurelyFixParagraphDirection) {
  NativeParagraphLayout layout(engine);
  NativeParagraphView view;
  view.text = "  ";
  view.final = false;
  Lines lines;
  size_t consumed = 0;
  int8_t level = -1;
  ASSERT_EQ(layout.layout(view, options, collect, &lines, consumed, level), TextStatus::Ok);
  EXPECT_EQ(consumed, 0u);
  EXPECT_TRUE(lines.empty());
  EXPECT_EQ(level, -1);
  view.text = "  אב";
  view.final = true;
  view.paragraphLevel = level;
  ASSERT_EQ(layout.layout(view, options, collect, &lines, consumed, level), TextStatus::Ok);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(level, 1);
  EXPECT_EQ(lines.front().line.paragraphLevel, 1);
}

TEST_F(NativeTextParagraphTest, BoundedHyphenPolicyPreservesMarkerPriorityAndRejectsShortOutput) {
  const std::string text = "abc-def";
  std::array<CodepointInfo, 7> points;
  for (size_t i = 0; i < text.size(); ++i) points[i] = {static_cast<uint32_t>(text[i]), i + 40};
  std::array<Hyphenator::BreakInfo, 7> result{};
  size_t count = 99;
  ASSERT_TRUE(Hyphenator::breakOffsets(points.data(), points.size(), true, result.data(), result.size(), count));
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(result[0].byteOffset, 44u);
  EXPECT_FALSE(result[0].requiresInsertedHyphen);
  // An explicit separator wins over emergency fallback, while original source
  // offsets need not start at zero (the native window supplies absolute bytes).
  points[3].value = '\'';
  ASSERT_TRUE(Hyphenator::breakOffsets(points.data(), points.size(), true, result.data(), result.size(), count));
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(result[0].byteOffset, 44u);
  EXPECT_FALSE(result[0].requiresInsertedHyphen);
  EXPECT_FALSE(Hyphenator::breakOffsets(points.data(), points.size(), true, result.data(), 1, count));
  EXPECT_EQ(count, 0u);
}
}  // namespace
