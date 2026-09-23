#include "NativeTextEngine.h"

#include <ft2build.h>

#include "NativeFontRegistry.h"
#include "NativePlatform.h"
#include "NativeThaiDictionary.generated.h"
#include "NativeUtf8.h"
#include "ThaiSegmenter.h"
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include <hb-ft.h>

extern "C" {
#include <minibidi.h>
}

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>

namespace {
constexpr size_t INITIAL_WORKSPACE = 256 * 1024;
constexpr size_t RUN_CACHE_LIMIT = 256 * 1024;
constexpr size_t GLYPH_CACHE_LIMIT = 512 * 1024;
constexpr size_t MAX_SCALARS = 4096;
constexpr size_t MAX_BYTES = 16 * 1024;
constexpr size_t MAX_GLYPHS = 32768;
constexpr FT_Int32 LOAD_FLAGS = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL;
constexpr uint8_t BOLD_STYLE = 1, ITALIC_STYLE = 2, SUP_STYLE = 16, SUB_STYLE = 32;
constexpr uint64_t FNV_OFFSET = UINT64_C(14695981039346656037);
constexpr uint64_t FNV_PRIME = UINT64_C(1099511628211);

struct LockGuard {
  LockGuard() { nativeTextLock(); }
  ~LockGuard() { nativeTextUnlock(); }
};

uint64_t hashBytes(uint64_t hash, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * FNV_PRIME;
  return hash;
}
uint64_t hashInteger(uint64_t hash, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    hash = (hash ^ static_cast<uint8_t>(value)) * FNV_PRIME;
    value >>= 8;
  }
  return hash;
}
size_t aligned(size_t bytes, size_t alignment) { return (bytes + alignment - 1) & ~(alignment - 1); }
bool checkedPosition(int64_t value, int32_t& result) {
  if (value < INT32_MIN || value > INT32_MAX) return false;
  result = static_cast<int32_t>(value);
  return true;
}
TextStatus ftStatus(FT_Error error, size_t failures) {
  if (native_text::allocationStats().failures != failures || error == FT_Err_Out_Of_Memory)
    return TextStatus::OutOfMemory;
  return error ? TextStatus::InvalidFont : TextStatus::Ok;
}
void* ftAllocate(FT_Memory, long size) { return size > 0 ? native_text_malloc(static_cast<size_t>(size)) : nullptr; }
void ftFree(FT_Memory, void* memory) { native_text_free(memory); }
void* ftReallocate(FT_Memory, long, long size, void* memory) {
  return size >= 0 ? native_text_realloc(memory, static_cast<size_t>(size)) : nullptr;
}

struct FaceLease {
  NativeFontRegistry& registry;
  NativeFaceHandle handle;
  explicit FaceLease(NativeFontRegistry& value) : registry(value) {}
  ~FaceLease() {
    if (handle.face) registry.release(handle);
  }
};
struct Arena {
  uint8_t* data;
  size_t position = 0;
  template <class T>
  T* take(size_t count) {
    position = aligned(position, alignof(T));
    auto* result = reinterpret_cast<T*>(data + position);
    position += sizeof(T) * count;
    return result;
  }
};
struct KeyWriter {
  uint8_t* data;
  size_t size = 0;
  void integer(uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
      data[size++] = static_cast<uint8_t>(value);
      value >>= 8;
    }
  }
  void bytes(const void* source, size_t count) {
    if (count) std::memcpy(data + size, source, count);
    size += count;
  }
};
struct Scalar {
  uint32_t cp, start, end;
  hb_script_t script;
  uint16_t unit;
  uint8_t style, level;
};
struct Unit {
  uint32_t start, end;
  uint16_t firstScalar, lastScalar, owner, next;
  hb_script_t script;
  NativeFaceChoice face;
  uint8_t style, level;
  bool boundary, missing, replacement, suffix;
};
struct Fragment {
  uint16_t first, end;
  uint8_t level;
};
bool sameFace(const NativeFaceChoice& a, const NativeFaceChoice& b) {
  return a.identity == b.identity && a.rasterSize26 == b.rasterSize26 && a.syntheticStyle == b.syntheticStyle;
}
bool neutralScript(hb_script_t script) {
  return script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_UNKNOWN;
}
bool mark(uint32_t cp) {
  const auto category = hb_unicode_general_category(hb_unicode_funcs_get_default(), cp);
  return category == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK ||
         category == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK || category == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
}
bool attaches(uint32_t previous, uint32_t cp, size_t regionalCount) {
  // Include join controls, variation selectors, emoji modifiers, virama chains,
  // and Thai leading-vowel cells before splitting styles or choosing a face.
  return mark(cp) || cp == 0x200c || cp == 0x200d || previous == 0x200d || previous == 0x200c ||
         (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef) || (cp >= 0x1f3fb && cp <= 0x1f3ff) ||
         hb_unicode_combining_class(hb_unicode_funcs_get_default(), previous) == 9 ||
         (previous >= 0x0e40 && previous <= 0x0e44 && cp >= 0x0e01 && cp <= 0x0e2e) ||
         (cp == 0x0e33 && native_text::isThai(previous)) ||
         (previous >= 0x1f1e6 && previous <= 0x1f1ff && cp >= 0x1f1e6 && cp <= 0x1f1ff && regionalCount % 2);
}
enum class ClusterSpacing { None, Text, Space };
ClusterSpacing clusterSpacing(std::string_view text, size_t start) {
  uint32_t cp = 0;
  if (!native_text::nextUtf8(text, start, cp)) return ClusterSpacing::None;
  const auto category = hb_unicode_general_category(hb_unicode_funcs_get_default(), cp);
  if (category == HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR) return ClusterSpacing::Space;
  if (category == HB_UNICODE_GENERAL_CATEGORY_CONTROL || category == HB_UNICODE_GENERAL_CATEGORY_FORMAT ||
      category == HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR ||
      category == HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR ||
      category == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK ||
      category == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK || category == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK)
    return ClusterSpacing::None;
  return ClusterSpacing::Text;
}
bool l1Whitespace(uint32_t cp) {
  const auto type = bidi_class(cp);
  return type == WS || type == BN || type == LRE || type == RLE || type == LRO || type == RLO || type == PDF ||
         type == LRI || type == RLI || type == FSI || type == PDI;
}
hb_language_t language(hb_script_t script) {
  return hb_language_from_string(script == HB_SCRIPT_THAI     ? "th"
                                 : script == HB_SCRIPT_ARABIC ? "ar"
                                 : script == HB_SCRIPT_HEBREW ? "he"
                                                              : "und",
                                 -1);
}

struct RunEntry {
  RunEntry* next;
  size_t bytes, keyBytes, glyphCount, clusterCount;
  uint64_t hash;
  uint8_t* key;
  NativeGlyph* glyphs;
  NativeCluster* clusters;
  int32_t advance, ascender, descender;
  NativeBounds ink;
  int8_t paragraph;
};
struct GlyphEntry {
  GlyphEntry* next;
  size_t bytes;
  uint64_t identity;
  uint32_t glyph, size;
  uint8_t synthetic;
  NativeBitmapView bitmap;
};
}  // namespace

struct NativeTextEngine::Impl {
  FT_MemoryRec_ memory{nullptr, ftAllocate, ftFree, ftReallocate};
  FT_Library library = nullptr;
  hb_buffer_t* buffer = nullptr;
  NativeFontRegistry registry;
  ThaiSegmenter segmenter;
  NativeBuffer<std::max_align_t> workspace;
  RunEntry* runs = nullptr;
  GlyphEntry* glyphs = nullptr;
  size_t runBytes = 0, glyphBytes = 0;

  ~Impl() {
    clearCaches();
    segmenter.shutdown();
    registry.shutdown();
    if (buffer) hb_buffer_destroy(buffer);
    if (library) FT_Done_Library(library);
  }
  void clearCaches() {
    while (runs) {
      auto* next = runs->next;
      native_text_free(runs);
      runs = next;
    }
    while (glyphs) {
      auto* next = glyphs->next;
      native_text_free(glyphs);
      glyphs = next;
    }
    runBytes = glyphBytes = 0;
  }
  void evictRun() {
    auto** cursor = &runs;
    if (!*cursor) return;
    while ((*cursor)->next) cursor = &(*cursor)->next;
    runBytes -= (*cursor)->bytes;
    native_text_free(*cursor);
    *cursor = nullptr;
  }
  void evictGlyph() {
    auto** cursor = &glyphs;
    if (!*cursor) return;
    while ((*cursor)->next) cursor = &(*cursor)->next;
    glyphBytes -= (*cursor)->bytes;
    native_text_free(*cursor);
    *cursor = nullptr;
  }
  bool reserveWorkspace(size_t bytes) {
    return workspace.resize((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
  }
  TextStatus initialize() {
    if (!reserveWorkspace(INITIAL_WORKSPACE)) return TextStatus::OutOfMemory;
    const size_t failures = native_text::allocationStats().failures;
    auto status = ftStatus(FT_New_Library(&memory, &library), failures);
    if (status != TextStatus::Ok) return status;
    FT_Add_Default_Modules(library);
    buffer = hb_buffer_create();
    hb_unicode_funcs_get_default();
    language(HB_SCRIPT_THAI);
    language(HB_SCRIPT_ARABIC);
    language(HB_SCRIPT_HEBREW);
    language(HB_SCRIPT_LATIN);
    if (!hb_buffer_allocation_successful(buffer) || native_text::allocationStats().failures != failures)
      return TextStatus::OutOfMemory;
    status = registry.initialize(library);
    if (status == TextStatus::Ok) status = registry.registerBuiltins();
    if (status == TextStatus::Ok) status = segmenter.initialize();
    return status;
  }
  TextStatus copyRun(const RunEntry& entry, NativeGlyphRun& output) {
    if (!output.glyphs.assign({entry.glyphs, entry.glyphCount}) ||
        !output.clusters.assign({entry.clusters, entry.clusterCount}))
      return TextStatus::OutOfMemory;
    output.advance26 = entry.advance;
    output.ascender26 = entry.ascender;
    output.descender26 = entry.descender;
    output.ink = entry.ink;
    output.paragraphLevel = entry.paragraph;
    return TextStatus::Ok;
  }
  TextStatus cacheRun(const uint8_t* key, size_t keyBytes, uint64_t hash, const NativeGlyphRun& output) {
    const size_t glyphOffset = aligned(sizeof(RunEntry) + keyBytes, alignof(NativeGlyph));
    const size_t clusterOffset =
        aligned(glyphOffset + output.glyphs.size() * sizeof(NativeGlyph), alignof(NativeCluster));
    const size_t bytes = clusterOffset + output.clusters.size() * sizeof(NativeCluster);
    // Account conservatively for allocator metadata in each independent cache.
    const size_t charge = bytes + alignof(std::max_align_t) * 2;
    if (charge > RUN_CACHE_LIMIT) return TextStatus::Ok;
    while (runBytes + charge > RUN_CACHE_LIMIT) evictRun();
    if (native_text::allocationStats().used + charge > native_text::MEMORY_LIMIT) return TextStatus::Ok;
    auto* allocation = static_cast<uint8_t*>(native_text_malloc(bytes));
    if (!allocation) return TextStatus::OutOfMemory;
    auto* entry = new (allocation) RunEntry{};
    entry->next = runs;
    entry->bytes = charge;
    entry->hash = hash;
    entry->keyBytes = keyBytes;
    entry->key = allocation + sizeof(RunEntry);
    entry->glyphs = reinterpret_cast<NativeGlyph*>(allocation + glyphOffset);
    entry->clusters = reinterpret_cast<NativeCluster*>(allocation + clusterOffset);
    entry->glyphCount = output.glyphs.size();
    entry->clusterCount = output.clusters.size();
    std::memcpy(entry->key, key, keyBytes);
    if (entry->glyphCount) std::memcpy(entry->glyphs, output.glyphs.data(), entry->glyphCount * sizeof(NativeGlyph));
    if (entry->clusterCount)
      std::memcpy(entry->clusters, output.clusters.data(), entry->clusterCount * sizeof(NativeCluster));
    entry->advance = output.advance26;
    entry->ascender = output.ascender26;
    entry->descender = output.descender26;
    entry->ink = output.ink;
    entry->paragraph = output.paragraphLevel;
    runs = entry;
    runBytes += charge;
    return TextStatus::Ok;
  }
  TextStatus shapeBuffer(std::string_view text, const Scalar* scalars, const Unit* units, uint16_t first, uint16_t last,
                         const NativeFaceHandle& face, bool replacement, bool readerFeatures) {
    const Unit& unit = units[scalars[first].unit];
    const uint32_t start = scalars[first].start, end = scalars[last - 1].end;
    const size_t failures = native_text::allocationStats().failures;
    hb_buffer_clear_contents(buffer);
    hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    hb_buffer_set_direction(buffer,
                            unit.script == HB_SCRIPT_THAI || !(unit.level & 1) ? HB_DIRECTION_LTR : HB_DIRECTION_RTL);
    hb_buffer_set_script(buffer, unit.script);
    hb_buffer_set_language(buffer, language(unit.script));
    unsigned flags = HB_BUFFER_FLAG_PRODUCE_UNSAFE_TO_CONCAT;
    if (!start) flags |= HB_BUFFER_FLAG_BOT;
    if (end == text.size()) flags |= HB_BUFFER_FLAG_EOT;
    hb_buffer_set_flags(buffer, static_cast<hb_buffer_flags_t>(flags));
    if (replacement) {
      hb_buffer_set_content_type(buffer, HB_BUFFER_CONTENT_TYPE_UNICODE);
      hb_buffer_add(buffer, 0xfffd, start);
    } else {
      // add_utf8 retains pre/post context in original Unicode, not presentation
      // forms. Only clusters are coalesced to protect base-owned markup cells.
      hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), start, end - start);
      unsigned count = 0;
      hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
      if (!hb_buffer_allocation_successful(buffer)) return TextStatus::OutOfMemory;
      if (count != static_cast<unsigned>(last - first)) return TextStatus::InvalidText;
      for (unsigned i = 0; i < count; ++i) infos[i].cluster = units[units[scalars[first + i].unit].owner].start;
    }
    hb_ft_font_set_load_flags(face.font, LOAD_FLAGS);
    const hb_feature_t proportional{HB_TAG('p', 'n', 'u', 'm'), 1, 0, HB_FEATURE_GLOBAL_END};
    hb_shape(face.font, buffer, readerFeatures ? &proportional : nullptr, readerFeatures ? 1 : 0);
    if (!hb_buffer_allocation_successful(buffer) || native_text::allocationStats().failures != failures)
      return TextStatus::OutOfMemory;
    const auto status = registry.faceStatus(face);
    if (status != TextStatus::Ok) return status;
    return hb_buffer_get_length(buffer) > MAX_GLYPHS ? TextStatus::CapacityExceeded : TextStatus::Ok;
  }
  bool missingGlyphs() const {
    unsigned count = 0;
    const auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
    for (unsigned i = 0; i < count; ++i)
      if (!infos[i].codepoint) return true;
    return count == 0;
  }
  uint16_t unitAt(const Unit* units, uint16_t first, uint16_t end, uint32_t byte) const {
    uint16_t low = first, high = end;
    while (low + 1 < high) {
      const uint16_t middle = low + (high - low) / 2;
      if (units[middle].start <= byte)
        low = middle;
      else
        high = middle;
    }
    return low;
  }
  TextStatus chooseFaces(const NativeLineInput& input, std::string_view text, Scalar* scalars, Unit* units,
                         uint16_t first, uint16_t end) {
    NativeFaceChoice choices[8];
    size_t choiceCount = 0;
    auto status = registry.candidates(input.fontId, units[first].style, units[first].script, choices, choiceCount);
    if (status != TextStatus::Ok) return status;
    if (!choiceCount) return TextStatus::InvalidFont;
    {
      FaceLease primary(registry);
      status = registry.acquire(choices[0], primary.handle);
      if (status != TextStatus::Ok) return status;
      status = shapeBuffer(text, scalars, units, units[first].firstScalar, units[end - 1].lastScalar, primary.handle,
                           false, input.readerFeatures);
      if (status != TextStatus::Ok) return status;
      unsigned count = 0;
      const auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
      if (!count) return TextStatus::InvalidFont;
      for (unsigned i = 0; i < count; ++i) {
        const uint16_t index = unitAt(units, first, end, infos[i].cluster);
        units[index].boundary = true;
        if (!infos[i].codepoint) units[index].missing = true;
      }
    }
    units[first].boundary = true;
    for (uint16_t index = first; index < end;) {
      uint16_t next = index + 1;
      while (next < end && !units[next].boundary) ++next;
      Unit& unit = units[index];
      unit.next = next;
      unit.end = units[next - 1].end;
      unit.lastScalar = units[next - 1].lastScalar;
      for (uint16_t i = index; i < next; ++i) {
        units[i].owner = index;
        unit.missing = unit.missing || units[i].missing;
      }
      unit.face = choices[0];
      bool found = !unit.missing;
      for (size_t candidate = 1; !found && candidate < choiceCount; ++candidate) {
        FaceLease fallback(registry);
        status = registry.acquire(choices[candidate], fallback.handle);
        if (status != TextStatus::Ok) return status;
        status = shapeBuffer(text, scalars, units, unit.firstScalar, unit.lastScalar, fallback.handle, false,
                             input.readerFeatures);
        if (status != TextStatus::Ok) return status;
        if (!missingGlyphs()) {
          unit.face = choices[candidate];
          found = true;
        }
      }
      if (!found) {
        unit.replacement = true;
        for (size_t candidate = 0; !found && candidate < choiceCount; ++candidate) {
          FaceLease fallback(registry);
          status = registry.acquire(choices[candidate], fallback.handle);
          if (status != TextStatus::Ok) return status;
          status = shapeBuffer(text, scalars, units, unit.firstScalar, unit.lastScalar, fallback.handle, true,
                               input.readerFeatures);
          if (status != TextStatus::Ok) return status;
          unsigned count = 0;
          const auto* positions = hb_buffer_get_glyph_positions(buffer, &count);
          int64_t advance = 0;
          for (unsigned i = 0; i < count; ++i) advance += positions[i].x_advance;
          if (!missingGlyphs() && advance > 0) {
            unit.face = choices[candidate];
            found = true;
          }
        }
        if (!found) return TextStatus::InvalidFont;
      }
      index = next;
      nativeTextYield();
    }
    return TextStatus::Ok;
  }
  TextStatus loadGlyph(const NativeFaceHandle& face, uint32_t glyph, uint8_t synthetic, int32_t& boldDelta) {
    const size_t failures = native_text::allocationStats().failures;
    const auto error = FT_Load_Glyph(face.face, glyph, LOAD_FLAGS);
    auto status = registry.faceStatus(face);
    if (status != TextStatus::Ok) return status;
    status = ftStatus(error, failures);
    if (status != TextStatus::Ok) return status;
    if (face.face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return TextStatus::InvalidFont;
    const FT_Pos advance = face.face->glyph->advance.x;
    if (synthetic & BOLD_STYLE) FT_GlyphSlot_Embolden(face.face->glyph);
    if (synthetic & ITALIC_STYLE) FT_GlyphSlot_Oblique(face.face->glyph);
    if (!checkedPosition(face.face->glyph->advance.x - advance, boldDelta)) return TextStatus::CapacityExceeded;
    return native_text::allocationStats().failures != failures ? TextStatus::OutOfMemory : TextStatus::Ok;
  }
  TextStatus baseline(const NativeLineInput& input, const Unit& unit, int32_t& offset) {
    offset = 0;
    if (!(unit.style & (SUP_STYLE | SUB_STYLE))) return TextStatus::Ok;
    NativeFaceChoice choices[8];
    size_t count = 0;
    auto status = registry.candidates(input.fontId, unit.style & ~(SUP_STYLE | SUB_STYLE), unit.script, choices, count);
    if (status != TextStatus::Ok) return status;
    if (!count) return TextStatus::InvalidFont;
    // Use the same fallback face at the parent size, not a Latin-only ascender.
    NativeFaceChoice parent = unit.face;
    parent.rasterSize26 = choices[0].rasterSize26;
    FaceLease lease(registry);
    status = registry.acquire(parent, lease.handle);
    if (status != TextStatus::Ok) return status;
    const int64_t ascent = lease.handle.face->size->metrics.ascender;
    return checkedPosition(unit.style & SUP_STYLE ? -(ascent * 40 / 100) : ascent * 25 / 100, offset)
               ? TextStatus::Ok
               : TextStatus::CapacityExceeded;
  }
  TextStatus emitFragment(const NativeLineInput& input, std::string_view text, Scalar* scalars, Unit* units,
                          const Fragment& fragment, bool* gapsUsed, NativeGlyphRun& output, bool& hasInk,
                          size_t& previousTextCluster, size_t& previousGlyphEnd) {
    Unit& unit = units[fragment.first];
    FaceLease face(registry);
    auto status = registry.acquire(unit.face, face.handle);
    if (status != TextStatus::Ok) return status;
    int32_t baselineOffset = 0;
    status = baseline(input, unit, baselineOffset);
    if (status != TextStatus::Ok) return status;
    const uint16_t lastScalar = units[fragment.end - 1].lastScalar;
    status = shapeBuffer(text, scalars, units, unit.firstScalar, lastScalar, face.handle, unit.replacement,
                         input.readerFeatures);
    if (status != TextStatus::Ok) return status;
    if (missingGlyphs()) return TextStatus::InvalidFont;
    unsigned count = 0;
    const auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const auto* positions = hb_buffer_get_glyph_positions(buffer, nullptr);
    if (output.glyphs.size() + count > MAX_GLYPHS) return TextStatus::CapacityExceeded;
    const size_t glyphBase = output.glyphs.size();
    const size_t requiredGlyphs = glyphBase + count;
    if (requiredGlyphs > output.glyphs.capacity() &&
        !output.glyphs.reserve(std::min(MAX_GLYPHS, std::max(requiredGlyphs, output.glyphs.capacity() * 2))))
      return TextStatus::OutOfMemory;
    if (!output.glyphs.resize(glyphBase + count)) return TextStatus::OutOfMemory;
    int32_t ascent = 0, descent = 0;
    if (!checkedPosition(static_cast<int64_t>(face.handle.face->size->metrics.ascender) - baselineOffset, ascent) ||
        !checkedPosition(-static_cast<int64_t>(face.handle.face->size->metrics.descender) + baselineOffset, descent))
      return TextStatus::CapacityExceeded;
    output.ascender26 = std::max(output.ascender26, ascent);
    output.descender26 = std::max(output.descender26, descent);
    const bool rtl = hb_buffer_get_direction(buffer) == HB_DIRECTION_RTL;
    int64_t pen = output.advance26;
    const uint32_t fragmentEnd = scalars[lastScalar - 1].end;
    for (unsigned firstGlyph = 0; firstGlyph < count;) {
      unsigned lastGlyph = firstGlyph + 1;
      while (lastGlyph < count && infos[lastGlyph].cluster == infos[firstGlyph].cluster) ++lastGlyph;
      const uint32_t start = infos[firstGlyph].cluster;
      const uint32_t end = rtl ? (firstGlyph ? infos[firstGlyph - 1].cluster : fragmentEnd)
                               : (lastGlyph < count ? infos[lastGlyph].cluster : fragmentEnd);
      const auto spacing = input.characterSpacing || input.wordSpacingPercent != 100 ? clusterSpacing(text, start)
                                                                                     : ClusterSpacing::None;
      if (input.characterSpacing && previousTextCluster != SIZE_MAX && spacing == ClusterSpacing::Text) {
        // Insert tracking only at complete visual shaping-cluster boundaries,
        // including face/style/bidi transitions. Never move a Thai mark on its own.
        auto& previous = output.clusters[previousTextCluster];
        const int32_t tracking = std::max<int32_t>(input.characterSpacing * 64, -previous.advance26);
        if (!checkedPosition(static_cast<int64_t>(previous.advance26) + tracking, previous.advance26))
          return TextStatus::CapacityExceeded;
        // Invisible zero-advance clusters (soft hyphens and bidi controls)
        // neither receive tracking nor swallow the gap between visible cells.
        for (size_t i = previousTextCluster + 1; i < output.clusters.size(); ++i)
          if (!checkedPosition(static_cast<int64_t>(output.clusters[i].x26) + tracking, output.clusters[i].x26))
            return TextStatus::CapacityExceeded;
        for (size_t i = previousGlyphEnd; i < glyphBase + firstGlyph; ++i)
          if (!checkedPosition(static_cast<int64_t>(output.glyphs[i].x26) + tracking, output.glyphs[i].x26))
            return TextStatus::CapacityExceeded;
        pen += tracking;
      }
      NativeCluster cluster{};
      cluster.startByte = std::min<uint32_t>(start, input.text.size());
      cluster.endByte = std::min<uint32_t>(end, input.text.size());
      cluster.style = unit.style;
      cluster.bidiLevel = unit.level;
      if (!checkedPosition(pen, cluster.x26)) return TextStatus::CapacityExceeded;
      const int64_t clusterPen = pen;
      bool clusterInk = false;
      for (unsigned i = firstGlyph; i < lastGlyph; ++i) {
        NativeGlyph& glyph = output.glyphs[glyphBase + i];
        glyph = {};
        glyph.faceIdentity = unit.face.identity;
        glyph.rasterSize26 = unit.face.rasterSize26;
        glyph.syntheticStyle = unit.face.syntheticStyle;
        glyph.style = unit.style;
        glyph.glyphId = infos[i].codepoint;
        glyph.startByte = cluster.startByte;
        glyph.endByte = cluster.endByte;
        if (!checkedPosition(pen + positions[i].x_offset, glyph.x26) ||
            !checkedPosition(static_cast<int64_t>(baselineOffset) - positions[i].y_offset, glyph.y26) ||
            !checkedPosition(-static_cast<int64_t>(positions[i].y_advance), glyph.advanceY26))
          return TextStatus::CapacityExceeded;
        int32_t boldDelta = 0;
        status = loadGlyph(face.handle, glyph.glyphId, glyph.syntheticStyle, boldDelta);
        if (status != TextStatus::Ok) return status;
        if (!checkedPosition(
                static_cast<int64_t>(positions[i].x_advance) + (positions[i].x_advance > 0 ? boldDelta : 0),
                glyph.advanceX26))
          return TextStatus::CapacityExceeded;
        cluster.unsafeToBreak =
            cluster.unsafeToBreak || (hb_glyph_info_get_glyph_flags(&infos[i]) & HB_GLYPH_FLAG_UNSAFE_TO_BREAK);
        const auto& outline = face.handle.face->glyph->outline;
        if (outline.n_points) {
          FT_BBox box;
          FT_Outline_Get_CBox(&outline, &box);
          // Match FT_Render_Glyph's pixel-aligned coverage bounds, after style
          // synthesis. Positions themselves retain their full 26.6 precision.
          const int64_t left = static_cast<int64_t>(box.xMin) & ~INT64_C(63);
          const int64_t right = (static_cast<int64_t>(box.xMax) + 63) & ~INT64_C(63);
          const int64_t bottom = static_cast<int64_t>(box.yMin) & ~INT64_C(63);
          const int64_t top = (static_cast<int64_t>(box.yMax) + 63) & ~INT64_C(63);
          NativeBounds ink;
          if (!checkedPosition(glyph.x26 + left, ink.left26) || !checkedPosition(glyph.x26 + right, ink.right26) ||
              !checkedPosition(glyph.y26 - top, ink.top26) || !checkedPosition(glyph.y26 - bottom, ink.bottom26))
            return TextStatus::CapacityExceeded;
          if (!hasInk) {
            output.ink = ink;
            hasInk = true;
          } else {
            output.ink.left26 = std::min(output.ink.left26, ink.left26);
            output.ink.right26 = std::max(output.ink.right26, ink.right26);
            output.ink.top26 = std::min(output.ink.top26, ink.top26);
            output.ink.bottom26 = std::max(output.ink.bottom26, ink.bottom26);
          }
          if (!clusterInk) {
            cluster.top26 = ink.top26;
            cluster.bottom26 = ink.bottom26;
            clusterInk = true;
          } else {
            cluster.top26 = std::min(cluster.top26, ink.top26);
            cluster.bottom26 = std::max(cluster.bottom26, ink.bottom26);
          }
        }
        pen += glyph.advanceX26;
      }
      // Word spacing scales the shaped advance of a real space, not a
      // dictionary word boundary or a justification/ruby gap.
      if (spacing == ClusterSpacing::Space && input.wordSpacingPercent != 100)
        pen = clusterPen + ((pen - clusterPen) * input.wordSpacingPercent + 50) / 100;
      if (cluster.startByte != cluster.endByte) {
        const auto gap =
            std::lower_bound(input.gaps.begin(), input.gaps.end(), cluster.endByte,
                             [](const NativeGap& value, uint32_t byte) { return value.byteOffset < byte; });
        if (gap != input.gaps.end() && gap->byteOffset == cluster.endByte) {
          pen += gap->extraAdvance26;
          gapsUsed[static_cast<size_t>(gap - input.gaps.begin())] = true;
        }
      }
      if (!checkedPosition(pen - clusterPen, cluster.advance26) || cluster.advance26 < 0)
        return TextStatus::CapacityExceeded;
      const size_t clusterIndex = output.clusters.size();
      if (!output.clusters.resize(clusterIndex + 1)) return TextStatus::OutOfMemory;
      output.clusters[clusterIndex] = cluster;
      if (spacing == ClusterSpacing::Text) {
        previousTextCluster = clusterIndex;
        previousGlyphEnd = glyphBase + lastGlyph;
      } else if (spacing == ClusterSpacing::Space || cluster.advance26 || clusterInk) {
        previousTextCluster = SIZE_MAX;
      }
      firstGlyph = lastGlyph;
    }
    if (!checkedPosition(pen, output.advance26)) return TextStatus::CapacityExceeded;
    if (hasInk) {
      if (output.ink.top26 == INT32_MIN) return TextStatus::CapacityExceeded;
      output.ascender26 = std::max(output.ascender26, -output.ink.top26);
      output.descender26 = std::max(output.descender26, output.ink.bottom26);
    }
    nativeTextYield();
    return TextStatus::Ok;
  }

  TextStatus shape(const NativeLineInput& input, NativeGlyphRun& output) {
    output.clear();
    if (input.text.size() > MAX_BYTES || input.spans.size() > MAX_SCALARS || input.gaps.size() > MAX_SCALARS)
      return TextStatus::CapacityExceeded;
    if (input.paragraphLevel < -1 || input.paragraphLevel > 1 || (input.resolvedLevels && input.paragraphLevel < 0) ||
        (input.syntheticSuffixCp != 0 && input.syntheticSuffixCp != 0x2d))
      return TextStatus::InvalidText;
    size_t scalarCount = 0, offset = 0;
    uint32_t cp = 0;
    while (offset < input.text.size()) {
      if (!native_text::nextUtf8(input.text, offset, cp)) return TextStatus::InvalidText;
      if (++scalarCount > MAX_SCALARS) return TextStatus::CapacityExceeded;
    }
    uint32_t previousEnd = 0;
    for (const auto& span : input.spans) {
      if (span.startByte < previousEnd || span.endByte <= span.startByte || span.bidiLevel > 125 ||
          !native_text::utf8Boundary(input.text, span.startByte) ||
          !native_text::utf8Boundary(input.text, span.endByte))
        return TextStatus::InvalidText;
      previousEnd = span.endByte;
    }
    previousEnd = 0;
    for (const auto& gap : input.gaps) {
      if (gap.extraAdvance26 < 0 || gap.byteOffset <= previousEnd ||
          !native_text::utf8Boundary(input.text, gap.byteOffset))
        return TextStatus::InvalidText;
      previousEnd = gap.byteOffset;
    }
    if (input.text.empty() && !input.syntheticSuffixCp) return TextStatus::Ok;
    if (!registry.hasFont(input.fontId)) return TextStatus::InvalidFont;
    const size_t totalScalars = scalarCount + (input.syntheticSuffixCp ? 1 : 0);
    const size_t keyCapacity = 64 + input.text.size() + input.spans.size() * 10 + input.gaps.size() * 8;
    const size_t scratchSize = bidi_scratch_size(scalarCount);
    const size_t required =
        keyCapacity + input.text.size() + 1 + scratchSize + 128 +
        totalScalars * (sizeof(Scalar) + sizeof(Unit) + sizeof(Fragment) + sizeof(bidi_char) + sizeof(uint8_t)) +
        input.gaps.size();
    if (!reserveWorkspace(std::max(INITIAL_WORKSPACE, required))) return TextStatus::OutOfMemory;
    Arena arena{reinterpret_cast<uint8_t*>(workspace.data())};
    auto* key = arena.take<uint8_t>(keyCapacity);
    KeyWriter writer{key};
    writer.integer(registry.fingerprint(input.fontId), 8);
    writer.integer(static_cast<uint32_t>(input.fontId), 4);
    writer.integer(static_cast<uint8_t>(input.paragraphLevel), 1);
    writer.integer(input.syntheticSuffixCp, 4);
    writer.integer(input.readerFeatures, 1);
    writer.integer(input.resolvedLevels, 1);
    writer.integer(static_cast<uint8_t>(input.characterSpacing), 1);
    writer.integer(input.wordSpacingPercent, 1);
    writer.integer(input.text.size(), 4);
    writer.bytes(input.text.data(), input.text.size());
    writer.integer(input.spans.size(), 4);
    for (const auto& span : input.spans) {
      writer.integer(span.startByte, 4);
      writer.integer(span.endByte, 4);
      writer.integer(span.style, 1);
      writer.integer(span.bidiLevel, 1);
    }
    writer.integer(input.gaps.size(), 4);
    for (const auto& gap : input.gaps) {
      writer.integer(gap.byteOffset, 4);
      writer.integer(static_cast<uint32_t>(gap.extraAdvance26), 4);
    }
    const uint64_t hash = hashBytes(FNV_OFFSET, key, writer.size);
    for (auto** entry = &runs; *entry; entry = &(*entry)->next) {
      if ((*entry)->hash == hash && (*entry)->keyBytes == writer.size &&
          !std::memcmp((*entry)->key, key, writer.size)) {
        RunEntry* hit = *entry;
        *entry = hit->next;
        hit->next = runs;
        runs = hit;
        return copyRun(*hit, output);
      }
    }
    if (!output.glyphs.reserve(totalScalars) || !output.clusters.reserve(totalScalars)) return TextStatus::OutOfMemory;
    auto* textBytes = arena.take<char>(input.text.size() + 1);
    if (!input.text.empty()) std::memcpy(textBytes, input.text.data(), input.text.size());
    if (input.syntheticSuffixCp) textBytes[input.text.size()] = '-';
    const std::string_view text{textBytes, input.text.size() + (input.syntheticSuffixCp ? 1 : 0)};
    auto* scalars = arena.take<Scalar>(totalScalars);
    auto* units = arena.take<Unit>(totalScalars);
    auto* fragments = arena.take<Fragment>(totalScalars);
    auto* bidi = arena.take<bidi_char>(scalarCount);
    auto* levels = arena.take<uint8_t>(scalarCount);
    auto* scratch = arena.take<uint8_t>(scratchSize);
    auto* gapsUsed = arena.take<bool>(input.gaps.size());
    if (!input.gaps.empty()) std::memset(gapsUsed, 0, input.gaps.size());
    offset = 0;
    size_t spanIndex = 0;
    for (size_t i = 0; i < totalScalars; ++i) {
      Scalar& scalar = scalars[i];
      scalar = {};
      scalar.start = static_cast<uint32_t>(offset);
      if (!native_text::nextUtf8(text, offset, scalar.cp)) return TextStatus::InvalidText;
      scalar.end = static_cast<uint32_t>(offset);
      scalar.script = hb_unicode_script(hb_unicode_funcs_get_default(), scalar.cp);
      while (spanIndex < input.spans.size() && input.spans[spanIndex].endByte <= scalar.start) ++spanIndex;
      if (spanIndex < input.spans.size() && input.spans[spanIndex].startByte <= scalar.start) {
        scalar.style = input.spans[spanIndex].style;
        scalar.level = input.spans[spanIndex].bidiLevel;
      } else
        scalar.level = input.paragraphLevel < 0 ? 0 : input.paragraphLevel;
      if (i < scalarCount) bidi[i] = {scalar.cp, scalar.cp, static_cast<uint16_t>(i), 0};
    }
    int paragraph = input.paragraphLevel < 0 ? 0 : input.paragraphLevel;
    if (!input.resolvedLevels && scalarCount) {
      paragraph = resolve_bidi_levels(input.paragraphLevel < 0, paragraph, bidi, static_cast<int>(scalarCount), levels,
                                      scratch, scratchSize);
      if (paragraph < 0) return TextStatus::CapacityExceeded;
      for (size_t i = 0; i < scalarCount; ++i) scalars[i].level = levels[i];
    }
    output.paragraphLevel = static_cast<int8_t>(paragraph);
    // UAX #9 L1 is line-local, unlike the persisted paragraph levels.
    for (size_t i = 0; i <= scalarCount; ++i) {
      if (i == scalarCount || bidi_class(scalars[i].cp) == B || bidi_class(scalars[i].cp) == S) {
        if (i < scalarCount) scalars[i].level = paragraph;
        size_t previous = i;
        while (previous && l1Whitespace(scalars[previous - 1].cp)) scalars[--previous].level = paragraph;
      }
    }
    // Resolve Common/Inherited against real adjacent scripts without deleting
    // punctuation; leading neutral text uses the following strong script.
    hb_script_t preceding = HB_SCRIPT_UNKNOWN;
    for (size_t i = 0; i < totalScalars; ++i) {
      if (!neutralScript(scalars[i].script))
        preceding = scalars[i].script;
      else if (!neutralScript(preceding))
        scalars[i].script = preceding;
    }
    hb_script_t following = HB_SCRIPT_LATIN;
    for (size_t i = totalScalars; i-- > 0;) {
      if (!neutralScript(scalars[i].script))
        following = scalars[i].script;
      else
        scalars[i].script = following;
    }
    uint16_t unitCount = 0;
    size_t regionalCount = 0;
    for (uint16_t i = 0; i < totalScalars; ++i) {
      Scalar& scalar = scalars[i];
      const bool suffix = i == scalarCount;
      if (suffix && i) {
        const Unit& base = units[unitCount - 1];
        scalar.style = base.style;
        scalar.level = base.level;
        scalar.script = base.script;
      }
      const bool attached = i && !suffix && attaches(scalars[i - 1].cp, scalar.cp, regionalCount);
      if (attached) {
        Unit& base = units[unitCount - 1];
        base.end = scalar.end;
        base.lastScalar = i + 1;
        scalar.style = base.style;
        scalar.level = base.level;
        scalar.script = base.script;
        scalar.unit = unitCount - 1;
      } else {
        scalar.unit = unitCount;
        Unit& unit = units[unitCount];
        unit = {};
        unit.start = scalar.start;
        unit.end = scalar.end;
        unit.firstScalar = i;
        unit.lastScalar = i + 1;
        unit.owner = unitCount;
        unit.next = unitCount + 1;
        unit.style = scalar.style;
        unit.level = scalar.level;
        unit.script = scalar.script;
        unit.suffix = suffix;
        ++unitCount;
      }
      regionalCount = scalar.cp >= 0x1f1e6 && scalar.cp <= 0x1f1ff ? regionalCount + 1 : 0;
    }
    for (uint16_t first = 0; first < unitCount;) {
      uint16_t end = first + 1;
      while (end < unitCount && units[end].style == units[first].style && units[end].level == units[first].level &&
             units[end].script == units[first].script && units[end].suffix == units[first].suffix)
        ++end;
      const auto status = chooseFaces(input, text, scalars, units, first, end);
      if (status != TextStatus::Ok) return status;
      first = end;
    }
    uint16_t fragmentCount = 0;
    uint8_t maximumLevel = 0, minimumOdd = 255;
    for (uint16_t first = 0; first < unitCount;) {
      const Unit& unit = units[first];
      uint16_t end = unit.next;
      while (!unit.replacement && end < unitCount && !units[end].replacement && units[end].style == unit.style &&
             units[end].level == unit.level && units[end].script == unit.script && units[end].suffix == unit.suffix &&
             sameFace(units[end].face, unit.face))
        end = units[end].next;
      fragments[fragmentCount++] = {first, end, unit.level};
      maximumLevel = std::max(maximumLevel, unit.level);
      if (unit.level & 1) minimumOdd = std::min(minimumOdd, unit.level);
      first = end;
    }
    // UAX #9 L2: HarfBuzz orders glyphs within a directional run; only runs
    // are reversed here. Mirroring is exclusively HarfBuzz's responsibility.
    for (int level = maximumLevel; level >= minimumOdd; --level) {
      for (uint16_t first = 0; first < fragmentCount;) {
        while (first < fragmentCount && fragments[first].level < level) ++first;
        uint16_t end = first;
        while (end < fragmentCount && fragments[end].level >= level) ++end;
        std::reverse(fragments + first, fragments + end);
        first = end;
      }
    }
    bool hasInk = false;
    size_t previousTextCluster = SIZE_MAX, previousGlyphEnd = 0;
    for (uint16_t i = 0; i < fragmentCount; ++i) {
      const auto status = emitFragment(input, text, scalars, units, fragments[i], gapsUsed, output, hasInk,
                                       previousTextCluster, previousGlyphEnd);
      if (status != TextStatus::Ok) return status;
    }
    for (size_t i = 0; i < input.gaps.size(); ++i)
      if (!gapsUsed[i]) return TextStatus::InvalidText;
    return cacheRun(key, writer.size, hash, output);
  }

  TextStatus raster(const NativeGlyph& glyph, NativeBitmapView& bitmap) {
    for (auto** cursor = &glyphs; *cursor; cursor = &(*cursor)->next) {
      auto* entry = *cursor;
      if (entry->identity == glyph.faceIdentity && entry->size == glyph.rasterSize26 && entry->glyph == glyph.glyphId &&
          entry->synthetic == glyph.syntheticStyle) {
        *cursor = entry->next;
        entry->next = glyphs;
        glyphs = entry;
        bitmap = entry->bitmap;
        return TextStatus::Ok;
      }
    }
    FaceLease face(registry);
    auto status = registry.acquireGlyph(glyph, face.handle);
    if (status != TextStatus::Ok) return status;
    int32_t unusedDelta = 0;
    status = loadGlyph(face.handle, glyph.glyphId, glyph.syntheticStyle, unusedDelta);
    if (status != TextStatus::Ok) return status;
    const size_t failures = native_text::allocationStats().failures;
    status = ftStatus(FT_Render_Glyph(face.handle.face->glyph, FT_RENDER_MODE_NORMAL), failures);
    if (status != TextStatus::Ok) return status;
    const FT_GlyphSlot slot = face.handle.face->glyph;
    const FT_Bitmap& source = slot->bitmap;
    if (source.width > INT_MAX || source.rows > INT_MAX ||
        (source.width && source.rows &&
         (!source.buffer || source.pixel_mode != FT_PIXEL_MODE_GRAY || source.num_grays != 256 ||
          static_cast<uint64_t>(source.pitch < 0 ? -static_cast<int64_t>(source.pitch) : source.pitch) < source.width)))
      return TextStatus::InvalidFont;
    const uint64_t pixels64 = static_cast<uint64_t>(source.width) * source.rows;
    if (pixels64 > native_text::MEMORY_LIMIT) return TextStatus::CapacityExceeded;
    const size_t pixels = static_cast<size_t>(pixels64);
    const size_t bytes = sizeof(GlyphEntry) + pixels;
    const size_t charge = bytes + alignof(std::max_align_t) * 2;
    bitmap = {
        source.buffer,   static_cast<int>(source.width), static_cast<int>(source.rows), source.pitch, slot->bitmap_left,
        slot->bitmap_top};
    // An oversized bitmap remains owned by the live FT face until the next
    // engine mutation. Its view retains the original signed pitch.
    if (charge > GLYPH_CACHE_LIMIT) return TextStatus::Ok;
    while (glyphBytes + charge > GLYPH_CACHE_LIMIT) evictGlyph();
    if (native_text::allocationStats().used + charge > native_text::MEMORY_LIMIT) return TextStatus::Ok;
    auto* allocation = static_cast<uint8_t*>(native_text_malloc(bytes));
    if (!allocation) return TextStatus::OutOfMemory;
    auto* entry = new (allocation) GlyphEntry{};
    entry->next = glyphs;
    entry->bytes = charge;
    entry->identity = glyph.faceIdentity;
    entry->size = glyph.rasterSize26;
    entry->glyph = glyph.glyphId;
    entry->synthetic = glyph.syntheticStyle;
    auto* data = allocation + sizeof(GlyphEntry);
    for (unsigned y = 0; y < source.rows && source.width; ++y) {
      // FT's buffer denotes the first row in storage; negative pitch means
      // the logical top scanline is the last stored row.
      const auto* row = source.pitch >= 0
                            ? source.buffer + static_cast<size_t>(y) * source.pitch
                            : source.buffer + static_cast<size_t>(source.rows - 1 - y) *
                                                  static_cast<size_t>(-static_cast<int64_t>(source.pitch));
      std::memcpy(data + static_cast<size_t>(y) * source.width, row, source.width);
    }
    entry->bitmap = {data,
                     static_cast<int>(source.width),
                     static_cast<int>(source.rows),
                     static_cast<int>(source.width),
                     slot->bitmap_left,
                     slot->bitmap_top};
    glyphs = entry;
    glyphBytes += charge;
    bitmap = entry->bitmap;
    return TextStatus::Ok;
  }
};

NativeTextEngine::~NativeTextEngine() { shutdown(); }
void NativeTextEngine::lock() { nativeTextLock(); }
void NativeTextEngine::unlock() { nativeTextUnlock(); }
TextStatus NativeTextEngine::report(TextStatus status) {
  status_ = status;
  if (status != TextStatus::Ok) {
    const char* message = status == TextStatus::OutOfMemory    ? "Native text allocation failed"
                          : status == TextStatus::InvalidFont  ? "Native text font is invalid or unavailable"
                          : status == TextStatus::InvalidText  ? "Native text input is invalid"
                          : status == TextStatus::StorageError ? "Native text font storage read failed"
                                                               : "Native text capacity exceeded";
    nativeTextLogError(message);
  }
  return status;
}
TextStatus NativeTextEngine::initialize() {
  LockGuard guard;
  if (impl_) return report(TextStatus::Ok);
  TextStatus status = TextStatus::OutOfMemory;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    void* memory = native_text_malloc(sizeof(Impl));
    if (memory) {
      auto* instance = new (memory) Impl;
      status = instance->initialize();
      if (status == TextStatus::Ok) {
        impl_ = instance;
        return report(status);
      }
      instance->~Impl();
      native_text_free(instance);
    }
    if (status != TextStatus::OutOfMemory) break;
  }
  return report(status);
}
void NativeTextEngine::shutdown() {
  LockGuard guard;
  if (impl_) {
    impl_->~Impl();
    native_text_free(impl_);
    impl_ = nullptr;
  }
  status_ = TextStatus::Ok;
}
bool NativeTextEngine::ready() const {
  LockGuard guard;
  return impl_ != nullptr;
}
TextStatus NativeTextEngine::lastStatus() const {
  LockGuard guard;
  return status_;
}
TextStatus NativeTextEngine::shapeLine(const NativeLineInput& input, NativeGlyphRun& output) {
  LockGuard guard;
  output.clear();
  if (!impl_) return report(TextStatus::InvalidFont);
  TextStatus status = TextStatus::Ok;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const size_t failures = native_text::allocationStats().failures;
    status = impl_->shape(input, output);
    if (native_text::allocationStats().failures != failures) status = TextStatus::OutOfMemory;
    if (status == TextStatus::Ok) return report(status);
    output.clear();
    if (status != TextStatus::OutOfMemory || attempt) break;
    impl_->clearCaches();
    impl_->registry.evictUnusedFaces();
    hb_buffer_reset(impl_->buffer);
  }
  return report(status);
}
TextStatus NativeTextEngine::fontMetrics(int fontId, uint8_t style, int32_t& lineHeight26, int32_t& ascender26,
                                         int32_t& descender26) {
  LockGuard guard;
  lineHeight26 = ascender26 = descender26 = 0;
  if (!impl_) return report(TextStatus::InvalidFont);
  TextStatus status = TextStatus::Ok;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const size_t failures = native_text::allocationStats().failures;
    NativeFaceChoice choices[8];
    size_t count = 0;
    status = impl_->registry.candidates(fontId, style, HB_SCRIPT_LATIN, choices, count);
    if (status == TextStatus::Ok && !count) status = TextStatus::InvalidFont;
    {
      FaceLease lease(impl_->registry);
      if (status == TextStatus::Ok) status = impl_->registry.acquire(choices[0], lease.handle);
      if (status == TextStatus::Ok) {
        status = impl_->registry.faceStatus(lease.handle);
        const auto& metrics = lease.handle.face->size->metrics;
        if (status == TextStatus::Ok &&
            (!checkedPosition(metrics.height, lineHeight26) || !checkedPosition(metrics.ascender, ascender26) ||
             !checkedPosition(-static_cast<int64_t>(metrics.descender), descender26)))
          status = TextStatus::CapacityExceeded;
      }
    }
    if (native_text::allocationStats().failures != failures) status = TextStatus::OutOfMemory;
    if (status == TextStatus::Ok) return report(status);
    lineHeight26 = ascender26 = descender26 = 0;
    if (status != TextStatus::OutOfMemory || attempt) break;
    impl_->clearCaches();
    impl_->registry.evictUnusedFaces();
  }
  return report(status);
}
TextStatus NativeTextEngine::rasterize(const NativeGlyph& glyph, NativeBitmapView& bitmap) {
  LockGuard guard;
  bitmap = {};
  if (!impl_) return report(TextStatus::InvalidFont);
  TextStatus status = TextStatus::Ok;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const size_t failures = native_text::allocationStats().failures;
    status = impl_->raster(glyph, bitmap);
    if (native_text::allocationStats().failures != failures) status = TextStatus::OutOfMemory;
    if (status == TextStatus::Ok) return report(status);
    bitmap = {};
    if (status != TextStatus::OutOfMemory || attempt) break;
    impl_->clearCaches();
    impl_->registry.evictUnusedFaces();
  }
  return report(status);
}
TextStatus NativeTextEngine::findThaiBreaks(std::string_view text, std::span<uint32_t> offsets, size_t& count,
                                            bool emergency) {
  LockGuard guard;
  count = 0;
  if (!impl_) return report(TextStatus::InvalidFont);
  TextStatus status = TextStatus::Ok;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const size_t failures = native_text::allocationStats().failures;
    status = emergency ? impl_->segmenter.findEmergencyBreaks(text, offsets, count)
                       : impl_->segmenter.findBreaks(text, offsets, count);
    if (native_text::allocationStats().failures != failures) status = TextStatus::OutOfMemory;
    if (status == TextStatus::Ok) return report(status);
    count = 0;
    if (status != TextStatus::OutOfMemory || attempt) break;
    impl_->clearCaches();
    impl_->registry.evictUnusedFaces();
  }
  return report(status);
}
TextStatus NativeTextEngine::registerCustomFont(int fontId, uint16_t pointSize, std::span<const NativeFontFile> files,
                                                bool serif) {
  LockGuard guard;
  if (!impl_) return report(TextStatus::InvalidFont);
  impl_->clearCaches();
  TextStatus status = TextStatus::Ok;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const size_t failures = native_text::allocationStats().failures;
    status = impl_->registry.registerCustomFont(fontId, pointSize, files, serif);
    if (native_text::allocationStats().failures != failures) status = TextStatus::OutOfMemory;
    if (status != TextStatus::OutOfMemory || attempt) break;
    impl_->registry.evictUnusedFaces();
  }
  return report(status);
}
TextStatus NativeTextEngine::registerFontAlias(int fontId, int sourceFontId, uint16_t pointSize) {
  LockGuard guard;
  if (!impl_) return report(TextStatus::InvalidFont);
  impl_->clearCaches();
  return report(impl_->registry.registerFontAlias(fontId, sourceFontId, pointSize));
}
TextStatus NativeTextEngine::validateFontFile(std::string_view path, std::span<const NativeVariation> axes) {
  LockGuard guard;
  if (!impl_) return report(TextStatus::InvalidFont);
  TextStatus status = TextStatus::Ok;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const size_t failures = native_text::allocationStats().failures;
    status = impl_->registry.validateFontFile(path, axes);
    if (native_text::allocationStats().failures != failures) status = TextStatus::OutOfMemory;
    if (status != TextStatus::OutOfMemory || attempt) break;
    impl_->clearCaches();
    impl_->registry.evictUnusedFaces();
  }
  return report(status);
}
TextStatus NativeTextEngine::setUiFallback(int customFontId) {
  LockGuard guard;
  if (!impl_) return report(TextStatus::InvalidFont);
  impl_->clearCaches();
  return report(impl_->registry.setUiFallback(customFontId));
}
void NativeTextEngine::clearCustomFonts() {
  LockGuard guard;
  if (!impl_) return;
  impl_->clearCaches();
  impl_->registry.clearCustomFonts();
}
void NativeTextEngine::clearCaches() {
  LockGuard guard;
  if (impl_) {
    impl_->clearCaches();
    impl_->registry.evictUnusedFaces();
  }
}
void NativeTextEngine::releaseSdFaces() {
  LockGuard guard;
  if (impl_) {
    impl_->clearCaches();
    impl_->registry.releaseSdFaces();
  }
}
uint64_t NativeTextEngine::layoutFingerprint(int fontId) const {
  LockGuard guard;
  if (!impl_ || !impl_->registry.hasFont(fontId)) return 0;
  static constexpr char policy[] =
      "native-text-v1;ft=2.14.3;hb=14.5.0;thai=0.1.30;datrie=0.2.14;"
      "36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f;"
      "b7132e148358a45185c9feafd049dbaf243649d3c44414b3534d9c95d18592b9;"
      "ddba8b53dfe584c3253766030218a88825488a51a7deef041d096e715af64bdd;"
      "f04095010518635b51c2313efa4f290b7db828d6273e39b2b8858f859dfe81d5;"
      "dpi=150;load=default,no-bitmap,target-normal;render=gray;metrics=26.6;"
      "bold=positive-advance-once;oblique=0x366a;sup=40%;sub=25%;pnum=reader;"
      "bidi=logical,L1,L2;clusters=monotone-graphemes;segment=libthai-window4096-v1;"
      "record=overflow-clip-spacing-v3;tracking=visual-clusters;word-spacing=unicode-space-advance";
  uint64_t hash = hashBytes(FNV_OFFSET, policy, sizeof(policy) - 1);
  hash = hashBytes(hash, native_text::assets::thaiDictionarySha256,
                   std::strlen(native_text::assets::thaiDictionarySha256));
  hash = hashBytes(hash, native_text::assets::thaiDictionarySourceSha256,
                   std::strlen(native_text::assets::thaiDictionarySourceSha256));
  return hashInteger(hash, impl_->registry.fingerprint(fontId));
}
