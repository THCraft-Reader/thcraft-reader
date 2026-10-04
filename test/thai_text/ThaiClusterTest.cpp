#include <ThaiCluster.h>
#include <ThaiConfig.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Span {
  size_t begin;
  size_t end;
  uint16_t codepoints;
  bool valid;

  bool operator==(const Span& other) const {
    return begin == other.begin && end == other.end && codepoints == other.codepoints && valid == other.valid;
  }
};

// The consumer keeps precisely the unconsumed suffix between callbacks. Offsets
// returned by the analyzer are local byte offsets, translated here for comparison.
void drain(std::string_view input, bool endOfRun, size_t base, std::vector<Span>& spans, size_t& consumed) {
  consumed = 0;
  while (consumed < input.size()) {
    const size_t previous = consumed;
    thai::Cluster cluster{};
    if (!thai::nextCluster(input, consumed, cluster, endOfRun)) {
      EXPECT_EQ(consumed, previous) << "An undecidable suffix must remain available to its caller";
      EXPECT_FALSE(endOfRun) << "Final input must make bounded progress";
      return;
    }
    EXPECT_EQ(cluster.begin, previous);
    EXPECT_EQ(cluster.end, consumed);
    if (consumed <= previous || consumed > input.size()) {
      ADD_FAILURE() << "Invalid progress from " << previous << " to " << consumed;
      return;
    }
    EXPECT_GE(cluster.codepoints, 1u);
    EXPECT_LE(cluster.codepoints, thai::MAX_CLUSTER_CODEPOINTS);
    spans.push_back({base + cluster.begin, base + cluster.end, cluster.codepoints, cluster.valid});
  }
}

std::vector<Span> whole(std::string_view input) {
  std::vector<Span> result;
  result.reserve(input.size());
  size_t consumed = 0;
  drain(input, true, 0, result, consumed);
  EXPECT_EQ(consumed, input.size());
  return result;
}

std::vector<Span> streamed(std::string_view input, const std::vector<size_t>& chunkEnds) {
  std::vector<Span> result;
  result.reserve(input.size());
  std::string pending;
  size_t received = 0;
  size_t committed = 0;
  for (size_t end : chunkEnds) {
    if (end < received || end > input.size()) {
      ADD_FAILURE() << "Invalid test callback boundary";
      return result;
    }
    pending.append(input.substr(received, end - received));
    received = end;
    size_t consumed = 0;
    drain(pending, false, committed, result, consumed);
    pending.erase(0, consumed);
    committed += consumed;
    EXPECT_LE(pending.size(), thai::MAX_CLUSTER_CODEPOINTS * 4u)
        << "The consumer must never need an unbounded retained suffix";
  }
  EXPECT_EQ(received, input.size());
  size_t consumed = 0;
  drain(pending, true, committed, result, consumed);
  EXPECT_EQ(consumed, pending.size());
  EXPECT_EQ(committed + consumed, input.size());
  return result;
}

void expectClusters(std::string_view text, const std::vector<std::string_view>& expected) {
  const auto spans = whole(text);
  ASSERT_EQ(spans.size(), expected.size()) << text;
  size_t position = 0;
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_TRUE(spans[i].valid);
    EXPECT_EQ(spans[i].begin, position);
    EXPECT_EQ(spans[i].end, position + expected[i].size());
    EXPECT_EQ(text.substr(spans[i].begin, spans[i].end - spans[i].begin), expected[i]);
    // All expected fixtures here contain exclusively three-byte Thai scalars.
    EXPECT_EQ(spans[i].codepoints, expected[i].size() / 3);
    position += expected[i].size();
  }
  EXPECT_EQ(position, text.size());
}

std::vector<size_t> justificationBoundaries(std::string_view text) {
  thai::JustificationBoundaryCursor cursor(text);
  std::vector<size_t> boundaries;
  size_t offset = text.size() + 1;
  while (cursor.next(offset)) boundaries.push_back(offset);
  const auto finalOffset = offset;
  EXPECT_FALSE(cursor.next(offset));
  EXPECT_EQ(offset, finalOffset);
  return boundaries;
}

void expectSpacingUnits(std::string_view text, const std::vector<std::string_view>& units) {
  std::vector<size_t> expected;
  size_t offset = 0;
  for (const auto unit : units) {
    if (offset) expected.push_back(offset);
    EXPECT_EQ(text.substr(offset, unit.size()), unit);
    offset += unit.size();
  }
  ASSERT_EQ(offset, text.size());
  EXPECT_EQ(justificationBoundaries(text), expected);
}

TEST(ThaiClusterTest, JustificationSeparatesLetterUnitsWithoutSeparatingMarks) {
  expectSpacingUnits("กขค", {"ก", "ข", "ค"});
  const std::string_view marked = "กี่น้ำเพื่อญูฐุนํ้า";
  expectSpacingUnits(marked, {"กี่", "น้ำ", "เ", "พื่", "อ", "ญู", "ฐุ", "นํ้า"});
  for (const auto& span : whole(marked)) {
    const thai::Cluster cluster{span.begin, span.end, span.codepoints, span.valid};
    EXPECT_TRUE(thai::isJustifiableLetterCluster(marked, cluster));
  }
  expectSpacingUnits("ภาษาไทย", {"ภ", "า", "ษ", "า", "ไ", "ท", "ย"});
  expectSpacingUnits("ประเทศไทย", {"ป", "ร", "ะ", "เ", "ท", "ศ", "ไ", "ท", "ย"});
  expectSpacingUnits("เรื่อง", {"เ", "รื่", "อ", "ง"});
}

TEST(ThaiClusterTest, InteriorSpacingDoesNotCreateLineBreaks) {
  for (const std::string_view text : {"เพื่อ", "เก่", "เรื่อ", "ตั้ง", "ไก่"}) {
    SCOPED_TRACE(text);
    const auto boundaries = justificationBoundaries(text);
    ASSERT_FALSE(boundaries.empty());
    expectClusters(text, {text});
    for (const size_t boundary : boundaries) {
      EXPECT_EQ(thai::lastSafeBoundary(text, boundary, true), 0u);
      EXPECT_FALSE(thai::isCombiningSign(thai::detail::decode(text, boundary, true).value));
    }
  }
  expectSpacingUnits("เพื่อ", {"เ", "พื่", "อ"});
  expectSpacingUnits("เก่", {"เ", "ก่"});
  expectSpacingUnits("ตั้ง", {"ตั้", "ง"});
}

TEST(ThaiClusterTest, RecipesRemainRigidAndMalformedStacksCannotExpand) {
  for (const std::string_view recipe : {"กี่", "กุ่", "น้ำ", "นํ้า", "ญู", "ฐุ", "กํ่", "ก็่"}) {
    SCOPED_TRACE(recipe);
    EXPECT_TRUE(justificationBoundaries(recipe).empty());
    const std::string text = std::string(recipe) + "ข";
    expectSpacingUnits(text, {recipe, "ข"});
  }
  for (const std::string_view malformed : {"ก่่", "กีี", "ก่ี", "กํุ่", "เกี่่", "นํ้้า"}) {
    SCOPED_TRACE(malformed);
    EXPECT_TRUE(justificationBoundaries(malformed).empty());
    const std::string text = std::string(malformed) + "ขค";
    EXPECT_EQ(justificationBoundaries(text), (std::vector<size_t>{text.size() - std::string_view("ค").size()}));
  }
}

TEST(ThaiClusterTest, JustificationExcludesSymbolsDigitsScriptsAndWhitespace) {
  for (const std::string_view separator :
       {"๑", "๙", "ๆ", "ฯ", "฿", "๏", "๚", "๛", ".", " ", "\xC2\xA0", "A", "中", "א"}) {
    SCOPED_TRACE(separator);
    const std::string text = std::string("กข") + std::string(separator) + "คง";
    EXPECT_EQ(justificationBoundaries(text), (std::vector<size_t>{3, 9 + separator.size()}));
  }
  for (const std::string_view fragment : {"", "เ", "า", "่", "่ก", "ํ้เก่"}) {
    SCOPED_TRACE(fragment);
    EXPECT_TRUE(justificationBoundaries(fragment).empty());
    size_t offset = 0;
    thai::Cluster cluster{};
    if (thai::nextCluster(fragment, offset, cluster, true))
      EXPECT_FALSE(thai::isJustifiableLetterCluster(fragment, cluster));
  }
  EXPECT_EQ(justificationBoundaries("่กขค"), (std::vector<size_t>{9}));
}

TEST(ThaiClusterTest, JustificationRecoversAfterMalformedAndPathologicalSpans) {
  for (const std::string& invalid : {std::string("\xFF"), std::string("\xE0\xB8"), std::string("\0", 1)}) {
    const std::string text = std::string("กข") + invalid + "คง";
    EXPECT_EQ(justificationBoundaries(text), (std::vector<size_t>{3, 9 + invalid.size()}));
  }
  std::string pathological = "ก";
  for (size_t i = 0; i < thai::MAX_CLUSTER_CODEPOINTS * 3; ++i) pathological += "่";
  const std::string text = std::string("ขค!") + pathological + "!งจ";
  EXPECT_EQ(justificationBoundaries(text), (std::vector<size_t>{3, text.size() - 3}));
  EXPECT_FALSE(thai::isJustifiableLetterCluster("ก", {0, 0, 0, true}));
  EXPECT_FALSE(thai::isJustifiableLetterCluster("ก", {0, 4, 1, true}));
  EXPECT_FALSE(thai::isJustifiableLetterCluster("ก", {0, 2, 1, true}));
  EXPECT_FALSE(thai::isJustifiableLetterCluster("ก", {0, 3, 1, false}));
}

TEST(ThaiClusterTest, SuppliedMarkedClustersHaveNoInteriorBoundary) {
  for (std::string_view cluster : {"ก่",  "ก้",  "ก๊",  "ก๋",  "กิ",   "กี",   "กึ", "กื", "กุ", "กู", "กี่", "กุ่",
                                   "น้ำ", "นํ้า", "ตั้ง", "เก่", "เรื่อ", "เพื่อ", "ญู", "ฐุ", "ปี่", "ฝี่", "ฟี่"}) {
    SCOPED_TRACE(cluster);
    expectClusters(cluster, {cluster});
    for (size_t limit = 0; limit < cluster.size(); ++limit) {
      EXPECT_EQ(thai::lastSafeBoundary(cluster, limit, true), 0u) << "limit=" << limit;
    }
    EXPECT_EQ(thai::lastSafeBoundary(cluster, cluster.size(), true), cluster.size());
  }
}

TEST(ThaiClusterTest, SuppliedWordsExposeOrthographicNotDictionaryBoundaries) {
  expectClusters("เก่ง", {"เก่", "ง"});
  expectClusters("เรื่อง", {"เรื่อ", "ง"});
  expectClusters("เพื่อ", {"เพื่อ"});
  expectClusters("อ่าน", {"อ่า", "น"});
  expectClusters("ผู้หญิง", {"ผู้", "ห", "ญิ", "ง"});
  expectClusters("ประเทศไทย", {"ป", "ระ", "เท", "ศ", "ไท", "ย"});
  expectClusters("โรงพยาบาล", {"โร", "ง", "พ", "ยา", "บา", "ล"});
  expectClusters("หนังสือ", {"ห", "นั", "ง", "สือ"});
  expectClusters("ภาษาไทย", {"ภา", "ษา", "ไท", "ย"});
}

TEST(ThaiClusterTest, FiniteTccAlternativesRetainPriorityAndGreediness) {
  // Independently fixed outputs of the ordered grammar at PyThaiNLP commit
  // 4be114097e0cb1d9cfe044691f2aa79bc294925e, with dependent-sign coalescing.
  // The first c[ั] alternative must win over the following suffix-bearing one.
  expectClusters("กั่กก์", {"กั่ก", "ก์"});
  for (std::string_view cluster : {"เก็กก์", "เกกาะ", "เกกียะ", "เกก็ก", "เกิก์ก", "เกิ่ก", "เกียะ", "เกื่อะ", "กื่ง", "กรรค์", "แก็ก",
                                   "แกก์", "แก่ะ", "แกก็ก", "แกกก์", "โก่ะ", "ไก่", "เกากกุ์"}) {
    SCOPED_TRACE(cluster);
    expectClusters(cluster, {cluster});
  }
  expectClusters("เกกียก", {"เกกีย", "ก"});
  expectClusters("เกิยก", {"เกิย", "ก"});
  expectClusters("เกกีย", {"เกกีย"});
  expectClusters("เกกียๆ", {"เก", "กี", "ย", "ๆ"});
  expectClusters("เรือะ", {"เรือะ"});
  expectClusters("เรื่อะ", {"เรื่อะ"});
}

TEST(ThaiClusterTest, EveryLeadingVowelStaysWithFollowingBaseAndSigns) {
  for (std::string_view cluster : {"เก่", "แก่", "โก่", "ใก่", "ไก่"}) {
    SCOPED_TRACE(cluster);
    expectClusters(cluster, {cluster});
    for (size_t bytes = 1; bytes <= cluster.size(); ++bytes) {
      size_t offset = 0;
      thai::Cluster result{};
      EXPECT_FALSE(thai::nextCluster(cluster.substr(0, bytes), offset, result, false));
      EXPECT_EQ(offset, 0u);
    }
  }
}

TEST(ThaiClusterTest, ComposedAndDecomposedSaraAmKeepOriginalBytes) {
  expectClusters("น้ำ", {"น้ำ"});
  expectClusters("นํ้า", {"นํ้า"});
  expectClusters("นํา", {"นํา"});
  expectClusters("น้ํา", {"น้ํา"});
  expectClusters("นํ้ากี่", {"นํ้า", "กี่"});
}

TEST(ThaiClusterTest, OrphanAndRepeatedSignsRemainWithFollowingOrPrecedingCluster) {
  expectClusters("่กี่", {"่กี่"});
  expectClusters("ํ้เก่", {"ํ้เก่"});
  expectClusters("ก่่่", {"ก่่่"});
  expectClusters("่่", {"่่"});
  std::string atBound = "ก";
  for (size_t i = 1; i < thai::MAX_CLUSTER_CODEPOINTS; ++i) atBound += "่";
  expectClusters(atBound, {atBound});
  size_t offset = 0;
  thai::Cluster result{};
  EXPECT_FALSE(thai::nextCluster(atBound, offset, result, false));
  EXPECT_EQ(offset, 0u);
}

TEST(ThaiClusterTest, SafeBoundaryUsesByteLimitsAndNeverCutsAProtectedSpan) {
  const std::string_view text = "กี่เก่งน้ำ";
  const std::array<size_t, 5> boundaries{0, 9, 18, 21, 30};
  for (size_t limit = 0; limit <= text.size() + 3; ++limit) {
    size_t expected = 0;
    for (size_t boundary : boundaries) {
      if (boundary <= limit) expected = boundary;
    }
    EXPECT_EQ(thai::lastSafeBoundary(text, limit, true), expected) << "limit=" << limit;
  }
  EXPECT_EQ(thai::lastSafeBoundary("กี่", 9, false), 0u);
}

TEST(ThaiClusterTest, EveryByteSplitMatchesWholeInputAndRetainsUndecidableSuffix) {
  const std::vector<std::string> inputs{"ก่ก้ก๊ก๋กิกีกึกืกุกูกี่กุ่",
                                        "เก่งน้ำตั้งเรื่องอ่านผู้หญิงประเทศไทยโรงพยาบาลหนังสือภาษาไทย",
                                        "เพื่อเรื่อะนํ้าน้ํา่่",
                                        "กั่กก์เกากกุ์เกกียกเกิยก",
                                        "เกกีย!",
                                        "เกกียๆ",
                                        "่กี่ํ้เก่",
                                        "EpubCraft เป็น EPUB Editor 4.2.0 02:40",
                                        "“สวัสดีครับ” คุณทำอะไรอยู่? ประเทศไทย...แล้วอย่างไรต่อ",
                                        "กี่\xE2\x80\x8Bน้ำ"};
  for (const auto& input : inputs) {
    SCOPED_TRACE(input);
    const auto expected = whole(input);
    for (size_t split = 0; split <= input.size(); ++split) {
      SCOPED_TRACE(split);
      EXPECT_EQ(streamed(input, {split, input.size()}), expected);
    }
    std::vector<size_t> byteEnds;
    byteEnds.reserve(input.size());
    for (size_t end = 1; end <= input.size(); ++end) byteEnds.push_back(end);
    EXPECT_EQ(streamed(input, byteEnds), expected);
  }
}

TEST(ThaiClusterTest, CallbackAndBufferSizedChunksDoNotChangeBoundaries) {
  std::string input;
  input.reserve(4096);
  while (input.size() < 3072) input += "เก่งนํ้าเรื่องประเทศไทย";
  const auto expected = whole(input);
  for (size_t chunk : {size_t{1}, size_t{2}, size_t{3}, size_t{198}, size_t{199}, size_t{200}, size_t{201}, size_t{202},
                       size_t{767}, size_t{768}, size_t{769}}) {
    SCOPED_TRACE(chunk);
    std::vector<size_t> ends;
    ends.reserve(input.size() / chunk + 1);
    for (size_t end = chunk; end < input.size(); end += chunk) ends.push_back(end);
    ends.push_back(input.size());
    EXPECT_EQ(streamed(input, ends), expected);
  }
}

TEST(ThaiClusterTest, EmptyAndNonterminatedViewsAreEndBounded) {
  size_t offset = 0;
  thai::Cluster result{};
  EXPECT_FALSE(thai::nextCluster({}, offset, result, true));
  EXPECT_FALSE(thai::nextCluster({}, offset, result, false));
  EXPECT_EQ(offset, 0u);
  EXPECT_FALSE(thai::containsThai({}));
  EXPECT_EQ(thai::lastSafeBoundary({}, 10, true), 0u);

  const std::array<char, 3> exact{char(0xE0), char(0xB8), char(0x81)};
  const std::string_view base(exact.data(), exact.size());
  expectClusters(base, {"ก"});
  EXPECT_TRUE(thai::containsThai(base));
  EXPECT_FALSE(thai::containsThai(base.substr(0, 2)));
  const std::array<char, 5> withNull{'A', '\0', char(0xE0), char(0xB8), char(0x81)};
  const std::string_view bounded(withNull.data(), withNull.size());
  EXPECT_FALSE(thai::containsThai(bounded.substr(0, 2)));
  EXPECT_TRUE(thai::containsThai(bounded));
  const auto spans = whole(bounded);
  ASSERT_EQ(spans.size(), 3u);
  EXPECT_EQ(spans[0].begin, 0u);
  EXPECT_EQ(spans[0].end, 1u);
  EXPECT_EQ(spans[1].begin, 1u);
  EXPECT_EQ(spans[1].end, 2u);
  EXPECT_EQ(spans[2].begin, 2u);
  EXPECT_EQ(spans[2].end, 5u);
}

TEST(ThaiClusterTest, InvalidUtf8IsPreservedAndNeverReportedAsValidThai) {
  const std::vector<std::string> invalid{std::string("\x80", 1),         std::string("\xC0\xAF", 2),
                                         std::string("\xED\xA0\x80", 3), std::string("\xF4\x90\x80\x80", 4),
                                         std::string("\xF5\xFF", 2),     std::string("\xE0\x28\xA1", 3)};
  for (const auto& bytes : invalid) {
    const std::string input = bytes + "กี่";
    const auto spans = whole(input);
    std::string preserved;
    bool rejected = false;
    for (const auto& span : spans) {
      preserved.append(input, span.begin, span.end - span.begin);
      if (span.begin < bytes.size() && static_cast<unsigned char>(input[span.begin]) >= 0x80) {
        EXPECT_FALSE(span.valid);
        rejected = true;
      }
    }
    EXPECT_EQ(preserved, input);
    EXPECT_TRUE(rejected);
    ASSERT_FALSE(spans.empty());
    EXPECT_EQ(spans.back().begin, bytes.size());
    EXPECT_EQ(spans.back().end, input.size());
    EXPECT_TRUE(spans.back().valid);
    for (size_t split = 0; split <= input.size(); ++split) {
      EXPECT_EQ(streamed(input, {split, input.size()}), spans) << "split=" << split;
    }
    EXPECT_FALSE(thai::containsThai(bytes));
  }
}

TEST(ThaiClusterTest, TruncatedUtf8WaitsUntilEofThenPreservesEveryByte) {
  for (const std::string bytes : {std::string("\xE0", 1), std::string("\xE0\xB8", 2), std::string("\xF0\x9F\x98", 3)}) {
    size_t offset = 0;
    thai::Cluster cluster{};
    EXPECT_FALSE(thai::nextCluster(bytes, offset, cluster, false));
    EXPECT_EQ(offset, 0u);
    const auto spans = whole(bytes);
    std::string preserved;
    for (const auto& span : spans) {
      EXPECT_FALSE(span.valid);
      preserved.append(bytes, span.begin, span.end - span.begin);
    }
    EXPECT_EQ(preserved, bytes);
    EXPECT_EQ(streamed(bytes, {bytes.size()}), spans);
  }
}

TEST(ThaiClusterTest, ThousandRepeatedSignsMakeBoundedProgressAndPreserveSource) {
  std::string input = "ก";
  for (size_t i = 0; i < 1000; ++i) input += "่";
  const size_t malformedEnd = input.size();
  input += "เก่ง";

  size_t offset = 0;
  thai::Cluster first{};
  ASSERT_TRUE(thai::nextCluster(input, offset, first, false));
  EXPECT_FALSE(first.valid);
  EXPECT_EQ(first.begin, 0u);
  EXPECT_EQ(first.codepoints, thai::MAX_CLUSTER_CODEPOINTS);
  EXPECT_EQ(first.end, thai::MAX_CLUSTER_CODEPOINTS * 3u);
  const auto spans = whole(input);
  std::string preserved;
  for (const auto& span : spans) preserved.append(input, span.begin, span.end - span.begin);
  EXPECT_EQ(preserved, input);

  // The residual orphan signs may attach to เก่, but the following normal
  // Thai must again be valid and neither normal protected span may be cut.
  ASSERT_GE(spans.size(), 2u);
  const auto& finalCluster = spans.back();
  EXPECT_TRUE(finalCluster.valid);
  EXPECT_EQ(finalCluster.begin, malformedEnd + std::string_view("เก่").size());
  EXPECT_EQ(finalCluster.end, input.size());
  const auto& resumedCluster = spans[spans.size() - 2];
  EXPECT_TRUE(resumedCluster.valid);
  EXPECT_LE(resumedCluster.begin, malformedEnd);
  EXPECT_EQ(resumedCluster.end, finalCluster.begin);

  std::vector<size_t> ends;
  ends.reserve(input.size());
  for (size_t end = 1; end <= input.size(); ++end) ends.push_back(end);
  EXPECT_EQ(streamed(input, ends), spans);
}

}  // namespace
