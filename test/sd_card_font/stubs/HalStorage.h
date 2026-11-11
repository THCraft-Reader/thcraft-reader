#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

inline std::vector<uint8_t> sdFontTestFile;
inline size_t sdFontTestReads = 0;
inline size_t sdFontTestOpenHandles = 0;
inline size_t sdFontTestOpenCalls = 0;
inline size_t sdFontTestFailReadOffset = static_cast<size_t>(-1);

struct SdFontTestEsp {
  size_t largestBlock = 200 * 1024;
  size_t getFreeHeap() const { return 200 * 1024; }
  size_t getMaxAllocHeap() const { return largestBlock; }
};
inline SdFontTestEsp ESP;
inline uint32_t millis() { return 0; }

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() {
    if (opened_) close();
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept {
    if (this == &other) return *this;
    if (opened_) close();
    position_ = other.position_;
    opened_ = std::exchange(other.opened_, false);
    return *this;
  }
  bool seekSet(size_t position) {
    position_ = position;
    return opened_ && position <= size();
  }
  int read(void* output, size_t count) {
    ++sdFontTestReads;
    if (position_ == sdFontTestFailReadOffset) return -1;
    if (!opened_ || position_ > size()) return 0;
    count = std::min(count, size() - position_);
    std::memcpy(output, sdFontTestFile.data() + position_, count);
    position_ += count;
    return static_cast<int>(count);
  }
  void close() {
    if (!opened_) std::abort();  // Production HAL asserts on an uninitialized handle.
    --sdFontTestOpenHandles;
    opened_ = false;
  }
  bool isOpen() const { return opened_; }
  size_t size() const { return sdFontTestFile.size(); }
  size_t fileSize() const { return size(); }
  void open() {
    if (opened_) close();
    opened_ = true;
    position_ = 0;
    ++sdFontTestOpenHandles;
  }

 private:
  size_t position_ = 0;
  bool opened_ = false;
};

class HalStorage {
 public:
  bool exists(const char*) const { return !sdFontTestFile.empty(); }
  bool openFileForRead(const char*, const char*, HalFile& file) {
    ++sdFontTestOpenCalls;
    if (sdFontTestFile.empty()) return false;
    file.open();
    return true;
  }
};
inline HalStorage Storage;
