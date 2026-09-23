#include "NativeTxtCache.h"

#ifdef CROSSPOINT_NATIVE_TEXT

#include <CrossPointSettings.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <NativePlatform.h>

#include <algorithm>

#include "Txt.h"
#include "activities/reader/ProgressFile.h"

namespace {
constexpr uint32_t INDEX_MAGIC = 0x4e545854;
constexpr uint8_t INDEX_VERSION = 2;
constexpr uint32_t PROGRESS_MAGIC = 0x52505854;
constexpr uint32_t LEGACY_MAGIC = 0x54585449;
constexpr uint32_t HEADER_BYTES = 42, RECORD_BYTES = 16, PROGRESS_BYTES = 21, LEGACY_HEADER_BYTES = 30;
constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL, FNV_PRIME = 1099511628211ULL;

void put(uint8_t* bytes, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
}
uint64_t get(const uint8_t* bytes, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i) value |= uint64_t(bytes[i]) << (i * 8);
  return value;
}
TextStatus readExact(HalFile& file, void* output, size_t bytes) {
  const uint64_t position = file.position(), size = file.fileSize64();
  if (position > size || bytes > size - position) return TextStatus::InvalidText;
  auto* out = static_cast<uint8_t*>(output);
  while (bytes) {
    const int got = file.read(out, bytes);
    if (got <= 0 || static_cast<size_t>(got) > bytes) return TextStatus::StorageError;
    out += got;
    bytes -= static_cast<size_t>(got);
  }
  return TextStatus::Ok;
}
uint64_t layoutIdentity(GfxRenderer& renderer, int font) {
  // Change this revision whenever TXT byte consumption or its Page codec changes.
  constexpr char revision[] = "native-txt-v1/page-v2";
  uint64_t hash = FNV_OFFSET;
  for (size_t i = 0; i < sizeof(revision) - 1; ++i) hash = (hash ^ uint8_t(revision[i])) * FNV_PRIME;
  const uint64_t engine = renderer.textLayoutFingerprint(font);
  for (size_t i = 0; i < 8; ++i) hash = (hash ^ uint8_t(engine >> (i * 8))) * FNV_PRIME;
  return hash;
}
}  // namespace

NativeTxtCache::~NativeTxtCache() { close(); }

NativeTxtCache::Result NativeTxtCache::failure(TextStatus status) {
  status_ = status;
  if (status != TextStatus::Ok) LOG_ERR("TEXT", "Native TXT cache failure: %u", static_cast<unsigned>(status));
  return status == TextStatus::InvalidText || status == TextStatus::Ok ? Result::Rebuild : Result::Error;
}

void NativeTxtCache::close() {
  index_.close();
  paginator_.close();
  txt_ = nullptr;
  directory_.clear();
  ready_ = progressValid_ = legacyPageValid_ = savedValid_ = false;
  pageCount_ = sourceSize_ = lutOffset_ = 0;
  sourceHash_ = 0;
  status_ = TextStatus::Ok;
}

bool NativeTxtCache::matches(GfxRenderer& renderer, uint16_t width, uint16_t height, uint8_t alignment) const {
  return txt_ && renderer.usesNativeText() && fontId_ == SETTINGS.getReaderFontId() && width_ == width &&
         height_ == height && alignment_ == alignment && fingerprint_ == layoutIdentity(renderer, fontId_);
}

bool NativeTxtCache::recoverBackup(const std::string& path) {
  if (Storage.exists(path.c_str())) return true;
  const std::string backup = path + ".bak";
  if (!Storage.exists(backup.c_str())) return Storage.ready();
  if (Storage.rename(backup.c_str(), path.c_str())) return true;
  failure(TextStatus::StorageError);
  return false;
}

NativeTxtCache::Result NativeTxtCache::open(const Txt& txt, GfxRenderer& renderer, uint16_t width, uint16_t height,
                                            uint8_t alignment) {
  close();
  txt_ = &txt;
  directory_ = txt.getCachePath() + "/native";
  fontId_ = SETTINGS.getReaderFontId();
  width_ = width;
  height_ = height;
  alignment_ = alignment;
  fingerprint_ = layoutIdentity(renderer, fontId_);
  if (!renderer.usesNativeText()) return failure(TextStatus::InvalidFont);
  if (!width || !height || width > INT16_MAX || height > INT16_MAX) return failure(TextStatus::CapacityExceeded);
  {
    HalFile source;
    if (!Storage.openFileForRead("TEXT", txt.getPath(), source)) return failure(TextStatus::StorageError);
    const uint64_t size = source.fileSize64();
    if (size > UINT32_MAX) return failure(TextStatus::CapacityExceeded);
    sourceSize_ = static_cast<uint32_t>(size);
    if (!source.close()) return failure(TextStatus::StorageError);
  }
  if (!captureProgress()) return failure(status_ == TextStatus::Ok ? TextStatus::StorageError : status_);
  const std::string path = directory_ + "/index.bin";
  if (!recoverBackup(path)) return failure(TextStatus::StorageError);
  if (!Storage.exists(path.c_str())) return failure(Storage.ready() ? TextStatus::Ok : TextStatus::StorageError);
  if (!Storage.openFileForRead("TEXT", path, index_)) return failure(TextStatus::StorageError);
  const auto result = readHeaderAndLut(index_);
  if (result != Result::Ready) {
    index_.close();
    pageCount_ = 0;
    return result;
  }
  uint64_t actualHash = 0;
  const auto status = paginator_.hashSource(txt, actualHash);
  if (status != TextStatus::Ok) {
    index_.close();
    return failure(status);
  }
  const bool unchanged = sourceSize_ == paginator_.sourceSize() && sourceHash_ == actualHash;
  const bool validEmpty = (pageCount_ == 0) == paginator_.emptySource();
  sourceSize_ = paginator_.sourceSize();
  sourceHash_ = actualHash;
  if (!unchanged || !validEmpty) {
    index_.close();
    pageCount_ = 0;
    return failure(validEmpty ? TextStatus::Ok : TextStatus::InvalidText);
  }
  paginator_.close();
  ready_ = true;
  status_ = TextStatus::Ok;
  return Result::Ready;
}

NativeTxtCache::Result NativeTxtCache::readHeaderAndLut(HalFile& file) {
  const uint64_t size = file.fileSize64();
  if (size < HEADER_BYTES || size > UINT32_MAX) return failure(TextStatus::InvalidText);
  if (!file.seek(0)) return failure(TextStatus::StorageError);
  uint8_t header[HEADER_BYTES];
  const auto status = readExact(file, header, sizeof(header));
  if (status != TextStatus::Ok) return failure(status);
  if (get(header, 4) != INDEX_MAGIC || header[4] != INDEX_VERSION) return failure(TextStatus::InvalidText);
  if (get(header + 5, 4) != sourceSize_ || get(header + 17, 8) != fingerprint_ ||
      static_cast<int32_t>(get(header + 25, 4)) != fontId_ || get(header + 29, 2) != width_ ||
      get(header + 31, 2) != height_ || header[33] != alignment_)
    return failure(TextStatus::Ok);
  sourceHash_ = get(header + 9, 8);
  pageCount_ = static_cast<uint32_t>(get(header + 34, 4));
  lutOffset_ = static_cast<uint32_t>(get(header + 38, 4));
  if (lutOffset_ < HEADER_BYTES || lutOffset_ > size || uint64_t(pageCount_) * RECORD_BYTES != size - lutOffset_ ||
      (!pageCount_ && lutOffset_ != HEADER_BYTES))
    return failure(TextStatus::InvalidText);
  uint32_t sourceEnd = 0, pageEnd = HEADER_BYTES;
  for (uint32_t i = 0; i < pageCount_; ++i) {
    Record record;
    const auto result = readRecord(file, i, record);
    if (result != Result::Ready) return result;
    if (record.sourceStart != sourceEnd || record.pageOffset != pageEnd) return failure(TextStatus::InvalidText);
    sourceEnd = record.sourceEnd;
    pageEnd = record.pageOffset + record.pageBytes;
    nativeTextYield();
  }
  if (pageEnd != lutOffset_ || (pageCount_ && sourceEnd != sourceSize_)) return failure(TextStatus::InvalidText);
  return Result::Ready;
}

NativeTxtCache::Result NativeTxtCache::readRecord(HalFile& file, uint32_t index, Record& record) {
  if (index >= pageCount_) return failure(TextStatus::InvalidText);
  const uint64_t offset = uint64_t(lutOffset_) + uint64_t(index) * RECORD_BYTES;
  if (!file.seek64(offset)) return failure(TextStatus::StorageError);
  uint8_t bytes[RECORD_BYTES];
  const auto status = readExact(file, bytes, sizeof(bytes));
  if (status != TextStatus::Ok) return failure(status);
  record.sourceStart = static_cast<uint32_t>(get(bytes, 4));
  record.sourceEnd = static_cast<uint32_t>(get(bytes + 4, 4));
  record.pageOffset = static_cast<uint32_t>(get(bytes + 8, 4));
  record.pageBytes = static_cast<uint32_t>(get(bytes + 12, 4));
  if (record.sourceStart >= record.sourceEnd || record.sourceEnd > sourceSize_ || record.pageOffset < HEADER_BYTES ||
      record.pageOffset > lutOffset_ || record.pageBytes < 6 || record.pageBytes > lutOffset_ - record.pageOffset)
    return failure(TextStatus::InvalidText);
  return Result::Ready;
}

NativeTxtCache::Result NativeTxtCache::decodePage(HalFile& file, const Record& record, std::unique_ptr<Page>& page) {
  page.reset();
  if (!file.seek(record.pageOffset)) return failure(TextStatus::StorageError);
  TextStatus status = TextStatus::Ok;
  page = Page::deserialize(file, record.pageBytes, status);
  if (!page) return failure(status == TextStatus::Ok ? TextStatus::InvalidText : status);
  if (file.position() != uint64_t(record.pageOffset) + record.pageBytes || page->elements.empty() ||
      !page->links.empty() || !page->footnotes.empty()) {
    page.reset();
    return failure(TextStatus::InvalidText);
  }
  for (const auto& element : page->elements) {
    if (!element || element->getTag() != TAG_PageLine || element->xPos != 0 || element->yPos < 0 ||
        element->yPos >= height_ || !static_cast<const PageLine&>(*element).getBlock()->nativeLine()) {
      page.reset();
      return failure(TextStatus::InvalidText);
    }
  }
  page->visibleTextOffset = record.sourceStart;
  status_ = TextStatus::Ok;
  return Result::Ready;
}

NativeTxtCache::Result NativeTxtCache::loadPage(uint32_t index, std::unique_ptr<Page>& page, uint32_t& sourceStart,
                                                uint32_t& sourceEnd) {
  page.reset();
  if (!ready_ || !index_) return failure(TextStatus::InvalidText);
  Record record;
  const auto result = readRecord(index_, index, record);
  if (result != Result::Ready) return result;
  sourceStart = record.sourceStart;
  sourceEnd = record.sourceEnd;
  return decodePage(index_, record, page);
}

bool NativeTxtCache::writeHeader(HalFile& file, uint8_t version) {
  uint8_t header[HEADER_BYTES]{};
  put(header, INDEX_MAGIC, 4);
  header[4] = version;
  put(header + 5, sourceSize_, 4);
  put(header + 9, sourceHash_, 8);
  put(header + 17, fingerprint_, 8);
  put(header + 25, static_cast<uint32_t>(fontId_), 4);
  put(header + 29, width_, 2);
  put(header + 31, height_, 2);
  header[33] = alignment_;
  put(header + 34, pageCount_, 4);
  put(header + 38, lutOffset_, 4);
  return file.seek(0) && file.write(header, sizeof(header)) == sizeof(header);
}

bool NativeTxtCache::replaceIndex(const std::string& temporary) {
  const std::string final = directory_ + "/index.bin", backup = final + ".bak";
  if (!recoverBackup(final)) return false;
  const bool hadPrevious = Storage.exists(final.c_str());
  if (hadPrevious) {
    if (Storage.exists(backup.c_str()) && !Storage.remove(backup.c_str())) return false;
    if (!Storage.rename(final.c_str(), backup.c_str())) return false;
  }
  if (!Storage.rename(temporary.c_str(), final.c_str())) {
    if (hadPrevious && !Storage.rename(backup.c_str(), final.c_str())) {
      LOG_ERR("TEXT", "Previous TXT index retained in recovery backup");
    }
    return false;
  }
  return true;
}

bool NativeTxtCache::build(GfxRenderer& renderer) {
  if (!txt_ || !matches(renderer, width_, height_, alignment_)) {
    failure(TextStatus::InvalidFont);
    return false;
  }
  index_.close();
  ready_ = false;
  if (!Storage.ensureDirectoryExists(directory_.c_str())) {
    failure(TextStatus::StorageError);
    return false;
  }
  const std::string temporary = directory_ + "/index.bin.tmp", lutPath = directory_ + "/index.lut.tmp";
  HalFile file, lut;
  const auto abort = [&](TextStatus status) {
    file.close();
    lut.close();
    paginator_.close();
    Storage.remove(temporary.c_str());
    Storage.remove(lutPath.c_str());
    pageCount_ = 0;
    failure(status);
    return false;
  };
  if (!paginator_.begin(*txt_, renderer, width_, height_, alignment_)) return abort(paginator_.lastStatus());
  sourceSize_ = paginator_.sourceSize();
  sourceHash_ = 0;
  pageCount_ = lutOffset_ = 0;
  if (!Storage.openFileForWrite("TEXT", temporary, file) || !Storage.openFileForWrite("TEXT", lutPath, lut) ||
      !writeHeader(file, 0))
    return abort(TextStatus::StorageError);
  uint32_t previousEnd = 0;
  for (;;) {
    std::unique_ptr<Page> page;
    uint32_t start = 0, end = 0;
    const auto result = paginator_.nextPage(page, start, end);
    if (result == NativeTxtPaginator::Result::Error) return abort(paginator_.lastStatus());
    if (result == NativeTxtPaginator::Result::End) break;
    if (!page || start != previousEnd || start >= end || end > sourceSize_) return abort(TextStatus::InvalidText);
    const uint64_t offset = file.position();
    if (!page->serialize(file)) return abort(TextStatus::StorageError);
    const uint64_t after = file.position();
    if (offset > UINT32_MAX || after > UINT32_MAX || after < offset || after - offset < 6 || pageCount_ == UINT32_MAX)
      return abort(TextStatus::CapacityExceeded);
    uint8_t record[RECORD_BYTES];
    put(record, start, 4);
    put(record + 4, end, 4);
    put(record + 8, offset, 4);
    put(record + 12, after - offset, 4);
    if (lut.write(record, sizeof(record)) != sizeof(record)) return abort(TextStatus::StorageError);
    ++pageCount_;
    previousEnd = end;
    nativeTextYield();
  }
  if ((pageCount_ && previousEnd != sourceSize_) || (!pageCount_ && !paginator_.emptySource()))
    return abort(TextStatus::InvalidText);
  sourceHash_ = paginator_.sourceHash();
  paginator_.close();
  if (!matches(renderer, width_, height_, alignment_)) return abort(TextStatus::InvalidFont);
  const uint64_t lutOffset = file.position();
  if (lutOffset + uint64_t(pageCount_) * RECORD_BYTES > UINT32_MAX) return abort(TextStatus::CapacityExceeded);
  lutOffset_ = static_cast<uint32_t>(lutOffset);
  lut.flush();
  if (!lut.close() || !Storage.openFileForRead("TEXT", lutPath, lut)) return abort(TextStatus::StorageError);
  for (uint32_t i = 0; i < pageCount_; ++i) {
    uint8_t record[RECORD_BYTES];
    const auto status = readExact(lut, record, sizeof(record));
    if (status != TextStatus::Ok) return abort(status);
    if (file.write(record, sizeof(record)) != sizeof(record)) return abort(TextStatus::StorageError);
    nativeTextYield();
  }
  if (!lut.close() || !writeHeader(file, 0)) return abort(TextStatus::StorageError);
  file.flush();
  // Decode each completed body within its LUT range before publishing. Only one
  // Page is resident, and the version remains incomplete throughout validation.
  for (uint32_t i = 0; i < pageCount_; ++i) {
    Record record;
    std::unique_ptr<Page> page;
    if (readRecord(file, i, record) != Result::Ready || decodePage(file, record, page) != Result::Ready)
      return abort(status_);
    nativeTextYield();
  }
  if (!matches(renderer, width_, height_, alignment_)) return abort(TextStatus::InvalidFont);
  const uint8_t complete = INDEX_VERSION;
  if (!file.seek(4) || file.write(&complete, 1) != 1) return abort(TextStatus::StorageError);
  file.flush();
  if (!file.close()) return abort(TextStatus::StorageError);
  if (!replaceIndex(temporary)) return abort(TextStatus::StorageError);
  Storage.remove(lutPath.c_str());
  const std::string published = directory_ + "/index.bin", backup = published + ".bak";
  const auto result =
      Storage.openFileForRead("TEXT", published, index_) ? readHeaderAndLut(index_) : failure(TextStatus::StorageError);
  if (result != Result::Ready) {
    index_.close();
    if (Storage.exists(backup.c_str())) {
      Storage.remove(published.c_str());
      if (!Storage.rename(backup.c_str(), published.c_str()))
        LOG_ERR("TEXT", "Previous TXT index retained in recovery backup");
    }
    return abort(status_);
  }
  Storage.remove(backup.c_str());
  ready_ = true;
  status_ = TextStatus::Ok;
  return true;
}

bool NativeTxtCache::pageForSource(uint32_t sourceByte, uint32_t& pageIndex) {
  pageIndex = 0;
  if (!ready_) {
    failure(TextStatus::InvalidText);
    return false;
  }
  if (!pageCount_) return true;
  sourceByte = std::min(sourceByte, sourceSize_);
  uint32_t low = 0, high = pageCount_;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    Record record;
    if (readRecord(index_, middle, record) != Result::Ready) return false;
    if (record.sourceStart <= sourceByte)
      low = middle + 1;
    else
      high = middle;
  }
  pageIndex = low ? low - 1 : 0;
  status_ = TextStatus::Ok;
  return true;
}

bool NativeTxtCache::captureProgress() {
  const std::string path = directory_ + "/progress.bin";
  if (!recoverBackup(path)) return false;
  if (Storage.exists(path.c_str())) {
    HalFile file;
    if (!Storage.openFileForRead("TEXT", path, file)) {
      failure(TextStatus::StorageError);
      return false;
    }
    if (file.fileSize64() == PROGRESS_BYTES) {
      uint8_t bytes[PROGRESS_BYTES];
      const auto status = readExact(file, bytes, sizeof(bytes));
      if (status == TextStatus::StorageError) {
        failure(status);
        return false;
      }
      if (status == TextStatus::Ok && get(bytes, 4) == PROGRESS_MAGIC && bytes[4] == 1 &&
          get(bytes + 5, 4) <= get(bytes + 9, 4)) {
        progressValid_ = savedValid_ = true;
        progressByte_ = savedByte_ = static_cast<uint32_t>(get(bytes + 5, 4));
        progressSize_ = savedSize_ = static_cast<uint32_t>(get(bytes + 9, 4));
        progressHash_ = savedHash_ = get(bytes + 13, 8);
        return true;
      }
    }
  }
  return captureLegacyProgress();
}

bool NativeTxtCache::captureLegacyProgress() {
  const std::string progress = txt_->getCachePath() + "/progress.bin";
  if (!Storage.exists(progress.c_str())) return Storage.ready();
  HalFile file;
  if (!Storage.openFileForRead("TEXT", progress, file)) {
    failure(TextStatus::StorageError);
    return false;
  }
  uint8_t bytes[LEGACY_HEADER_BYTES];
  if (file.fileSize64() != 4) return true;
  auto status = readExact(file, bytes, 4);
  if (status != TextStatus::Ok) {
    failure(status);
    return false;
  }
  legacyPage_ = static_cast<uint32_t>(get(bytes, 4));
  legacyPageValid_ = true;
  file.close();
  const std::string path = txt_->getCachePath() + "/index.bin";
  if (!Storage.exists(path.c_str())) return true;
  if (!Storage.openFileForRead("TEXT", path, file)) {
    failure(TextStatus::StorageError);
    return false;
  }
  if (file.fileSize64() < LEGACY_HEADER_BYTES) return true;
  status = readExact(file, bytes, sizeof(bytes));
  if (status != TextStatus::Ok) {
    failure(status);
    return false;
  }
  const uint32_t count = static_cast<uint32_t>(get(bytes + 26, 4));
  if (get(bytes, 4) != LEGACY_MAGIC || bytes[4] != 3 || get(bytes + 5, 4) != sourceSize_ || !count ||
      uint64_t(LEGACY_HEADER_BYTES) + uint64_t(count) * 4 != file.fileSize64())
    return true;
  const uint32_t selected = std::min(legacyPage_, count - 1);
  uint32_t previous = 0, selectedByte = 0;
  for (uint32_t i = 0; i < count; ++i) {
    status = readExact(file, bytes, 4);
    if (status != TextStatus::Ok) {
      failure(status);
      return false;
    }
    const uint32_t offset = static_cast<uint32_t>(get(bytes, 4));
    if (offset > sourceSize_ || (i == 0 && offset != 0) || (i && offset <= previous)) return true;
    if (i == selected) selectedByte = offset;
    previous = offset;
    nativeTextYield();
  }
  // No old source hash exists. The v3 size/range proof supplies an exact byte
  // coordinate, but cannot promise a semantic match after a same-size edit.
  progressValid_ = true;
  progressByte_ = selectedByte;
  progressSize_ = sourceSize_;
  progressHash_ = 0;
  legacyPageValid_ = false;
  return true;
}

bool NativeTxtCache::restorePage(uint32_t& pageIndex) {
  if (progressValid_) {
    if (progressSize_ != sourceSize_ || (progressHash_ && progressHash_ != sourceHash_))
      LOG_DBG("TEXT", "TXT source changed; restoring preceding byte position best-effort");
    return pageForSource(std::min(progressByte_, sourceSize_), pageIndex);
  }
  if (!ready_) {
    failure(TextStatus::InvalidText);
    return false;
  }
  pageIndex = legacyPageValid_ && pageCount_ ? std::min(legacyPage_, pageCount_ - 1) : 0;
  status_ = TextStatus::Ok;
  return true;
}

bool NativeTxtCache::saveProgress(uint32_t sourceByte) {
  if (!ready_ || !pageCount_ || sourceByte > sourceSize_) {
    failure(TextStatus::InvalidText);
    return false;
  }
  if (savedValid_ && savedByte_ == sourceByte && savedSize_ == sourceSize_ && savedHash_ == sourceHash_) {
    status_ = TextStatus::Ok;
    return true;
  }
  uint8_t bytes[PROGRESS_BYTES];
  put(bytes, PROGRESS_MAGIC, 4);
  bytes[4] = 1;
  put(bytes + 5, sourceByte, 4);
  put(bytes + 9, sourceSize_, 4);
  put(bytes + 13, sourceHash_, 8);
  const std::string final = directory_ + "/progress.bin", backup = final + ".bak";
  if (!recoverBackup(final)) return false;
  const bool previous = Storage.exists(final.c_str());
  if (previous && ((Storage.exists(backup.c_str()) && !Storage.remove(backup.c_str())) ||
                   !Storage.rename(final.c_str(), backup.c_str()))) {
    failure(TextStatus::StorageError);
    return false;
  }
  if (!ProgressFile::writeAtomic(directory_, bytes, sizeof(bytes))) {
    // The shared helper deliberately removes its destination before renaming.
    // Native resume has a stronger contract, so retain a recoverable old copy.
    if (previous && !Storage.rename(backup.c_str(), final.c_str()))
      LOG_ERR("TEXT", "Previous TXT progress retained in recovery backup");
    Storage.remove((final + ".tmp").c_str());
    failure(TextStatus::StorageError);
    return false;
  }
  if (previous) Storage.remove(backup.c_str());
  progressValid_ = savedValid_ = true;
  progressByte_ = savedByte_ = sourceByte;
  progressSize_ = savedSize_ = sourceSize_;
  progressHash_ = savedHash_ = sourceHash_;
  legacyPageValid_ = false;
  status_ = TextStatus::Ok;
  return true;
}

#endif
