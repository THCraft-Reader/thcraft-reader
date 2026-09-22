#include "GfxRenderer.h"

#if defined(CROSSPOINT_NATIVE_TEXT)
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <NativeParagraphLayout.h>
#include <NativeTextEngine.h>
#include <fontIds.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
class EngineLock {
  NativeTextEngine& engine_;

 public:
  explicit EngineLock(NativeTextEngine& engine) : engine_(engine) { engine_.lock(); }
  ~EngineLock() { engine_.unlock(); }
};
int pixel(int32_t value) { return static_cast<int>((static_cast<int64_t>(value) + 32) >> 6); }
int floorPixel(int32_t value) { return value >> 6; }
int ceilPixel(int32_t value) { return static_cast<int>((static_cast<int64_t>(value) + 63) >> 6); }
int inkWidth(const NativeGlyphRun& run) { return std::max(0, ceilPixel(run.ink.right26) - floorPixel(run.ink.left26)); }
NativeLineInput uiInput(int fontId, std::string_view text, uint8_t style, int8_t level, NativeStyleSpan& span) {
  span = {0, static_cast<uint32_t>(text.size()), style, 0};
  NativeLineInput input;
  input.text = text;
  input.fontId = fontId;
  input.spans = {&span, 1};
  input.paragraphLevel = level;
  input.readerFeatures = false;
  return input;
}
size_t encodeScalar(uint32_t cp, char* output) {
  if (cp == 0) return 0;
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
  if (cp < 0x80) {
    output[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    output[0] = static_cast<char>(0xc0 | (cp >> 6));
    output[1] = static_cast<char>(0x80 | (cp & 63));
    return 2;
  }
  if (cp < 0x10000) {
    output[0] = static_cast<char>(0xe0 | (cp >> 12));
    output[1] = static_cast<char>(0x80 | ((cp >> 6) & 63));
    output[2] = static_cast<char>(0x80 | (cp & 63));
    return 3;
  }
  output[0] = static_cast<char>(0xf0 | (cp >> 18));
  output[1] = static_cast<char>(0x80 | ((cp >> 12) & 63));
  output[2] = static_cast<char>(0x80 | ((cp >> 6) & 63));
  output[3] = static_cast<char>(0x80 | (cp & 63));
  return 4;
}
struct RasterPlacement {
  int x, y, width, height;
  size_t offset;
  bool solid;
};
}  // namespace

struct GfxRenderer::NativeTextState {
  NativeGlyphRun run;
  NativeBuffer<RasterPlacement> placements;
  NativeBuffer<uint8_t> coverage;
  NativeBuffer<uint32_t> boundaries;
  NativeBuffer<char> text;
  void clearPaint() {
    placements.clear();
    coverage.clear();
  }
};

GfxRenderer::NativeTextState* GfxRenderer::nativeState() const {
  if (!nativeTextState_) {
    void* memory = native_text_malloc(sizeof(NativeTextState));
    if (!memory) {
      nativeTextEngine_->clearCaches();
      memory = native_text_malloc(sizeof(NativeTextState));
    }
    if (!memory) {
      reportNativeStatus(TextStatus::OutOfMemory);
      return nullptr;
    }
    nativeTextState_ = new (memory) NativeTextState;
  }
  return nativeTextState_;
}
void GfxRenderer::releaseNativeState() {
  if (!nativeTextState_) return;
  nativeTextState_->~NativeTextState();
  native_text_free(nativeTextState_);
  nativeTextState_ = nullptr;
}
void GfxRenderer::setNativeTextEngine(NativeTextEngine* engine) {
  if (nativeTextEngine_ == engine) return;
  if (nativeTextEngine_) {
    EngineLock lock(*nativeTextEngine_);
    releaseNativeState();
  }
  nativeTextEngine_ = engine;
  clearTextStatus();
}
uint64_t GfxRenderer::textLayoutFingerprint(int fontId) const {
  return nativeTextEngine_ ? nativeTextEngine_->layoutFingerprint(fontId) : 0;
}
void GfxRenderer::clearTextStatus() const {
  textStatus_ = TextStatus::Ok;
  nativeErrorShown_ = false;
}
bool GfxRenderer::reportNativeStatus(TextStatus status) const {
  if (status == TextStatus::Ok) return true;
  if (textStatus_ == TextStatus::Ok) textStatus_ = status;
  LOG_ERR("TEXT", "Native renderer failed (%u)", static_cast<unsigned>(status));
  return false;
}

int GfxRenderer::nativeMeasure(int fontId, const char* text, EpdFontFamily::Style style, int8_t level,
                               bool advance) const {
  if (!text || !*text) return 0;
  EngineLock lock(*nativeTextEngine_);
  auto* state = nativeState();
  if (!state) return 0;
  NativeStyleSpan span;
  const auto input = uiInput(fontId, text, style, level, span);
  if (!reportNativeStatus(nativeTextEngine_->shapeLine(input, state->run))) return 0;
  return advance ? pixel(state->run.advance26) : inkWidth(state->run);
}
int GfxRenderer::nativeMetric(int fontId, bool ascender) const {
  EngineLock lock(*nativeTextEngine_);
  int32_t height26, ascender26, descender26;
  if (!reportNativeStatus(nativeTextEngine_->fontMetrics(fontId, 0, height26, ascender26, descender26))) return 0;
  return ceilPixel(ascender ? ascender26 : height26);
}
int GfxRenderer::nativePairAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style,
                                   bool space) const {
  EngineLock lock(*nativeTextEngine_);
  auto* state = nativeState();
  if (!state) return 0;
  char text[9];
  const size_t leftBytes = encodeScalar(leftCp, text);
  size_t rightOffset = leftBytes;
  if (space) text[rightOffset++] = ' ';
  const size_t rightBytes = encodeScalar(rightCp, text + rightOffset);
  if ((leftCp && !leftBytes) || (rightCp && !rightBytes)) {
    reportNativeStatus(TextStatus::InvalidText);
    return 0;
  }
  NativeStyleSpan span;
  auto input = uiInput(fontId, {text, rightOffset + rightBytes}, style, -1, span);
  if (!reportNativeStatus(nativeTextEngine_->shapeLine(input, state->run))) return 0;
  int32_t result = state->run.advance26;
  for (const auto single : {std::string_view(text, leftBytes), std::string_view(text + rightOffset, rightBytes)}) {
    if (single.empty()) continue;
    input = uiInput(fontId, single, style, -1, span);
    if (!reportNativeStatus(nativeTextEngine_->shapeLine(input, state->run))) return 0;
    result -= state->run.advance26;
  }
  return pixel(result);
}

TextStatus GfxRenderer::stageNativeRun(const NativeGlyphRun& run, int32_t x26, int32_t y26, bool rotated,
                                       bool warm) const {
  auto& state = *nativeTextState_;
  const int clipRight = std::min(getScreenWidth(), clipRight_);
  const int clipBottom = std::min(getScreenHeight(), clipBottom_);
  if (!warm && !state.placements.reserve(state.placements.size() + run.glyphs.size() + 2 * run.clusters.size()))
    return TextStatus::OutOfMemory;
  const auto visible = [&](int x, int y, int width, int height) {
    const int x0 = x, y0 = rotated ? y - width + 1 : y;
    const int x1 = x + (rotated ? height : width) - 1;
    const int y1 = rotated ? y : y + height - 1;
    return x1 >= std::max(0, clipLeft_) && x0 < clipRight && y1 >= std::max(0, clipTop_) && y0 < clipBottom &&
           glyphIntersectsStrip(x0, y0, x1, y1);
  };
  if (!warm) {
    // Cull the complete run before looking up any glyph bitmap for a remote strip.
    // Expand for rounded bearings and the one-pixel underline below actual ink.
    const int left = floorPixel(std::min(0, run.ink.left26)) - 1;
    const int top = floorPixel(std::min(-64, run.ink.top26)) - 1;
    const int width = ceilPixel(std::max(run.advance26, run.ink.right26)) - left + 2;
    const int height = ceilPixel(std::max(128, run.ink.bottom26 + 128)) - top + 2;
    if (!visible(pixel(x26) + (rotated ? top : left), pixel(y26) + (rotated ? -left : top), width, height))
      return TextStatus::Ok;
  }
  for (const auto& glyph : run.glyphs.span()) {
    NativeBitmapView bitmap;
    const auto status = nativeTextEngine_->rasterize(glyph, bitmap);
    if (status != TextStatus::Ok) return status;
    if (warm || bitmap.width == 0 || bitmap.height == 0) continue;
    const int gx = rotated ? pixel(x26 + glyph.y26) - bitmap.top : pixel(x26 + glyph.x26) + bitmap.left;
    const int gy = rotated ? pixel(y26 - glyph.x26) - bitmap.left : pixel(y26 + glyph.y26) - bitmap.top;
    if (!visible(gx, gy, bitmap.width, bitmap.height)) continue;
    if (!bitmap.data || bitmap.pitch == std::numeric_limits<int>::min() || std::abs(bitmap.pitch) < bitmap.width ||
        bitmap.width < 0 || bitmap.height < 0)
      return TextStatus::InvalidFont;
    const size_t bytes = static_cast<size_t>(bitmap.width) * bitmap.height;
    const size_t offset = state.coverage.size();
    if (bytes > native_text::MEMORY_LIMIT || offset > native_text::MEMORY_LIMIT - bytes)
      return TextStatus::CapacityExceeded;
    const size_t needed = offset + bytes;
    if (needed > state.coverage.capacity() &&
        !state.coverage.reserve(std::max(
            needed, std::min(native_text::MEMORY_LIMIT, std::max(size_t(4096), state.coverage.capacity() * 2)))))
      return TextStatus::OutOfMemory;
    if (!state.coverage.resize(needed)) return TextStatus::OutOfMemory;
    for (int row = 0; row < bitmap.height; ++row) {
      const int sourceRow = bitmap.pitch < 0 ? bitmap.height - 1 - row : row;
      std::memcpy(state.coverage.data() + offset + static_cast<size_t>(row) * bitmap.width,
                  bitmap.data + static_cast<size_t>(sourceRow) * std::abs(bitmap.pitch), bitmap.width);
    }
    const size_t index = state.placements.size();
    if (!state.placements.resize(index + 1)) return TextStatus::OutOfMemory;
    state.placements[index] = {gx, gy, bitmap.width, bitmap.height, offset, false};
  }
  if (warm) return TextStatus::Ok;
  // Decorations follow final visual clusters, including expanded real spaces.
  for (const auto& cluster : run.clusters.span()) {
    const int width = std::max(0, ceilPixel(cluster.x26 + cluster.advance26) - floorPixel(cluster.x26));
    if (!width) continue;
    for (const uint8_t flag : {uint8_t(EpdFontFamily::UNDERLINE), uint8_t(EpdFontFamily::STRIKETHROUGH)}) {
      if (!(cluster.style & flag)) continue;
      const int32_t offset26 =
          flag == EpdFontFamily::UNDERLINE ? cluster.bottom26 + 64 : (cluster.top26 + cluster.bottom26) / 2;
      const int gx = rotated ? pixel(x26 + offset26) : pixel(x26 + cluster.x26);
      const int gy = rotated ? pixel(y26 - cluster.x26) : pixel(y26 + offset26);
      if (!visible(gx, gy, width, 1)) continue;
      const size_t index = state.placements.size();
      if (!state.placements.resize(index + 1)) return TextStatus::OutOfMemory;
      state.placements[index] = {gx, gy, width, 1, 0, true};
    }
  }
  return TextStatus::Ok;
}

void GfxRenderer::paintNativeRun(bool black, bool rotated) const {
  if (!frameBuffer) return;  // Never modify loaned storage, including an error surface.
  const auto& state = *nativeTextState_;
  const RenderMode mode = absoluteGrayPlanes ? BW : renderMode;
  const int screenWidth = getScreenWidth(), screenHeight = getScreenHeight();
  for (const auto& placement : state.placements.span()) {
    for (int row = 0; row < placement.height; ++row) {
      for (int col = 0; col < placement.width; ++col) {
        const uint8_t bucket =
            placement.solid ? 3
                            : state.coverage[placement.offset + static_cast<size_t>(row) * placement.width + col] >> 6;
        if (!bucket) continue;
        const int x = placement.x + (rotated ? row : col);
        const int y = placement.y + (rotated ? -col : row);
        if (x < 0 || y < 0 || x >= screenWidth || y >= screenHeight) continue;
        if (mode == BW) {
          drawPixel(x, y, black);
        } else if (mode == GRAYSCALE_MSB && bucket < 3) {
          drawPixel(x, y, false);
        } else if (mode == GRAYSCALE_LSB && bucket == (black ? 2 : 1)) {
          drawPixel(x, y, false);
        }
      }
    }
  }
}

TextStatus GfxRenderer::stageNativeLine(int fontId, const NativeLineData& line, int x, int y, bool warm) const {
  auto& state = *nativeTextState_;
  state.clearPaint();
  auto status = nativeTextEngine_->shapeLine(line.input(fontId), state.run);
  if (status != TextStatus::Ok) return status;
  status = stageNativeRun(state.run, x * 64 + line.alignmentX26, (y + line.baseline) * 64, false, warm);
  if (status != TextStatus::Ok) return status;
  for (const auto& ruby : line.ruby.span()) {
    if (ruby.textOffset > line.rubyText.size() || ruby.textBytes > line.rubyText.size() - ruby.textOffset)
      return TextStatus::InvalidText;
    const uint8_t style = (ruby.style & ~(EpdFontFamily::SUP | EpdFontFamily::SUB)) | EpdFontFamily::SUP;
    NativeStyleSpan span;
    auto input = uiInput(fontId, {line.rubyText.data() + ruby.textOffset, ruby.textBytes}, style, -1, span);
    input.readerFeatures = true;
    status = nativeTextEngine_->shapeLine(input, state.run);
    if (status != TextStatus::Ok) return status;
    status = stageNativeRun(state.run, x * 64 + ruby.x26, y * 64 + ruby.y26, false, warm);
    if (status != TextStatus::Ok) return status;
  }
  return TextStatus::Ok;
}
bool GfxRenderer::drawNativeLine(int fontId, const NativeLineData& line, int x, int y, bool black) const {
  if (!nativeTextEngine_) return reportNativeStatus(TextStatus::InvalidFont);
  EngineLock lock(*nativeTextEngine_);
  const int savedLeft = clipLeft_, savedRight = clipRight_;
  ScopedCleanup restoreClip{[&] {
    clipLeft_ = savedLeft;
    clipRight_ = savedRight;
  }};
  if (line.overflowClipWidth) {
    clipLeft_ = std::max(clipLeft_, x);
    clipRight_ = std::min(clipRight_, x + line.overflowClipWidth);
  }
  if (!nativeState()) {
    showNativeError(x, y, black);
    return false;
  }
  auto status = stageNativeLine(fontId, line, x, y, false);
  if (status == TextStatus::OutOfMemory) {
    nativeTextEngine_->clearCaches();
    status = stageNativeLine(fontId, line, x, y, false);
  }
  if (!reportNativeStatus(status)) {
    nativeTextState_->clearPaint();
    showNativeError(x, y, black);
    return false;
  }
  paintNativeRun(black, false);
  return true;
}
bool GfxRenderer::warmNativeLine(int fontId, const NativeLineData& line) const {
  if (!nativeTextEngine_) return reportNativeStatus(TextStatus::InvalidFont);
  EngineLock lock(*nativeTextEngine_);
  if (!nativeState()) return false;
  return reportNativeStatus(stageNativeLine(fontId, line, 0, 0, true));
}
bool GfxRenderer::warmNativeText(int fontId, const char* text, EpdFontFamily::Style style) const {
  if (!text || !*text) return true;
  EngineLock lock(*nativeTextEngine_);
  auto* state = nativeState();
  if (!state) return false;
  NativeStyleSpan span;
  auto input = uiInput(fontId, text, style, -1, span);
  if (!reportNativeStatus(nativeTextEngine_->shapeLine(input, state->run))) return false;
  return reportNativeStatus(stageNativeRun(state->run, 0, 0, false, true));
}
void GfxRenderer::drawNativeText(int fontId, int x, int y, const char* text, bool black, EpdFontFamily::Style style,
                                 int8_t level, bool rotated, bool centered) const {
  if (!text || !*text) return;
  EngineLock lock(*nativeTextEngine_);
  auto* state = nativeState();
  if (!state) {
    showNativeError(x, y, black);
    return;
  }
  NativeStyleSpan span;
  auto input = uiInput(fontId, text, style, level, span);
  TextStatus status = TextStatus::Ok;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    state->clearPaint();
    status = nativeTextEngine_->shapeLine(input, state->run);
    if (status == TextStatus::Ok) {
      if (centered) x = (getScreenWidth() - inkWidth(state->run)) / 2 - floorPixel(state->run.ink.left26);
      const int baseline = ceilPixel(std::max(state->run.ascender26, -state->run.ink.top26));
      status = stageNativeRun(state->run, (x + (rotated ? baseline : 0)) * 64, (y + (rotated ? 0 : baseline)) * 64,
                              rotated, false);
    }
    if (status != TextStatus::OutOfMemory || attempt) break;
    nativeTextEngine_->clearCaches();
  }
  if (!reportNativeStatus(status)) {
    state->clearPaint();
    showNativeError(x, y, black);
    return;
  }
  paintNativeRun(black, rotated);
}
void GfxRenderer::showNativeError(int x, int y, bool black) const {
  if (nativeErrorShown_ || !frameBuffer || !nativeTextEngine_) return;
  nativeErrorShown_ = true;
  nativeTextEngine_->clearCaches();
  auto* state = nativeState();
  if (!state) return;
  state->clearPaint();
  NativeStyleSpan span;
  const char* message = textStatus_ == TextStatus::OutOfMemory ? tr(STR_MEMORY_ERROR) : tr(STR_TEXT_RENDER_ERROR);
  auto input = uiInput(SMALL_FONT_ID, message, 0, -1, span);
  if (nativeTextEngine_->shapeLine(input, state->run) != TextStatus::Ok) return;
  const int baseline = ceilPixel(std::max(state->run.ascender26, -state->run.ink.top26));
  if (stageNativeRun(state->run, x * 64, (y + baseline) * 64, false, false) == TextStatus::Ok)
    paintNativeRun(black, false);
  state->clearPaint();
}

std::string GfxRenderer::truncateNativeText(int fontId, std::string_view text, int maxWidth, EpdFontFamily::Style style,
                                            bool forceEllipsis) const {
  if (maxWidth <= 0) return {};
  EngineLock lock(*nativeTextEngine_);
  auto* state = nativeState();
  if (!state) return {};
  const std::string_view source = text;
  NativeStyleSpan span;
  auto input = uiInput(fontId, source, style, -1, span);
  if (!reportNativeStatus(nativeTextEngine_->shapeLine(input, state->run))) return {};
  if (!forceEllipsis && inkWidth(state->run) <= maxWidth) return std::string(source);
  const int8_t level = state->run.paragraphLevel;
  state->boundaries.clear();
  if (!state->boundaries.reserve(state->run.clusters.size() + 2)) {
    reportNativeStatus(TextStatus::OutOfMemory);
    return {};
  }
  state->boundaries.resize(1);
  state->boundaries[0] = 0;
  for (const auto& cluster : state->run.clusters.span()) {
    if (cluster.startByte && !cluster.unsafeToBreak) {
      const size_t index = state->boundaries.size();
      state->boundaries.resize(index + 1);
      state->boundaries[index] = cluster.startByte;
    }
  }
  if (forceEllipsis) {
    const size_t index = state->boundaries.size();
    state->boundaries.resize(index + 1);
    state->boundaries[index] = static_cast<uint32_t>(source.size());
  }
  std::sort(state->boundaries.data(), state->boundaries.data() + state->boundaries.size());
  constexpr std::string_view ellipsis("\xe2\x80\xa6", 3);
  input = uiInput(fontId, ellipsis, style, level, span);
  if (!reportNativeStatus(nativeTextEngine_->shapeLine(input, state->run)) || inkWidth(state->run) > maxWidth)
    return {};
  if (!state->text.resize(source.size() + ellipsis.size() + 1)) {
    reportNativeStatus(TextStatus::OutOfMemory);
    return {};
  }
  std::memcpy(state->text.data(), source.data(), source.size());
  for (size_t i = state->boundaries.size(); i > 0; --i) {
    const size_t bytes = state->boundaries[i - 1];
    std::memcpy(state->text.data() + bytes, ellipsis.data(), ellipsis.size());
    input = uiInput(fontId, {state->text.data(), bytes + ellipsis.size()}, style, level, span);
    if (!reportNativeStatus(nativeTextEngine_->shapeLine(input, state->run))) return {};
    if (inkWidth(state->run) <= maxWidth) return std::string(input.text);
  }
  return {};
}

std::vector<std::string> GfxRenderer::wrapNativeText(int fontId, const char* text, int maxWidth, int maxLines,
                                                     EpdFontFamily::Style style) const {
  std::vector<std::string> lines;
  if (!text || !*text || maxWidth <= 0 || maxLines <= 0) return lines;
  EngineLock lock(*nativeTextEngine_);
  NativeParagraphLayout fitter(*nativeTextEngine_);
  const std::string_view source(text);
  size_t offset = 0;
  int8_t level = -1;
  while (offset < source.size() && lines.size() < static_cast<size_t>(maxLines)) {
    const auto remaining = source.substr(offset);
    size_t windowBytes = 0;
    auto status = NativeParagraphLayout::windowPrefix(remaining, windowBytes, true);
    if (!reportNativeStatus(status)) return {};
    NativeStyleSpan span{0, static_cast<uint32_t>(windowBytes), static_cast<uint8_t>(style), 0};
    NativeParagraphView paragraph;
    paragraph.text = remaining.substr(0, windowBytes);
    paragraph.spans = {&span, 1};
    paragraph.paragraphLevel = level;
    paragraph.final = windowBytes == remaining.size();
    NativeLayoutOptions options;
    options.fontId = fontId;
    options.width = static_cast<uint16_t>(std::min(maxWidth, 65535));
    options.maxLines = static_cast<uint16_t>(std::min<size_t>(maxLines - lines.size(), 65535));
    options.readerFeatures = false;
    struct Output {
      std::vector<std::string>& lines;
      size_t lastStart = 0;
    } output{lines};
    const auto emit = [](void* context, NativeLayoutEmission&& emission) -> TextStatus {
      auto& output = *static_cast<Output*>(context);
      output.lastStart = emission.startByte;
      output.lines.emplace_back(emission.line.logicalText());
      return TextStatus::Ok;
    };
    size_t consumed = 0;
    status = fitter.layout(paragraph, options, emit, &output, consumed, level);
    if (!reportNativeStatus(status)) return {};
    if (!consumed) {
      reportNativeStatus(TextStatus::CapacityExceeded);
      return {};
    }
    if (lines.size() == static_cast<size_t>(maxLines) && offset + consumed < source.size()) {
      // Refit the final logical line plus ellipsis, never a byte/codepoint width loop.
      // A bounded prefix suffices: retained source after it cannot enter this line.
      const auto last = paragraph.text.substr(output.lastStart);
      lines.back() = truncateNativeText(fontId, last, maxWidth, style, true);
      if (textStatus_ != TextStatus::Ok) return {};
      return lines;
    }
    offset += consumed;
  }
  return lines;
}
#endif
