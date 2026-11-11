#pragma once

#include <string>

#include "ReadingStatsStore.h"
#include "activities/Activity.h"

// Read-only summary of ReadingStatsStore, opened from the THCraft home. Only
// Back is interactive; the header back tap folds into Button::Back.
class ReadingStatsActivity final : public Activity {
 public:
  ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ReadingStatsSnapshot stats;
  std::string needClock[2];
};
