// Save the pre-change ThaiSegmenter.cpp outside the tree, then configure test/ with
// -DTHAI_SEGMENTER_BASELINE_SOURCE=/absolute/path/to/ThaiSegmenter.cpp and build
// ThaiSegmenterBenchmark. Run ThaiSegmenterBenchmark [iterations] (default: 50).
// Both implementations use the same dictionary/cluster code; only the saved
// segmenter translation unit is renamed. No obsolete tokenizer is copied here.
#include <ThaiCluster.h>
#include <ThaiDictionary.h>
#include <ThaiSegmenter.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace {
bool countAllocations = false;
uint64_t allocations = 0;

void* allocate(size_t bytes) {
  if (void* memory = std::malloc(bytes ? bytes : 1)) {
    if (countAllocations) ++allocations;
    return memory;
  }
  throw std::bad_alloc();
}

void* allocateAligned(size_t bytes, size_t alignment) {
  if (!bytes) bytes = 1;
  if (bytes > std::numeric_limits<size_t>::max() - (alignment - 1)) throw std::bad_alloc();
#ifdef _MSC_VER
  void* memory = _aligned_malloc(bytes, alignment);
#else
  void* memory = std::aligned_alloc(alignment, (bytes + alignment - 1) / alignment * alignment);
#endif
  if (!memory) throw std::bad_alloc();
  if (countAllocations) ++allocations;
  return memory;
}

void freeAligned(void* memory) noexcept {
#ifdef _MSC_VER
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}
}  // namespace

// Count standard C++ allocations, including array/aligned/nothrow forms. This
// single-threaded executable does not claim to intercept direct malloc calls.
void* operator new(size_t bytes) { return allocate(bytes); }
void* operator new[](size_t bytes) { return allocate(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }
void* operator new(size_t bytes, std::align_val_t alignment) {
  return allocateAligned(bytes, static_cast<size_t>(alignment));
}
void* operator new[](size_t bytes, std::align_val_t alignment) {
  return allocateAligned(bytes, static_cast<size_t>(alignment));
}
void operator delete(void* memory, std::align_val_t) noexcept { freeAligned(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { freeAligned(memory); }
void operator delete(void* memory, size_t, std::align_val_t) noexcept { freeAligned(memory); }
void operator delete[](void* memory, size_t, std::align_val_t) noexcept { freeAligned(memory); }
void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
  try {
    return allocate(bytes);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](size_t bytes, const std::nothrow_t&) noexcept {
  try {
    return allocate(bytes);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void* operator new(size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return allocateAligned(bytes, static_cast<size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return allocateAligned(bytes, static_cast<size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned(memory); }
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned(memory); }

namespace thai {
bool greedyNextSegment(std::string_view text, size_t& offset, Segment& result, bool endOfRun,
                       const ThaiDictionary& dictionary);
}  // namespace thai

namespace {
using Segmenter = bool (*)(std::string_view, size_t&, thai::Segment&, bool, const thai::ThaiDictionary&);
using Clock = std::chrono::steady_clock;

struct Statistics {
  uint64_t bytes = 0;
  uint64_t tokens = 0;
  uint64_t unknown = 0;
  uint64_t singletons = 0;
  uint64_t checksum = 14695981039346656037ull;
  size_t pendingHighWater = 0;

  void hash(uint64_t value) { checksum = (checksum ^ value) * 1099511628211ull; }

  void add(std::string_view text, const thai::Segment& segment, size_t base) {
    bytes += segment.end - segment.begin;
    ++tokens;
    if (segment.known && segment.codepoints == 1) ++singletons;
    if (!segment.known) {
      for (size_t offset = segment.begin; offset < segment.end;) {
        const auto scalar = thai::detail::decode(text, offset, true);
        if (scalar.valid && thai::isThai(scalar.value) && !thai::isDigit(scalar.value) &&
            !thai::isRepetition(scalar.value) && !thai::isAbbreviation(scalar.value))
          ++unknown;
        offset += scalar.bytes;
      }
    }
    hash(base + segment.begin);
    hash(base + segment.end);
    hash(segment.codepoints);
    hash(static_cast<uint8_t>(segment.before));
    hash(static_cast<uint8_t>(segment.after));
    hash(segment.known);
    hash(segment.valid);
  }
};

size_t drain(std::string_view text, bool final, size_t base, Segmenter segmenter,
             const thai::ThaiDictionary& dictionary, Statistics& statistics) {
  size_t offset = 0;
  while (offset < text.size()) {
    const size_t previous = offset;
    thai::Segment segment{};
    if (!segmenter(text, offset, segment, final, dictionary)) {
      if (final || offset != previous) throw std::runtime_error("segmenter failed to drain input");
      break;
    }
    if (offset <= previous || offset > text.size() || segment.begin != previous || segment.end != offset) {
      throw std::runtime_error("segmenter returned an invalid span");
    }
    statistics.add(text, segment, base);
  }
  return offset;
}

void consume(std::string_view text, bool streaming, Segmenter segmenter, const thai::ThaiDictionary& dictionary,
             Statistics& statistics) {
  if (!streaming) {
    drain(text, true, 0, segmenter, dictionary, statistics);
    return;
  }
  // Like the parser, feed Thai runs one decoded scalar at a time and finalize
  // them at non-Thai boundaries. Generic spans are supplied whole: the production
  // parser buffers those separately rather than calling nextSegment per letter.
  std::array<char, 768> pending{};
  size_t size = 0;
  size_t base = 0;
  size_t received = 0;
  while (received < text.size()) {
    const auto scalar = thai::detail::decode(text, received, true);
    if (!scalar.bytes) throw std::runtime_error("invalid benchmark UTF-8");
    if (!thai::isThai(scalar.value)) {
      drain(std::string_view(pending.data(), size), true, base, segmenter, dictionary, statistics);
      size = 0;
      const size_t begin = received;
      do {
        received += thai::detail::decode(text, received, true).bytes;
      } while (received < text.size() && !thai::isThai(thai::detail::decode(text, received, true).value));
      drain(text.substr(begin, received - begin), true, begin, segmenter, dictionary, statistics);
      base = received;
      continue;
    }
    if (scalar.bytes > pending.size() - size) throw std::runtime_error("parser pending capacity exceeded");
    std::memcpy(pending.data() + size, text.data() + received, scalar.bytes);
    size += scalar.bytes;
    received += scalar.bytes;
    if (size > statistics.pendingHighWater) statistics.pendingHighWater = size;
    const size_t consumed =
        drain(std::string_view(pending.data(), size), false, base, segmenter, dictionary, statistics);
    if (consumed) {
      size -= consumed;
      std::memmove(pending.data(), pending.data() + consumed, size);
      base += consumed;
    }
  }
  drain(std::string_view(pending.data(), size), true, base, segmenter, dictionary, statistics);
}

std::string repeated(std::string_view text, size_t count) {
  std::string result;
  result.reserve(text.size() * count);
  while (count--) result.append(text);
  return result;
}

struct Corpus {
  const char* name;
  std::string text;
  const thai::ThaiDictionary* dictionary;
};

void verifyStreaming(const Corpus& corpus, Segmenter segmenter) {
  Statistics whole;
  Statistics streamed;
  consume(corpus.text, false, segmenter, *corpus.dictionary, whole);
  consume(corpus.text, true, segmenter, *corpus.dictionary, streamed);
  if (whole.bytes != streamed.bytes || whole.tokens != streamed.tokens || whole.unknown != streamed.unknown ||
      whole.singletons != streamed.singletons || whole.checksum != streamed.checksum) {
    throw std::runtime_error(std::string("whole/stream mismatch in ") + corpus.name);
  }
}

void measure(const Corpus& corpus, const char* implementation, Segmenter segmenter, bool streaming, size_t iterations) {
  Statistics warm;
  consume(corpus.text, streaming, segmenter, *corpus.dictionary, warm);
  if (warm.bytes != corpus.text.size()) throw std::runtime_error("benchmark did not consume every byte");
  Statistics statistics;
  allocations = 0;
  countAllocations = true;
  const auto start = Clock::now();
  for (size_t i = 0; i < iterations; ++i) {
    consume(corpus.text, streaming, segmenter, *corpus.dictionary, statistics);
  }
  const auto end = Clock::now();
  countAllocations = false;
  const uint64_t measuredAllocations = allocations;
  if (statistics.bytes != corpus.text.size() * iterations || statistics.tokens != warm.tokens * iterations ||
      statistics.unknown != warm.unknown * iterations || statistics.singletons != warm.singletons * iterations) {
    throw std::runtime_error("inconsistent benchmark results");
  }
  const double elapsed = std::chrono::duration<double, std::micro>(end - start).count();
  std::cout << corpus.name << ',' << (streaming ? "scalar-stream" : "whole") << ',' << implementation << ','
            << iterations << ',' << std::fixed << std::setprecision(3) << elapsed << ',' << statistics.bytes << ','
            << statistics.tokens << ',' << statistics.unknown << ',' << statistics.singletons << ','
            << statistics.checksum << ',' << measuredAllocations << ',' << statistics.pendingHighWater << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    size_t iterations = 50;
    if (argc > 2) throw std::runtime_error("usage: ThaiSegmenterBenchmark [iterations: 1..100000]");
    if (argc == 2) {
      const std::string_view argument(argv[1]);
      const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), iterations);
      if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || !iterations ||
          iterations > 100000)
        throw std::runtime_error("iterations must be an integer in 1..100000");
    }
    const thai::ThaiDictionary production;
    if (!production.available()) throw std::runtime_error("production Thai dictionary is unavailable");

    // One real front-compressed block: กข, กขค, คง. Greedy chooses กขค|ง,
    // stranding an unknown; the globally covered alternative is กข|คง.
    constexpr uint8_t fixtureData[] = {0, 2, 1, 2, 2, 1, 4, 0, 2, 4, 7};
    constexpr uint32_t fixtureOffsets[] = {0, sizeof(fixtureData)};
    const thai::ThaiDictionary fixture({fixtureData, sizeof(fixtureData), fixtureOffsets, 2, 3, 0});
    if (!fixture.available()) throw std::runtime_error("compressed ambiguity fixture is invalid");
    Statistics greedyProof;
    Statistics hybridProof;
    consume("กขคง", false, thai::greedyNextSegment, fixture, greedyProof);
    consume("กขคง", false, thai::nextSegment, fixture, hybridProof);
    if (greedyProof.unknown != 1 || hybridProof.unknown != 0 || hybridProof.tokens != 2) {
      throw std::runtime_error("ambiguity fixture did not demonstrate greedy failure and hybrid repair");
    }

    const std::array<Corpus, 6> corpora = {{
        {"clean-known-prose", repeated("ประเทศไทยมีประชากรจำนวนมาก ", 8), &production},
        // A user-reported device example, not a claim that saved source splits มาก.
        {"requested-phrase", repeated("ยิ่งได้มากเท่าไหร่ ", 16), &production},
        {"compound-heavy", repeated("ประเทศไทยจำนวนมากประชากร ", 8), &production},
        {"ambiguity-fixture", repeated("กขคง ", 32), &fixture},
        {"long-unknown-run", repeated("ฃฅ", 192), &fixture},
        {"mixed-thai-latin", repeated("ภาษาไทย (reader 2.5): มากเท่าไหร่? Thai/English! ", 8), &production},
    }};
    std::cout << "# dictionary_id=" << production.dataId()
              << "; unknown=Thai unknown codepoints; singletons=known one-codepoint words"
              << "; cpp_allocations=standard new calls; counts/checksum cover all iterations"
              << "; ambiguity-fixture/long-unknown-run use tiny dictionary, other corpora use production\n";
    std::cout << "corpus,mode,implementation,iterations,microseconds,bytes,tokens,unknown,singletons,checksum,"
                 "cpp_allocations,pending_high_water_bytes\n";
    for (const auto& corpus : corpora) {
      verifyStreaming(corpus, thai::greedyNextSegment);
      verifyStreaming(corpus, thai::nextSegment);
      for (bool streaming : {false, true}) {
        measure(corpus, "greedy", thai::greedyNextSegment, streaming, iterations);
        measure(corpus, "hybrid", thai::nextSegment, streaming, iterations);
      }
    }
    return 0;
  } catch (const std::exception& error) {
    countAllocations = false;
    std::cerr << "ThaiSegmenterBenchmark: " << error.what() << '\n';
    return 1;
  }
}
