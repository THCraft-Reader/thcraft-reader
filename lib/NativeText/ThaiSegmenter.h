#pragma once

#include "NativeTextTypes.h"

class ThaiSegmenter {
  void* breaker_ = nullptr;
  NativeBuffer<uint8_t> tis_;
  NativeBuffer<uint32_t> byteMap_;
  NativeBuffer<int> positions_;
  NativeBuffer<char> hints_;

  TextStatus find(std::string_view utf8, std::span<uint32_t> byteOffsets, size_t& count, bool emergency);

 public:
  static constexpr size_t MAX_SCALARS = 4096;
  static constexpr size_t MAX_BYTES = 16 * 1024;
  ThaiSegmenter() = default;
  ThaiSegmenter(const ThaiSegmenter&) = delete;
  ThaiSegmenter& operator=(const ThaiSegmenter&) = delete;
  ~ThaiSegmenter();
  TextStatus initialize();
  void shutdown();
  bool ready() const { return breaker_ != nullptr; }
  TextStatus findBreaks(std::string_view utf8, std::span<uint32_t> byteOffsets, size_t& count);
  // Candidate typographic boundaries only; callers must intersect HB clusters.
  TextStatus findEmergencyBreaks(std::string_view utf8, std::span<uint32_t> byteOffsets, size_t& count);
};
