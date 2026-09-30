#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

inline std::vector<uint8_t> sdFontTestFile;
inline std::vector<uint8_t> sdFontTestCompanion;
inline size_t sdFontTestReads = 0;

struct SdFontTestEsp {
  size_t largestBlock = 200 * 1024;
  size_t getFreeHeap() const { return 200 * 1024; }
  size_t getMaxAllocHeap() const { return largestBlock; }
};
inline SdFontTestEsp ESP;
inline uint32_t millis() { return 0; }

class HalFile {
 public:
  bool seekSet(size_t position) {
    position_ = position;
    return opened_ && position <= bytes_->size();
  }
  int read(void* output, size_t count) {
    if (!opened_ || position_ > bytes_->size()) return 0;
    count = std::min(count, bytes_->size() - position_);
    std::memcpy(output, bytes_->data() + position_, count);
    position_ += count;
    sdFontTestReads++;
    return static_cast<int>(count);
  }
  void close() { opened_ = false; }
  size_t size() const { return bytes_->size(); }
  void open(const std::vector<uint8_t>* bytes = &sdFontTestFile) {
    bytes_ = bytes;
    opened_ = true;
    position_ = 0;
  }

 private:
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
