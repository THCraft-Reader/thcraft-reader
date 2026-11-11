#pragma once

#include <cstdint>
#include <ctime>

// Converts a UTC epoch to local calendar time; false when it cannot.
using ReadingStatsLocalTimeFn = bool (*)(int64_t epoch, struct tm& out);

struct ReadingDayStat {
  uint16_t day = 0;  // local day number (see ReadingStatsCore::dayNumber), 0 = empty slot
  uint16_t sessions = 0;
  uint32_t seconds = 0;
};

struct ReadingFinishedBook {
  uint32_t key = 0;  // hash of title + author, 0 = empty slot
  uint16_t day = 0;  // 0 when the clock was not reliable
};

// Plain persisted state. Rings are indexed by day % DAY_COUNT and validated by
// each slot's own day field, so a stale slot never reads as a recent day.
struct ReadingStatsState {
  static constexpr int DAY_COUNT = 30;
  static constexpr int FINISHED_COUNT = 32;

  ReadingDayStat days[DAY_COUNT];
  uint16_t hourDay = 0;
  uint16_t hourSeconds[24] = {};
  uint16_t streakLastDay = 0;
  uint16_t streakLength = 0;
  uint16_t bestStreak = 0;
  uint32_t totalSeconds = 0;
  uint32_t totalSessions = 0;
  uint32_t booksFinished = 0;
  ReadingFinishedBook finished[FINISHED_COUNT];
  uint8_t finishedNext = 0;
};

// Reading-time bookkeeping without HAL or I/O; ReadingStatsStore owns persistence
// and decides whether the clock is reliable enough to record.
class ReadingStatsCore {
 public:
  // A day counts toward the streak once it reaches this much reading.
  static constexpr uint32_t STREAK_MIN_SECONDS = 60;

  explicit ReadingStatsCore(ReadingStatsLocalTimeFn localTime) : localTime(localTime) {}

  // Days since 2019-12-31 for a local calendar date, so 2020-01-01 is day 1.
  static uint16_t dayNumber(int year, int month, int dayOfMonth);
  // Local day number and hour for an epoch; day 0 when the conversion fails.
  uint16_t dayOf(int64_t epoch, int* hour = nullptr, int* weekday = nullptr) const;

  // Credits the interval [endEpoch - seconds, endEpoch], split at local hour
  // and day boundaries.
  void addActiveSeconds(int64_t endEpoch, uint32_t seconds);
  void addSession(int64_t epoch);
  // Returns false when the book was already counted.
  bool noteBookFinished(uint32_t key, int64_t epoch, bool clockReliable);

  uint32_t secondsOn(uint16_t day) const;
  uint16_t sessionsOn(uint16_t day) const;
  // Today's per-hour seconds, or nullptr when the stored hours belong to another day.
  const uint16_t* hourSecondsOn(uint16_t day) const;
  uint16_t currentStreak(uint16_t today) const;
  uint16_t finishedSince(uint16_t firstDay) const;

  ReadingStatsState& state() { return data; }
  const ReadingStatsState& state() const { return data; }
  bool isDirty() const { return dirty; }
  void markDirty() { dirty = true; }
  void clearDirty() { dirty = false; }
  void reset();

 private:
  void creditChunk(uint16_t day, int hour, uint32_t seconds);
  ReadingDayStat* slotFor(uint16_t day);

  ReadingStatsLocalTimeFn localTime;
  ReadingStatsState data;
  bool dirty = false;
};
