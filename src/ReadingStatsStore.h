#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

#include "util/ReadingStatsCore.h"

// Read-only copy for screens, taken once when a screen opens. Every time-based
// field is zero unless `reliable`.
struct ReadingStatsSnapshot {
  bool reliable = false;
  int weekday = 0;  // 0 = Monday
  int hour = 0;     // local hour now
  uint32_t todaySeconds = 0;
  uint16_t todaySessions = 0;
  uint32_t hourSeconds[24] = {};
  uint32_t weekSeconds[7] = {};  // Monday..Sunday of the current week; days after today are 0
  uint32_t last7Seconds = 0;
  uint32_t last30Seconds = 0;
  uint16_t last30Sessions = 0;
  uint16_t streak = 0;
  uint16_t bestStreak = 0;
  uint16_t finishedLast30 = 0;
  uint32_t booksFinished = 0;  // known without a clock
  uint32_t totalSeconds = 0;
};

// Persistent reading time, sessions, streak and finished books. Fed by
// ReaderActivity; records only while the wall clock is confirmed so no
// minute is ever credited to a guessed date.
class ReadingStatsStore : public PersistableStore<ReadingStatsStore> {
  ReadingStatsStore();

  friend class PersistableStore<ReadingStatsStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/reading_stats.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static bool clockReliable();

  void addActiveSeconds(int64_t endEpoch, uint32_t seconds);
  void addSession(int64_t epoch);
  void noteBookFinished(const std::string& title, const std::string& author, int64_t epoch);
  void saveIfDirty();

  // Today's local day number, or 0 when the clock is not reliable.
  uint16_t today(int* hour = nullptr, int* weekday = nullptr) const;
  const ReadingStatsCore& core() const { return stats; }
  ReadingStatsSnapshot snapshot() const;

 private:
  ReadingStatsCore stats;
};

#define READING_STATS ReadingStatsStore::getInstance()
