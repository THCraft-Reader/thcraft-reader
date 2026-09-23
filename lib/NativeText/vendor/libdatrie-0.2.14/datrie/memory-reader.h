/* NativeText port, 2026. SPDX-License-Identifier: LGPL-2.1-or-later
 * Bounded reader for the unchanged libdatrie big-endian wire format.
 */
#ifndef DATRIE_MEMORY_READER_H
#define DATRIE_MEMORY_READER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "typedefs.h"

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} TrieMemoryReader;

static int
trie_memory_bytes (TrieMemoryReader *r, size_t n, const uint8_t **bytes)
{
    if (n > r->size - r->pos)
        return 0;
    *bytes = r->data + r->pos;
    r->pos += n;
    return 1;
}

static int
trie_memory_u32 (TrieMemoryReader *r, uint32_t *value)
{
    const uint8_t *p;
    if (!trie_memory_bytes (r, 4, &p))
        return 0;
    *value = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
           | ((uint32_t)p[2] << 8) | p[3];
    return 1;
}

static int
trie_memory_i32 (TrieMemoryReader *r, int32 *value)
{
    uint32_t u;
    if (!trie_memory_u32 (r, &u))
        return 0;
    *value = u <= INT32_MAX ? (int32)u : -1 - (int32)(UINT32_MAX - u);
    return 1;
}

static int
trie_memory_length (TrieMemoryReader *r, size_t *length)
{
    const uint8_t *p;
    if (!trie_memory_bytes (r, 2, &p) || (p[0] & 0x80))
        return 0; /* upstream suffix offsets and lengths are signed shorts */
    *length = ((size_t)p[0] << 8) | p[1];
    return 1;
}

#endif
