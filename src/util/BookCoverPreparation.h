#pragma once

#include <string>

class GfxRenderer;

namespace bookcovers {
// Caller holds RenderLock. Returns true if preparation replaced the framebuffer.
bool prepareForReader(GfxRenderer& renderer, const std::string& bookPath);
// Caller holds RenderLock and has displayed the sleep popup. Restores fonts before returning.
bool prepareForSleep(GfxRenderer& renderer, const std::string& bookPath, std::string& coverPath);
}  // namespace bookcovers
