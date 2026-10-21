#include "ReadingStatsCore.h"

#include <algorithm>
#include <iterator>

namespace {

// Howard Hinnant's days_from_civil: days since 1970-01-01 for a proleptic
// Gregorian date.
int32_t daysFromCivil(int year, const int month, const int dayOfMonth) {
  year -= month <= 2 ? 1 : 0;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const int32_t yearOfEra = year - era * 400;
  const int32_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + dayOfMonth - 1;
  const int32_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + dayOfEra - 719468;
}

constexpr int32_t DAY_NUMBER_BASE = 18261;  // daysFromCivil(2019, 12, 31)

}  // namespace

uint16_t ReadingStatsCore::dayNumber(const int year, const int month, const int dayOfMonth) {
  const int32_t days = daysFromCivil(year, month, dayOfMonth) - DAY_NUMBER_BASE;
  if (days <= 0 || days > UINT16_MAX) return 0;
  return static_cast<uint16_t>(days);
}

uint16_t ReadingStatsCore::dayOf(const int64_t epoch, int* hour, int* weekday) const {
  struct tm local{};
  if (epoch <= 0 || !localTime || !localTime(epoch, local)) return 0;
  if (hour) *hour = local.tm_hour;
  if (weekday) *weekday = local.tm_wday;
  return dayNumber(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

ReadingDayStat* ReadingStatsCore::slotFor(const uint16_t day) {
  if (day == 0) return nullptr;
  ReadingDayStat& slot = data.days[day % ReadingStatsState::DAY_COUNT];
  if (slot.day == day) return &slot;
  // Never let an older day (clock stepped back) evict a newer one.
  if (slot.day > day) return nullptr;
  slot = ReadingDayStat{day, 0, 0};
  return &slot;
}

void ReadingStatsCore::creditChunk(const uint16_t day, const int hour, const uint32_t seconds) {
  ReadingDayStat* slot = slotFor(day);
  if (!slot || seconds == 0) return;
  const uint32_t before = slot->seconds;
  slot->seconds += seconds;
  data.totalSeconds += seconds;

  if (data.hourDay < day) {
    data.hourDay = day;
    std::fill(std::begin(data.hourSeconds), std::end(data.hourSeconds), 0);
  }
  if (data.hourDay == day && hour >= 0 && hour < 24) {
    data.hourSeconds[hour] = static_cast<uint16_t>(std::min<uint32_t>(data.hourSeconds[hour] + seconds, 3600));
  }

  if (before < STREAK_MIN_SECONDS && slot->seconds >= STREAK_MIN_SECONDS && day > data.streakLastDay) {
    data.streakLength = day == data.streakLastDay + 1 ? data.streakLength + 1 : 1;
    data.streakLastDay = day;
    data.bestStreak = std::max(data.bestStreak, data.streakLength);
  }
  dirty = true;
}

void ReadingStatsCore::addActiveSeconds(const int64_t endEpoch, const uint32_t seconds) {
  if (seconds == 0 || endEpoch <= static_cast<int64_t>(seconds)) return;
  int64_t cursor = endEpoch - seconds;
  while (cursor < endEpoch) {
    struct tm local{};
    if (!localTime || !localTime(cursor, local)) return;
    const int64_t toNextHour = 3600 - (local.tm_min * 60 + local.tm_sec);
    const int64_t chunk = std::min<int64_t>(std::max<int64_t>(toNextHour, 1), endEpoch - cursor);
    creditChunk(dayNumber(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday), local.tm_hour,
                static_cast<uint32_t>(chunk));
    cursor += chunk;
  }
}

void ReadingStatsCore::addSession(const int64_t epoch) {
  ReadingDayStat* slot = slotFor(dayOf(epoch));
  if (!slot) return;
  slot->sessions++;
  data.totalSessions++;
  dirty = true;
}

bool ReadingStatsCore::noteBookFinished(const uint32_t key, const int64_t epoch, const bool clockReliable) {
  if (key == 0) return false;
  for (const auto& book : data.finished) {
    if (book.key == key) return false;
  }
  data.finished[data.finishedNext] = ReadingFinishedBook{key, clockReliable ? dayOf(epoch) : uint16_t{0}};
  data.finishedNext = static_cast<uint8_t>((data.finishedNext + 1) % ReadingStatsState::FINISHED_COUNT);
  data.booksFinished++;
  dirty = true;
  return true;
}

uint32_t ReadingStatsCore::secondsOn(const uint16_t day) const {
  const ReadingDayStat& slot = data.days[day % ReadingStatsState::DAY_COUNT];
  return day != 0 && slot.day == day ? slot.seconds : 0;
}

uint16_t ReadingStatsCore::sessionsOn(const uint16_t day) const {
  const ReadingDayStat& slot = data.days[day % ReadingStatsState::DAY_COUNT];
  return day != 0 && slot.day == day ? slot.sessions : 0;
}

const uint16_t* ReadingStatsCore::hourSecondsOn(const uint16_t day) const {
  return day != 0 && data.hourDay == day ? data.hourSeconds : nullptr;
}

uint16_t ReadingStatsCore::currentStreak(const uint16_t today) const {
  if (today == 0 || data.streakLastDay == 0) return 0;
  return today == data.streakLastDay || today == data.streakLastDay + 1 ? data.streakLength : 0;
}

uint16_t ReadingStatsCore::finishedSince(const uint16_t firstDay) const {
  uint16_t count = 0;
  for (const auto& book : data.finished) {
    if (book.key != 0 && book.day != 0 && book.day >= firstDay) count++;
  }
  return count;
}

void ReadingStatsCore::reset() {
  data = ReadingStatsState{};
  dirty = false;
}
