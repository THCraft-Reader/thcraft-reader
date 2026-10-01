"""Run the real clock HAL with deterministic host time and RTC peripherals."""

from pathlib import Path
import subprocess
import tempfile

from test_trusted_time import STUBS as TIME_STUBS

ROOT = Path(__file__).resolve().parents[1]
STUBS = dict(TIME_STUBS)
STUBS["Arduino.h"] = STUBS["Arduino.h"].replace(
    "const char* tz, const char*)", "const char* tz, const char*, const char* = nullptr)"
)
STUBS["Logging.h"] = "#pragma once\n#define LOG_DBG(...) ((void)0)\n#define LOG_INF(...) ((void)0)\n#define LOG_ERR(...) ((void)0)\n"
STUBS["WiFi.h"] = r'''
#pragma once
constexpr int WL_CONNECTED = 1;
inline bool connected = true;
struct WifiStub { int status() const { return connected ? WL_CONNECTED : 0; } };
inline WifiStub WiFi;
'''
STUBS["Rtc.h"] = r'''
#pragma once
#include <cstdint>
inline bool rtcPresent = false;
inline unsigned rtcReads = 0;
struct Rtc {
  struct DateTime {
    uint16_t year = 2026;
    uint8_t month = 10, day = 1, hour = 0, minute = 0, second = 0, weekday = 4;
  };
  static DateTime value;
  bool begin() { return rtcPresent; }
  bool now(DateTime& out) { ++rtcReads; out = value; return true; }
  bool set(const DateTime& in) { value = in; return true; }
};
inline Rtc::DateTime Rtc::value;
'''
STUBS["host_time.h"] = r'''
#pragma once
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <sys/time.h>
inline time_t hostEpoch = 0;
inline time_t hostTime(time_t* out) { if (out) *out = hostEpoch; return hostEpoch; }
#define time hostTime
'''
CHECK = r'''
#include <HalClock.h>
#include <WiFi.h>
#include <cassert>
#include <cstring>

int main() {
  HalClock software;
  software.begin();
  software.setTimezone("UTC0");
  char buf[9] = {};
  assert(!software.isAvailable());
  assert(!software.hasTime());
  assert(!software.formatTime(buf, sizeof(buf)));

  hostEpoch = 1790812800;  // 2026-10-01 00:00 UTC
  assert(software.hasTime());
  assert(software.formatTime(buf, sizeof(buf)) && strcmp(buf, "00:00") == 0);
  assert(software.formatTime(buf, sizeof(buf), true) && strcmp(buf, "12:00 AM") == 0);
  assert(!software.formatTime(buf, 5));
  assert(!software.formatTime(buf, 8, true));
  hostEpoch += 12 * 3600;
  assert(software.formatTime(buf, sizeof(buf), true) && strcmp(buf, "12:00 PM") == 0);
  software.setTimezone("ICT-7");
  assert(software.formatTime(buf, sizeof(buf)) && strcmp(buf, "19:00") == 0);
  hostEpoch += 60;
  assert(software.formatTime(buf, sizeof(buf)) && strcmp(buf, "19:01") == 0);
  assert(rtcReads == 0);

  connected = false;
  assert(!software.syncFromNTP());
  connected = true;
  completed = false;
  assert(!software.syncFromNTP());
  completed = true;
  assert(software.syncFromNTP());
  assert(strcmp(getenv("TZ"), "ICT-7") == 0);

  rtcPresent = true;
  HalClock hardware;
  hardware.begin();
  hardware.setTimezone("UTC0");
  assert(hardware.isAvailable() && hardware.hasTime());
  assert(hardware.formatTime(buf, sizeof(buf)) && strcmp(buf, "00:00") == 0);
  assert(rtcReads == 1);
  Rtc::value.minute = 1;
  ticks += 1000;
  assert(hardware.formatTime(buf, sizeof(buf)) && strcmp(buf, "00:00") == 0);
  assert(rtcReads == 1);
  ticks += 10000;
  assert(hardware.formatTime(buf, sizeof(buf)) && strcmp(buf, "00:01") == 0);
  assert(rtcReads == 2);
  assert(hardware.syncFromNTP());
  assert(hardware.formatTime(buf, sizeof(buf)) && strcmp(buf, "12:01") == 0);
}
'''

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="hal-clock-test-") as directory:
        work = Path(directory)
        for name, content in STUBS.items():
            (work / name).write_text(content)
        (work / "check.cpp").write_text(CHECK)
        subprocess.run([
            "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-include", str(work / "host_time.h"),
            f"-I{work}", f"-I{ROOT / 'lib/hal'}", f"-I{ROOT / 'lib/TrustedTime'}",
            str(ROOT / "lib/hal/HalClock.cpp"),
            str(ROOT / "lib/TrustedTime/TrustedTime.cpp"), str(work / "check.cpp"),
            "-o", str(work / "check"),
        ], check=True)
        subprocess.run([str(work / "check")], check=True)
    print("Clock checks passed: unset time, minute changes, 12/24h, timezone, sync, RTC cache")
