#include "ReadingStatsStore.h"

#include <Logging.h>
#include <TrustedTime.h>

#include <algorithm>

namespace {

bool localTime(const int64_t epoch, struct tm& out) {
  const time_t t = static_cast<time_t>(epoch);
  return localtime_r(&t, &out) != nullptr;
}

uint32_t fnv1a(const std::string& text, uint32_t hash = 2166136261u) {
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}

constexpr int FORMAT_VERSION = 1;

}  // namespace

ReadingStatsStore::ReadingStatsStore() : stats(&localTime) {}

bool ReadingStatsStore::clockReliable() { return trustedtime::wallClockConfirmed(); }

void ReadingStatsStore::toJson(JsonDocument& doc) const {
  const ReadingStatsState& s = stats.state();
  doc["v"] = FORMAT_VERSION;
  JsonArray days = doc["days"].to<JsonArray>();
  for (const auto& day : s.days) {
    if (day.day == 0) continue;
    JsonArray entry = days.add<JsonArray>();
    entry.add(day.day);
    entry.add(day.sessions);
    entry.add(day.seconds);
  }
  doc["hourDay"] = s.hourDay;
  JsonArray hours = doc["hours"].to<JsonArray>();
  for (const uint16_t seconds : s.hourSeconds) hours.add(seconds);
  doc["streakLastDay"] = s.streakLastDay;
  doc["streakLength"] = s.streakLength;
  doc["bestStreak"] = s.bestStreak;
  doc["totalSeconds"] = s.totalSeconds;
  doc["totalSessions"] = s.totalSessions;
  doc["booksFinished"] = s.booksFinished;
  JsonArray finished = doc["finished"].to<JsonArray>();
  for (const auto& book : s.finished) {
    JsonArray entry = finished.add<JsonArray>();
    entry.add(book.key);
    entry.add(book.day);
  }
  doc["finishedNext"] = s.finishedNext;
}

bool ReadingStatsStore::fromJson(JsonVariantConst doc) {
  stats.reset();
  if ((doc["v"] | 0) != FORMAT_VERSION) {
    LOG_ERR("RSTAT", "Unknown reading stats format, starting fresh");
    return true;
  }
  ReadingStatsState& s = stats.state();
  for (JsonArrayConst entry : doc["days"].as<JsonArrayConst>()) {
    const uint16_t day = entry[0] | 0;
    if (day == 0) continue;
    ReadingDayStat& slot = s.days[day % ReadingStatsState::DAY_COUNT];
    if (slot.day > day) continue;
    slot.day = day;
    slot.sessions = entry[1] | 0;
    slot.seconds = entry[2] | 0u;
  }
  s.hourDay = doc["hourDay"] | 0;
  int hour = 0;
  for (JsonVariantConst seconds : doc["hours"].as<JsonArrayConst>()) {
    if (hour >= 24) break;
    s.hourSeconds[hour++] = seconds | 0;
  }
  s.streakLastDay = doc["streakLastDay"] | 0;
  s.streakLength = doc["streakLength"] | 0;
  s.bestStreak = doc["bestStreak"] | 0;
  s.totalSeconds = doc["totalSeconds"] | 0u;
  s.totalSessions = doc["totalSessions"] | 0u;
  s.booksFinished = doc["booksFinished"] | 0u;
  int index = 0;
  for (JsonArrayConst entry : doc["finished"].as<JsonArrayConst>()) {
    if (index >= ReadingStatsState::FINISHED_COUNT) break;
    s.finished[index++] = ReadingFinishedBook{entry[0] | 0u, entry[1] | uint16_t{0}};
  }
  s.finishedNext = static_cast<uint8_t>((doc["finishedNext"] | 0) % ReadingStatsState::FINISHED_COUNT);
  return true;
}

void ReadingStatsStore::addActiveSeconds(const int64_t endEpoch, const uint32_t seconds) {
  if (clockReliable()) stats.addActiveSeconds(endEpoch, seconds);
}

void ReadingStatsStore::addSession(const int64_t epoch) {
  if (clockReliable()) stats.addSession(epoch);
}

void ReadingStatsStore::noteBookFinished(const std::string& title, const std::string& author, const int64_t epoch) {
  if (title.empty()) return;
  const uint32_t key = fnv1a(author, fnv1a("\x1f", fnv1a(title)));
  if (stats.noteBookFinished(key, epoch, clockReliable())) saveIfDirty();
}

void ReadingStatsStore::saveIfDirty() {
  if (!stats.isDirty()) return;
  if (saveToFile()) {
    stats.clearDirty();
  } else {
    LOG_ERR("RSTAT", "Failed to save reading stats");
  }
}

uint16_t ReadingStatsStore::today(int* hour, int* weekday) const {
  if (!clockReliable()) return 0;
  return stats.dayOf(trustedtime::trustedNow(), hour, weekday);
}

ReadingStatsSnapshot ReadingStatsStore::snapshot() const {
  ReadingStatsSnapshot snap;
  const ReadingStatsState& s = stats.state();
  snap.booksFinished = s.booksFinished;
  snap.bestStreak = s.bestStreak;
  int hour = 0;
  int weekday = 0;
  const uint16_t day = today(&hour, &weekday);
  if (day == 0) return snap;
  snap.reliable = true;
  snap.hour = hour;
  snap.weekday = (weekday + 6) % 7;
  snap.todaySeconds = stats.secondsOn(day);
  snap.todaySessions = stats.sessionsOn(day);
  if (const uint16_t* hours = stats.hourSecondsOn(day)) std::copy(hours, hours + 24, snap.hourSeconds);
  for (int i = 0; i <= snap.weekday; ++i) snap.weekSeconds[i] = stats.secondsOn(day - snap.weekday + i);
  for (int back = 0; back < ReadingStatsState::DAY_COUNT && back < day; ++back) {
    const uint32_t seconds = stats.secondsOn(day - back);
    if (back < 7) snap.last7Seconds += seconds;
    snap.last30Seconds += seconds;
    snap.last30Sessions += stats.sessionsOn(day - back);
  }
  snap.streak = stats.currentStreak(day);
  snap.finishedLast30 =
      stats.finishedSince(day > ReadingStatsState::DAY_COUNT ? day - ReadingStatsState::DAY_COUNT + 1 : 1);
  snap.totalSeconds = s.totalSeconds;
  return snap;
}
