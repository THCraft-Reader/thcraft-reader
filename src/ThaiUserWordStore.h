#pragma once

#include <ThaiUserWords.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// The reader's own Thai words for word breaking: one global list in
// /.crosspoint/thai-words.txt, UTF-8, one word per line, read at boot.
// Each change saves the file, then swaps the new index in under the render
// lock; section caches rebuild through thai::layoutId().
// Mutating calls take the render lock: never call them while holding it.
class ThaiUserWordStore {
 public:
  enum class AddResult : uint8_t { Added, Duplicate, KnownWord, Invalid, Full, StorageError, OutOfMemory };

  static ThaiUserWordStore& getInstance() { return instance; }

  // Loads the saved list at boot; unreadable lines are skipped and logged.
  void loadFromFile();
  AddResult add(std::string_view utf8);
  // Index 0 is the newest word, as the list screen shows them.
  bool removeAt(size_t index);
  bool removeAll();
  size_t count() const { return table ? table->view.count : 0; }
  bool wordAt(size_t index, std::string& out) const;
  // As above into a buffer (NUL-terminated); false if it does not fit.
  bool wordAt(size_t index, char* out, size_t outSize) const;

 private:
  // Owns the storage a thai::UserWordsView points into; heap-allocated so the
  // installed view's address stays fixed.
  struct Table {
    std::unique_ptr<uint8_t[]> words;
    std::unique_ptr<uint16_t[]> sorted;
    thai::UserWordsView view;
  };

  static ThaiUserWordStore instance;

  // Indexes `bytes` of packed words; an empty list gives a null table. False on
  // allocation failure or malformed words.
  static bool buildTable(std::unique_ptr<uint8_t[]> words, size_t bytes, std::unique_ptr<Table>& out);
  // Offset of word `listIndex` (oldest first), or SIZE_MAX.
  size_t offsetOf(size_t listIndex) const;
  // Saves next, then installs it; on a failed save the current list stays.
  bool commit(std::unique_ptr<Table> next);
  static bool save(const Table* next);
  void install(std::unique_ptr<Table> next);

  std::unique_ptr<Table> table;
};

#define THAI_WORDS ThaiUserWordStore::getInstance()
