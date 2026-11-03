#include <gtest/gtest.h>

#include "ReadingStatsStore.h"

namespace trustedtime {
bool wallClockConfirmed() { return false; }
int64_t trustedNow() { return 0; }
}  // namespace trustedtime

namespace {

void loadFinishedBooks(const int cursor) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["booksFinished"] = ReadingStatsState::FINISHED_COUNT;
  doc["finishedNext"] = cursor;
  JsonArray finished = doc["finished"].to<JsonArray>();
  for (int i = 0; i < ReadingStatsState::FINISHED_COUNT; ++i) {
    JsonArray entry = finished.add<JsonArray>();
    entry.add(100u + i);
    entry.add(0);
  }
  ASSERT_TRUE(READING_STATS.fromJson(doc.as<JsonVariantConst>()));
}

TEST(ReadingStatsStore, NegativeFinishedCursorCannotEscapeTheRing) {
  loadFinishedBooks(-1);
  ASSERT_LT(READING_STATS.core().state().finishedNext, ReadingStatsState::FINISHED_COUNT);

  READING_STATS.noteBookFinished("New book", "Author", 0);
  const auto& state = READING_STATS.core().state();
  EXPECT_EQ(state.booksFinished, ReadingStatsState::FINISHED_COUNT + 1u);
  EXPECT_EQ(state.finishedNext, 1);
  EXPECT_NE(state.finished[0].key, 0u);
  EXPECT_NE(state.finished[0].key, 100u);
  for (int i = 1; i < ReadingStatsState::FINISHED_COUNT; ++i) {
    EXPECT_EQ(state.finished[i].key, 100u + i);
  }
}

TEST(ReadingStatsStore, RestoredLastCursorReplacesOnlyItsSlotAndWraps) {
  loadFinishedBooks(ReadingStatsState::FINISHED_COUNT - 1);
  READING_STATS.noteBookFinished("New book", "Author", 0);
  const auto& state = READING_STATS.core().state();
  EXPECT_EQ(state.booksFinished, ReadingStatsState::FINISHED_COUNT + 1u);
  EXPECT_EQ(state.finishedNext, 0);
  const auto& replaced = state.finished[ReadingStatsState::FINISHED_COUNT - 1];
  EXPECT_NE(replaced.key, 0u);
  EXPECT_NE(replaced.key, 100u + ReadingStatsState::FINISHED_COUNT - 1);
  for (int i = 0; i < ReadingStatsState::FINISHED_COUNT - 1; ++i) {
    EXPECT_EQ(state.finished[i].key, 100u + i);
  }
}

}  // namespace
