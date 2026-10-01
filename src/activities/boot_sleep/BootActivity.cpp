#include "BootActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const bool dark = SETTINGS.screenInverted != 0;
  // Boot draws directly from onEnter, before the per-render polarity hook.
  display.setInverted(dark);
  renderer.clearScreen();
  GUI.drawMascotScreen(renderer, tr(STR_BOOTING), dark, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
