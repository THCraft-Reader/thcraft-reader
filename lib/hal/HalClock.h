#pragma once

#include <Arduino.h>
#include <Rtc.h>
#include <time.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  // The RTC keeps UTC; local time comes from newlib's localtime_r under the
  // POSIX TZ rule set via setTimezone(), so zones with DST are correct
  // year-round. Cached as a UTC epoch to keep the RTC bus quiet.
  mutable time_t _cachedUtc = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds
  static constexpr time_t MIN_VALID_EPOCH = 1735689600;  // 2025-01-01 UTC

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // A hardware RTC or a software clock set by Wi-Fi sync/boot restoration.
  bool hasTime() const { return _available || time(nullptr) >= MIN_VALID_EPOCH; }

  // Set the POSIX TZ rule (e.g. "CET-1CEST,M3.5.0,M10.5.0/3") applied to every
  // read. nullptr/empty falls back to UTC. Drops the read cache so the change
  // shows immediately.
  void setTimezone(const char* posixTz);

  // Current wall-clock time in the configured timezone.
  // Without an RTC, uses system time; false until a valid time is available.
  bool localTime(struct tm& out) const;

  // Get current local hour (0-23) and minute (0-59).
  // Returns false until a valid time is available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format the local time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // Returns false until a valid time is available.
  bool formatTime(char* buf, size_t bufSize, bool use12Hour = false) const;

  // Sync from NTP, updating the hardware RTC when present. Requires WiFi.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the clock was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();
};
