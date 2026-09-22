#include "NativeTxtPaginator.h"

#ifdef CROSSPOINT_NATIVE_TEXT
#include <CrossPointSettings.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <NativeParagraphLayout.h>
#include <NativePlatform.h>
#include <NativeTextEngine.h>
#include <NativeUtf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <new>

#include "Txt.h"

namespace {
constexpr size_t INPUT_BYTES = 8192;
constexpr size_t WINDOW_BYTES = 16384;
constexpr size_t WINDOW_SCALARS = 4096;
constexpr uint64_t HASH_SEED = 14695981039346656037ULL;
constexpr uint64_t HASH_PRIME = 1099511628211ULL;

void hashBytes(uint64_t& hash, const uint8_t* bytes, size_t count) {
  for (size_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * HASH_PRIME;
}
void* allocate(size_t bytes, NativeTextEngine& engine) {
  void* memory = native_text_malloc(bytes);
  if (!memory) {
    engine.clearCaches();
    memory = native_text_malloc(bytes);
  }
  return memory;
}
}  // namespace

struct NativeTxtPaginator::State {
  struct PendingLine {
    std::unique_ptr<TextBlock> block;
    PendingLine* next = nullptr;
    explicit PendingLine(std::unique_ptr<TextBlock> value) : block(std::move(value)) {}
  };

  NativeTxtPaginator& owner;
  GfxRenderer& renderer;
  NativeTextEngine& engine;
  NativeParagraphLayout layout;
  NativeLayoutOptions options;
  HalFile source;
  NativeBuffer<char> window;
  PendingLine* head = nullptr;
  PendingLine* tail = nullptr;
  size_t readCursor = 0, readLength = 0, scalars = 0;
  uint32_t bytesRead = 0, sourceCursor = 0, windowSource = 0, pageSource = 0;
  uint16_t height = 0;
  int8_t paragraphLevel = -1;
  bool eof = false, started = false, ended = false, terminated = false, finished = false;

  State(NativeTxtPaginator& owner, GfxRenderer& renderer, uint16_t width, uint16_t height, uint8_t alignment)
      : owner(owner), renderer(renderer), engine(*renderer.nativeTextEngine()), layout(engine), height(height) {
    options.fontId = SETTINGS.getReaderFontId();
    options.width = width;
    if (alignment == CrossPointSettings::CENTER_ALIGN)
      options.alignment = NativeAlignment::Center;
    else if (alignment == CrossPointSettings::RIGHT_ALIGN)
      options.alignment = NativeAlignment::Right;
  }
  ~State() {
    while (head) pop();
    source.close();
  }
  void pop() {
    auto* previous = head;
    head = head->next;
    if (!head) tail = nullptr;
    previous->~PendingLine();
    native_text_free(previous);
  }

  // A positive short read is not EOF. Preserve unread UTF-8 bytes while topping
  // up the same buffer, and distinguish an early zero/error from expected EOF.
  TextStatus ensure(size_t wanted) {
    if (readLength - readCursor >= wanted || eof) return TextStatus::Ok;
    const size_t remaining = readLength - readCursor;
    if (remaining) std::memmove(owner.input_.data(), owner.input_.data() + readCursor, remaining);
    readCursor = 0;
    readLength = remaining;
    while (readLength < wanted && !eof) {
      const int got = source.read(owner.input_.data() + readLength, INPUT_BYTES - readLength);
      if (got < 0) return TextStatus::StorageError;
      if (!got) {
        if (bytesRead != owner.sourceSize_ || source.fileSize64() != owner.sourceSize_) return TextStatus::StorageError;
        eof = true;
        break;
      }
      if (static_cast<size_t>(got) > INPUT_BYTES - readLength ||
          static_cast<uint32_t>(got) > owner.sourceSize_ - bytesRead)
        return TextStatus::StorageError;
      hashBytes(owner.sourceHash_, owner.input_.data() + readLength, static_cast<size_t>(got));
      bytesRead += static_cast<uint32_t>(got);
      readLength += static_cast<size_t>(got);
      nativeTextYield();
    }
    return TextStatus::Ok;
  }
  void consume(size_t bytes) {
    readCursor += bytes;
    sourceCursor += static_cast<uint32_t>(bytes);
  }
  TextStatus start() {
    if (started) return TextStatus::Ok;
    auto status = ensure(3);
    if (status != TextStatus::Ok) return status;
    if (readLength >= 3 && owner.input_[0] == 0xef && owner.input_[1] == 0xbb && owner.input_[2] == 0xbf) consume(3);
    windowSource = sourceCursor;
    owner.emptySource_ = sourceCursor == owner.sourceSize_;
    started = true;
    return TextStatus::Ok;
  }

  TextStatus fillWindow() {
    auto status = start();
    if (status != TextStatus::Ok) return status;
    while (!ended) {
      status = ensure(1);
      if (status != TextStatus::Ok) return status;
      if (readCursor == readLength) {
        ended = true;
        break;
      }
      const uint8_t lead = owner.input_[readCursor];
      if (lead == '\n' || lead == '\r') {
        consume(1);
        if (lead == '\r') {
          status = ensure(1);
          if (status != TextStatus::Ok) return status;
          if (readCursor < readLength && owner.input_[readCursor] == '\n') consume(1);
        }
        ended = terminated = true;
        break;
      }
      // Look for the physical terminator before cutting a full window. Exactly
      // 4096 scalars followed by LF/CRLF/EOF is a final window, not a fake word cut.
      if (scalars == WINDOW_SCALARS) break;
      const size_t length = lead < 0x80                    ? 1
                            : lead >= 0xc2 && lead <= 0xdf ? 2
                            : lead >= 0xe0 && lead <= 0xef ? 3
                            : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                           : 0;
      if (!length) return TextStatus::InvalidText;
      status = ensure(length);
      if (status != TextStatus::Ok) return status;
      if (readLength - readCursor < length) return TextStatus::InvalidText;
      size_t decoded = 0;
      uint32_t cp = 0;
      const auto* bytes = reinterpret_cast<const char*>(owner.input_.data() + readCursor);
      if (!native_text::nextUtf8({bytes, length}, decoded, cp) || !cp) return TextStatus::InvalidText;
      const size_t offset = window.size();
      if (length > WINDOW_BYTES - offset) return TextStatus::CapacityExceeded;
      if (!window.resize(offset + length)) return TextStatus::OutOfMemory;
      std::memcpy(window.data() + offset, bytes, length);
      consume(length);
      ++scalars;
    }
    return TextStatus::Ok;
  }

  TextStatus append(NativeLineData&& line, uint32_t sourceStart, uint32_t sourceEnd) {
    BlockStyle style;
    style.isRtl = (line.paragraphLevel & 1) != 0;
    auto block = makeUniqueNoThrow<TextBlock>(std::move(line), style);
    if (!block) {
      engine.clearCaches();
      block = makeUniqueNoThrow<TextBlock>(std::move(line), style);
    }
    if (!block) return TextStatus::OutOfMemory;
    if (!block->valid()) return TextStatus::InvalidText;
    block->setSourceRange(sourceStart, sourceEnd);
    void* memory = allocate(sizeof(PendingLine), engine);
    if (!memory) return TextStatus::OutOfMemory;
    auto* next = new (memory) PendingLine(std::move(block));
    if (tail)
      tail->next = next;
    else
      head = next;
    tail = next;
    return TextStatus::Ok;
  }
  static TextStatus emit(void* context, NativeLayoutEmission&& emission) {
    auto& self = *static_cast<State*>(context);
    const uint32_t end = self.ended && emission.endByte == self.window.size() ? self.sourceCursor : emission.sourceEnd;
    return self.append(std::move(emission.line), emission.sourceStart, end);
  }
  TextStatus blankLine() {
    int32_t height26 = 0, ascent26 = 0, descent26 = 0;
    const auto status = engine.fontMetrics(options.fontId, 0, height26, ascent26, descent26);
    if (status != TextStatus::Ok) return status;
    const int32_t baseline = std::max<int32_t>(0, (ascent26 + 63) / 64);
    const int32_t lineHeight = std::max(baseline, (height26 + 63) / 64);
    if (lineHeight <= 0 || lineHeight > INT16_MAX) return TextStatus::CapacityExceeded;
    NativeLineData line;
    line.baseline = static_cast<int16_t>(baseline);
    line.lineHeight = static_cast<int16_t>(lineHeight);
    return append(std::move(line), windowSource, sourceCursor);
  }
  void nextParagraph() {
    ended = terminated = false;
    paragraphLevel = -1;
    options.firstLine = true;
    windowSource = sourceCursor;
  }
  TextStatus fitWindow() {
    auto status = fillWindow();
    if (status != TextStatus::Ok) return status;
    if (window.empty()) {
      if (!terminated) {
        finished = true;
        return TextStatus::Ok;
      }
      status = blankLine();
      if (status == TextStatus::Ok) nextParagraph();
      return status;
    }
    NativeParagraphView view;
    view.text = {window.data(), window.size()};
    view.sourceStart = windowSource;
    view.sourceUnit = NativeSourceUnit::Byte;
    view.paragraphLevel = paragraphLevel;
    view.final = ended;
    size_t consumed = 0;
    status = layout.layout(view, options, emit, this, consumed, paragraphLevel);
    if (status != TextStatus::Ok) return status;
    if (!consumed || consumed > window.size() || (ended && consumed != window.size()))
      return TextStatus::CapacityExceeded;
    const size_t remaining = window.size() - consumed;
    if (remaining) std::memmove(window.data(), window.data() + consumed, remaining);
    window.resize(remaining);  // Shrinking cannot allocate.
    windowSource += static_cast<uint32_t>(consumed);
    scalars = 0;
    size_t offset = 0;
    uint32_t cp = 0;
    while (offset < remaining) {
      if (!native_text::nextUtf8({window.data(), remaining}, offset, cp)) return TextStatus::InvalidText;
      ++scalars;
    }
    options.firstLine = false;
    if (ended) nextParagraph();
    return TextStatus::Ok;
  }
};

NativeTxtPaginator::~NativeTxtPaginator() { close(); }

void NativeTxtPaginator::releaseState() {
  if (!state_) return;
  state_->~State();
  native_text_free(state_);
  state_ = nullptr;
}
TextStatus NativeTxtPaginator::fail(TextStatus status) {
  LOG_ERR("TEXT", "Native TXT pagination failed (%u)", static_cast<unsigned>(status));
  status_ = status;
  releaseState();
  input_.reset();
  return status;
}
void NativeTxtPaginator::close() {
  releaseState();
  input_.reset();
  sourceHash_ = HASH_SEED;
  sourceSize_ = 0;
  emptySource_ = true;
  status_ = TextStatus::Ok;
}

bool NativeTxtPaginator::begin(const Txt& txt, GfxRenderer& renderer, uint16_t width, uint16_t height,
                               uint8_t alignment) {
  releaseState();
  status_ = TextStatus::Ok;
  sourceHash_ = HASH_SEED;
  sourceSize_ = 0;
  emptySource_ = true;
  auto* engine = renderer.nativeTextEngine();
  if (!engine || !engine->ready()) return fail(TextStatus::InvalidFont), false;
  if (!width || !height || width > INT16_MAX || height > INT16_MAX) return fail(TextStatus::CapacityExceeded), false;
  if (!input_.resize(INPUT_BYTES)) {
    engine->clearCaches();
    if (!input_.resize(INPUT_BYTES)) return fail(TextStatus::OutOfMemory), false;
  }
  void* memory = allocate(sizeof(State), *engine);
  if (!memory) return fail(TextStatus::OutOfMemory), false;
  state_ = new (memory) State(*this, renderer, width, height, alignment);
  if (!Storage.openFileForRead("TEXT", txt.getPath(), state_->source) || state_->source.isDirectory())
    return fail(TextStatus::StorageError), false;
  const uint64_t size = state_->source.fileSize64();
  if (size > UINT32_MAX) return fail(TextStatus::CapacityExceeded), false;
  sourceSize_ = static_cast<uint32_t>(size);
  emptySource_ = !size;
  if (!state_->window.reserve(WINDOW_BYTES)) {
    engine->clearCaches();
    if (!state_->window.reserve(WINDOW_BYTES)) return fail(TextStatus::OutOfMemory), false;
  }
  return true;
}

NativeTxtPaginator::Result NativeTxtPaginator::nextPage(std::unique_ptr<Page>& page, uint32_t& sourceStart,
                                                        uint32_t& sourceEnd) {
  page.reset();
  sourceStart = sourceEnd = 0;
  if (!state_) {
    if (status_ == TextStatus::Ok) fail(TextStatus::InvalidText);
    return Result::Error;
  }
  auto& state = *state_;
  int y = 0;
  auto abort = [&](TextStatus status) {
    page.reset();
    sourceStart = sourceEnd = 0;
    fail(status);
    return Result::Error;
  };
  while (true) {
    if (!state.head && !state.finished) {
      const auto status = state.fitWindow();
      if (status != TextStatus::Ok) return abort(status);
    }
    if (!state.head) {
      if (!state.finished) return abort(TextStatus::CapacityExceeded);
      if (state.source.isOpen() && !state.source.close()) return abort(TextStatus::StorageError);
      if (!page) return Result::End;
      break;
    }
    const int lineHeight = state.head->block->layoutHeight(state.renderer, state.options.fontId, 1.0f);
    if (state.renderer.lastTextStatus() != TextStatus::Ok) return abort(state.renderer.lastTextStatus());
    if (lineHeight <= 0 || lineHeight > INT16_MAX) return abort(TextStatus::CapacityExceeded);
    if (page && (y + lineHeight > state.height || page->elements.size() == Page::MAX_ELEMENTS_PER_PAGE)) break;
    if (!page) {
      page = makeUniqueNoThrow<Page>();
      if (!page) return abort(TextStatus::OutOfMemory);
      const size_t capacity =
          std::min<size_t>(Page::MAX_ELEMENTS_PER_PAGE, std::max<size_t>(1, state.height / lineHeight + 1));
      if (!page->reserveElements(capacity)) {
        state.engine.clearCaches();
        if (!page->reserveElements(capacity)) return abort(TextStatus::OutOfMemory);
      }
      sourceStart = state.pageSource;
      page->visibleTextOffset = sourceStart;
    }
    const uint32_t end = state.head->block->sourceEndOffset();
    auto element = makeUniqueNoThrow<PageLine>(std::move(state.head->block), 0, static_cast<int16_t>(y));
    if (!element) return abort(TextStatus::OutOfMemory);
    if (page->elements.size() == page->elements.capacity()) {
      const size_t capacity =
          std::min<size_t>(Page::MAX_ELEMENTS_PER_PAGE, std::max<size_t>(1, page->elements.capacity() * 2));
      if (!page->reserveElements(capacity)) {
        state.engine.clearCaches();
        if (!page->reserveElements(capacity)) return abort(TextStatus::OutOfMemory);
      }
    }
    if (!page->addElement(std::move(element))) return abort(TextStatus::OutOfMemory);
    state.pop();
    state.pageSource = sourceEnd = end;
    y += lineHeight;
    // Even a single taller-than-viewport line is published once. The reader's
    // content clip owns raster clipping; never create an empty-page loop here.
    if (y >= state.height) break;
  }
  return Result::PageReady;
}

TextStatus NativeTxtPaginator::hashSource(const Txt& txt, uint64_t& hash) {
  releaseState();
  status_ = TextStatus::Ok;
  sourceHash_ = HASH_SEED;
  sourceSize_ = 0;
  emptySource_ = true;
  hash = 0;
  if (!input_.resize(INPUT_BYTES)) return fail(TextStatus::OutOfMemory);
  HalFile source;
  if (!Storage.openFileForRead("TEXT", txt.getPath(), source) || source.isDirectory())
    return fail(TextStatus::StorageError);
  const uint64_t size = source.fileSize64();
  if (size > UINT32_MAX) return fail(TextStatus::CapacityExceeded);
  sourceSize_ = static_cast<uint32_t>(size);
  uint32_t read = 0;
  bool bomOnly = size == 3;
  constexpr uint8_t bom[3] = {0xef, 0xbb, 0xbf};
  while (true) {
    const int got = source.read(input_.data(), INPUT_BYTES);
    if (got < 0) return fail(TextStatus::StorageError);
    if (!got) break;
    if (static_cast<size_t>(got) > INPUT_BYTES || static_cast<uint32_t>(got) > sourceSize_ - read)
      return fail(TextStatus::StorageError);
    if (bomOnly) {
      for (int i = 0; i < got; ++i) {
        if (input_[i] != bom[read + static_cast<uint32_t>(i)]) bomOnly = false;
      }
    }
    hashBytes(sourceHash_, input_.data(), static_cast<size_t>(got));
    read += static_cast<uint32_t>(got);
    nativeTextYield();
  }
  if (read != sourceSize_ || source.fileSize64() != sourceSize_) return fail(TextStatus::StorageError);
  if (!source.close()) return fail(TextStatus::StorageError);
  emptySource_ = !sourceSize_ || bomOnly;
  hash = sourceHash_;
  return status_;
}
#endif
