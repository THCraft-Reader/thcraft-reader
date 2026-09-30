#include <ThaiCluster.h>
#include <ThaiDictionary.h>
#include <ThaiSegmenter.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using thai::BreakKind;

// Real compressed dictionary bytes, not a lookup mock. The backing vectors live
// longer than each DictionaryView consumer, including malformed-view tests.
class DictionaryFixture {
 public:
  explicit DictionaryFixture(std::vector<std::string> words) : wordCount(words.size()) {
    std::sort(words.begin(), words.end());
    std::vector<uint8_t> previous;
    for (size_t i = 0; i < words.size(); ++i) {
      std::vector<uint8_t> symbols;
      for (size_t offset = 0; offset < words[i].size();) {
        const auto scalar = thai::detail::decode(words[i], offset, true);
        EXPECT_TRUE(scalar.valid);
        EXPECT_GE(scalar.value, 0x0E01u);
        EXPECT_LE(scalar.value, 0x0E5Bu);
        symbols.push_back(static_cast<uint8_t>(scalar.value - 0x0E00));
        offset += scalar.bytes;
      }
      size_t prefix = 0;
      if (i % thai::DICTIONARY_BLOCK_WORDS == 0) offsets.push_back(static_cast<uint32_t>(data.size()));
      else {
        while (prefix < previous.size() && prefix < symbols.size() && previous[prefix] == symbols[prefix]) ++prefix;
      }
      data.push_back(static_cast<uint8_t>(prefix));
      data.push_back(static_cast<uint8_t>(symbols.size() - prefix));
      data.insert(data.end(), symbols.begin() + prefix, symbols.end());
      previous = std::move(symbols);
    }
    offsets.push_back(static_cast<uint32_t>(data.size()));
  }

  thai::DictionaryView view() const {
    return {data.data(), data.size(), offsets.data(), offsets.size(), wordCount, 0};
  }

  std::vector<uint8_t> data;
  std::vector<uint32_t> offsets;
  size_t wordCount;
};

struct Span {
  size_t begin;
  size_t end;
  uint16_t codepoints;
  BreakKind before;
  BreakKind after;
  bool known;
  bool valid;

  bool operator==(const Span& other) const {
    return begin == other.begin && end == other.end && codepoints == other.codepoints && before == other.before &&
           after == other.after && known == other.known && valid == other.valid;
  }
};

void drain(std::string_view text, bool final, size_t base, const thai::ThaiDictionary& dictionary,
           std::vector<Span>& output, size_t& consumed) {
  consumed = 0;
  while (consumed < text.size()) {
    const size_t previous = consumed;
    thai::Segment segment{9, 10, 11, BreakKind::Space, BreakKind::Prohibited, true, false};
    if (!thai::nextSegment(text, consumed, segment, final, dictionary)) {
      EXPECT_EQ(consumed, previous);
      EXPECT_EQ(segment.begin, 9u);
      EXPECT_EQ(segment.end, 10u);
      EXPECT_EQ(segment.codepoints, 11u);
      EXPECT_EQ(segment.before, BreakKind::Space);
      EXPECT_EQ(segment.after, BreakKind::Prohibited);
      EXPECT_TRUE(segment.known);
      EXPECT_FALSE(segment.valid);
      EXPECT_FALSE(final) << "Final input must make progress";
      return;
    }
    if (consumed <= previous || consumed > text.size()) {
      ADD_FAILURE() << "Invalid progress from " << previous << " to " << consumed;
      return;
    }
    EXPECT_EQ(segment.begin, previous);
    EXPECT_EQ(segment.end, consumed);
    EXPECT_GE(segment.codepoints, 1u);
    output.push_back({base + segment.begin, base + segment.end, segment.codepoints, segment.before, segment.after,
                      segment.known, segment.valid});
  }
}

std::vector<Span> whole(std::string_view text, const thai::ThaiDictionary& dictionary) {
  std::vector<Span> result;
  size_t consumed = 0;
  drain(text, true, 0, dictionary, result, consumed);
  EXPECT_EQ(consumed, text.size());
  return result;
}

std::vector<Span> streamed(std::string_view text, const std::vector<size_t>& chunkEnds,
                           const thai::ThaiDictionary& dictionary) {
  std::vector<Span> result;
  std::string pending;
  size_t received = 0;
  size_t committed = 0;
  for (size_t end : chunkEnds) {
    if (end < received || end > text.size()) {
      ADD_FAILURE() << "Invalid fixture chunk boundary";
      return result;
    }
    pending.append(text.substr(received, end - received));
    received = end;
    size_t consumed = 0;
    drain(pending, false, committed, dictionary, result, consumed);
    pending.erase(0, consumed);
    committed += consumed;
    EXPECT_LE(pending.size(), (thai::MAX_DICTIONARY_WORD_CODEPOINTS + thai::MAX_CLUSTER_CODEPOINTS) * 4u);
  }
  EXPECT_EQ(received, text.size());
  size_t consumed = 0;
  drain(pending, true, committed, dictionary, result, consumed);
  EXPECT_EQ(consumed, pending.size());
  EXPECT_EQ(committed + consumed, text.size());
  return result;
}

std::vector<std::string_view> pieces(std::string_view input, const std::vector<Span>& spans) {
  std::vector<std::string_view> result;
  size_t position = 0;
  for (const auto& span : spans) {
    EXPECT_EQ(span.begin, position);
    result.push_back(input.substr(span.begin, span.end - span.begin));
    position = span.end;
  }
  EXPECT_EQ(position, input.size());
  return result;
}

std::string repeated(std::string_view text, size_t count) {
  std::string result;
  result.reserve(text.size() * count);
  while (count--) result += text;
  return result;
}

void expectKnownWords(std::string_view text, const thai::ThaiDictionary& dictionary,
                      const std::vector<std::string_view>& expected) {
  const auto spans = whole(text, dictionary);
  EXPECT_EQ(pieces(text, spans), expected);
  for (const auto& span : spans) {
    EXPECT_TRUE(span.known);
    EXPECT_TRUE(span.valid);
    EXPECT_EQ(span.before, BreakKind::Word);
    EXPECT_EQ(span.after, BreakKind::Word);
    EXPECT_EQ(span.codepoints, (span.end - span.begin) / 3);
  }
}

#if THAI_DICTIONARY
TEST(ThaiSegmenterTest, ProductionLongestMatchKeepsCompoundAndOffersItsLexicalSplit) {
  const thai::ThaiDictionary dictionary;
  ASSERT_TRUE(dictionary.available());
  ASSERT_TRUE(dictionary.valid());
  expectKnownWords("ประเทศไทยมีประชากรจำนวนมาก", dictionary, {"ประเทศไทย", "มี", "ประชากร", "จำนวนมาก"});
  const std::string_view compound = "จำนวนมาก";
  const size_t prefix = std::string_view("จำนวน").size();
  EXPECT_EQ(thai::dictionarySplit(compound, prefix, dictionary), prefix);
  EXPECT_EQ(thai::dictionarySplit(compound, compound.size(), dictionary), prefix);
}
#endif

TEST(ThaiSegmenterTest, TinyReviewedLexiconProducesFivePrimaryWords) {
  const DictionaryFixture fixture({"ประเทศไทย", "มี", "ประชากร", "จำนวน", "มาก"});
  const thai::ThaiDictionary dictionary(fixture.view());
  expectKnownWords("ประเทศไทยมีประชากรจำนวนมาก", dictionary, {"ประเทศไทย", "มี", "ประชากร", "จำนวน", "มาก"});
}

TEST(ThaiSegmenterTest, WordTerminalWaitsForMaximumWordAndClusterLookahead) {
  const std::string longest = repeated("ก", thai::MAX_DICTIONARY_WORD_CODEPOINTS);
  const DictionaryFixture fixture({"ก", longest});
  const thai::ThaiDictionary dictionary(fixture.view());
  const std::string pending = longest + repeated("ข", thai::MAX_CLUSTER_CODEPOINTS - 1);
  size_t offset = 0;
  thai::Segment segment{};
  EXPECT_FALSE(thai::nextSegment(pending, offset, segment, false, dictionary));
  EXPECT_EQ(offset, 0u);
  const std::string decidable = pending + "ข";
  ASSERT_TRUE(thai::nextSegment(decidable, offset, segment, false, dictionary));
  EXPECT_EQ(offset, longest.size());
  EXPECT_EQ(segment.codepoints, thai::MAX_DICTIONARY_WORD_CODEPOINTS);
  EXPECT_TRUE(segment.known);
  expectKnownWords(longest, dictionary, {longest});
  expectKnownWords("ก", dictionary, {"ก"});
}

TEST(ThaiSegmenterTest, RealRunTransitionsCommitWithoutFillingTheThaiWindow) {
  const DictionaryFixture fixture({"ก", "กข"});
  const thai::ThaiDictionary dictionary(fixture.view());
  for (std::string_view text : {"ก ", "ก!", "กA", "ก1", "ก๑", "ก\xE2\x80\x8B"}) {
    SCOPED_TRACE(text);
    size_t offset = 0;
    thai::Segment segment{};
    ASSERT_TRUE(thai::nextSegment(text, offset, segment, false, dictionary));
    EXPECT_EQ(offset, std::string_view("ก").size());
    EXPECT_TRUE(segment.known);
  }
}

TEST(ThaiSegmenterTest, DictionaryTerminalCannotEndInsideTheOriginalCluster) {
  const DictionaryFixture fixture({"ก", "กี่", "ข"});
  const thai::ThaiDictionary dictionary(fixture.view());
  expectKnownWords("กี่ข", dictionary, {"กี่", "ข"});
  EXPECT_EQ(thai::dictionarySplit("กี่ข", std::string_view("ก").size(), dictionary), 0u);
  EXPECT_EQ(thai::dictionarySplit("กี่ข", std::string_view("กี่").size(), dictionary), std::string_view("กี่").size());
}

TEST(ThaiSegmenterTest, EveryThaiByteSplitMatchesWholeIncludingOldBufferBoundary) {
  const std::string longest = repeated("ก", thai::MAX_DICTIONARY_WORD_CODEPOINTS);
  const DictionaryFixture fixture({"ก", longest, "กี่", "น้ำ", "เก่ง", "เรื่อง", "ประเทศไทย", "มี", "ประชากร", "จำนวน", "มาก"});
  const thai::ThaiDictionary dictionary(fixture.view());
  const std::vector<std::string> inputs{
      "ประเทศไทยมีประชากรจำนวนมาก", "กี่น้ำเก่งเรื่อง", longest + "กี่น้ำ" + longest,
      "“ประเทศไทยๆ”กี่ฯลฯน้ำ...ขคง", "่กี่นํ้าน้ําขคง", "กี่\xC2\xA0น้ำ\xE2\x80\xAFกี่\xE2\x80\x8Bน้ำ"};
  for (const auto& text : inputs) {
    SCOPED_TRACE(text);
    const auto expected = whole(text, dictionary);
    for (size_t split = 0; split <= text.size(); ++split) {
      SCOPED_TRACE(split);
      EXPECT_EQ(streamed(text, {split, text.size()}, dictionary), expected);
    }
    std::vector<size_t> byteEnds;
    for (size_t end = 1; end <= text.size(); ++end) byteEnds.push_back(end);
    EXPECT_EQ(streamed(text, byteEnds, dictionary), expected);
  }
}

TEST(ThaiSegmenterTest, UnknownRunsStayEmergencyAcrossKnownWordsAndWindowEdges) {
  const DictionaryFixture fixture({"กี่", "น้ำ"});
  const thai::ThaiDictionary dictionary(fixture.view());
  const std::string text = "กี่" + repeated("ข", 120) + "น้ำ";
  const auto spans = whole(text, dictionary);
  ASSERT_EQ(spans.size(), 122u);
  EXPECT_TRUE(spans.front().known);
  EXPECT_EQ(spans.front().after, BreakKind::Word);
  EXPECT_TRUE(spans.back().known);
  EXPECT_EQ(spans.back().before, BreakKind::Word);
  for (size_t i = 1; i + 1 < spans.size(); ++i) {
    EXPECT_FALSE(spans[i].known);
    EXPECT_TRUE(spans[i].valid);
    EXPECT_EQ(spans[i].end - spans[i].begin, 3u);
    EXPECT_EQ(spans[i].before, BreakKind::Emergency);
    EXPECT_EQ(spans[i].after, BreakKind::Emergency);
  }
  EXPECT_EQ(streamed(text, {198, 199, 200, 201, 202, 306, text.size()}, dictionary), spans);
}

TEST(ThaiSegmenterTest, LongUnknownParagraphMakesBoundedProgressWithoutPromotingWindowEdges) {
  const DictionaryFixture fixture({"ก"});
  const thai::ThaiDictionary dictionary(fixture.view());
  const std::string text = repeated("ข", (64 * 1024 + 2) / 3);
  std::vector<size_t> ends;
  for (size_t end = 200; end < text.size(); end += 200) ends.push_back(end);
  ends.push_back(text.size());
  const auto spans = streamed(text, ends, dictionary);
  ASSERT_EQ(spans.size(), text.size() / 3);
  size_t position = 0;
  for (const auto& span : spans) {
    EXPECT_EQ(span.begin, position);
    EXPECT_EQ(span.end, position + 3);
    EXPECT_FALSE(span.known);
    EXPECT_TRUE(span.valid);
    EXPECT_EQ(span.before, BreakKind::Emergency);
    EXPECT_EQ(span.after, BreakKind::Emergency);
    position = span.end;
  }
  EXPECT_EQ(position, text.size());
}

TEST(ThaiSegmenterTest, EmptyAndCorruptDictionaryKeepSafeSourceClusters) {
  const thai::ThaiDictionary empty(thai::DictionaryView{});
  DictionaryFixture fixture({"กี่"});
  fixture.data[1] = 255;  // Declared suffix exceeds both the word limit and data extent.
  const thai::ThaiDictionary corrupt(fixture.view());
  const std::string_view text = "กี่น้ำเก่ง";
  const auto expected = whole(text, empty);
  EXPECT_EQ(pieces(text, expected), (std::vector<std::string_view>{"กี่", "น้ำ", "เก่", "ง"}));
  EXPECT_EQ(whole(text, corrupt), expected);
  EXPECT_TRUE(empty.valid());
  EXPECT_FALSE(empty.available());
  EXPECT_FALSE(corrupt.valid());
  for (const auto& span : expected) {
    EXPECT_TRUE(span.valid);
    EXPECT_FALSE(span.known);
    EXPECT_EQ(span.before, BreakKind::Emergency);
    EXPECT_EQ(span.after, BreakKind::Emergency);
  }
  EXPECT_EQ(thai::dictionarySplit(text, 9, empty), 0u);
  EXPECT_EQ(thai::dictionarySplit(text, 9, corrupt), 0u);
}

TEST(ThaiSegmenterTest, PunctuationUnitsAttachOnTheCorrectSide) {
  const DictionaryFixture fixture({"กี่", "น้ำ"});
  const thai::ThaiDictionary dictionary(fixture.view());
  const std::string_view text = "“กี่ๆฯลฯน้ำ...”";
  const auto spans = whole(text, dictionary);
  ASSERT_EQ(pieces(text, spans), (std::vector<std::string_view>{"“", "กี่", "ๆ", "ฯลฯ", "น้ำ", "...", "”"}));
  EXPECT_EQ(spans.front().before, BreakKind::Punctuation);
  EXPECT_EQ(spans.front().after, BreakKind::Prohibited);
  for (size_t i : {2u, 3u, 5u, 6u}) {
    EXPECT_EQ(spans[i].before, BreakKind::Prohibited);
    EXPECT_EQ(spans[i].after, BreakKind::Punctuation);
  }
  for (std::string_view punctuation : {"ฯ", "ฯล", ".", ".."}) {
    size_t offset = 0;
    thai::Segment segment{};
    EXPECT_FALSE(thai::nextSegment(punctuation, offset, segment, false, dictionary));
    EXPECT_EQ(offset, 0u);
    const auto final = whole(punctuation, dictionary);
    EXPECT_EQ(final.front().before, BreakKind::Prohibited);
  }
}

TEST(ThaiSegmenterTest, LexicalSuffixMarksRemainKnownButNeverDivideAbbreviationUnits) {
  const DictionaryFixture fixture({"ก", "กๆ", "ขฯ", "งฯลฯ", "ล"});
  const thai::ThaiDictionary dictionary(fixture.view());
  expectKnownWords("กๆ", dictionary, {"กๆ"});
  expectKnownWords("ขฯ", dictionary, {"ขฯ"});
  expectKnownWords("งฯลฯ", dictionary, {"งฯลฯ"});
  const DictionaryFixture extendedFixture({"ก", "กๆ", "กๆข"});
  const thai::ThaiDictionary extended(extendedFixture.view());
  expectKnownWords("กๆข", extended, {"กๆข"});
  size_t offset = 0;
  thai::Segment segment{};
  EXPECT_FALSE(thai::nextSegment("กๆ", offset, segment, false, extended));
  EXPECT_EQ(offset, 0u);
  const std::string_view suffixed = "กๆขฯงฯลฯ";
  const auto expected = whole(suffixed, dictionary);
  for (size_t split = 0; split <= suffixed.size(); ++split) {
    EXPECT_EQ(streamed(suffixed, {split, suffixed.size()}, dictionary), expected) << split;
  }
  const std::string_view text = "ขฯลฯ";
  const auto spans = whole(text, dictionary);
  ASSERT_EQ(pieces(text, spans), (std::vector<std::string_view>{"ข", "ฯลฯ"}));
  EXPECT_FALSE(spans.front().known);
  EXPECT_EQ(spans.back().before, BreakKind::Prohibited);
  EXPECT_EQ(spans.back().after, BreakKind::Punctuation);
}

TEST(ThaiSegmenterTest, GenericLatinNumbersAndFullUrlsNeverUseThaiDictionaryCuts) {
  const DictionaryFixture fixture({"ภาษา", "ไทย"});
  const thai::ThaiDictionary dictionary(fixture.view());
  for (std::string_view text : {"EpubCraft", "EPUB", "4.2.0", "02:40", "๑๒๓", "https://example.org/ภาษาไทย?q=12",
                               "http://example.org/ภาษาไทย", "www.example.org/ภาษาไทย"}) {
    SCOPED_TRACE(text);
    const auto spans = whole(text, dictionary);
    ASSERT_EQ(pieces(text, spans), (std::vector<std::string_view>{text}));
    EXPECT_FALSE(spans.front().known);
    EXPECT_TRUE(spans.front().valid);
    EXPECT_EQ(spans.front().before, BreakKind::Prohibited);
    EXPECT_EQ(spans.front().after, BreakKind::Prohibited);
  }
  const std::string_view text = "ภาษาไทยEPUB4.2.0ไทย๑๒";
  const auto spans = whole(text, dictionary);
  ASSERT_EQ(pieces(text, spans), (std::vector<std::string_view>{"ภาษา", "ไทย", "EPUB4.2.0", "ไทย", "๑๒"}));
  EXPECT_TRUE(spans[0].known);
  EXPECT_TRUE(spans[1].known);
  EXPECT_FALSE(spans[2].known);
  EXPECT_TRUE(spans[3].known);
  EXPECT_FALSE(spans[4].known);
}

TEST(ThaiSegmenterTest, GenericPrefixesDoNotRetainAnUnboundedRun) {
  const thai::ThaiDictionary dictionary(thai::DictionaryView{});
  const std::string text(64 * 1024, 'a');
  size_t offset = 0;
  thai::Segment segment{};
  ASSERT_TRUE(thai::nextSegment(text, offset, segment, false, dictionary));
  EXPECT_EQ(offset, thai::MAX_DICTIONARY_WORD_CODEPOINTS + thai::MAX_CLUSTER_CODEPOINTS);
  EXPECT_EQ(segment.before, BreakKind::Prohibited);
  EXPECT_EQ(segment.after, BreakKind::Prohibited);
  EXPECT_FALSE(segment.known);
}

TEST(ThaiSegmenterTest, WhitespaceNoBreakSpacesAndExplicitZeroWidthOpportunityStayDistinct) {
  const thai::ThaiDictionary dictionary(thai::DictionaryView{});
  const std::string_view text = " \t\n\xC2\xA0\xE2\x80\xAF\xE2\x80\x8B";
  const auto spans = whole(text, dictionary);
  ASSERT_EQ(spans.size(), 6u);
  for (size_t i = 0; i < spans.size(); ++i) {
    const BreakKind expected = i < 3 ? BreakKind::Space : i < 5 ? BreakKind::Prohibited : BreakKind::Word;
    EXPECT_EQ(spans[i].before, expected);
    EXPECT_EQ(spans[i].after, expected);
  }
}

TEST(ThaiSegmenterTest, SplitRetriesShorterPrefixesWhenLongestLeavesAnUnknownSuffix) {
  const DictionaryFixture fixture({"ก", "กข", "กขค", "ขคง"});
  const thai::ThaiDictionary dictionary(fixture.view());
  EXPECT_EQ(thai::dictionarySplit("กขคง", std::string_view("กขค").size(), dictionary), std::string_view("ก").size());
  EXPECT_EQ(thai::dictionarySplit("กขคง", std::string_view("ก").size() - 1, dictionary), 0u);
  EXPECT_EQ(thai::dictionarySplit("กขคจ", std::string_view("กขค").size(), dictionary), 0u);
}

TEST(ThaiSegmenterTest, SplitRequiresWholeSuffixCoverageAndAProperProtectedBoundary) {
  const DictionaryFixture fixture({"ก", "กข", "ข", "ค", "คง", "ง", "ๆ", "ฯ", "กี่"});
  const thai::ThaiDictionary dictionary(fixture.view());
  EXPECT_EQ(thai::dictionarySplit("กขคง", std::string_view("กขค").size(), dictionary), std::string_view("กข").size());
  EXPECT_EQ(thai::dictionarySplit("กข", 100, dictionary), std::string_view("ก").size());
  EXPECT_EQ(thai::dictionarySplit("กขจ", 100, dictionary), 0u);
  EXPECT_EQ(thai::dictionarySplit("ก", 100, dictionary), 0u);
  EXPECT_EQ(thai::dictionarySplit("", 100, dictionary), 0u);
  EXPECT_EQ(thai::dictionarySplit("กข", 0, dictionary), 0u);
  EXPECT_EQ(thai::dictionarySplit("กๆ", 3, dictionary), 0u);
  EXPECT_EQ(thai::dictionarySplit("กฯ", 3, dictionary), 0u);
  EXPECT_EQ(thai::dictionarySplit("กี่ข", 3, dictionary), 0u);
}

TEST(ThaiSegmenterTest, InvalidUtf8AndPathologicalSignsPreserveBytesAndResumeKnownText) {
  const DictionaryFixture fixture({"กี่", "น้ำ"});
  const thai::ThaiDictionary dictionary(fixture.view());
  std::string text = "ก" + repeated("่", 1000) + "กี่น้ำ";
  text.insert(0, "\xF0\x28\x8C\x28", 4);
  text.append("\xE0\xB8", 2);
  const auto spans = whole(text, dictionary);
  std::string restored;
  bool invalid = false;
  bool resumed = false;
  for (const auto& span : spans) {
    restored += text.substr(span.begin, span.end - span.begin);
    invalid = invalid || !span.valid;
    resumed = resumed || (span.known && text.substr(span.begin, span.end - span.begin) == "น้ำ");
    if (!span.valid) EXPECT_LE(span.codepoints, thai::MAX_CLUSTER_CODEPOINTS);
  }
  EXPECT_EQ(restored, text);
  EXPECT_TRUE(invalid);
  EXPECT_TRUE(resumed);
  std::vector<size_t> ends;
  for (size_t end = 1; end <= text.size(); ++end) ends.push_back(end);
  EXPECT_EQ(streamed(text, ends, dictionary), spans);
}
}  // namespace
