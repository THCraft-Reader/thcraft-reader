#pragma once

#include "NativeTextTypes.h"

void nativeTextLock();
void nativeTextUnlock();
void nativeTextLogError(const char* message);
void nativeTextYield();

// Registry-private stream bridge: the implementation owns one persistent
// HalFile (FILE on hosts) and a native-budgeted 4 KiB read cache.
namespace native_text::detail {
struct FontStream;
TextStatus openFontStream(const char* path, FontStream*& stream);
void closeFontStream(FontStream* stream);
uint32_t fontStreamSize(const FontStream* stream);
size_t readFontStream(FontStream* stream, uint32_t offset, void* data, size_t bytes);
bool fontStreamFailed(const FontStream* stream);
TextStatus fontStreamFingerprint(FontStream* stream, uint64_t& fingerprint);
}  // namespace native_text::detail
