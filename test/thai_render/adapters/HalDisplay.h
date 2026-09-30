#pragma once
#include "Arduino.h"
#include <algorithm>
#include <cstring>
#include <vector>

// Only controller/refresh services are replaced. GfxRenderer and GlyphBitmap
// write this same packed framebuffer through their production paths.
class HalDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 800, DISPLAY_HEIGHT = 480, DISPLAY_WIDTH_BYTES = 100;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  enum class GrayscaleMode { Overlay, Absolute };
  enum class GrayscaleBase { Separate, Combined };
  struct GrayscaleCapabilities { bool asyncBase = false; bool stripUploads = false; GrayscaleBase base = GrayscaleBase::Separate; };
  HalDisplay(uint16_t width, uint16_t height) : width_(width), height_(height), stride_((width + 7) / 8), pixels_(stride_ * height, 0xff) {}
  uint8_t* getFrameBuffer() { return pixels_.data(); }
  uint16_t getDisplayWidth() const { return width_; }
  uint16_t getDisplayHeight() const { return height_; }
  uint16_t getDisplayWidthBytes() const { return stride_; }
  uint32_t getBufferSize() const { return static_cast<uint32_t>(pixels_.size()); }
  void clearScreen(uint8_t color) { std::fill(pixels_.begin(), pixels_.end(), color); }
  uint8_t* lendFrameBufferStorage(uint32_t* size) { *size = getBufferSize(); return pixels_.data(); }
  void returnFrameBufferStorage() { clearScreen(0xff); }
  bool isInverted() const { return false; }
  void drawImage(const uint8_t* bitmap, int x, int y, int width, int height) {
    for (int row = 0; row < height; ++row) for (int col = 0; col < width; ++col) {
      if (x + col < 0 || x + col >= width_ || y + row < 0 || y + row >= height_) continue;
      const bool white = bitmap[row * ((width + 7) / 8) + col / 8] & (0x80 >> (col % 8));
      auto& dst = pixels_[(y + row) * stride_ + (x + col) / 8];
      const uint8_t mask = 0x80 >> ((x + col) % 8);
      if (white) dst |= mask; else dst &= ~mask;
    }
  }
  void displayBuffer(RefreshMode, bool = false) {}
  void displayBufferAsync(RefreshMode) {}
  void waitRefreshComplete() {}
  bool supportsAsyncRefresh() const { return false; }
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode) const { return {}; }
  void displayGrayscaleBase(RefreshMode, bool) {}
  bool displayGrayscaleBase(GrayscaleMode, RefreshMode, bool) { return true; }
  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void displayGrayBuffer(bool) {}
  void cleanupGrayscaleBuffers(const uint8_t*) {}
  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) {}
 private:
  uint16_t width_, height_, stride_;
  std::vector<uint8_t> pixels_;
};
