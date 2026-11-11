#pragma once

#include <ctime>
#include <string>

#include "ReadingStatsStore.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Draw functions for the THCraft home widget slots. Each draws its frame and
// content inside `rect` from data captured when home opened; no I/O except the
// image widget's one cached BMP.
namespace thcraft_widgets {

// Message lines prepared (wrapped) once by the home screen.
struct Messages {
  std::string needClock[2];
  std::string noImage[2];
};

void drawFrame(const GfxRenderer& renderer, Rect rect, bool focused);
void drawToday(const GfxRenderer& renderer, Rect rect, const ReadingStatsSnapshot& stats, const Messages& messages);
void drawWeek(const GfxRenderer& renderer, Rect rect, const ReadingStatsSnapshot& stats, const Messages& messages);
void drawGoal(const GfxRenderer& renderer, Rect rect, const ReadingStatsSnapshot& stats, int goalMinutes,
              const Messages& messages);
// Formats like HalClock::formatTime, from a reading already taken.
void formatClock(char* out, size_t size, const struct tm& local);
// `local` is the frame's device-clock reading, or nullptr when there is no time.
void drawClock(const GfxRenderer& renderer, Rect rect, const struct tm* local);
// Image area inside the frame; the home screen converts the image to this size.
Rect imageArea(Rect rect);
void drawImage(const GfxRenderer& renderer, Rect rect, const std::string& bmpPath, const Messages& messages);

// Rounded outline with a solid fill for value / max.
void drawProgressBar(const GfxRenderer& renderer, int x, int y, int width, int height, uint32_t value, uint32_t max);
// Small bar-chart glyph for the Stats nav item.
void drawChartIcon(const GfxRenderer& renderer, int x, int y, int size);

}  // namespace thcraft_widgets
