#include <Epub/blocks/ImageBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <stdexcept>

// Corpus documents contain text only. Image decoding is a board service outside
// this probe: refuse it explicitly rather than silently draw fake image pixels.
ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}
bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageToFramebufferDecoder::validateAndStoreDimensions(int64_t, int64_t, ImageDimensions&, const char*) { return false; }
void ImageBlock::render(GfxRenderer&, int, int) { throw std::runtime_error("ThaiRenderProbe does not support image blocks"); }
void ImageBlock::renderPlaceholder(GfxRenderer&, int, int) const { throw std::runtime_error("ThaiRenderProbe does not support image blocks"); }
bool ImageBlock::needsDecode() const { return false; }
bool ImageBlock::serialize(HalFile&) { return false; }
std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile&) { return nullptr; }
