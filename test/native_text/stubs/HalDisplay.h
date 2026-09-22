#pragma once

#include <Arduino.h>
#include <BoardConfig.h>
#include <GrayscaleCapabilities.h>

#include <array>
#include <span>

// In-memory panel RAM adapter. All text, orientation, clipping and compositing
// remain in the production GfxRenderer; this models only HAL byte transport.
class HalDisplay {
 public:
  using Controller = BoardConfig::DisplayController;
  using GrayscaleMode = freeink::GrayscaleMode;
  using GrayscaleCapabilities = freeink::GrayscaleCapabilities;
  using GrayscaleBase = freeink::GrayscaleBase;
  using GrayscaleEncoding = freeink::GrayscaleEncoding;
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  HalDisplay() { begin(); }
  void begin(bool = false) {
    lent_ = asleep_ = pending_ = false;
    frame_.fill(0xff);
    base_.fill(0xff);
    lsb_.fill(0xff);
    msb_.fill(0xff);
    visible_.fill(255);
  }
  Controller getController() const { return Controller::SSD1677; }
  uint16_t getDisplayWidth() const { return DISPLAY_WIDTH; }
  uint16_t getDisplayHeight() const { return DISPLAY_HEIGHT; }
  uint16_t getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
  uint32_t getBufferSize() const { return BUFFER_SIZE; }
  uint8_t* getFrameBuffer() const { return lent_ ? nullptr : frame_.data(); }
  void clearScreen(uint8_t color = 0xff) const {
    requireFrame();
    frame_.fill(color);
  }
  uint8_t* lendFrameBufferStorage(uint32_t* size) {
    if (lent_) return nullptr;
    if (size) *size = BUFFER_SIZE;
    lent_ = true;
    return frame_.data();
  }
  void returnFrameBufferStorage() {
    lent_ = false;
    frame_.fill(0xff);
  }
  void drawImage(const uint8_t* data, uint16_t x, uint16_t y, uint16_t width, uint16_t height, bool = false) const {
    requireFrame();
    const size_t rowBytes = (width + 7u) / 8u;
    for (unsigned row = 0; row < height && y + row < DISPLAY_HEIGHT; ++row) {
      for (unsigned column = 0; column < width && x + column < DISPLAY_WIDTH; ++column) {
        const bool white = (data[row * rowBytes + column / 8] >> (7 - column % 8)) & 1;
        setPixel(x + column, y + row, white);
      }
    }
  }
  void drawImageTransparent(const uint8_t* data, uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                            bool = false) const {
    requireFrame();
    const size_t rowBytes = (width + 7u) / 8u;
    for (unsigned row = 0; row < height && y + row < DISPLAY_HEIGHT; ++row) {
      for (unsigned column = 0; column < width && x + column < DISPLAY_WIDTH; ++column) {
        if (!((data[row * rowBytes + column / 8] >> (7 - column % 8)) & 1)) setPixel(x + column, y + row, false);
      }
    }
  }
  void displayBuffer(RefreshMode = FAST_REFRESH, bool = false) {
    requireFrame();
    base_ = frame_;
    for (size_t pixel = 0; pixel < visible_.size(); ++pixel) {
      const bool white = (base_[pixel / 8] >> (7 - pixel % 8)) & 1;
      visible_[pixel] = (white != inverted_) ? 255 : 0;
    }
    ++refreshes_;
  }
  void displayBufferAsync(RefreshMode mode = FAST_REFRESH) {
    displayBuffer(mode);
    pending_ = true;
  }
  void waitRefreshComplete() { pending_ = false; }
  bool supportsAsyncRefresh() const { return true; }
  bool supportsAsyncGrayscaleBase() const { return true; }
  void refreshDisplay(RefreshMode mode = FAST_REFRESH, bool off = false) { displayBuffer(mode, off); }
  void setInverted(bool inverted) { inverted_ = inverted; }
  bool toggleInverted() {
    inverted_ = !inverted_;
    return inverted_;
  }
  bool isInverted() const { return inverted_; }
  void deepSleep() { asleep_ = true; }
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const {
    return {mode == GrayscaleMode::Overlay ? GrayscaleEncoding::OverlayMasks : GrayscaleEncoding::AbsolutePlanes,
            GrayscaleBase::Separate, true, true, true};
  }
  void displayGrayscaleBase(RefreshMode mode = HALF_REFRESH, bool off = false) {
    displayGrayscaleBase(GrayscaleMode::Overlay, mode, off);
  }
  bool displayGrayscaleBase(GrayscaleMode mode, RefreshMode refresh = HALF_REFRESH, bool off = false) {
    displayBuffer(refresh, off);
    mode_ = mode;
    lsb_.fill(mode == GrayscaleMode::Overlay ? 0 : 0xff);
    msb_.fill(mode == GrayscaleMode::Overlay ? 0 : 0xff);
    return true;
  }
  void copyGrayscaleBuffers(const uint8_t* lsb, const uint8_t* msb) {
    copyGrayscaleLsbBuffers(lsb);
    copyGrayscaleMsbBuffers(msb);
  }
  void copyGrayscaleLsbBuffers(const uint8_t* bytes) {
    requireFrame();
    std::memcpy(lsb_.data(), bytes, BUFFER_SIZE);
  }
  void copyGrayscaleMsbBuffers(const uint8_t* bytes) {
    requireFrame();
    std::memcpy(msb_.data(), bytes, BUFFER_SIZE);
  }
  void cleanupGrayscaleBuffers(const uint8_t* bytes) {
    if (bytes) std::memcpy(base_.data(), bytes, BUFFER_SIZE);
    lsb_.fill(0);
    msb_.fill(0);
    mode_ = GrayscaleMode::Overlay;
  }
  void displayGrayBuffer(bool = false) {
    requireFrame();
    for (size_t pixel = 0; pixel < visible_.size(); ++pixel) {
      const uint8_t mask = static_cast<uint8_t>(0x80u >> (pixel % 8));
      const bool lsb = (lsb_[pixel / 8] & mask) != 0;
      const bool msb = (msb_[pixel / 8] & mask) != 0;
      uint8_t value;
      if (mode_ == GrayscaleMode::Overlay) {
        value = msb ? (lsb ? 85 : 170) : ((base_[pixel / 8] & mask) ? 255 : 0);
      } else {
        value = lsb ? (msb ? 255 : 85) : (msb ? 170 : 0);
      }
      visible_[pixel] = inverted_ ? static_cast<uint8_t>(255 - value) : value;
    }
    ++refreshes_;
  }
  void writeGrayscalePlaneStrip(bool lsb, const uint8_t* rows, uint16_t y, uint16_t count) {
    requireFrame();
    if (y > DISPLAY_HEIGHT || count > DISPLAY_HEIGHT - y) throw std::out_of_range("Invalid panel strip");
    auto& plane = lsb ? lsb_ : msb_;
    std::memcpy(plane.data() + size_t{y} * DISPLAY_WIDTH_BYTES, rows, size_t{count} * DISPLAY_WIDTH_BYTES);
  }
  bool supportsStripGrayscale() const { return true; }
  bool combinesGrayscaleBase() const { return false; }
  void preconditionGrayscale() { preconditionGrayscale(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT); }
  void preconditionGrayscale(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    precondition_ = {x, y, width, height};
  }
  std::span<const uint8_t> visibleGray() const { return visible_; }
  unsigned refreshCount() const { return refreshes_; }

 private:
  void requireFrame() const {
    if (lent_ || asleep_) throw std::logic_error("Panel operation while framebuffer is unavailable");
  }
  void setPixel(unsigned x, unsigned y, bool white) const {
    const size_t offset = size_t{y} * DISPLAY_WIDTH_BYTES + x / 8;
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x % 8));
    if (white)
      frame_[offset] |= mask;
    else
      frame_[offset] &= static_cast<uint8_t>(~mask);
  }
  mutable std::array<uint8_t, BUFFER_SIZE> frame_{};
  std::array<uint8_t, BUFFER_SIZE> base_{}, lsb_{}, msb_{};
  std::array<uint8_t, DISPLAY_WIDTH * DISPLAY_HEIGHT> visible_{};
  std::array<uint16_t, 4> precondition_{};
  GrayscaleMode mode_ = GrayscaleMode::Overlay;
  bool lent_ = false, asleep_ = false, pending_ = false, inverted_ = false;
  unsigned refreshes_ = 0;
};

extern HalDisplay display;
