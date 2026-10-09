/* pc_cbrt: SPDX-License-Identifier: MIT
 * origin: core-math/src/binary64/cbrt/cbrt.c, Copyright (c) 2021-2022 Alexei Sibidanov;
 * via the Rust port in rust-lang/libm (MIT) by Trevor Gross, 2025. Round-to-nearest path only.
 *
 * pc_cbrtf: origin FreeBSD /usr/src/lib/msun/src/s_cbrtf.c (as in rust-lang/libm).
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 * Debugged and optimized by Bruce D. Evans.
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ==================================================== */
#include "rmath.h"

#include <math.h>
#include <stdint.h>

#include "bytes.h"

static unsigned clz64(uint64_t v)
{
    unsigned n = 0;
    if (v == 0) {
        return 64;
    }
    while ((v & 0x8000000000000000u) == 0) {
        v <<= 1;
        n++;
    }
    return n;
}

double pc_cbrt(double x)
{
    static const double ESCALE[3] = {1.0, 0x1.428a2f98d728bp+0, 0x1.965fea53d6e3dp+0};
    /* c0 + c1 x + c2 x^2 + c3 x^3 approximates x^(1/3) on [1, 2] (max error < 9.2e-5). */
    static const double C[4] = {0x1.1b0babccfef9cp-1, 0x1.2c9a3e94d1da5p-1, -0x1.4dc30b1a1ddbap-3,
                                0x1.7a8d3e4ec9b07p-6};
    static const double RSC[6] = {1.0, -1.0, 0.5, -0.5, 0.25, -0.25};
    const double u0 = 0x1.5555555555555p-2;
    const double u1 = 0x1.c71c71c71c71cp-3;
    const double off = 0x1p-53; /* rounding to nearest */
    const uint64_t sig_mask = 0x000fffffffffffffu;
    uint64_t hx = pc_f64_to_bits(x);
    uint64_t mant = hx & sig_mask;
    uint64_t sign = hx >> 63;
    uint32_t e = (uint32_t)(hx >> 52) & 0x7ffu;
    uint64_t cvt1;
    uint64_t cvt5;
    uint64_t isc;
    uint32_t et;
    uint32_t it;
    double zz;
    double z;
    double r;
    double rr;
    double z2;
    double c0;
    double c2;
    double y;
    double y2;
    double h;
    double y2l;
    double y3;
    double y3l;
    double dy;
    double y1;
    double ady;
    double ady0;
    double ady1;
    uint64_t cvt3;
    uint64_t m0;
    uint64_t m1;

    if (((e + 1) & 0x7ffu) < 2) {
        uint64_t ix = hx & ~((uint64_t)1 << 63);
        unsigned nz;
        /* 0, inf, nan: x + x (so a signalling NaN raises invalid). */
        if (e == 0x7ffu || ix == 0) {
            return x + x;
        }
        nz = clz64(ix) - 11; /* subnormal */
        mant <<= nz;
        mant &= sig_mask;
        e = e - (nz - 1); /* wrapping, as the reference */
    }
    e += 3072;
    cvt1 = mant | ((uint64_t)0x3ff << 52);
    cvt5 = cvt1;
    et = e / 3;
    it = e % 3;
    /* 2^(3k+it) <= x < 2^(3k+it+1), with 0 <= it <= 3 */
    cvt5 += (uint64_t)it << 52;
    cvt5 |= sign << 63;
    zz = pc_f64_from_bits(cvt5);
    /* cbrt(x) = cbrt(zz) * 2^(et-1365) where 1 <= zz < 8 */
    isc = pc_f64_to_bits(ESCALE[it]) | sign << 63;
    z = pc_f64_from_bits(cvt1);
    /* cbrt(zz) = cbrt(z) * isc, where isc encodes 1, 2^(1/3) or 2^(2/3), and 1 <= z < 2 */
    r = 1.0 / z;
    rr = r * RSC[(it << 1) | (uint32_t)sign];
    z2 = z * z;
    c0 = C[0] + z * C[1];
    c2 = C[2] + z * C[3];
    y = c0 + z2 * c2;
    y2 = y * y;
    /* y approximates z^(1/3); h measures its error */
    h = y2 * (y * r) - 1.0;
    /* a cubic variant of Newton's method on f(y) = 1 - z / y^3 */
    y -= (h * y) * (u0 - u1 * h);
    y *= pc_f64_from_bits(isc);
    /* y approximates zz^(1/3) and rr 1/zz: one more (linear) Newton-Raphson step */
    y2 = y * y;
    y2l = fma(y, y, -y2); /* y2 + y2l = y^2 exactly */
    y3 = y2 * y;
    y3l = fma(y, y2, -y3) + y * y2l; /* y3 + y3l ~ y^3 to ~106 bits */
    h = ((y3 - zz) + y3l) * rr;
    dy = h * (y * u0);
    /* zz^(1/3) ~ y - dy */
    y1 = y - dy;
    dy = (y - y1) - dy;
    /* zz^(1/3) ~ y1 + dy with |dy| < 1/2 ulp(y) */
    ady = fabs(dy);
    /* ady0 is tiny when dy is near 1/2 ulp(1), ady1 near 3/2 ulp(1): hard cases */
    ady0 = fabs(ady - off);
    ady1 = fabs(ady - (0x1p-52 + off));
    if (ady0 < 0x1p-75 || ady1 < 0x1p-75) {
        y2 = y1 * y1;
        y2l = fma(y1, y1, -y2);
        y3 = y2 * y1;
        y3l = fma(y1, y2, -y3) + y1 * y2l;
        h = ((y3 - zz) + y3l) * rr;
        dy = h * (y1 * u0);
        y = y1 - dy;
        dy = (y1 - y) - dy;
        y1 = y;
        ady = fabs(dy);
        ady0 = fabs(ady - off);
        ady1 = fabs(ady - (0x1p-52 + off));
        if (ady0 < 0x1p-98 || ady1 < 0x1p-98) {
            double azz = fabs(zz);
            if (azz == 0x1.9b78223aa307cp+1) {
                y1 = copysign(0x1.79d15d0e8d59cp+0, zz);
            }
            if (azz == 0x1.a202bfc89ddffp+2) {
                y1 = copysign(0x1.de87aa837820fp+0, zz);
            }
        }
    }
    cvt3 = pc_f64_to_bits(y1);
    cvt3 += (uint64_t)(uint32_t)(et - 342u - 1023u) << 52;
    m0 = cvt3 << 30;
    m1 = m0 >> 63;
    if ((m0 ^ m1) <= ((uint64_t)1 << 30)) {
        uint64_t cvt4 = pc_f64_to_bits(y1);
        cvt4 = (cvt4 + ((uint64_t)164 << 15)) & 0xffffffffffff0000u;
        if (fabs((pc_f64_from_bits(cvt4) - y1) - dy) < 0x1p-60 || fabs(zz) == 1.0) {
            cvt3 = (cvt3 + ((uint64_t)1 << 15)) & 0xffffffffffff0000u;
        }
    }
    return pc_f64_from_bits(cvt3);
}

float pc_cbrtf(float x)
{
    const uint32_t B1 = 709958130; /* (127 - 127.0/3 - 0.03306235651) * 2^23 */
    const uint32_t B2 = 642849266; /* (127 - 127.0/3 - 24/3 - 0.03306235651) * 2^23 */
    const float x1p24 = 0x1p24f;
    double r;
    double t;
    double xd;
    uint32_t ui = pc_f32_to_bits(x);
    uint32_t hx = ui & 0x7fffffffu;
    if (hx >= 0x7f800000u) {
        return x + x; /* NaN, inf */
    }
    /* rough cbrt to 5 bits */
    if (hx < 0x00800000u) {
        if (hx == 0) {
            return x; /* +-0 */
        }
        ui = pc_f32_to_bits(x * x1p24);
        hx = ui & 0x7fffffffu;
        hx = hx / 3 + B2;
    } else {
        hx = hx / 3 + B1;
    }
    ui &= 0x80000000u;
    ui |= hx;
    /* Two Newton steps in double precision (to 16, then 47 bits). */
    xd = (double)x;
    t = (double)pc_f32_from_bits(ui);
    r = t * t * t;
    t = t * (xd + xd + r) / (xd + r + r);
    r = t * t * t;
    t = t * (xd + xd + r) / (xd + r + r);
    /* rounding to 24 bits is perfect in round-to-nearest mode */
    return (float)t;
}
