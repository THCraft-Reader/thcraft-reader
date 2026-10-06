#pragma once
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
inline uint64_t hostMicros() {
  static const auto origin = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - origin).count();
}
inline uint32_t micros() { return static_cast<uint32_t>(hostMicros()); }
inline uint32_t millis() { return static_cast<uint32_t>(hostMicros() / 1000); }
inline void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline void vTaskDelay(uint32_t) { std::this_thread::yield(); }
struct HostEsp {
  uint32_t getFreeHeap() const { return 256U * 1024 * 1024; }
  uint32_t getMaxAllocHeap() const { return getFreeHeap(); }
  [[noreturn]] void restart() const { std::abort(); }
};
// Host capacity is intentionally not reported as measured device heap.
inline HostEsp ESP;
