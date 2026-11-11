#include "ThcraftHomeLayout.h"

#include <algorithm>

namespace {

constexpr int SECTION_GAP = 10;
constexpr int LABEL_GAP = 6;
constexpr int TEXT_GAP = 4;
constexpr int PROGRESS_BAR_HEIGHT = 8;
constexpr int RECENT_COLUMN_GAP = 12;
constexpr int WIDGET_GAP = 12;

int coverWidthFor(const int height) { return height * 3 / 5; }

// Section label, title (2 lines), author, big percent, bar and chapter line.
int cardTextHeight(const ThcraftHomeLayoutInput& in) {
  return in.smallLineHeight + 2 * in.titleLineHeight + in.bodyLineHeight + in.bigLineHeight + PROGRESS_BAR_HEIGHT +
         in.smallLineHeight + 5 * TEXT_GAP;
}

// Cover, one title line, then the bar and percent line.
int recentRowHeight(const ThcraftHomeLayoutInput& in, const int coverHeight) {
  return coverHeight + TEXT_GAP + in.bodyLineHeight + TEXT_GAP + in.smallLineHeight;
}

}  // namespace

ThcraftHomeLayout computeThcraftHomeLayout(const ThcraftHomeLayoutInput& in) {
  ThcraftHomeLayout out;
  const int left = in.content.x + in.sidePadding;
  const int width = std::max(0, in.content.width - 2 * in.sidePadding);
  const int bottom = in.content.y + in.content.height - in.buttonHintsHeight;

  out.header = Rect{left, in.content.y + in.topPadding, width, in.titleLineHeight + in.smallLineHeight};
  const int navHeight = ThcraftHomeLayout::NAV_ICON_SIZE + TEXT_GAP + in.smallLineHeight + 2 * TEXT_GAP;
  out.nav = Rect{left, bottom - navHeight, width, navHeight};

  const int top = out.header.y + out.header.height + SECTION_GAP;
  const int available = out.nav.y - SECTION_GAP - top;

  // The card cover is capped so the text column keeps at least 60% of the card.
  const int cardInner = std::max(0, width - 2 * ThcraftHomeLayout::CARD_PADDING);
  const int cardMax = std::max(1, std::min(ThcraftHomeLayout::CARD_MAX_COVER, cardInner * 2 / 3));
  const int cardMin = std::min(cardMax, std::max(ThcraftHomeLayout::CARD_MIN_COVER, cardTextHeight(in)));
  const int columnWidth = std::max(
      0, (width - (ThcraftHomeLayout::RECENT_COUNT - 1) * RECENT_COLUMN_GAP) / ThcraftHomeLayout::RECENT_COUNT);
  const int recentMax = std::max(1, std::min(ThcraftHomeLayout::RECENT_MAX_COVER, columnWidth * 5 / 3));
  const int recentMin = std::min(recentMax, ThcraftHomeLayout::RECENT_MIN_COVER);

  const int cardChrome = 2 * ThcraftHomeLayout::CARD_PADDING;
  const int recentChrome = SECTION_GAP + in.bodyLineHeight + LABEL_GAP + recentRowHeight(in, 0);
  const int widgetChrome = SECTION_GAP;

  out.showRecent = true;
  out.showWidgets = in.leftWidget || in.rightWidget;
  const auto required = [&] {
    return cardMin + cardChrome + (out.showRecent ? recentMin + recentChrome : 0) +
           (out.showWidgets ? ThcraftHomeLayout::WIDGET_MIN_HEIGHT + widgetChrome : 0);
  };
  // Space runs out from the bottom up: widgets go first, then the recent row.
  if (required() > available) out.showWidgets = false;
  if (required() > available) out.showRecent = false;

  int slack = std::max(0, available - required());
  int widgetHeight = ThcraftHomeLayout::WIDGET_MIN_HEIGHT;
  if (out.showWidgets) {
    const int grow = std::min(slack, ThcraftHomeLayout::WIDGET_PREFERRED_HEIGHT - widgetHeight);
    widgetHeight += grow;
    slack -= grow;
  }
  int cardCover = cardMin;
  int recentCover = recentMin;
  const int cardShare = out.showRecent ? slack * 3 / 5 : slack;
  const int cardGrow = std::min(cardShare, cardMax - cardCover);
  cardCover += cardGrow;
  slack -= cardGrow;
  if (out.showRecent) {
    const int recentGrow = std::min(slack, recentMax - recentCover);
    recentCover += recentGrow;
    slack -= recentGrow;
    const int cardRest = std::min(slack, cardMax - cardCover);
    cardCover += cardRest;
    slack -= cardRest;
  }
  // Even heights keep the dithered thumbs and their halves on whole pixels.
  cardCover &= ~1;
  recentCover &= ~1;
  if (!out.showRecent) cardCover = std::min(cardCover, std::max(1, available - cardChrome));

  int y = top;
  out.card = Rect{left, y, width, cardCover + cardChrome};
  out.cardCover = Rect{left + ThcraftHomeLayout::CARD_PADDING, y + ThcraftHomeLayout::CARD_PADDING,
                       coverWidthFor(cardCover), cardCover};
  const int textX = out.cardCover.x + out.cardCover.width + 2 * ThcraftHomeLayout::CARD_PADDING;
  out.cardText =
      Rect{textX, out.cardCover.y, std::max(0, left + width - ThcraftHomeLayout::CARD_PADDING - textX), cardCover};
  y += out.card.height;

  if (out.showRecent) {
    y += SECTION_GAP;
    out.recentLabel = Rect{left, y, width, in.bodyLineHeight};
    y += in.bodyLineHeight + LABEL_GAP;
    const int rowHeight = recentRowHeight(in, recentCover);
    for (int i = 0; i < ThcraftHomeLayout::RECENT_COUNT; ++i) {
      const int x = left + i * (columnWidth + RECENT_COLUMN_GAP);
      out.recentSlots[i] = Rect{x, y, columnWidth, rowHeight};
      const int coverWidth = coverWidthFor(recentCover);
      out.recentCovers[i] = Rect{x + (columnWidth - coverWidth) / 2, y, coverWidth, recentCover};
    }
    y += rowHeight;
  }

  if (out.showWidgets) {
    // Widgets sit on the nav bar, so leftover slack becomes space above them.
    const int widgetY = out.nav.y - SECTION_GAP - widgetHeight;
    const int half = std::max(0, (width - WIDGET_GAP) / 2);
    if (in.leftWidget && in.rightWidget) {
      out.widgets[0] = Rect{left, widgetY, half, widgetHeight};
      out.widgets[1] = Rect{left + width - half, widgetY, half, widgetHeight};
    } else {
      out.widgets[in.leftWidget ? 0 : 1] = Rect{left, widgetY, width, widgetHeight};
    }
  }
  return out;
}
