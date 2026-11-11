#pragma once

#include <string>

class SdCardFontRegistry;

// Per-book reader font (family and size) in "font.json" inside the book's cache
// directory. A book with its own font makes it the live font in SETTINGS and
// holds the default aside; other books use the default font directly.
namespace BookFont {

// Starts the book's scope and makes its saved font live, snapped to a size the
// family ships. A saved family that is no longer installed is ignored for this session.
void open(const std::string& bookPath, const SdCardFontRegistry& registry);
// Ends the scope. Returns true when the live font changed back to the default,
// so the caller must reload SD fonts.
bool close();
bool isOpen();
// True while the open book uses its own font instead of the default.
bool isCustom();
// Call before changing the live font inside a book, so the change applies to
// this book only. No-op when no book is open.
void makeCustom();
// Makes the open book follow the default font again.
void useDefault();
// Persists the open book's font.json: writes it while the book uses its own
// font and removes it after useDefault(), or after an in-book edit picked the
// default font. Does not write settings.json.
bool save();

}  // namespace BookFont
