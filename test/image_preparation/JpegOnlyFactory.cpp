#include <cctype>

#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/JpegToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  const size_t dot = imagePath.rfind('.');
  std::string extension = dot == std::string::npos ? "" : imagePath.substr(dot);
  for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (!JpegToFramebufferConverter::supportsFormat(extension)) return nullptr;
  if (!jpegDecoder) jpegDecoder = std::make_unique<JpegToFramebufferConverter>();
  return jpegDecoder.get();
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) { return getDecoder(imagePath) != nullptr; }
