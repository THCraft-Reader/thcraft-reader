#include "Hyphenator.h"

#include <Utf8.h>

#include <algorithm>
#include <cassert>
#include <vector>

#include "HyphenationCommon.h"
#include "LanguageHyphenator.h"
#include "LanguageRegistry.h"

const LanguageHyphenator* Hyphenator::cachedHyphenator_ = nullptr;

namespace {

// Normalize ISO 639-2 (three-letter) codes to ISO 639-1 (two-letter) codes used by the
// hyphenation registry.  EPUBs may use either form in their dc:language metadata (e.g.
// "eng" instead of "en").  Both the bibliographic ("fre"/"ger") and terminological
// ("fra"/"deu") ISO 639-2 variants are mapped.
struct Iso639Mapping {
  const char* iso639_2;
  const char* iso639_1;
};
static constexpr Iso639Mapping kIso639Mappings[] = {{"eng", "en"}, {"fra", "fr"}, {"fre", "fr"}, {"deu", "de"},
                                                    {"ger", "de"}, {"rus", "ru"}, {"spa", "es"}, {"ita", "it"},
                                                    {"ukr", "uk"}, {"swe", "sv"}, {"fin", "fi"}};

// Maps a BCP-47 or ISO 639-2 language tag to a language-specific hyphenator.
const LanguageHyphenator* hyphenatorForLanguage(const std::string& langTag) {
  if (langTag.empty()) return nullptr;

  // Extract primary subtag and normalize to lowercase (e.g., "en-US" -> "en", "ENG" -> "en").
  std::string primary;
  primary.reserve(langTag.size());
  for (char c : langTag) {
    if (c == '-' || c == '_') break;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    primary.push_back(c);
  }
  if (primary.empty()) return nullptr;

  // Normalize ISO 639-2 three-letter codes to two-letter equivalents.
  for (const auto& mapping : kIso639Mappings) {
    if (primary == mapping.iso639_2) {
      primary = mapping.iso639_1;
      break;
    }
  }

  return getLanguageHyphenatorForPrimaryTag(primary);
}

// Both public forms share the same separator, pattern and fallback policy. The
// bounded form never constructs a temporary word or an allocating container.
bool separator(uint32_t cp) { return isExplicitHyphen(cp) || isApostrophe(cp); }

void trimRange(const CodepointInfo*& cps, size_t& count) {
  if (count >= 3) {
    size_t end = count;
    while (end && isPunctuation(cps[end - 1].value)) --end;
    size_t pos = end;
    while (pos && isAsciiDigit(cps[pos - 1].value)) --pos;
    if (pos && pos < end && cps[pos - 1].value == '[' && end - pos > 1) count = pos - 1;
  }
  while (count && isPunctuation(cps[0].value)) {
    ++cps;
    --count;
  }
  while (count && isPunctuation(cps[count - 1].value)) --count;
}

struct BreakOutput {
  Hyphenator::BreakInfo* values;
  size_t count = 0;
  void add(size_t offset, bool hyphen) { values[count++] = {offset, hyphen}; }
};

void patternBreaks(const CodepointInfo* cps, size_t count, const LanguageHyphenator* hyphenator, bool fallback,
                   BreakOutput& output, bool cjkWithoutHyphen = true) {
  // The shared Liang evaluator deliberately bounds words to 68 characters.
  size_t indexes[70];
  const size_t found = hyphenator ? hyphenator->breakIndexes(cps, count, indexes, 70) : 0;
  auto add = [&](size_t at) {
    const bool cjk = utf8IsCjkBreakable(cps[at].value) || utf8IsCjkBreakable(cps[at - 1].value);
    output.add(cps[at].byteOffset, !cjkWithoutHyphen || !cjk);
  };
  if (found) {
    for (size_t i = 0; i < found; ++i) add(indexes[i]);
  } else if (fallback) {
    const size_t prefix = hyphenator ? hyphenator->minPrefix() : LiangWordConfig::kDefaultMinPrefix;
    const size_t suffix = hyphenator ? hyphenator->minSuffix() : LiangWordConfig::kDefaultMinSuffix;
    for (size_t at = prefix; at < count && suffix <= count - at; ++at) add(at);
  }
}
}  // namespace

bool Hyphenator::breakOffsets(const CodepointInfo* cps, size_t count, bool includeFallback, BreakInfo* output,
                              size_t capacity, size_t& written) {
  written = 0;
  if (capacity < count || (count && (!cps || !output))) return false;
  trimRange(cps, count);
  if (!count) return true;
  BreakOutput result{output};
  bool explicitBreak = false, apostrophe = false;
  for (size_t i = 0; i < count; ++i) {
    apostrophe |= isApostrophe(cps[i].value);
    if (i && i + 1 < count && isExplicitHyphen(cps[i].value) && isAlphabetic(cps[i - 1].value) &&
        isAlphabetic(cps[i + 1].value)) {
      result.add(cps[i + 1].byteOffset, isSoftHyphen(cps[i].value));
      explicitBreak = true;
    }
  }
  if (explicitBreak || apostrophe) {
    size_t start = 0;
    for (size_t end = 0; end <= count; ++end) {
      if (end < count && !separator(cps[end].value)) continue;
      if (cachedHyphenator_ && end > start)
        patternBreaks(cps + start, end - start, cachedHyphenator_, includeFallback && !explicitBreak, result, false);
      if (end < count && isApostrophe(cps[end].value) && end && end + 1 < count && isAlphabetic(cps[end - 1].value) &&
          isAlphabetic(cps[end + 1].value)) {
        size_t left = 0, right = 0;
        for (size_t i = start; i < end; ++i) left += isAlphabetic(cps[i].value);
        for (size_t i = end + 1; i < count && !separator(cps[i].value); ++i) right += isAlphabetic(cps[i].value);
        if (left >= 3 && right >= 3) result.add(cps[end + 1].byteOffset, false);
      }
      start = end + 1;
    }
  } else {
    patternBreaks(cps, count, cachedHyphenator_, includeFallback, result);
  }
  if (result.count) {
    std::sort(output, output + result.count, [](const BreakInfo& a, const BreakInfo& b) {
      return a.byteOffset < b.byteOffset ||
             (a.byteOffset == b.byteOffset && a.requiresInsertedHyphen < b.requiresInsertedHyphen);
    });
    for (size_t i = 0; i < result.count; ++i)
      if (!written || output[i].byteOffset != output[written - 1].byteOffset) output[written++] = output[i];
  }
  return true;
}

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string& word, const bool includeFallback) {
  auto cps = collectCodepoints(word);
  std::vector<BreakInfo> result(cps.size());
  size_t count = 0;
  breakOffsets(cps.data(), cps.size(), includeFallback, result.data(), result.size(), count);
  result.resize(count);
  return result;
}

void Hyphenator::setPreferredLanguage(const std::string& lang) { cachedHyphenator_ = hyphenatorForLanguage(lang); }
