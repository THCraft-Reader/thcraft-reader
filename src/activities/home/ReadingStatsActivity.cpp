#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/themes/thcraft/ThcraftTheme.h"
#include "fontIds.h"

namespace {

constexpr int GAP = 12;
constexpr int PADDING = 10;
constexpr int RADIUS = 8;

constexpr StrId WEEKDAYS[7] = {StrId::STR_WEEKDAY_MON, StrId::STR_WEEKDAY_TUE, StrId::STR_WEEKDAY_WED,
                               StrId::STR_WEEKDAY_THU, StrId::STR_WEEKDAY_FRI, StrId::STR_WEEKDAY_SAT,
                               StrId::STR_WEEKDAY_SUN};

constexpr const char* HOUR_LABELS[24] = {"0",     nullptr, nullptr, nullptr, nullptr, nullptr, "6",     nullptr,
                                         nullptr, nullptr, nullptr, nullptr, "12",    nullptr, nullptr, nullptr,
                                         nullptr, nullptr, "18",    nullptr, nullptr, nullptr, nullptr, nullptr};

void formatDuration(char* out, const size_t size, const uint32_t seconds) {
  const uint32_t minutes = seconds / 60;
  if (minutes < 60) {
    snprintf(out, size, tr(STR_MINUTES_FMT), static_cast<int>(minutes));
  } else {
    snprintf(out, size, tr(STR_HOURS_MINUTES_FMT), static_cast<int>(minutes / 60), static_cast<int>(minutes % 60));
  }
}

// One metric tile: label, large value, small caption.
void drawTile(const GfxRenderer& renderer, const Rect r, const char* label, const char* value, const char* caption) {
  renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 1, RADIUS, true);
  int y = r.y + PADDING;
  renderer.drawText(SMALL_FONT_ID, r.x + PADDING, y, label);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 2;
  const int bigFont = ThcraftTheme::bigFontId();
  renderer.drawText(bigFont, r.x + PADDING, y, value, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(bigFont);
  if (caption && caption[0] != '\0') renderer.drawText(SMALL_FONT_ID, r.x + PADDING, y, caption);
}

// Columns with labels underneath, scaled to the largest value; `highlight` is solid.
void drawColumns(const GfxRenderer& renderer, const Rect r, const uint32_t* values, const int count,
                 const int highlight, const char* const* labels) {
  const int labelHeight = labels ? renderer.getLineHeight(SMALL_FONT_ID) + 2 : 0;
  const int base = r.y + r.height - labelHeight;
  const int chartHeight = base - r.y;
  const int slot = r.width / std::max(1, count);
  if (chartHeight < 4 || slot < 2) return;
  uint32_t largest = 1;
  for (int i = 0; i < count; ++i) largest = std::max(largest, values[i]);
  renderer.drawLine(r.x, base, r.x + slot * count - 1, base);
  const int barWidth = std::max(1, slot * 3 / 5);
  for (int i = 0; i < count; ++i) {
    const int x = r.x + i * slot + (slot - barWidth) / 2;
    const int height = static_cast<int>(static_cast<uint64_t>(chartHeight - 1) * values[i] / largest);
    if (height > 0) {
      if (i == highlight) {
        renderer.fillRect(x, base - height, barWidth, height);
      } else {
        renderer.fillRectDither(x, base - height, barWidth, height, Color::DarkGray);
      }
    }
    if (labels && labels[i]) {
      const int width = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      renderer.drawText(SMALL_FONT_ID, r.x + i * slot + (slot - width) / 2, base + 2, labels[i]);
    }
  }
}

}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  stats = READING_STATS.snapshot();
  const Rect content = UITheme::getContentArea(renderer);
  const int width = content.width - 2 * UITheme::getInstance().getMetrics().contentSidePadding;
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_STATS_NEED_CLOCK), width, 2);
  for (size_t i = 0; i < lines.size() && i < 2; ++i) needClock[i] = lines[i];
  requestUpdate();
}

void ReadingStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome(HomeMenuItem::STATS_SCREEN);
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_READING_STATS), nullptr, true);

  const int left = content.x + metrics.contentSidePadding;
  const int width = content.width - 2 * metrics.contentSidePadding;
  const int bottom = content.y + content.height - metrics.buttonHintsHeight - GAP;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  char value[24];
  char caption[48];
  const int tileWidth = (width - GAP) / 2;
  const int tileHeight =
      PADDING * 2 + renderer.getLineHeight(SMALL_FONT_ID) * 2 + 2 + renderer.getLineHeight(ThcraftTheme::bigFontId());

  if (!stats.reliable) {
    for (const auto& line : needClock) {
      if (line.empty()) continue;
      renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID);
    }
    y += GAP;
    // Finished books are counted without dates, so they stay meaningful.
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(stats.booksFinished));
    drawTile(renderer, Rect{left, y, tileWidth, tileHeight}, tr(STR_BOOKS_FINISHED), value, nullptr);
  } else {
    formatDuration(value, sizeof(value), stats.todaySeconds);
    snprintf(caption, sizeof(caption), tr(STR_SESSIONS_FMT), static_cast<int>(stats.todaySessions));
    drawTile(renderer, Rect{left, y, tileWidth, tileHeight}, tr(STR_WIDGET_TODAY), value, caption);
    formatDuration(value, sizeof(value), stats.last7Seconds);
    drawTile(renderer, Rect{left + width - tileWidth, y, tileWidth, tileHeight}, tr(STR_LAST_7_DAYS), value, nullptr);
    y += tileHeight + GAP;

    snprintf(value, sizeof(value), "%d", static_cast<int>(stats.streak));
    snprintf(caption, sizeof(caption), tr(STR_BEST_STREAK_FMT), static_cast<int>(stats.bestStreak));
    drawTile(renderer, Rect{left, y, tileWidth, tileHeight}, tr(STR_STREAK), value, caption);
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(stats.booksFinished));
    snprintf(caption, sizeof(caption), tr(STR_FINISHED_30_FMT), static_cast<int>(stats.finishedLast30));
    drawTile(renderer, Rect{left + width - tileWidth, y, tileWidth, tileHeight}, tr(STR_BOOKS_FINISHED), value,
             caption);
    y += tileHeight + GAP;

    const int titleHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int remaining = bottom - y;
    const int chartHeight = (remaining - 2 * (titleHeight + GAP)) / 2;
    if (chartHeight >= 40) {
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_THIS_WEEK), true, EpdFontFamily::BOLD);
      y += titleHeight + 4;
      const char* labels[7];
      for (int i = 0; i < 7; ++i) labels[i] = I18N.get(WEEKDAYS[i]);
      drawColumns(renderer, Rect{left, y, width, chartHeight}, stats.weekSeconds, 7, stats.weekday, labels);
      y += chartHeight + GAP;

      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_TODAY_BY_HOUR), true, EpdFontFamily::BOLD);
      y += titleHeight + 4;
      drawColumns(renderer, Rect{left, y, width, chartHeight}, stats.hourSeconds, 24, stats.hour, HOUR_LABELS);
      y += chartHeight + GAP;
    }
    if (y + titleHeight <= bottom) {
      char total[24];
      formatDuration(total, sizeof(total), stats.last30Seconds);
      snprintf(caption, sizeof(caption), tr(STR_LAST_30_DAYS_FMT), total);
      renderer.drawText(UI_10_FONT_ID, left, y, caption);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
