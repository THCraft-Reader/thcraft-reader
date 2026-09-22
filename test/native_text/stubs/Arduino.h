#pragma once

#include <NativeAllocator.h>
#include <freertos/task.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#define PROGMEM
#define memcpy_P memcpy
using String = std::string;

inline unsigned long millis() {
  static const auto start = std::chrono::steady_clock::now();
  return static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}
inline void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline void yield() { std::this_thread::yield(); }
struct HostEsp {
  [[noreturn]] void restart() const {
    throw std::runtime_error("Firmware requested a restart during host verification");
  }
  // Host native allocation budget, not ESP32 internal-heap telemetry.
  uint32_t getFreeHeap() const {
    return static_cast<uint32_t>(native_text::MEMORY_LIMIT - native_text::allocationStats().used);
  }
  uint32_t getMaxAllocHeap() const { return getFreeHeap(); }
};
inline HostEsp ESP;
