#include <gtest/gtest.h>

#include "ReadingStatsCore.h"

namespace {

bool utcTime(int64_t epoch, struct tm& out) {
  const time_t t = static_cast<time_t>(epoch);
#ifdef _WIN32
  return gmtime_s(&out, &t) == 0;
#else
  return gmtime_r(&t, &out) != nullptr;
#endif
}

// UTC+7, the default Thai offset.
bool bangkokTime(int64_t epoch, struct tm& out) { return utcTime(epoch + 7 * 3600, out); }

// 2026-03-29 00:00 UTC: a synthetic DST start, offset +1 h before and +2 h after.
constexpr int64_t DST_SWITCH = 1774742400LL;
bool dstTime(int64_t epoch, struct tm& out) { return utcTime(epoch + (epoch < DST_SWITCH ? 3600 : 7200), out); }

// 2026-10-21 00:00 local (UTC+7).
constexpr int64_t DAY_START = 1792540800LL - 7 * 3600;

TEST(ReadingStatsCore, DayNumberStartsAt2020) {
  EXPECT_EQ(ReadingStatsCore::dayNumber(2020, 1, 1), 1);
  EXPECT_EQ(ReadingStatsCore::dayNumber(2020, 3, 1), 61);  // leap year
  EXPECT_EQ(ReadingStatsCore::dayNumber(2019, 12, 31), 0);
}

TEST(ReadingStatsCore, DayOfUsesLocalCalendar) {
  ReadingStatsCore core(bangkokTime);
  int hour = -1;
  int weekday = -1;
  const uint16_t day = core.dayOf(DAY_START, &hour, &weekday);
  EXPECT_EQ(day, ReadingStatsCore::dayNumber(2026, 10, 21));
  EXPECT_EQ(hour, 0);
  EXPECT_EQ(weekday, 3);  // Wednesday
  EXPECT_EQ(core.dayOf(DAY_START - 1), day - 1);
  EXPECT_EQ(core.dayOf(0), 0);
}

TEST(ReadingStatsCore, SplitsAcrossHourBoundary) {
  ReadingStatsCore core(bangkokTime);
  const int64_t end = DAY_START + 9 * 3600 + 600;  // 09:10
  core.addActiveSeconds(end, 1200);                // 08:50 -> 09:10
  const uint16_t day = core.dayOf(end);
  const uint16_t* hours = core.hourSecondsOn(day);
  ASSERT_NE(hours, nullptr);
  EXPECT_EQ(hours[8], 600);
  EXPECT_EQ(hours[9], 600);
  EXPECT_EQ(core.secondsOn(day), 1200u);
  EXPECT_EQ(core.state().totalSeconds, 1200u);
  EXPECT_TRUE(core.isDirty());
}

TEST(ReadingStatsCore, SplitsAcrossMidnight) {
  ReadingStatsCore core(bangkokTime);
  core.addActiveSeconds(DAY_START + 300, 900);  // 23:50 -> 00:05
  const uint16_t today = core.dayOf(DAY_START);
  EXPECT_EQ(core.secondsOn(today - 1), 600u);
  EXPECT_EQ(core.secondsOn(today), 300u);
  const uint16_t* hours = core.hourSecondsOn(today);
  ASSERT_NE(hours, nullptr);
  EXPECT_EQ(hours[0], 300);
  EXPECT_EQ(hours[23], 0);  // yesterday's hours were replaced
  EXPECT_EQ(core.hourSecondsOn(today - 1), nullptr);
}

TEST(ReadingStatsCore, SplitsAtDstSwitch) {
  ReadingStatsCore core(dstTime);
  // 00:50 local (+1) to 02:10 local (+2): 10 min before the jump, 10 min after.
  core.addActiveSeconds(DST_SWITCH + 600, 1200);
  const uint16_t day = core.dayOf(DST_SWITCH);
  const uint16_t* hours = core.hourSecondsOn(day);
  ASSERT_NE(hours, nullptr);
  EXPECT_EQ(hours[0], 600);
  EXPECT_EQ(hours[1], 0);
  EXPECT_EQ(hours[2], 600);
  EXPECT_EQ(core.secondsOn(day), 1200u);
}

TEST(ReadingStatsCore, CountsSessionsPerDay) {
  ReadingStatsCore core(bangkokTime);
  core.addSession(DAY_START + 3600);
  core.addSession(DAY_START + 7200);
  core.addSession(DAY_START - 3600);
  const uint16_t today = core.dayOf(DAY_START);
  EXPECT_EQ(core.sessionsOn(today), 2);
  EXPECT_EQ(core.sessionsOn(today - 1), 1);
  EXPECT_EQ(core.state().totalSessions, 3u);
}

TEST(ReadingStatsCore, StreakGrowsBreaksAndKeepsBest) {
  ReadingStatsCore core(bangkokTime);
  constexpr int64_t DAY = 86400;
  const int64_t noon = DAY_START + 12 * 3600;
  core.addActiveSeconds(noon, 30);  // under the threshold
  EXPECT_EQ(core.currentStreak(core.dayOf(noon)), 0);
  core.addActiveSeconds(noon + 100, 30);
  EXPECT_EQ(core.currentStreak(core.dayOf(noon)), 1);
  core.addActiveSeconds(noon + 200, 600);  // same day again
  EXPECT_EQ(core.currentStreak(core.dayOf(noon)), 1);
  core.addActiveSeconds(noon + DAY, 120);
  core.addActiveSeconds(noon + 2 * DAY, 120);
  const uint16_t third = core.dayOf(noon + 2 * DAY);
  EXPECT_EQ(core.currentStreak(third), 3);
  EXPECT_EQ(core.currentStreak(third + 1), 3);  // today not read yet
  EXPECT_EQ(core.currentStreak(third + 2), 0);  // a missed day
  core.addActiveSeconds(noon + 4 * DAY, 120);
  EXPECT_EQ(core.currentStreak(third + 2), 1);
  EXPECT_EQ(core.state().bestStreak, 3);
}

TEST(ReadingStatsCore, OlderDayNeverEvictsNewerSlot) {
  ReadingStatsCore core(bangkokTime);
  const int64_t now = DAY_START + 3600;
  core.addActiveSeconds(now, 100);
  core.addActiveSeconds(now - 30 * 86400, 100);  // same ring slot, 30 days older
  EXPECT_EQ(core.secondsOn(core.dayOf(now)), 100u);
  EXPECT_EQ(core.secondsOn(core.dayOf(now - 30 * 86400)), 0u);
}

TEST(ReadingStatsCore, RingReusesSlotForNewDay) {
  ReadingStatsCore core(bangkokTime);
  const int64_t now = DAY_START + 3600;
  core.addActiveSeconds(now, 100);
  core.addActiveSeconds(now + 30 * 86400, 50);
  EXPECT_EQ(core.secondsOn(core.dayOf(now)), 0u);
  EXPECT_EQ(core.secondsOn(core.dayOf(now + 30 * 86400)), 50u);
}

TEST(ReadingStatsCore, FinishedBooksCountOnce) {
  ReadingStatsCore core(bangkokTime);
  EXPECT_TRUE(core.noteBookFinished(42, DAY_START, true));
  EXPECT_FALSE(core.noteBookFinished(42, DAY_START + 86400, true));
  EXPECT_TRUE(core.noteBookFinished(43, 0, false));
  EXPECT_FALSE(core.noteBookFinished(0, DAY_START, true));
  EXPECT_EQ(core.state().booksFinished, 2u);
  EXPECT_EQ(core.finishedSince(core.dayOf(DAY_START)), 1);  // the undated book is not in a window
}

TEST(ReadingStatsCore, IgnoresUnsetClock) {
  ReadingStatsCore core(bangkokTime);
  core.addActiveSeconds(100, 200);
  core.addSession(0);
  EXPECT_EQ(core.state().totalSeconds, 0u);
  EXPECT_EQ(core.state().totalSessions, 0u);
  EXPECT_FALSE(core.isDirty());
}

TEST(ReadingStatsCore, StateStaysSmall) { EXPECT_LE(sizeof(ReadingStatsState), 640u); }

}  // namespace
