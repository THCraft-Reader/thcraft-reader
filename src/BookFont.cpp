#include "BookFont.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <SdCardFontRegistry.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"

namespace {

using ReaderFont = CrossPointSettings::ReaderFont;

// "/.crosspoint/epub_<hash>/font.json"; empty while no book is open.
char fontPath[64] = "";
bool fileExists = false;
ReaderFont fileFont;  // what font.json holds when fileExists

// What the next save() must do beyond writing a changed font.
enum class PendingSave : uint8_t {
  None,
  FontEdited,  // the font was changed inside the book; picking the default follows it
  UseDefault,  // the user chose "Use Default Font"; remove font.json
};
PendingSave pending = PendingSave::None;

bool removeFile() {
  if (Storage.exists(fontPath) && !Storage.remove(fontPath)) {
    LOG_ERR("BFONT", "Failed to remove %s", fontPath);
    return false;
  }
  fileExists = false;
  return true;
}

}  // namespace

void BookFont::open(const std::string& bookPath, const SdCardFontRegistry& registry) {
  if (isOpen()) close();
  const std::string cachePath = Epub(bookPath, "/.crosspoint").getCachePath();
  if (snprintf(fontPath, sizeof(fontPath), "%s/font.json", cachePath.c_str()) >= static_cast<int>(sizeof(fontPath))) {
    LOG_ERR("BFONT", "Book font path too long: %s", cachePath.c_str());
    fontPath[0] = '\0';
    return;
  }

  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(fontPath, doc)) return;  // the book uses the default
  fileExists = true;
  ReaderFont font;
  strncpy(font.sdFamilyName, doc["family"] | "", sizeof(font.sdFamilyName) - 1);
  font.pointSize = doc["size"] | static_cast<uint8_t>(0);
  fileFont = font;
  if (font.pointSize == 0) {
    LOG_ERR("BFONT", "Ignoring %s: no font size", fontPath);
    return;
  }
  if (font.sdFamilyName[0] != '\0' && !registry.findFamily(font.sdFamilyName)) {
    LOG_DBG("BFONT", "Book font %s is not installed, using the default", font.sdFamilyName);
    return;
  }
  font.pointSize = snapToNearestPointSize(readerFontPointSizes(&registry, font.sdFamilyName), font.pointSize);
  SETTINGS.holdDefaultReaderFont();
  SETTINGS.setLiveReaderFont(font);
}

bool BookFont::close() {
  if (!isOpen()) return false;
  const bool changed = isCustom() && SETTINGS.liveReaderFont() != SETTINGS.defaultReaderFont();
  SETTINGS.releaseDefaultReaderFont();
  fontPath[0] = '\0';
  fileExists = false;
  pending = PendingSave::None;
  return changed;
}

bool BookFont::isOpen() { return fontPath[0] != '\0'; }

bool BookFont::isCustom() { return isOpen() && SETTINGS.isDefaultReaderFontHeld(); }

void BookFont::makeCustom() {
  if (!isOpen()) return;
  SETTINGS.holdDefaultReaderFont();
  pending = PendingSave::FontEdited;
}

void BookFont::useDefault() {
  if (!isOpen()) return;
  SETTINGS.releaseDefaultReaderFont();
  pending = PendingSave::UseDefault;
}

bool BookFont::save() {
  if (!isOpen()) return true;
  if (!isCustom()) {
    // A font.json whose family is missing stays until the user picks a font.
    if (pending != PendingSave::UseDefault) return true;
    if (!removeFile()) return false;  // retried on the next save
    pending = PendingSave::None;
    return true;
  }

  const bool edited = pending == PendingSave::FontEdited;
  pending = PendingSave::None;
  const ReaderFont live = SETTINGS.liveReaderFont();
  if (edited && live == SETTINGS.defaultReaderFont()) {
    // The default was picked inside the book: follow it from now on.
    SETTINGS.releaseDefaultReaderFont();
    return removeFile();
  }
  if (fileExists && live == fileFont) return true;

  JsonDocument doc;
  doc["family"] = static_cast<const char*>(live.sdFamilyName);  // copied, not kept by pointer
  doc["size"] = live.pointSize;
  if (!PersistableStoreBase::writeDocToFile(fontPath, doc)) return false;
  fileExists = true;
  fileFont = live;
  return true;
}
