/* Counter-based deterministic randomness for the brush engine (port of crates/paint/src/rng.rs).
 *
 * Every random value is a pure hash of (seed, index, stream), so a dab's jitter depends only on
 * the stroke seed and the dab's position in the stroke, never on how the points were chunked,
 * on thread scheduling or on earlier draws. That is what makes journaled strokes replay exactly.
 * Pure functions; thread-safe; cannot fail. */
#ifndef PC_PAINT_RNG_H
#define PC_PAINT_RNG_H

#include <stddef.h>
#include <stdint.h>

/* One stream per random quantity, so they are independent. */
enum {
    PC_PAINT_STREAM_SIZE = 1,
    PC_PAINT_STREAM_ANGLE = 2,
    PC_PAINT_STREAM_ROUNDNESS = 3,
    PC_PAINT_STREAM_FLIP_X = 4,
    PC_PAINT_STREAM_FLIP_Y = 5,
    PC_PAINT_STREAM_SCATTER_X = 6,
    PC_PAINT_STREAM_SCATTER_Y = 7,
    PC_PAINT_STREAM_COUNT = 8,
    PC_PAINT_STREAM_OPACITY = 9,
    PC_PAINT_STREAM_FLOW = 10,
    PC_PAINT_STREAM_FG_BG = 11,
    PC_PAINT_STREAM_HUE = 12,
    PC_PAINT_STREAM_SAT = 13,
    PC_PAINT_STREAM_BRIGHT = 14,
    PC_PAINT_STREAM_DEPTH = 15,
    PC_PAINT_STREAM_DUAL_X = 16,
    PC_PAINT_STREAM_DUAL_Y = 17,
    PC_PAINT_STREAM_DUAL_FLIP = 18,
    PC_PAINT_STREAM_WET = 19,
    PC_PAINT_STREAM_MIX = 20
};

/* SplitMix64 finaliser. */
static inline uint64_t pc_paint_mix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Uniform in [0, 1). The top 24 bits are exact in a float, so the division is exact too. */
static inline float pc_paint_rand01(uint64_t seed, uint64_t index, uint64_t stream)
{
    uint64_t h =
        pc_paint_mix64(seed ^ pc_paint_mix64(index * 0xD1B54A32D192ED03ull ^ stream * 0x8CB92BA72F3D8DD7ull));
    return (float)(uint32_t)(h >> 40) / 16777216.0f;
}

/* Uniform in [-1, 1). */
static inline float pc_paint_rand_signed(uint64_t seed, uint64_t index, uint64_t stream)
{
    return pc_paint_rand01(seed, index, stream) * 2.0f - 1.0f;
}

/* Hash of integer document coordinates to [0, 1) (noise, dissolve). */
static inline float pc_paint_hash2(int32_t x, int32_t y, uint64_t salt)
{
    uint64_t k = ((uint64_t)(uint32_t)x << 32) | (uint64_t)(uint32_t)y;
    return (float)(uint32_t)(pc_paint_mix64(k ^ salt * 0xA24BAED4963EE407ull) >> 40) / 16777216.0f;
}

/* Seed derived from arbitrary bytes (FNV-1a then mixed), for commands without an explicit seed.
 * `b` may be NULL when n is 0. */
uint64_t pc_paint_seed_from_bytes(const uint8_t *b, size_t n);

#endif
