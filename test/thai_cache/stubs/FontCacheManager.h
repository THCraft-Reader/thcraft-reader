#pragma once

// This fixture has no SD fonts; GfxRenderer returns no font-cache manager.
class FontCacheManager {
 public:
  void releaseSdFontCaches() {}
};
