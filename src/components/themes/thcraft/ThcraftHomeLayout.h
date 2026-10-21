#pragma once

#include "components/themes/BaseTheme.h"

// Geometry of the THCraft home screen. A pure function of the screen, theme
// metrics, font heights and which widget slots are on (never of book data), so
// the same numbers feed homeCoverThumbHeight() and the draw. Turning both slots
// off resizes the covers, which generates new thumbs once.
struct ThcraftHomeLayoutInput {
  Rect content;  // UITheme::getContentArea()
  int topPadding = 0;
  int sidePadding = 0;
  int buttonHintsHeight = 0;  // 0 on touch boards
  int titleLineHeight = 0;    // UI_12
  int bodyLineHeight = 0;     // UI_10
  int smallLineHeight = 0;    // SMALL
  int bigLineHeight = 0;      // progress percent font
  // Slots set to something other than Off; a lone slot spans the row.
  bool leftWidget = true;
  bool rightWidget = true;
};

struct ThcraftHomeLayout {
  static constexpr int RECENT_COUNT = 3;
  static constexpr int WIDGET_COUNT = 2;
  static constexpr int NAV_ICON_SIZE = 32;
  static constexpr int CARD_PADDING = 10;
  static constexpr int CARD_MIN_COVER = 150;
  static constexpr int CARD_MAX_COVER = 240;
  static constexpr int RECENT_MIN_COVER = 96;
  static constexpr int RECENT_MAX_COVER = 170;
  static constexpr int WIDGET_MIN_HEIGHT = 112;
  static constexpr int WIDGET_PREFERRED_HEIGHT = 150;

  Rect header;
  Rect card;
  Rect cardCover;
  Rect cardText;
  bool showRecent = false;
  Rect recentLabel;
  Rect recentSlots[RECENT_COUNT];
  Rect recentCovers[RECENT_COUNT];
  bool showWidgets = false;
  Rect widgets[WIDGET_COUNT];
  Rect nav;

  // Thumbs are generated to cover a 3:5 box, and covers are 3:5, so a thumb
  // generated at the cover height fills its slot 1:1.
  int cardThumbHeight() const { return cardCover.height; }
  int recentThumbHeight() const { return recentCovers[0].height; }
};

ThcraftHomeLayout computeThcraftHomeLayout(const ThcraftHomeLayoutInput& in);
