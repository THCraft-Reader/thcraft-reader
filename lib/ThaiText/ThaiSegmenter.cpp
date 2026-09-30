#include "ThaiSegmenter.h"

#include "ThaiCluster.h"

namespace thai {
namespace {
constexpr size_t LOOKAHEAD_CODEPOINTS = MAX_DICTIONARY_WORD_CODEPOINTS + MAX_CLUSTER_CODEPOINTS;

bool isWhitespace(uint32_t cp) {
  return cp == ' ' || (cp >= '\t' && cp <= '\r') || cp == 0x00A0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
         cp == 0x3000;
}

bool isPunctuation(uint32_t cp) {
  return isOpeningPunctuation(cp) || isClosingPunctuation(cp) || isRepetition(cp) || isAbbreviation(cp);
}

bool isThaiWordScalar(uint32_t cp) {
  return isThai(cp) && !isDigit(cp) && !isRepetition(cp) && !isAbbreviation(cp);
}

bool startsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

uint16_t countCodepoints(std::string_view text) {
  uint16_t count = 0;
  for (size_t offset = 0; offset < text.size(); ++count) offset += detail::decode(text, offset, true).bytes;
  return count;
}

bool emit(size_t& offset, Segment& result, size_t end, uint16_t codepoints, BreakKind before, BreakKind after,
          bool known, bool valid) {
  result = {offset, end, codepoints, before, after, known, valid};
  offset = end;
  return true;
}

bool genericSegment(std::string_view text, size_t& offset, Segment& result, bool endOfRun) {
  const auto remaining = text.substr(offset);
  const bool url = startsWith(remaining, "http://") || startsWith(remaining, "https://") || startsWith(remaining, "www.");
  size_t end = offset;
  uint16_t codepoints = 0;
  while (codepoints < LOOKAHEAD_CODEPOINTS && end < text.size()) {
    const auto scalar = detail::decode(text, end, endOfRun);
    if (!scalar.bytes || !scalar.valid || isWhitespace(scalar.value) || scalar.value == 0x200B ||
        (!url && isThaiWordScalar(scalar.value))) break;
    // A Thai suffix is a protected punctuation unit, not part of a number or a
    // Latin token. ASCII decimal/version/time punctuation stays generic.
    if (!url && (isRepetition(scalar.value) || isAbbreviation(scalar.value))) break;
    end += scalar.bytes;
    ++codepoints;
  }
  if (end == offset) return false;
  return emit(offset, result, end, codepoints, BreakKind::Prohibited, BreakKind::Prohibited, false, true);
}

bool wordLookahead(std::string_view text, bool endOfRun, size_t& maxBytes) {
  size_t offset = 0;
  for (size_t count = 0; count < LOOKAHEAD_CODEPOINTS; ++count) {
    if (offset == text.size()) {
      if (!endOfRun) return false;
      maxBytes = offset;
      return true;
    }
    const auto scalar = detail::decode(text, offset, endOfRun);
    if (!scalar.bytes) return false;
    // Lexical entries may include Thai suffix marks, even before another Thai
    // syllable. Do not treat those marks as evidence that no longer word exists.
    if (!scalar.valid || !isThai(scalar.value) || isDigit(scalar.value)) {
      maxBytes = offset;
      return true;
    }
    offset += scalar.bytes;
  }
  maxBytes = offset;
  return true;
}

size_t completePunctuationPrefix(std::string_view text, size_t end) {
  // Dictionary symbols are three-byte Thai scalars. Only ฯลฯ can straddle a
  // dictionary terminal; examine its two possible interior boundaries.
  for (size_t distance = 3; distance <= 6 && distance <= end; distance += 3) {
    const size_t begin = end - distance;
    if (!isAbbreviation(detail::decode(text, begin, true).value)) continue;
    if (begin + punctuationUnitBytes(text, begin, true) > end) return begin;
  }
  return end;
}

size_t longestWholeUnitMatch(std::string_view text, size_t maxBytes, const ThaiDictionary& dictionary) {
  size_t matched = dictionary.longestMatch(text, maxBytes);
  while (matched) {
    const size_t complete = completePunctuationPrefix(text, matched);
    if (complete == matched) break;
    matched = dictionary.longestMatch(text, complete);
  }
  return matched;
}

bool allowsPrefixBreak(std::string_view text, size_t end) {
  uint32_t left = detail::INVALID_SCALAR;
  for (size_t offset = 0; offset < end;) {
    const auto scalar = detail::decode(text, offset, true);
    left = scalar.value;
    offset += scalar.bytes;
  }
  return !prohibitsBreak(left, detail::decode(text, end, true).value);
}
}  // namespace

bool nextSegment(std::string_view text, size_t& offset, Segment& result, bool endOfRun,
                 const ThaiDictionary& dictionary) {
  const auto first = detail::decode(text, offset, endOfRun);
  if (!first.bytes) return false;
  if (first.valid && isPunctuation(first.value)) {
    const size_t bytes = punctuationUnitBytes(text, offset, endOfRun);
    if (!bytes) return false;
    const bool opening = isOpeningPunctuation(first.value);
    return emit(offset, result, offset + bytes, countCodepoints(text.substr(offset, bytes)),
                opening ? BreakKind::Punctuation : BreakKind::Prohibited,
                opening ? BreakKind::Prohibited : BreakKind::Punctuation, false, true);
  }
  if (first.valid && (isWhitespace(first.value) || first.value == 0x200B)) {
    const BreakKind rank = first.value == 0x00A0 || first.value == 0x202F ? BreakKind::Prohibited :
                           first.value == 0x200B ? BreakKind::Word : BreakKind::Space;
    return emit(offset, result, offset + first.bytes, 1, rank, rank, false, true);
  }
  if (first.valid && !isThaiWordScalar(first.value)) return genericSegment(text, offset, result, endOfRun);

  size_t clusterEnd = offset;
  Cluster cluster{};
  if (!nextCluster(text, clusterEnd, cluster, endOfRun)) return false;
  if (cluster.valid && dictionary.available() && dictionary.valid()) {
    const auto remaining = text.substr(offset);
    size_t maxBytes = 0;
    if (!wordLookahead(remaining, endOfRun, maxBytes)) return false;
    const size_t matched = longestWholeUnitMatch(remaining, maxBytes, dictionary);
    if (matched && dictionary.valid()) {
      return emit(offset, result, offset + matched, countCodepoints(remaining.substr(0, matched)),
                  BreakKind::Word, BreakKind::Word, true, true);
    }
  }
  return emit(offset, result, cluster.end, cluster.codepoints, BreakKind::Emergency, BreakKind::Emergency, false,
              cluster.valid);
}

size_t dictionarySplit(std::string_view token, size_t maxBytes, const ThaiDictionary& dictionary) {
  if (token.empty() || !dictionary.available() || !dictionary.valid()) return 0;
  if (maxBytes >= token.size()) maxBytes = token.size() - 1;
  for (size_t attempt = 0; maxBytes && attempt < MAX_DICTIONARY_WORD_CODEPOINTS; ++attempt) {
    const size_t prefix = longestWholeUnitMatch(token, maxBytes, dictionary);
    if (!prefix || !dictionary.valid()) return 0;
    if (!allowsPrefixBreak(token, prefix)) {
      maxBytes = prefix - 1;
      continue;
    }
    size_t covered = prefix;
    while (covered < token.size()) {
      const size_t matched = longestWholeUnitMatch(token.substr(covered), token.size() - covered, dictionary);
      if (!matched || !dictionary.valid()) break;
      covered += matched;
    }
    if (!dictionary.valid()) return 0;
    if (covered == token.size()) return prefix;
    maxBytes = prefix - 1;
  }
  return 0;
}
}  // namespace thai
