#include <BuildScratch.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/converters/ImageDecoderFactory.h"

namespace {
// Only the production inflater's heap is denied. JPEGDEC, the renderer, and
// GoogleTest continue to use their ordinary allocators.
bool denyInflateHeap = false;
size_t inflateAllocationAttempts = 0;
size_t liveInflateAllocations = 0;
void* imageTestMalloc(size_t bytes) {
  ++inflateAllocationAttempts;
  if (denyInflateHeap) return nullptr;
  void* result = std::malloc(bytes);
  if (result) ++liveInflateAllocations;
  return result;
}
void imageTestFree(void* pointer) {
  if (pointer) --liveInflateAllocations;
  std::free(pointer);
}
}  // namespace

// Do not also compile InflateStream.cpp as a separate target source.
#define malloc imageTestMalloc
#define free imageTestFree
#include "../../lib/miniz/src/InflateStream.cpp"
#undef free
#undef malloc

namespace {
namespace fs = std::filesystem;
constexpr int IMAGE_WIDTH = 200;
constexpr int IMAGE_HEIGHT = 300;
constexpr size_t CACHE_BYTES = 15004;
constexpr const char* SOURCE = "OEBPS/images/grayscale_test.jpg";
using Planes = std::array<std::vector<uint8_t>, 3>;  // BW, MSB, LSB
constexpr std::array<GfxRenderer::RenderMode, 3> MODES = {GfxRenderer::BW, GfxRenderer::GRAYSCALE_MSB,
                                                          GfxRenderer::GRAYSCALE_LSB};

// Expected 4x4 output tiles for the fixture's gray 96 and 160 interiors.
// Bayer phase is screen-relative; a square's exact center need not match its nominal level.
unsigned expectedLevel(int square, int x, int y) {
  static constexpr uint8_t dark[4][4] = {{0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 0, 1}, {2, 1, 1, 1}};
  static constexpr uint8_t light[4][4] = {{1, 2, 2, 2}, {2, 2, 2, 2}, {2, 2, 1, 2}, {3, 2, 2, 2}};
  return square == 1 ? dark[y & 3][x & 3] : square == 2 ? light[y & 3][x & 3] : square;
}

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<uint8_t> readBytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(static_cast<bool>(input), "Cannot read " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(output), "Cannot write " + path.string());
}

struct TempDirectory {
  fs::path path;
  TempDirectory() {
    static unsigned sequence = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path parent = fs::temp_directory_path();
    do {
      path = parent / ("image-preparation-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
    } while (!fs::create_directory(path));
  }
  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};

struct ImageCase {
  uint16_t width;
  uint16_t height;
  GfxRenderer::Orientation orientation;
  const char* name;
};

const std::array<ImageCase, 8> CASES = {{
    {792, 528, GfxRenderer::Portrait, "X3_Portrait"},
    {792, 528, GfxRenderer::PortraitInverted, "X3_Inverted"},
    {792, 528, GfxRenderer::LandscapeClockwise, "X3_CW"},
    {792, 528, GfxRenderer::LandscapeCounterClockwise, "X3_CCW"},
    {800, 480, GfxRenderer::Portrait, "X4_Portrait"},
    {800, 480, GfxRenderer::PortraitInverted, "X4_Inverted"},
    {800, 480, GfxRenderer::LandscapeClockwise, "X4_CW"},
    {800, 480, GfxRenderer::LandscapeCounterClockwise, "X4_CCW"},
}};

struct Extractor {
  GfxRenderer* renderer = nullptr;
  std::vector<uint8_t> compressed;
  size_t expectedBytes = 0;
  std::string truncatedSource;
  bool leavePartialOnFailure = false;
  size_t calls = 0;
  size_t borrowedCalls = 0;
  size_t releasedClaims = 0;
  size_t partialBytes = 0;
  size_t inputOffset = 0;
  size_t inputLimit = 0;

  static size_t fill(void* context, const uint8_t** data) {
    auto& self = *static_cast<Extractor*>(context);
    const size_t count = std::min<size_t>(1024, self.inputLimit - self.inputOffset);
    *data = self.compressed.data() + self.inputOffset;
    self.inputOffset += count;
    return count;
  }

  static bool extract(void* context, const char* source, const char* destination) {
    auto& self = *static_cast<Extractor*>(context);
    ++self.calls;
    if (!self.renderer->hasFrameBuffer()) ++self.borrowedCalls;
    bool success = false;
    size_t total = 0;
    {
      // The callback never lends memory. Production ImageBlock::prepare must
      // have done that already, and this stream dies before the callback exits.
      HalFile output;
      if (!Storage.openFileForWrite("TEST", destination, output)) return false;
      {
        InflateStream stream;
        if (stream.init(true)) {
          self.inputOffset = 0;
          self.inputLimit = self.truncatedSource == source ? self.compressed.size() / 2 : self.compressed.size();
          stream.setFill(fill, &self);
          std::array<uint8_t, 8192> chunk{};
          for (;;) {
            size_t produced = 0;
            const auto status = stream.readAtMost(chunk.data(), chunk.size(), &produced);
            if (produced && output.write(chunk.data(), produced) != produced) break;
            total += produced;
            if (status == InflateStream::Status::Error) break;
            if (status == InflateStream::Status::Done) {
              success = total == self.expectedBytes;
              break;
            }
            if (!produced) break;
          }
        }
      }
      success = output.close() && success;
    }
    // Also prove the stream relinquished its exclusive claim while ownership
    // still belongs to prepare's loan (not merely that the framebuffer returns).
    uint8_t* claim = buildscratch::claim(self.renderer->getBufferSize());
    if (claim) {
      ++self.releasedClaims;
      buildscratch::release(claim);
    }
    if (!success) {
      self.partialBytes = total;
      if (!self.leavePartialOnFailure) Storage.remove(destination);
    }
    return success;
  }
};

struct Scenario {
  TempDirectory directory;
  HalDisplay display;
  GfxRenderer renderer;
  Extractor extractor;
  ImageCase config;

  explicit Scenario(ImageCase imageCase)
      : display(imageCase.width, imageCase.height), renderer(display), config(imageCase) {
    ImageBlock::releaseRenderCache();
    ImageBlock::clearRenderFailures();
    buildscratch::reclaim();
    denyInflateHeap = true;
    inflateAllocationAttempts = 0;
    liveInflateAllocations = 0;
    renderer.begin();
    renderer.setOrientation(imageCase.orientation);
    extractor.renderer = &renderer;
    extractor.compressed = readBytes(fs::path(IMAGE_PREPARATION_FIXTURE_DIR) / "grayscale.rawdeflate");
    extractor.expectedBytes = readBytes(fs::path(IMAGE_PREPARATION_FIXTURE_DIR) / "grayscale.jpg").size();
    require(extractor.expectedBytes == 27208, "Checked-in JPEG fixture changed unexpectedly");
    ImageBlock::setExtractor(&extractor, Extractor::extract);
  }
  ~Scenario() {
    ImageBlock::setExtractor(nullptr, nullptr);
    ImageBlock::releaseRenderCache();
    ImageBlock::clearRenderFailures();
    buildscratch::reclaim();
    denyInflateHeap = false;
    inflateAllocationAttempts = 0;
    liveInflateAllocations = 0;
  }

  fs::path source(const std::string& name = "image") const { return directory.path / (name + ".jpg"); }
  fs::path cache(const std::string& name = "image") const { return directory.path / (name + ".pxc"); }

  const ImageBlock& addImage(Page& page, const std::string& name = "image", int x = 0, int y = 0,
                             const std::string& sourceName = SOURCE) {
    auto block = std::make_unique<ImageBlock>(source(name).string(), sourceName, IMAGE_WIDTH, IMAGE_HEIGHT);
    const auto* pointer = block.get();
    page.elements.emplace_back(std::make_unique<PageImage>(std::move(block), x, y));
    return *pointer;
  }

  void requireOwnership() {
    require(renderer.hasFrameBuffer(), "Framebuffer ownership was not restored");
    require(renderer.getFrameBuffer() == display.getFrameBuffer(), "Framebuffer pointer changed");
    require(buildscratch::claim(1) == nullptr, "Scratch remains published after prepare");
    require(liveInflateAllocations == 0, "Inflater leaked heap storage");
    require(inflateAllocationAttempts == 0, "Cold extraction attempted denied heap allocation");
    require(extractor.borrowedCalls == extractor.calls, "Extractor ran without production framebuffer loan");
    require(extractor.releasedClaims == extractor.calls, "Inflater claim outlived the extraction callback");
  }

  std::vector<uint8_t> requireCache(const std::string& name = "image", int xOffset = 0, int yOffset = 0) const {
    const auto bytes = readBytes(cache(name));
    require(bytes.size() == CACHE_BYTES, "PXC must contain exactly 15004 bytes");
    const unsigned width = bytes[0] | (static_cast<unsigned>(bytes[1]) << 8);
    const unsigned height = bytes[2] | (static_cast<unsigned>(bytes[3]) << 8);
    require(width == IMAGE_WIDTH && height == IMAGE_HEIGHT, "PXC header dimensions differ");
    for (int i = 0; i < 4; ++i) {
      const int centerY = 50 + 51 * i;
      for (int dy = 0; dy < 4; ++dy) {
        for (int dx = 0; dx < 4; ++dx) {
          const int x = 100 + dx, y = centerY + dy;
          const unsigned pixel = (bytes[4 + y * 50 + x / 4] >> (6 - 2 * (x % 4))) & 3;
          require(pixel == expectedLevel(i, xOffset + x, yOffset + y),
                  "PXC grayscale square " + std::to_string(i) + " differs");
        }
      }
    }
    return bytes;
  }

  Planes captureDrawing(const std::function<void()>& draw, const fs::path& blockedCache = {}) {
    ImageBlock::releaseRenderCache();
    Planes result;
    for (size_t i = 0; i < MODES.size(); ++i) {
      // PixelCache abort removes an empty blocker directory. Recreate it for
      // every plane so the fallback test exercises real decoding each time.
      if (!blockedCache.empty()) fs::create_directory(blockedCache);
      renderer.setRenderMode(MODES[i]);
      renderer.clearScreen(i == 0 ? 0xff : 0);
      draw();
      result[i].assign(renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize());
    }
    renderer.setRenderMode(GfxRenderer::BW);
    return result;
  }

  Planes capture(const Page& page, int xOffset = 0, int yOffset = 0, const fs::path& blockedCache = {}) {
    return captureDrawing([&] { page.render(renderer, 0, xOffset, yOffset); }, blockedCache);
  }

  uint8_t grayAt(const Planes& planes, int logicalX, int logicalY) const {
    int x = 0, y = 0;
    switch (config.orientation) {
      case GfxRenderer::Portrait:
        x = logicalY;
        y = config.height - 1 - logicalX;
        break;
      case GfxRenderer::PortraitInverted:
        x = config.width - 1 - logicalY;
        y = logicalX;
        break;
      case GfxRenderer::LandscapeClockwise:
        x = config.width - 1 - logicalX;
        y = config.height - 1 - logicalY;
        break;
      case GfxRenderer::LandscapeCounterClockwise:
        x = logicalX;
        y = logicalY;
        break;
    }
    const size_t position = y * renderer.getDisplayWidthBytes() + x / 8;
    const uint8_t mask = 0x80 >> (x % 8);
    return (planes[0][position] & mask)    ? 255
           : !(planes[1][position] & mask) ? 0
           : (planes[2][position] & mask)  ? 85
                                           : 170;
  }

  void requirePixels(const Planes& planes, int xOffset = 0, int yOffset = 0) const {
    for (int i = 0; i < 4; ++i) {
      for (int dy = 0; dy < 4; ++dy) {
        for (int dx = 0; dx < 4; ++dx) {
          const int x = xOffset + 100 + dx, y = yOffset + 50 + 51 * i + dy;
          require(grayAt(planes, x, y) == expectedLevel(i, x, y) * 85,
                  "Rendered grayscale square " + std::to_string(i) + " differs");
        }
      }
    }
    require(grayAt(planes, renderer.getScreenWidth() - 1, renderer.getScreenHeight() - 1) == 255,
            "Extraction scribbles escaped the image rectangle");
  }
};

void writePgm(const fs::path& path, const Scenario& scenario, const Planes& planes) {
  std::ofstream output(path, std::ios::binary);
  output << "P5\n" << scenario.config.width << ' ' << scenario.config.height << "\n255\n";
  for (int y = 0; y < scenario.config.height; ++y) {
    for (int x = 0; x < scenario.config.width; ++x) {
      const size_t position = y * scenario.renderer.getDisplayWidthBytes() + x / 8;
      const uint8_t mask = 0x80 >> (x % 8);
      // Same BW/overlay-plane combination as ThaiRenderProbe.
      const uint8_t value = (planes[0][position] & mask)    ? 255
                            : !(planes[1][position] & mask) ? 0
                            : (planes[2][position] & mask)  ? 85
                                                            : 170;
      output.put(static_cast<char>(value));
    }
  }
  require(static_cast<bool>(output), "Cannot write " + path.string());
}

// Shared by the regression and --smoke-output: neither substitutes mocked pixels.
void coldWarmScenario(ImageCase imageCase, const fs::path& outputDirectory = {}, std::ostream* report = nullptr) {
  Scenario scenario(imageCase);
  Page page;
  const auto& image = scenario.addImage(page);
  require(page.hasImagesNeedingDecode(), "Cold page did not require preparation");
  const uint32_t loans = scenario.renderer.frameBufferLoanCount();
  page.prepareImages(scenario.renderer, 0, 0);
  scenario.requireOwnership();
  require(scenario.renderer.frameBufferLoanCount() == loans + 1, "Cold image did not use exactly one loan");
  require(scenario.extractor.calls == 1, "Cold page did not extract exactly once");
  require(image.hasValidCache() && !page.hasImagesNeedingDecode(), "Prepared page has no valid cache");
  const auto cache = scenario.requireCache();
  require(readBytes(scenario.source()) == readBytes(fs::path(IMAGE_PREPARATION_FIXTURE_DIR) / "grayscale.jpg"),
          "Inflated source differs from checked-in JPEG");
  const Planes cold = scenario.capture(page);  // Clears all temporary preparation pixels first.
  scenario.requirePixels(cold);
  require(scenario.extractor.calls == 1, "Drawing unexpectedly extracted the source");

  // A warm page must work without either a source file or extraction service.
  fs::remove(scenario.source());
  ImageBlock::setExtractor(nullptr, nullptr);
  ImageBlock::releaseRenderCache();
  ImageBlock::clearRenderFailures();
  scenario.renderer.clearScreen(0xa5);
  const std::vector<uint8_t> untouched(scenario.renderer.getFrameBuffer(),
                                       scenario.renderer.getFrameBuffer() + scenario.renderer.getBufferSize());
  require(image.prepare(scenario.renderer, 0, 0), "Valid-cache prepare failed without source/extractor");
  page.prepareImages(scenario.renderer, 0, 0);
  require(scenario.renderer.frameBufferLoanCount() == loans + 1, "Warm preparation borrowed the framebuffer");
  require(std::equal(untouched.begin(), untouched.end(), scenario.renderer.getFrameBuffer()),
          "Warm preparation rendered pixels");
  require(scenario.requireCache() == cache, "Warm preparation changed the PXC");
  const Planes warm = scenario.capture(page);
  require(cold == warm, "Cold/warm BW, MSB or LSB planes differ");
  scenario.requirePixels(warm);
  scenario.requireOwnership();
  if (!outputDirectory.empty()) {
    const std::string prefix =
        std::to_string(imageCase.width) + "x" + std::to_string(imageCase.height) + "-" + imageCase.name;
    writePgm(outputDirectory / (prefix + "-cold.pgm"), scenario, cold);
    writePgm(outputDirectory / (prefix + "-warm.pgm"), scenario, warm);
  }
  if (report) {
    *report << imageCase.name << ' ' << imageCase.width << 'x' << imageCase.height
            << " PXC=15004 header=200x300 centers=0,1,1,3 four_level_tiles=verified cold_warm_BW_MSB_LSB=identical"
               " extraction_calls=1 inflater_heap_attempts=0 ownership=restored\n";
  }
}

class ImagePreparationTest : public ::testing::TestWithParam<ImageCase> {};

TEST_P(ImagePreparationTest, ColdPrepareAndSourcelessWarmCacheHaveIdenticalPlanes) { coldWarmScenario(GetParam()); }

TEST_P(ImagePreparationTest, HeapDenialRequiresTheProductionLoan) {
  Scenario scenario(GetParam());
  {
    InflateStream stream;
    EXPECT_FALSE(stream.init(true));
  }
  EXPECT_GT(inflateAllocationAttempts, 0u);
  EXPECT_EQ(liveInflateAllocations, 0u);
  inflateAllocationAttempts = 0;
  Page page;
  scenario.addImage(page);
  page.prepareImages(scenario.renderer, 0, 0);
  scenario.requireCache();
  scenario.requireOwnership();
}

TEST_P(ImagePreparationTest, SecondImageKeepsFirstCacheAndOtherPageRemainsCold) {
  Scenario scenario(GetParam());
  Page page, other;
  const auto& first = scenario.addImage(page, "first", 3, 4);
  const auto& second = scenario.addImage(page, "second", 210, 4);
  const auto& isolated = scenario.addImage(other, "isolated");
  require(first.prepare(scenario.renderer, 10, 13), "First prepare failed");
  const auto firstCache = scenario.requireCache("first", 10, 13);
  page.prepareImages(scenario.renderer, 7, 9);
  scenario.requireOwnership();
  EXPECT_EQ(scenario.extractor.calls, 2u);
  EXPECT_EQ(scenario.requireCache("first", 10, 13), firstCache);
  scenario.requireCache("second", 217, 13);
  EXPECT_TRUE(second.hasValidCache());
  EXPECT_TRUE(isolated.needsDecode());
  EXPECT_FALSE(fs::exists(scenario.source("isolated")));
  EXPECT_FALSE(fs::exists(scenario.cache("isolated")));
  const Planes planes = scenario.capture(page, 7, 9);
  scenario.requirePixels(planes, 10, 13);
  scenario.requirePixels(planes, 217, 13);
}

TEST_P(ImagePreparationTest, CorruptAndTruncatedCachesRegenerate) {
  Scenario scenario(GetParam());
  Page page;
  scenario.addImage(page);
  page.prepareImages(scenario.renderer, 0, 0);
  const auto good = scenario.requireCache();
  const Planes baseline = scenario.capture(page);
  for (bool corruptHeader : {false, true}) {
    ImageBlock::releaseRenderCache();
    ImageBlock::clearRenderFailures();
    auto damaged = good;
    if (corruptHeader)
      damaged[0] = 0;
    else
      damaged.resize(damaged.size() - 1);
    writeBytes(scenario.cache(), damaged);
    fs::remove(scenario.source());
    EXPECT_TRUE(page.hasImagesNeedingDecode());
    page.prepareImages(scenario.renderer, 0, 0);
    scenario.requireOwnership();
    EXPECT_EQ(scenario.requireCache(), good);
    EXPECT_EQ(scenario.capture(page), baseline);
  }
  EXPECT_EQ(scenario.extractor.calls, 3u);
}

TEST_P(ImagePreparationTest, TruncatedInflateLeavesPlaceholderSuppressesRetryThenRecovers) {
  Scenario scenario(GetParam());
  Page page;
  const auto& image = scenario.addImage(page);
  scenario.extractor.truncatedSource = SOURCE;
  page.prepareImages(scenario.renderer, 0, 0);
  scenario.requireOwnership();
  EXPECT_TRUE(std::all_of(scenario.renderer.getFrameBuffer(),
                          scenario.renderer.getFrameBuffer() + scenario.renderer.getBufferSize(),
                          [](uint8_t byte) { return byte == 0xff; }));
  EXPECT_GT(scenario.extractor.partialBytes, 0u);
  EXPECT_FALSE(fs::exists(scenario.source()));
  EXPECT_FALSE(image.hasValidCache());
  EXPECT_FALSE(fs::exists(scenario.cache()));
  EXPECT_FALSE(image.needsDecode());  // Failure latch, not a valid-cache claim.
  const Planes failed = scenario.capture(page);
  const Planes placeholder = scenario.captureDrawing([&] { image.renderPlaceholder(scenario.renderer, 0, 0); });
  EXPECT_EQ(failed, placeholder);
  scenario.extractor.truncatedSource.clear();
  EXPECT_FALSE(image.prepare(scenario.renderer, 0, 0));
  page.prepareImages(scenario.renderer, 0, 0);
  EXPECT_EQ(scenario.extractor.calls, 1u);
  ImageBlock::clearRenderFailures();
  EXPECT_TRUE(image.needsDecode());
  page.prepareImages(scenario.renderer, 0, 0);
  scenario.requireOwnership();
  scenario.requireCache();
  scenario.requirePixels(scenario.capture(page));
  EXPECT_EQ(scenario.extractor.calls, 2u);
}

TEST_P(ImagePreparationTest, FailedExtractorPartialFileIsRemovedDefensively) {
  Scenario scenario(GetParam());
  Page page;
  const auto& image = scenario.addImage(page);
  scenario.extractor.truncatedSource = SOURCE;
  scenario.extractor.leavePartialOnFailure = true;
  EXPECT_FALSE(image.prepare(scenario.renderer, 0, 0));
  scenario.requireOwnership();
  EXPECT_GT(scenario.extractor.partialBytes, 0u);
  EXPECT_FALSE(fs::exists(scenario.source()));
  EXPECT_FALSE(image.hasValidCache());
}

TEST_P(ImagePreparationTest, InvalidJpegDecodeIsSuppressedUntilNextRender) {
  Scenario scenario(GetParam());
  Page page;
  const auto& image = scenario.addImage(page);
  writeBytes(scenario.source(), {0, 1, 2, 3, 4, 5, 6, 7});
  EXPECT_FALSE(image.prepare(scenario.renderer, 0, 0));
  EXPECT_FALSE(image.hasValidCache());
  EXPECT_FALSE(image.needsDecode());
  scenario.requireOwnership();
  const auto placeholder = scenario.captureDrawing([&] { image.renderPlaceholder(scenario.renderer, 0, 0); });
  EXPECT_EQ(scenario.capture(page), placeholder);
  fs::copy_file(fs::path(IMAGE_PREPARATION_FIXTURE_DIR) / "grayscale.jpg", scenario.source(),
                fs::copy_options::overwrite_existing);
  const auto reads = probe::readCalls;
  EXPECT_FALSE(image.prepare(scenario.renderer, 0, 0));
  EXPECT_EQ(probe::readCalls, reads);
  ImageBlock::clearRenderFailures();
  EXPECT_TRUE(image.prepare(scenario.renderer, 0, 0));
  scenario.requireCache();
  scenario.requirePixels(scenario.capture(page));
  EXPECT_EQ(scenario.extractor.calls, 0u);
  EXPECT_EQ(scenario.renderer.frameBufferLoanCount(), 0u);
  scenario.requireOwnership();
}

TEST_P(ImagePreparationTest, FailedImageDoesNotStopLaterPageImages) {
  Scenario scenario(GetParam());
  Page page;
  const auto& bad = scenario.addImage(page, "bad", 0, 0, "broken-source");
  const auto& good = scenario.addImage(page, "good", 210, 0);
  scenario.extractor.truncatedSource = "broken-source";
  page.prepareImages(scenario.renderer, 0, 0);
  scenario.requireOwnership();
  EXPECT_EQ(scenario.extractor.calls, 2u);
  EXPECT_FALSE(bad.hasValidCache());
  EXPECT_TRUE(good.hasValidCache());
  EXPECT_FALSE(fs::exists(scenario.source("bad")));
  scenario.requireCache("good", 210, 0);
  scenario.requirePixels(scenario.capture(page), 210, 0);
}

TEST_P(ImagePreparationTest, CacheWriteFailureKeepsSuccessfulDecodeAndDirectDrawFallback) {
  Scenario scenario(GetParam());
  Page baselinePage, fallbackPage;
  scenario.addImage(baselinePage, "baseline");
  baselinePage.prepareImages(scenario.renderer, 0, 0);
  scenario.requireCache("baseline");
  const Planes baseline = scenario.capture(baselinePage);
  const auto& fallback = scenario.addImage(fallbackPage, "fallback");
  fs::copy_file(scenario.source("baseline"), scenario.source("fallback"));
  ImageBlock::releaseRenderCache();
  fs::create_directory(scenario.cache("fallback"));
  EXPECT_TRUE(fallback.prepare(scenario.renderer, 0, 0));
  EXPECT_FALSE(fallback.hasValidCache());
  EXPECT_TRUE(fallback.needsDecode());  // Decode success must not set the failure latch.
  const size_t calls = scenario.extractor.calls;
  const Planes direct = scenario.capture(fallbackPage, 0, 0, scenario.cache("fallback"));
  EXPECT_EQ(direct, baseline);
  scenario.requirePixels(direct);
  EXPECT_TRUE(fallback.needsDecode());
  EXPECT_FALSE(fallback.hasValidCache());
  EXPECT_EQ(scenario.extractor.calls, calls);
  scenario.requireOwnership();
}

TEST_P(ImagePreparationTest, RenderNeverExtractsMissingSources) {
  Scenario scenario(GetParam());
  Page page;
  const auto& image = scenario.addImage(page);
  const auto loans = scenario.renderer.frameBufferLoanCount();
  const Planes actual = scenario.capture(page);
  const Planes placeholder = scenario.captureDrawing([&] { image.renderPlaceholder(scenario.renderer, 0, 0); });
  EXPECT_EQ(actual, placeholder);
  EXPECT_EQ(scenario.extractor.calls, 0u);
  EXPECT_EQ(scenario.renderer.frameBufferLoanCount(), loans);
  EXPECT_FALSE(fs::exists(scenario.source()));
  ImageBlock::clearRenderFailures();
  page.prepareImages(scenario.renderer, 0, 0);
  scenario.requireOwnership();
  scenario.requireCache();
}

TEST_P(ImagePreparationTest, MissingExtractorAndInvalidBoundsFailWithoutLoan) {
  Scenario scenario(GetParam());
  Page page;
  const auto& image = scenario.addImage(page);
  EXPECT_FALSE(image.prepare(scenario.renderer, scenario.renderer.getScreenWidth() - IMAGE_WIDTH + 1, 0));
  ImageBlock::clearRenderFailures();
  EXPECT_FALSE(image.prepare(scenario.renderer, 0, scenario.renderer.getScreenHeight() - IMAGE_HEIGHT + 1));
  ImageBlock::clearRenderFailures();
  EXPECT_FALSE(image.prepare(scenario.renderer, -1, 0));
  EXPECT_EQ(scenario.extractor.calls, 0u);
  EXPECT_EQ(scenario.renderer.frameBufferLoanCount(), 0u);
  ImageBlock::clearRenderFailures();
  ImageBlock::setExtractor(nullptr, nullptr);
  EXPECT_FALSE(image.prepare(scenario.renderer, 0, 0));
  EXPECT_FALSE(image.needsDecode());
  EXPECT_EQ(scenario.renderer.frameBufferLoanCount(), 0u);
  scenario.requireOwnership();
}

TEST(ImagePreparationFactoryTest, MatchesJpegExtensionsAndRejectsUnsupportedFormats) {
  EXPECT_NE(ImageDecoderFactory::getDecoder("a.jpg"), nullptr);
  EXPECT_NE(ImageDecoderFactory::getDecoder("a.JpEg"), nullptr);
  EXPECT_EQ(ImageDecoderFactory::getDecoder("a.png"), nullptr);
  EXPECT_EQ(ImageDecoderFactory::getDecoder("jpg"), nullptr);
}

INSTANTIATE_TEST_SUITE_P(BoardAndOrientation, ImagePreparationTest, ::testing::ValuesIn(CASES),
                         [](const ::testing::TestParamInfo<ImageCase>& info) { return info.param.name; });
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--smoke-output") {
    if (argc != 3) {
      std::cerr << "Usage: ImagePreparationTest --smoke-output DIR\n";
      return 2;
    }
    try {
      const fs::path output(argv[2]);
      fs::create_directories(output);
      std::ofstream report(output / "result.txt");
      require(static_cast<bool>(report), "Cannot open smoke result.txt");
      report << "Production JPEG/inflater/framebuffer host smoke; no physical-board acceptance implied.\n";
      try {
        for (const auto& imageCase : CASES) coldWarmScenario(imageCase, output, &report);
      } catch (const std::exception& error) {
        report << "FAIL: " << error.what() << '\n';
        throw;
      }
      report << "PASS: all geometries/orientations have identical cold/warm planes and expected grayscale pixels.\n";
      report.close();
      require(static_cast<bool>(report), "Failed writing smoke result.txt");
      return 0;
    } catch (const std::exception& error) {
      std::cerr << "Image preparation smoke: " << error.what() << '\n';
      return 1;
    }
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
