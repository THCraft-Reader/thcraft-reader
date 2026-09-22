#pragma once

#include <ft2build.h>

#include "NativeTextTypes.h"
#include FT_FREETYPE_H
#include <hb.h>

struct NativeFaceChoice {
  uint64_t identity = 0;
  uint32_t rasterSize26 = 0;
  uint8_t syntheticStyle = 0;
};
struct NativeFaceHandle {
  FT_Face face = nullptr;
  hb_font_t* font = nullptr;
  uint8_t slot = 255;
};

// All calls, including destruction, require the native-engine mutex. Handles
// pin a record until release; neither FreeType nor HarfBuzz objects escape it.
class NativeFontRegistry {
 public:
  NativeFontRegistry() = default;
  ~NativeFontRegistry();
  NativeFontRegistry(const NativeFontRegistry&) = delete;
  NativeFontRegistry& operator=(const NativeFontRegistry&) = delete;

  TextStatus initialize(FT_Library library);
  TextStatus registerBuiltins();
  TextStatus registerCustomFont(int fontId, uint16_t pointSize, std::span<const NativeFontFile> files, bool serif);
  // Aliases reuse validated, point-independent sources without reading/hashing files again.
  TextStatus registerFontAlias(int fontId, int sourceFontId, uint16_t pointSize);
  TextStatus validateFontFile(std::string_view path, std::span<const NativeVariation> axes = {});
  TextStatus setUiFallback(int customFontId);
  void clearCustomFonts();
  TextStatus candidates(int fontId, uint8_t style, uint32_t script, std::span<NativeFaceChoice> output, size_t& count);
  TextStatus acquire(const NativeFaceChoice& choice, NativeFaceHandle& handle);
  TextStatus acquireGlyph(const NativeGlyph& glyph, NativeFaceHandle& handle);
  TextStatus faceStatus(const NativeFaceHandle& handle) const;
  void release(const NativeFaceHandle& handle);
  void releaseSdFaces();
  void evictUnusedFaces();
  void shutdown();
  uint64_t fingerprint(int fontId) const;
  bool hasFont(int fontId) const;

 private:
  struct State;
  State* state_ = nullptr;
};
