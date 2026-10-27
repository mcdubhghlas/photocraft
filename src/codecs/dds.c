/* DirectDraw Surface (DDS) textures, after Microsoft's public DDS documentation ("DDS",
 * "DDS_HEADER", "DDS_PIXELFORMAT", "DDS_HEADER_DXT10"). Port of crates/codecs/src/codecs/dds.rs
 * and dds_bc.rs.
 *
 * Reading: the top mip level of a 2D texture, uncompressed (channel masks, RGB or luminance) or
 * BC1/BC2/BC3 (DXT1/DXT3/DXT5), with or without the DX10 header. Cubemaps, texture arrays and
 * volume textures are refused. Writing: a 2D texture, uncompressed (BGRA/BGRX, luminance with or
 * without alpha) or BC1/BC2/BC3, optionally with its full mip chain. */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "codecs/internal.h"

#define F PC_CF_DDS

enum {
    DDS_HEADER = 128,     /* `DDS ` plus the 124-byte DDS_HEADER */
    DDS_DX10_HEADER = 20, /* DDS_HEADER_DXT10, present when the FourCC is `DX10` */
};

#define DDSD_PITCH 0x8u
#define DDSD_DEPTH 0x800000u
#define DDPF_ALPHAPIXELS 0x1u
#define DDPF_FOURCC 0x4u
#define DDPF_RGB 0x40u
#define DDPF_LUMINANCE 0x20000u
#define DDSCAPS2_CUBEMAP 0x200u
#define DDSCAPS2_VOLUME 0x200000u
#define DIMENSION_TEXTURE2D 3u
#define MISC_TEXTURECUBE 0x4u

#define DDSD_CAPS 0x1u
#define DDSD_HEIGHT 0x2u
#define DDSD_WIDTH 0x4u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSD_MIPMAPCOUNT 0x20000u
#define DDSD_LINEARSIZE 0x80000u
#define DDSCAPS_COMPLEX 0x8u
#define DDSCAPS_TEXTURE 0x1000u
#define DDSCAPS_MIPMAP 0x400000u

/* ================================ shared BC block helpers ================================ */
/* A 4x4 block is 16 texels of RGBA (block[t][0..4]); `inside` has bit t set for texels that
 * count (an edge block of a texture whose size is not a multiple of four has texels outside). */

static uint8_t bc_scale(uint32_t v, uint32_t max)
{
    return (uint8_t)(((uint64_t)v * 255 + max / 2) / max);
}

static void rgb565(uint16_t c, uint8_t out[3])
{
    out[0] = bc_scale((uint32_t)(c >> 11), 31);
    out[1] = bc_scale((uint32_t)((c >> 5) & 63), 63);
    out[2] = bc_scale((uint32_t)(c & 31), 31);
}

/* The colours a colour block's indices select. Four-colour mode adds two colours at thirds;
 * three-colour mode adds the midpoint and transparent black. */
static void colour_palette(uint16_t c0, uint16_t c1, bool four, uint8_t pal[4][4])
{
    uint8_t a[3];
    uint8_t b[3];
    size_t i;
    rgb565(c0, a);
    rgb565(c1, b);
    for (i = 0; i < 3; i++) {
        pal[0][i] = a[i];
        pal[1][i] = b[i];
    }
    pal[0][3] = pal[1][3] = 255;
    if (four) {
        for (i = 0; i < 3; i++) {
            pal[2][i] = (uint8_t)((2u * a[i] + 1u * b[i] + 1) / 3);
            pal[3][i] = (uint8_t)((1u * a[i] + 2u * b[i] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (i = 0; i < 3; i++) {
            pal[2][i] = (uint8_t)((a[i] + b[i] + 1) / 2);
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
}

/* A BC3 alpha block's eight values. */
static void alpha_palette(uint8_t a0b, uint8_t a1b, uint8_t pal[8])
{
    uint32_t a0 = a0b;
    uint32_t a1 = a1b;
    uint32_t p[8] = {a0, a1, 0, 0, 0, 0, 0, 255};
    size_t k;
    if (a0 > a1) {
        for (k = 2; k < 8; k++) {
            p[k] = ((8 - (uint32_t)k) * a0 + ((uint32_t)k - 1) * a1 + 3) / 7;
        }
    } else {
        for (k = 2; k < 6; k++) {
            p[k] = ((6 - (uint32_t)k) * a0 + ((uint32_t)k - 1) * a1 + 2) / 5;
        }
    }
    for (k = 0; k < 8; k++) {
        pal[k] = (uint8_t)p[k];
    }
}

static void to_rgb_f(const uint8_t t[4], float out[3])
{
    out[0] = (float)t[0];
    out[1] = (float)t[1];
    out[2] = (float)t[2];
}

static uint16_t to565(const float c[3])
{
    float r = c[0] < 0.0f ? 0.0f : c[0] > 255.0f ? 255.0f : c[0];
    float g = c[1] < 0.0f ? 0.0f : c[1] > 255.0f ? 255.0f : c[1];
    float b = c[2] < 0.0f ? 0.0f : c[2] > 255.0f ? 255.0f : c[2];
    uint16_t qr = (uint16_t)(roundf(r * 31.0f / 255.0f));
    uint16_t qg = (uint16_t)(roundf(g * 63.0f / 255.0f));
    uint16_t qb = (uint16_t)(roundf(b * 31.0f / 255.0f));
    return (uint16_t)((qr << 11) | (qg << 5) | qb);
}

static uint32_t bc_distance(const uint8_t a[4], const uint8_t b[4])
{
    uint32_t s = 0;
    size_t i;
    for (i = 0; i < 3; i++) {
        int32_t d = (int32_t)a[i] - (int32_t)b[i];
        uint32_t ad = (uint32_t)(d < 0 ? -d : d);
        s += ad * ad;
    }
    return s;
}

static void bc_pack(uint16_t c0, uint16_t c1, uint32_t indices, uint8_t out[8])
{
    out[0] = (uint8_t)c0;
    out[1] = (uint8_t)(c0 >> 8);
    out[2] = (uint8_t)c1;
    out[3] = (uint8_t)(c1 >> 8);
    out[4] = (uint8_t)indices;
    out[5] = (uint8_t)(indices >> 8);
    out[6] = (uint8_t)(indices >> 16);
    out[7] = (uint8_t)(indices >> 24);
}

/* The ends of the colours' spread along their principal axis (power iteration), clamped to the
 * RGB cube. `colours` holds `nc` RGB triples. */
static void principal_endpoints(float (*colours)[3], size_t nc, float lo_out[3], float hi_out[3])
{
    float mean[3] = {0, 0, 0};
    float cov[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    float n = (float)(nc > 0 ? nc : 1);
    float axis[3];
    float best_axis[3] = {1.0f, 0.0f, 0.0f};
    float best_spread = -1.0f;
    size_t i;
    size_t start;
    float lo;
    float hi;
    if (nc == 0) {
        lo_out[0] = lo_out[1] = lo_out[2] = 0.0f;
        hi_out[0] = hi_out[1] = hi_out[2] = 0.0f;
        return;
    }
    for (i = 0; i < 3; i++) {
        size_t k;
        float s = 0.0f;
        for (k = 0; k < nc; k++) {
            s += colours[k][i];
        }
        mean[i] = s / n;
    }
    for (i = 0; i < nc; i++) {
        float d[3] = {colours[i][0] - mean[0], colours[i][1] - mean[1], colours[i][2] - mean[2]};
        size_t r;
        size_t c;
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                cov[r][c] += d[r] * d[c];
            }
        }
    }
    for (start = 0; start < 3; start++) {
        size_t it;
        axis[0] = axis[1] = axis[2] = 0.0f;
        axis[start] = 1.0f;
        for (it = 0; it < 8; it++) {
            float next[3];
            float len;
            size_t r;
            for (r = 0; r < 3; r++) {
                next[r] = cov[r][0] * axis[0] + cov[r][1] * axis[1] + cov[r][2] * axis[2];
            }
            len = sqrtf(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
            if (!(len > 0.0f) || !isnormal(len)) {
                break;
            }
            axis[0] = next[0] / len;
            axis[1] = next[1] / len;
            axis[2] = next[2] / len;
        }
        {
            float ca[3];
            float spread;
            size_t r;
            for (r = 0; r < 3; r++) {
                ca[r] = cov[r][0] * axis[0] + cov[r][1] * axis[1] + cov[r][2] * axis[2];
            }
            spread = ca[0] * axis[0] + ca[1] * axis[1] + ca[2] * axis[2];
            if (spread > best_spread) {
                best_spread = spread;
                best_axis[0] = axis[0];
                best_axis[1] = axis[1];
                best_axis[2] = axis[2];
            }
        }
    }
    lo = 1e30f;
    hi = -1e30f;
    for (i = 0; i < nc; i++) {
        float t = (colours[i][0] - mean[0]) * best_axis[0] + (colours[i][1] - mean[1]) * best_axis[1] +
                  (colours[i][2] - mean[2]) * best_axis[2];
        if (t < lo) {
            lo = t;
        }
        if (t > hi) {
            hi = t;
        }
    }
    if (lo > hi) {
        lo_out[0] = hi_out[0] = mean[0];
        lo_out[1] = hi_out[1] = mean[1];
        lo_out[2] = hi_out[2] = mean[2];
        return;
    }
    for (i = 0; i < 3; i++) {
        float vlo = mean[i] + lo * best_axis[i];
        float vhi = mean[i] + hi * best_axis[i];
        lo_out[i] = vlo < 0.0f ? 0.0f : vlo > 255.0f ? 255.0f : vlo;
        hi_out[i] = vhi < 0.0f ? 0.0f : vhi > 255.0f ? 255.0f : vhi;
    }
}

/* Orders endpoints for the mode and picks each texel's nearest palette colour. Writes the block
 * bytes and returns its squared error. */
static uint32_t bc_fit(uint8_t block[16][4], uint16_t opaque, uint16_t transparent, uint16_t e0, uint16_t e1,
                       bool four, uint8_t out[8])
{
    uint16_t c0;
    uint16_t c1;
    uint8_t pal[4][4];
    size_t choices;
    uint32_t indices = 0;
    uint32_t error = 0;
    size_t t;
    if (four == (e0 >= e1)) {
        c0 = e0;
        c1 = e1;
    } else {
        c0 = e1;
        c1 = e0;
    }
    colour_palette(c0, c1, four && c0 > c1, pal);
    choices = (four && c0 > c1) ? 4 : 3;
    for (t = 0; t < 16; t++) {
        uint32_t index;
        if ((transparent >> t) & 1) {
            index = 3;
        } else if ((opaque >> t) & 1) {
            size_t best_i = 0;
            uint32_t best_d = 0;
            size_t i;
            bool first = true;
            for (i = 0; i < choices; i++) {
                uint32_t d = bc_distance(block[t], pal[i]);
                if (first || d < best_d) {
                    best_d = d;
                    best_i = i;
                    first = false;
                }
            }
            error += best_d;
            index = (uint32_t)best_i;
        } else {
            index = 0;
        }
        indices |= index << (2 * t);
    }
    bc_pack(c0, c1, indices, out);
    return error;
}

/* The endpoints that minimise the squared error for the indices `bytes` chose. Returns false when
 * the indices do not constrain both endpoints. */
static bool bc_least_squares(uint8_t block[16][4], uint16_t opaque, const uint8_t bytes[8], uint16_t *e0o,
                             uint16_t *e1o)
{
    uint16_t c0 = (uint16_t)(bytes[0] | (bytes[1] << 8));
    uint16_t c1 = (uint16_t)(bytes[2] | (bytes[3] << 8));
    uint32_t indices = (uint32_t)bytes[4] | ((uint32_t)bytes[5] << 8) | ((uint32_t)bytes[6] << 16) |
                       ((uint32_t)bytes[7] << 24);
    bool four = c0 > c1;
    float aa = 0, ab = 0, bb = 0;
    float ax[3] = {0, 0, 0};
    float bx[3] = {0, 0, 0};
    float det;
    float e0[3];
    float e1[3];
    size_t t;
    for (t = 0; t < 16; t++) {
        uint32_t idx = (indices >> (2 * t)) & 3;
        float a;
        float bw;
        float c[3];
        if (((opaque >> t) & 1) == 0) {
            continue;
        }
        if (idx == 0) {
            a = 1.0f;
        } else if (idx == 1) {
            a = 0.0f;
        } else if (four && idx == 2) {
            a = 2.0f / 3.0f;
        } else if (four) {
            a = 1.0f / 3.0f;
        } else if (idx == 2) {
            a = 0.5f;
        } else {
            continue;
        }
        bw = 1.0f - a;
        to_rgb_f(block[t], c);
        aa += a * a;
        ab += a * bw;
        bb += bw * bw;
        {
            size_t i;
            for (i = 0; i < 3; i++) {
                ax[i] += a * c[i];
                bx[i] += bw * c[i];
            }
        }
    }
    det = aa * bb - ab * ab;
    if (fabsf(det) < 1e-3f) {
        return false;
    }
    {
        size_t i;
        for (i = 0; i < 3; i++) {
            e0[i] = (ax[i] * bb - bx[i] * ab) / det;
            e1[i] = (bx[i] * aa - ax[i] * ab) / det;
        }
    }
    *e0o = to565(e0);
    *e1o = to565(e1);
    return true;
}

/* A colour block (endpoints, then 2-bit indices) for the `opaque` texels' colours, with the
 * `transparent` ones on BC1's transparent index. `bc1` allows the three-colour mode. */
static void colour_block(uint8_t block[16][4], uint16_t opaque, uint16_t transparent, bool bc1, uint8_t out[8])
{
    float colours[16][3];
    size_t nc = 0;
    float lo[3];
    float hi[3];
    uint16_t cand[16][2];
    size_t ncand = 0;
    bool modes[2];
    size_t nmodes;
    bool have_best = false;
    uint32_t best_err = 0;
    uint8_t best_bytes[8];
    int round;
    size_t t;
    for (t = 0; t < 16; t++) {
        if ((opaque >> t) & 1) {
            to_rgb_f(block[t], colours[nc]);
            nc++;
        }
    }
    if (nc == 0) {
        uint32_t indices = transparent != 0 ? 0xFFFFFFFFu : 0;
        bc_pack(0, 0, indices, out);
        return;
    }
    principal_endpoints(colours, nc, lo, hi);
    cand[0][0] = to565(lo);
    cand[0][1] = to565(hi);
    ncand = 1;
    if (transparent != 0) {
        modes[0] = false;
        nmodes = 1;
    } else if (bc1) {
        modes[0] = true;
        modes[1] = false;
        nmodes = 2;
    } else {
        modes[0] = true;
        nmodes = 1;
    }
    for (round = 0; round < 3; round++) {
        size_t ci;
        uint16_t ls0;
        uint16_t ls1;
        for (ci = 0; ci < ncand; ci++) {
            size_t mi;
            for (mi = 0; mi < nmodes; mi++) {
                uint8_t bytes[8];
                uint32_t err = bc_fit(block, opaque, transparent, cand[ci][0], cand[ci][1], modes[mi], bytes);
                if (!have_best || err < best_err) {
                    have_best = true;
                    best_err = err;
                    memcpy(best_bytes, bytes, 8);
                }
            }
        }
        if (!have_best || best_err == 0 || round == 2) {
            break;
        }
        if (!bc_least_squares(block, opaque, best_bytes, &ls0, &ls1)) {
            break;
        }
        cand[0][0] = ls0;
        cand[0][1] = ls1;
        ncand = 1;
    }
    if (have_best) {
        memcpy(out, best_bytes, 8);
    } else {
        memset(out, 0, 8);
    }
}

/* The texels of `inside` for which alpha < 128 (BC1 punch-through). */
static uint16_t select_transparent(uint8_t block[16][4], uint16_t inside)
{
    uint16_t m = 0;
    size_t t;
    for (t = 0; t < 16; t++) {
        if (((inside >> t) & 1) && block[t][3] < 128) {
            m |= (uint16_t)(1u << t);
        }
    }
    return m;
}

static uint32_t alpha_fit(uint8_t block[16][4], uint16_t inside, uint8_t a0, uint8_t a1, uint8_t out[8])
{
    uint8_t pal[8];
    uint64_t bits = 0;
    uint32_t error = 0;
    size_t t;
    alpha_palette(a0, a1, pal);
    for (t = 0; t < 16; t++) {
        size_t best_i = 0;
        uint32_t best_d = 0;
        bool first = true;
        size_t i;
        if (((inside >> t) & 1) == 0) {
            continue;
        }
        for (i = 0; i < 8; i++) {
            int32_t d = (int32_t)pal[i] - (int32_t)block[t][3];
            uint32_t ad = (uint32_t)(d < 0 ? -d : d);
            uint32_t dd = ad * ad;
            if (first || dd < best_d) {
                best_d = dd;
                best_i = i;
                first = false;
            }
        }
        error += best_d;
        bits |= (uint64_t)best_i << (3 * t);
    }
    out[0] = a0;
    out[1] = a1;
    out[2] = (uint8_t)bits;
    out[3] = (uint8_t)(bits >> 8);
    out[4] = (uint8_t)(bits >> 16);
    out[5] = (uint8_t)(bits >> 24);
    out[6] = (uint8_t)(bits >> 32);
    out[7] = (uint8_t)(bits >> 40);
    return error;
}

/* A BC3 alpha block: whichever of the two modes fits better. */
static void alpha_block(uint8_t block[16][4], uint16_t inside, uint8_t out[8])
{
    uint8_t lo = 0;
    uint8_t hi = 0;
    bool have = false;
    uint8_t mlo = 0;
    uint8_t mhi = 0;
    bool mhave = false;
    uint8_t eight[8];
    uint8_t six[8];
    uint32_t eerr;
    uint32_t serr;
    size_t t;
    for (t = 0; t < 16; t++) {
        uint8_t a = block[t][3];
        if (((inside >> t) & 1) == 0) {
            continue;
        }
        if (!have) {
            lo = hi = a;
            have = true;
        } else {
            if (a < lo) {
                lo = a;
            }
            if (a > hi) {
                hi = a;
            }
        }
        if (a != 0 && a != 255) {
            if (!mhave) {
                mlo = mhi = a;
                mhave = true;
            } else {
                if (a < mlo) {
                    mlo = a;
                }
                if (a > mhi) {
                    mhi = a;
                }
            }
        }
    }
    eerr = alpha_fit(block, inside, hi, lo, eight);
    serr = alpha_fit(block, inside, mlo, mhi, six);
    memcpy(out, serr < eerr ? six : eight, 8);
}

static void encode_bc1(uint8_t block[16][4], uint16_t inside, uint8_t out[8])
{
    uint16_t transparent = select_transparent(block, inside);
    colour_block(block, (uint16_t)(inside & ~transparent), transparent, true, out);
}

static void encode_bc2(uint8_t block[16][4], uint16_t inside, uint8_t out[16])
{
    uint64_t bits = 0;
    size_t t;
    for (t = 0; t < 16; t++) {
        bits |= (uint64_t)(((uint32_t)block[t][3] * 15 + 127) / 255) << (4 * t);
    }
    for (t = 0; t < 8; t++) {
        out[t] = (uint8_t)(bits >> (8 * t));
    }
    colour_block(block, inside, 0, false, out + 8);
}

static void encode_bc3(uint8_t block[16][4], uint16_t inside, uint8_t out[16])
{
    alpha_block(block, inside, out);
    colour_block(block, inside, 0, false, out + 8);
}

/* ================================ decode ================================ */

static bool dds_le32(const uint8_t *b, size_t n, size_t at, uint32_t *v)
{
    if (at > n || n - at < 4) {
        return false;
    }
    *v = (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24);
    return true;
}

typedef enum { ENC_MASKS, ENC_BC1, ENC_BC2, ENC_BC3 } enc_kind;

typedef struct {
    enc_kind kind;
    size_t bytes; /* masks: bytes per pixel */
    bool gray;
    bool has_field[4];
    uint32_t shift[4];
    uint32_t fmax[4];
} dds_encoding;

static uint32_t trailing_zeros32(uint32_t v)
{
    uint32_t c = 0;
    if (v == 0) {
        return 32;
    }
    while ((v & 1u) == 0) {
        v >>= 1;
        c++;
    }
    return c;
}

static uint32_t leading_zeros32(uint32_t v)
{
    uint32_t c = 0;
    uint32_t bit = 0x80000000u;
    if (v == 0) {
        return 32;
    }
    while ((v & bit) == 0) {
        c++;
        bit >>= 1;
    }
    return c;
}

/* Field::new: sets *present and (shift,max) or errors for a non-contiguous mask. */
static pc_status field_new(uint32_t mask, uint32_t bits, bool *present, uint32_t *shift, uint32_t *fmax,
                           pc_error *err)
{
    uint32_t s;
    uint32_t m;
    if (mask == 0) {
        *present = false;
        return PC_OK;
    }
    s = trailing_zeros32(mask);
    m = mask >> s;
    if ((m & (m + 1)) != 0 || (bits < 32 && (mask >> bits) != 0)) {
        return cx_malformed(err, F, "channel mask %#x is not a contiguous field of a %u-bit pixel",
                            (unsigned)mask, (unsigned)bits);
    }
    *present = true;
    *shift = s;
    *fmax = m;
    return PC_OK;
}

static uint8_t field_get(uint32_t shift, uint32_t fmax, uint32_t pixel)
{
    uint64_t v = (pixel >> shift) & fmax;
    uint64_t max = fmax;
    return (uint8_t)((v * 255 + max / 2) / max);
}

static pc_status encoding_masks(uint32_t bits, const uint32_t masks[4], bool gray, dds_encoding *out, pc_error *err)
{
    size_t i;
    bool colour;
    pc_status st;
    if (!(bits == 8 || bits == 16 || bits == 24 || bits == 32)) {
        return cx_malformed(err, F, "%u bits per pixel is not 8, 16, 24 or 32", (unsigned)bits);
    }
    out->kind = ENC_MASKS;
    out->bytes = bits / 8;
    out->gray = gray;
    for (i = 0; i < 4; i++) {
        st = field_new(masks[i], bits, &out->has_field[i], &out->shift[i], &out->fmax[i], err);
        if (st != PC_OK) {
            return st;
        }
    }
    colour = gray ? out->has_field[0] : (out->has_field[0] || out->has_field[1] || out->has_field[2]);
    if (!colour) {
        return cx_malformed(err, F, "the pixel format has no colour channel masks");
    }
    return PC_OK;
}

static pc_clayout encoding_layout(const dds_encoding *e)
{
    if (e->kind == ENC_MASKS) {
        if (e->gray) {
            return e->has_field[3] ? PC_CL_GRAYA : PC_CL_GRAY;
        }
        if (!e->has_field[3]) {
            return PC_CL_RGB;
        }
    }
    return PC_CL_RGBA;
}

/* Stored row stride and byte size of the top mip level. */
static pc_status encoding_level_layout(const dds_encoding *e, uint32_t width, uint32_t height, bool has_pitch,
                                       uint32_t pitch, size_t *row_bytes_out, size_t *size_out, pc_error *err)
{
    size_t w = width;
    size_t h = height;
    size_t row_bytes;
    size_t rows;
    size_t size;
    if (e->kind == ENC_MASKS) {
        size_t packed;
        if (e->bytes != 0 && w > SIZE_MAX / e->bytes) {
            return cx_malformed(err, F, "pixel data size overflow");
        }
        packed = w * e->bytes;
        row_bytes = has_pitch ? (size_t)pitch : packed;
        if (row_bytes < packed) {
            return cx_malformed(err, F, "row pitch %zu is smaller than the %zu-byte pixel row", row_bytes, packed);
        }
        rows = h;
    } else {
        size_t block = e->kind == ENC_BC1 ? 8 : 16;
        size_t bx = (w + 3) / 4;
        if (bx != 0 && block > SIZE_MAX / bx) {
            return cx_malformed(err, F, "pixel data size overflow");
        }
        row_bytes = bx * block;
        rows = (h + 3) / 4;
    }
    if (rows != 0 && row_bytes > SIZE_MAX / rows) {
        return cx_malformed(err, F, "pixel data size overflow");
    }
    size = row_bytes * rows;
    *row_bytes_out = row_bytes;
    *size_out = size;
    return PC_OK;
}

static pc_status legacy_encoding(const uint8_t *b, size_t n, dds_encoding *out, pc_error *err)
{
    uint32_t flags;
    uint32_t fourcc;
    uint32_t bits;
    uint32_t alpha = 0;
    uint32_t masks[4];
    if (!dds_le32(b, n, 80, &flags) || !dds_le32(b, n, 84, &fourcc) || !dds_le32(b, n, 88, &bits)) {
        return cx_malformed(err, F, "truncated header");
    }
    if ((flags & DDPF_ALPHAPIXELS) && !dds_le32(b, n, 104, &alpha)) {
        return cx_malformed(err, F, "truncated header");
    }
    if (flags & DDPF_FOURCC) {
        uint8_t fc[4] = {(uint8_t)fourcc, (uint8_t)(fourcc >> 8), (uint8_t)(fourcc >> 16), (uint8_t)(fourcc >> 24)};
        if (memcmp(fc, "DXT1", 4) == 0) {
            out->kind = ENC_BC1;
            return PC_OK;
        }
        if (memcmp(fc, "DXT3", 4) == 0) {
            out->kind = ENC_BC2;
            return PC_OK;
        }
        if (memcmp(fc, "DXT5", 4) == 0) {
            out->kind = ENC_BC3;
            return PC_OK;
        }
        if (memcmp(fc, "DXT2", 4) == 0 || memcmp(fc, "DXT4", 4) == 0) {
            return cx_unsupported(err, F, "premultiplied-alpha DXT2/DXT4 textures are not supported");
        }
        {
            bool printable = true;
            size_t i;
            for (i = 0; i < 4; i++) {
                if (!((fc[i] >= 0x21 && fc[i] <= 0x7e) || fc[i] == ' ')) {
                    printable = false;
                }
            }
            if (printable) {
                return cx_unsupported(err, F, "compression `%c%c%c%c` is not supported (DXT1, DXT3 and DXT5 are)",
                                      fc[0], fc[1], fc[2], fc[3]);
            }
            return cx_unsupported(err, F, "compression `%u` is not supported (DXT1, DXT3 and DXT5 are)",
                                  (unsigned)fourcc);
        }
    }
    if (flags & DDPF_RGB) {
        if (!dds_le32(b, n, 92, &masks[0]) || !dds_le32(b, n, 96, &masks[1]) || !dds_le32(b, n, 100, &masks[2])) {
            return cx_malformed(err, F, "truncated header");
        }
        masks[3] = alpha;
        return encoding_masks(bits, masks, false, out, err);
    }
    if (flags & DDPF_LUMINANCE) {
        if (!dds_le32(b, n, 92, &masks[0])) {
            return cx_malformed(err, F, "truncated header");
        }
        masks[1] = masks[2] = 0;
        masks[3] = alpha;
        return encoding_masks(bits, masks, true, out, err);
    }
    return cx_unsupported(err, F, "pixel format flags %#x are not RGB, luminance or a supported compression",
                          (unsigned)flags);
}

static pc_status dx10_encoding(const uint8_t *b, size_t n, dds_encoding *out, pc_error *err)
{
    uint32_t dxgi;
    uint32_t dimension;
    uint32_t misc;
    uint32_t array;
    static const uint32_t RGBA[4] = {0xffu, 0xff00u, 0xff0000u, 0xff000000u};
    static const uint32_t BGRA[4] = {0xff0000u, 0xff00u, 0xffu, 0xff000000u};
    static const uint32_t BGRX[4] = {0xff0000u, 0xff00u, 0xffu, 0u};
    if (!dds_le32(b, n, DDS_HEADER, &dxgi) || !dds_le32(b, n, DDS_HEADER + 4, &dimension) ||
        !dds_le32(b, n, DDS_HEADER + 8, &misc) || !dds_le32(b, n, DDS_HEADER + 12, &array)) {
        return cx_malformed(err, F, "truncated header");
    }
    if (misc & MISC_TEXTURECUBE) {
        return cx_unsupported(err, F, "cubemaps are not supported");
    }
    if (dimension != DIMENSION_TEXTURE2D) {
        return cx_unsupported(err, F, "only 2D textures are supported (resource dimension %u)", (unsigned)dimension);
    }
    if (array > 1) {
        return cx_unsupported(err, F, "texture arrays are not supported (%u textures)", (unsigned)array);
    }
    if (dxgi >= 27 && dxgi <= 29) {
        return encoding_masks(32, RGBA, false, out, err);
    }
    if (dxgi == 87 || dxgi == 90 || dxgi == 91) {
        return encoding_masks(32, BGRA, false, out, err);
    }
    if (dxgi == 88 || dxgi == 92 || dxgi == 93) {
        return encoding_masks(32, BGRX, false, out, err);
    }
    if (dxgi >= 70 && dxgi <= 72) {
        out->kind = ENC_BC1;
        return PC_OK;
    }
    if (dxgi >= 73 && dxgi <= 75) {
        out->kind = ENC_BC2;
        return PC_OK;
    }
    if (dxgi >= 76 && dxgi <= 78) {
        out->kind = ENC_BC3;
        return PC_OK;
    }
    return cx_unsupported(err, F, "DXGI format %u is not supported (BC1, BC2, BC3 and 8-bit RGBA or BGRA are)",
                          (unsigned)dxgi);
}

/* Decodes one BC1 colour block into 16 RGBA texels. */
static void bc1_block(const uint8_t *block, size_t block_n, bool one_bit_alpha, uint8_t texels[16][4])
{
    uint16_t c0 = (uint16_t)((block_n > 0 ? block[0] : 0) | ((block_n > 1 ? block[1] : 0) << 8));
    uint16_t c1 = (uint16_t)((block_n > 2 ? block[2] : 0) | ((block_n > 3 ? block[3] : 0) << 8));
    uint8_t pal[4][4];
    uint32_t indices = (uint32_t)(block_n > 4 ? block[4] : 0) | ((uint32_t)(block_n > 5 ? block[5] : 0) << 8) |
                       ((uint32_t)(block_n > 6 ? block[6] : 0) << 16) | ((uint32_t)(block_n > 7 ? block[7] : 0) << 24);
    size_t t;
    colour_palette(c0, c1, c0 > c1 || !one_bit_alpha, pal);
    for (t = 0; t < 16; t++) {
        memcpy(texels[t], pal[(indices >> (2 * t)) & 3], 4);
    }
}

static pc_status decode_masks(const dds_encoding *e, const uint8_t *data, size_t row_pitch, size_t w, size_t h,
                              pc_clayout layout, uint8_t *pixels, pc_error *err)
{
    size_t channels = pc_clayout_channels(layout);
    size_t row_size = w * channels;
    size_t y;
    (void)err;
    for (y = 0; y < h; y++) {
        const uint8_t *src_row = data + y * row_pitch;
        uint8_t *dst_row = pixels + y * row_size;
        size_t x;
        for (x = 0; x < w; x++) {
            const uint8_t *src = src_row + x * e->bytes;
            uint8_t *dst = dst_row + x * channels;
            uint32_t pixel = 0;
            size_t i;
            uint8_t values[4];
            for (i = e->bytes; i-- > 0;) {
                pixel = (pixel << 8) | src[i];
            }
            {
                uint8_t ch[4];
                for (i = 0; i < 4; i++) {
                    ch[i] = e->has_field[i] ? field_get(e->shift[i], e->fmax[i], pixel) : 0;
                }
                if (e->gray) {
                    values[0] = ch[0];
                    values[1] = ch[3];
                    values[2] = 0;
                    values[3] = 0;
                } else {
                    values[0] = ch[0];
                    values[1] = ch[1];
                    values[2] = ch[2];
                    values[3] = ch[3];
                }
            }
            memcpy(dst, values, channels);
        }
    }
    return PC_OK;
}

static void decode_bc(const dds_encoding *e, const uint8_t *data, size_t size, size_t w, size_t h, uint8_t *pixels)
{
    size_t block_size = e->kind == ENC_BC1 ? 8 : 16;
    size_t blocks_x = (w + 3) / 4;
    size_t nblocks = block_size > 0 ? size / block_size : 0;
    size_t i;
    for (i = 0; i < nblocks; i++) {
        const uint8_t *block = data + i * block_size;
        uint8_t texels[16][4];
        size_t bx = (i % (blocks_x > 0 ? blocks_x : 1)) * 4;
        size_t by = (i / (blocks_x > 0 ? blocks_x : 1)) * 4;
        size_t t;
        if (e->kind == ENC_BC1) {
            bc1_block(block, block_size, true, texels);
        } else {
            size_t ci;
            bc1_block(block + 8, block_size - 8, false, texels);
            if (e->kind == ENC_BC2) {
                uint64_t bits = 0;
                for (ci = 0; ci < 8; ci++) {
                    bits |= (uint64_t)block[ci] << (8 * ci);
                }
                for (t = 0; t < 16; t++) {
                    texels[t][3] = (uint8_t)(((bits >> (4 * t)) & 0xf) * 17);
                }
            } else {
                uint8_t pal[8];
                uint64_t bits = 0;
                alpha_palette(block[0], block[1], pal);
                for (ci = 2; ci < 8; ci++) {
                    bits |= (uint64_t)block[ci] << (8 * (ci - 2));
                }
                for (t = 0; t < 16; t++) {
                    texels[t][3] = pal[(bits >> (3 * t)) & 7];
                }
            }
        }
        for (t = 0; t < 16; t++) {
            size_t x = bx + t % 4;
            size_t y = by + t / 4;
            if (x < w && y < h) {
                memcpy(pixels + (y * w + x) * 4, texels[t], 4);
            }
        }
    }
}

pc_status cx_dds_decode(const uint8_t *b, size_t n, const pc_climits *lim, pc_cimage *out, pc_error *err)
{
    uint32_t hsize;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t depth;
    uint32_t caps2;
    uint32_t pf_flags;
    uint32_t pf_fourcc;
    bool dx10;
    dds_encoding e;
    pc_clayout layout;
    bool has_pitch;
    uint32_t pitch = 0;
    size_t start;
    size_t row_pitch;
    size_t size;
    size_t w;
    size_t h;
    size_t channels;
    size_t row_size;
    size_t total;
    uint8_t *pixels;
    const uint8_t *data;
    pc_status st;
    memset(&e, 0, sizeof e);
    if (n < 4 || memcmp(b, "DDS ", 4) != 0) {
        return cx_malformed(err, F, "missing the `DDS ` signature");
    }
    if (!dds_le32(b, n, 4, &hsize) || hsize != 124) {
        return cx_malformed(err, F, "header size is not 124");
    }
    if (!dds_le32(b, n, 8, &flags) || !dds_le32(b, n, 12, &height) || !dds_le32(b, n, 16, &width) ||
        !dds_le32(b, n, 24, &depth) || !dds_le32(b, n, 112, &caps2)) {
        return cx_malformed(err, F, "truncated header");
    }
    if (caps2 & DDSCAPS2_CUBEMAP) {
        return cx_unsupported(err, F, "cubemaps are not supported");
    }
    if ((caps2 & DDSCAPS2_VOLUME) || ((flags & DDSD_DEPTH) && depth > 1)) {
        return cx_unsupported(err, F, "volume textures are not supported");
    }
    if (!dds_le32(b, n, 80, &pf_flags) || !dds_le32(b, n, 84, &pf_fourcc)) {
        return cx_malformed(err, F, "truncated header");
    }
    dx10 = (pf_flags & DDPF_FOURCC) && pf_fourcc == 0x30315844u; /* "DX10" little-endian */
    if (dx10) {
        st = dx10_encoding(b, n, &e, err);
        start = DDS_HEADER + DDS_DX10_HEADER;
    } else {
        st = legacy_encoding(b, n, &e, err);
        start = DDS_HEADER;
    }
    if (st != PC_OK) {
        return st;
    }
    layout = encoding_layout(&e);
    st = pc_climits_check(lim, width, height, layout, PC_CS_U8, err);
    if (st != PC_OK) {
        return st;
    }
    has_pitch = (flags & DDSD_PITCH) != 0;
    if (has_pitch && !dds_le32(b, n, 20, &pitch)) {
        return cx_malformed(err, F, "truncated header");
    }
    st = encoding_level_layout(&e, width, height, has_pitch, pitch, &row_pitch, &size, err);
    if (st != PC_OK) {
        return st;
    }
    if (start > n || n - start < size) {
        return cx_malformed(err, F, "truncated pixel data: %zu bytes needed", size);
    }
    data = b + start;
    w = width;
    h = height;
    channels = pc_clayout_channels(layout);
    if (channels != 0 && w > SIZE_MAX / channels) {
        return cx_malformed(err, F, "pixel size overflow");
    }
    row_size = w * channels;
    if (h != 0 && row_size > SIZE_MAX / h) {
        return cx_malformed(err, F, "pixel size overflow");
    }
    total = row_size * h;
    pixels = calloc(total > 0 ? total : 1, 1);
    if (pixels == NULL) {
        return cx_nomem(err);
    }
    if (e.kind == ENC_MASKS) {
        st = decode_masks(&e, data, row_pitch, w, h, layout, pixels, err);
        if (st != PC_OK) {
            free(pixels);
            return st;
        }
    } else {
        decode_bc(&e, data, size, w, h, pixels);
    }
    return pc_cimage_from_raw(width, height, layout, PC_CS_U8, pixels, total, out, err);
}

/* ================================ encode ================================ */

typedef enum { STORE_LUM, STORE_LUMA, STORE_BGRX, STORE_BGRA, STORE_BC1, STORE_BC2, STORE_BC3 } store_t;

static pc_status store_new(pc_dds_compression comp, pc_clayout layout, store_t *out, pc_error *err)
{
    if (comp == PC_DDS_NONE) {
        switch (layout) {
        case PC_CL_GRAY:
            *out = STORE_LUM;
            return PC_OK;
        case PC_CL_GRAYA:
            *out = STORE_LUMA;
            return PC_OK;
        case PC_CL_RGB:
            *out = STORE_BGRX;
            return PC_OK;
        case PC_CL_RGBA:
            *out = STORE_BGRA;
            return PC_OK;
        default:
            break;
        }
    } else if (layout == PC_CL_RGBA) {
        *out = comp == PC_DDS_BC1 ? STORE_BC1 : comp == PC_DDS_BC2 ? STORE_BC2 : STORE_BC3;
        return PC_OK;
    }
    return cx_encode(err, F, "%s pixels can't be written as `%s`", pc_clayout_name(layout),
                     pc_dds_compression_name(comp));
}

static size_t store_unit(store_t s)
{
    switch (s) {
    case STORE_LUM:
        return 1;
    case STORE_LUMA:
        return 2;
    case STORE_BGRX:
    case STORE_BGRA:
        return 4;
    case STORE_BC1:
        return 8;
    default:
        return 16;
    }
}

static bool store_is_block(store_t s) { return s == STORE_BC1 || s == STORE_BC2 || s == STORE_BC3; }

static bool store_row_bytes(store_t s, size_t width, size_t *out)
{
    size_t unit = store_unit(s);
    if (store_is_block(s)) {
        size_t bx = (width + 3) / 4;
        if (bx != 0 && unit > SIZE_MAX / bx) {
            return false;
        }
        *out = bx * unit;
    } else {
        if (width != 0 && unit > SIZE_MAX / width) {
            return false;
        }
        *out = width * unit;
    }
    return true;
}

static bool store_level_bytes(store_t s, size_t width, size_t height, size_t *out)
{
    size_t rows = store_is_block(s) ? (height + 3) / 4 : height;
    size_t rb;
    if (!store_row_bytes(s, width, &rb)) {
        return false;
    }
    if (rows != 0 && rb > SIZE_MAX / rows) {
        return false;
    }
    *out = rb * rows;
    return true;
}

static void store_pixel_format(store_t s, uint32_t *flags, uint8_t fourcc[4], uint32_t *bits, uint32_t masks[4])
{
    memset(fourcc, 0, 4);
    masks[0] = masks[1] = masks[2] = masks[3] = 0;
    switch (s) {
    case STORE_LUM:
        *flags = DDPF_LUMINANCE;
        *bits = 8;
        masks[0] = 0xff;
        break;
    case STORE_LUMA:
        *flags = DDPF_LUMINANCE | DDPF_ALPHAPIXELS;
        *bits = 16;
        masks[0] = 0xff;
        masks[3] = 0xff00;
        break;
    case STORE_BGRX:
        *flags = DDPF_RGB;
        *bits = 32;
        masks[0] = 0xff0000;
        masks[1] = 0xff00;
        masks[2] = 0xff;
        break;
    case STORE_BGRA:
        *flags = DDPF_RGB | DDPF_ALPHAPIXELS;
        *bits = 32;
        masks[0] = 0xff0000;
        masks[1] = 0xff00;
        masks[2] = 0xff;
        masks[3] = 0xff000000u;
        break;
    case STORE_BC1:
        *flags = DDPF_FOURCC;
        *bits = 0;
        memcpy(fourcc, "DXT1", 4);
        break;
    case STORE_BC2:
        *flags = DDPF_FOURCC;
        *bits = 0;
        memcpy(fourcc, "DXT3", 4);
        break;
    default:
        *flags = DDPF_FOURCC;
        *bits = 0;
        memcpy(fourcc, "DXT5", 4);
        break;
    }
}

static void le32_put(uint8_t *b, size_t at, uint32_t v)
{
    b[at] = (uint8_t)v;
    b[at + 1] = (uint8_t)(v >> 8);
    b[at + 2] = (uint8_t)(v >> 16);
    b[at + 3] = (uint8_t)(v >> 24);
}

static pc_status dds_header(uint32_t width, uint32_t height, store_t store, uint32_t levels, size_t top,
                            uint8_t out[DDS_HEADER], pc_error *err)
{
    uint32_t size_flag;
    size_t pitch_or_size_sz;
    uint32_t pitch_or_size;
    bool mipmapped = levels > 1;
    uint32_t flags;
    uint8_t fourcc[4];
    uint32_t bits;
    uint32_t masks[4];
    size_t i;
    memset(out, 0, DDS_HEADER);
    if (store_is_block(store)) {
        size_flag = DDSD_LINEARSIZE;
        pitch_or_size_sz = top;
    } else {
        size_flag = DDSD_PITCH;
        if (!store_row_bytes(store, width, &pitch_or_size_sz)) {
            return cx_encode(err, F, "a %ux%u level is too large for a DDS header", (unsigned)width, (unsigned)height);
        }
    }
    if (pitch_or_size_sz > 0xFFFFFFFFu) {
        return cx_encode(err, F, "a %ux%u level is too large for a DDS header", (unsigned)width, (unsigned)height);
    }
    pitch_or_size = (uint32_t)pitch_or_size_sz;
    le32_put(out, 4, 124);
    le32_put(out, 8,
             DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | size_flag |
                 (mipmapped ? DDSD_MIPMAPCOUNT : 0));
    le32_put(out, 12, height);
    le32_put(out, 16, width);
    le32_put(out, 20, pitch_or_size);
    le32_put(out, 28, levels);
    store_pixel_format(store, &flags, fourcc, &bits, masks);
    le32_put(out, 76, 32);
    le32_put(out, 80, flags);
    le32_put(out, 84, (uint32_t)fourcc[0] | ((uint32_t)fourcc[1] << 8) | ((uint32_t)fourcc[2] << 16) |
                          ((uint32_t)fourcc[3] << 24));
    le32_put(out, 88, bits);
    for (i = 0; i < 4; i++) {
        le32_put(out, 92 + 4 * i, masks[i]);
    }
    le32_put(out, 108, DDSCAPS_TEXTURE | (mipmapped ? (DDSCAPS_COMPLEX | DDSCAPS_MIPMAP) : 0));
    memcpy(out, "DDS ", 4);
    return PC_OK;
}

static uint32_t mip_levels(uint32_t width, uint32_t height)
{
    uint32_t m = width > height ? width : height;
    if (m < 1) {
        m = 1;
    }
    return 32 - leading_zeros32(m);
}

static void gather_block(const uint8_t *pixels, size_t width, size_t height, size_t bx, size_t by,
                         uint8_t block[16][4], uint16_t *inside)
{
    size_t t;
    uint16_t m = 0;
    memset(block, 0, 16 * 4);
    for (t = 0; t < 16; t++) {
        size_t x = bx * 4 + t % 4;
        size_t y = by * 4 + t / 4;
        if (x < width && y < height) {
            memcpy(block[t], pixels + (y * width + x) * 4, 4);
            m |= (uint16_t)(1u << t);
        }
    }
    *inside = m;
}

static pc_status write_level(store_t store, const uint8_t *pixels, size_t pixels_len, size_t width, size_t height,
                             uint8_t *dst, size_t dst_len, pc_error *err)
{
    size_t row_bytes;
    if (!store_row_bytes(store, width, &row_bytes)) {
        return cx_encode(err, F, "texture size overflow");
    }
    if (store == STORE_LUM || store == STORE_LUMA) {
        if (pixels_len < dst_len) {
            return cx_encode(err, F, "the level is smaller than its size");
        }
        memcpy(dst, pixels, dst_len);
    } else if (store == STORE_BGRX || store == STORE_BGRA) {
        size_t channels = store == STORE_BGRA ? 4 : 3;
        size_t y;
        for (y = 0; y < height; y++) {
            uint8_t *row = dst + y * row_bytes;
            size_t base = y * width * channels;
            size_t x;
            for (x = 0; x < width; x++) {
                size_t off = base + x * channels;
                const uint8_t *s = pixels + off;
                size_t rem = off < pixels_len ? pixels_len - off : 0;
                uint8_t d[4];
                if (rem == 0) {
                    continue; /* past the source: leave the zero-filled destination (as the reference does) */
                }
                d[0] = (channels > 2 && 2 < rem) ? s[2] : 255;
                d[1] = (channels > 1 && 1 < rem) ? s[1] : 255;
                d[2] = (channels > 0 && 0 < rem) ? s[0] : 255;
                d[3] = (channels > 3 && 3 < rem) ? s[3] : 255;
                memcpy(row + x * 4, d, 4);
            }
        }
    } else {
        size_t blocks_y = (height + 3) / 4;
        size_t by;
        for (by = 0; by < blocks_y; by++) {
            uint8_t *row = dst + by * row_bytes;
            size_t blocks_x = (width + 3) / 4;
            size_t bx;
            for (bx = 0; bx < blocks_x; bx++) {
                uint8_t block[16][4];
                uint16_t inside;
                uint8_t enc[16];
                gather_block(pixels, width, height, bx, by, block, &inside);
                if (store == STORE_BC1) {
                    encode_bc1(block, inside, enc);
                } else if (store == STORE_BC2) {
                    encode_bc2(block, inside, enc);
                } else {
                    encode_bc3(block, inside, enc);
                }
                memcpy(row + bx * store_unit(store), enc, store_unit(store));
            }
        }
    }
    return PC_OK;
}

static pc_status downsample(const uint8_t *src, size_t w, size_t h, size_t nw, size_t nh, size_t channels,
                            uint8_t **out, pc_error *err)
{
    size_t len;
    uint8_t *dst;
    size_t y;
    if (nw != 0 && nh > SIZE_MAX / nw) {
        return cx_encode(err, F, "mip level size overflow");
    }
    len = nw * nh;
    if (channels != 0 && len > SIZE_MAX / channels) {
        return cx_encode(err, F, "mip level size overflow");
    }
    len *= channels;
    dst = calloc(len > 0 ? len : 1, 1);
    if (dst == NULL) {
        return cx_nomem(err);
    }
    for (y = 0; y < nh; y++) {
        size_t y0 = 2 * y;
        size_t y1 = (y + 1 == nh) ? (h > 0 ? h - 1 : 0) : 2 * y + 1;
        size_t x;
        for (x = 0; x < nw; x++) {
            size_t x0 = 2 * x;
            size_t x1 = (x + 1 == nw) ? (w > 0 ? w - 1 : 0) : 2 * x + 1;
            size_t c;
            for (c = 0; c < channels; c++) {
                uint32_t sum = 0;
                uint32_t cnt = 0;
                size_t sy;
                for (sy = y0; sy <= y1; sy++) {
                    size_t sx;
                    for (sx = x0; sx <= x1; sx++) {
                        size_t at = (sy * w + sx) * channels + c;
                        if (at < w * h * channels) {
                            sum += src[at];
                            cnt++;
                        }
                    }
                }
                dst[(y * nw + x) * channels + c] = (uint8_t)((sum + cnt / 2) / (cnt > 0 ? cnt : 1));
            }
        }
    }
    *out = dst;
    return PC_OK;
}

pc_status cx_dds_encode(const pc_cimage *img, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *src;
    store_t store;
    uint32_t width;
    uint32_t height;
    uint32_t levels;
    size_t channels;
    size_t sizes[32];
    size_t nsizes = 0;
    size_t total = DDS_HEADER;
    uint8_t *buf = NULL;
    uint8_t header[DDS_HEADER];
    size_t sw;
    size_t sh;
    size_t i;
    size_t at;
    uint8_t *level = NULL;
    bool level_owned = false;
    pc_status st = pc_cimage_converted(img, p.layout, PC_CS_U8, &tmp, &src, err);
    if (st != PC_OK) {
        pc_cimage_fini(&tmp);
        return st;
    }
    if (src->sample != PC_CS_U8) {
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "the image was not converted to 8 bits per channel");
    }
    st = store_new(o->dds_compression, src->layout, &store, err);
    if (st != PC_OK) {
        pc_cimage_fini(&tmp);
        return st;
    }
    width = src->width;
    height = src->height;
    levels = o->dds_mipmaps ? mip_levels(width, height) : 1;
    if (levels > 32) {
        levels = 32;
    }
    channels = pc_clayout_channels(src->layout);
    sw = width;
    sh = height;
    for (i = 0; i < levels; i++) {
        size_t s;
        if (!store_level_bytes(store, sw, sh, &s)) {
            pc_cimage_fini(&tmp);
            return cx_encode(err, F, "texture size overflow");
        }
        sizes[nsizes++] = s;
        if (total > SIZE_MAX - s) {
            pc_cimage_fini(&tmp);
            return cx_encode(err, F, "texture size overflow");
        }
        total += s;
        sw = sw / 2 > 0 ? sw / 2 : 1;
        sh = sh / 2 > 0 ? sh / 2 : 1;
    }
    st = dds_header(width, height, store, levels, nsizes > 0 ? sizes[0] : 0, header, err);
    if (st != PC_OK) {
        pc_cimage_fini(&tmp);
        return st;
    }
    buf = calloc(total > 0 ? total : 1, 1);
    if (buf == NULL) {
        pc_cimage_fini(&tmp);
        return cx_nomem(err);
    }
    memcpy(buf, header, DDS_HEADER);
    sw = width;
    sh = height;
    at = DDS_HEADER;
    level = src->data;
    level_owned = false;
    for (i = 0; i < nsizes; i++) {
        st = write_level(store, level, i == 0 ? src->len : sw * sh * channels, sw, sh, buf + at, sizes[i], err);
        if (st != PC_OK) {
            break;
        }
        at += sizes[i];
        if (i + 1 < nsizes) {
            size_t nw = sw / 2 > 0 ? sw / 2 : 1;
            size_t nh = sh / 2 > 0 ? sh / 2 : 1;
            uint8_t *next = NULL;
            st = downsample(level, sw, sh, nw, nh, channels, &next, err);
            if (level_owned) {
                free(level);
            }
            if (st != PC_OK) {
                level = NULL;
                level_owned = false;
                break;
            }
            level = next;
            level_owned = true;
            sw = nw;
            sh = nh;
        }
    }
    if (level_owned) {
        free(level);
    }
    pc_cimage_fini(&tmp);
    if (st != PC_OK) {
        free(buf);
        return st;
    }
    {
        cx_w w;
        w.b = out;
        w.st = PC_OK;
        w.err = err;
        cx_w_put(&w, buf, total);
        free(buf);
        return w.st;
    }
}
