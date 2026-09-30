#pragma once

// Host-test stub for FreeInkFont's PSRAM-preferring helpers (FontPsram.h):
// plain heap on the host, same contracts (psramNewArray is nothrow, nullptr
// on OOM). Allocation goes through nothrow operator new[] so the test's
// fail-next-allocation override (see SdCardFontTest.cpp) can inject OOM.
#include <new>
#include <vector>

namespace probe {
inline std::size_t failFontAllocationBytes = 0;
}

namespace freeink {
namespace font {

template <typename T>
using PsramVector = std::vector<T>;

template <typename T>
T* psramNewArray(std::size_t n) {
  if (probe::failFontAllocationBytes && probe::failFontAllocationBytes % sizeof(T) == 0 &&
      n == probe::failFontAllocationBytes / sizeof(T)) {
    probe::failFontAllocationBytes = 0;
    return nullptr;
  }
  return new (std::nothrow) T[n ? n : 1];
}

template <typename T>
void psramDeleteArray(T* p) {
  delete[] p;
}

}  // namespace font
}  // namespace freeink
