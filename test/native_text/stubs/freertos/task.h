#pragma once
#include <chrono>
#include <thread>

#include "FreeRTOS.h"
inline void vTaskDelay(TickType_t ticks) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ticks * portTICK_PERIOD_MS));
}
