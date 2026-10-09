#include "hash.h"

#include <blake3.h>

void pc_blake3(const void *data, size_t len, uint8_t out[PC_BLAKE3_LEN])
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data, len);
    blake3_hasher_finalize(&h, out, PC_BLAKE3_LEN);
}

void pc_blake3_hex(const void *data, size_t len, char out[PC_BLAKE3_HEX_LEN + 1])
{
    uint8_t h[PC_BLAKE3_LEN];
    pc_blake3(data, len, h);
    pc_hex_encode(h, sizeof h, out);
}

void pc_hex_encode(const uint8_t *bytes, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * len] = '\0';
}
