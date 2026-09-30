#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "components/UITheme.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  renderer.clearScreen();
  GUI.drawMascotScreen(renderer, tr(STR_BOOTING), CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
