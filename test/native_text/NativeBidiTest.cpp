#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

extern "C" {
#include "minibidi.h"
}

namespace {

std::vector<bidi_char> logical(std::initializer_list<ucschar> text) {
  std::vector<bidi_char> line;
  for (const auto cp : text) line.push_back({cp, cp, static_cast<uint16_t>(line.size()), 0});
  return line;
}

int resolve(const std::vector<bidi_char>& line, std::vector<uchar>& levels, bool autodir = true, int base = 0) {
  std::vector<uchar> scratch(bidi_scratch_size(line.size()));
  levels.assign(line.size(), 0xff);
  return resolve_bidi_levels(autodir, base, line.data(), static_cast<int>(line.size()), levels.data(), scratch.data(),
                             scratch.size());
}

// L2 applied by the consumer to logical indices, not to the input codepoints.
std::vector<uint16_t> visualOrder(const std::vector<uchar>& levels) {
  std::vector<uint16_t> indices(levels.size());
  std::iota(indices.begin(), indices.end(), 0);
  int highest = 0, lowestOdd = 256;
  for (auto level : levels) {
    highest = std::max(highest, static_cast<int>(level));
    if (level & 1) lowestOdd = std::min(lowestOdd, static_cast<int>(level));
  }
  for (int threshold = highest; threshold >= lowestOdd; --threshold) {
    for (size_t start = 0; start < levels.size();) {
      if (levels[start] < threshold) {
        ++start;
        continue;
      }
      size_t end = start + 1;
      while (end < levels.size() && levels[end] >= threshold) ++end;
      std::reverse(indices.begin() + start, indices.begin() + end);
      start = end;
    }
  }
  return indices;
}

std::vector<uint16_t> legacyOrder(std::vector<bidi_char> line, bool autodir = true, int base = 0) {
  do_bidi(autodir, base, line.data(), static_cast<int>(line.size()));
  std::vector<uint16_t> result;
  for (const auto& ch : line) result.push_back(ch.index);
  return result;
}

void expectUnchanged(const std::vector<bidi_char>& actual, const std::vector<bidi_char>& original) {
  ASSERT_EQ(actual.size(), original.size());
  for (size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(actual[i].origwc, original[i].origwc);
    EXPECT_EQ(actual[i].wc, original[i].wc);
    EXPECT_EQ(actual[i].index, original[i].index);
    EXPECT_EQ(actual[i].joiners, original[i].joiners);
  }
}

}  // namespace

TEST(NativeTextBidi, PureLtrInitializesEveryLevelIncludingThaiMarks) {
  const auto line = logical({'A', ' ', 0x0e01, 0x0e34, 0x0e48, '1', '2'});
  std::vector<uchar> levels;
  ASSERT_EQ(resolve(line, levels), 0);
  EXPECT_EQ(levels, (std::vector<uchar>{0, 0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(visualOrder(levels), legacyOrder(line));
}

TEST(NativeTextBidi, MixedArabicHebrewAndDigitsRetainLogicalCodepoints) {
  auto line = logical({'A', ' ', 0x0628, 0x062a, ' ', 0x05d0, 0x05d1, ' ', '1', '2'});
  line[2].joiners = ZWJ;
  const auto original = line;
  std::vector<uchar> levels;
  ASSERT_EQ(resolve(line, levels), 0);
  EXPECT_EQ(levels, (std::vector<uchar>{0, 0, 1, 1, 1, 1, 1, 1, 2, 2}));
  const auto order = visualOrder(levels);
  EXPECT_EQ(order, (std::vector<uint16_t>{0, 1, 8, 9, 7, 6, 5, 4, 3, 2}));
  EXPECT_EQ(order, legacyOrder(line));
  expectUnchanged(line, original);
}

TEST(NativeTextBidi, AutoAndExplicitParagraphDirectionsRemainDistinct) {
  const auto line = logical({0x05d0, ' ', 'A'});
  std::vector<uchar> levels;
  ASSERT_EQ(resolve(line, levels), 1);
  EXPECT_EQ(levels, (std::vector<uchar>{1, 1, 2}));
  EXPECT_EQ(visualOrder(levels), legacyOrder(line));
  ASSERT_EQ(resolve(line, levels, false, 0), 0);
  EXPECT_EQ(levels, (std::vector<uchar>{1, 0, 0}));
  EXPECT_EQ(visualOrder(levels), legacyOrder(line, false, 0));
}

TEST(NativeTextBidi, BracketResolutionDoesNotMirrorOrShape) {
  const auto line = logical({0x0628, '(', 0x05d0, ')'});
  const auto original = line;
  std::vector<uchar> levels;
  ASSERT_EQ(resolve(line, levels), 1);
  EXPECT_EQ(levels, (std::vector<uchar>{1, 1, 1, 1}));
  EXPECT_EQ(visualOrder(levels), (std::vector<uint16_t>{3, 2, 1, 0}));
  EXPECT_EQ(visualOrder(levels), legacyOrder(line));
  expectUnchanged(line, original);
}

TEST(NativeTextBidi, ExplicitEmbeddingsAndIsolatesUseCallerWorkspace) {
  std::vector<uchar> levels;
  auto line = logical({0x202a, 'a', 0x202c, 0x202b, 0x05d0, 0x202c});
  ASSERT_EQ(resolve(line, levels, false, 0), 0);
  EXPECT_EQ(levels, (std::vector<uchar>{0, 2, 0, 0, 1, 0}));
  EXPECT_EQ(visualOrder(levels), legacyOrder(line, false, 0));
  line = logical({'A', 0x2067, 0x05d0, 0x2069, 'B'});
  ASSERT_EQ(resolve(line, levels), 0);
  EXPECT_EQ(levels, (std::vector<uchar>{0, 0, 1, 0, 0}));
  EXPECT_EQ(visualOrder(levels), legacyOrder(line));
}

TEST(NativeTextBidi, ParagraphWhitespaceIsRetainedForFinalLineL1) {
  const auto line = logical({'A', ' ', 0x05d0, ' ', 0x05d1});
  std::vector<uchar> levels;
  ASSERT_EQ(resolve(line, levels), 0);
  // A line ending after index 3 resets that space to level 0 during L1.
  // Paragraph resolution must retain level 1 until that line is chosen.
  EXPECT_EQ(levels, (std::vector<uchar>{0, 0, 1, 1, 1}));
}

TEST(NativeTextBidi, FullNativeWindowIsNotTruncatedAtLegacyLimit) {
  std::vector<bidi_char> line(BIDI_MAX_NATIVE_LINE);
  for (size_t i = 0; i < line.size(); ++i) {
    const ucschar cp = i < line.size() / 2 ? 'A' : 0x05d0;
    line[i] = {cp, cp, static_cast<uint16_t>(i), 0};
  }
  std::vector<uchar> levels;
  ASSERT_EQ(resolve(line, levels), 0);
  for (size_t i = 0; i < levels.size(); ++i) EXPECT_EQ(levels[i], i < line.size() / 2 ? 0 : 1);
  const auto order = visualOrder(levels);
  EXPECT_EQ(order[line.size() / 2], line.size() - 1);
  EXPECT_EQ(order.back(), line.size() / 2);
}

TEST(NativeTextBidi, UnalignedScratchUsesOnlyAdvertisedBytes) {
  const auto line = logical({0x05d0, '(', 0x05d1, ')'});
  const size_t bytes = bidi_scratch_size(line.size());
  std::vector<uchar> storage(bytes + 2, 0xa5);
  std::vector<uchar> levels(line.size(), 0xff);
  ASSERT_EQ(resolve_bidi_levels(true, 0, line.data(), static_cast<int>(line.size()), levels.data(), storage.data() + 1,
                                bytes),
            1);
  EXPECT_EQ(levels, (std::vector<uchar>{1, 1, 1, 1}));
  EXPECT_EQ(storage.front(), 0xa5);
  EXPECT_EQ(storage.back(), 0xa5);
}

TEST(NativeTextBidi, RejectsInsufficientWorkspaceAndOverlappingBuffers) {
  const auto line = logical({'A', 0x05d0});
  const size_t bytes = bidi_scratch_size(line.size());
  std::vector<uchar> storage(bytes);
  std::vector<uchar> levels(line.size(), 0xfe);
  EXPECT_EQ(resolve_bidi_levels(true, 0, line.data(), 2, levels.data(), storage.data(), bytes - 1), -1);
  EXPECT_EQ(levels, (std::vector<uchar>{0xfe, 0xfe}));
  EXPECT_EQ(resolve_bidi_levels(true, 0, line.data(), 2, storage.data(), storage.data(), bytes), -1);
  EXPECT_EQ(resolve_bidi_levels(true, 0, line.data(), 2, levels.data(), nullptr, bytes), -1);
  EXPECT_EQ(resolve_bidi_levels(true, 0, nullptr, 2, levels.data(), storage.data(), bytes), -1);
  EXPECT_EQ(resolve_bidi_levels(true, 0, line.data(), 2, nullptr, storage.data(), bytes), -1);
}

TEST(NativeTextBidi, RejectsUnsupportedCountsWithoutAccessingBuffers) {
  EXPECT_EQ(bidi_scratch_size(BIDI_MAX_NATIVE_LINE + 1), 0u);
  EXPECT_EQ(bidi_scratch_size(std::numeric_limits<size_t>::max()), 0u);
  EXPECT_EQ(resolve_bidi_levels(true, 0, nullptr, BIDI_MAX_NATIVE_LINE + 1, nullptr, nullptr, 0), -1);
  EXPECT_EQ(resolve_bidi_levels(true, 0, nullptr, -1, nullptr, nullptr, 0), -1);
  EXPECT_EQ(resolve_bidi_levels(true, -1, nullptr, 0, nullptr, nullptr, 0), -1);
  EXPECT_EQ(resolve_bidi_levels(false, 2, nullptr, 0, nullptr, nullptr, 0), -1);
  EXPECT_EQ(resolve_bidi_levels(true, 0, nullptr, 0, nullptr, nullptr, 0), 0);
  EXPECT_EQ(resolve_bidi_levels(true, 1, nullptr, 0, nullptr, nullptr, 0), 1);
}

TEST(NativeTextBidi, LegacyStillProcessesOnlyItsOriginalBound) {
  std::vector<bidi_char> line(BIDI_MAX_LINE + 1);
  for (size_t i = 0; i < line.size(); ++i) line[i] = {0x05d0, 0x05d0, static_cast<uint16_t>(i), 0};
  ASSERT_EQ(do_bidi(true, 0, line.data(), static_cast<int>(line.size())), 1);
  EXPECT_EQ(line.front().index, BIDI_MAX_LINE - 1);
  EXPECT_EQ(line[BIDI_MAX_LINE - 1].index, 0);
  EXPECT_EQ(line.back().index, BIDI_MAX_LINE);
}
