#include "ThaiSegmenter.h"

#include <thai/thbrk.h>
#include <thai/thwchar.h>

#include "NativePlatform.h"
#include "NativeThaiDictionary.generated.h"
#include "NativeUtf8.h"
extern "C" {
#include <thbrk/brk-common.h>
}

namespace {
struct SegmenterLock {
  SegmenterLock() { nativeTextLock(); }
  ~SegmenterLock() { nativeTextUnlock(); }
};
TextStatus segmentError(TextStatus status, size_t& count) {
  count = 0;
  nativeTextLogError("Thai segmentation failed");
  return status;
}
}  // namespace

ThaiSegmenter::~ThaiSegmenter() { shutdown(); }

TextStatus ThaiSegmenter::initialize() {
  SegmenterLock lock;
  if (breaker_) return TextStatus::Ok;
  const size_t failures = native_text::allocationStats().failures;
  breaker_ = th_brk_new_from_memory(native_text::assets::thaiDictionary, native_text::assets::thaiDictionarySize);
  if (!breaker_) {
    nativeTextLogError("Unable to initialize the bundled Thai dictionary");
    return native_text::allocationStats().failures != failures ? TextStatus::OutOfMemory : TextStatus::InvalidText;
  }
  return TextStatus::Ok;
}

void ThaiSegmenter::shutdown() {
  SegmenterLock lock;
  if (breaker_) th_brk_delete(static_cast<ThBrk*>(breaker_));
  breaker_ = nullptr;
  tis_.reset();
  byteMap_.reset();
  positions_.reset();
  hints_.reset();
}

TextStatus ThaiSegmenter::findBreaks(std::string_view utf8, std::span<uint32_t> byteOffsets, size_t& count) {
  return find(utf8, byteOffsets, count, false);
}

TextStatus ThaiSegmenter::findEmergencyBreaks(std::string_view utf8, std::span<uint32_t> byteOffsets, size_t& count) {
  return find(utf8, byteOffsets, count, true);
}

TextStatus ThaiSegmenter::find(std::string_view utf8, std::span<uint32_t> byteOffsets, size_t& count, bool emergency) {
  SegmenterLock lock;
  count = 0;
  if (utf8.empty()) return TextStatus::Ok;
  if (!breaker_) return segmentError(TextStatus::InvalidText, count);
  if (utf8.size() > MAX_BYTES) return segmentError(TextStatus::CapacityExceeded, count);
  size_t offset = 0, scalars = 0;
  uint32_t cp = 0;
  while (offset < utf8.size()) {
    if (!native_text::nextUtf8(utf8, offset, cp)) return segmentError(TextStatus::InvalidText, count);
    if (++scalars > MAX_SCALARS) return segmentError(TextStatus::CapacityExceeded, count);
  }
  if (!tis_.resize(scalars + 1) || !byteMap_.resize(scalars + 1) || !positions_.resize(scalars) ||
      (emergency && !hints_.resize(scalars))) {
    return segmentError(TextStatus::OutOfMemory, count);
  }
  size_t length = 0;
  bool hasThai = false;
  auto emit = [&](uint32_t byteOffset) {
    if (!byteOffset || byteOffset >= utf8.size() || (count && byteOffsets[count - 1] == byteOffset)) return true;
    if (count == byteOffsets.size()) return false;
    byteOffsets[count++] = byteOffset;
    return true;
  };
  auto flush = [&]() -> TextStatus {
    if (!length || !hasThai) {
      length = 0;
      hasThai = false;
      return TextStatus::Ok;
    }
    tis_[length] = 0;
    if (emergency) {
      brk_brkpos_hints(tis_.data(), static_cast<int>(length), hints_.data());
      for (size_t i = 1; i < length; ++i) {
        // ASCII supplies punctuation context, not emergency Latin word splits.
        if (hints_[i] && tis_[i] >= 0xa1 && tis_[i - 1] >= 0xa1 && !emit(byteMap_[i])) {
          return TextStatus::CapacityExceeded;
        }
      }
    } else {
      const size_t failures = native_text::allocationStats().failures;
      const int found = th_brk_find_breaks(static_cast<ThBrk*>(breaker_), tis_.data(), positions_.data(), length);
      if (native_text::allocationStats().failures != failures) return TextStatus::OutOfMemory;
      if (found < 0 || static_cast<size_t>(found) > length) return TextStatus::InvalidText;
      for (int i = 0; i < found; ++i) {
        const int position = positions_[i];
        if (position < 0 || static_cast<size_t>(position) > length) return TextStatus::InvalidText;
        if (!emit(byteMap_[position])) return TextStatus::CapacityExceeded;
      }
    }
    length = 0;
    hasThai = false;
    nativeTextYield();
    return TextStatus::Ok;
  };
  offset = 0;
  while (offset < utf8.size()) {
    const size_t start = offset;
    native_text::nextUtf8(utf8, offset, cp);  // Validated in the first pass.
    const bool representable = (cp > 0 && cp < 0x80) || native_text::isThai(cp);
    if (!representable) {
      const TextStatus status = flush();
      if (status != TextStatus::Ok) return segmentError(status, count);
      if (cp == 0x200b && !emit(static_cast<uint32_t>(offset)))
        return segmentError(TextStatus::CapacityExceeded, count);
      continue;
    }
    byteMap_[length] = static_cast<uint32_t>(start);
    tis_[length++] = th_uni2tis(cp);
    byteMap_[length] = static_cast<uint32_t>(offset);
    hasThai = hasThai || native_text::isThai(cp);
  }
  const TextStatus status = flush();
  return status == TextStatus::Ok ? status : segmentError(status, count);
}
