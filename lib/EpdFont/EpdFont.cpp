#include "EpdFont.h"

#include <Utf8.h>
#include <ThaiShape.h>
#include <ThaiCluster.h>

#include <algorithm>
#include <cstring>

void EpdFont::getTextBounds(const char* string, const int startX, const int startY, int* minX, int* minY, int* maxX,
                            int* maxY) const {
  *minX = startX;
  *minY = startY;
  *maxX = startX;
  *maxY = startY;

  if (*string == '\0') {
    return;
  }

  int lastBaseX = startX;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  uint32_t cp;
  uint32_t prevCp = 0;
#if THAI_SHAPING
  const ThaiShapeView* shape = getThaiShape();
  const bool shapedText = shape && thai::containsThai(string);
  const char* end = shapedText ? string + strlen(string) : string;
  const char* nativeUntil = string;
#else
  constexpr bool shapedText = false;
#endif
  while (*string) {
#if THAI_SHAPING
    if (shapedText && string >= nativeUntil) {
      ThaiGlyphCursor cursor;
      if (cursor.begin(std::string_view(string, end - string), *shape)) {
        ThaiGlyphPlacement placement;
        int32_t pen = 0;
        uint8_t previousFlags = 0;
        bool first = true;
        while (cursor.next(placement)) {
          if (first) {
            if (prevCp) lastBaseX += fp4::toPixel(prevAdvanceFP + getKerning(prevCp, placement.codepoint));
          } else if ((previousFlags & ThaiGlyphPlacement::Native) ||
                     (placement.flags & (ThaiGlyphPlacement::Native | ThaiGlyphPlacement::RecipeStart))) {
            pen += getKerning(prevCp, placement.codepoint);
          }
          const EpdGlyph* glyph = getGlyph(placement.codepoint);
          if (glyph) {
            const int gx = lastBaseX + fp4::toPixel(pen + placement.xOffsetFP) + glyph->left;
            const int gy = startY - fp4::toPixel(placement.yOffsetFP) - glyph->top;
            *minX = std::min(*minX, gx);
            *maxX = std::max(*maxX, gx + glyph->width);
            *minY = std::min(*minY, gy);
            *maxY = std::max(*maxY, gy + glyph->height);
          }
          pen += (placement.flags & ThaiGlyphPlacement::Native) ? (glyph ? glyph->advanceX : 0)
                                                               : placement.advanceFP;
          prevCp = placement.codepoint;
          previousFlags = placement.flags;
          first = false;
        }
        prevAdvanceFP = fp4::toPixel(pen) * 16;
        string += cursor.consumedBytes();
        continue;
      }
      nativeUntil = string + cursor.consumedBytes();
    }
#endif
    cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string));
    if (!cp) break;
    const bool isCombining = utf8IsCombiningMark(cp);

    if (!isCombining) {
      cp = applyLigatures(cp, string);
    }

    const EpdGlyph* glyph = getGlyph(cp);
    if (!glyph) {
      // Keep cursor movement stable when a base glyph is missing, but don't attach subsequent
      // combining marks to stale base metrics.
      if (!isCombining) {
        lastBaseX += fp4::toPixel(prevAdvanceFP);  // flush pending advance before resetting
        prevCp = 0;
        prevAdvanceFP = 0;
        lastBaseLeft = 0;
        lastBaseWidth = 0;
        lastBaseTop = 0;
      }
      continue;
    }

    const combiningMark::Anchor anchor = combiningMark::anchorFor(cp);
    const int raiseBy = isCombining ? combiningMark::raiseAboveBase(anchor, glyph->top, glyph->height, lastBaseTop) : 0;

    if (!isCombining && prevCp != 0) {
      const auto kernFP = getKerning(prevCp, cp);  // 4.4 fixed-point kern
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
    }

    const int glyphBaseX = isCombining ? combiningMark::anchorOver(anchor, lastBaseX, lastBaseLeft, lastBaseWidth,
                                                                   glyph->left, glyph->width)
                                       : lastBaseX;
    const int glyphBaseY = startY - raiseBy;

    *minX = std::min(*minX, glyphBaseX + glyph->left);
    *maxX = std::max(*maxX, glyphBaseX + glyph->left + glyph->width);
    const int glyphTop = shapedText ? glyphBaseY - glyph->top : glyphBaseY + glyph->top - glyph->height;
    *minY = std::min(*minY, glyphTop);
    *maxY = std::max(*maxY, glyphTop + glyph->height);

    if (!isCombining) {
      lastBaseLeft = glyph->left;
      lastBaseWidth = glyph->width;
      lastBaseTop = glyph->top;
      prevAdvanceFP = glyph->advanceX;  // 12.4 fixed-point
      prevCp = cp;
    }
  }
}

void EpdFont::getTextDimensions(const char* string, int* w, int* h) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;

  getTextBounds(string, 0, 0, &minX, &minY, &maxX, &maxY);

  *w = maxX - minX;
  *h = maxY - minY;
}

// Split form: the search touches only the codepoint array. See EpdFontData::kernLeftCodepoints.
static uint8_t lookupKernClassSplit(const uint16_t* codepoints, const uint8_t* classIds, const uint16_t count,
                                    const uint32_t cp) {
  if (!codepoints || count == 0 || cp > 0xFFFF) {
    return 0;
  }
  const auto target = static_cast<uint16_t>(cp);
  const uint16_t* end = codepoints + count;
  const auto it = std::lower_bound(codepoints, end, target);
  return (it != end && *it == target) ? classIds[it - codepoints] : 0;
}

static uint8_t lookupKernClass(const EpdKernClassEntry* entries, const uint16_t count, const uint32_t cp) {
  if (!entries || count == 0 || cp > 0xFFFF) {
    return 0;
  }

  const auto target = static_cast<uint16_t>(cp);
  const auto* end = entries + count;

  // lower_bound: exact-key lookup. Finds the first entry with codepoint >= target,
  // then the equality check confirms an exact match exists.
  const auto it = std::lower_bound(
      entries, end, target, [](const EpdKernClassEntry& entry, uint16_t value) { return entry.codepoint < value; });

  if (it != end && it->codepoint == target) {
    return it->classId;
  }

  return 0;
}

int8_t EpdFont::getKerning(const uint32_t leftCp, const uint32_t rightCp) const {
  if (utf8IsCjkBreakable(leftCp) || utf8IsCjkBreakable(rightCp)) {
    return 0;
  }
  if (data->kernHandler) {
    return data->kernHandler(data->glyphMissCtx, leftCp, rightCp);
  }
  if (!data->kernMatrix && !data->kernRowOffsets) {
    return 0;
  }
  if (!data->kernLeftClasses && !data->kernLeftCodepoints) {
    return 0;
  }
  // Built-in fonts carry the split arrays, SD-card fonts the packed ones; never both.
  const bool split = data->kernLeftCodepoints != nullptr;
  const uint8_t lc =
      split ? lookupKernClassSplit(data->kernLeftCodepoints, data->kernLeftClassIds, data->kernLeftEntryCount, leftCp)
            : lookupKernClass(data->kernLeftClasses, data->kernLeftEntryCount, leftCp);
  if (lc == 0) return 0;
  const uint8_t rc = split ? lookupKernClassSplit(data->kernRightCodepoints, data->kernRightClassIds,
                                                  data->kernRightEntryCount, rightCp)
                           : lookupKernClass(data->kernRightClasses, data->kernRightEntryCount, rightCp);
  if (rc == 0) return 0;

  // Sparse (built-in fonts): scan the row. Linear rather than binary — rows hold ~15 entries on
  // average, short enough that the scan measured faster (worst-case mix +11% over dense against
  // +19% for std::lower_bound), and it should widen on hardware that reads these arrays through
  // a flash cache, since the scan walks forwards through a cache line.
  if (data->kernRowOffsets) {
    const uint16_t begin = data->kernRowOffsets[lc - 1];
    const uint16_t end = data->kernRowOffsets[lc];
    const auto target = static_cast<uint8_t>(rc - 1);
    const uint8_t* cols = data->kernSparseCols;
    for (uint16_t i = begin; i < end; i++) {
      if (cols[i] == target) return data->kernSparseValues[i];
      if (cols[i] > target) break;  // sorted ascending, so past the target means absent
    }
    return 0;
  }

  // Dense (SD-card fonts, mapped straight out of the .cpfont).
  return data->kernMatrix[(lc - 1) * data->kernRightClassCount + (rc - 1)];
}

// Arabic contextual joining (including Lam-Alef) is resolved earlier by
// do_shape() in MiniBidi, which emits presentation forms in visual order.
// Font GSUB ligatures must not run a second pass over that output: a shaped
// Alef+Lam ("…ال…") is FEDF+FE8E, which the font's Lam-Alef pairs would
// wrongly re-collapse into FEFB/FEFC — transposing the letters (e.g. کسالت →
// کسلات). Latin ligatures (ff/fi/fl) key off ASCII and are unaffected.
static inline bool isArabicPresentationForm(const uint32_t cp) {
  return (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF);
}

uint32_t EpdFont::getLigature(const uint32_t leftCp, const uint32_t rightCp) const {
  const auto* pairs = data->ligaturePairs;
  const auto count = data->ligaturePairCount;
  if (!pairs || count == 0 || leftCp > 0xFFFF || rightCp > 0xFFFF) {
    return 0;
  }
  if (isArabicPresentationForm(leftCp) || isArabicPresentationForm(rightCp)) {
    return 0;
  }

  const uint32_t key = (leftCp << 16) | rightCp;
  const auto* end = pairs + count;

  // lower_bound: exact-key lookup. Finds the first entry with pair >= key,
  // then the equality check confirms an exact match exists.
  const auto it =
      std::lower_bound(pairs, end, key, [](const EpdLigaturePair& pair, uint32_t value) { return pair.pair < value; });

  if (it != end && it->pair == key) {
    return it->ligatureCp;
  }

  return 0;
}

uint32_t EpdFont::applyLigatures(uint32_t cp, const char*& text) const {
  if (!data->ligaturePairs || data->ligaturePairCount == 0) {
    return cp;
  }
  while (true) {
    const auto saved = reinterpret_cast<const uint8_t*>(text);
    const uint32_t nextCp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text));
    if (nextCp == 0) break;
    const uint32_t lig = getLigature(cp, nextCp);
    if (lig == 0) {
      text = reinterpret_cast<const char*>(saved);
      break;
    }
    cp = lig;
  }
  return cp;
}

const EpdGlyph* EpdFont::getGlyph(const uint32_t cp) const {
  const int count = data->intervalCount;
  if (count == 0 && !data->glyphMissHandler) return nullptr;

  if (count > 0) {
    const EpdUnicodeInterval* intervals = data->intervals;
    const auto* end = intervals + count;

    // upper_bound: range lookup. Finds the first interval with first > cp, so the
    // interval just before it is the last one with first <= cp. That's the only
    // candidate that could contain cp. Then we verify cp <= candidate.last.
    const auto it = std::upper_bound(
        intervals, end, cp, [](uint32_t value, const EpdUnicodeInterval& interval) { return value < interval.first; });

    if (it != intervals) {
      const auto& interval = *(it - 1);
      if (cp <= interval.last) {
        return &data->glyph[interval.offset + (cp - interval.first)];
      }
    }
  }

  // Codepoint not in interval table — try on-demand loading (SD card fonts).
  if (data->glyphMissHandler) {
    const EpdGlyph* loaded = data->glyphMissHandler(data->glyphMissCtx, cp);
    if (loaded) return loaded;
  }

  if (cp != REPLACEMENT_GLYPH) {
    return getGlyph(REPLACEMENT_GLYPH);
  }
  return nullptr;
}

bool EpdFont::hasCodepoint(const uint32_t cp) const {
  const int count = data->intervalCount;
  if (count > 0) {
    const EpdUnicodeInterval* intervals = data->intervals;
    const auto* end = intervals + count;
    const auto it = std::upper_bound(
        intervals, end, cp, [](uint32_t value, const EpdUnicodeInterval& interval) { return value < interval.first; });
    if (it != intervals && cp <= (it - 1)->last) return true;
  }

  // Interval table miss. SD card fonts only keep the current page's glyphs in
  // their interval table — ask their full RAM-resident coverage index instead.
  if (data->coverageHandler) {
    return data->coverageHandler(data->glyphMissCtx, cp);
  }
  return false;
}
