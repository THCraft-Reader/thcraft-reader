#pragma once

#ifdef CROSSPOINT_NATIVE_TEXT

#include <HalStorage.h>
#include <NativeTextTypes.h>

#include <cstdint>
#include <memory>
#include <string>

#include "NativeTxtPaginator.h"

class GfxRenderer;
class Page;
class Txt;

// Disk owns completed pages; only the selected page and a fixed-size LUT record
// are read into memory. Source positions are original UTF-8 file byte offsets.
class NativeTxtCache {
 public:
  enum class Result { Ready, Rebuild, Error };
  NativeTxtCache() = default;
  ~NativeTxtCache();
  NativeTxtCache(const NativeTxtCache&) = delete;
  NativeTxtCache& operator=(const NativeTxtCache&) = delete;

  Result open(const Txt& txt, GfxRenderer& renderer, uint16_t width, uint16_t height, uint8_t alignment);
  bool build(GfxRenderer& renderer);
  Result loadPage(uint32_t index, std::unique_ptr<Page>& page, uint32_t& sourceStart, uint32_t& sourceEnd);
  bool restorePage(uint32_t& pageIndex);
  bool pageForSource(uint32_t sourceByte, uint32_t& pageIndex);
  bool saveProgress(uint32_t sourceByte);
  bool matches(GfxRenderer& renderer, uint16_t width, uint16_t height, uint8_t alignment) const;
  uint32_t pageCount() const { return pageCount_; }
  uint32_t sourceSize() const { return sourceSize_; }
  uint64_t sourceHash() const { return sourceHash_; }
  TextStatus lastStatus() const { return status_; }
  void close();

 private:
  struct Record {
    uint32_t sourceStart = 0, sourceEnd = 0, pageOffset = 0, pageBytes = 0;
  };
  const Txt* txt_ = nullptr;
  std::string directory_;
  HalFile index_;
  NativeTxtPaginator paginator_;
  int32_t fontId_ = 0;
  uint16_t width_ = 0, height_ = 0;
  uint8_t alignment_ = 0;
  uint64_t fingerprint_ = 0, sourceHash_ = 0;
  uint32_t sourceSize_ = 0, pageCount_ = 0, lutOffset_ = 0;
  TextStatus status_ = TextStatus::Ok;
  bool ready_ = false;

  bool progressValid_ = false, legacyPageValid_ = false;
  uint32_t progressByte_ = 0, progressSize_ = 0, legacyPage_ = 0;
  uint64_t progressHash_ = 0;
  bool savedValid_ = false;
  uint32_t savedByte_ = 0, savedSize_ = 0;
  uint64_t savedHash_ = 0;

  Result failure(TextStatus status);
  Result readHeaderAndLut(HalFile& file);
  Result readRecord(HalFile& file, uint32_t index, Record& record);
  Result decodePage(HalFile& file, const Record& record, std::unique_ptr<Page>& page);
  bool captureProgress();
  bool captureLegacyProgress();
  bool writeHeader(HalFile& file, uint8_t version);
  bool recoverBackup(const std::string& path);
  bool replaceIndex(const std::string& temporary);
};

#endif
