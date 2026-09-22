#pragma once

#include <Utf8.h>

namespace CjkBreakPolicy {
inline bool isNoBreakBeforeCjkPunctuation(uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case 0x00BB:
    case 0x2019:
    case 0x201D:
    case 0x3001:
    case 0x3002:
    case 0x3009:
    case 0x300B:
    case 0x300D:
    case 0x300F:
    case 0x3011:
    case 0x3015:
    case 0x3017:
    case 0x3019:
    case 0x301B:
    case 0xFF01:
    case 0xFF09:
    case 0xFF0C:
    case 0xFF0E:
    case 0xFF1A:
    case 0xFF1B:
    case 0xFF1F:
    case 0xFF3D:
    case 0xFF5D:
      return true;
    default:
      return false;
  }
}
inline bool isNoBreakAfterCjkPunctuation(uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0x00AB:
    case 0x2018:
    case 0x201C:
    case 0x3008:
    case 0x300A:
    case 0x300C:
    case 0x300E:
    case 0x3010:
    case 0x3014:
    case 0x3016:
    case 0x3018:
    case 0x301A:
    case 0xFF08:
    case 0xFF3B:
    case 0xFF5B:
      return true;
    default:
      return false;
  }
}
inline bool hasCjkBreakOpportunityBetween(uint32_t left, uint32_t right) {
  return (utf8IsCjkBreakable(left) || utf8IsCjkBreakable(right)) && !isNoBreakAfterCjkPunctuation(left) &&
         !isNoBreakBeforeCjkPunctuation(right) && !utf8IsCombiningMark(right);
}
}  // namespace CjkBreakPolicy
