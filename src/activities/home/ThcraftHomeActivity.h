#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "ThcraftHomeWidgets.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "components/HomeCoverCache.h"
#include "components/UiAppHost.h"
#include "components/themes/thcraft/ThcraftHomeLayout.h"
#include "util/ButtonNavigator.h"

// THCraft theme home: the book being read, three recent books, two widget
// slots and an icon bar. Drawn with GfxRenderer; UiAppHost only carries the
// touch targets. Physical buttons move focus within a zone (Left/Right) and
// between zones (Up/Down).
class ThcraftHomeActivity final : public Activity, private UiAppHost {
 public:
  ThcraftHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HomeMenuItem initialMenuItem,
                      bool cleanInitialRefresh);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  enum class Zone : uint8_t { Card, Recent, Widgets, Nav };
  static constexpr int ZONE_COUNT = 4;
  enum class NavItem : uint8_t { Library, Files, Stats, Transfer, Online, Settings };
  static constexpr int MAX_NAV = 6;
  static constexpr int MAX_BOOKS = 1 + ThcraftHomeLayout::RECENT_COUNT;

  struct BookText {
    std::string title[2];
    std::string author;
  };

  static void screenFn(UiScreen& screen, void* user);
  static void onTouchAction(const freeink::ui::ActionEvent& event, void* user);

  void loadBooks();
  void prepareText();
  void prepareWidgets();
  void applyInitialFocus();
  int zoneCount(Zone zone) const;
  void moveWithinZone(int direction);
  void moveZone(int direction);
  void activate(Zone zone, int index);
  bool loadMissingAssets();
  void loadMissingProgress();
  const char* navLabel(NavItem item) const;
  int widgetType(int slot) const;
  bool widgetActionable(int slot) const;
  int widgetSlotAt(int index) const;

  void drawHeader();
  void drawCard();
  void drawRecent();
  void drawWidgets();
  void drawNav();
  void drawProgress(int x, int y, int width, const RecentBook& book);
  bool focused(Zone zone, int index) const;

  ButtonNavigator buttonNavigator;
  HomeCoverCache coverCache;
  ThcraftHomeLayout layout;
  std::vector<RecentBook> books;  // [0] is the book being read
  std::array<BookText, MAX_BOOKS> bookText;
  std::array<std::string, MAX_BOOKS> coverPaths;
  std::array<NavItem, MAX_NAV> navItems{};
  std::array<std::string, MAX_NAV> navLabels;
  int navCount = 0;
  std::array<bool, MAX_BOOKS> savedPosition{};
  ReadingStatsSnapshot stats;
  thcraft_widgets::Messages messages;
  std::array<std::string, ThcraftHomeLayout::WIDGET_COUNT> widgetImages;
  std::string noOtherBooks;

  Zone focusZone = Zone::Card;
  std::array<int, ZONE_COUNT> zoneIndex{};
  int pendingTouch = -1;
  int64_t lastClockMinute = -1;
  bool hasOpdsServers = false;
  bool hasPlugins = false;
  // Written by the render task, read by loop().
  std::atomic<bool> firstRenderDone{false};
  std::atomic<bool> assetsLoaded{false};
  bool progressLoaded = false;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;
};
