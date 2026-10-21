#include "ThcraftTheme.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

int ThcraftTheme::bigFontId() {
#ifdef OMIT_FONTS
  return UI_12_FONT_ID;
#else
  return NOTOSANS_18_FONT_ID;
#endif
}

ThcraftHomeLayout ThcraftTheme::homeLayout(const GfxRenderer& renderer) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  ThcraftHomeLayoutInput in;
  in.content = UITheme::getContentArea(renderer);
  in.topPadding = metrics.topPadding;
  in.sidePadding = metrics.contentSidePadding;
  in.buttonHintsHeight = metrics.buttonHintsHeight;
  in.titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  in.bodyLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  in.smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  in.bigLineHeight = renderer.getLineHeight(bigFontId());
  in.leftWidget = SETTINGS.homeWidgetLeft != CrossPointSettings::HOME_WIDGET_OFF;
  in.rightWidget = SETTINGS.homeWidgetRight != CrossPointSettings::HOME_WIDGET_OFF;
  return computeThcraftHomeLayout(in);
}

int ThcraftTheme::homeCoverThumbHeight(const GfxRenderer& renderer) const {
  return homeLayout(renderer).cardThumbHeight();
}
