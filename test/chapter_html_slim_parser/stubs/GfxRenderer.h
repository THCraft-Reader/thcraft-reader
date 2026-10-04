#pragma once

#include <EpdFontFamily.h>
#include <ThaiCluster.h>
#include <Utf8.h>

#include <deque>
#include <string>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  enum class TextMeasureMode { Layout, Rendered };
  // The fixture has no framebuffer to lend; image probes run as without a loan.
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer&) {}
    void end() {}
  };
  // Fixture metrics: every glyph is 8 px wide, a space is 4 px, kerning is zero.
  static int trackingBetween(uint32_t left, uint32_t right, int8_t tracking) {
    return left == 0 || left == ' ' || right == ' ' ? 0 : tracking;
  }
  bool isFontCacheScanning() const { return false; }
  void drawLine(int, int, int, int, int, bool) const {}
  void drawText(int, int, int, const char*, bool, EpdFontFamily::Style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0, uint16_t = 0) const {}
  void drawTextRotated90CW(int, int, int, const char*, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR,
                           uint16_t = 0) const {}
  int getTextWidth(int font, const char* text, EpdFontFamily::Style style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    return getTextAdvanceX(font, text, style);
  }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int, float = 1.0f) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  inline static int naturalSpace = 4;
  int getSpaceWidth(int, EpdFontFamily::Style) const { return naturalSpace; }
  // Configurable uncompressed metrics let layout fixtures exercise zero/small caps.
  inline static int thaiAdvanceY = 48;
  int getThaiJustificationGapLimit(int, const char*, EpdFontFamily::Style style) const {
    return (style & (EpdFontFamily::SUP | EpdFontFamily::SUB) ? thaiAdvanceY / 2 : thaiAdvanceY) / 24;
  }
  size_t countThaiJustificationGaps(int, const char* text, EpdFontFamily::Style,
                                    BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    thai::JustificationBoundaryCursor cursor(text ? std::string_view(text) : std::string_view{});
    size_t count = 0;
    size_t offset;
    while (cursor.next(offset)) ++count;
    return count;
  }
  int getTextAdvanceX(int font, const char* text, EpdFontFamily::Style style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode = TextMeasureMode::Layout, uint16_t thaiExtraPixels = 0) const {
    if (!text) return 0;
    int width = thaiExtraPixels && countThaiJustificationGaps(font, text, style, baseDir) ? thaiExtraPixels : 0;
    uint32_t previous = 0;
    while (const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      if (utf8IsCombiningMark(cp)) continue;
      width += 8 + trackingBetween(previous, cp, tracking);
      previous = cp;
    }
    return width;
  }
  int getKerning(int, uint32_t left, uint32_t right, EpdFontFamily::Style, int8_t tracking = 0) const {
    return trackingBetween(left, right, tracking);
  }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return naturalSpace; }
  bool isSdCardFont(int) const { return false; }
  bool ensureSdCardFontReady(int, const char* const*, const size_t*, size_t, bool, bool, uint8_t = 0x0F,
                             bool = false) const {
    return true;
  }
};
