#include "crc32c.h"

static uint32_t table[256];
static int ready;
static void build(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0x82f63b78U & (uint32_t)-(int32_t)(c & 1));
        table[i] = c;
    }
    ready = 1;
}
uint32_t crc32c_begin(void) { return 0xffffffffU; }
uint32_t crc32c_update(uint32_t state, const void *data, size_t length) {
    if (!ready) build();
    const uint8_t *p = data;
    for (size_t i = 0; i < length; ++i) state = table[(state ^ p[i]) & 0xff] ^ (state >> 8);
    return state;
}
uint32_t crc32c_end(uint32_t state) { return ~state; }
