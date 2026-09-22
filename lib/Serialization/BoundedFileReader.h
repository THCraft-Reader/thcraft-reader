#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace serialization {
enum class FileReadStatus : uint8_t { Ok, InvalidData, StorageError, OutOfMemory };

// A borrowed file range; failed bounds checks never touch the underlying file.
class BoundedFileReader {
  HalFile& file_;
  size_t begin_ = 0;
  size_t end_ = 0;
  FileReadStatus status_ = FileReadStatus::Ok;

 public:
  explicit BoundedFileReader(HalFile& file, size_t bytes = std::numeric_limits<size_t>::max()) : file_(file) {
    if (!file_) {
      status_ = FileReadStatus::StorageError;
      return;
    }
    begin_ = file_.position();
    const size_t length = file_.size();
    if (begin_ > length || (bytes != std::numeric_limits<size_t>::max() && bytes > length - begin_)) {
      status_ = FileReadStatus::InvalidData;
      return;
    }
    end_ = bytes == std::numeric_limits<size_t>::max() ? length : begin_ + bytes;
  }
  int read(void* destination, size_t bytes) {
    if (status_ != FileReadStatus::Ok) return -1;
    const size_t position = file_.position();
    if (position < begin_ || position > end_ || bytes > end_ - position) {
      status_ = FileReadStatus::InvalidData;
      return -1;
    }
    if (!bytes) return 0;
    const int received = file_.read(destination, bytes);
    if (received < 0 || static_cast<size_t>(received) != bytes) status_ = FileReadStatus::StorageError;
    return received;
  }
  bool seek(size_t position) {
    if (status_ != FileReadStatus::Ok) return false;
    if (position < begin_ || position > end_) {
      status_ = FileReadStatus::InvalidData;
      return false;
    }
    if (!file_.seek(position)) {
      status_ = FileReadStatus::StorageError;
      return false;
    }
    return true;
  }
  size_t position() const { return file_.position(); }
  size_t size() const { return end_; }
  FileReadStatus status() const { return status_; }
  bool valid() const { return status_ == FileReadStatus::Ok; }
  void outOfMemory() {
    if (status_ == FileReadStatus::Ok) status_ = FileReadStatus::OutOfMemory;
  }
};
}  // namespace serialization
