#ifndef ULTRIUM_CRC32C_H
#define ULTRIUM_CRC32C_H
#include <stddef.h>
#include <stdint.h>

/* CRC-32C (Castagnoli): reflected polynomial 0x82F63B78, initial value
   0xFFFFFFFF, final XOR 0xFFFFFFFF. The algorithm is part of the UltriumFS 2
   format; do not change it without a new format version. Usage:
     c = crc32c_begin(); c = crc32c_update(c, a, n); ... value = crc32c_end(c); */
uint32_t crc32c_begin(void);
uint32_t crc32c_update(uint32_t state, const void *data, size_t length);
uint32_t crc32c_end(uint32_t state);
#endif
