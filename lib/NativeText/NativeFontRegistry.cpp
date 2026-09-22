#include "NativeFontRegistry.h"

#include <ft2build.h>

#include "../../src/fontIds.h"
#include "NativeFontAssets.generated.h"
#include "NativePlatform.h"
#include FT_FONT_FORMATS_H
#include FT_MULTIPLE_MASTERS_H
#include <hb-ft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace {
using native_text::detail::closeFontStream;
using native_text::detail::FontStream;
using native_text::detail::fontStreamFailed;
using native_text::detail::fontStreamSize;
using native_text::detail::openFontStream;
using native_text::detail::readFontStream;
constexpr size_t BUILTIN_COUNT = 20;
constexpr size_t SOURCE_LIMIT = BUILTIN_COUNT + 128 * 4;
constexpr size_t MAPPING_LIMIT = 11 + 128 * 4;
constexpr size_t FACE_LIMIT = 24;
constexpr uint16_t NO_SOURCE = std::numeric_limits<uint16_t>::max();
constexpr uint64_t FNV_OFFSET = UINT64_C(14695981039346656037);
constexpr uint64_t FNV_PRIME = UINT64_C(1099511628211);
constexpr FT_Int32 LOAD_FLAGS = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL;

uint64_t hashBytes(uint64_t hash, const void* bytes, size_t count) {
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < count; ++i) hash = (hash ^ data[i]) * FNV_PRIME;
  return hash;
}
uint64_t hashNumber(uint64_t hash, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) {
    hash = (hash ^ static_cast<uint8_t>(value)) * FNV_PRIME;
    value >>= 8;
  }
  return hash;
}
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
TextStatus report(TextStatus status, const char* message) {
  if (status != TextStatus::Ok) nativeTextLogError(message);
  return status;
}

struct Axis {
  uint32_t tag = 0;
  int32_t value = 0;
};
struct Source {
  NativeBuffer<char> path;
  const uint8_t* bytes = nullptr;
  uint32_t size = 0;
  uint64_t identity = 0;
  Axis axes[8]{};
  uint16_t references = 0;
  uint8_t axisCount = 0;
  bool used = false;
  bool builtin = false;
};
struct StagedSources {
  Source entries[4];
};
struct Mapping {
  uint16_t sources[4] = {NO_SOURCE, NO_SOURCE, NO_SOURCE, NO_SOURCE};
  uint64_t fingerprint = 0;
  int fontId = 0;
  int uiFallbackFontId = 0;
  uint16_t points = 0;
  bool serif = false;
  bool used = false;
  bool builtin = false;
};
struct FaceRecord {
  FT_StreamRec stream{};
  FontStream* file = nullptr;
  FT_Face face = nullptr;
  hb_font_t* font = nullptr;
  uint64_t age = 0;
  uint32_t rasterSize26 = 0;
  uint32_t pins = 0;
  uint16_t source = NO_SOURCE;
  uint8_t syntheticStyle = 0;
  bool closeOnRelease = false;
};

void closeFace(FaceRecord& record) {
  // hb_ft_font_create_referenced owns a second reference to the same FT face.
  // FreeType's stream close callback deliberately does not close the backing
  // file: both references must disappear before the adapter/cache is destroyed.
  if (record.font) hb_font_destroy(record.font);
  if (record.face) FT_Done_Face(record.face);
  closeFontStream(record.file);
  record = {};
}
unsigned long streamRead(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count) {
  auto* file = static_cast<FontStream*>(stream->descriptor.pointer);
  if (!count) return offset <= fontStreamSize(file) ? 0 : 1;
  if (offset > fontStreamSize(file) || count > fontStreamSize(file) - offset) return 0;
  return static_cast<unsigned long>(readFontStream(file, static_cast<uint32_t>(offset), buffer, count));
}
void streamClose(FT_Stream) {}

TextStatus ftStatus(FT_Error error, const FaceRecord& record, size_t failures) {
  if (native_text::allocationStats().failures != failures || error == FT_Err_Out_Of_Memory)
    return TextStatus::OutOfMemory;
  if (fontStreamFailed(record.file)) return TextStatus::StorageError;
  return error ? TextStatus::InvalidFont : TextStatus::Ok;
}

bool readSource(const Source& source, FontStream* file, uint32_t offset, void* data, size_t count) {
  if (offset > source.size || count > source.size - offset) return false;
  if (source.bytes) {
    std::memcpy(data, source.bytes + offset, count);
    return true;
  }
  return readFontStream(file, offset, data, count) == count;
}

TextStatus validateSfnt(const Source& source, FontStream* file) {
  uint8_t header[12];
  if (!readSource(source, file, 0, header, sizeof(header)))
    return fontStreamFailed(file) ? TextStatus::StorageError : TextStatus::InvalidFont;
  const uint32_t signature = be32(header);
  if (signature != 0x00010000 && signature != HB_TAG('O', 'T', 'T', 'O') && signature != HB_TAG('t', 'r', 'u', 'e'))
    return TextStatus::InvalidFont;
  const uint16_t count = be16(header + 4);
  if (!count || count > (source.size - sizeof(header)) / 16) return TextStatus::InvalidFont;
  bool head = false, cmap = false, maxp = false, glyf = false, loca = false, cff = false;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t table[16];
    if (!readSource(source, file, 12 + i * 16, table, sizeof(table))) return TextStatus::StorageError;
    const uint32_t offset = be32(table + 8), length = be32(table + 12), tag = be32(table);
    if (offset > source.size || length > source.size - offset || (length && offset < 12u + uint32_t{count} * 16))
      return TextStatus::InvalidFont;
    if (tag == HB_TAG('h', 'e', 'a', 'd')) head = length >= 54;
    if (tag == HB_TAG('c', 'm', 'a', 'p')) cmap = length >= 4;
    if (tag == HB_TAG('m', 'a', 'x', 'p')) maxp = length >= 6;
    if (tag == HB_TAG('g', 'l', 'y', 'f')) glyf = length != 0;
    if (tag == HB_TAG('l', 'o', 'c', 'a')) loca = length != 0;
    if (tag == HB_TAG('C', 'F', 'F', ' ') || tag == HB_TAG('C', 'F', 'F', '2')) cff = length != 0;
  }
  return head && cmap && maxp && ((glyf && loca) || cff) ? TextStatus::Ok : TextStatus::InvalidFont;
}

TextStatus openFace(FT_Library library, const Source& source, FaceRecord& record) {
  TextStatus status;
  if (!source.bytes) {
    if (!record.file) {
      status = openFontStream(source.path.data(), record.file);
      if (status != TextStatus::Ok) return status;
    }
    if (fontStreamSize(record.file) != source.size) return TextStatus::InvalidFont;
  }
  status = validateSfnt(source, record.file);
  if (status != TextStatus::Ok) return status;
  const size_t failures = native_text::allocationStats().failures;
  FT_Error error;
  if (source.bytes) {
    if (source.size > static_cast<uint64_t>(std::numeric_limits<FT_Long>::max())) return TextStatus::CapacityExceeded;
    error = FT_New_Memory_Face(library, source.bytes, static_cast<FT_Long>(source.size), 0, &record.face);
  } else {
    record.stream.size = source.size;
    record.stream.descriptor.pointer = record.file;
    record.stream.read = streamRead;
    record.stream.close = streamClose;
    FT_Open_Args args{};
    args.flags = FT_OPEN_STREAM;
    args.stream = &record.stream;
    error = FT_Open_Face(library, &args, 0, &record.face);
  }
  status = ftStatus(error, record, failures);
  if (status != TextStatus::Ok) return status;
  const char* format = FT_Get_Font_Format(record.face);
  if (!FT_IS_SFNT(record.face) || !FT_IS_SCALABLE(record.face) || record.face->num_faces != 1 ||
      record.face->num_glyphs <= 0 || !format || (std::strcmp(format, "TrueType") && std::strcmp(format, "CFF")))
    return TextStatus::InvalidFont;
  return ftStatus(FT_Select_Charmap(record.face, FT_ENCODING_UNICODE), record, failures);
}

TextStatus configureAxes(FT_Library library, Source& source, FaceRecord& record,
                         std::span<const NativeVariation> requested, bool canonicalize) {
  const size_t count = canonicalize ? requested.size() : source.axisCount;
  if (!count) return TextStatus::Ok;
  if (count > 8 || !FT_HAS_MULTIPLE_MASTERS(record.face)) return TextStatus::InvalidFont;
  const size_t failures = native_text::allocationStats().failures;
  FT_MM_Var* variation = nullptr;
  FT_Error error = FT_Get_MM_Var(record.face, &variation);
  TextStatus status = ftStatus(error, record, failures);
  if (status != TextStatus::Ok) {
    if (variation) FT_Done_MM_Var(library, variation);
    return status;
  }
  NativeBuffer<FT_Fixed> coordinates;
  if (!coordinates.resize(variation->num_axis)) {
    FT_Done_MM_Var(library, variation);
    return TextStatus::OutOfMemory;
  }
  for (size_t i = 0; i < variation->num_axis; ++i) coordinates[i] = variation->axis[i].def;
  for (size_t i = 0; i < count && status == TextStatus::Ok; ++i) {
    const uint32_t tag = canonicalize ? requested[i].tag : source.axes[i].tag;
    for (size_t previous = 0; previous < i; ++previous) {
      if (source.axes[previous].tag == tag) status = TextStatus::InvalidFont;
    }
    bool found = false;
    for (size_t axis = 0; axis < variation->num_axis; ++axis) {
      if (variation->axis[axis].tag != tag) continue;
      found = true;
      double value = canonicalize ? static_cast<double>(requested[i].value) * 65536.0 : source.axes[i].value;
      if (!std::isfinite(value)) {
        status = TextStatus::InvalidFont;
        break;
      }
      value = std::clamp(value, static_cast<double>(variation->axis[axis].minimum),
                         static_cast<double>(variation->axis[axis].maximum));
      const auto fixed = static_cast<int32_t>(std::llround(value));
      coordinates[axis] = static_cast<FT_Fixed>(fixed);
      if (canonicalize) source.axes[i] = {tag, fixed};
      break;
    }
    if (!found) status = TextStatus::InvalidFont;
  }
  if (status == TextStatus::Ok) {
    error = FT_Set_Var_Design_Coordinates(record.face, variation->num_axis, coordinates.data());
    status = ftStatus(error, record, failures);
  }
  FT_Done_MM_Var(library, variation);
  if (status == TextStatus::Ok && canonicalize) {
    source.axisCount = static_cast<uint8_t>(count);
    std::sort(source.axes, source.axes + count, [](const Axis& a, const Axis& b) { return a.tag < b.tag; });
  }
  return status;
}

TextStatus finishFace(FaceRecord& record, uint32_t size26) {
  if (!size26 || size26 > static_cast<uint64_t>(std::numeric_limits<FT_F26Dot6>::max())) return TextStatus::InvalidFont;
  const size_t failures = native_text::allocationStats().failures;
  TextStatus status = ftStatus(FT_Set_Char_Size(record.face, 0, size26, 150, 150), record, failures);
  if (status != TextStatus::Ok) return status;
  record.font = hb_ft_font_create_referenced(record.face);
  if (!record.font || record.font == hb_font_get_empty()) return TextStatus::OutOfMemory;
  hb_ft_font_set_load_flags(record.font, LOAD_FLAGS);
  hb_ft_font_changed(record.font);
  record.rasterSize26 = size26;
  return ftStatus(0, record, failures);
}

bool fontExtension(std::string_view path) {
  if (path.size() < 4 || path[path.size() - 4] != '.') return false;
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
  const char a = lower(path[path.size() - 3]), b = lower(path[path.size() - 2]), c = lower(path.back());
  return (a == 't' && b == 't' && c == 'f') || (a == 'o' && b == 't' && c == 'f');
}

TextStatus prepareCustom(FT_Library library, const NativeFontFile& file, Source& source, bool requireExtension = true) {
  if ((requireExtension && !fontExtension(file.path)) || file.path.empty() ||
      file.path.find('\0') != std::string_view::npos || file.axes.size() > 8)
    return TextStatus::InvalidFont;
  if (file.path.size() > 1023) return TextStatus::CapacityExceeded;
  if (!source.path.resize(file.path.size() + 1)) return TextStatus::OutOfMemory;
  std::memcpy(source.path.data(), file.path.data(), file.path.size());
  source.path[file.path.size()] = '\0';
  FaceRecord face;
  TextStatus status = openFontStream(source.path.data(), face.file);
  if (status != TextStatus::Ok) return status;
  source.size = fontStreamSize(face.file);
  status = validateSfnt(source, face.file);
  uint64_t identity = FNV_OFFSET;
  if (status == TextStatus::Ok) status = native_text::detail::fontStreamFingerprint(face.file, identity);
  if (status == TextStatus::Ok) status = openFace(library, source, face);
  if (status == TextStatus::Ok) status = configureAxes(library, source, face, file.axes, true);
  if (status == TextStatus::Ok) status = finishFace(face, 12 * 64);
  closeFace(face);
  if (status != TextStatus::Ok) return status;
  // The default face identity is exactly the FNV digest of its bytes. Explicit
  // variation coordinates extend it using sorted tags and clamped 16.16 values.
  for (size_t i = 0; i < source.axisCount; ++i) {
    identity = hashNumber(identity, source.axes[i].tag, 4);
    identity = hashNumber(identity, static_cast<uint32_t>(source.axes[i].value), 4);
  }
  source.identity = identity;
  source.used = true;
  return TextStatus::Ok;
}

constexpr const char* ASSET_NAMES[BUILTIN_COUNT] = {
    "NotoSans-Regular",       "NotoSans-Bold",       "NotoSans-Italic",           "NotoSans-BoldItalic",
    "NotoSerif-Regular",      "NotoSerif-Bold",      "NotoSerif-Italic",          "NotoSerif-BoldItalic",
    "Ubuntu-Regular",         "Ubuntu-Bold",         "Ubuntu-Vietnamese-Regular", "Ubuntu-Vietnamese-Bold",
    "NotoSansHebrew-Regular", "NotoSansHebrew-Bold", "NotoSansArabic-Regular",    "NotoSansArabic-Bold",
    "NotoSansThai-Regular",   "NotoSansThai-Bold",   "NotoSerifThai-Regular",     "NotoSerifThai-Bold"};
}  // namespace

struct NativeFontRegistry::State {
  FT_Library library = nullptr;
  Source sources[SOURCE_LIMIT];
  Mapping mappings[MAPPING_LIMIT];
  FaceRecord faces[FACE_LIMIT];
  uint64_t age = 0;
  bool builtins = false;

  Mapping* mapping(int id) {
    for (auto& entry : mappings)
      if (entry.used && entry.fontId == id) return &entry;
    return nullptr;
  }
  bool pinned(uint16_t source) const {
    for (const auto& face : faces)
      if (face.source == source && face.pins) return true;
    return false;
  }
  void closeSource(uint16_t source) {
    for (auto& face : faces)
      if (face.source == source && !face.pins) closeFace(face);
  }
  void retireSource(uint16_t index) {
    auto& source = sources[index];
    if (source.builtin || source.references) return;
    closeSource(index);
    if (!pinned(index)) source = Source{};
  }
  uint64_t familyFingerprint(const Mapping& entry) const {
    uint64_t hash = hashBytes(FNV_OFFSET, "native-font-registry-v1", 23);
    hash = hashNumber(hash, entry.points, 2);
    hash = hashNumber(hash, entry.serif, 1);
    for (size_t style = 0; style < 4; ++style) {
      const auto index = entry.sources[style];
      hash = hashNumber(hash, index == NO_SOURCE ? 0 : sources[index].identity, 8);
      hash = hashNumber(hash, index == NO_SOURCE ? style : 0, 1);
    }
    if (entry.uiFallbackFontId) {
      for (const auto& fallback : mappings) {
        if (!fallback.used || fallback.fontId != entry.uiFallbackFontId) continue;
        for (auto index : fallback.sources)
          hash = hashNumber(hash, index == NO_SOURCE ? 0 : sources[index].identity, 8);
        break;
      }
    }
    // Every possible fallback is fingerprinted once, not only faces encountered
    // by the current paragraph. A fallback asset change invalidates old pages.
    for (size_t i = 0; i < BUILTIN_COUNT; ++i) hash = hashNumber(hash, sources[i].identity, 8);
    return hash;
  }
  void refreshUiFingerprints(int customFontId) {
    for (auto& entry : mappings)
      if (entry.used && entry.uiFallbackFontId == customFontId) entry.fingerprint = familyFingerprint(entry);
  }
};

NativeFontRegistry::~NativeFontRegistry() { shutdown(); }

TextStatus NativeFontRegistry::initialize(FT_Library library) {
  if (!library) return report(TextStatus::InvalidFont, "No FreeType library for native fonts");
  if (state_)
    return state_->library == library ? TextStatus::Ok
                                      : report(TextStatus::InvalidFont, "Native font registry already initialized");
  void* memory = native_text_malloc(sizeof(State));
  if (!memory) return report(TextStatus::OutOfMemory, "Native font registry allocation failed");
  state_ = new (memory) State;
  state_->library = library;
  return TextStatus::Ok;
}

TextStatus NativeFontRegistry::registerBuiltins() {
  if (!state_) return report(TextStatus::InvalidFont, "Native font registry is not initialized");
  if (state_->builtins) return TextStatus::Ok;
  if (native_text::assets::fontCount != BUILTIN_COUNT)
    return report(TextStatus::InvalidFont, "Native builtin font catalogue is incomplete");
  for (size_t i = 0; i < BUILTIN_COUNT; ++i) {
    const native_text::assets::FontAsset* asset = nullptr;
    for (size_t j = 0; j < native_text::assets::fontCount; ++j) {
      if (!std::strcmp(ASSET_NAMES[i], native_text::assets::fonts[j].name)) asset = &native_text::assets::fonts[j];
    }
    if (!asset || asset->size > std::numeric_limits<uint32_t>::max())
      return report(TextStatus::InvalidFont, "Native builtin font asset is missing");
    auto& source = state_->sources[i];
    source.bytes = asset->data;
    source.size = static_cast<uint32_t>(asset->size);
    source.identity = asset->fingerprint;
    source.used = source.builtin = true;
    FaceRecord face;
    TextStatus status = openFace(state_->library, source, face);
    if (status == TextStatus::Ok) status = finishFace(face, 12 * 64);
    closeFace(face);
    if (status != TextStatus::Ok) return report(status, "Native builtin font validation failed");
  }
  struct BuiltinMapping {
    int id;
    uint16_t points, first;
    bool serif, fullStyles;
  };
  constexpr BuiltinMapping mappings[] = {
      {NOTOSANS_12_FONT_ID, 12, 0, false, true}, {NOTOSANS_14_FONT_ID, 14, 0, false, true},
      {NOTOSANS_16_FONT_ID, 16, 0, false, true}, {NOTOSANS_18_FONT_ID, 18, 0, false, true},
      {NOTOSERIF_12_FONT_ID, 12, 4, true, true}, {NOTOSERIF_14_FONT_ID, 14, 4, true, true},
      {NOTOSERIF_16_FONT_ID, 16, 4, true, true}, {NOTOSERIF_18_FONT_ID, 18, 4, true, true},
      {UI_10_FONT_ID, 10, 8, false, false},      {UI_12_FONT_ID, 12, 8, false, false},
      {SMALL_FONT_ID, 8, 0, false, true}};
  for (size_t i = 0; i < std::size(mappings); ++i) {
    const auto& builtin = mappings[i];
    auto& entry = state_->mappings[i];
    entry.fontId = builtin.id;
    entry.points = builtin.points;
    entry.serif = builtin.serif;
    entry.used = entry.builtin = true;
    entry.sources[0] = builtin.first;
    entry.sources[1] = builtin.first + 1;
    if (builtin.fullStyles) {
      entry.sources[2] = builtin.first + 2;
      entry.sources[3] = builtin.first + 3;
    }
    entry.fingerprint = state_->familyFingerprint(entry);
  }
  state_->builtins = true;
  return TextStatus::Ok;
}

TextStatus NativeFontRegistry::registerCustomFont(int fontId, uint16_t pointSize, std::span<const NativeFontFile> files,
                                                  bool serif) {
  if (!state_ || !state_->builtins || !fontId || !pointSize || files.empty() || files.size() > 4)
    return report(TextStatus::InvalidFont, "Invalid native font family registration");
  Mapping* previous = state_->mapping(fontId);
  if (previous && previous->builtin) return report(TextStatus::InvalidFont, "Cannot replace a builtin font ID");
  Mapping* target = previous;
  if (!target)
    for (auto& mapping : state_->mappings)
      if (!mapping.used) {
        target = &mapping;
        break;
      }
  if (!target) return report(TextStatus::CapacityExceeded, "Native font mapping capacity exceeded");
  // Four style records exceed the task-stack budget; the transaction is cold-path storage.
  void* staging = native_text_malloc(sizeof(StagedSources));
  if (!staging) return report(TextStatus::OutOfMemory, "Native font transaction allocation failed");
  auto destroyStaging = [](StagedSources* value) {
    value->~StagedSources();
    native_text_free(value);
  };
  std::unique_ptr<StagedSources, decltype(destroyStaging)> transaction(new (staging) StagedSources, destroyStaging);
  auto& staged = transaction->entries;
  uint8_t styles = 0;
  for (const auto& file : files) {
    if (file.style > 3 || (styles & (1 << file.style)))
      return report(TextStatus::InvalidFont, "Duplicate or invalid native font style");
    styles |= 1 << file.style;
    const TextStatus status = prepareCustom(state_->library, file, staged[file.style]);
    if (status != TextStatus::Ok) return report(status, "Native font family validation failed");
  }
  if (!(styles & 1)) return report(TextStatus::InvalidFont, "Native font family requires Regular");

  uint16_t selected[4] = {NO_SOURCE, NO_SOURCE, NO_SOURCE, NO_SOURCE};
  for (size_t style = 0; style < 4; ++style) {
    if (!staged[style].used) continue;
    for (uint16_t i = 0; i < SOURCE_LIMIT; ++i) {
      if (state_->sources[i].used && state_->sources[i].identity == staged[style].identity) {
        selected[style] = i;
        break;
      }
    }
    for (size_t earlier = 0; earlier < style; ++earlier) {
      if (staged[earlier].used && staged[earlier].identity == staged[style].identity)
        selected[style] = selected[earlier];
    }
  }
  for (size_t style = 0; style < 4; ++style) {
    if (!staged[style].used || selected[style] != NO_SOURCE) continue;
    for (uint16_t i = BUILTIN_COUNT; i < SOURCE_LIMIT; ++i) {
      bool chosen = false;
      for (auto index : selected)
        if (index == i) chosen = true;
      if (chosen) continue;
      const auto& source = state_->sources[i];
      bool reusable = !source.used;
      if (!reusable && previous && !state_->pinned(i)) {
        size_t oldReferences = 0;
        for (auto old : previous->sources)
          if (old == i) ++oldReferences;
        reusable = oldReferences && source.references == oldReferences;
      }
      if (reusable) {
        selected[style] = i;
        break;
      }
    }
    if (selected[style] == NO_SOURCE)
      return report(TextStatus::CapacityExceeded, "Native font source capacity exceeded");
    for (size_t later = style + 1; later < 4; ++later) {
      if (staged[later].used && staged[later].identity == staged[style].identity) selected[later] = selected[style];
    }
  }

  // Every fallible operation has completed. Publish all styles atomically under
  // the engine mutex, retaining old source records while any handles pin them.
  uint16_t oldSources[4] = {NO_SOURCE, NO_SOURCE, NO_SOURCE, NO_SOURCE};
  if (previous) {
    std::copy(std::begin(previous->sources), std::end(previous->sources), oldSources);
    for (auto index : oldSources)
      if (index != NO_SOURCE) --state_->sources[index].references;
  }
  for (size_t style = 0; style < 4; ++style) {
    const uint16_t index = selected[style];
    if (index == NO_SOURCE) continue;
    auto& source = state_->sources[index];
    if (!source.used || source.identity != staged[style].identity) {
      state_->closeSource(index);
      source = std::move(staged[style]);
    }
    ++source.references;
  }
  *target = Mapping{};
  std::copy(std::begin(selected), std::end(selected), target->sources);
  target->fontId = fontId;
  target->points = pointSize;
  target->serif = serif;
  target->used = true;
  target->fingerprint = state_->familyFingerprint(*target);
  state_->refreshUiFingerprints(fontId);
  for (auto index : oldSources)
    if (index != NO_SOURCE) state_->retireSource(index);
  return TextStatus::Ok;
}

TextStatus NativeFontRegistry::registerFontAlias(int fontId, int sourceFontId, uint16_t pointSize) {
  if (!state_ || !state_->builtins || !fontId || !pointSize || fontId == sourceFontId)
    return report(TextStatus::InvalidFont, "Invalid native font alias");
  const Mapping* source = state_->mapping(sourceFontId);
  if (!source || source->builtin) return report(TextStatus::InvalidFont, "Alias source is not a custom family");
  Mapping* target = state_->mapping(fontId);
  if (target && target->builtin) return report(TextStatus::InvalidFont, "Cannot replace a builtin font ID");
  if (!target)
    for (auto& entry : state_->mappings)
      if (!entry.used) {
        target = &entry;
        break;
      }
  if (!target) return report(TextStatus::CapacityExceeded, "Native font mapping capacity exceeded");
  const Mapping replacement = *source;
  uint16_t oldSources[4] = {NO_SOURCE, NO_SOURCE, NO_SOURCE, NO_SOURCE};
  if (target->used) {
    std::copy(std::begin(target->sources), std::end(target->sources), oldSources);
    for (auto index : oldSources)
      if (index != NO_SOURCE) --state_->sources[index].references;
  }
  *target = replacement;
  target->fontId = fontId;
  target->points = pointSize;
  for (auto index : target->sources)
    if (index != NO_SOURCE) ++state_->sources[index].references;
  target->fingerprint = state_->familyFingerprint(*target);
  state_->refreshUiFingerprints(fontId);
  for (auto index : oldSources)
    if (index != NO_SOURCE) state_->retireSource(index);
  return TextStatus::Ok;
}

TextStatus NativeFontRegistry::validateFontFile(std::string_view path, std::span<const NativeVariation> axes) {
  if (!state_ || !state_->builtins) return report(TextStatus::InvalidFont, "Native registry is not initialized");
  Source source;
  return report(prepareCustom(state_->library, {path, 0, axes}, source, false), "Native font validation failed");
}

TextStatus NativeFontRegistry::setUiFallback(int customFontId) {
  if (!state_ || !state_->builtins) return TextStatus::InvalidFont;
  const Mapping* custom = customFontId ? state_->mapping(customFontId) : nullptr;
  if (customFontId && (!custom || custom->builtin)) return TextStatus::InvalidFont;
  for (int id : {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID}) {
    auto* mapping = state_->mapping(id);
    mapping->uiFallbackFontId = customFontId;
    mapping->fingerprint = state_->familyFingerprint(*mapping);
  }
  return TextStatus::Ok;
}

void NativeFontRegistry::clearCustomFonts() {
  if (!state_) return;
  setUiFallback(0);
  releaseSdFaces();
  for (auto& mapping : state_->mappings) {
    if (!mapping.used || mapping.builtin) continue;
    for (auto index : mapping.sources)
      if (index != NO_SOURCE) --state_->sources[index].references;
    mapping = Mapping{};
  }
  for (uint16_t index = BUILTIN_COUNT; index < SOURCE_LIMIT; ++index) state_->retireSource(index);
}

TextStatus NativeFontRegistry::candidates(int fontId, uint8_t style, uint32_t script,
                                          std::span<NativeFaceChoice> output, size_t& count) {
  count = 0;
  const Mapping* mapping = state_ && state_->builtins ? state_->mapping(fontId) : nullptr;
  if (!mapping) return report(TextStatus::InvalidFont, "Unknown native logical font ID");
  NativeFaceChoice choices[8];
  size_t total = 0;
  const uint8_t variant = style & 3;
  const uint32_t size26 = uint32_t{mapping->points} * ((style & (16 | 32)) ? 32 : 64);
  const auto add = [&](uint16_t index, uint8_t synthetic) {
    if (index == NO_SOURCE) return;
    const NativeFaceChoice choice{state_->sources[index].identity, size26, synthetic};
    for (size_t i = 0; i < total; ++i)
      if (choices[i].identity == choice.identity && choices[i].syntheticStyle == synthetic) return;
    if (total < std::size(choices)) choices[total++] = choice;
  };
  uint16_t primary = mapping->sources[variant];
  uint8_t synthesis = 0;
  if (primary == NO_SOURCE) {
    // Native custom styles are synthesized from Regular, never an unrelated
    // installed variant. Bundled two-style families can retain their true bold.
    if (mapping->builtin && (variant & 1) && mapping->sources[1] != NO_SOURCE) {
      primary = mapping->sources[1];
      synthesis = variant & 2;
    } else {
      primary = mapping->sources[0];
      synthesis = variant;
    }
  }
  add(primary, synthesis);
  if (mapping->uiFallbackFontId) {
    const auto* fallback = state_->mapping(mapping->uiFallbackFontId);
    if (fallback) {
      const bool actualStyle = fallback->sources[variant] != NO_SOURCE;
      add(fallback->sources[actualStyle ? variant : 0], actualStyle ? 0 : variant);
    }
  }
  const uint16_t thai = (mapping->serif ? 18 : 16) + (variant & 1);
  const uint16_t hebrew = 12 + (variant & 1), arabic = 14 + (variant & 1);
  if (script == HB_SCRIPT_THAI) add(thai, variant & 2);
  if (script == HB_SCRIPT_HEBREW) add(hebrew, variant & 2);
  if (script == HB_SCRIPT_ARABIC) add(arabic, variant & 2);
  add((mapping->serif ? 4 : 0) + variant, 0);
  add(thai, variant & 2);
  add(arabic, variant & 2);
  add(hebrew, variant & 2);
  add(10 + (variant & 1), variant & 2);
  if (output.size() < total) return report(TextStatus::CapacityExceeded, "Native font candidate buffer is too small");
  std::copy_n(choices, total, output.data());
  count = total;
  return TextStatus::Ok;
}

TextStatus NativeFontRegistry::acquire(const NativeFaceChoice& choice, NativeFaceHandle& handle) {
  handle = {};
  if (!state_ || !state_->builtins || !choice.rasterSize26 || (choice.syntheticStyle & ~3u))
    return report(TextStatus::InvalidFont, "Invalid native face request");
  uint16_t source = NO_SOURCE;
  for (uint16_t i = 0; i < SOURCE_LIMIT; ++i)
    if (state_->sources[i].used && state_->sources[i].identity == choice.identity) {
      source = i;
      break;
    }
  if (source == NO_SOURCE) return report(TextStatus::InvalidFont, "Native glyph refers to an unregistered face");
  size_t selected = FACE_LIMIT;
  for (size_t i = 0; i < FACE_LIMIT; ++i) {
    auto& face = state_->faces[i];
    if (face.face && face.source == source && face.rasterSize26 == choice.rasterSize26 &&
        face.syntheticStyle == choice.syntheticStyle && !face.closeOnRelease) {
      if (fontStreamFailed(face.file)) {
        if (!face.pins) closeFace(face);
        return report(TextStatus::StorageError, "Native font stream previously failed");
      }
      if (face.pins == std::numeric_limits<uint32_t>::max()) return TextStatus::CapacityExceeded;
      ++face.pins;
      face.age = ++state_->age;
      handle = {face.face, face.font, static_cast<uint8_t>(i)};
      return TextStatus::Ok;
    }
    if (!face.pins && (selected == FACE_LIMIT || !face.face ||
                       (state_->faces[selected].face && face.age < state_->faces[selected].age)))
      selected = i;
  }
  if (selected == FACE_LIMIT) return report(TextStatus::CapacityExceeded, "All native font faces are pinned");
  auto& record = state_->faces[selected];
  closeFace(record);
  TextStatus status = openFace(state_->library, state_->sources[source], record);
  if (status == TextStatus::Ok) status = configureAxes(state_->library, state_->sources[source], record, {}, false);
  if (status == TextStatus::Ok) status = finishFace(record, choice.rasterSize26);
  if (status != TextStatus::Ok) {
    closeFace(record);
    return report(status, "Native font face could not be opened");
  }
  record.source = source;
  record.syntheticStyle = choice.syntheticStyle;
  record.age = ++state_->age;
  record.pins = 1;
  handle = {record.face, record.font, static_cast<uint8_t>(selected)};
  return TextStatus::Ok;
}

TextStatus NativeFontRegistry::acquireGlyph(const NativeGlyph& glyph, NativeFaceHandle& handle) {
  return acquire({glyph.faceIdentity, glyph.rasterSize26, glyph.syntheticStyle}, handle);
}
TextStatus NativeFontRegistry::faceStatus(const NativeFaceHandle& handle) const {
  if (!state_ || handle.slot >= FACE_LIMIT) return TextStatus::InvalidFont;
  const auto& face = state_->faces[handle.slot];
  if (!face.face || face.face != handle.face || face.font != handle.font || !face.pins) return TextStatus::InvalidFont;
  return fontStreamFailed(face.file) ? TextStatus::StorageError : TextStatus::Ok;
}
void NativeFontRegistry::release(const NativeFaceHandle& handle) {
  if (!state_ || handle.slot >= FACE_LIMIT) return;
  auto& face = state_->faces[handle.slot];
  if (face.face != handle.face || face.font != handle.font || !face.pins) return;
  --face.pins;
  const uint16_t source = face.source;
  if (!face.pins && face.closeOnRelease) closeFace(face);
  if (source != NO_SOURCE && !state_->sources[source].references && !state_->sources[source].builtin)
    state_->retireSource(source);
}
void NativeFontRegistry::releaseSdFaces() {
  if (!state_) return;
  for (auto& face : state_->faces) {
    if (!face.file) continue;
    if (face.pins)
      face.closeOnRelease = true;
    else
      closeFace(face);
  }
}
void NativeFontRegistry::evictUnusedFaces() {
  if (!state_) return;
  for (auto& face : state_->faces)
    if (!face.pins) closeFace(face);
}
void NativeFontRegistry::shutdown() {
  if (!state_) return;
  for (auto& face : state_->faces) closeFace(face);
  state_->~State();
  native_text_free(state_);
  state_ = nullptr;
}
uint64_t NativeFontRegistry::fingerprint(int fontId) const {
  const auto* mapping = state_ && state_->builtins ? state_->mapping(fontId) : nullptr;
  return mapping ? mapping->fingerprint : 0;
}
bool NativeFontRegistry::hasFont(int fontId) const { return state_ && state_->builtins && state_->mapping(fontId); }
