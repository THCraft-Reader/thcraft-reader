#include <BidiUtils.h>
#include <Epub/Page.h>
#include <Epub/ParsedText.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FontPsram.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <SdCardFont.h>
#include <ThaiCluster.h>
#include <ThaiDictionary.h>
#include <ThaiStats.h>
#include <expat.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "HostAllocation.h"

namespace {
constexpr int FONT_ID = 1;
struct Options {
  std::string font, xhtml, output, orientation = "portrait", glyphSheet;
  std::string alignment = "justify";
  int width = 480, height = 800, characterSpacing = 0, wordSpacing = 100;
  int thaiSpaceWeight = 4;
  float compression = 1.0f;
  bool hyphenation = false, focus = false;
  bool failThaiAllocation = false;
  bool failShapeAllocation = false;
  size_t shapeInternalFree = 256u * 1024u * 1024u;
  std::string shapeFailReadPhase = "none";
  bool cacheRoundtrip = false, verifyThaiPlacement = false;
};
std::string json(const std::string& text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : text) {
    switch (c) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (c < 32)
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else
          out << static_cast<char>(c);
    }
  }
  return out.str() + '"';
}
int integer(const std::string& value) {
  size_t end = 0;
  const int result = std::stoi(value, &end);
  if (end != value.size()) throw std::runtime_error("Invalid integer: " + value);
  return result;
}
size_t byteCount(const std::string& value) {
  if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("Invalid byte count: " + value);
  const auto result = std::stoull(value);
  if (result > std::numeric_limits<size_t>::max()) throw std::runtime_error("Byte count exceeds size_t");
  return static_cast<size_t>(result);
}
bool toggle(const std::string& value) {
  if (value == "on") return true;
  if (value == "off") return false;
  throw std::runtime_error("Expected on|off: " + value);
}
uint8_t alignmentValue(const std::string& name) {
  static constexpr const char* names[] = {"justify", "left", "center", "right", "book", "thai-justify"};
  for (uint8_t i = 0; i < std::size(names); ++i) {
    if (name == names[i]) return i;
  }
  throw std::runtime_error("Invalid alignment: " + name);
}
Options options(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--help") {
      std::cout << "ThaiRenderProbe --font PATH --xhtml PATH --width N --height N --orientation "
                   "portrait|inverted|cw|ccw --alignment justify|left|center|right|book|thai-justify "
                   "--character-spacing N --word-spacing-percent N --line-compression F --hyphenation on|off --focus "
                   "on|off --output DIR [--cache-roundtrip on|off] [--verify-thai-placement on|off] [--glyph-sheet "
                   "normal|rotated90cw] [--fail-thai-allocation on|off] [--fail-shape-allocation on|off] "
                   "[--shape-internal-free BYTES] [--shape-fail-read-phase none|layout|bw|gray-msb|gray-lsb] "
                   "[--thai-space-weight 2|4|6]\n";
      std::exit(0);
    }
    if (++i == argc) throw std::runtime_error("Missing value for " + key);
    const std::string value = argv[i];
    if (key == "--font")
      o.font = value;
    else if (key == "--xhtml")
      o.xhtml = value;
    else if (key == "--output")
      o.output = value;
    else if (key == "--width")
      o.width = integer(value);
    else if (key == "--height")
      o.height = integer(value);
    else if (key == "--orientation")
      o.orientation = value;
    else if (key == "--alignment")
      o.alignment = value;
    else if (key == "--cache-roundtrip")
      o.cacheRoundtrip = toggle(value);
    else if (key == "--verify-thai-placement")
      o.verifyThaiPlacement = toggle(value);
    else if (key == "--glyph-sheet")
      o.glyphSheet = value;
    else if (key == "--character-spacing")
      o.characterSpacing = integer(value);
    else if (key == "--word-spacing-percent")
      o.wordSpacing = integer(value);
    else if (key == "--thai-space-weight")
      o.thaiSpaceWeight = integer(value);
    else if (key == "--line-compression") {
      size_t end = 0;
      o.compression = std::stof(value, &end);
      if (end != value.size()) throw std::runtime_error("Invalid compression");
    } else if (key == "--hyphenation")
      o.hyphenation = toggle(value);
    else if (key == "--focus")
      o.focus = toggle(value);
    else if (key == "--fail-thai-allocation")
      o.failThaiAllocation = toggle(value);
    else if (key == "--fail-shape-allocation")
      o.failShapeAllocation = toggle(value);
    else if (key == "--shape-internal-free")
      o.shapeInternalFree = byteCount(value);
    else if (key == "--shape-fail-read-phase")
      o.shapeFailReadPhase = value;
    else
      throw std::runtime_error("Unknown option " + key);
  }
  if (o.font.empty() || o.xhtml.empty() || o.output.empty())
    throw std::runtime_error("--font, --xhtml and --output are required");
  if (o.font.size() >= 128)
    throw std::runtime_error("CPFont path exceeds production loader's 127-byte limit; use a relative path");
  if (o.width < 1 || o.height < 1 || o.width > 8192 || o.height > 8192)
    throw std::runtime_error("Viewport must be within 1..8192 pixels");
  if (o.characterSpacing < -128 || o.characterSpacing > 127 || o.wordSpacing < 0 || o.wordSpacing > 255)
    throw std::runtime_error("Spacing exceeds production setting storage");
  if (o.thaiSpaceWeight != 2 && o.thaiSpaceWeight != 4 && o.thaiSpaceWeight != 6)
    throw std::runtime_error("Thai space calibration weight must be 2, 4, or 6");
  if (!std::isfinite(o.compression) || o.compression <= 0 || o.compression > 4)
    throw std::runtime_error("Invalid line compression");
  if (o.orientation != "portrait" && o.orientation != "inverted" && o.orientation != "cw" && o.orientation != "ccw")
    throw std::runtime_error("Invalid orientation");
  if (!o.glyphSheet.empty() && o.glyphSheet != "normal" && o.glyphSheet != "rotated90cw")
    throw std::runtime_error("Invalid glyph-sheet mode");
  if (o.shapeFailReadPhase != "none" && o.shapeFailReadPhase != "layout" && o.shapeFailReadPhase != "bw" &&
      o.shapeFailReadPhase != "gray-msb" && o.shapeFailReadPhase != "gray-lsb")
    throw std::runtime_error("Invalid shape read failure phase");
  alignmentValue(o.alignment);
  return o;
}
GfxRenderer::Orientation orientation(const Options& o) {
  if (o.orientation == "portrait") return GfxRenderer::Portrait;
  if (o.orientation == "inverted") return GfxRenderer::PortraitInverted;
  if (o.orientation == "cw") return GfxRenderer::LandscapeClockwise;
  return GfxRenderer::LandscapeCounterClockwise;
}
std::pair<int, int> logical(const GfxRenderer& r, int x, int y) {
  switch (r.getOrientation()) {
    case GfxRenderer::Portrait:
      return {r.getDisplayHeight() - 1 - y, x};
    case GfxRenderer::PortraitInverted:
      return {y, r.getDisplayWidth() - 1 - x};
    case GfxRenderer::LandscapeClockwise:
      return {r.getDisplayWidth() - 1 - x, r.getDisplayHeight() - 1 - y};
    default:
      return {x, y};
  }
}
std::array<int, 4> inkBounds(const GfxRenderer& r) {
  int left = r.getScreenWidth(), top = r.getScreenHeight(), right = -1, bottom = -1;
  const auto* pixels = r.getFrameBuffer();
  for (int py = 0; py < r.getDisplayHeight(); ++py) {
    for (int byte = 0; byte < r.getDisplayWidthBytes(); ++byte) {
      const uint8_t ink = static_cast<uint8_t>(~pixels[py * r.getDisplayWidthBytes() + byte]);
      if (!ink) continue;
      for (int bit = 0; bit < 8 && byte * 8 + bit < r.getDisplayWidth(); ++bit) {
        if (!(ink & (0x80 >> bit))) continue;
        const auto [x, y] = logical(r, byte * 8 + bit, py);
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
      }
    }
  }
  if (right < left) return {0, 0, 0, 0};
  return {left, top, right - left + 1, bottom - top + 1};
}
void boundsJson(std::ostream& out, const std::array<int, 4>& b) {
  out << '[' << b[0] << ',' << b[1] << ',' << b[2] << ',' << b[3] << ']';
}
uint64_t scalarCount(std::string_view text) {
  uint64_t n = 0;
  for (unsigned char c : text) n += (c & 0xc0) != 0x80;
  return n;
}
void clustersJson(std::ostream& out, std::string_view text) {
  out << '[';
  size_t offset = 0, scalar = 0;
  thai::Cluster cluster{};
  while (thai::nextCluster(text, offset, cluster, true)) {
    if (cluster.begin) out << ',';
    out << "{\"begin\":" << scalar << ",\"end\":" << scalar + cluster.codepoints << ",\"byte_begin\":" << cluster.begin
        << ",\"byte_end\":" << cluster.end << ",\"valid\":" << (cluster.valid ? "true" : "false") << ",\"thai\":"
        << (thai::containsThai(text.substr(cluster.begin, cluster.end - cluster.begin)) ? "true" : "false") << '}';
    scalar += cluster.codepoints;
  }
  out << ']';
}
struct ObservedGlyph {
  size_t source;
  uint32_t codepoint;
  int x, y;
};
void observeGlyph(void* context, size_t source, uint32_t codepoint, int x, int y) {
  static_cast<std::vector<ObservedGlyph>*>(context)->push_back({source, codepoint, x, y});
}
void verifyPlacement(GfxRenderer& renderer, int fontId, const char* text, const std::vector<size_t>& slots,
                     EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  constexpr uint16_t budget = 23;
  const auto count = renderer.countThaiJustificationGaps(fontId, text, style);
  if (count != slots.size()) throw std::runtime_error("Unexpected surviving Thai gap count");
  for (bool rotated : {false, true}) {
    std::vector<ObservedGlyph> natural, expanded, repeat;
    natural.reserve(128);
    expanded.reserve(128);
    repeat.reserve(128);
    auto draw = [&](std::vector<ObservedGlyph>& output, uint16_t extra) {
      renderer.setGlyphPlacementObserver(&output, observeGlyph);
      if (rotated)
        renderer.drawTextRotated90CW(fontId, 80, renderer.getScreenHeight() - 16, text, true, style, extra);
      else
        renderer.drawText(fontId, 16, 80, text, true, style, BidiUtils::BidiBaseDir::AUTO, 0, extra);
      renderer.setGlyphPlacementObserver(nullptr, nullptr);
    };
    draw(natural, 0);
    draw(expanded, budget);
    draw(repeat, 0);
    if (natural.empty() || natural.size() != expanded.size() || natural.size() != repeat.size()) {
      throw std::runtime_error("Thai expansion changed emitted glyph stream");
    }
    for (size_t i = 0; i < natural.size(); ++i) {
      const auto& a = natural[i];
      const auto& b = expanded[i];
      const auto& c = repeat[i];
      const size_t preceding = std::upper_bound(slots.begin(), slots.end(), a.source) - slots.begin();
      const int shift = count ? (budget * preceding + count / 2) / count : 0;
      if (a.source != b.source || a.codepoint != b.codepoint || b.x != a.x + (rotated ? 0 : shift) ||
          b.y != a.y - (rotated ? shift : 0) || a.source != c.source || a.codepoint != c.codepoint || a.x != c.x ||
          a.y != c.y) {
        throw std::runtime_error("Thai glyph/mark placement is not a rigid cluster translation");
      }
    }
  }
  const int natural = renderer.getTextAdvanceX(fontId, text, style, 0, BidiUtils::BidiBaseDir::AUTO,
                                               GfxRenderer::TextMeasureMode::Rendered);
  const int expanded = renderer.getTextAdvanceX(fontId, text, style, 0, BidiUtils::BidiBaseDir::AUTO,
                                                GfxRenderer::TextMeasureMode::Rendered, budget);
  if (expanded != natural + (count ? budget : 0)) throw std::runtime_error("Thai expanded advance differs from budget");
}
void verifyMarkedPlacement(GfxRenderer& renderer, SdCardFont& font) {
  for (const char* text : {"กี่น้ำเพื่อญูฐุนํ้า", "ก่ี่ขกีีค", "ภาษาไทย", "ประเทศไทย", "เพื่อ", "เรื่อง"}) {
    const size_t length = strlen(text);
    if (!renderer.ensureSdCardFontReady(FONT_ID, &text, &length, 1, false, false, 1, true)) {
      throw std::runtime_error("Cannot prepare exact Thai metrics");
    }
    const int cold = renderer.getTextAdvanceX(FONT_ID, text, EpdFontFamily::REGULAR, 0, BidiUtils::BidiBaseDir::AUTO,
                                              GfxRenderer::TextMeasureMode::Rendered, 23);
    if (font.prewarm(text, 1, false, true, false) < 0)
      throw std::runtime_error("Cannot prewarm Thai verification text");
    const int warm = renderer.getTextAdvanceX(FONT_ID, text, EpdFontFamily::REGULAR, 0, BidiUtils::BidiBaseDir::AUTO,
                                              GfxRenderer::TextMeasureMode::Rendered, 23);
    if (cold != warm) throw std::runtime_error("Cold/prewarmed Thai metrics differ");
    std::vector<size_t> slots;
    slots.reserve(32);
    thai::JustificationBoundaryCursor cursor(text);
    size_t offset;
    while (cursor.next(offset)) slots.push_back(offset);
    for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::SUP, EpdFontFamily::SUB}) {
      verifyPlacement(renderer, FONT_ID, text, slots, style);
    }
  }
}

int observeAddedGap(GfxRenderer& renderer, const char* text, EpdFontFamily::Style style,
                    BidiUtils::BidiBaseDir direction, int tracking, uint16_t budget) {
  if (!budget) return 0;
  std::vector<ObservedGlyph> natural, expanded;
  natural.reserve(strlen(text));
  expanded.reserve(strlen(text));
  renderer.setGlyphPlacementObserver(&natural, observeGlyph);
  renderer.drawText(FONT_ID, 0, 0, text, true, style, direction, tracking);
  renderer.setGlyphPlacementObserver(&expanded, observeGlyph);
  renderer.drawText(FONT_ID, 0, 0, text, true, style, direction, tracking, budget);
  renderer.setGlyphPlacementObserver(nullptr, nullptr);
  if (natural.size() != expanded.size()) throw std::runtime_error("Distribution changed glyph count");
  const int limit = renderer.getThaiJustificationGapLimit(FONT_ID, text, style);
  int previousShift = 0, maximum = 0;
  size_t previousSource = 0;
  for (size_t i = 0; i < natural.size(); ++i) {
    const auto& a = natural[i];
    const auto& b = expanded[i];
    const int shift = b.x - a.x;
    if (a.source != b.source || a.codepoint != b.codepoint || a.y != b.y)
      throw std::runtime_error("Distribution changed glyph identity or vertical placement");
    if (i && a.source == previousSource && shift != previousShift)
      throw std::runtime_error("Distribution separated attached glyphs");
    const int extra = shift - previousShift;
    if (extra < 0 || extra > limit) {
      throw std::runtime_error("Observed Thai spacing exceeds its limit: text=" + std::string(text) +
                               " source=" + std::to_string(a.source) + " extra=" + std::to_string(extra) +
                               " limit=" + std::to_string(limit) + " budget=" + std::to_string(budget) +
                               " direction=" + std::to_string(static_cast<int>(direction)));
    }
    maximum = std::max(maximum, extra);
    previousShift = shift;
    previousSource = a.source;
  }
  if (previousShift != budget) throw std::runtime_error("Observed Thai expansion lost pixels");
  return maximum;
}
void verifyNativeLigatures(const Options& options) {
  // A native-only CPFont exercises the same cold/mini/full cache transitions as an SD font.
  const auto path = (std::filesystem::path(options.output) / "native-ligatures.cpfont").string();
  static constexpr EpdUnicodeInterval intervals[] = {
      {'A', 'C', 0}, {0xE01, 0xE04, 3}, {0xE35, 0xE35, 7}, {0xE48, 0xE48, 8}};
  static constexpr EpdLigaturePair pairs[] = {
      {('A' << 16) | 0xE04u, 'B'}, {(0xE01u << 16) | 0xE02u, 'A'}, {(0xE01u << 16) | 0xE35u, 'C'}};
  static constexpr EpdKernClassEntry left[] = {{'A', 1}, {'B', 1}, {'C', 1}};
  static constexpr EpdKernClassEntry right[] = {{0xE02, 1}, {0xE03, 1}, {0xE04, 1}};
  std::array<EpdGlyph, 9> glyphs{};
  for (auto& glyph : glyphs) glyph = {1, 1, 128, 0, 1, 1, 0};
  glyphs[7].advanceX = glyphs[8].advanceX = 0;
  std::array<uint8_t, 64> header{};
  memcpy(header.data(), "CPFONT", 6);
  const auto put16 = [&](size_t offset, uint16_t value) {
    header[offset] = value;
    header[offset + 1] = value >> 8;
  };
  const auto put32 = [&](size_t offset, uint32_t value) {
    put16(offset, value);
    put16(offset + 2, value >> 16);
  };
  put16(8, CPFONT_VERSION);
  header[12] = 1;
  put32(36, std::size(intervals));
  put32(40, glyphs.size());
  header[44] = 12;
  put16(45, 8);
  put16(47, 2);
  put16(49, std::size(left));
  put16(51, std::size(right));
  header[53] = header[54] = 1;
  header[55] = std::size(pairs);
  put32(56, header.size());
  {
    HalFile file;
    if (!Storage.openFileForWrite("PROBE", path, file)) throw std::runtime_error("Cannot write synthetic CPFont");
    const auto put = [&](const void* bytes, size_t size) {
      if (file.write(bytes, size) != size) throw std::runtime_error("Synthetic CPFont short write");
    };
    put(header.data(), header.size());
    put(intervals, sizeof(intervals));
    put(glyphs.data(), sizeof(glyphs));
    put(left, sizeof(left));
    put(right, sizeof(right));
    const int8_t kern = -16;
    put(&kern, sizeof(kern));
    put(pairs, sizeof(pairs));
    const uint8_t bitmap = 0x80;
    put(&bitmap, sizeof(bitmap));
  }
  HalDisplay display(320, 320);
  GfxRenderer renderer(display);
  renderer.begin();
  SdCardFont font;
  if (!font.load(path.c_str())) throw std::runtime_error("Cannot load synthetic CPFont");
  renderer.insertFont(FONT_ID, EpdFontFamily(font.getEpdFont()));
  renderer.registerSdCardFont(FONT_ID, &font);
  FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
  renderer.setFontCacheManager(&cache);
  struct Case {
    const char* text;
    size_t edge;
    uint32_t output, right;
  };
  for (const auto& c : {Case{"กขฃ", 6, 'A', 0xE03}, Case{"กขคฃ", 9, 'B', 0xE03}, Case{"กีข", 6, 'C', 0xE02}}) {
    font.releaseResidentCaches();
    const size_t length = strlen(c.text);
    if (!renderer.ensureSdCardFontReady(FONT_ID, &c.text, &length, 1, false, false, 1, true)) {
      throw std::runtime_error("Cannot prepare synthetic native metrics");
    }
    const int cold = renderer.getTextAdvanceX(FONT_ID, c.text, EpdFontFamily::REGULAR, 0, BidiUtils::BidiBaseDir::AUTO,
                                              GfxRenderer::TextMeasureMode::Rendered);
    if (cold != 15 || font.getEpdFont()->getKerning(c.output, c.right) != -16) {
      throw std::runtime_error(std::string("Cold native ligature metrics differ for ") + c.text +
                               ": advance=" + std::to_string(cold) +
                               ", kern=" + std::to_string(font.getEpdFont()->getKerning(c.output, c.right)));
    }
    verifyPlacement(renderer, FONT_ID, c.text, {c.edge});
    {
      auto scan = cache.createPrewarmScope();
      renderer.drawText(FONT_ID, 0, 0, c.text);
      scan.endScanAndPrewarm();
    }
    const int warm = renderer.getTextAdvanceX(FONT_ID, c.text, EpdFontFamily::REGULAR, 0, BidiUtils::BidiBaseDir::AUTO,
                                              GfxRenderer::TextMeasureMode::Rendered);
    if (cold != warm) throw std::runtime_error("Prewarm discarded native ligature output kerning");
    verifyPlacement(renderer, FONT_ID, c.text, {c.edge});
    if (font.prewarm(c.text, 1, false, true, false) < 0) throw std::runtime_error("Synthetic source prewarm failed");
    verifyPlacement(renderer, FONT_ID, c.text, {c.edge});
    font.releaseResidentCaches();
    {
      auto scan = cache.createPrewarmScope();
      renderer.drawText(FONT_ID, 0, 0, c.text);
      scan.endScanAndPrewarm();
    }
    if (renderer.getTextAdvanceX(FONT_ID, c.text, EpdFontFamily::REGULAR, 0, BidiUtils::BidiBaseDir::AUTO,
                                 GfxRenderer::TextMeasureMode::Rendered) != cold) {
      throw std::runtime_error("Cold cached-page prewarm lost native ligature output kerning");
    }
    verifyPlacement(renderer, FONT_ID, c.text, {c.edge});
  }
  verifyPlacement(renderer, FONT_ID, "กข", {});
  font.releaseResidentCaches();
  probe::failFontAllocationBytes = sizeof(left);
  const char* failureText = "กขคฃ";
  const size_t failureLength = strlen(failureText);
  if (renderer.ensureSdCardFontReady(FONT_ID, &failureText, &failureLength, 1, false, false, 1, true) ||
      probe::failFontAllocationBytes != 0) {
    throw std::runtime_error("Exact Thai preparation did not report injected allocation failure");
  }
  Storage.remove(path);
}
struct SheetLine {
  std::string text;
  uint32_t offset;
};
struct SheetReader {
  std::vector<SheetLine> lines;
  std::string current;
  int depth = 0, paragraphDepth = -1;
  uint32_t offset = 0, paragraphOffset = 0;
  static void XMLCALL start(void* ctx, const char* name, const char**) {
    auto& s = *static_cast<SheetReader*>(ctx);
    ++s.depth;
    if (std::string_view(name) == "p") {
      s.paragraphDepth = s.depth;
      s.current.clear();
      s.paragraphOffset = s.offset;
    }
    if (std::string_view(name) == "img") throw std::runtime_error("Glyph sheets must contain text only");
  }
  static void XMLCALL text(void* ctx, const char* bytes, int count) {
    auto& s = *static_cast<SheetReader*>(ctx);
    if (s.paragraphDepth >= 0) {
      s.current.append(bytes, count);
      s.offset += static_cast<uint32_t>(scalarCount(std::string_view(bytes, count)));
    }
  }
  static void XMLCALL end(void* ctx, const char*) {
    auto& s = *static_cast<SheetReader*>(ctx);
    if (s.depth == s.paragraphDepth) {
      s.lines.push_back({s.current, s.paragraphOffset});
      s.paragraphDepth = -1;
    }
    --s.depth;
  }
};
std::vector<SheetLine> readSheet(const std::string& path) {
  HalFile input;
  if (!Storage.openFileForRead("PROBE", path, input)) throw std::runtime_error("Cannot open XHTML " + path);
  SheetReader sheet;
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) throw std::bad_alloc();
  std::unique_ptr<std::remove_pointer_t<XML_Parser>, decltype(&XML_ParserFree)> guard(parser, XML_ParserFree);
  XML_SetUserData(parser, &sheet);
  XML_SetElementHandler(parser, SheetReader::start, SheetReader::end);
  XML_SetCharacterDataHandler(parser, SheetReader::text);
  char buffer[4096];
  while (true) {
    const size_t n = input.read(buffer, sizeof(buffer));
    if (XML_Parse(parser, buffer, static_cast<int>(n), n == 0) == XML_STATUS_ERROR)
      throw std::runtime_error(XML_ErrorString(XML_GetErrorCode(parser)));
    if (!n) break;
  }
  if (sheet.lines.empty()) throw std::runtime_error("Glyph-sheet XHTML must contain p elements");
  return sheet.lines;
}

class ShapeReadFault {
 public:
  ShapeReadFault(const Options& options, SdCardFont& font, GfxRenderer& renderer)
      : options_(options), font_(font), renderer_(renderer) {}
  void enter(std::string_view phase) {
    check();
    if (armed_ && options_.shapeFailReadPhase != phase)
      throw std::runtime_error("Requested companion read failure was not exercised during " +
                               options_.shapeFailReadPhase);
    if (armed_ || options_.shapeFailReadPhase != phase) return;
    font_.releaseResidentCaches();
    failuresBefore_ = probe::shapeReadFailures;
    readsBefore_ = probe::shapeReadCalls;
    probe::shapeFailReadAt = readsBefore_ + 1;
    armed_ = true;
  }
  void check() const {
    const bool failed = font_.hasThaiShapeError() || renderer_.hasThaiShapeError();
    if (armed_ && probe::shapeReadFailures != failuresBefore_) {
      if (!failed) throw std::runtime_error("Companion read failed without latching a shaping fault");
      throw std::runtime_error("Exercised companion read failure during " + options_.shapeFailReadPhase +
                               " (HAL read attempts: " + std::to_string(probe::shapeReadCalls - readsBefore_) + ")");
    }
    if (failed) throw std::runtime_error("Latched shaping source fault");
  }
  void leave(std::string_view phase) const {
    check();
    if (armed_ && options_.shapeFailReadPhase == phase)
      throw std::runtime_error("Requested companion read failure was not exercised during " +
                               options_.shapeFailReadPhase);
  }
  void finish() const {
    check();
    if (options_.shapeFailReadPhase != "none")
      throw std::runtime_error("Requested companion read failure was not exercised during " +
                               options_.shapeFailReadPhase);
  }

 private:
  const Options& options_;
  SdCardFont& font_;
  GfxRenderer& renderer_;
  uint64_t failuresBefore_ = 0, readsBefore_ = 0;
  bool armed_ = false;
};

class Probe {
 public:
  Probe(const Options& options, GfxRenderer& renderer, FontCacheManager& cache, std::ostream& report,
        ShapeReadFault& fault)
      : o(options), r(renderer), cache(cache), report(report), fault(fault) {}
  uint64_t drawUs = 0, callbackUs = 0;
  std::array<uint64_t, 3> shapeDrawCalls{}, shapeDrawBytes{};
  size_t pageCount = 0;
  bool cacheRoundtripEqual = true;
  void page(Page& page, uint32_t sourceOffset) {
    fault.check();
    const uint64_t callbackStart = hostMicros();
    auto draw = [&] { page.render(r, FONT_ID, 0, 0); };
    auto prewarm = cache.createPrewarmScope();
    draw();
    prewarm.endScanAndPrewarm();
    fault.check();
    if (o.cacheRoundtrip) verifyCacheRoundtrip(page);
    beginPage(sourceOffset, draw);
    bool first = true;
    for (const auto& element : page.elements) {
      if (element->getTag() != TAG_PageLine) continue;
      if (!first) report << ',';
      first = false;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = *line.getBlock();
      std::string text;
      uint32_t lineOffset = UINT32_MAX;
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        text += block.wordText(i);
        lineOffset = std::min(lineOffset, block.probeVisibleOffset(i));
      }
      if (lineOffset == UINT32_MAX) lineOffset = sourceOffset;
      r.clearScreen();
      line.getBlock()->render(r, FONT_ID, line.xPos, line.yPos);
      report << "{\"text\":" << json(text) << ",\"x\":" << line.xPos << ",\"y\":" << line.yPos << ",\"baseline\":"
             << line.yPos + r.getFontAscenderSize(FONT_ID) + block.getRubyShift(r.getFontAscenderSize(FONT_ID))
             << ",\"baseline_axis\":\"y\",\"source_offset\":" << lineOffset << ",\"ink_bounds\":";
      boundsJson(report, inkBounds(r));
      const auto& distribution = block.probeThaiDistribution();
      report << ",\"distribution\":{\"handled\":" << (distribution.handled ? "true" : "false");
      if (distribution.handled) {
        report << ",\"natural_width\":" << distribution.naturalWidth
               << ",\"available_width\":" << distribution.availableWidth
               << ",\"allocated_extra\":" << distribution.allocated << ",\"remaining_slack\":"
               << distribution.availableWidth - distribution.naturalWidth - static_cast<int>(distribution.allocated)
               << ",\"thai_capacity\":" << distribution.thaiCapacity
               << ",\"space_capacity\":" << distribution.spaceCapacity
               << ",\"other_capacity\":" << distribution.otherCapacity << ",\"gaps\":[";
        for (size_t g = 0; g < distribution.gaps.size(); ++g) {
          if (g) report << ',';
          const auto& gap = distribution.gaps[g];
          const int observed = block.wordXpos(gap.right) - block.wordXpos(gap.left) - gap.leftAdvance - gap.natural;
          if (observed != gap.extra || observed < 0 || observed > gap.limit)
            throw std::runtime_error("Cached inter-token spacing exceeds its limit");
          report << "{\"left\":" << gap.left << ",\"right\":" << gap.right << ",\"natural\":" << gap.natural
                 << ",\"extra\":" << observed << ",\"limit\":" << gap.limit
                 << ",\"space\":" << (gap.space ? "true" : "false") << '}';
        }
        report << ']';
      }
      report << '}';
      report << ",\"tokens\":[";
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        if (i) report << ',';
        const auto style = block.wordStyle(i);
        const auto baseDir = static_cast<BidiUtils::BidiBaseDir>(
            BidiUtils::detectParagraphLevel(block.wordText(i), block.getBlockStyle().isRtl ? 1 : 0));
        const int x = line.xPos + block.wordXpos(i);
        int y = line.yPos + block.getRubyShift(r.getFontAscenderSize(FONT_ID));
        if (style & EpdFontFamily::SUP)
          y -= r.getFontAscenderSize(FONT_ID) * 2 / 5;
        else if (style & EpdFontFamily::SUB)
          y += r.getFontAscenderSize(FONT_ID) / 4;
        const int tracking = block.getBlockStyle().characterSpacing;
        const uint8_t focusBoundary = block.focusBoundary(i);
        const char* measuredText = block.wordText(i) + focusBoundary;
        const int prefixAdvance = focusBoundary ? block.focusSuffixX(i) : 0;
        // Measure a single token through production TextBlock, retaining focus
        // prefix positioning and style handling rather than reimplementing it.
        const uint16_t expansion = block.thaiExpansion(i);
        const int observedMaximum = observeAddedGap(r, measuredText, style, baseDir, tracking, expansion);
        TextBlock isolated({block.wordText(i)}, {block.wordXpos(i)}, {style}, {block.focusBoundary(i)},
                           {block.focusSuffixX(i)}, block.getBlockStyle(), {}, {}, std::span(&expansion, 1));
        r.clearScreen();
        isolated.render(r, FONT_ID, line.xPos, line.yPos + block.getRubyShift(r.getFontAscenderSize(FONT_ID)));
        report << "{\"text\":" << json(block.wordText(i)) << ",\"x\":" << x << ",\"y\":" << y
               << ",\"baseline\":" << y + r.getFontAscenderSize(FONT_ID) << ",\"style\":" << unsigned(style)
               << ",\"source_offset\":" << block.probeVisibleOffset(i)
               << ",\"focus_boundary\":" << unsigned(block.focusBoundary(i))
               << ",\"focus_suffix_x\":" << block.focusSuffixX(i) << ",\"thai_extra_pixels\":" << expansion
               << ",\"thai_gap_count\":" << r.countThaiJustificationGaps(FONT_ID, measuredText, style, baseDir)
               << ",\"thai_gap_limit\":" << r.getThaiJustificationGapLimit(FONT_ID, measuredText, style)
               << ",\"observed_max_added_gap\":" << observedMaximum << ",\"advance\":"
               << prefixAdvance + r.getTextAdvanceX(FONT_ID, measuredText, style, tracking, baseDir,
                                                    GfxRenderer::TextMeasureMode::Layout, expansion)
               << ",\"render_advance\":"
               << prefixAdvance + r.getTextAdvanceX(FONT_ID, measuredText, style, tracking, baseDir,
                                                    GfxRenderer::TextMeasureMode::Rendered, expansion)
               << ",\"ink_bounds\":";
        boundsJson(report, inkBounds(r));
        report << ",\"clusters\":";
        clustersJson(report, block.wordText(i));
        report << '}';
      }
      report << "]}";
    }
    report << "],\"links\":[";
    for (size_t i = 0; i < page.links.size(); ++i) {
      if (i) report << ',';
      const auto& link = page.links[i];
      report << "{\"href\":" << json(link.href) << ",\"x\":" << link.x << ",\"y\":" << link.y
             << ",\"width\":" << link.width << ",\"height\":" << link.height << '}';
    }
    report << "],\"footnotes\":[";
    for (size_t i = 0; i < page.footnotes.size(); ++i) {
      if (i) report << ',';
      report << "{\"number\":" << json(page.footnotes[i].number) << ",\"href\":" << json(page.footnotes[i].href) << '}';
    }
    report << "]}";
    callbackUs += hostMicros() - callbackStart;
  }
  void sheets(const std::vector<SheetLine>& lines) {
    const bool rotated = o.glyphSheet == "rotated90cw";
    const int step = std::max(1, 2 * r.getLineHeight(FONT_ID, o.compression));
    const size_t perPage = static_cast<size_t>(std::max(1, ((rotated ? o.width : o.height) - 32) / step));
    for (size_t start = 0; start < lines.size(); start += perPage) {
      const size_t end = std::min(lines.size(), start + perPage);
      for (size_t i = start; i < end; ++i) cache.prewarmCache(FONT_ID, lines[i].text.c_str(), 1, i != start);
      fault.check();
      auto drawLine = [&](size_t i) {
        const int offset = 16 + static_cast<int>(i - start) * step;
        if (rotated)
          r.drawTextRotated90CW(FONT_ID, offset, o.height - 16, lines[i].text.c_str());
        else
          r.drawText(FONT_ID, 16, offset, lines[i].text.c_str(), true, EpdFontFamily::REGULAR,
                     BidiUtils::BidiBaseDir::AUTO, o.characterSpacing);
      };
      auto draw = [&] {
        for (size_t i = start; i < end; ++i) drawLine(i);
      };
      beginPage(lines[start].offset, draw);
      for (size_t i = start; i < end; ++i) {
        if (i != start) report << ',';
        const int offset = 16 + static_cast<int>(i - start) * step;
        const int x = rotated ? offset : 16, y = rotated ? o.height - 16 : offset;
        r.clearScreen();
        drawLine(i);
        const auto bounds = inkBounds(r);
        const int advance =
            r.getTextAdvanceX(FONT_ID, lines[i].text.c_str(), EpdFontFamily::REGULAR, rotated ? 0 : o.characterSpacing);
        report << "{\"text\":" << json(lines[i].text) << ",\"x\":" << x << ",\"y\":" << y
               << ",\"baseline\":" << (rotated ? x : y) + r.getFontAscenderSize(FONT_ID)
               << ",\"baseline_axis\":" << json(rotated ? "x" : "y") << ",\"source_offset\":" << lines[i].offset
               << ",\"advance\":" << advance << ",\"ink_bounds\":";
        boundsJson(report, bounds);
        report << ",\"clusters\":";
        clustersJson(report, lines[i].text);
        report << ",\"tokens\":[{\"text\":" << json(lines[i].text) << ",\"x\":" << x << ",\"y\":" << y
               << ",\"baseline\":" << (rotated ? x : y) + r.getFontAscenderSize(FONT_ID)
               << ",\"style\":0,\"source_offset\":" << lines[i].offset << ",\"advance\":" << advance
               << ",\"render_advance\":"
               << r.getTextAdvanceX(FONT_ID, lines[i].text.c_str(), EpdFontFamily::REGULAR,
                                    rotated ? 0 : o.characterSpacing, BidiUtils::BidiBaseDir::AUTO,
                                    GfxRenderer::TextMeasureMode::Rendered)
               << ",\"ink_bounds\":";
        boundsJson(report, bounds);
        report << ",\"clusters\":";
        clustersJson(report, lines[i].text);
        report << "}]}";
      }
      report << "]}";
    }
  }

 private:
  const Options& o;
  GfxRenderer& r;
  FontCacheManager& cache;
  std::ostream& report;
  ShapeReadFault& fault;
  void verifyCacheRoundtrip(Page& fresh) {
    const auto path = (std::filesystem::path(o.output) / "roundtrip.bin").string();
    {
      HalFile file;
      if (!Storage.openFileForWrite("PROBE", path, file) || !fresh.serialize(file)) {
        throw std::runtime_error("Cannot serialize cache roundtrip");
      }
    }
    std::unique_ptr<Page> replay;
    {
      HalFile file;
      if (!Storage.openFileForRead("PROBE", path, file) || !(replay = Page::deserialize(file))) {
        throw std::runtime_error("Cannot deserialize cache roundtrip");
      }
    }
    Storage.remove(path);
    if (fresh.links.size() != replay->links.size()) cacheRoundtripEqual = false;
    for (size_t i = 0; cacheRoundtripEqual && i < fresh.links.size(); ++i) {
      const auto& a = fresh.links[i];
      const auto& b = replay->links[i];
      cacheRoundtripEqual =
          std::string_view(a.href) == b.href && a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
    for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_MSB, GfxRenderer::GRAYSCALE_LSB}) {
      r.setRenderMode(mode);
      r.clearScreen(mode == GfxRenderer::BW ? 0xff : 0);
      fresh.render(r, FONT_ID, 0, 0);
      fault.check();
      const std::vector<uint8_t> pixels(r.getFrameBuffer(), r.getFrameBuffer() + r.getBufferSize());
      r.clearScreen(mode == GfxRenderer::BW ? 0xff : 0);
      replay->render(r, FONT_ID, 0, 0);
      fault.check();
      cacheRoundtripEqual &= std::equal(pixels.begin(), pixels.end(), r.getFrameBuffer());
    }
    r.setRenderMode(GfxRenderer::BW);
    if (!cacheRoundtripEqual) throw std::runtime_error("Cache replay pixels/link geometry differ");
  }
  void beginPage(uint32_t offset, const std::function<void()>& draw) {
    if (pageCount) report << ',';
    std::ostringstream name;
    name << "page-" << std::setw(3) << std::setfill('0') << pageCount++;
    const auto prefix = std::filesystem::path(o.output) / name.str();
    const uint64_t started = hostMicros();
    fault.enter("bw");
    const uint64_t bwCalls = probe::shapeReadCalls, bwBytes = probe::shapeReadBytes;
    r.setRenderMode(GfxRenderer::BW);
    r.clearScreen();
    draw();
    fault.leave("bw");
    shapeDrawCalls[0] += probe::shapeReadCalls - bwCalls;
    shapeDrawBytes[0] += probe::shapeReadBytes - bwBytes;
    const uint64_t elapsed = hostMicros() - started;
    drawUs += elapsed;
    const std::vector<uint8_t> bw(r.getFrameBuffer(), r.getFrameBuffer() + r.getBufferSize());
    const auto bounds = inkBounds(r);
    fault.enter("gray-msb");
    const uint64_t msbCalls = probe::shapeReadCalls, msbBytes = probe::shapeReadBytes;
    r.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    r.clearScreen(0);
    draw();
    fault.leave("gray-msb");
    shapeDrawCalls[1] += probe::shapeReadCalls - msbCalls;
    shapeDrawBytes[1] += probe::shapeReadBytes - msbBytes;
    const std::vector<uint8_t> msb(r.getFrameBuffer(), r.getFrameBuffer() + r.getBufferSize());
    fault.enter("gray-lsb");
    const uint64_t lsbCalls = probe::shapeReadCalls, lsbBytes = probe::shapeReadBytes;
    r.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    r.clearScreen(0);
    draw();
    fault.leave("gray-lsb");
    shapeDrawCalls[2] += probe::shapeReadCalls - lsbCalls;
    shapeDrawBytes[2] += probe::shapeReadBytes - lsbBytes;
    std::ofstream pbm(prefix.string() + ".pbm", std::ios::binary);
    pbm << "P4\n" << r.getDisplayWidth() << ' ' << r.getDisplayHeight() << '\n';
    for (uint8_t byte : bw) pbm.put(static_cast<char>(~byte));
    std::ofstream pgm(prefix.string() + ".pgm", std::ios::binary);
    pgm << "P5\n" << r.getDisplayWidth() << ' ' << r.getDisplayHeight() << "\n255\n";
    for (int y = 0; y < r.getDisplayHeight(); ++y)
      for (int x = 0; x < r.getDisplayWidth(); ++x) {
        const size_t pos = y * r.getDisplayWidthBytes() + x / 8;
        const int bit = 0x80 >> (x % 8);
        const uint8_t value = (bw[pos] & bit)                   ? 255
                              : !(msb[pos] & bit)               ? 0
                              : (r.getFrameBuffer()[pos] & bit) ? 85
                                                                : 170;
        pgm.put(static_cast<char>(value));
      }
    if (!pbm || !pgm) throw std::runtime_error("Failed writing page images");
    r.setRenderMode(GfxRenderer::BW);
    report << "{\"image\":" << json(name.str() + ".pbm") << ",\"gray_image\":" << json(name.str() + ".pgm")
           << ",\"width\":" << r.getDisplayWidth() << ",\"height\":" << r.getDisplayHeight()
           << ",\"phase\":" << json(pageCount == 1 ? "first_page" : "page_turn") << ",\"source_offset\":" << offset
           << ",\"draw_us\":" << elapsed << ",\"ink_bounds\":";
    boundsJson(report, bounds);
    report << ",\"lines\":[";
  }
};
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--analyze-file") {
      std::ifstream input(argv[2], std::ios::binary);
      if (!input) throw std::runtime_error("Cannot open analyzer input");
      const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
      clustersJson(std::cout, text);
      std::cout << '\n';
      return 0;
    }
    const Options o = options(argc, argv);
    probe::internalHeadroomBytes = o.shapeInternalFree;
    probe::resetInternalHeapSamples();
    probe::resetShapeFaults();
    ParsedText::probeThaiSpaceWeight = static_cast<uint8_t>(o.thaiSpaceWeight);
    std::filesystem::create_directories(o.output);
    const bool portrait = o.orientation == "portrait" || o.orientation == "inverted";
    HalDisplay display(portrait ? o.height : o.width, portrait ? o.width : o.height);
    GfxRenderer renderer(display);
    renderer.begin();
    renderer.setOrientation(orientation(o));
    SdCardFont font;
#if THAI_ENGINE_STATS
    const auto engineBefore = thai::statsSnapshot();
#endif
    const uint64_t sdCallsBefore = probe::readCalls, sdBytesBefore = probe::readBytes;
    const uint64_t shapeCallsBefore = probe::shapeReadCalls, shapeBytesBefore = probe::shapeReadBytes;
    const uint64_t loadStart = hostMicros();
    bool loaded = false;
#if THAI_SHAPING
    const bool rejectResident =
        o.failShapeAllocation || (o.shapeFailReadPhase != "none" && o.shapeInternalFree > 50u * 1024u);
    if (rejectResident) {
      auto companion = std::filesystem::path(o.font);
      companion.replace_extension(".cpshape");
      probe::ScopedNullableAllocationFailure failFull(std::filesystem::file_size(companion), 1,
                                                      probe::NullableAllocationKind::Array);
      loaded = font.load(o.font.c_str());
      if (probe::nullableAllocationFailure().failures != 1)
        throw std::runtime_error("Requested companion allocation failure was not exercised");
    } else {
      loaded = font.load(o.font.c_str());
    }
#else
    if (o.shapeFailReadPhase != "none")
      throw std::runtime_error("Companion read fault injection requires THAI_SHAPING");
    loaded = font.load(o.font.c_str());
#endif
    if (!loaded || !font.getEpdFont()) throw std::runtime_error("Cannot load regular CPFont " + o.font);
    const uint64_t loadUs = hostMicros() - loadStart;
    const uint64_t shapeLoadCalls = probe::shapeReadCalls - shapeCallsBefore;
    const uint64_t shapeLoadBytes = probe::shapeReadBytes - shapeBytesBefore;
#if THAI_SHAPING
    if ((o.failShapeAllocation || o.shapeFailReadPhase != "none") && !font.getEpdFont()->getThaiShape())
      throw std::runtime_error("Requested cached companion mode did not preserve active shaping");
#endif
    renderer.insertFont(FONT_ID,
                        EpdFontFamily(font.getEpdFont(), font.getEpdFont(1), font.getEpdFont(2), font.getEpdFont(3)));
    renderer.registerSdCardFont(FONT_ID, &font);
    FontDecompressor decompressor;
    decompressor.init();
    FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
    cache.setFontDecompressor(&decompressor);
    renderer.setFontCacheManager(&cache);
    ShapeReadFault shapeFault(o, font, renderer);
    if (o.verifyThaiPlacement) {
      verifyMarkedPlacement(renderer, font);
      verifyNativeLigatures(o);
    }
    shapeFault.check();
    const auto pendingReport = std::filesystem::path(o.output) / "report.pending.json";
    std::ofstream report(pendingReport);
    if (!report) throw std::runtime_error("Cannot create pending report");
    report << "{\"schema_version\":1,\"settings\":{\"font\":" << json(o.font) << ",\"xhtml\":" << json(o.xhtml)
           << ",\"width\":" << o.width << ",\"height\":" << o.height
           << ",\"panel_width\":" << renderer.getDisplayWidth() << ",\"panel_height\":" << renderer.getDisplayHeight()
           << ",\"orientation\":" << json(o.orientation) << ",\"alignment\":" << json(o.alignment)
           << ",\"character_spacing\":" << (o.glyphSheet == "rotated90cw" ? 0 : o.characterSpacing)
           << ",\"requested_character_spacing\":" << o.characterSpacing << ",\"word_spacing_percent\":" << o.wordSpacing
           << ",\"thai_space_weight\":" << o.thaiSpaceWeight << ",\"line_compression\":" << o.compression
           << ",\"hyphenation\":" << json(o.hyphenation ? "on" : "off") << ",\"focus\":" << json(o.focus ? "on" : "off")
           << ",\"glyph_sheet\":" << json(o.glyphSheet) << ",\"output\":" << json(o.output)
           << ",\"paragraph_settings_applied\":" << (o.glyphSheet.empty() ? "true" : "false")
           << ",\"font_ascender\":" << renderer.getFontAscenderSize(FONT_ID)
           << ",\"line_height\":" << renderer.getLineHeight(FONT_ID, o.compression)
           << ",\"font_id\":" << font.contentHash()
           << ",\"thai_shape_active\":" << (font.getEpdFont()->getThaiShape() ? "true" : "false")
           << "},\"coordinate_space\":\"logical; images physical\","
              "\"source_offset_unit\":\"visible Unicode codepoints\","
              "\"line_text_convention\":\"concatenated tokens; no inferred spaces\","
              "\"ink_bounds_unit\":\"logical clipped pixels [x,y,width,height]\","
              "\"pages\":[";
    Probe probe(o, renderer, cache, report, shapeFault);
    shapeFault.enter("layout");
    uint64_t layoutUs = 0;
    bool analysisUnavailable = false;
    if (o.glyphSheet.empty()) {
      CssParser css(o.output);
      ChapterHtmlSlimParser parser(
          nullptr, o.xhtml, renderer, FONT_ID, o.compression, false, alignmentValue(o.alignment), o.width, o.height,
          o.hyphenation, o.focus,
          [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t offset) { probe.page(*page, offset); }, true, "",
          "", 0, {}, nullptr, &css);
      parser.setTextSpacing(static_cast<int8_t>(o.characterSpacing), static_cast<uint8_t>(o.wordSpacing));
      parser.failThaiAllocation = o.failThaiAllocation;
      const uint64_t started = hostMicros();
      const bool parsed = parser.parseAndBuildPages();
      shapeFault.check();
      if (!parsed) throw std::runtime_error("Production chapter parse/layout failed");
      layoutUs = hostMicros() - started - probe.callbackUs;
      analysisUnavailable = parser.thaiAnalysisUnavailable();
    } else {
      const uint64_t started = hostMicros();
      const auto lines = readSheet(o.xhtml);
      layoutUs = hostMicros() - started;
      probe.sheets(lines);
    }
    shapeFault.finish();
    if (!probe.pageCount) throw std::runtime_error("No pages produced");
#if THAI_ENGINE_STATS
    thai::recordLayoutMicros(static_cast<uint32_t>(layoutUs));
    const auto engineAfter = thai::statsSnapshot();
#endif
    report
        << "],\"measurements\":{\"phase\":\"host_complete\",\"font_load_us\":" << loadUs
        << ",\"layout_us\":" << layoutUs << ",\"draw_us\":" << probe.drawUs
        << ",\"thai_analysis_unavailable\":" << (analysisUnavailable ? "true" : "false")
        << ",\"cache_roundtrip_equal\":" << (o.cacheRoundtrip ? (probe.cacheRoundtripEqual ? "true" : "false") : "null")
#if THAI_ENGINE_STATS
        << ",\"engine_stats_enabled\":true,\"segment_us\":"
        << uint32_t(engineAfter.segment_us - engineBefore.segment_us)
        << ",\"input_bytes\":" << uint32_t(engineAfter.input_bytes - engineBefore.input_bytes)
        << ",\"clusters\":" << uint32_t(engineAfter.clusters - engineBefore.clusters)
        << ",\"words\":" << uint32_t(engineAfter.words - engineBefore.words)
        << ",\"unknown_clusters\":" << uint32_t(engineAfter.unknown_clusters - engineBefore.unknown_clusters)
        << ",\"max_pending_bytes\":" << engineAfter.max_pending_bytes
        << ",\"engine_draw_us\":" << uint32_t(engineAfter.draw_us - engineBefore.draw_us)
#else
        << ",\"engine_stats_enabled\":false,\"segment_us\":null,\"input_bytes\":null,\"clusters\":null,"
           "\"words\":null,\"unknown_clusters\":null,\"max_pending_bytes\":null,\"engine_draw_us\":null"
#endif
        << ",\"internal_free\":null,\"internal_largest\":null,\"internal_min_observed\":null,"
           "\"internal_min_since_boot\":null,\"psram_free\":null,\"psram_largest\":null,\"refresh_ms\":null,"
           "\"dictionary_id\":"
        << thai::dictionaryDataId() << ",\"font_id\":" << font.contentHash()
        << ",\"sd_read_calls\":" << (probe::readCalls - sdCallsBefore)
        << ",\"sd_read_bytes\":" << (probe::readBytes - sdBytesBefore)
        << ",\"shape_hal_read_calls\":" << (probe::shapeReadCalls - shapeCallsBefore)
        << ",\"shape_hal_read_bytes\":" << (probe::shapeReadBytes - shapeBytesBefore)
        << ",\"shape_load_hal_read_calls\":" << shapeLoadCalls << ",\"shape_load_hal_read_bytes\":" << shapeLoadBytes
        << ",\"shape_bw_hal_read_calls\":" << probe.shapeDrawCalls[0]
        << ",\"shape_bw_hal_read_bytes\":" << probe.shapeDrawBytes[0]
        << ",\"shape_gray_msb_hal_read_calls\":" << probe.shapeDrawCalls[1]
        << ",\"shape_gray_msb_hal_read_bytes\":" << probe.shapeDrawBytes[1]
        << ",\"shape_gray_lsb_hal_read_calls\":" << probe.shapeDrawCalls[2]
        << ",\"shape_gray_lsb_hal_read_bytes\":" << probe.shapeDrawBytes[2]
        << ",\"shape_simulated_internal_free_bytes\":" << o.shapeInternalFree
        << ",\"shape_scope\":\"companion-only HAL requests including failed attempts; not physical SD transactions; "
           "draw phase counters exclude prewarm, cache replay and diagnostics\""
        << ",\"host_allocation_high_water_bytes\":" << probe::allocationHighWater()
        << ",\"host_allocation_live_bytes\":" << probe::allocationLive()
        << ",\"host_allocation_calls\":" << probe::allocationCalls()
        << ",\"host_allocation_live_count\":" << probe::allocationLiveCount()
        << ",\"host_allocation_scope\":\"host-only C++ new/new[] and font allocator; excludes libc/Expat and device "
           "heap; sampled before font teardown\","
           "\"measurement_scope\":\"host run; cumulative counter snapshot deltas; max_pending_bytes is process "
           "high-water\","
           "\"layout_scope\":\"production parser/layout excluding all rendering callbacks\","
           "\"segment_scope\":\"production nextSegment calls including uncommitted lookahead attempts\","
           "\"count_scope\":\"committed valid Thai-run clusters and dictionary words; unknown_clusters excludes "
           "punctuation and malformed fallback\","
           "\"draw_scope\":\"production BW page render after prewarm; excludes image export, trace and gray plane "
           "passes\","
           "\"engine_draw_scope\":\"glyph loops for all BW/gray/diagnostic renders; excludes scan and batch prewarm\","
           "\"refresh_scope\":\"unmeasured; host has no physical panel\","
           "\"sd_scope\":\"all HAL requests including font load, XHTML, layout, prewarm, gray and diagnostic renders; "
           "not physical sectors\"}}\n";
    report.flush();
    if (!report) throw std::runtime_error("Failed writing report.json");
    report.close();
    if (!report) throw std::runtime_error("Failed closing pending report");
    std::filesystem::rename(pendingReport, std::filesystem::path(o.output) / "report.json");
    std::cout << "Rendered " << probe.pageCount << " pages to " << o.output << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ThaiRenderProbe: " << e.what() << '\n';
    return 1;
  }
}