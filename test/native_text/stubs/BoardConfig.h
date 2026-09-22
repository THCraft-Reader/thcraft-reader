#pragma once
#include <cstdint>
namespace BoardConfig {
enum class DisplayController { SSD1677, UC8179, UC8279 };
struct ViewableInsets {
  uint8_t top, right, bottom, left;
};
struct Profile {
  uint16_t displayWidth = 800, displayHeight = 480;
  ViewableInsets viewableInsets{9, 7, 3, 7};
  float uiScale = 1.2f;
};
// XTEINK_X4_PRO geometry from the production BoardConfig profile.
inline constexpr Profile ACTIVE{};
inline constexpr bool isX4Pro() { return true; }
inline constexpr bool isX4Classic() { return false; }
inline constexpr bool isPaperMono() { return false; }
}  // namespace BoardConfig
