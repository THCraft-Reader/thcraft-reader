// Host-only construction of the pinned LibThai dictionary. The input is the
// bytewise sorted, unique UTF-8 list prepared from data/Makefile.am.
#include <datrie/trie.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "NativeAllocator.h"

namespace {
namespace fs = std::filesystem;

using AlphaMapOwner = std::unique_ptr<AlphaMap, decltype(&alpha_map_free)>;
using TrieOwner = std::unique_ptr<Trie, decltype(&trie_free)>;

bool bytewiseLess(const std::string& left, const std::string& right) {
  return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
    return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
  });
}

void decodeWord(const std::string& word, std::vector<AlphaChar>& key, size_t line) {
  // Every character in the upstream alphabet has a canonical three-byte UTF-8
  // encoding. Reject malformed input instead of trimming, normalizing or losing
  // entries as an encoding-conversion utility might do.
  if (word.empty() || word.size() % 3 != 0 ||
      word.size() / 3 > static_cast<size_t>(std::numeric_limits<int16_t>::max())) {
    throw std::runtime_error("invalid or oversized dictionary word at line " + std::to_string(line));
  }
  key.clear();
  key.reserve(word.size() / 3 + 1);
  for (size_t i = 0; i < word.size(); i += 3) {
    const auto first = static_cast<unsigned char>(word[i]);
    const auto second = static_cast<unsigned char>(word[i + 1]);
    const auto third = static_cast<unsigned char>(word[i + 2]);
    if (first != 0xe0 || (second != 0xb8 && second != 0xb9) || (third & 0xc0) != 0x80) {
      throw std::runtime_error("invalid UTF-8 or non-Thai alphabet character at line " + std::to_string(line));
    }
    const AlphaChar cp = static_cast<AlphaChar>(((second & 0x3f) << 6) | (third & 0x3f));
    if (cp < 0x0e01 || cp > 0x0e5b) {
      throw std::runtime_error("character outside thbrk.abm alphabet at line " + std::to_string(line));
    }
    key.push_back(cp);
  }
  key.push_back(0);
}

template <typename Visit>
size_t readWords(std::ifstream& input, Visit visit) {
  std::string word;
  std::string previous;
  std::vector<AlphaChar> key;
  size_t count = 0;
  while (std::getline(input, word)) {
    if (count == std::numeric_limits<size_t>::max()) {
      throw std::runtime_error("dictionary entry count overflow");
    }
    ++count;
    if (count > 1 && !bytewiseLess(previous, word)) {
      throw std::runtime_error("input must be bytewise sorted and unique at line " + std::to_string(count));
    }
    decodeWord(word, key, count);
    visit(key.data(), count);
    previous.swap(word);
  }
  if (input.bad() || !input.eof()) {
    throw std::runtime_error("failed to read dictionary input");
  }
  if (count == 0) {
    throw std::runtime_error("dictionary input is empty");
  }
  return count;
}

void buildDictionary(const fs::path& inputPath, const fs::path& outputPath) {
  if (fs::absolute(inputPath).lexically_normal() == fs::absolute(outputPath).lexically_normal() ||
      (fs::exists(outputPath) && fs::equivalent(inputPath, outputPath))) {
    throw std::runtime_error("input and output paths must differ");
  }
  std::ifstream input(inputPath, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open sorted dictionary input");
  }

  const auto initialStats = native_text::allocationStats();
  AlphaMapOwner alphabet(alpha_map_new(), alpha_map_free);
  if (!alphabet || alpha_map_add_range(alphabet.get(), 0x0e01, 0x0e5b) != 0 ||
      native_text::allocationStats().failures != initialStats.failures) {
    throw std::runtime_error("cannot allocate dictionary alphabet");
  }
  TrieOwner trie(trie_new(alphabet.get()), trie_free);
  alphabet.reset();
  if (!trie || native_text::allocationStats().failures != initialStats.failures) {
    throw std::runtime_error("cannot allocate dictionary trie");
  }
  const size_t wordCount = readWords(input, [&](const AlphaChar* key, size_t line) {
    // trietool add-list uses TRIE_DATA_ERROR when an entry has no data column.
    if (!trie_store(trie.get(), key, TRIE_DATA_ERROR) ||
        native_text::allocationStats().failures != initialStats.failures) {
      throw std::runtime_error("failed to insert dictionary word at line " + std::to_string(line));
    }
  });

  // Confirm that all entries survive subsequent insertions before publishing a
  // dictionary. Unlike trie_enumerate, this lookup path needs no allocations.
  input.clear();
  input.seekg(0);
  if (!input) {
    throw std::runtime_error("cannot rewind dictionary input");
  }
  const size_t retainedCount = readWords(input, [&](const AlphaChar* key, size_t line) {
    TrieData data = 0;
    if (!trie_retrieve(trie.get(), key, &data) || data != TRIE_DATA_ERROR) {
      throw std::runtime_error("dictionary lost word at line " + std::to_string(line));
    }
  });
  if (retainedCount != wordCount || native_text::allocationStats().failures != initialStats.failures) {
    throw std::runtime_error("dictionary input changed or allocation failed during construction");
  }

  const size_t serializedSize = trie_get_serialized_size(trie.get());
  if (serializedSize == 0 || serializedSize > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
    throw std::runtime_error("invalid dictionary serialized size");
  }
  std::vector<uint8_t> serialized(serializedSize);
  trie_serialize(trie.get(), serialized.data());
  trie.reset();
  const auto finalStats = native_text::allocationStats();
  if (finalStats.failures != initialStats.failures || finalStats.used != initialStats.used) {
    throw std::runtime_error("dictionary construction failed to release its allocations");
  }

  std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot open serialized dictionary output");
  }
  output.write(reinterpret_cast<const char*>(serialized.data()), static_cast<std::streamsize>(serialized.size()));
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write or close serialized dictionary output");
  }

  // The Python driver adds SHA-256 source/serialized digests to this report and
  // publishes the complete asset set only after successful construction.
  std::cout.exceptions(std::ios::badbit | std::ios::failbit);
  std::cout << "{\"word_count\":" << wordCount << ",\"retained_count\":" << retainedCount
            << ",\"serialized_bytes\":" << serializedSize << ",\"allocator_peak_bytes\":" << finalStats.peak << "}\n";
  std::cout.flush();
}

template <typename Char>
int run(int argc, Char* argv[]) {
  try {
    if (argc != 5 || fs::path(argv[1]) != "--input" || fs::path(argv[3]) != "--output") {
      throw std::runtime_error("usage: NativeThaiDictionaryBuilder --input SORTED_UTF8 --output DICTIONARY.tri");
    }
    buildDictionary(fs::path(argv[2]), fs::path(argv[4]));
    return 0;
  } catch (const std::exception& error) {
    const auto stats = native_text::allocationStats();
    std::fprintf(stderr, "NativeThaiDictionaryBuilder: %s (allocator peak: %zu bytes, failures: %zu)\n", error.what(),
                 stats.peak, stats.failures);
    return 1;
  }
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) { return run(argc, argv); }
#else
int main(int argc, char* argv[]) { return run(argc, argv); }
#endif
