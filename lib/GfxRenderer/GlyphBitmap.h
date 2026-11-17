#pragma once

#include <algorithm>
#include <cstdint>

#ifdef _MSC_VER
#define GLYPH_ALWAYS_INLINE __forceinline
#else
#define GLYPH_ALWAYS_INLINE __attribute__((always_inline)) inline
#endif

// Glyph rasterizer that resolves orientation, clipping and framebuffer
// addressing once per glyph instead of once per pixel. It has no dependency
// on GfxRenderer so it can be unit-tested on the host.
namespace glyphBitmap {

// Framebuffer plane a glyph is painted into.
enum class Plane : uint8_t { BW, GrayLSB, GrayMSB };

// Position of glyph pixel (0, 0) plus the step for one move along the glyph's
// x and y axes, all in one coordinate space. The axes must describe an
// orthogonal unit rotation: each is +/-1 on exactly one component.
struct Frame {
  int x;
  int y;
  int dxX;
  int dxY;
  int dyX;
  int dyY;
};

// Where a glyph lands in physical framebuffer space.
struct Target {
  uint8_t* buffer;  // First byte of row originY (framebuffer or strip scratch)
  int width;        // Physical panel width in pixels
  int stride;       // Bytes per physical row
  int originY;      // First physical row held by buffer
  int rows;         // Number of physical rows held by buffer
  Frame frame;      // Physical placement of the glyph
};

// Half-open rectangle in glyph-local pixel coordinates.
struct Clip {
  int left;
  int top;
  int right;
  int bottom;
};

// Narrow [start, end) on one glyph axis so that base + i * step stays within
// [lower, upper). step is +1 or -1 because the transform is orthogonal.
inline void clipAxis(int base, int step, int lower, int upper, int& start, int& end) {
  if (step > 0) {
    start = std::max(start, lower - base);
    end = std::min(end, upper - base);
  } else {
    start = std::max(start, base - upper + 1);
    end = std::min(end, base - lower + 1);
  }
}

// Narrow a glyph-local clip so every kept pixel maps inside the half-open
// rectangle [left, right) x [top, bottom) of the space frame is expressed in.
// Which glyph axis runs along that space's x depends on the rotation:
// dxX != 0 means glyph x does. Runs twice per glyph, so it stays inline
// under -Os.
GLYPH_ALWAYS_INLINE void clipToRect(const Frame& frame, int left, int top, int right, int bottom, Clip& clip) {
  if (frame.dxX != 0) {
    clipAxis(frame.x, frame.dxX, left, right, clip.left, clip.right);
    clipAxis(frame.y, frame.dyY, top, bottom, clip.top, clip.bottom);
  } else {
    clipAxis(frame.x, frame.dyX, left, right, clip.top, clip.bottom);
    clipAxis(frame.y, frame.dxY, top, bottom, clip.left, clip.right);
  }
}

// Keep pixel writes inline when decoding a group of four pixels.
GLYPH_ALWAYS_INLINE void paint(uint8_t* buffer, int destination, uint8_t ink, uint8_t levels, bool clearBits) {
  if ((levels & (1u << ink)) == 0) return;
  const uint8_t mask = 0x80u >> (destination & 7);
  if (clearBits)
    buffer[destination >> 3] &= static_cast<uint8_t>(~mask);
  else
    buffer[destination >> 3] |= mask;
}

// Paint a packed glyph into target.
//
// bitmap: rows are contiguous, MSB first, 1 or 2 bits per pixel; widths need
//   not be byte-aligned. 2bpp values are 0=white, 1=light gray, 2=dark gray,
//   3=black. 1bpp value 1 is ink.
// plane: which 2bpp values are painted. BW paints every non-white value and
//   honours state (true clears the bit, i.e. black). The gray planes only ever
//   set bits, because there 0 means leave alone and 1 means update: MSB paints
//   both grays, LSB paints dark gray only. 1bpp glyphs paint ink with state on
//   every plane.
// Clipping happens before pixel decoding, so fully hidden glyphs cost nothing.
// Bit n of levels set means source value n is painted.
inline void planeLevels(bool twoBit, Plane plane, bool state, uint8_t& levels, bool& clearBits) {
  levels = 0x02;
  clearBits = state;
  if (twoBit) {
    levels = plane == Plane::BW ? 0x0e : plane == Plane::GrayMSB ? 0x06 : 0x04;
    if (plane != Plane::BW) clearBits = false;
  }
}

inline void draw(const uint8_t* bitmap, int width, int height, bool twoBit, Plane plane, bool state,
                 const Target& target, Clip clip) {
  uint8_t levels;
  bool clearBits;
  planeLevels(twoBit, plane, state, levels, clearBits);

  // Intersect with the glyph bounds, then with the panel width and the
  // buffered row band.
  clip.left = std::max(clip.left, 0);
  clip.top = std::max(clip.top, 0);
  clip.right = std::min(clip.right, width);
  clip.bottom = std::min(clip.bottom, height);
  clipToRect(target.frame, 0, target.originY, target.width, target.originY + target.rows, clip);
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;

  // Walk the framebuffer as a flat bit index. One glyph column advances
  // stepX bits and one glyph row advances stepY bits; each is +/-1 for the
  // axis that maps to physical x, or +/-strideBits for physical y.
  const Frame& frame = target.frame;
  const int strideBits = target.stride * 8;
  const int stepX = frame.dxY * strideBits + frame.dxX;
  const int stepY = frame.dyY * strideBits + frame.dyX;
  int rowBit = (frame.y - target.originY) * strideBits + frame.x + clip.left * stepX + clip.top * stepY;
  for (int y = clip.top; y < clip.bottom; ++y, rowBit += stepY) {
    int source = y * width + clip.left;
    int destination = rowBit;
    int remaining = clip.right - clip.left;
    if (twoBit) {
      // Rows are bit-contiguous, not byte-padded. Align after any clipped prefix.
      while (remaining && (source & 3)) {
        paint(target.buffer, destination, (bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3, levels, clearBits);
        ++source;
        destination += stepX;
        --remaining;
      }
      while (remaining >= 4) {
        const uint8_t packed = bitmap[source >> 2];
        paint(target.buffer, destination, packed >> 6, levels, clearBits);
        paint(target.buffer, destination + stepX, (packed >> 4) & 3, levels, clearBits);
        paint(target.buffer, destination + 2 * stepX, (packed >> 2) & 3, levels, clearBits);
        paint(target.buffer, destination + 3 * stepX, packed & 3, levels, clearBits);
        source += 4;
        destination += 4 * stepX;
        remaining -= 4;
      }
    }
    // One-bit glyphs and the clipped tail use scalar decoding.
    while (remaining--) {
      const uint8_t ink = twoBit ? ((bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3)
                                 : ((bitmap[source >> 3] >> (7 - (source & 7))) & 1);
      paint(target.buffer, destination, ink, levels, clearBits);
      ++source;
      destination += stepX;
    }
  }
}

// Width a glyph paints at the given weight (see drawWeighted()).
inline int weightedWidth(int width, int weight) { return width + (weight > 0 ? weight : 0); }

// One output pixel of drawWeighted(), from the source values around it.
// weight 1 or 2: the darkest of the pixel and the weight pixels to its left,
//   so strokes grow right. A pixel with a darker right neighbour is kept, so
//   the gap before the next stroke stays open (such as a Thai head loop),
//   including a gray anti-aliased gap.
// weight -1: the lighter of the pixel and its left neighbour, which trims one
//   pixel from the left of each stroke. The darkest pixel at the right of a
//   stroke is kept, so 1px strokes do not disappear. A left-edge pixel is also
//   kept when diagonalLeft (the darker of the pixels above-left and
//   below-left) is darker than left: the stroke continues diagonally there,
//   and trimming would cut the join, for example in a small loop.
inline uint8_t weightedInk(uint8_t left2, uint8_t left, uint8_t current, uint8_t right, uint8_t diagonalLeft,
                           int weight) {
  if (weight > 0) {
    if (right > current) return current;
    const uint8_t ink = std::max(current, left);
    return weight > 1 ? std::max(ink, left2) : ink;
  }
  if ((current >= left && current > right) || diagonalLeft > left) return current;
  return std::min(current, left);
}

// Like draw(), but changes stroke weight along glyph x with weightedInk().
// Every pass gets the same values, so the BW and gray planes stay consistent.
// The glyph paints weightedWidth() pixels wide; clip is in those pixels. Only
// black extends past the bitmap: gray there could mark the next glyph's black
// stroke gray in the gray planes.
inline void drawWeighted(const uint8_t* bitmap, int width, int height, bool twoBit, Plane plane, bool state,
                         const Target& target, Clip clip, int weight) {
  uint8_t levels;
  bool clearBits;
  planeLevels(twoBit, plane, state, levels, clearBits);

  clip.left = std::max(clip.left, 0);
  clip.top = std::max(clip.top, 0);
  clip.right = std::min(clip.right, weightedWidth(width, weight));
  clip.bottom = std::min(clip.bottom, height);
  clipToRect(target.frame, 0, target.originY, target.width, target.originY + target.rows, clip);
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;

  const Frame& frame = target.frame;
  const int strideBits = target.stride * 8;
  const int stepX = frame.dxY * strideBits + frame.dxX;
  const int stepY = frame.dyY * strideBits + frame.dyX;
  int rowBit = (frame.y - target.originY) * strideBits + frame.x + clip.left * stepX + clip.top * stepY;
  const uint8_t black = twoBit ? 3 : 1;
  // Source value at glyph pixel (x, y); white outside the glyph.
  const auto sample = [&](int x, int y) -> uint8_t {
    if (x < 0 || x >= width || y < 0 || y >= height) return 0;
    const int source = y * width + x;
    return twoBit ? ((bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3)
                  : ((bitmap[source >> 3] >> (7 - (source & 7))) & 1);
  };
  for (int y = clip.top; y < clip.bottom; ++y, rowBit += stepY) {
    // Sliding window: left2, left, current, right.
    uint8_t left2 = sample(clip.left - 2, y);
    uint8_t left = sample(clip.left - 1, y);
    uint8_t current = sample(clip.left, y);
    int destination = rowBit;
    for (int x = clip.left; x < clip.right; ++x, destination += stepX) {
      const uint8_t right = sample(x + 1, y);
      // Only a pixel that thinning would trim needs the rows around it.
      const uint8_t diagonalLeft =
          weight < 0 && left < current ? std::max(sample(x - 1, y - 1), sample(x - 1, y + 1)) : 0;
      uint8_t ink = weightedInk(left2, left, current, right, diagonalLeft, weight);
      if (x >= width && ink != black) ink = 0;
      paint(target.buffer, destination, ink, levels, clearBits);
      left2 = left;
      left = current;
      current = right;
    }
  }
}

}  // namespace glyphBitmap

#undef GLYPH_ALWAYS_INLINE
