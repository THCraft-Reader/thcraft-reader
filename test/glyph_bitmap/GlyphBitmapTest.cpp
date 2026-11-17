#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "lib/GfxRenderer/GlyphBitmap.h"

namespace {
constexpr int PANEL_WIDTH = 40;
constexpr int PANEL_HEIGHT = 32;
constexpr int STRIDE = PANEL_WIDTH / 8;

using glyphBitmap::Plane;

// Logical placements used by the renderer: unrotated text, and side-button
// labels rotated 90 degrees clockwise (glyph x runs up, glyph y runs right).
constexpr std::array<std::array<int, 4>, 2> AXES = {{{1, 0, 0, 1}, {0, -1, 1, 0}}};

std::pair<int, int> physical(int orientation, int x, int y) {
  switch (orientation) {
    case 0:
      return {y, PANEL_HEIGHT - 1 - x};
    case 1:
      return {PANEL_WIDTH - 1 - x, PANEL_HEIGHT - 1 - y};
    case 2:
      return {PANEL_WIDTH - 1 - y, x};
    default:
      return {x, y};
  }
}

// Logical rectangle the renderer clips to; the glyph-local clip is derived from it.
struct Rect {
  int left;
  int top;
  int right;
  int bottom;
};

// Raw source value: 2bpp 0 (white) .. 3 (black), 1bpp 0 or 1; white outside the glyph.
int sourceValue(const std::vector<uint8_t>& bitmap, bool twoBit, int width, int height, int gx, int gy) {
  if (gx < 0 || gx >= width || gy < 0 || gy >= height) return 0;
  const int source = gy * width + gx;
  return twoBit ? (bitmap[source / 4] >> ((3 - source % 4) * 2)) & 3 : (bitmap[source / 8] >> (7 - source % 8)) & 1;
}

// Plain restatement of drawWeighted()'s filter.
int weightedValue(const std::vector<uint8_t>& bitmap, bool twoBit, int width, int height, int gx, int gy, int weight) {
  const auto at = [&](int x, int y) { return sourceValue(bitmap, twoBit, width, height, x, y); };
  const int current = at(gx, gy);
  if (weight == 0) return current;
  if (weight > 0) {
    int ink = current;
    if (at(gx + 1, gy) <= current) {
      for (int k = 1; k <= weight; ++k) ink = std::max(ink, at(gx - k, gy));
    }
    // Past the bitmap only black is drawn.
    return gx >= width && ink != (twoBit ? 3 : 1) ? 0 : ink;
  }
  const int left = at(gx - 1, gy);
  const int diagonalLeft = std::max(at(gx - 1, gy - 1), at(gx - 1, gy + 1));
  if ((current >= left && current > at(gx + 1, gy)) || diagonalLeft > left) return current;
  return std::min(current, left);
}

void compare(int orientation, int rotation, bool twoBit, Plane plane, bool state, int width, int height, int x, int y,
             int originY, int rows, Rect clip, int weight = 0) {
  SCOPED_TRACE(::testing::Message() << orientation << ',' << rotation << ',' << twoBit << ',' << static_cast<int>(plane)
                                    << ',' << state << " size=" << width << 'x' << height << " at=" << x << ',' << y
                                    << " band=" << originY << ',' << rows << " weight=" << weight);
  const int inkWidth = glyphBitmap::weightedWidth(width, weight);
  const auto [dxX, dxY, dyX, dyY] = AXES[rotation];
  const glyphBitmap::Frame logical{x, y, dxX, dxY, dyX, dyY};
  std::vector<uint8_t> bitmap((width * height * (twoBit ? 2 : 1) + 7) / 8);
  for (size_t i = 0; i < bitmap.size(); ++i) bitmap[i] = static_cast<uint8_t>(i * 73 + 0x1b);
  // Nonuniform background and guards verify transparent pixels and all off-band bytes.
  std::vector<uint8_t> expected(rows * STRIDE + 32);
  for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i * 53 + 0xa5);
  auto actual = expected;
  for (int gy = 0; gy < height; ++gy) {
    for (int gx = 0; gx < inkWidth; ++gx) {
      const int lx = x + gx * dxX + gy * dyX;
      const int ly = y + gx * dxY + gy * dyY;
      if (lx < clip.left || ly < clip.top || lx >= clip.right || ly >= clip.bottom) continue;
      const auto [px, py] = physical(orientation, lx, ly);
      if (px < 0 || px >= PANEL_WIDTH || py < originY || py >= originY + rows) continue;
      const int ink = weightedValue(bitmap, twoBit, width, height, gx, gy, weight);
      bool draw = false;
      bool black = state;
      if (twoBit) {
        const int value = 3 - ink;
        if (plane == Plane::BW) draw = value < 3;
        if (plane == Plane::GrayLSB) draw = value == 1;
        if (plane == Plane::GrayMSB) draw = value == 1 || value == 2;
        if (plane != Plane::BW) black = false;
      } else {
        draw = ink != 0;
      }
      if (!draw) continue;
      auto& byte = expected[16 + (py - originY) * STRIDE + px / 8];
      if (black)
        byte &= static_cast<uint8_t>(~(0x80 >> (px % 8)));
      else
        byte |= 0x80 >> (px % 8);
    }
  }

  glyphBitmap::Clip local{0, 0, inkWidth, height};
  glyphBitmap::clipToRect(logical, clip.left, clip.top, clip.right, clip.bottom, local);

  const auto [px, py] = physical(orientation, x, y);
  const auto [xx, xy] = physical(orientation, x + dxX, y + dxY);
  const auto [yx, yy] = physical(orientation, x + dyX, y + dyY);
  const glyphBitmap::Target target{
      actual.data() + 16, PANEL_WIDTH, STRIDE, originY, rows, {px, py, xx - px, xy - py, yx - px, yy - py}};
  if (weight == 0) {
    glyphBitmap::draw(bitmap.data(), width, height, twoBit, plane, state, target, local);
  } else {
    glyphBitmap::drawWeighted(bitmap.data(), width, height, twoBit, plane, state, target, local, weight);
  }
  EXPECT_EQ(expected, actual);
}

using Grid = std::vector<std::vector<int>>;

// Paint a 2bpp glyph (rows of values 0..3) into one plane of an unrotated
// panel and read back which pixels the plane marks, over the weighted width.
Grid weightedGlyph(const Grid& values, int weight, Plane plane = Plane::BW) {
  const int height = static_cast<int>(values.size());
  const int width = static_cast<int>(values[0].size());
  std::vector<uint8_t> bitmap((width * height * 2 + 7) / 8);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int i = y * width + x;
      bitmap[i / 4] |= static_cast<uint8_t>(values[y][x] << ((3 - i % 4) * 2));
    }
  }
  // BW marks black by clearing bits; the gray planes mark by setting them.
  const uint8_t background = plane == Plane::BW ? 0xff : 0x00;
  std::vector<uint8_t> panel(STRIDE * height, background);
  const glyphBitmap::Target target{panel.data(), PANEL_WIDTH, STRIDE, 0, height, {0, 0, 1, 0, 0, 1}};
  const int inkWidth = glyphBitmap::weightedWidth(width, weight);
  glyphBitmap::drawWeighted(bitmap.data(), width, height, true, plane, true, target, {0, 0, inkWidth, height}, weight);
  Grid marked(height, std::vector<int>(inkWidth));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < inkWidth; ++x) {
      const bool bit = (panel[y * STRIDE + x / 8] & (0x80 >> (x % 8))) != 0;
      marked[y][x] = plane == Plane::BW ? !bit : bit;
    }
  }
  return marked;
}

std::vector<int> weightedRow(const std::vector<int>& values, int weight) { return weightedGlyph({values}, weight)[0]; }
}  // namespace

TEST(GlyphBitmap, MatchesPerPixelReferenceAcrossOrientationsRotationsPlanesAndClipping) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      for (bool twoBit : {false, true}) {
        for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
          for (bool state : {false, true}) {
            for (int width : {1, 3, 7, 16, 31}) {
              for (const auto [x, y] : {std::pair{-5, -3}, std::pair{0, 0}, std::pair{9, 11}, std::pair{29, 31}}) {
                for (const auto [origin, rows] :
                     {std::pair{0, 32}, std::pair{0, 7}, std::pair{7, 11}, std::pair{28, 4}}) {
                  compare(orientation, rotation, twoBit, plane, state, width, 13, x, y, origin, rows,
                          {-20, -20, 60, 60});
                  compare(orientation, rotation, twoBit, plane, state, width, 13, x, y, origin, rows, {2, 3, 18, 20});
                }
              }
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, EmptyAndFullyClippedGlyphsDoNotWrite) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      compare(orientation, rotation, true, Plane::BW, true, 0, 0, 0, 0, 0, 32, {0, 0, 40, 32});
      compare(orientation, rotation, true, Plane::BW, true, 11, 9, 100, 100, 0, 32, {0, 0, 40, 32});
      compare(orientation, rotation, true, Plane::BW, true, 11, 9, 0, 0, 0, 32, {12, 10, 15, 20});
    }
  }
}

TEST(GlyphBitmap, WeightedMatchesPerPixelReference) {
  for (int weight : {-1, 1, 2}) {
    for (int orientation = 0; orientation < 4; ++orientation) {
      for (int rotation = 0; rotation < 2; ++rotation) {
        for (bool twoBit : {false, true}) {
          for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
            for (int width : {1, 3, 7, 16}) {
              for (const auto [origin, rows] : {std::pair{0, 32}, std::pair{7, 11}}) {
                compare(orientation, rotation, twoBit, plane, true, width, 13, 9, 11, origin, rows, {-20, -20, 60, 60},
                        weight);
                compare(orientation, rotation, twoBit, plane, false, width, 13, -5, -3, origin, rows, {2, 3, 18, 20},
                        weight);
              }
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, BoldKeepsLoopCountersOpen) {
  // A row through a small Thai head loop: two 1px walls around a 1px or 2px hole.
  EXPECT_EQ(weightedRow({0, 3, 0, 3, 0}, 2), (std::vector<int>{0, 1, 0, 1, 1, 1, 0}));
  EXPECT_EQ(weightedRow({0, 3, 0, 0, 3, 0}, 2), (std::vector<int>{0, 1, 1, 0, 1, 1, 1, 0}));
  // Open space still grows: a lone stem widens by the weight.
  EXPECT_EQ(weightedRow({0, 3, 0, 0, 0}, 1), (std::vector<int>{0, 1, 1, 0, 0, 0}));
}

TEST(GlyphBitmap, BoldKeepsGrayLoopCountersOpen) {
  // Anti-aliased hole: a light gray pixel between two black walls stays light.
  EXPECT_EQ(glyphBitmap::weightedInk(0, 3, 1, 3, 0, 1), 1);
  EXPECT_EQ(glyphBitmap::weightedInk(3, 0, 1, 3, 0, 2), 1);
  // At a stroke's right edge the gray pixel darkens as the stroke grows.
  EXPECT_EQ(glyphBitmap::weightedInk(3, 3, 1, 0, 0, 1), 3);
}

TEST(GlyphBitmap, BoldExtendsOnlyBlackPastTheGlyph) {
  // A gray right edge must not paint gray over whatever glyph follows.
  EXPECT_EQ(weightedGlyph({{3, 1}}, 1, Plane::GrayMSB), (Grid{{0, 0, 0}}));
  EXPECT_EQ(weightedGlyph({{3, 3}}, 1, Plane::BW), (Grid{{1, 1, 1}}));
}

TEST(GlyphBitmap, ThinKeepsDiagonalJoinsInLoops) {
  // A small ring whose top and bottom meet the walls only at corners.
  const Grid ring = {{0, 3, 3, 0}, {3, 0, 0, 3}, {3, 0, 0, 3}, {0, 3, 3, 0}};
  EXPECT_EQ(weightedGlyph(ring, -1), (Grid{{0, 1, 1, 0}, {1, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 1, 0}}));
  // A straight stem still thins.
  const Grid stem = {{0, 3, 3, 3, 0}, {0, 3, 3, 3, 0}, {0, 3, 3, 3, 0}};
  EXPECT_EQ(weightedGlyph(stem, -1), (Grid{{0, 0, 1, 1, 0}, {0, 0, 1, 1, 0}, {0, 0, 1, 1, 0}}));
}

TEST(GlyphBitmap, ThinKeepsOnePixelStrokes) {
  EXPECT_EQ(weightedRow({0, 3, 0, 3, 0}, -1), (std::vector<int>{0, 1, 0, 1, 0}));
  EXPECT_EQ(weightedRow({0, 3, 3, 3, 0}, -1), (std::vector<int>{0, 0, 1, 1, 0}));
  // An anti-aliased stem loses its left gray pixel, not its core.
  EXPECT_EQ(weightedRow({0, 1, 3, 1, 0}, -1), (std::vector<int>{0, 0, 1, 1, 0}));
}
