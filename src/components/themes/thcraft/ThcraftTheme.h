#pragma once

#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/thcraft/ThcraftHomeLayout.h"

class GfxRenderer;

namespace ThcraftMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.contentSidePadding = 16;
  return v;
}();
}  // namespace ThcraftMetrics

// Lyra chrome everywhere except home, which ThcraftHomeActivity draws.
class ThcraftTheme : public LyraTheme {
 public:
  // Large numbers: card percent, widget values, clock and stats tiles.
  static int bigFontId();
  static ThcraftHomeLayout homeLayout(const GfxRenderer& renderer);
  // Card cover height, so the reader pre-generates the thumb home draws first.
  int homeCoverThumbHeight(const GfxRenderer& renderer) const override;
};
