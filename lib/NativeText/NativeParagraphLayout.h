#pragma once

#include "NativeTextTypes.h"

class NativeTextEngine;

enum class NativeAlignment : uint8_t { Start, Left, Center, Right, Justify };
enum class NativeSourceUnit : uint8_t { Codepoint, Byte };
struct NativeSourceAnchor {
  uint32_t byteOffset = 0, sourceOffset = 0;
};
struct NativeLinkRange {
  uint32_t startByte = 0, endByte = 0, id = 0;
};
struct NativeRubyInput {
  uint32_t baseStartByte = 0, baseEndByte = 0;
  std::string_view text;
  uint8_t style = 0;
};
struct NativeParagraphView {
  std::string_view text;
  std::span<const NativeStyleSpan> spans;
  std::span<const NativeSourceAnchor> sourceAnchors;
  std::span<const NativeLinkRange> links;
  std::span<const NativeRubyInput> ruby;
  uint32_t sourceStart = 0;
  NativeSourceUnit sourceUnit = NativeSourceUnit::Codepoint;
  int8_t paragraphLevel = -1;
  bool resolvedLevels = false;
  bool final = true;
  bool semanticBoundary = false;  // Explicit ruby boundary, never a parser arrival chunk.
};
struct NativeLayoutOptions {
  int fontId = 0;
  uint16_t width = 0;
  int16_t firstLineIndent = 0;
  NativeAlignment alignment = NativeAlignment::Start;
  uint16_t maxLines = 0;  // Zero means no caller-imposed line limit.
  bool hyphenation = false;
  bool focus = false;
  bool readerFeatures = true;  // False fits UI strings only; persisted NativeLineData uses reader pnum.
  bool firstLine = true;
  bool emergencyHyphenation = false;  // EPUB oversized-word policy; never enabled for UI/TXT.
};
struct NativeLinkBox {
  uint32_t id = 0;
  int32_t x26 = 0, width26 = 0;
  int16_t top = 0, height = 0;
};
struct NativeLayoutEmission {
  NativeLineData line;
  NativeBuffer<NativeLinkBox> links;
  uint32_t sourceStart = 0, sourceEnd = 0;
  uint32_t startByte = 0, endByte = 0;
};
using NativeLineCallback = TextStatus (*)(void* context, NativeLayoutEmission&& line);

// A bounded paragraph-window fitter shared by UI, EPUB and TXT adapters.
// Non-final windows retain the final 512 scalars and the uncommitted last line.
class NativeParagraphLayout {
  NativeTextEngine& engine_;
  struct State;
  State* state_ = nullptr;

 public:
  explicit NativeParagraphLayout(NativeTextEngine& engine) : engine_(engine) {}
  ~NativeParagraphLayout();
  NativeParagraphLayout(const NativeParagraphLayout&) = delete;
  NativeParagraphLayout& operator=(const NativeParagraphLayout&) = delete;
  // Returns a complete UTF-8 prefix within 4096 scalars / 16 KiB. Incomplete
  // trailing input is retained unless finalInput, when it is InvalidText.
  static TextStatus windowPrefix(std::string_view text, size_t& bytes, bool finalInput);
  TextStatus layout(const NativeParagraphView& paragraph, const NativeLayoutOptions& options, NativeLineCallback emit,
                    void* context, size_t& consumedBytes, int8_t& paragraphLevel);
};
