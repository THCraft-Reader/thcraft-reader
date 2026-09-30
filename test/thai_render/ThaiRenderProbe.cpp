#include <BidiUtils.h>
#include <Epub/Page.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FontPsram.h>
#include <GfxRenderer.h>
#include <SdCardFont.h>
#include <ThaiDictionary.h>
#include <ThaiCluster.h>
#include <ThaiStats.h>
#include <expat.h>
#include "HostAllocation.h"
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

namespace {
constexpr int FONT_ID = 1;
struct Options {
  std::string font, xhtml, output, orientation = "portrait", glyphSheet;
  int width = 480, height = 800, characterSpacing = 0, wordSpacing = 100;
  float compression = 1.0f;
  bool hyphenation = false, focus = false;
  bool failThaiAllocation = false;
  bool failShapeAllocation = false;
};
std::string json(const std::string& text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : text) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << static_cast<char>(c);
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
bool toggle(const std::string& value) {
  if (value == "on") return true;
  if (value == "off") return false;
  throw std::runtime_error("Expected on|off: " + value);
}
Options options(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--help") {
      std::cout << "ThaiRenderProbe --font PATH --xhtml PATH --width N --height N --orientation portrait|inverted|cw|ccw --character-spacing N --word-spacing-percent N --line-compression F --hyphenation on|off --focus on|off --output DIR [--glyph-sheet normal|rotated90cw] [--fail-thai-allocation on|off]\n";
      std::exit(0);
    }
    if (++i == argc) throw std::runtime_error("Missing value for " + key);
    const std::string value = argv[i];
    if (key == "--font") o.font = value;
    else if (key == "--xhtml") o.xhtml = value;
    else if (key == "--output") o.output = value;
    else if (key == "--width") o.width = integer(value);
    else if (key == "--height") o.height = integer(value);
    else if (key == "--orientation") o.orientation = value;
    else if (key == "--glyph-sheet") o.glyphSheet = value;
    else if (key == "--character-spacing") o.characterSpacing = integer(value);
    else if (key == "--word-spacing-percent") o.wordSpacing = integer(value);
    else if (key == "--line-compression") { size_t end = 0; o.compression = std::stof(value, &end); if (end != value.size()) throw std::runtime_error("Invalid compression"); }
    else if (key == "--hyphenation") o.hyphenation = toggle(value);
    else if (key == "--focus") o.focus = toggle(value);
    else if (key == "--fail-thai-allocation") o.failThaiAllocation = toggle(value);
    else if (key == "--fail-shape-allocation") o.failShapeAllocation = toggle(value);
    else throw std::runtime_error("Unknown option " + key);
  }
  if (o.font.empty() || o.xhtml.empty() || o.output.empty()) throw std::runtime_error("--font, --xhtml and --output are required");
  if (o.font.size() >= 128) throw std::runtime_error("CPFont path exceeds production loader's 127-byte limit; use a relative path");
  if (o.width < 1 || o.height < 1 || o.width > 8192 || o.height > 8192) throw std::runtime_error("Viewport must be within 1..8192 pixels");
  if (o.characterSpacing < -128 || o.characterSpacing > 127 || o.wordSpacing < 0 || o.wordSpacing > 255) throw std::runtime_error("Spacing exceeds production setting storage");
  if (!std::isfinite(o.compression) || o.compression <= 0 || o.compression > 4) throw std::runtime_error("Invalid line compression");
  if (o.orientation != "portrait" && o.orientation != "inverted" && o.orientation != "cw" && o.orientation != "ccw") throw std::runtime_error("Invalid orientation");
  if (!o.glyphSheet.empty() && o.glyphSheet != "normal" && o.glyphSheet != "rotated90cw") throw std::runtime_error("Invalid glyph-sheet mode");
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
    case GfxRenderer::Portrait: return {r.getDisplayHeight() - 1 - y, x};
    case GfxRenderer::PortraitInverted: return {y, r.getDisplayWidth() - 1 - x};
    case GfxRenderer::LandscapeClockwise: return {r.getDisplayWidth() - 1 - x, r.getDisplayHeight() - 1 - y};
    default: return {x, y};
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
        left = std::min(left, x); right = std::max(right, x);
        top = std::min(top, y); bottom = std::max(bottom, y);
      }
    }
  }
  if (right < left) return {0, 0, 0, 0};
  return {left, top, right - left + 1, bottom - top + 1};
}
void boundsJson(std::ostream& out, const std::array<int, 4>& b) { out << '[' << b[0] << ',' << b[1] << ',' << b[2] << ',' << b[3] << ']'; }
uint64_t scalarCount(std::string_view text) { uint64_t n = 0; for (unsigned char c : text) n += (c & 0xc0) != 0x80; return n; }
void clustersJson(std::ostream& out, std::string_view text) {
  out << '[';
  size_t offset = 0, scalar = 0;
  thai::Cluster cluster{};
  while (thai::nextCluster(text, offset, cluster, true)) {
    if (cluster.begin) out << ',';
    out << "{\"begin\":" << scalar << ",\"end\":" << scalar + cluster.codepoints
        << ",\"byte_begin\":" << cluster.begin << ",\"byte_end\":" << cluster.end
        << ",\"valid\":" << (cluster.valid ? "true" : "false")
        << ",\"thai\":" << (thai::containsThai(text.substr(cluster.begin, cluster.end - cluster.begin)) ? "true" : "false") << '}';
    scalar += cluster.codepoints;
  }
  out << ']';
}
struct SheetLine { std::string text; uint32_t offset; };
struct SheetReader {
  std::vector<SheetLine> lines;
  std::string current;
  int depth = 0, paragraphDepth = -1;
  uint32_t offset = 0, paragraphOffset = 0;
  static void XMLCALL start(void* ctx, const char* name, const char**) {
    auto& s = *static_cast<SheetReader*>(ctx);
    ++s.depth;
    if (std::string_view(name) == "p") { s.paragraphDepth = s.depth; s.current.clear(); s.paragraphOffset = s.offset; }
    if (std::string_view(name) == "img") throw std::runtime_error("Glyph sheets must contain text only");
  }
  static void XMLCALL text(void* ctx, const char* bytes, int count) {
    auto& s = *static_cast<SheetReader*>(ctx);
    if (s.paragraphDepth >= 0) { s.current.append(bytes, count); s.offset += static_cast<uint32_t>(scalarCount(std::string_view(bytes, count))); }
  }
  static void XMLCALL end(void* ctx, const char*) {
    auto& s = *static_cast<SheetReader*>(ctx);
    if (s.depth == s.paragraphDepth) { s.lines.push_back({s.current, s.paragraphOffset}); s.paragraphDepth = -1; }
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
    if (XML_Parse(parser, buffer, static_cast<int>(n), n == 0) == XML_STATUS_ERROR) throw std::runtime_error(XML_ErrorString(XML_GetErrorCode(parser)));
    if (!n) break;
  }
  if (sheet.lines.empty()) throw std::runtime_error("Glyph-sheet XHTML must contain p elements");
  return sheet.lines;
}

class Probe {
 public:
  Probe(const Options& options, GfxRenderer& renderer, FontCacheManager& cache, std::ostream& report)
      : o(options), r(renderer), cache(cache), report(report) {}
  uint64_t drawUs = 0, callbackUs = 0;
  size_t pageCount = 0;
  void page(Page& page, uint32_t sourceOffset) {
    const uint64_t callbackStart = hostMicros();
    auto draw = [&] { page.render(r, FONT_ID, 0, 0); };
    auto prewarm = cache.createPrewarmScope();
    draw();
    prewarm.endScanAndPrewarm();
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
      r.clearScreen(); line.getBlock()->render(r, FONT_ID, line.xPos, line.yPos);
      report << "{\"text\":" << json(text) << ",\"x\":" << line.xPos << ",\"y\":" << line.yPos
             << ",\"baseline\":" << line.yPos + r.getFontAscenderSize(FONT_ID) + block.getRubyShift(r.getFontAscenderSize(FONT_ID))
             << ",\"baseline_axis\":\"y\",\"source_offset\":" << lineOffset
             << ",\"ink_bounds\":";
      boundsJson(report, inkBounds(r));
      report << ",\"tokens\":[";
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        if (i) report << ',';
        const auto style = block.wordStyle(i);
        const auto baseDir = static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(block.wordText(i), block.getBlockStyle().isRtl ? 1 : 0));
        const int x = line.xPos + block.wordXpos(i);
        int y = line.yPos + block.getRubyShift(r.getFontAscenderSize(FONT_ID));
        if (style & EpdFontFamily::SUP) y -= r.getFontAscenderSize(FONT_ID) * 2 / 5;
        else if (style & EpdFontFamily::SUB) y += r.getFontAscenderSize(FONT_ID) / 4;
        const int tracking = block.getBlockStyle().characterSpacing;
        const uint8_t focusBoundary = block.focusBoundary(i);
        const char* measuredText = block.wordText(i) + focusBoundary;
        const int prefixAdvance = focusBoundary ? block.focusSuffixX(i) : 0;
        // Measure a single token through production TextBlock, retaining focus
        // prefix positioning and style handling rather than reimplementing it.
        TextBlock isolated({block.wordText(i)}, {block.wordXpos(i)}, {style}, {block.focusBoundary(i)},
                           {block.focusSuffixX(i)}, block.getBlockStyle());
        r.clearScreen();
        isolated.render(r, FONT_ID, line.xPos, line.yPos + block.getRubyShift(r.getFontAscenderSize(FONT_ID)));
        report << "{\"text\":" << json(block.wordText(i)) << ",\"x\":" << x << ",\"y\":" << y
               << ",\"baseline\":" << y + r.getFontAscenderSize(FONT_ID) << ",\"style\":" << unsigned(style)
               << ",\"source_offset\":" << block.probeVisibleOffset(i)
               << ",\"focus_boundary\":" << unsigned(block.focusBoundary(i)) << ",\"focus_suffix_x\":" << block.focusSuffixX(i)
               << ",\"advance\":" << prefixAdvance + r.getTextAdvanceX(FONT_ID, measuredText, style, tracking, baseDir)
               << ",\"render_advance\":" << prefixAdvance + r.getTextAdvanceX(FONT_ID, measuredText, style, tracking, baseDir, GfxRenderer::TextMeasureMode::Rendered)
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
      report << "{\"href\":" << json(link.href) << ",\"x\":" << link.x << ",\"y\":" << link.y << ",\"width\":" << link.width << ",\"height\":" << link.height << '}';
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
      auto drawLine = [&](size_t i) {
        const int offset = 16 + static_cast<int>(i - start) * step;
        if (rotated) r.drawTextRotated90CW(FONT_ID, offset, o.height - 16, lines[i].text.c_str());
        else r.drawText(FONT_ID, 16, offset, lines[i].text.c_str(), true, EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO, o.characterSpacing);
      };
      auto draw = [&] { for (size_t i = start; i < end; ++i) drawLine(i); };
      beginPage(lines[start].offset, draw);
      for (size_t i = start; i < end; ++i) {
        if (i != start) report << ',';
        const int offset = 16 + static_cast<int>(i - start) * step;
        const int x = rotated ? offset : 16, y = rotated ? o.height - 16 : offset;
        r.clearScreen(); drawLine(i);
        const auto bounds = inkBounds(r);
        const int advance = r.getTextAdvanceX(FONT_ID, lines[i].text.c_str(), EpdFontFamily::REGULAR, rotated ? 0 : o.characterSpacing);
        report << "{\"text\":" << json(lines[i].text) << ",\"x\":" << x << ",\"y\":" << y
               << ",\"baseline\":" << (rotated ? x : y) + r.getFontAscenderSize(FONT_ID)
               << ",\"baseline_axis\":" << json(rotated ? "x" : "y") << ",\"source_offset\":" << lines[i].offset
               << ",\"advance\":" << advance << ",\"ink_bounds\":";
        boundsJson(report, bounds);
        report << ",\"clusters\":";
        clustersJson(report, lines[i].text);
        report << ",\"tokens\":[{\"text\":" << json(lines[i].text) << ",\"x\":" << x << ",\"y\":" << y
               << ",\"baseline\":" << (rotated ? x : y) + r.getFontAscenderSize(FONT_ID) << ",\"style\":0,\"source_offset\":" << lines[i].offset
               << ",\"advance\":" << advance << ",\"render_advance\":" << r.getTextAdvanceX(FONT_ID, lines[i].text.c_str(), EpdFontFamily::REGULAR, rotated ? 0 : o.characterSpacing, BidiUtils::BidiBaseDir::AUTO, GfxRenderer::TextMeasureMode::Rendered)
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
  void beginPage(uint32_t offset, const std::function<void()>& draw) {
    if (pageCount) report << ',';
    std::ostringstream name;
    name << "page-" << std::setw(3) << std::setfill('0') << pageCount++;
    const auto prefix = std::filesystem::path(o.output) / name.str();
    const uint64_t started = hostMicros();
    r.setRenderMode(GfxRenderer::BW); r.clearScreen(); draw();
    const uint64_t elapsed = hostMicros() - started;
    drawUs += elapsed;
    const std::vector<uint8_t> bw(r.getFrameBuffer(), r.getFrameBuffer() + r.getBufferSize());
    const auto bounds = inkBounds(r);
    std::ofstream pbm(prefix.string() + ".pbm", std::ios::binary);
    pbm << "P4\n" << r.getDisplayWidth() << ' ' << r.getDisplayHeight() << '\n';
    for (uint8_t byte : bw) pbm.put(static_cast<char>(~byte));
    r.setRenderMode(GfxRenderer::GRAYSCALE_MSB); r.clearScreen(0); draw();
    const std::vector<uint8_t> msb(r.getFrameBuffer(), r.getFrameBuffer() + r.getBufferSize());
    r.setRenderMode(GfxRenderer::GRAYSCALE_LSB); r.clearScreen(0); draw();
    std::ofstream pgm(prefix.string() + ".pgm", std::ios::binary);
    pgm << "P5\n" << r.getDisplayWidth() << ' ' << r.getDisplayHeight() << "\n255\n";
    for (int y = 0; y < r.getDisplayHeight(); ++y) for (int x = 0; x < r.getDisplayWidth(); ++x) {
      const size_t pos = y * r.getDisplayWidthBytes() + x / 8;
      const int bit = 0x80 >> (x % 8);
      const uint8_t value = (bw[pos] & bit) ? 255 : !(msb[pos] & bit) ? 0 : (r.getFrameBuffer()[pos] & bit) ? 85 : 170;
      pgm.put(static_cast<char>(value));
    }
    if (!pbm || !pgm) throw std::runtime_error("Failed writing page images");
    r.setRenderMode(GfxRenderer::BW);
    report << "{\"image\":" << json(name.str() + ".pbm") << ",\"gray_image\":" << json(name.str() + ".pgm")
           << ",\"width\":" << r.getDisplayWidth() << ",\"height\":" << r.getDisplayHeight()
           << ",\"phase\":" << json(pageCount == 1 ? "first_page" : "page_turn")
           << ",\"source_offset\":" << offset << ",\"draw_us\":" << elapsed << ",\"ink_bounds\":";
    boundsJson(report, bounds);
    report << ",\"lines\":[";
  }
};
}

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
    std::filesystem::create_directories(o.output);
    const bool portrait = o.orientation == "portrait" || o.orientation == "inverted";
    HalDisplay display(portrait ? o.height : o.width, portrait ? o.width : o.height);
    GfxRenderer renderer(display);
    renderer.begin(); renderer.setOrientation(orientation(o));
    SdCardFont font;
#if THAI_ENGINE_STATS
    const auto engineBefore = thai::statsSnapshot();
#endif
    const uint64_t sdCallsBefore = probe::readCalls, sdBytesBefore = probe::readBytes;
    const uint64_t loadStart = hostMicros();
    if (o.failShapeAllocation) {
      auto companion = std::filesystem::path(o.font);
      companion.replace_extension(".cpshape");
      probe::failFontAllocationBytes = std::filesystem::file_size(companion);
    }
    if (!font.load(o.font.c_str()) || !font.getEpdFont()) throw std::runtime_error("Cannot load regular CPFont " + o.font);
    const uint64_t loadUs = hostMicros() - loadStart;
    if (o.failShapeAllocation && probe::failFontAllocationBytes != 0) {
      throw std::runtime_error("Requested companion allocation failure was not exercised");
    }
    renderer.insertFont(FONT_ID, EpdFontFamily(font.getEpdFont(), font.getEpdFont(1), font.getEpdFont(2), font.getEpdFont(3)));
    renderer.registerSdCardFont(FONT_ID, &font);
    FontDecompressor decompressor; decompressor.init();
    FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
    cache.setFontDecompressor(&decompressor); renderer.setFontCacheManager(&cache);
    std::ofstream report(std::filesystem::path(o.output) / "report.json");
    if (!report) throw std::runtime_error("Cannot create report.json");
    report << "{\"schema_version\":1,\"settings\":{\"font\":" << json(o.font) << ",\"xhtml\":" << json(o.xhtml)
           << ",\"width\":" << o.width << ",\"height\":" << o.height << ",\"panel_width\":" << renderer.getDisplayWidth()
           << ",\"panel_height\":" << renderer.getDisplayHeight() << ",\"orientation\":" << json(o.orientation)
           << ",\"character_spacing\":" << (o.glyphSheet == "rotated90cw" ? 0 : o.characterSpacing)
           << ",\"requested_character_spacing\":" << o.characterSpacing << ",\"word_spacing_percent\":" << o.wordSpacing
           << ",\"line_compression\":" << o.compression << ",\"hyphenation\":" << json(o.hyphenation ? "on" : "off")
           << ",\"focus\":" << json(o.focus ? "on" : "off") << ",\"glyph_sheet\":" << json(o.glyphSheet)
           << ",\"output\":" << json(o.output)
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
    Probe probe(o, renderer, cache, report);
    uint64_t layoutUs = 0;
    bool analysisUnavailable = false;
    if (o.glyphSheet.empty()) {
      CssParser css(o.output);
      ChapterHtmlSlimParser parser(
          nullptr, o.xhtml, renderer, FONT_ID, o.compression, false, 0, o.width, o.height,
          o.hyphenation, o.focus,
          [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t offset) { probe.page(*page, offset); },
          true, "", "", 0, {}, nullptr, &css);
      parser.setTextSpacing(static_cast<int8_t>(o.characterSpacing), static_cast<uint8_t>(o.wordSpacing));
      parser.failThaiAllocation = o.failThaiAllocation;
      const uint64_t started = hostMicros();
      if (!parser.parseAndBuildPages()) throw std::runtime_error("Production chapter parse/layout failed");
      layoutUs = hostMicros() - started - probe.callbackUs;
      analysisUnavailable = parser.thaiAnalysisUnavailable();
    } else {
      const uint64_t started = hostMicros();
      const auto lines = readSheet(o.xhtml);
      layoutUs = hostMicros() - started;
      probe.sheets(lines);
    }
    if (!probe.pageCount) throw std::runtime_error("No pages produced");
#if THAI_ENGINE_STATS
    thai::recordLayoutMicros(static_cast<uint32_t>(layoutUs));
    const auto engineAfter = thai::statsSnapshot();
#endif
    report << "],\"measurements\":{\"phase\":\"host_complete\",\"font_load_us\":" << loadUs
           << ",\"layout_us\":" << layoutUs << ",\"draw_us\":" << probe.drawUs
           << ",\"thai_analysis_unavailable\":" << (analysisUnavailable ? "true" : "false")
#if THAI_ENGINE_STATS
           << ",\"engine_stats_enabled\":true,\"segment_us\":" << uint32_t(engineAfter.segment_us - engineBefore.segment_us)
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
              "\"dictionary_id\":" << thai::dictionaryDataId() << ",\"font_id\":" << font.contentHash()
           << ",\"sd_read_calls\":" << (probe::readCalls - sdCallsBefore)
           << ",\"sd_read_bytes\":" << (probe::readBytes - sdBytesBefore)
           << ",\"host_allocation_high_water_bytes\":" << probe::allocationHighWater()
           << ",\"host_allocation_live_bytes\":" << probe::allocationLive()
           << ",\"host_allocation_scope\":\"host-only C++ new/new[] and font allocator; excludes libc/Expat and device heap; sampled before font teardown\","
              "\"measurement_scope\":\"host run; cumulative counter snapshot deltas; max_pending_bytes is process high-water\","
              "\"layout_scope\":\"production parser/layout excluding all rendering callbacks\","
              "\"segment_scope\":\"production nextSegment calls including uncommitted lookahead attempts\","
              "\"count_scope\":\"committed valid Thai-run clusters and dictionary words; unknown_clusters excludes punctuation and malformed fallback\","
              "\"draw_scope\":\"production BW page render after prewarm; excludes image export, trace and gray plane passes\","
              "\"engine_draw_scope\":\"glyph loops for all BW/gray/diagnostic renders; excludes scan and batch prewarm\","
              "\"refresh_scope\":\"unmeasured; host has no physical panel\","
              "\"sd_scope\":\"all HAL requests including font load, XHTML, layout, prewarm, gray and diagnostic renders; not physical sectors\"}}\n";
    report.flush();
    if (!report) throw std::runtime_error("Failed writing report.json");
    std::cout << "Rendered " << probe.pageCount << " pages to " << o.output << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ThaiRenderProbe: " << e.what() << '\n';
    return 1;
  }
}