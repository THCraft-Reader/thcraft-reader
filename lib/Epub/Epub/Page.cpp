#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
NativePageElements::~NativePageElements() {
  for (size_t i = 0; i < size_; ++i) std::destroy_at(data_ + i);
  native_text_free(data_);
}

bool NativePageElements::reserve(size_t count) {
  if (count <= capacity_) return true;
  if (count > SIZE_MAX / sizeof(Element)) return false;
  auto* next = static_cast<Element*>(native_text_malloc(count * sizeof(Element)));
  if (!next) return false;
  for (size_t i = 0; i < size_; ++i) {
    std::construct_at(next + i, std::move(data_[i]));
    std::destroy_at(data_ + i);
  }
  native_text_free(data_);
  data_ = next;
  capacity_ = count;
  return true;
}

bool NativePageElements::append(Element element) {
  if (size_ == capacity_) return false;
  std::construct_at(data_ + size_, std::move(element));
  ++size_;
  return true;
}
#endif

bool Page::reserveElements(size_t count) {
  if (count > MAX_ELEMENTS_PER_PAGE) {
    LOG_ERR("PGE", "Page element capacity exceeded");
    return false;
  }
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  if (!elements.reserve(count)) {
    LOG_ERR("PGE", "Could not allocate page elements");
    return false;
  }
#else
  elements.reserve(count);
#endif
  return true;
}

bool Page::addElement(std::unique_ptr<PageElement> element) {
  if (!element || elements.size() >= MAX_ELEMENTS_PER_PAGE) {
    LOG_ERR("PGE", "Invalid or excessive page element");
    return false;
  }
  if (elements.size() == elements.capacity() &&
      !reserveElements(std::min<size_t>(MAX_ELEMENTS_PER_PAGE, std::max<size_t>(16, elements.capacity() * 2))))
    return false;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  return elements.append(std::move(element));
#else
  elements.push_back(std::move(element));
  return true;
#endif
}

namespace {
template <typename T>
bool readField(serialization::BoundedFileReader& file, T& value) {
  return file.read(&value, sizeof(value)) == sizeof(value);
}
template <typename T>
bool writeField(HalFile& file, const T& value) {
  return file.write(&value, sizeof(value)) == sizeof(value);
}
bool hasBytes(serialization::BoundedFileReader& file, size_t bytes) {
  const size_t position = file.position();
  const size_t size = file.size();
  return position <= size && bytes <= size - position;
}

template <typename Predicate>
void renderFilteredPageElements(const Page::ElementList& elements, GfxRenderer& renderer, const int fontId,
                                const int xOffset, const int yOffset, Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

}  // namespace

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(HalFile& file) {
  if (!block || !writeField(file, xPos) || !writeField(file, yPos)) return false;

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(serialization::BoundedFileReader& file) {
  int16_t xPos = 0, yPos = 0;
  if (!readField(file, xPos) || !readField(file, yPos)) return nullptr;

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR("PGE", "Deserialization failed: null TextBlock");
    return nullptr;
  }

  auto line = makeUniqueNoThrow<PageLine>(std::move(tb), xPos, yPos);
  if (!line) {
    file.outOfMemory();
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  return line;
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

void PageImage::renderPlaceholder(GfxRenderer& renderer, const int xOffset, const int yOffset) const {
  imageBlock->renderPlaceholder(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(HalFile& file) {
  if (!imageBlock || !writeField(file, xPos) || !writeField(file, yPos)) return false;

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(serialization::BoundedFileReader& file) {
  int16_t xPos = 0, yPos = 0;
  if (!readField(file, xPos) || !readField(file, yPos)) return nullptr;
  auto ib = ImageBlock::deserialize(file);
  if (!ib) {
    LOG_ERR("PGE", "Deserialization failed: null ImageBlock");
    return nullptr;
  }
  auto image = makeUniqueNoThrow<PageImage>(std::move(ib), xPos, yPos);
  if (!image) {
    file.outOfMemory();
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageImage");
    return nullptr;
  }
  return image;
}

void PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness, true);
}

bool PageHorizontalRule::serialize(HalFile& file) {
  return width > 0 && thickness > 0 && writeField(file, xPos) && writeField(file, yPos) && writeField(file, width) &&
         writeField(file, thickness);
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(serialization::BoundedFileReader& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  if (!readField(file, xPos) || !readField(file, yPos) || !readField(file, width) || !readField(file, thickness))
    return nullptr;

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto rule = makeUniqueNoThrow<PageHorizontalRule>(width, thickness, xPos, yPos);
  if (!rule) {
    file.outOfMemory();
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return rule;
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, [](const PageElement&) { return true; });
}

bool Page::warmNativeText(GfxRenderer& renderer, const int fontId) const {
  for (const auto& element : elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine&>(*element).getBlock();
    if (!block || !block->warmNativeText(renderer, fontId)) return false;
  }
  return true;
}

void Page::renderImages(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset,
                             [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

void Page::renderWithImagePlaceholders(GfxRenderer& renderer, const int fontId, const int xOffset,
                                       const int yOffset) const {
  for (const auto& element : elements) {
    if (element->getTag() == TAG_PageImage) {
      static_cast<const PageImage&>(*element).renderPlaceholder(renderer, xOffset, yOffset);
    } else {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

bool Page::serialize(HalFile& file) const {
  if (elements.size() > MAX_ELEMENTS_PER_PAGE || footnotes.size() > MAX_FOOTNOTES_PER_PAGE ||
      links.size() > MAX_LINKS_PER_PAGE)
    return false;
  const uint16_t count = static_cast<uint16_t>(elements.size());
  if (!writeField(file, count)) return false;

  for (const auto& el : elements) {
    if (!el || !writeField(file, static_cast<uint8_t>(el->getTag()))) return false;

    if (!el->serialize(file)) {
      return false;
    }
  }

  const uint16_t fnCount = static_cast<uint16_t>(footnotes.size());
  if (!writeField(file, fnCount)) return false;
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  const uint16_t linkCount = static_cast<uint16_t>(links.size());
  if (!writeField(file, linkCount)) return false;
  for (uint16_t i = 0; i < linkCount; i++) {
    const auto& link = links[i];
    if (file.write(link.href, sizeof(link.href)) != sizeof(link.href)) {
      LOG_ERR("PGE", "Failed to write link %u", i);
      return false;
    }
    if (!writeField(file, link.x) || !writeField(file, link.y) || !writeField(file, link.width) ||
        !writeField(file, link.height))
      return false;
  }

  return true;
}

std::unique_ptr<Page> Page::decode(serialization::BoundedFileReader& file) {
  if (!file.valid()) return nullptr;
  auto page = makeUniqueNoThrow<Page>();
  if (!page) {
    file.outOfMemory();
    LOG_ERR("PGE", "Deserialization failed: could not allocate Page");
    return nullptr;
  }

  uint16_t count = 0;
  if (!readField(file, count) || count > MAX_ELEMENTS_PER_PAGE || !hasBytes(file, size_t(count) * 8 + 4))
    return nullptr;
  if (!page->reserveElements(count)) {
    file.outOfMemory();
    return nullptr;
  }

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag = 0;
    if (!readField(file, tag)) return nullptr;

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      if (!page->addElement(std::move(pl))) {
        file.outOfMemory();
        return nullptr;
      }
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      if (!page->addElement(std::move(pi))) {
        file.outOfMemory();
        return nullptr;
      }
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      if (!page->addElement(std::move(rule))) {
        file.outOfMemory();
        return nullptr;
      }
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount = 0;
  if (!readField(file, fnCount) || fnCount > MAX_FOOTNOTES_PER_PAGE ||
      !hasBytes(file, size_t(fnCount) * (sizeof(FootnoteEntry::number) + sizeof(FootnoteEntry::href)) + 2)) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    if (!memchr(entry.number, '\0', sizeof(entry.number)) || !memchr(entry.href, '\0', sizeof(entry.href)))
      return nullptr;
  }

  uint16_t linkCount = 0;
  if (!readField(file, linkCount) || linkCount > MAX_LINKS_PER_PAGE ||
      !hasBytes(file, size_t(linkCount) * (sizeof(PageLink::href) + 8))) {
    LOG_ERR("PGE", "Invalid link count %u", linkCount);
    return nullptr;
  }
  page->links.resize(linkCount);
  for (uint16_t i = 0; i < linkCount; i++) {
    auto& link = page->links[i];
    if (file.read(link.href, sizeof(link.href)) != sizeof(link.href)) {
      LOG_ERR("PGE", "Failed to read link %u", i);
      return nullptr;
    }
    if (!memchr(link.href, '\0', sizeof(link.href)) || !readField(file, link.x) || !readField(file, link.y) ||
        !readField(file, link.width) || !readField(file, link.height))
      return nullptr;
    if (link.href[0] == '\0' || link.width <= 0 || link.height <= 0) {
      LOG_ERR("PGE", "Invalid link geometry %u", i);
      return nullptr;
    }
  }

  return page;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file) {
  serialization::BoundedFileReader input(file);
  auto page = decode(input);
  if (!input.valid()) return nullptr;
  return page;
}

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
std::unique_ptr<Page> Page::deserialize(HalFile& file, uint32_t bytes, TextStatus& status) {
  serialization::BoundedFileReader input(file, bytes);
  auto page = decode(input);
  status = TextStatus::InvalidText;
  if (input.status() == serialization::FileReadStatus::StorageError)
    status = TextStatus::StorageError;
  else if (input.status() == serialization::FileReadStatus::OutOfMemory)
    status = TextStatus::OutOfMemory;
  else if (input.valid() && page && input.position() == input.size())
    status = TextStatus::Ok;
  if (status != TextStatus::Ok) {
    LOG_ERR("PGE", "Bounded page decode failed: %u", static_cast<unsigned>(status));
    return nullptr;
  }
  return page;
}
#endif
