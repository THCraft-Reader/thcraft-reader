#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace native_text {
inline bool nextUtf8(std::string_view text, size_t& offset, uint32_t& codepoint) {
  if (offset >= text.size()) return false;
  const auto lead = static_cast<uint8_t>(text[offset]);
  unsigned length;
  uint32_t minimum;
  if (lead < 0x80) {
    codepoint = lead;
    ++offset;
    return true;
  }
  if (lead >= 0xc2 && lead <= 0xdf) {
    length = 2;
    codepoint = lead & 0x1f;
    minimum = 0x80;
  } else if (lead >= 0xe0 && lead <= 0xef) {
    length = 3;
    codepoint = lead & 0x0f;
    minimum = 0x800;
  } else if (lead >= 0xf0 && lead <= 0xf4) {
    length = 4;
    codepoint = lead & 0x07;
    minimum = 0x10000;
  } else {
    return false;
  }
  if (text.size() - offset < length) return false;
  for (unsigned i = 1; i < length; ++i) {
    const auto byte = static_cast<uint8_t>(text[offset + i]);
    if ((byte & 0xc0) != 0x80) return false;
    codepoint = (codepoint << 6) | (byte & 0x3f);
  }
  if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
  offset += length;
  return true;
}
inline bool utf8Boundary(std::string_view text, size_t offset) {
  return offset <= text.size() && (offset == text.size() || (static_cast<uint8_t>(text[offset]) & 0xc0) != 0x80);
}
inline bool isThai(uint32_t cp) { return cp >= 0x0e01 && cp <= 0x0e5b; }
}  // namespace native_text
