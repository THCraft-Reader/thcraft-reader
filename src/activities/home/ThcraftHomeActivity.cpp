#include "ThcraftHomeActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "SdCardFontSystem.h"
#include "ThcraftHomeAssets.h"
#include "components/UITheme.h"
#include "components/icons/blocks.h"
#include "components/icons/book.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/themes/thcraft/ThcraftTheme.h"
#include "fontIds.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;

namespace {

constexpr fui::ActionId TOUCH_ACTION = 1;
constexpr int TOUCH_ZONE_STRIDE = 8;
constexpr int CARD_RADIUS = 10;
constexpr int TEXT_GAP = 4;
constexpr int ICON_SIZE = 32;

fui::Rect toFui(const Rect& r) {
  return fui::Rect{static_cast<int16_t>(r.x), static_cast<int16_t>(r.y), static_cast<int16_t>(r.width),
                   static_cast<int16_t>(r.height)};
}

const char* displayTitle(const RecentBook& book) {
  if (!book.title.empty()) return book.title.c_str();
  const size_t slash = book.path.find_last_of('/');
  return slash == std::string::npos ? book.path.c_str() : book.path.c_str() + slash + 1;
}

}  // namespace

ThcraftHomeActivity::ThcraftHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         const HomeMenuItem initialMenuItem, const bool cleanInitialRefresh)
    : Activity("Home", renderer, mappedInput),
      UiAppHost(renderer),
      coverCache(renderer),
      initialMenuItem(initialMenuItem),
      cleanInitialRefresh(cleanInitialRefresh) {}

void ThcraftHomeActivity::onEnter() {
  Activity::onEnter();
  firstRenderDone.store(false, std::memory_order_relaxed);
  assetsLoaded.store(false, std::memory_order_relaxed);
  progressLoaded = false;
  hasOpdsServers = OPDS_STORE.hasServers();
  hasPlugins = SETTINGS.pluginsEnabled != 0;

  layout = ThcraftTheme::homeLayout(renderer);
  loadBooks();
  prepareWidgets();
  prepareText();

  navCount = 0;
  navItems[navCount++] = NavItem::Library;
  navItems[navCount++] = NavItem::Files;
  navItems[navCount++] = NavItem::Stats;
  navItems[navCount++] = NavItem::Transfer;
  if (hasPlugins || hasOpdsServers) navItems[navCount++] = NavItem::Online;
  navItems[navCount++] = NavItem::Settings;
  const int navSlotWidth = layout.nav.width / navCount;
  for (int i = 0; i < navCount; ++i) {
    navLabels[i] = renderer.truncatedText(SMALL_FONT_ID, navLabel(navItems[i]), navSlotWidth - 4);
  }
  applyInitialFocus();

  // Snapshots need PSRAM; without it covers redraw from their small 1-bit BMPs.
  if (!books.empty() && UITheme::supportsCoverGrid()) coverCache.begin();
  resetUi();
  app.on(TOUCH_ACTION, &ThcraftHomeActivity::onTouchAction, this);
  app.setScreen(&ThcraftHomeActivity::screenFn, this);
  requestUpdate();
}

const char* ThcraftHomeActivity::navLabel(const NavItem item) const {
  switch (item) {
    case NavItem::Library:
      return tr(STR_LIBRARY);
    case NavItem::Files:
      return tr(STR_NAV_FILES);
    case NavItem::Stats:
      return tr(STR_NAV_STATS);
    case NavItem::Transfer:
      return tr(STR_NAV_TRANSFER);
    case NavItem::Online:
      return hasPlugins ? tr(STR_PLUGINS) : tr(STR_NAV_OPDS);
    case NavItem::Settings:
      return tr(STR_SETTINGS_TITLE);
  }
  return "";
}

void ThcraftHomeActivity::onExit() {
  Activity::onExit();
  closeRouting();
  books.clear();
  books.shrink_to_fit();
  for (auto& text : bookText) text = BookText{};
  for (auto& path : coverPaths) std::string().swap(path);
  for (auto& path : widgetImages) std::string().swap(path);
  for (auto& label : navLabels) std::string().swap(label);
}

void ThcraftHomeActivity::loadBooks() {
  books.clear();
  books.reserve(MAX_BOOKS);
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (static_cast<int>(books.size()) >= MAX_BOOKS) break;
    if (RecentBooksStore::isMissing(book)) continue;
    books.push_back(book);
  }
  for (size_t i = 0; i < books.size(); ++i) {
    thcraft_home::resolveCoverPath(books[i]);
    coverPaths[i] = thcraft_home::thumbPath(books[i], i == 0 ? layout.cardThumbHeight() : layout.recentThumbHeight());
    savedPosition[i] = thcraft_home::hasSavedPosition(books[i]);
    // A cleared cache drops the reader's position; the stored copy would be stale.
    if (!savedPosition[i]) {
      books[i].progressBp = RecentBook::PROGRESS_UNKNOWN;
      books[i].chapterIndex = books[i].chapterCount = 0;
    }
  }
}

void ThcraftHomeActivity::prepareText() {
  for (size_t i = 0; i < books.size(); ++i) {
    BookText& text = bookText[i];
    const char* title = displayTitle(books[i]);
    if (i == 0) {
      const auto lines = renderer.wrappedText(UI_12_FONT_ID, title, layout.cardText.width, 2);
      for (size_t line = 0; line < lines.size() && line < 2; ++line) text.title[line] = lines[line];
      if (!books[i].author.empty()) {
        text.author = renderer.truncatedText(UI_10_FONT_ID, books[i].author.c_str(), layout.cardText.width);
      }
    } else {
      text.title[0] = renderer.truncatedText(UI_10_FONT_ID, title, layout.recentSlots[i - 1].width);
    }
  }
  // Wrap for the narrower visible slot so one set of lines fits either.
  int narrowest = 0;
  for (const Rect& widget : layout.widgets) {
    if (widget.width > 0) narrowest = narrowest == 0 ? widget.width : std::min(narrowest, widget.width);
  }
  const int messageWidth = std::max(0, narrowest - 20);
  const auto wrapInto = [this, messageWidth](StrId id, std::string(&out)[2]) {
    const auto lines = renderer.wrappedText(SMALL_FONT_ID, I18N.get(id), messageWidth, 2);
    for (size_t i = 0; i < lines.size() && i < 2; ++i) out[i] = lines[i];
  };
  wrapInto(StrId::STR_STATS_NEED_CLOCK, messages.needClock);
  wrapInto(StrId::STR_NO_WIDGET_IMAGE, messages.noImage);
  noOtherBooks = renderer.truncatedText(UI_10_FONT_ID, tr(STR_NO_OTHER_BOOKS), layout.recentLabel.width);
}

void ThcraftHomeActivity::prepareWidgets() {
  stats = READING_STATS.snapshot();
  const char* names[ThcraftHomeLayout::WIDGET_COUNT] = {SETTINGS.homeWidgetLeftImage, SETTINGS.homeWidgetRightImage};
  for (int slot = 0; slot < ThcraftHomeLayout::WIDGET_COUNT; ++slot) {
    widgetImages[slot].clear();
    if (!layout.showWidgets || widgetType(slot) != CrossPointSettings::HOME_WIDGET_IMAGE) continue;
    const Rect area = thcraft_widgets::imageArea(layout.widgets[slot]);
    widgetImages[slot] = thcraft_home::widgetImagePath(names[slot], area.width, area.height);
  }
}

int ThcraftHomeActivity::widgetType(const int slot) const {
  const uint8_t type = slot == 0 ? SETTINGS.homeWidgetLeft : SETTINGS.homeWidgetRight;
  return type < CrossPointSettings::HOME_WIDGET_COUNT ? type : CrossPointSettings::HOME_WIDGET_OFF;
}

bool ThcraftHomeActivity::widgetActionable(const int slot) const {
  return layout.showWidgets && widgetType(slot) != CrossPointSettings::HOME_WIDGET_OFF;
}

int ThcraftHomeActivity::widgetSlotAt(const int index) const {
  int seen = 0;
  for (int slot = 0; slot < ThcraftHomeLayout::WIDGET_COUNT; ++slot) {
    if (!widgetActionable(slot)) continue;
    if (seen++ == index) return slot;
  }
  return -1;
}

int ThcraftHomeActivity::zoneCount(const Zone zone) const {
  switch (zone) {
    case Zone::Card:
      return books.empty() ? 0 : 1;
    case Zone::Recent:
      return layout.showRecent ? std::min<int>(ThcraftHomeLayout::RECENT_COUNT, std::max<int>(0, books.size() - 1)) : 0;
    case Zone::Widgets: {
      int count = 0;
      for (int slot = 0; slot < ThcraftHomeLayout::WIDGET_COUNT; ++slot) count += widgetActionable(slot) ? 1 : 0;
      return count;
    }
    case Zone::Nav:
      return navCount;
  }
  return 0;
}

void ThcraftHomeActivity::applyInitialFocus() {
  NavItem target = NavItem::Library;
  bool toNav = true;
  switch (initialMenuItem) {
    case HomeMenuItem::NONE:
      toNav = books.empty();
      break;
    case HomeMenuItem::FILE_BROWSER:
      target = NavItem::Files;
      break;
    case HomeMenuItem::LIBRARY:
      target = NavItem::Library;
      break;
    case HomeMenuItem::OPDS_BROWSER:
      target = NavItem::Online;
      break;
    case HomeMenuItem::FILE_TRANSFER:
      target = NavItem::Transfer;
      break;
    case HomeMenuItem::SETTINGS_MENU:
      target = NavItem::Settings;
      break;
    case HomeMenuItem::STATS_SCREEN:
      target = NavItem::Stats;
      break;
  }
  zoneIndex.fill(0);
  if (!toNav) {
    focusZone = Zone::Card;
    return;
  }
  focusZone = Zone::Nav;
  for (int i = 0; i < navCount; ++i) {
    if (navItems[i] == target) zoneIndex[static_cast<int>(Zone::Nav)] = i;
  }
}

void ThcraftHomeActivity::moveWithinZone(const int direction) {
  int& index = zoneIndex[static_cast<int>(focusZone)];
  const int next = index + direction;
  if (next >= 0 && next < zoneCount(focusZone)) {
    index = next;
    requestUpdate();
    return;
  }
  // Past either end: continue into the neighbouring zone, so front buttons alone reach everything.
  for (int step = 1; step <= ZONE_COUNT; ++step) {
    const auto zone = static_cast<Zone>((static_cast<int>(focusZone) + direction * step + ZONE_COUNT) % ZONE_COUNT);
    const int count = zoneCount(zone);
    if (count == 0) continue;
    focusZone = zone;
    zoneIndex[static_cast<int>(zone)] = direction > 0 ? 0 : count - 1;
    requestUpdate();
    return;
  }
}

void ThcraftHomeActivity::moveZone(const int direction) {
  for (int step = 1; step < ZONE_COUNT; ++step) {
    const auto zone = static_cast<Zone>((static_cast<int>(focusZone) + direction * step + ZONE_COUNT) % ZONE_COUNT);
    const int count = zoneCount(zone);
    if (count == 0) continue;
    focusZone = zone;
    int& index = zoneIndex[static_cast<int>(zone)];
    index = std::min(index, count - 1);
    requestUpdate();
    return;
  }
}

bool ThcraftHomeActivity::focused(const Zone zone, const int index) const {
  // Touch boards tap directly; a cursor would only mark the last tap.
  return !BoardConfig::hasTouch() && focusZone == zone && zoneIndex[static_cast<int>(zone)] == index;
}

void ThcraftHomeActivity::activate(const Zone zone, const int index) {
  switch (zone) {
    case Zone::Card:
      if (!books.empty()) activityManager.goToReader(books[0].path);
      return;
    case Zone::Recent:
      if (index >= 0 && index + 1 < static_cast<int>(books.size())) activityManager.goToReader(books[index + 1].path);
      return;
    case Zone::Widgets: {
      const int slot = widgetSlotAt(index);
      if (slot < 0) return;
      const int type = widgetType(slot);
      if (type == CrossPointSettings::HOME_WIDGET_CLOCK || type == CrossPointSettings::HOME_WIDGET_IMAGE) {
        activityManager.goToSettings(SettingsSection::Display);
      } else {
        activityManager.goToReadingStats();
      }
      return;
    }
    case Zone::Nav:
      if (index < 0 || index >= navCount) return;
      switch (navItems[index]) {
        case NavItem::Library:
          activityManager.goToLibrary();
          return;
        case NavItem::Files:
          activityManager.goToFileBrowser();
          return;
        case NavItem::Stats:
          activityManager.goToReadingStats();
          return;
        case NavItem::Transfer:
          activityManager.goToFileTransfer();
          return;
        case NavItem::Online:
          if (hasPlugins) {
            activityManager.goToPlugins(hasOpdsServers);
          } else {
            activityManager.goToBrowser();
          }
          return;
        case NavItem::Settings:
          activityManager.goToSettings();
          return;
      }
      return;
  }
}

void ThcraftHomeActivity::onTouchAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<ThcraftHomeActivity*>(user);
  self.pendingTouch = event.value;
  self.app.clearTapFlash();
}

void ThcraftHomeActivity::screenFn(UiScreen& screen, void* user) {
  // Only touch targets: the visuals are drawn with GfxRenderer before renderUi().
  auto& self = *static_cast<ThcraftHomeActivity*>(user);
  auto& frame = screen.frame();
  const auto hit = [&frame](const Rect& rect, const Zone zone, const int index) {
    frame.hit(toFui(rect), TOUCH_ACTION, static_cast<int16_t>(static_cast<int>(zone) * TOUCH_ZONE_STRIDE + index),
              fui::InputTouch);
  };
  if (!self.books.empty()) hit(self.layout.card, Zone::Card, 0);
  for (int i = 0; i < self.zoneCount(Zone::Recent); ++i) hit(self.layout.recentSlots[i], Zone::Recent, i);
  for (int i = 0; i < self.zoneCount(Zone::Widgets); ++i) {
    hit(self.layout.widgets[self.widgetSlotAt(i)], Zone::Widgets, i);
  }
  const int slotWidth = self.navCount > 0 ? self.layout.nav.width / self.navCount : 0;
  for (int i = 0; i < self.navCount; ++i) {
    hit(Rect{self.layout.nav.x + i * slotWidth, self.layout.nav.y, slotWidth, self.layout.nav.height}, Zone::Nav, i);
  }
}

void ThcraftHomeActivity::loop() {
  pendingTouch = -1;
  const auto touch = routeTouch(mappedInput);
  if (touch.snap.touchReleased && pendingTouch >= 0) {
    activate(static_cast<Zone>(pendingTouch / TOUCH_ZONE_STRIDE), pendingTouch % TOUCH_ZONE_STRIDE);
    return;
  }

  // Back opens the book being read, as on the standard home.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !books.empty()) {
    activityManager.goToReader(books[0].path);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(focusZone, zoneIndex[static_cast<int>(focusZone)]);
    return;
  }
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { moveWithinZone(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { moveWithinZone(+1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { moveZone(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { moveZone(+1); });

  if (assetsLoaded.load(std::memory_order_acquire) && !progressLoaded) {
    progressLoaded = true;
    loadMissingProgress();
  }

  // A visible clock widget repaints when the minute changes; nothing else ticks.
  const bool clockShown = layout.showWidgets && (widgetType(0) == CrossPointSettings::HOME_WIDGET_CLOCK ||
                                                 widgetType(1) == CrossPointSettings::HOME_WIDGET_CLOCK);
  const int64_t minute = static_cast<int64_t>(time(nullptr)) / 60;
  if (clockShown && firstRenderDone.load(std::memory_order_acquire) && minute != lastClockMinute) {
    if (lastClockMinute >= 0) requestUpdate();
    lastClockMinute = minute;
  }
}

void ThcraftHomeActivity::loadMissingProgress() {
  // Entries written before progress was stored in recents: read the book's saved position once and keep it.
  bool changed = false;
  {
    RenderLock lock(*this);
    for (size_t i = 0; i < books.size(); ++i) {
      RecentBook& book = books[i];
      if (book.hasProgress() || !savedPosition[i]) continue;
      const int percent = loadBookProgress(book.path);
      if (percent < 0) continue;
      book.progressBp = static_cast<uint16_t>(std::clamp(percent, 0, 100) * 100);
      RECENT_BOOKS.updateProgress(book.path, book.progressBp, 0, 0);
      changed = true;
    }
  }
  if (changed) requestUpdate();
}

bool ThcraftHomeActivity::loadMissingAssets() {
  bool showingLoading = false;
  {
    // render() holds RenderLock for the whole batch, including font restoration.
    SdCardFontSystem::CoverDecodeScope fonts(sdFontSystem, renderer);
    const auto beginLoading = [&] {
      if (showingLoading) return;
      showingLoading = true;
      GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      fonts.releaseFonts();
    };
    for (size_t i = 0; i < books.size(); ++i) {
      const int height = i == 0 ? layout.cardThumbHeight() : layout.recentThumbHeight();
      if (i > 0 && !layout.showRecent) break;
      if (!thcraft_home::needsThumb(books[i], height)) continue;
      beginLoading();
      // The framebuffer is decode scratch while the panel keeps showing the popup.
      GfxRenderer::FrameBufferLoan loan(renderer);
      thcraft_home::generateThumb(books[i], height);
      coverPaths[i] = thcraft_home::thumbPath(books[i], height);
    }
    const char* names[ThcraftHomeLayout::WIDGET_COUNT] = {SETTINGS.homeWidgetLeftImage, SETTINGS.homeWidgetRightImage};
    for (int slot = 0; slot < ThcraftHomeLayout::WIDGET_COUNT; ++slot) {
      if (!thcraft_home::needsWidgetImage(widgetImages[slot])) continue;
      beginLoading();
      const Rect area = thcraft_widgets::imageArea(layout.widgets[slot]);
      GfxRenderer::FrameBufferLoan loan(renderer);
      thcraft_home::generateWidgetImage(names[slot], widgetImages[slot], area.width, area.height);
    }
  }
  if (!widgetImages[0].empty() || !widgetImages[1].empty()) {
    thcraft_home::pruneWidgetImages(widgetImages[0], widgetImages[1]);
  }
  if (showingLoading) coverCache.invalidate();
  return showingLoading;
}

void ThcraftHomeActivity::drawHeader() {
  const Rect& h = layout.header;
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int titleHeight = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, h.x, h.y, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, h.x, h.y + titleHeight, tr(STR_THCRAFT_TAGLINE));

  const uint16_t percent = powerManager.getBatteryPercentage();
  int x = h.x + h.width - metrics.batteryWidth;
  const int iconY = h.y + (titleHeight - metrics.batteryHeight) / 2;
  BaseTheme::drawBatteryOutline(renderer, x, iconY, metrics.batteryWidth, metrics.batteryHeight);
  GUI.fillBatteryIcon(renderer, Rect{x, iconY, metrics.batteryWidth, metrics.batteryHeight}, percent);
  if (SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_ALWAYS) {
    char text[8];
    snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(percent));
    x -= renderer.getTextWidth(UI_10_FONT_ID, text) + 6;
    renderer.drawText(UI_10_FONT_ID, x, h.y + 2, text);
  }
  char time[12];
  struct tm local{};
  if (thcraft_widgets::localNow(local) && thcraft_widgets::formatClock(time, sizeof(time), local)) {
    x -= renderer.getTextWidth(UI_10_FONT_ID, time) + 14;
    renderer.drawText(UI_10_FONT_ID, x, h.y + 2, time);
  }
}

void ThcraftHomeActivity::drawCard() {
  const Rect& c = layout.card;
  renderer.drawRoundedRect(c.x, c.y, c.width, c.height, focused(Zone::Card, 0) ? 3 : 1, CARD_RADIUS, true);
  const Rect& t = layout.cardText;
  if (books.empty()) {
    const int y = c.y + (c.height - ICON_SIZE - 2 * renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawIcon(BookIcon, c.x + (c.width - ICON_SIZE) / 2, y, ICON_SIZE);
    UITheme::drawCenteredText(renderer, c, UI_12_FONT_ID, y + ICON_SIZE + TEXT_GAP, tr(STR_NO_OPEN_BOOK), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, c, UI_10_FONT_ID,
                              y + ICON_SIZE + TEXT_GAP + renderer.getLineHeight(UI_12_FONT_ID), tr(STR_START_READING));
    return;
  }

  coverCache.paint(toFui(layout.cardCover), 0, coverPaths[0]);
  const RecentBook& book = books[0];
  const BookText& text = bookText[0];
  int y = t.y;
  renderer.drawText(SMALL_FONT_ID, t.x, y, tr(STR_CONTINUE_READING));
  y += renderer.getLineHeight(SMALL_FONT_ID) + TEXT_GAP;
  for (const auto& line : text.title) {
    if (line.empty()) continue;
    renderer.drawText(UI_12_FONT_ID, t.x, y, line.c_str());
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }
  if (!text.author.empty()) renderer.drawText(UI_10_FONT_ID, t.x, y + TEXT_GAP, text.author.c_str());

  // Progress block anchored to the bottom of the text column.
  const int smallHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int bottom = t.y + t.height;
  const int chapterY = bottom - smallHeight;
  if (book.progressBp >= 10000) {
    renderer.drawText(SMALL_FONT_ID, t.x, chapterY, tr(STR_BOOK_FINISHED));
  } else if (book.chapterCount > 0) {
    char chapter[48];
    snprintf(chapter, sizeof(chapter), tr(STR_CHAPTER_OF), static_cast<int>(book.chapterIndex),
             static_cast<int>(book.chapterCount));
    renderer.drawText(SMALL_FONT_ID, t.x, chapterY, chapter);
  }
  if (!book.hasProgress()) return;
  constexpr int BAR_HEIGHT = 8;
  const int barY = chapterY - TEXT_GAP - BAR_HEIGHT;
  thcraft_widgets::drawProgressBar(renderer, t.x, barY, t.width, BAR_HEIGHT, book.progressBp, 10000);
  const int bigFont = ThcraftTheme::bigFontId();
  const int percentY = barY - TEXT_GAP - renderer.getLineHeight(bigFont);
  char value[8];
  snprintf(value, sizeof(value), "%d", book.progressBp / 100);
  renderer.drawText(bigFont, t.x, percentY, value, true, EpdFontFamily::BOLD);
  const int unitX = t.x + renderer.getTextWidth(bigFont, value, EpdFontFamily::BOLD) + 2;
  renderer.drawText(UI_12_FONT_ID, unitX,
                    percentY + renderer.getLineHeight(bigFont) - renderer.getLineHeight(UI_12_FONT_ID), "%", true,
                    EpdFontFamily::BOLD);
}

void ThcraftHomeActivity::drawRecent() {
  renderer.drawText(UI_10_FONT_ID, layout.recentLabel.x, layout.recentLabel.y, tr(STR_RECENT_READ), true,
                    EpdFontFamily::BOLD);
  const int count = zoneCount(Zone::Recent);
  if (count == 0) {
    const Rect& row = layout.recentSlots[0];
    renderer.drawText(UI_10_FONT_ID, row.x, row.y + TEXT_GAP, noOtherBooks.c_str());
    return;
  }
  const int bodyHeight = renderer.getLineHeight(UI_10_FONT_ID);
  for (int i = 0; i < ThcraftHomeLayout::RECENT_COUNT; ++i) {
    const Rect& slot = layout.recentSlots[i];
    const Rect& cover = layout.recentCovers[i];
    if (i >= count) {
      renderer.drawRoundedRect(cover.x, cover.y, cover.width, cover.height, 1, 4, true);
      continue;
    }
    coverCache.paint(toFui(cover), i + 1, coverPaths[i + 1]);
    int y = cover.y + cover.height + TEXT_GAP;
    renderer.drawText(UI_10_FONT_ID, slot.x, y, bookText[i + 1].title[0].c_str());
    y += bodyHeight + TEXT_GAP;
    drawProgress(slot.x, y, slot.width, books[i + 1]);
    if (focused(Zone::Recent, i)) {
      renderer.drawRoundedRect(slot.x - 4, slot.y - 4, slot.width + 8, slot.height + 8, 2, 6, true);
    }
  }
}

void ThcraftHomeActivity::drawProgress(const int x, const int y, const int width, const RecentBook& book) {
  if (!book.hasProgress()) return;
  char text[24];
  if (book.progressBp >= 10000) {
    snprintf(text, sizeof(text), "%s", tr(STR_BOOK_FINISHED));
  } else {
    snprintf(text, sizeof(text), "%d%%", book.progressBp / 100);
  }
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, text);
  const int barWidth = std::max(0, width - textWidth - 6);
  constexpr int BAR_HEIGHT = 6;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  thcraft_widgets::drawProgressBar(renderer, x, y + (lineHeight - BAR_HEIGHT) / 2, barWidth, BAR_HEIGHT,
                                   book.progressBp, 10000);
  renderer.drawText(SMALL_FONT_ID, x + width - textWidth, y, text);
}

void ThcraftHomeActivity::drawWidgets() {
  int actionable = 0;
  for (int slot = 0; slot < ThcraftHomeLayout::WIDGET_COUNT; ++slot) {
    const int type = widgetType(slot);
    if (type == CrossPointSettings::HOME_WIDGET_OFF) continue;
    const Rect& rect = layout.widgets[slot];
    thcraft_widgets::drawFrame(renderer, rect, focused(Zone::Widgets, actionable++));
    switch (type) {
      case CrossPointSettings::HOME_WIDGET_IMAGE:
        thcraft_widgets::drawImage(renderer, rect, widgetImages[slot], messages);
        break;
      case CrossPointSettings::HOME_WIDGET_TODAY:
        thcraft_widgets::drawToday(renderer, rect, stats, messages);
        break;
      case CrossPointSettings::HOME_WIDGET_WEEK:
        thcraft_widgets::drawWeek(renderer, rect, stats, messages);
        break;
      case CrossPointSettings::HOME_WIDGET_GOAL:
        thcraft_widgets::drawGoal(renderer, rect, stats, SETTINGS.readingGoalMinutes, messages);
        break;
      case CrossPointSettings::HOME_WIDGET_CLOCK:
        thcraft_widgets::drawClock(renderer, rect);
        break;
      default:
        break;
    }
  }
}

void ThcraftHomeActivity::drawNav() {
  const Rect& nav = layout.nav;
  renderer.fillRectDither(nav.x, nav.y, nav.width, 1, Color::DarkGray);
  if (navCount == 0) return;
  const int slotWidth = nav.width / navCount;
  for (int i = 0; i < navCount; ++i) {
    const int slotX = nav.x + i * slotWidth;
    const int cx = slotX + slotWidth / 2;
    if (focused(Zone::Nav, i)) {
      renderer.fillRoundedRect(slotX + 3, nav.y + 3, slotWidth - 6, nav.height - 4, 8, Color::LightGray);
      renderer.fillRect(cx - 14, nav.y + nav.height - 3, 28, 3);
    }
    const int iconX = cx - ICON_SIZE / 2;
    const int iconY = nav.y + TEXT_GAP + 2;
    switch (navItems[i]) {
      case NavItem::Library:
        renderer.drawIcon(LibraryIcon, iconX, iconY, ICON_SIZE);
        break;
      case NavItem::Files:
        renderer.drawIcon(FolderIcon, iconX, iconY, ICON_SIZE);
        break;
      case NavItem::Stats:
        thcraft_widgets::drawChartIcon(renderer, iconX, iconY, ICON_SIZE);
        break;
      case NavItem::Transfer:
        renderer.drawIcon(TransferIcon, iconX, iconY, ICON_SIZE);
        break;
      case NavItem::Online:
        renderer.drawIcon(BlocksIcon, iconX, iconY, ICON_SIZE);
        break;
      case NavItem::Settings:
        renderer.drawIcon(Settings2Icon, iconX, iconY, ICON_SIZE);
        break;
    }
    const std::string& label = navLabels[i];
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
    renderer.drawText(SMALL_FONT_ID, cx - labelWidth / 2, iconY + ICON_SIZE + TEXT_GAP, label.c_str());
  }
}

void ThcraftHomeActivity::render(RenderLock&&) {
  const unsigned long started = millis();
  renderer.clearScreen();
  coverCache.prepare();
  drawHeader();
  drawCard();
  if (layout.showRecent) drawRecent();
  if (layout.showWidgets) drawWidgets();
  drawNav();
  renderUi();

  const auto labels =
      mappedInput.mapLabels(books.empty() ? "" : tr(STR_RESUME), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  const bool firstFrame = !firstRenderDone.load(std::memory_order_relaxed);
  renderer.displayBuffer(cleanInitialRefresh && firstFrame ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  LOG_DBG("THOME", "render %lu ms", millis() - started);

  if (firstFrame) {
    firstRenderDone.store(true, std::memory_order_release);
    requestUpdate();
  } else if (!assetsLoaded.load(std::memory_order_relaxed)) {
    const bool generated = loadMissingAssets();
    // Published after the batch so loop() does not queue on RenderLock behind it.
    assetsLoaded.store(true, std::memory_order_release);
    if (generated) requestUpdate();
  }
}
