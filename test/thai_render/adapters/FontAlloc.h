#pragma once
#include "../HostAllocation.h"
inline void* fiFontMalloc(size_t n) noexcept { try { return probe::allocate(n); } catch (...) { return nullptr; } }
inline void fiFontFree(void* p) { probe::deallocate(p); }
