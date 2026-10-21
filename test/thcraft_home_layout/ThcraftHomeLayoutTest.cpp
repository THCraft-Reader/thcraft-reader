#include <gtest/gtest.h>

#include "components/themes/thcraft/ThcraftHomeLayout.h"

namespace {

ThcraftHomeLayoutInput inputFor(int width, int height, int hints, bool widgets = true, bool rightWidget = true) {
  ThcraftHomeLayoutInput in;
  in.content = Rect{0, 0, width, height};
  in.topPadding = 10;
  in.sidePadding = 20;
  in.buttonHintsHeight = hints;
  in.titleLineHeight = 22;
  in.bodyLineHeight = 19;
  in.smallLineHeight = 14;
  in.bigLineHeight = 34;
  in.leftWidget = widgets;
  in.rightWidget = widgets && rightWidget;
  return in;
}

int bottomOf(const Rect& r) { return r.y + r.height; }

void expectInside(const Rect& inner, const Rect& outer) {
  EXPECT_GE(inner.x, outer.x);
  EXPECT_GE(inner.y, outer.y);
  EXPECT_LE(inner.x + inner.width, outer.x + outer.width);
  EXPECT_LE(bottomOf(inner), bottomOf(outer));
}

void expectSane(const ThcraftHomeLayoutInput& in, const ThcraftHomeLayout& l) {
  const Rect screen = in.content;
  EXPECT_LT(bottomOf(l.header), l.card.y);
  expectInside(l.cardCover, l.card);
  expectInside(l.cardText, l.card);
  EXPECT_GT(l.cardText.width, l.cardCover.width);  // text keeps the larger share
  EXPECT_EQ(l.cardCover.width, l.cardCover.height * 3 / 5);
  int floor = bottomOf(l.card);
  if (l.showRecent) {
    EXPECT_GT(l.recentLabel.y, floor);
    for (int i = 0; i < ThcraftHomeLayout::RECENT_COUNT; ++i) {
      expectInside(l.recentCovers[i], l.recentSlots[i]);
      if (i > 0) EXPECT_GT(l.recentSlots[i].x, l.recentSlots[i - 1].x + l.recentSlots[i - 1].width);
    }
    EXPECT_EQ(l.recentThumbHeight(), l.recentCovers[0].height);
    floor = bottomOf(l.recentSlots[0]);
  }
  if (l.showWidgets) {
    EXPECT_GE(l.widgets[0].y, floor);
    EXPECT_GE(l.widgets[0].height, ThcraftHomeLayout::WIDGET_MIN_HEIGHT);
    EXPECT_LT(l.widgets[0].x + l.widgets[0].width, l.widgets[1].x);
    floor = bottomOf(l.widgets[0]);
  }
  EXPECT_LT(floor, l.nav.y);
  EXPECT_LE(bottomOf(l.nav), bottomOf(screen) - in.buttonHintsHeight);
  EXPECT_EQ(l.cardThumbHeight() % 2, 0);
}

TEST(ThcraftHomeLayout, X4PortraitShowsEverySection) {
  const auto in = inputFor(480, 800, 40);
  const auto l = computeThcraftHomeLayout(in);
  EXPECT_TRUE(l.showRecent);
  EXPECT_TRUE(l.showWidgets);
  EXPECT_EQ(l.widgets[0].height, ThcraftHomeLayout::WIDGET_PREFERRED_HEIGHT);
  EXPECT_GE(l.cardThumbHeight(), 200);
  EXPECT_GE(l.recentThumbHeight(), 120);
  expectSane(in, l);
}

TEST(ThcraftHomeLayout, X3AndEegoPanels) {
  for (const auto& in : {inputFor(528, 792, 40), inputFor(552, 768, 40), inputFor(480, 800, 0)}) {
    const auto l = computeThcraftHomeLayout(in);
    EXPECT_TRUE(l.showRecent);
    EXPECT_TRUE(l.showWidgets);
    expectSane(in, l);
  }
}

TEST(ThcraftHomeLayout, WidgetsOffGivesCoversTheRoom) {
  const auto on = computeThcraftHomeLayout(inputFor(480, 800, 40, true));
  const auto off = computeThcraftHomeLayout(inputFor(480, 800, 40, false));
  EXPECT_FALSE(off.showWidgets);
  EXPECT_GE(off.recentThumbHeight(), on.recentThumbHeight());
  EXPECT_LE(off.cardThumbHeight(), ThcraftHomeLayout::CARD_MAX_COVER);
  EXPECT_LE(off.recentThumbHeight(), ThcraftHomeLayout::RECENT_MAX_COVER);
}

TEST(ThcraftHomeLayout, LoneWidgetSpansTheRow) {
  const auto both = computeThcraftHomeLayout(inputFor(480, 800, 40));
  auto in = inputFor(480, 800, 40, true, false);
  const auto leftOnly = computeThcraftHomeLayout(in);
  EXPECT_TRUE(leftOnly.showWidgets);
  EXPECT_EQ(leftOnly.widgets[0].x, both.widgets[0].x);
  EXPECT_EQ(leftOnly.widgets[0].width, both.widgets[1].x + both.widgets[1].width - both.widgets[0].x);
  EXPECT_EQ(leftOnly.widgets[1].width, 0);
  in.leftWidget = false;
  in.rightWidget = true;
  const auto rightOnly = computeThcraftHomeLayout(in);
  EXPECT_EQ(rightOnly.widgets[0].width, 0);
  EXPECT_EQ(rightOnly.widgets[1].width, leftOnly.widgets[0].width);
  // Thumb heights do not depend on which slot is on.
  EXPECT_EQ(rightOnly.cardThumbHeight(), both.cardThumbHeight());
  EXPECT_EQ(rightOnly.recentThumbHeight(), both.recentThumbHeight());
}

TEST(ThcraftHomeLayout, ShortScreenDropsWidgetsThenRecent) {
  const auto landscape = computeThcraftHomeLayout(inputFor(800, 480, 40));
  EXPECT_FALSE(landscape.showWidgets);
  expectSane(inputFor(800, 480, 40), landscape);
  const auto tiny = computeThcraftHomeLayout(inputFor(480, 330, 40));
  EXPECT_FALSE(tiny.showWidgets);
  EXPECT_FALSE(tiny.showRecent);
  EXPECT_LT(bottomOf(tiny.card), tiny.nav.y);
}

TEST(ThcraftHomeLayout, ThumbHeightIgnoresContent) {
  const auto a = computeThcraftHomeLayout(inputFor(480, 800, 40));
  const auto b = computeThcraftHomeLayout(inputFor(480, 800, 40));
  EXPECT_EQ(a.cardThumbHeight(), b.cardThumbHeight());
  EXPECT_EQ(a.recentThumbHeight(), b.recentThumbHeight());
}

}  // namespace
