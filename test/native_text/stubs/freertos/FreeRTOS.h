#pragma once
#include <cstdint>
using TickType_t = uint32_t;
inline constexpr TickType_t portTICK_PERIOD_MS = 1;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
