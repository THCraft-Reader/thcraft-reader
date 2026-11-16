#include "ThaiUserWordStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ThaiDictionary.h>

#include <cstring>

#include "activities/RenderLock.h"

namespace {
constexpr const char* WORDS_DIR = "/.crosspoint";
constexpr const char* WORDS_PATH = "/.crosspoint/thai-words.txt";
constexpr const char* WORDS_TMP_PATH = "/.crosspoint/thai-words.txt.tmp";
// Longest accepted line: a maximal word plus a BOM, spaces and a CR.
constexpr size_t MAX_LINE_BYTES = thai::MAX_DICTIONARY_WORD_CODEPOINTS * 3 + 16;
// Boot reads no further than this, however large a file edited on a computer is.
constexpr size_t MAX_READ_BYTES = 64 * 1024;

bool containsWord(const uint8_t* words, const size_t bytes, const uint8_t* symbols, const size_t length) {
  for (size_t position = 0; position < bytes; position += 1 + words[position]) {
    if (words[position] == length && std::memcmp(words + position + 1, symbols, length) == 0) return true;
  }
  return false;
}

bool isComment(std::string_view line) {
  if (line.size() >= 3 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
  return !line.empty() && line.front() == '#';
}
}  // namespace

ThaiUserWordStore ThaiUserWordStore::instance;

void ThaiUserWordStore::loadFromFile() {
  // save() writes the temporary file in full before replacing the list. With the
  // list present, a leftover temporary file is an interrupted save: discard it.
  // Without it, a power cut fell between removal and rename: recover it, but drop
  // an unfinished last line (every saved line ends with a newline).
  bool recovered = false;
  if (Storage.exists(WORDS_TMP_PATH)) {
    if (Storage.exists(WORDS_PATH)) {
      Storage.remove(WORDS_TMP_PATH);
    } else if (Storage.rename(WORDS_TMP_PATH, WORDS_PATH)) {
      recovered = true;
    } else {
      LOG_ERR("THW", "Failed to recover %s", WORDS_TMP_PATH);
    }
  }
  if (!Storage.exists(WORDS_PATH)) return;
  HalFile file;
  if (!Storage.openFileForRead("THW", WORDS_PATH, file)) return;
  // Boot-time scratch at the list's byte cap; trimmed to size below.
  auto words = makeUniqueNoThrow<uint8_t[]>(thai::MAX_USER_WORD_BYTES);
  if (!words) {
    LOG_ERR("THW", "OOM: word list load");
    file.close();
    return;
  }

  size_t bytes = 0;
  size_t count = 0;
  size_t skipped = 0;
  char line[MAX_LINE_BYTES];
  size_t lineLength = 0;
  bool overlong = false;
  const auto finishLine = [&] {
    const std::string_view raw(line, lineLength);
    // Also drops a BOM, CR, and pasted no-break or zero-width spaces.
    const std::string_view text = thai::trimToUserWord(raw);
    if (overlong) {
      ++skipped;
    } else if (!text.empty() && !isComment(raw)) {
      uint8_t symbols[thai::MAX_DICTIONARY_WORD_CODEPOINTS];
      const size_t length = thai::encodeUserWord(text, symbols, sizeof(symbols));
      if (!length || count == thai::MAX_USER_WORDS || bytes + 1 + length > thai::MAX_USER_WORD_BYTES) {
        ++skipped;
      } else if (!containsWord(words.get(), bytes, symbols, length)) {
        words[bytes] = static_cast<uint8_t>(length);
        std::memcpy(words.get() + bytes + 1, symbols, length);
        bytes += 1 + length;
        ++count;
      }
    }
    lineLength = 0;
    overlong = false;
  };

  uint8_t chunk[128];
  int read = 0;
  size_t total = 0;
  // Stop at the caps: a huge file edited on a computer must not stall boot.
  while (count < thai::MAX_USER_WORDS && total < MAX_READ_BYTES && (read = file.read(chunk, sizeof(chunk))) > 0) {
    total += static_cast<size_t>(read);
    for (int i = 0; i < read; ++i) {
      if (chunk[i] == '\n') {
        finishLine();
      } else if (lineLength < sizeof(line)) {
        line[lineLength++] = static_cast<char>(chunk[i]);
      } else {
        overlong = true;
      }
    }
  }
  if (!recovered) finishLine();
  file.close();
  if (skipped) LOG_ERR("THW", "Skipped %u unusable word line(s)", static_cast<unsigned>(skipped));
  if (bytes == 0) return;

  auto exact = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!exact) {
    LOG_ERR("THW", "OOM: word list (%u bytes)", static_cast<unsigned>(bytes));
    return;
  }
  std::memcpy(exact.get(), words.get(), bytes);
  words.reset();
  std::unique_ptr<Table> next;
  if (!buildTable(std::move(exact), bytes, next)) return;
  install(std::move(next));
  LOG_INF("THW", "Loaded %u Thai word(s)", static_cast<unsigned>(count));
}

ThaiUserWordStore::AddResult ThaiUserWordStore::add(std::string_view utf8) {
  uint8_t symbols[thai::MAX_DICTIONARY_WORD_CODEPOINTS];
  const size_t length = thai::encodeUserWord(utf8, symbols, sizeof(symbols));
  if (!length) return AddResult::Invalid;
  const size_t oldBytes = table ? table->view.wordBytes : 0;
  if (table && containsWord(table->words.get(), oldBytes, symbols, length)) return AddResult::Duplicate;
  const thai::ThaiDictionary dictionary;
  if (dictionary.longestBuiltinMatch(utf8, utf8.size()) == utf8.size()) return AddResult::KnownWord;
  const size_t bytes = oldBytes + 1 + length;
  if (count() >= thai::MAX_USER_WORDS || bytes > thai::MAX_USER_WORD_BYTES) return AddResult::Full;

  auto words = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!words) {
    LOG_ERR("THW", "OOM: add word (%u bytes)", static_cast<unsigned>(bytes));
    return AddResult::OutOfMemory;
  }
  if (oldBytes) std::memcpy(words.get(), table->words.get(), oldBytes);
  words[oldBytes] = static_cast<uint8_t>(length);
  std::memcpy(words.get() + oldBytes + 1, symbols, length);
  std::unique_ptr<Table> next;
  if (!buildTable(std::move(words), bytes, next)) return AddResult::OutOfMemory;
  return commit(std::move(next)) ? AddResult::Added : AddResult::StorageError;
}

bool ThaiUserWordStore::removeAt(const size_t index) {
  const size_t total = count();
  if (index >= total) return false;
  const size_t offset = offsetOf(total - 1 - index);
  if (offset == SIZE_MAX) return false;
  const size_t removed = 1 + table->words[offset];
  const size_t bytes = table->view.wordBytes - removed;
  std::unique_ptr<uint8_t[]> words;
  if (bytes) {
    words = makeUniqueNoThrow<uint8_t[]>(bytes);
    if (!words) {
      LOG_ERR("THW", "OOM: remove word (%u bytes)", static_cast<unsigned>(bytes));
      return false;
    }
    std::memcpy(words.get(), table->words.get(), offset);
    std::memcpy(words.get() + offset, table->words.get() + offset + removed, bytes - offset);
  }
  std::unique_ptr<Table> next;
  return buildTable(std::move(words), bytes, next) && commit(std::move(next));
}

bool ThaiUserWordStore::removeAll() { return !table || commit(nullptr); }

bool ThaiUserWordStore::wordAt(const size_t index, std::string& out) const {
  const size_t total = count();
  if (index >= total) return false;
  const size_t offset = offsetOf(total - 1 - index);
  if (offset == SIZE_MAX) return false;
  const uint8_t length = table->words[offset];
  out.resize(length * size_t{3});
  thai::decodeUserWord(table->words.get() + offset + 1, length, out.data());
  return true;
}

bool ThaiUserWordStore::wordAt(const size_t index, char* out, const size_t outSize) const {
  const size_t total = count();
  const size_t offset = index < total ? offsetOf(total - 1 - index) : SIZE_MAX;
  if (offset == SIZE_MAX || outSize == 0) return false;
  const uint8_t length = table->words[offset];
  if (length * size_t{3} >= outSize) return false;
  out[thai::decodeUserWord(table->words.get() + offset + 1, length, out)] = '\0';
  return true;
}

bool ThaiUserWordStore::buildTable(std::unique_ptr<uint8_t[]> words, const size_t bytes, std::unique_ptr<Table>& out) {
  out.reset();
  if (bytes == 0) return true;
  size_t count = 0;
  for (size_t position = 0; position < bytes; position += 1 + words[position]) ++count;
  auto next = makeUniqueNoThrow<Table>();
  auto sorted = makeUniqueNoThrow<uint16_t[]>(count);
  if (!next || !sorted) {
    LOG_ERR("THW", "OOM: word index (%u words)", static_cast<unsigned>(count));
    return false;
  }
  next->words = std::move(words);
  next->sorted = std::move(sorted);
  next->view.words = next->words.get();
  next->view.wordBytes = bytes;
  if (!thai::indexUserWords(next->view, next->sorted.get())) {
    LOG_ERR("THW", "Malformed word list");
    return false;
  }
  out = std::move(next);
  return true;
}

size_t ThaiUserWordStore::offsetOf(const size_t listIndex) const {
  if (!table) return SIZE_MAX;
  size_t index = 0;
  for (size_t position = 0; position < table->view.wordBytes; position += 1 + table->words[position]) {
    if (index++ == listIndex) return position;
  }
  return SIZE_MAX;
}

bool ThaiUserWordStore::commit(std::unique_ptr<Table> next) {
  if (!save(next.get())) return false;
  install(std::move(next));
  return true;
}

bool ThaiUserWordStore::save(const Table* next) {
  if (!next) {
    if (Storage.exists(WORDS_TMP_PATH)) Storage.remove(WORDS_TMP_PATH);
    if (Storage.exists(WORDS_PATH) && !Storage.remove(WORDS_PATH)) {
      LOG_ERR("THW", "Failed to remove %s", WORDS_PATH);
      return false;
    }
    return true;
  }
  Storage.mkdir(WORDS_DIR);
  HalFile file;
  if (!Storage.openFileForWrite("THW", WORDS_TMP_PATH, file)) return false;
  char line[thai::MAX_DICTIONARY_WORD_CODEPOINTS * 3 + 1];
  bool written = true;
  const uint8_t* words = next->words.get();
  for (size_t position = 0; written && position < next->view.wordBytes; position += 1 + words[position]) {
    size_t length = thai::decodeUserWord(words + position + 1, words[position], line);
    line[length++] = '\n';
    written = file.write(line, length) == length;
  }
  file.flush();
  // The last sector reaches the card in close(); a failure there means a short file.
  const bool closed = file.close();
  if (!written || !closed || !Storage.replaceFile(WORDS_TMP_PATH, WORDS_PATH)) {
    LOG_ERR("THW", "Failed to save %s", WORDS_PATH);
    Storage.remove(WORDS_TMP_PATH);
    return false;
  }
  return true;
}

void ThaiUserWordStore::install(std::unique_ptr<Table> next) {
  // Layout reads the installed view under this lock; the old table is freed after
  // the swap, when nothing can reach it.
  RenderLock lock;
  thai::setUserWords(next ? &next->view : nullptr);
  table.swap(next);
}
