#pragma once

#include <cstdint>

namespace thai {
inline constexpr uint32_t ANALYSIS_UNAVAILABLE_LAYOUT_BIT = uint32_t{1} << 31;

// Stable analysis identity; bit 31 is reserved for transient fallback cache output.
uint32_t layoutId();
}  // namespace thai
