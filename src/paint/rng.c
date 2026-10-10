/* Seeds from bytes (rng.h); the per-dab hashes are inline in the header. */
#include "paint/rng.h"

uint64_t pc_paint_seed_from_bytes(const uint8_t *b, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x00000100000001B3ull;
    }
    return pc_paint_mix64(h);
}
