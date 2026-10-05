#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

inline std::vector<uint8_t> sdFontTestFile;
inline std::vector<uint8_t> sdFontTestCompanion;
inline size_t sdFontTestReads = 0;
inline size_t sdFontTestShapeReads = 0;
inline size_t sdFontTestShapeBytes = 0;
inline size_t sdFontTestShapeSeeks = 0;
inline size_t sdFontTestShapeFailures = 0;
inline size_t sdFontTestShapeOpenHandles = 0;
inline size_t sdFontTestFailShapeSeekAt = 0;
inline size_t sdFontTestFailShapeReadAt = 0;
inline size_t sdFontTestShortShapeReadAt = 0;
inline size_t sdFontTestZeroShapeReadAt = 0;
inline size_t sdFontTestShapeEof = std::numeric_limits<size_t>::max();

inline void sdFontTestResetShapeFaults() {
  sdFontTestFailShapeSeekAt = sdFontTestFailShapeReadAt = sdFontTestShortShapeReadAt = sdFontTestZeroShapeReadAt = 0;
  sdFontTestShapeEof = std::numeric_limits<size_t>::max();
}

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
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept {
    if (this == &other) return *this;
    close();
    bytes_ = other.bytes_;
    position_ = other.position_;
    opened_ = std::exchange(other.opened_, false);
    return *this;
  }
  bool seekSet(size_t position) {
    if (shape() && ++sdFontTestShapeSeeks == sdFontTestFailShapeSeekAt) {
      ++sdFontTestShapeFailures;
      return false;
    }
    position_ = position;
    const bool ok = opened_ && position <= size();
    if (!ok && shape()) ++sdFontTestShapeFailures;
    return ok;
  }
  int read(void* output, size_t count) {
    ++sdFontTestReads;
    if (shape()) {
      ++sdFontTestShapeReads;
      if (sdFontTestShapeReads == sdFontTestFailShapeReadAt) {
        ++sdFontTestShapeFailures;
        return -1;
      }
      if (sdFontTestShapeReads == sdFontTestZeroShapeReadAt) {
        ++sdFontTestShapeFailures;
        return 0;
      }
    }
    if (!opened_ || position_ > size()) {
      if (shape()) ++sdFontTestShapeFailures;
      return 0;
    }
    const size_t requested = count;
    count = std::min(count, size() - position_);
    if (shape() && sdFontTestShapeReads == sdFontTestShortShapeReadAt && count) --count;
    std::memcpy(output, bytes_->data() + position_, count);
    position_ += count;
    if (shape()) {
      sdFontTestShapeBytes += count;
      if (count != requested) ++sdFontTestShapeFailures;
    }
    return static_cast<int>(count);
  }
  void close() {
    if (opened_ && shape()) --sdFontTestShapeOpenHandles;
    opened_ = false;
  }
  size_t size() const { return shape() ? std::min(bytes_->size(), sdFontTestShapeEof) : bytes_->size(); }
  void open(const std::vector<uint8_t>* bytes = &sdFontTestFile) {
    close();
    bytes_ = bytes;
    opened_ = true;
    position_ = 0;
    if (shape()) ++sdFontTestShapeOpenHandles;
  }

 private:
  bool shape() const { return bytes_ == &sdFontTestCompanion; }
  const std::vector<uint8_t>* bytes_ = &sdFontTestFile;
  size_t position_ = 0;
  bool opened_ = false;
};

class HalStorage {
 public:
  bool exists(const char* path) const {
    return std::strstr(path, ".cpshape") ? !sdFontTestCompanion.empty() : !sdFontTestFile.empty();
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    if (!exists(path)) return false;
    file.open(std::strstr(path, ".cpshape") ? &sdFontTestCompanion : &sdFontTestFile);
    return true;
  }
};
inline HalStorage Storage;
