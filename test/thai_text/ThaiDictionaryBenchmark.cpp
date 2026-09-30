#include <ThaiCluster.h>
#include <ThaiDictionary.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double elapsedUs(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

bool symbolsFor(std::string_view text, std::string& symbols) {
  symbols.clear();
  if (text.empty() || text.size() % 3 != 0) return false;
  for (size_t i = 0; i < text.size(); i += 3) {
    const auto first = static_cast<uint8_t>(text[i]);
    const auto middle = static_cast<uint8_t>(text[i + 1]);
    const auto last = static_cast<uint8_t>(text[i + 2]);
    if (first != 0xE0 || !((middle == 0xB8 && last >= 0x81 && last <= 0xBF) ||
                         (middle == 0xB9 && last >= 0x80 && last <= 0x9B))) return false;
    symbols.push_back(static_cast<char>(((middle & 63) << 6 | (last & 63)) - 0xE00));
  }
  return true;
}

void loadWords(const std::string& path, std::vector<std::string>& words, bool local) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Cannot open " + path);
  std::string line, symbols;
  size_t lineNumber = 0;
  while (std::getline(file, line)) {
    ++lineNumber;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!symbolsFor(line, symbols)) {
      if (local) throw std::runtime_error(path + ":" + std::to_string(lineNumber) + ": invalid supplement");
      continue;
    }
    if (symbols.size() > thai::MAX_DICTIONARY_WORD_CODEPOINTS) throw std::runtime_error("Overlong dictionary word");
    words.push_back(line);
  }
}

bool boundary(std::string_view text, size_t target) {
  size_t offset = 0;
  thai::Cluster cluster{};
  while (offset < target) {
    if (!thai::nextCluster(text, offset, cluster, true) || !cluster.valid) return false;
  }
  return offset == target;
}

template <bool compact>
struct Indexed {
  std::vector<char> data;
  std::vector<uint32_t> offsets;

  explicit Indexed(const std::vector<std::string>& words) {
    std::string symbols;
    for (const auto& word : words) {
      offsets.push_back(static_cast<uint32_t>(data.size()));
      if (compact) symbolsFor(word, symbols);
      const auto& entry = compact ? symbols : word;
      data.insert(data.end(), entry.begin(), entry.end());
      data.push_back(0);
    }
    offsets.push_back(static_cast<uint32_t>(data.size()));
  }
  std::string_view at(size_t index) const {
    return {data.data() + offsets[index], offsets[index + 1] - offsets[index] - 1};
  }
  size_t bytes() const { return data.size() + offsets.size() * sizeof(uint32_t); }
  size_t longestMatch(std::string_view text) const {
    std::array<char, compact ? thai::MAX_DICTIONARY_WORD_CODEPOINTS + 1 : 0> scratch{};
    const size_t count = std::min(text.size() / 3, thai::MAX_DICTIONARY_WORD_CODEPOINTS);
    if (compact) {
      for (size_t i = 0; i < count; ++i) {
        scratch[i] = static_cast<char>(((static_cast<uint8_t>(text[i * 3 + 1]) & 63) << 6 |
                                       (static_cast<uint8_t>(text[i * 3 + 2]) & 63)) - 0xE00);
      }
    }
    std::string_view query = compact ? std::string_view(scratch.data(), count) : text.substr(0, count * 3);
    while (!query.empty()) {
      size_t low = 0, high = offsets.size() - 1;
      while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (at(middle) <= query) low = middle + 1;
        else high = middle;
      }
      if (low == 0) return 0;
      const auto word = at(low - 1);
      size_t common = 0;
      while (common < std::min(word.size(), query.size()) && word[common] == query[common]) ++common;
      if (!compact) common -= common % 3;
      if (common == 0) return 0;
      if (common == word.size()) {
        const size_t bytes = compact ? common * 3 : common;
        if (boundary(text, bytes)) return bytes;
        common -= compact ? 1 : 3;
      }
      query = query.substr(0, common);
    }
    return 0;
  }
};

struct Compressed {
  std::vector<uint8_t> data;
  std::vector<uint32_t> offsets;
  size_t count;
  explicit Compressed(const std::vector<std::string>& words) : count(words.size()) {
    std::string previous, symbols;
    for (size_t i = 0; i < words.size(); ++i) {
      symbolsFor(words[i], symbols);
      size_t prefix = 0;
      if (i % thai::DICTIONARY_BLOCK_WORDS == 0) offsets.push_back(static_cast<uint32_t>(data.size()));
      else while (prefix < std::min(previous.size(), symbols.size()) && previous[prefix] == symbols[prefix]) ++prefix;
      data.push_back(static_cast<uint8_t>(prefix));
      data.push_back(static_cast<uint8_t>(symbols.size() - prefix));
      data.insert(data.end(), symbols.begin() + prefix, symbols.end());
      previous = symbols;
    }
    offsets.push_back(static_cast<uint32_t>(data.size()));
  }
  thai::DictionaryView view() const { return {data.data(), data.size(), offsets.data(), offsets.size(), count, 0}; }
  size_t bytes() const { return data.size() + offsets.size() * sizeof(uint32_t); }
};

template <typename Dictionary>
void measure(const char* name, const Dictionary& dictionary, const std::vector<std::string>& queries,
             size_t iterations, size_t bytes, size_t scratch, double prepareUs, double initUs, bool comma) {
  uint64_t matchedBytes = 0;
  const auto start = Clock::now();
  for (size_t repeat = 0; repeat < iterations; ++repeat) {
    for (const auto& query : queries) matchedBytes += dictionary.longestMatch(query);
  }
  const double us = elapsedUs(start);
  const double lookups = static_cast<double>(queries.size()) * iterations;
  std::cout << "    {\"representation\":\"" << name << "\",\"storage_bytes\":" << bytes
            << ",\"decoder_scratch_bytes\":" << scratch << ",\"host_prepare_us\":" << prepareUs
            << ",\"accessor_init_us\":" << initUs << ",\"lookups\":" << static_cast<uint64_t>(lookups)
            << ",\"lookup_us\":" << us << ",\"lookups_per_second\":" << lookups * 1e6 / us
            << ",\"matched_bytes_checksum\":" << matchedBytes << "}" << (comma ? "," : "") << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::string input = "lib/ThaiText/dictionary/words_th.txt";
    std::string extra = "lib/ThaiText/dictionary/extra_words.txt";
    size_t iterations = 10;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (i + 1 >= argc) throw std::runtime_error("Expected --input PATH --extra PATH --iterations N");
      if (arg == "--input") input = argv[++i];
      else if (arg == "--extra") extra = argv[++i];
      else if (arg == "--iterations") iterations = std::stoul(argv[++i]);
      else throw std::runtime_error("Unknown option");
    }
    if (iterations == 0) throw std::runtime_error("iterations must be positive");
    std::vector<std::string> words;
    loadWords(input, words, false);
    loadWords(extra, words, true);
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    if (words.empty()) throw std::runtime_error("Empty benchmark corpus");
    auto start = Clock::now();
    Indexed<false> utf8(words);
    const double utf8Prepare = elapsedUs(start);
    start = Clock::now();
    Indexed<true> symbols(words);
    const double symbolsPrepare = elapsedUs(start);
    start = Clock::now();
    Compressed storage(words);
    const double compressedPrepare = elapsedUs(start);
    start = Clock::now();
    thai::ThaiDictionary compressed(storage.view());
    const double checkedInit = elapsedUs(start);
    if (!compressed.valid()) throw std::runtime_error("Invalid benchmark encoding");
    // Production construction points to compiled flash; the checked host view
    // above additionally measures validation of caller-provided external data.
    start = Clock::now();
    thai::ThaiDictionary production;
    const double productionInit = elapsedUs(start);
    std::vector<std::string> queries = words;
    for (const auto& word : words) queries.push_back(word + "กข");
    for (const auto& query : queries) {
      const size_t expected = compressed.longestMatch(query);
      if (utf8.longestMatch(query) != expected || symbols.longestMatch(query) != expected) {
        throw std::runtime_error("Representations disagree on query: " + query);
      }
    }
    std::cout << "{\n  \"host_only\":true,\n  \"word_count\":" << words.size()
              << ",\n  \"query_count\":" << queries.size()
              << ",\n  \"production_flash_accessor_init_us\":" << productionInit
              << ",\n  \"production_dictionary_id\":" << production.dataId()
              << ",\n  \"results\":[\n";
    measure("indexed_utf8", utf8, queries, iterations, utf8.bytes(), 0, utf8Prepare, 0, true);
    measure("indexed_symbols", symbols, queries, iterations, symbols.bytes(), 71, symbolsPrepare, 0, true);
    measure("prefix_blocks_16", compressed, queries, iterations, storage.bytes(), 71, compressedPrepare, checkedInit, false);
    std::cout << "  ]\n}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ThaiDictionaryBenchmark: " << error.what() << '\n';
    return 1;
  }
}
