#include "ThcraftHomeWidgets.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "components/themes/thcraft/ThcraftTheme.h"
#include "fontIds.h"
#include "util/WidgetImages.h"

namespace thcraft_widgets {
namespace {

constexpr int PADDING = 10;
constexpr int RADIUS = 8;
constexpr int GAP = 4;
constexpr uint32_t READ_DAY_SECONDS = 60;

constexpr StrId WEEKDAYS[7] = {StrId::STR_WEEKDAY_MON, StrId::STR_WEEKDAY_TUE, StrId::STR_WEEKDAY_WED,
                               StrId::STR_WEEKDAY_THU, StrId::STR_WEEKDAY_FRI, StrId::STR_WEEKDAY_SAT,
                               StrId::STR_WEEKDAY_SUN};
constexpr StrId WEEKDAY_INITIALS[7] = {StrId::STR_WEEKDAY_INITIAL_MON, StrId::STR_WEEKDAY_INITIAL_TUE,
                                       StrId::STR_WEEKDAY_INITIAL_WED, StrId::STR_WEEKDAY_INITIAL_THU,
                                       StrId::STR_WEEKDAY_INITIAL_FRI, StrId::STR_WEEKDAY_INITIAL_SAT,
                                       StrId::STR_WEEKDAY_INITIAL_SUN};

// Title row; returns the y where content starts.
int drawTitle(const GfxRenderer& renderer, const Rect rect, const char* title) {
  renderer.drawText(UI_10_FONT_ID, rect.x + PADDING, rect.y + PADDING, title, true, EpdFontFamily::BOLD);
  return rect.y + PADDING + renderer.getLineHeight(UI_10_FONT_ID) + GAP;
}

void drawMessage(const GfxRenderer& renderer, const Rect rect, const int top, const std::string (&lines)[2]) {
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  for (int i = 0; i < 2; ++i) {
    if (!lines[i].empty()) renderer.drawText(SMALL_FONT_ID, rect.x + PADDING, top + i * lineHeight, lines[i].c_str());
  }
}

// Big number followed by a small unit on the same baseline band.
int drawBigValue(const GfxRenderer& renderer, const int x, const int y, const char* value, const char* unit) {
  const int bigFont = ThcraftTheme::bigFontId();
  renderer.drawText(bigFont, x, y, value, true, EpdFontFamily::BOLD);
  const int width = renderer.getTextWidth(bigFont, value, EpdFontFamily::BOLD);
  if (unit) {
    const int unitY = y + renderer.getLineHeight(bigFont) - renderer.getLineHeight(UI_10_FONT_ID) - 2;
    renderer.drawText(UI_10_FONT_ID, x + width + GAP, unitY, unit);
  }
  return y + renderer.getLineHeight(bigFont);
}

}  // namespace

void drawProgressBar(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                     const uint32_t value, const uint32_t max) {
  if (width <= 4 || height <= 0) return;
  renderer.drawRoundedRect(x, y, width, height, 1, height / 2, true);
  const uint32_t clamped = std::min(value, max);
  const int fill = max > 0 ? static_cast<int>(static_cast<uint64_t>(width - 4) * clamped / max) : 0;
  if (fill > 0) renderer.fillRoundedRect(x + 2, y + 2, fill, height - 4, (height - 4) / 2, Color::Black);
}

bool localNow(struct tm& out) {
  // Below 2025-01-01 the clock was never set.
  constexpr time_t MIN_VALID_EPOCH = 1735689600;
  const time_t now = time(nullptr);
  return now >= MIN_VALID_EPOCH && localtime_r(&now, &out) != nullptr;
}

bool formatClock(char* out, const size_t size, const struct tm& local) {
  if (SETTINGS.clockFormat == 1) {
    const int hour = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
    return snprintf(out, size, "%d:%02d %s", hour, local.tm_min, local.tm_hour >= 12 ? "PM" : "AM") > 0;
  }
  return snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min) > 0;
}

void drawFrame(const GfxRenderer& renderer, const Rect rect, const bool focused) {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, focused ? 3 : 1, RADIUS, true);
}

void drawToday(const GfxRenderer& renderer, const Rect rect, const ReadingStatsSnapshot& stats,
               const Messages& messages) {
  int y = drawTitle(renderer, rect, tr(STR_WIDGET_TODAY));
  if (!stats.reliable) {
    drawMessage(renderer, rect, y, messages.needClock);
    return;
  }
  char value[12];
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(stats.todaySeconds / 60));
  y = drawBigValue(renderer, rect.x + PADDING, y, value, tr(STR_MINUTES_SHORT));
  char sessions[32];
  snprintf(sessions, sizeof(sessions), tr(STR_SESSIONS_FMT), static_cast<int>(stats.todaySessions));
  renderer.drawText(SMALL_FONT_ID, rect.x + PADDING, y, sessions);
  y += renderer.getLineHeight(SMALL_FONT_ID) + GAP;

  // 24 hour bars along the bottom, scaled to the busiest hour; the current hour is solid.
  const int bottom = rect.y + rect.height - PADDING;
  const int chartHeight = bottom - y;
  const int slot = (rect.width - 2 * PADDING) / 24;
  if (chartHeight < 6 || slot < 2) return;
  uint32_t busiest = 1;
  for (const uint32_t seconds : stats.hourSeconds) busiest = std::max(busiest, seconds);
  const int chartX = rect.x + (rect.width - slot * 24) / 2;
  renderer.drawLine(chartX, bottom, chartX + slot * 24 - 1, bottom);
  for (int hour = 0; hour < 24; ++hour) {
    const int height = static_cast<int>(static_cast<uint64_t>(chartHeight - 1) * stats.hourSeconds[hour] / busiest);
    if (height <= 0) continue;
    const int x = chartX + hour * slot;
    if (hour == stats.hour) {
      renderer.fillRect(x, bottom - height, slot - 1, height);
    } else {
      renderer.fillRectDither(x, bottom - height, slot - 1, height, Color::DarkGray);
    }
  }
}

void drawWeek(const GfxRenderer& renderer, const Rect rect, const ReadingStatsSnapshot& stats,
              const Messages& messages) {
  int y = drawTitle(renderer, rect, tr(STR_WIDGET_WEEK));
  if (!stats.reliable) {
    drawMessage(renderer, rect, y, messages.needClock);
    return;
  }
  const int columnWidth = (rect.width - 2 * PADDING) / 7;
  const int dot = std::max(6, std::min(columnWidth - 8, 18));
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  // Full names only when every one fits its column with a gap; otherwise initials.
  int widest = 0;
  for (const StrId day : WEEKDAYS) widest = std::max(widest, renderer.getTextWidth(SMALL_FONT_ID, I18N.get(day)));
  const StrId* labels = widest <= columnWidth - 4 ? WEEKDAYS : WEEKDAY_INITIALS;
  y += GAP;
  for (int day = 0; day < 7; ++day) {
    const int cx = rect.x + PADDING + day * columnWidth + columnWidth / 2;
    const int x = cx - dot / 2;
    if (day > stats.weekday) {
      renderer.drawRoundedRect(x, y, dot, dot, 1, dot / 2, true);
    } else if (stats.weekSeconds[day] >= READ_DAY_SECONDS) {
      renderer.fillRoundedRect(x, y, dot, dot, dot / 2, Color::Black);
    } else {
      renderer.drawRoundedRect(x, y, dot, dot, day == stats.weekday ? 3 : 2, dot / 2, true);
    }
    const char* label = I18N.get(labels[day]);
    renderer.drawText(SMALL_FONT_ID, cx - renderer.getTextWidth(SMALL_FONT_ID, label) / 2, y + dot + GAP, label);
  }
  y += dot + GAP + labelHeight + GAP;
  if (y + labelHeight <= rect.y + rect.height - PADDING / 2) {
    char streak[32];
    snprintf(streak, sizeof(streak), tr(STR_STREAK_FMT), static_cast<int>(stats.streak));
    renderer.drawText(UI_10_FONT_ID, rect.x + PADDING, y, streak);
  }
}

void drawGoal(const GfxRenderer& renderer, const Rect rect, const ReadingStatsSnapshot& stats, const int goalMinutes,
              const Messages& messages) {
  int y = drawTitle(renderer, rect, tr(STR_WIDGET_GOAL));
  if (!stats.reliable) {
    drawMessage(renderer, rect, y, messages.needClock);
    return;
  }
  const int minutes = static_cast<int>(stats.todaySeconds / 60);
  const int goal = std::max(1, goalMinutes);
  char value[12];
  snprintf(value, sizeof(value), "%d", minutes);
  char unit[24];
  snprintf(unit, sizeof(unit), tr(STR_GOAL_FMT), goal);
  y = drawBigValue(renderer, rect.x + PADDING, y, value, unit) + GAP;
  drawProgressBar(renderer, rect.x + PADDING, y, rect.width - 2 * PADDING, 10, static_cast<uint32_t>(minutes),
                  static_cast<uint32_t>(goal));
  y += 10 + GAP;
  if (minutes >= goal && y + renderer.getLineHeight(SMALL_FONT_ID) <= rect.y + rect.height) {
    renderer.drawText(SMALL_FONT_ID, rect.x + PADDING, y, tr(STR_GOAL_REACHED));
  }
}

void drawClock(const GfxRenderer& renderer, const Rect rect) {
  int y = drawTitle(renderer, rect, tr(STR_WIDGET_CLOCK));
  char time[12];
  struct tm local{};
  if (!localNow(local) || !formatClock(time, sizeof(time), local)) {
    renderer.drawText(UI_10_FONT_ID, rect.x + PADDING, y, tr(STR_NOT_SET));
    return;
  }
  y = drawBigValue(renderer, rect.x + PADDING, y, time, nullptr) + GAP;
  char date[32];
  snprintf(date, sizeof(date), "%s  %04d-%02d-%02d", I18N.get(WEEKDAYS[(local.tm_wday + 6) % 7]), local.tm_year + 1900,
           local.tm_mon + 1, local.tm_mday);
  renderer.drawText(UI_10_FONT_ID, rect.x + PADDING, y, date);
}

Rect imageArea(const Rect rect) {
  constexpr int INSET = 4;
  return Rect{rect.x + INSET, rect.y + INSET, std::max(0, rect.width - 2 * INSET),
              std::max(0, rect.height - 2 * INSET)};
}

void drawImage(const GfxRenderer& renderer, const Rect rect, const std::string& bmpPath, const Messages& messages) {
  const Rect area = imageArea(rect);
  bool drawn = false;
  HalFile file;
  if (!bmpPath.empty() && Storage.openFileForRead("THOME", bmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      if (widget_images::isCachePath(bmpPath)) {
        // Converted to cover the slot: draw 1:1, cropped to the area.
        drawn = BaseTheme::drawCoverThumbFill(renderer, bitmap, area);
      } else {
        drawn = renderer.drawBitmap(bitmap, area.x, area.y, area.width, area.height);
      }
    }
    file.close();
  }
  if (!drawn) {
    const int y = drawTitle(renderer, rect, tr(STR_WIDGET_IMAGE));
    drawMessage(renderer, rect, y, messages.noImage);
  }
}

void drawChartIcon(const GfxRenderer& renderer, const int x, const int y, const int size) {
  const int bar = std::max(3, size / 6);
  const int base = y + size - 3;
  renderer.fillRect(x + 2, base, size - 4, 2);
  const int heights[3] = {size * 2 / 5, size * 4 / 5, size * 3 / 5};
  for (int i = 0; i < 3; ++i) {
    const int bx = x + size / 6 + i * (bar + size / 8);
    renderer.fillRect(bx, base - heights[i], bar, heights[i]);
  }
}

}  // namespace thcraft_widgets
