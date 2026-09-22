#pragma once

#include "NativeTextTypes.h"

// One shared instance owns the native font/shaping state. Every method locks the
// platform's recursive mutex; a draw transaction can retain it across bitmap use.
class NativeTextEngine {
 public:
  NativeTextEngine() = default;
  ~NativeTextEngine();
  NativeTextEngine(const NativeTextEngine&) = delete;
  NativeTextEngine& operator=(const NativeTextEngine&) = delete;

  TextStatus initialize();
  void shutdown();
  bool ready() const;
  TextStatus shapeLine(const NativeLineInput& input, NativeGlyphRun& output);
  TextStatus fontMetrics(int fontId, uint8_t style, int32_t& lineHeight26, int32_t& ascender26, int32_t& descender26);
  TextStatus rasterize(const NativeGlyph& glyph, NativeBitmapView& bitmap);
  uint64_t layoutFingerprint(int fontId) const;
  TextStatus registerCustomFont(int fontId, uint16_t pointSize, std::span<const NativeFontFile> files, bool serif);
  TextStatus registerFontAlias(int fontId, int sourceFontId, uint16_t pointSize);
  // Content validation also accepts staged .part paths; naming is the installer's responsibility.
  TextStatus validateFontFile(std::string_view path, std::span<const NativeVariation> axes = {});
  TextStatus setUiFallback(int customFontId);
  void clearCustomFonts();
  TextStatus findThaiBreaks(std::string_view text, std::span<uint32_t> offsets, size_t& count, bool emergency = false);
  void releaseSdFaces();
  void clearCaches();
  TextStatus lastStatus() const;
  void lock();
  void unlock();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  TextStatus status_ = TextStatus::Ok;
  TextStatus report(TextStatus status);
};
