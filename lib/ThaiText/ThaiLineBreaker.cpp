#include "ThaiLineBreaker.h"

#include "ThaiCharClass.h"

namespace thai {
bool isOpeningPunctuation(uint32_t cp) {
  switch (cp) {
    case '(': case '[': case '{': case 0x2018: case 0x201C: case 0x00AB: case 0x2039:
    case 0x3008: case 0x300A: case 0x300C: case 0x300E: case 0x3010:
      return true;
    default:
      return false;
  }
}

bool isClosingPunctuation(uint32_t cp) {
  switch (cp) {
    case ')': case ']': case '}': case '"': case '\'':
    case ',': case '.': case '!': case '?': case ':': case ';':
    case 0x2019: case 0x201D: case 0x00BB: case 0x203A: case 0x2026:
    case 0x3009: case 0x300B: case 0x300D: case 0x300F: case 0x3011:
      return true;
    default:
      return false;
  }
}

bool prohibitsBreak(uint32_t left, uint32_t right) {
  return isOpeningPunctuation(left) || isClosingPunctuation(right) || isRepetition(right) ||
         isAbbreviation(right) || isDependentSign(right) || (isLeadingVowel(left) && isBase(right));
}

size_t punctuationUnitBytes(std::string_view text, size_t offset, bool endOfRun) {
  const auto first = detail::decode(text, offset, endOfRun);
  if (!first.valid) return 0;
  size_t bytes = first.bytes;
  if (first.value == '.') {
    for (unsigned i = 1; i < 3; ++i) {
      const auto next = detail::decode(text, offset + bytes, endOfRun);
      if (!next.bytes && !endOfRun) return 0;
      if (next.value != '.') break;
      bytes += next.bytes;
    }
    return bytes;
  }
  if (isAbbreviation(first.value)) {
    const auto middle = detail::decode(text, offset + bytes, endOfRun);
    if (!middle.bytes && !endOfRun) return 0;
    if (middle.value == 0x0E25) {
      const auto last = detail::decode(text, offset + bytes + middle.bytes, endOfRun);
      if (!last.bytes && !endOfRun) return 0;
      if (last.value == 0x0E2F) bytes += middle.bytes + last.bytes;
    }
    return bytes;
  }
  return isOpeningPunctuation(first.value) || isClosingPunctuation(first.value) || isRepetition(first.value) ? bytes : 0;
}
}  // namespace thai
