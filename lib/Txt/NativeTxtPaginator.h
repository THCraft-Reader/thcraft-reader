#pragma once

#ifdef CROSSPOINT_NATIVE_TEXT
#include <NativeTextTypes.h>

#include <cstdint>
#include <memory>

class GfxRenderer;
class Page;
class Txt;

// File-byte adapter over the shared paragraph fitter. A source window is fitted
// once; its bounded pending lines survive page turns without restarting bidi or
// segmentation in the middle of a physical paragraph.
class NativeTxtPaginator {
 public:
  enum class Result { PageReady, End, Error };

  NativeTxtPaginator() = default;
  ~NativeTxtPaginator();
  NativeTxtPaginator(const NativeTxtPaginator&) = delete;
  NativeTxtPaginator& operator=(const NativeTxtPaginator&) = delete;

  bool begin(const Txt& txt, GfxRenderer& renderer, uint16_t width, uint16_t height, uint8_t alignment);
  Result nextPage(std::unique_ptr<Page>& page, uint32_t& sourceStart, uint32_t& sourceEnd);
  TextStatus hashSource(const Txt& txt, uint64_t& hash);
  uint64_t sourceHash() const { return sourceHash_; }
  uint32_t sourceSize() const { return sourceSize_; }
  bool emptySource() const { return emptySource_; }
  TextStatus lastStatus() const { return status_; }
  void close();

 private:
  struct State;
  State* state_ = nullptr;
  NativeBuffer<uint8_t> input_;
  uint64_t sourceHash_ = 14695981039346656037ULL;
  uint32_t sourceSize_ = 0;
  TextStatus status_ = TextStatus::Ok;
  bool emptySource_ = true;

  void releaseState();
  TextStatus fail(TextStatus status);
};
#endif
