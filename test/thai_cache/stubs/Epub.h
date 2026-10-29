#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class CssParser;

// Host-only publication metadata. The fixture supplies already-extracted HTML;
// all section persistence, parsing, layout, and page deserialization are production code.
class Epub {
 public:
  static inline bool enableGifProbeForTest = false;
  std::vector<uint8_t> imageBytes;
  mutable size_t headerProbes = 0;
  mutable size_t fullExtractions = 0;
  struct SpineEntry {
    std::string href;
  };
  struct TocEntry {
    int spineIndex;
    std::string anchor;
  };
  explicit Epub(std::string cachePath) : cachePath_(std::move(cachePath)) {}
  const std::string& getCachePath() const { return cachePath_; }
  SpineEntry getSpineItem(int) const { return {"chapter.xhtml"}; }
  int getTocIndexForSpineIndex(int) const { return -1; }
  int getTocItemsCount() const { return 0; }
  TocEntry getTocItem(int) const { return {-1, {}}; }
  CssParser* getCssParser() const { return nullptr; }
  const std::string& getLanguage() const {
    static const std::string language = "th";
    return language;
  }
  template <typename Stream>
  bool readItemContentsToStream(const std::string&, Stream& output, size_t, bool allowEarlyStop = false) const {
    if (imageBytes.empty()) return false;
    if (allowEarlyStop)
      ++headerProbes;
    else
      ++fullExtractions;
    const size_t written = output.write(imageBytes.data(), imageBytes.size());
    return written == imageBytes.size() || allowEarlyStop;
  }

 private:
  std::string cachePath_;
};
