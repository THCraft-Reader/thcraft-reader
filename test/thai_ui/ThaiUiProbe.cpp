#include <EpdFont.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>
#include <Utf8.h>
#include <builtinFonts/notosans_8_regular.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_10_bold.h>
#include <builtinFonts/ubuntu_12_regular.h>
#include <builtinFonts/ubuntu_12_bold.h>
#include <fontIds.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int WIDTH = 480, HEIGHT = 800, LEFT = 24;
struct UiFont { int id; uint8_t size; const char* symbol; };
constexpr UiFont UI[] = {{SMALL_FONT_ID, 8, "notosans_8_regular"},
                         {UI_10_FONT_ID, 10, "ubuntu_10_regular"},
                         {UI_12_FONT_ID, 12, "ubuntu_12_regular"}};
using Pixels = std::vector<uint8_t>;
std::string json(const std::string& text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
    else out << static_cast<char>(c);
  }
  return out.str() + '"';
}
void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
// Export the real production framebuffer in logical portrait coordinates.
Pixels capture(const GfxRenderer& renderer) {
  Pixels pixels(WIDTH * HEIGHT, 255);
  const auto* frame = renderer.getFrameBuffer();
  for (int y = 0; y < HEIGHT; ++y) for (int x = 0; x < WIDTH; ++x) {
    const int px = y, py = WIDTH - 1 - x;
    pixels[y * WIDTH + x] = (frame[py * renderer.getDisplayWidthBytes() + px / 8] & (0x80 >> (px % 8))) ? 255 : 0;
  }
  return pixels;
}
std::array<int, 4> bounds(const Pixels& pixels) {
  int left = WIDTH, top = HEIGHT, right = -1, bottom = -1;
  for (int y = 0; y < HEIGHT; ++y) for (int x = 0; x < WIDTH; ++x) {
    if (pixels[y * WIDTH + x] != 255) {
      left = std::min(left, x); right = std::max(right, x);
      top = std::min(top, y); bottom = std::max(bottom, y);
    }
  }
  return right < left ? std::array<int, 4>{0, 0, 0, 0} : std::array<int, 4>{left, top, right - left + 1, bottom - top + 1};
}
void arrayJson(std::ostream& out, const std::array<int, 4>& value) {
  out << '[' << value[0] << ',' << value[1] << ',' << value[2] << ',' << value[3] << ']';
}
void writePgm(const std::filesystem::path& path, const Pixels& pixels) {
  std::ofstream out(path, std::ios::binary);
  require(bool(out), "Cannot create " + path.string());
  out << "P5\n" << WIDTH << ' ' << HEIGHT << "\n255\n";
  out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
  require(bool(out), "Cannot write " + path.string());
}
std::set<uint32_t> missing(const EpdFontFamily& font, const std::string& text, EpdFontFamily::Style style) {
  std::set<uint32_t> result;
  const char* p = text.c_str();
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&p));
    if (!font.hasCodepoint(cp, style)) result.insert(cp);
  }
  return result;
}
void setJson(std::ostream& out, const std::set<uint32_t>& values) {
  out << '[';
  bool first = true;
  for (auto cp : values) { if (!first) out << ','; first = false; out << cp; }
  out << ']';
}
struct Failures {
  std::vector<std::string> messages;
  void check(bool pass, const std::string& message) { if (!pass) messages.push_back(message); }
};

void renderSize(GfxRenderer& renderer, const SdCardFontFamilyInfo& family, const UiFont& ui,
                int fallback, const std::filesystem::path& output, std::ostream& report, Failures& failures) {
  const int height = renderer.getLineHeight(ui.id);
  const int fallbackHeight = renderer.getLineHeight(fallback);
  const int offset = (height - fallbackHeight) / 2;
  // X4 Pro is a touch device. Screen::resolveListProps (FreeInkApp.h)
  // uses listTouchRowPaddingY=8 and listTouchMinRowHeight=56 from
  // FreeInkUICore.h; uiThemeTokens does not override either value.
  // The primary font line box is NOT a clipping rectangle.
  const int rowHeight = std::max(56, height + 2 * 8);
  const auto& primaryFont = renderer.getFontMap().at(ui.id);
  const auto& fallbackFont = renderer.getFontMap().at(fallback);
  const bool companion = fallbackFont.getThaiShape() != nullptr;
  failures.check(companion, family.name + ": companion inactive at " + std::to_string(ui.size));
  const std::string stem = family.name + "_" + std::to_string(ui.size);
  Pixels sheet(WIDTH * HEIGHT, 255), guides = sheet;
  int rowY = 24;
  bool first = true;
  report << "{\"point_size\":" << unsigned(ui.size) << ",\"primary_font_id\":" << ui.id
         << ",\"primary_symbol\":" << json(ui.symbol) << ",\"fallback_font_id\":" << fallback
         << ",\"active_companion\":" << (companion ? "true" : "false")
         << ",\"primary_line_height\":" << height << ",\"fallback_line_height\":" << fallbackHeight
         << ",\"fallback_centering_offset\":" << offset << ",\"image\":" << json(stem + ".pgm")
         << ",\"band_guide_image\":" << json(stem + "-bands.pgm")
         << ",\"row_geometry\":\"X4 Pro touch list: max(56, primary line height + 16); isolated lines\""
         << ",\"rows\":[";
  auto draw = [&](const char* api, const std::string& source, const std::string& text, int width,
                  EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    require(rowY + rowHeight + 8 < HEIGHT, "Probe rows exceed canvas");
    // FreeInkUIGfxRenderer.h:307/313 centers in the supplied text rect.
    const int drawY = rowY + (rowHeight - height) / 2;
    const auto primaryMissing = missing(primaryFont, text, style);
    const auto fallbackMissing = missing(fallbackFont, text, style);
    const bool redirected = !primaryMissing.empty();
    const int effective = redirected ? fallback : ui.id;
    const auto& effectiveMissing = redirected ? fallbackMissing : primaryMissing;
    const std::string label = stem + ": " + api + ": " + text;
    failures.check(effectiveMissing.empty(), label + ": missing codepoint coverage");
    const int advance = renderer.getTextAdvanceX(ui.id, text.c_str(), style);
    const int renderAdvance = renderer.getTextAdvanceX(ui.id, text.c_str(), style, 0,
        BidiUtils::BidiBaseDir::AUTO, GfxRenderer::TextMeasureMode::Rendered);
    const int directAdvance = renderer.getTextAdvanceX(effective, text.c_str(), style);
    renderer.clearScreen();
    renderer.drawText(ui.id, LEFT, drawY, text.c_str(), true, style);
    const Pixels pixels = capture(renderer);
    const auto ink = bounds(pixels);
    // Compare actual fallback routing AND baseline centering against direct
    // rendering, not a replacement glyph-placement implementation.
    renderer.clearScreen();
    renderer.drawText(effective, LEFT, drawY + (redirected ? offset : 0), text.c_str(), true, style);
    const bool routingMatches = pixels == capture(renderer);
    const bool inBand = ink[2] > 0 && ink[3] > 0 && ink[1] >= rowY && ink[1] + ink[3] <= rowY + rowHeight;
    const bool inPrimaryLine = ink[1] >= drawY && ink[1] + ink[3] <= drawY + height;
    failures.check(routingMatches && advance == directAdvance, label + ": fallback routing/centering mismatch");
    failures.check(inBand, label + ": ink outside X4 Pro touch row band or missing");
    failures.check(advance == renderAdvance, label + ": layout/render advance mismatch");
    failures.check(renderer.getTextWidth(ui.id, text.c_str(), style) <= width, label + ": UI width exceeded");
    for (size_t i = 0; i < sheet.size(); ++i) sheet[i] = std::min(sheet[i], pixels[i]);
    for (int x = 0; x < WIDTH; ++x) {
      guides[(rowY - 1) * WIDTH + x] = 192;
      guides[(rowY + rowHeight) * WIDTH + x] = 192;
    }
    if (!first) report << ',';
    first = false;
    report << "{\"api\":" << json(api) << ",\"source\":" << json(source) << ",\"text\":" << json(text)
           << ",\"style\":" << unsigned(style) << ",\"effective_font_id\":" << effective
           << ",\"fallback_used\":" << (redirected ? "true" : "false") << ",\"max_width\":" << width
           << ",\"primary_missing_codepoints\":";
    setJson(report, primaryMissing);
    report << ",\"effective_missing_codepoints\":"; setJson(report, effectiveMissing);
    report << ",\"advance\":" << advance << ",\"render_advance\":" << renderAdvance
           << ",\"baseline\":" << drawY + (redirected ? offset : 0) + renderer.getFontAscenderSize(effective)
           << ",\"row_band\":"; arrayJson(report, {LEFT, rowY, width, rowHeight});
    report << ",\"primary_line_box\":"; arrayJson(report, {LEFT, drawY, width, height});
    report << ",\"ink_inside_primary_line_box\":" << (inPrimaryLine ? "true" : "false");
    report << ",\"ink_bounds\":"; arrayJson(report, ink);
    report << ",\"ink_inside_row_band\":" << (inBand ? "true" : "false")
           << ",\"direct_font_pixels_match\":" << (routingMatches ? "true" : "false") << '}';
    rowY += rowHeight + 6;  // ThemeTokens::listTouchRowGap.
  };
  const std::string words = "ตั้ง เรียง ที่ นั้น ยิ่ง ครั้ง";
  const std::string title = "หนังสือ ภาษาไทย " + words;
  const std::string filename = "หนังสือภาษาไทย2.epub";
  draw("drawText_ascii_control", "Reader 2.epub", "Reader 2.epub", WIDTH - 2 * LEFT);
  draw("drawText_photo_words", words, words, WIDTH - 2 * LEFT);
  draw("truncatedText_title", title, renderer.truncatedText(ui.id, title.c_str(), 300), 300);
  draw("truncatedText_filename", filename, renderer.truncatedText(ui.id, filename.c_str(), WIDTH - 2 * LEFT), WIDTH - 2 * LEFT);
  const std::string mixed = "EPUB Editor " + words + " Version 4.2.0 ภาษาไทย";
  draw("truncatedText_mixed", mixed, renderer.truncatedText(ui.id, mixed.c_str(), 250), 250);
  for (const auto& line : renderer.wrappedText(ui.id, mixed.c_str(), 250, 3)) {
    draw("wrappedText_mixed", mixed, line, 250);
  }
  draw("truncatedText_bold", title, renderer.truncatedText(ui.id, title.c_str(), 300, EpdFontFamily::BOLD),
       300, EpdFontFamily::BOLD);
  for (size_t i = 0; i < sheet.size(); ++i) guides[i] = std::min(guides[i], sheet[i]);
  writePgm(output / (stem + ".pgm"), sheet);
  writePgm(output / (stem + "-bands.pgm"), guides);
  report << ']';
  if (ui.size == 8) {
    // BaseTheme.cpp:798/899-930, UITheme.cpp:148-155 and the normal reader
    // call (paddingBottom=0, textYOffset=0). Text lane only, no progress bar,
    // zero viewable bottom inset: translating an inset does not change the
    // lane-relative ink. The real renderer clips at the display, NOT at the
    // 19px layout reservation. Report any reservation intrusion separately.
    constexpr int reservedHeight = 19;
    constexpr int reservedTop = HEIGHT - reservedHeight;
    constexpr int statusDrawY = reservedTop - 4;
    const std::string statusTitle = renderer.truncatedText(ui.id, title.c_str(), 300);
    const int statusX = (WIDTH - renderer.getTextWidth(ui.id, statusTitle.c_str())) / 2;
    renderer.clearScreen();
    renderer.drawText(ui.id, statusX, 400, statusTitle.c_str());
    auto unboundedInk = bounds(capture(renderer));
    unboundedInk[1] += statusDrawY - 400;
    const bool insideDisplay = unboundedInk[0] >= 0 && unboundedInk[1] >= 0 &&
        unboundedInk[0] + unboundedInk[2] <= WIDTH && unboundedInk[1] + unboundedInk[3] <= HEIGHT;
    failures.check(insideDisplay, stem + ": actual status title clipped by display bounds");
    renderer.clearScreen();
    renderer.drawText(ui.id, statusX, statusDrawY, statusTitle.c_str());
    const auto statusPixels = capture(renderer);
    writePgm(output / (stem + "-status.pgm"), statusPixels);
    report << ",\"status_surface\":{\"image\":" << json(stem + "-status.pgm")
           << ",\"settings\":\"text lane only; paddingBottom=0; textYOffset=0; bottom inset=0\""
           << ",\"text\":" << json(statusTitle) << ",\"draw_y\":" << statusDrawY
           << ",\"reserved_band\":"; arrayJson(report, {0, reservedTop, WIDTH, reservedHeight});
    report << ",\"projected_unclipped_ink_bounds\":"; arrayJson(report, unboundedInk);
    report << ",\"actual_ink_bounds\":"; arrayJson(report, bounds(statusPixels));
    report << ",\"inside_display_clip\":" << (insideDisplay ? "true" : "false")
           << ",\"pixels_above_layout_reservation\":" << std::max(0, reservedTop - unboundedInk[1])
           << ",\"body_collision_verified\":false}";
  }
  report << '}';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root, output;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--help") {
        std::cout << "ThaiUiProbe --font-root DIR --output DIR\nDIR is an extracted SD root containing fonts/, or that fonts/ directory.\n"
                     "Production font/fallback surface probe, not a full BaseTheme/FUI page emulation.\n";
        return 0;
      }
      require(i + 1 < argc, "Missing argument value");
      if (arg == "--font-root") root = argv[++i];
      else if (arg == "--output") output = argv[++i];
      else throw std::runtime_error("Unknown option " + arg);
    }
    require(!root.empty() && !output.empty(), "--font-root and --output are required");
    root = std::filesystem::absolute(root).lexically_normal();
    if (root.filename() == "fonts" || root.filename() == ".fonts") root = root.parent_path();
    require(std::filesystem::is_directory(root), "Font root does not exist");
    probe::sdRoot = root;
    std::filesystem::create_directories(output);
    SdCardFontRegistry registry;
    require(registry.discover(), "Production registry discovered no installable fonts");
    HalDisplay display(HEIGHT, WIDTH);
    GfxRenderer renderer(display);
    renderer.begin();
    renderer.setOrientation(GfxRenderer::Portrait);
    EpdFont small(&notosans_8_regular), ui10(&ubuntu_10_regular), ui10Bold(&ubuntu_10_bold),
        ui12(&ubuntu_12_regular), ui12Bold(&ubuntu_12_bold);
    renderer.insertFont(SMALL_FONT_ID, EpdFontFamily(&small));
    renderer.insertFont(UI_10_FONT_ID, EpdFontFamily(&ui10, &ui10Bold));
    renderer.insertFont(UI_12_FONT_ID, EpdFontFamily(&ui12, &ui12Bold));
    FontDecompressor decompressor;
    decompressor.init();
    FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
    cache.setFontDecompressor(&decompressor);
    renderer.setFontCacheManager(&cache);
    SdCardFontManager manager;
    Failures failures;
    std::ofstream report(output / "report.json");
    require(bool(report), "Cannot create report.json");
    report << "{\"schema_version\":1,\"scope\":\"production font/fallback surface probe; not full BaseTheme/FUI page emulation\","
              "\"hardware_verified\":false,\"image_format\":\"PGM P5, logical portrait coordinates\",\"width\":480,\"height\":800,"
              "\"sd_root\":" << json(root.string()) << ",\"families\":[";
    bool firstFamily = true;
    for (const auto& family : registry.getFamilies()) {
      require(family.hasSize(16), family.name + ": exact reader size 16 absent");
      require(manager.loadFamily(family, renderer, 16), "Reader font failed to load: " + family.name);
      const int readerId = manager.getFontId(family.name);
      require(renderer.getFontMap().at(readerId).hasCodepoint(0x0E01), "Reader font lacks Thai coverage");
      if (!firstFamily) report << ',';
      firstFamily = false;
      report << "{\"name\":" << json(family.name) << ",\"reader_font_id\":" << readerId
             << ",\"discovered_files\":[";
      bool firstFile = true;
      for (const auto& file : family.files) {
        if (!firstFile) report << ',';
        firstFile = false;
        report << "{\"path\":" << json(file.path) << ",\"point_size\":" << unsigned(file.pointSize) << '}';
      }
      report << "],\"sizes\":[";
      bool firstSize = true;
      for (const auto& ui : UI) {
        const int id = manager.loadFamilyExtraSize(family, renderer, ui.size);
        require(id != 0, family.name + ": exact UI font size missing: " + std::to_string(ui.size));
        const auto* loaded = renderer.getSdCardFonts().at(id);
        require(manager.loadFamilyExtraSize(family, renderer, ui.size) == id &&
                    renderer.getSdCardFonts().at(id) == loaded, "Exact-size font cache was not reused");
        renderer.setFallbackFont(ui.id, id);
        if (!firstSize) report << ',';
        firstSize = false;
        renderSize(renderer, family, ui, id, output, report, failures);
      }
      require(renderer.getSdCardFonts().size() == 4, "Expected one reader and three exact UI fonts");
      manager.unloadAll(renderer);
      require(renderer.getSdCardFonts().empty() && renderer.getFontMap().size() == 3 &&
                  manager.getFontId(family.name) == 0, "Font teardown retained registrations");
      // Exercise normal built-in rendering after SD family teardown.
      renderer.clearScreen();
      renderer.drawText(SMALL_FONT_ID, LEFT, 24, "Reader 2.epub");
      const auto afterUnload = capture(renderer);
      renderer.clearFallbackFonts();
      renderer.clearScreen();
      renderer.drawText(SMALL_FONT_ID, LEFT, 24, "Reader 2.epub");
      require(afterUnload == capture(renderer), "Teardown changed built-in rendering");
      report << "],\"exact_size_cache_reused\":true,\"unload_verified\":true}";
    }
    report << "],\"sd_read_calls\":" << probe::readCalls << ",\"sd_read_bytes\":" << probe::readBytes
           << ",\"passed\":" << (failures.messages.empty() ? "true" : "false") << ",\"failures\":[";
    for (size_t i = 0; i < failures.messages.size(); ++i) {
      if (i) report << ',';
      report << json(failures.messages[i]);
      std::cerr << failures.messages[i] << '\n';
    }
    report << "]}\n";
    require(bool(report), "Report write failed");
    std::cout << "Production UI fallback probe: " << registry.getFamilyCount() << " families; "
              << failures.messages.size() << " failures; " << (output / "report.json").string() << '\n';
    return failures.messages.empty() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "ThaiUiProbe: " << error.what() << '\n';
    return 1;
  }
}
