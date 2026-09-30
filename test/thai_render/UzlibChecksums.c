#include "../../lib/miniz/src/MinizConfig.h"
#include "uzlib.h"

uint32_t TINFCC uzlib_adler32(const void* data, unsigned int length, uint32_t previous) {
  return (uint32_t)mz_adler32(previous, (const unsigned char*)data, length);
}

uint32_t TINFCC uzlib_crc32(const void* data, unsigned int length, uint32_t previous) {
  return ~(uint32_t)mz_crc32(~previous, (const unsigned char*)data, length);
}
