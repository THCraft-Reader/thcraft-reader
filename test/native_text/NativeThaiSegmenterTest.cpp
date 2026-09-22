#include <NativeAllocator.h>
#include <NativeUtf8.h>
#include <ThaiSegmenter.h>
#include <gtest/gtest.h>
#include <thai/thbrk.h>
#include <thai/thwchar.h>

#include <array>
#include <limits>
#include <string>
#include <vector>

TEST(NativeTextThaiSegmenter, MapsPinnedDictionaryPositionsToOriginalUtf8Bytes) {
  ThaiSegmenter segmenter;
  ASSERT_EQ(segmenter.initialize(), TextStatus::Ok);
  ThBrk* reference = th_brk_new(NATIVE_DICTIONARY_PATH);
  ASSERT_NE(reference, nullptr);
  for (std::string_view text : {"ภาษาไทย", "ประเทศไทย", "ภาษาไทย EPUB 123 กี่", "๑๒๓ภาษาไทย", "กขฃคฅฆงจฉชซฌ"}) {
    std::array<uint8_t, 128> tis{};
    std::array<uint32_t, 128> mapping{}, actual{};
    size_t offset = 0, length = 0, count = 0;
    uint32_t cp = 0;
    while (offset < text.size()) {
      mapping[length] = static_cast<uint32_t>(offset);
      ASSERT_TRUE(native_text::nextUtf8(text, offset, cp));
      tis[length++] = th_uni2tis(cp);
    }
    mapping[length] = static_cast<uint32_t>(offset);
    std::array<int, 128> expected{};
    const int expectedCount = th_brk_find_breaks(reference, tis.data(), expected.data(), expected.size());
    ASSERT_EQ(segmenter.findBreaks(text, actual, count), TextStatus::Ok);
    ASSERT_EQ(count, static_cast<size_t>(expectedCount));
    for (size_t i = 0; i < count; ++i) {
      EXPECT_EQ(actual[i], mapping[expected[i]]);
      EXPECT_TRUE(native_text::utf8Boundary(text, actual[i]));
    }
  }
  th_brk_delete(reference);
}

TEST(NativeTextThaiSegmenter, EmptyMalformedAndCapacityHaveDistinctResults) {
  ThaiSegmenter segmenter;
  ASSERT_EQ(segmenter.initialize(), TextStatus::Ok);
  std::array<uint32_t, 128> breaks{};
  size_t count = 99;
  EXPECT_EQ(segmenter.findBreaks({}, breaks, count), TextStatus::Ok);
  EXPECT_EQ(count, 0u);
  EXPECT_EQ(segmenter.findBreaks(std::string_view("\xe0\xb8", 2), breaks, count), TextStatus::InvalidText);
  EXPECT_EQ(count, 0u);
  EXPECT_EQ(segmenter.findBreaks("ภาษาไทย", {}, count), TextStatus::CapacityExceeded);
  EXPECT_EQ(count, 0u);
}

TEST(NativeTextThaiSegmenter, AllocationFailureCannotBecomeNoWordBoundaries) {
  const auto before = native_text::allocationStats().used;
  {
    ThaiSegmenter segmenter;
    ASSERT_EQ(segmenter.initialize(), TextStatus::Ok);
    std::array<uint32_t, 128> breaks{};
    size_t count = 0;
    native_text::failAllocationsAfter(0);
    EXPECT_EQ(segmenter.findBreaks("ภาษาไทย", breaks, count), TextStatus::OutOfMemory);
    EXPECT_EQ(count, 0u);
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    EXPECT_EQ(segmenter.findBreaks("ภาษาไทย", breaks, count), TextStatus::Ok);
    EXPECT_GT(count, 0u);
  }
  EXPECT_EQ(native_text::allocationStats().used, before);
}

TEST(NativeTextThaiSegmenter, EmergencyHintsProtectLeadingVowelsAndMarks) {
  ThaiSegmenter segmenter;
  ASSERT_EQ(segmenter.initialize(), TextStatus::Ok);
  const std::string_view text = "เก่งเก่งกี่กี่";
  std::array<uint32_t, 128> breaks{};
  size_t count = 0;
  ASSERT_EQ(segmenter.findEmergencyBreaks(text, breaks, count), TextStatus::Ok);
  for (size_t i = 0; i < count; ++i) {
    const size_t boundary = breaks[i];
    EXPECT_TRUE(native_text::utf8Boundary(text, boundary));
    size_t offset = boundary;
    uint32_t cp = 0;
    ASSERT_TRUE(native_text::nextUtf8(text, offset, cp));
    EXPECT_FALSE((cp >= 0x0e48 && cp <= 0x0e4e) || cp == 0x0e34);
    if (boundary >= 3) {
      offset = boundary - 3;
      ASSERT_TRUE(native_text::nextUtf8(text, offset, cp));
      EXPECT_FALSE(cp >= 0x0e40 && cp <= 0x0e44);
    }
  }
}
