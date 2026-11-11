#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace thai {
constexpr bool isThai(uint32_t cp) { return cp >= 0x0E00 && cp <= 0x0E7F; }
constexpr bool isBase(uint32_t cp) { return cp >= 0x0E01 && cp <= 0x0E2E; }
constexpr bool isLeadingVowel(uint32_t cp) { return cp >= 0x0E40 && cp <= 0x0E44; }
constexpr bool isAboveVowel(uint32_t cp) { return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E37); }
constexpr bool isBelowVowel(uint32_t cp) { return cp >= 0x0E38 && cp <= 0x0E3A; }
constexpr bool isTone(uint32_t cp) { return cp >= 0x0E48 && cp <= 0x0E4B; }
constexpr bool isSign(uint32_t cp) { return cp == 0x0E47 || (cp >= 0x0E4C && cp <= 0x0E4E); }
constexpr bool isSpacingVowel(uint32_t cp) {
  return cp == 0x0E30 || cp == 0x0E32 || cp == 0x0E33 || cp == 0x0E45;
}
constexpr bool isCombiningSign(uint32_t cp) {
  return isAboveVowel(cp) || isBelowVowel(cp) || isTone(cp) || isSign(cp);
}
// This is a boundary property, not a glyph-advance or normalization rule.
constexpr bool isDependentSign(uint32_t cp) { return isCombiningSign(cp) || isSpacingVowel(cp); }
constexpr bool isDigit(uint32_t cp) { return cp >= 0x0E50 && cp <= 0x0E59; }
constexpr bool isRepetition(uint32_t cp) { return cp == 0x0E46; }
constexpr bool isAbbreviation(uint32_t cp) { return cp == 0x0E2F; }

namespace detail {
inline constexpr uint32_t INVALID_SCALAR = 0xFFFFFFFF;
struct Scalar {
  uint32_t value;
  uint8_t bytes;
  bool valid;
};

// bytes=0 means a potentially valid but incomplete scalar, or no input.
// Invalid bytes consume one byte, preserving every original byte for fallback.
inline Scalar decode(std::string_view text, size_t offset, bool endOfRun) {
  if (offset >= text.size()) return {INVALID_SCALAR, 0, false};
  const auto first = static_cast<uint8_t>(text[offset]);
  if (first < 0x80) return {first, 1, true};
  const uint8_t length = first >= 0xC2 && first <= 0xDF ? 2 :
                         first >= 0xE0 && first <= 0xEF ? 3 :
                         first >= 0xF0 && first <= 0xF4 ? 4 : 0;
  if (!length) return {INVALID_SCALAR, 1, false};
  uint32_t cp = first & (0x7F >> length);
  for (uint8_t i = 1; i < length; ++i) {
    if (text.size() - offset <= i) return {INVALID_SCALAR, static_cast<uint8_t>(endOfRun ? 1 : 0), false};
    const auto byte = static_cast<uint8_t>(text[offset + i]);
    if ((byte & 0xC0) != 0x80 ||
        (i == 1 && ((first == 0xE0 && byte < 0xA0) || (first == 0xED && byte >= 0xA0) ||
                    (first == 0xF0 && byte < 0x90) || (first == 0xF4 && byte >= 0x90)))) {
      return {INVALID_SCALAR, 1, false};
    }
    cp = (cp << 6) | (byte & 0x3F);
  }
  return {cp, length, true};
}
}  // namespace detail
}  // namespace thai
