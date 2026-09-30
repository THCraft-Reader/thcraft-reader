#pragma once

#include <string>
#include <utility>

class CssParser;

// Host-only publication metadata. The fixture supplies already-extracted HTML;
// all section persistence, parsing, layout, and page deserialization are production code.
class Epub {
 public:
  struct SpineEntry { std::string href; };
  struct TocEntry { int spineIndex; std::string anchor; };
  explicit Epub(std::string cachePath) : cachePath_(std::move(cachePath)) {}
  const std::string& getCachePath() const { return cachePath_; }
  SpineEntry getSpineItem(int) const { return {"chapter.xhtml"}; }
  int getTocIndexForSpineIndex(int) const { return -1; }
  int getTocItemsCount() const { return 0; }
  TocEntry getTocItem(int) const { return {-1, {}}; }
  CssParser* getCssParser() const { return nullptr; }
  const std::string& getLanguage() const { static const std::string language = "th"; return language; }
  template <typename Stream>
  bool readItemContentsToStream(const std::string&, Stream&, size_t, bool = false) const { return false; }
 private:
  std::string cachePath_;
};
