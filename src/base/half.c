/* Line-by-line port of half-2.7.1 src/binary16/arch.rs: the *_fallback functions, except f64 -> f16
 * (see pc_f16_from_f64). */
#include "half.h"

#include <math.h>

#include "bytes.h"

uint16_t pc_f16_from_f32(float value)
{
    uint32_t x = pc_f32_to_bits(value);
    uint32_t sign = x & 0x80000000u;
    uint32_t exp = x & 0x7F800000u;
    uint32_t man = x & 0x007FFFFFu;
    uint32_t half_sign;
    int32_t half_exp;
    uint32_t half_man;
    uint32_t round_bit;

    if (exp == 0x7F800000u) {
        uint32_t nan_bit = man == 0 ? 0 : 0x0200u;
        return (uint16_t)((sign >> 16) | 0x7C00u | nan_bit | (man >> 13));
    }
    half_sign = sign >> 16;
    half_exp = (int32_t)(exp >> 23) - 127 + 15;
    if (half_exp >= 0x1F) {
        return (uint16_t)(half_sign | 0x7C00u);
    }
    if (half_exp <= 0) {
        if (14 - half_exp > 24) {
            return (uint16_t)half_sign;
        }
        man |= 0x00800000u;
        half_man = man >> (14 - half_exp);
        round_bit = 1u << (13 - half_exp);
        if ((man & round_bit) != 0 && (man & (3 * round_bit - 1)) != 0) {
            half_man++;
        }
        return (uint16_t)(half_sign | half_man);
    }
    half_man = man >> 13;
    round_bit = 0x00001000u;
    if ((man & round_bit) != 0 && (man & (3 * round_bit - 1)) != 0) {
        return (uint16_t)((half_sign | ((uint32_t)half_exp << 10) | half_man) + 1);
    }
    return (uint16_t)(half_sign | ((uint32_t)half_exp << 10) | half_man);
}

uint16_t pc_f16_from_f64(double value)
{
    /* The crate's x86 F16C path (what the reference runs on): `value as f32`, then f32 -> f16.
     * Anything at or beyond 65520 is infinity in f16 anyway; handling it here also keeps the
     * double -> float conversion in range (out-of-range conversion is undefined in C). */
    if (fabs(value) >= 65520.0) {
        return (uint16_t)((pc_f64_to_bits(value) >> 48 & 0x8000u) | 0x7C00u);
    }
    return pc_f16_from_f32((float)value);
}

/* For a subnormal half mantissa m (1..0x3FF): the crate's `leading_zeros_u16(m) - 6`, i.e. one
 * less than the shift that brings m's top bit to bit 10. A bounded loop keeps it visibly in 0..9. */
static uint32_t subnormal_shift(uint32_t m)
{
    uint32_t s = 1;
    while (s < 10 && ((m << s) & 0x400u) == 0) {
        s++;
    }
    return s - 1;
}

float pc_f16_to_f32(uint16_t i)
{
    uint32_t half_sign = (uint32_t)(i & 0x8000u);
    uint32_t half_exp = (uint32_t)(i & 0x7C00u);
    uint32_t half_man = (uint32_t)(i & 0x03FFu);
    uint32_t sign;

    if ((i & 0x7FFFu) == 0) {
        return pc_f32_from_bits((uint32_t)i << 16);
    }
    if (half_exp == 0x7C00u) {
        if (half_man == 0) {
            return pc_f32_from_bits((half_sign << 16) | 0x7F800000u);
        }
        return pc_f32_from_bits((half_sign << 16) | 0x7FC00000u | (half_man << 13));
    }
    sign = half_sign << 16;
    if (half_exp == 0) {
        uint32_t e = subnormal_shift(half_man);
        uint32_t exp = (127 - 15 - e) << 23;
        uint32_t man = (half_man << (14 + e)) & 0x7FFFFFu;
        return pc_f32_from_bits(sign | exp | man);
    }
    return pc_f32_from_bits(sign | (((half_exp >> 10) - 15 + 127) << 23) | (half_man << 13));
}

double pc_f16_to_f64(uint16_t i)
{
    uint64_t half_sign = (uint64_t)(i & 0x8000u);
    uint64_t half_exp = (uint64_t)(i & 0x7C00u);
    uint64_t half_man = (uint64_t)(i & 0x03FFu);
    uint64_t sign;

    if ((i & 0x7FFFu) == 0) {
        return pc_f64_from_bits((uint64_t)i << 48);
    }
    if (half_exp == 0x7C00u) {
        if (half_man == 0) {
            return pc_f64_from_bits((half_sign << 48) | 0x7FF0000000000000u);
        }
        return pc_f64_from_bits((half_sign << 48) | 0x7FF8000000000000u | (half_man << 42));
    }
    sign = half_sign << 48;
    if (half_exp == 0) {
        uint64_t e = subnormal_shift((uint32_t)half_man);
        uint64_t exp = (1023 - 15 - e) << 52;
        uint64_t man = (half_man << (43 + e)) & 0xFFFFFFFFFFFFFu;
        return pc_f64_from_bits(sign | exp | man);
    }
    return pc_f64_from_bits(sign | (((half_exp >> 10) - 15 + 1023) << 52) | (half_man << 42));
}
