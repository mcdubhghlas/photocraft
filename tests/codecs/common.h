/* Shared helpers for the codecs tests (port of crates/codecs/tests/common/mod.rs):
 * deterministic PRNG, synthetic image generators, comparison metrics, sample metadata. */
#ifndef PC_TESTS_CODECS_COMMON_H
#define PC_TESTS_CODECS_COMMON_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/half.h"
#include "codecs/codecs.h"
#include "harness.h"

/* SplitMix64. */
typedef struct rng {
    uint64_t s;
} rng;

static inline uint64_t rng_next(rng *r)
{
    uint64_t z;
    r->s += 0x9E3779B97F4A7C15ull;
    z = r->s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Uniform in [0, 1). */
static inline float rng_f32(rng *r) { return (float)(rng_next(r) >> 40) / (float)(1u << 24); }

static inline uint8_t *rng_bytes(rng *r, size_t n)
{
    uint8_t *v = malloc(n > 0 ? n : 1);
    size_t i;
    for (i = 0; i < n; i++) {
        v[i] = (uint8_t)rng_next(r);
    }
    return v;
}

static inline float clamp01f(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

/* Gradient + noise in [0, 1]; alpha is a separate gradient with transparent and opaque areas. */
static inline pc_cimage synth(uint32_t w, uint32_t h, pc_clayout l, pc_csample s, uint64_t seed, float noise)
{
    rng r = {seed};
    size_t nc = pc_clayout_channels(l);
    size_t n = (size_t)w * h * nc;
    float *v = malloc((n > 0 ? n : 1) * sizeof *v);
    size_t k = 0;
    uint32_t x;
    uint32_t y;
    pc_cimage img;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float fx = (float)x / (float)((w > 2 ? w : 2) - 1);
            float fy = (float)y / (float)((h > 2 ? h : 2) - 1);
            size_t c;
            for (c = 0; c < nc; c++) {
                bool is_alpha = pc_clayout_has_alpha(l) && c == nc - 1;
                float base;
                float nz;
                if (is_alpha) {
                    base = clamp01f((fx + fy) * 0.75f - 0.25f);
                } else {
                    switch (c % 4) {
                    case 0:
                        base = fx;
                        break;
                    case 1:
                        base = fy;
                        break;
                    case 2:
                        base = 1.0f - fx * 0.5f - fy * 0.5f;
                        break;
                    default:
                        base = sqrtf(fx * fy);
                    }
                }
                nz = (rng_f32(&r) - 0.5f) * noise;
                v[k++] = clamp01f(base + nz);
            }
        }
    }
    if (pc_cimage_from_normalized(w, h, l, s, v, n, &img, NULL) != PC_OK) {
        pc_cimage_init(&img);
    }
    free(v);
    return img;
}

static inline pc_cimage test_image(pc_clayout l, pc_csample s) { return synth(37, 23, l, s, 0xC0DEC, 0.2f); }

/* PSNR in dB over normalised samples clamped to [0, 1]. */
static inline double psnr(const pc_cimage *a, const pc_cimage *b)
{
    float *av = NULL;
    float *bv = NULL;
    size_t na = 0;
    size_t nb = 0;
    size_t i;
    double mse = 0.0;
    if (a->width != b->width || a->height != b->height || a->layout != b->layout) {
        PC_T_FAIL("psnr of different shapes");
        return 0.0;
    }
    (void)pc_cimage_to_normalized(a, &av, &na, NULL);
    (void)pc_cimage_to_normalized(b, &bv, &nb, NULL);
    for (i = 0; i < na && i < nb; i++) {
        double d = (double)(clamp01f(av[i]) - clamp01f(bv[i]));
        mse += d * d;
    }
    mse /= (double)(na > 0 ? na : 1);
    free(av);
    free(bv);
    return mse == 0.0 ? INFINITY : 10.0 * log10(1.0 / mse);
}

static inline float max_abs_diff(const pc_cimage *a, const pc_cimage *b)
{
    float *av = NULL;
    float *bv = NULL;
    size_t na = 0;
    size_t nb = 0;
    size_t i;
    float m = 0.0f;
    (void)pc_cimage_to_normalized(a, &av, &na, NULL);
    (void)pc_cimage_to_normalized(b, &bv, &nb, NULL);
    for (i = 0; i < na && i < nb; i++) {
        float d = fabsf(av[i] - bv[i]);
        m = d > m ? d : m;
    }
    free(av);
    free(bv);
    return m;
}

static inline uint16_t ne16(const uint8_t *d, size_t i)
{
    uint16_t v;
    memcpy(&v, d + 2 * i, 2);
    return v;
}

static inline float nef32(const uint8_t *d, size_t i)
{
    float v;
    memcpy(&v, d + 4 * i, 4);
    return v;
}

static inline bool writable(pc_cformat f) { return pc_cformat_caps(f).write; }
static inline bool rw(pc_cformat f) { return pc_cformat_caps(f).write && pc_cformat_caps(f).read; }

/* One IFD with an Orientation entry (little-endian TIFF-structured EXIF). */
static inline pc_buf sample_exif(void)
{
    static const uint8_t v[26] = {'I', 'I', '*', 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    pc_buf b = PC_BUF_INIT;
    (void)pc_buf_append(&b, v, sizeof v, NULL);
    return b;
}

/* A fake ICC profile of len bytes (big-endian length, "acsp", deterministic noise). */
static inline pc_buf sample_icc(size_t len)
{
    rng r = {len};
    pc_buf b = PC_BUF_INIT;
    uint8_t *v = rng_bytes(&r, len);
    if (len >= 4) {
        v[0] = (uint8_t)(len >> 24);
        v[1] = (uint8_t)(len >> 16);
        v[2] = (uint8_t)(len >> 8);
        v[3] = (uint8_t)len;
    }
    if (len >= 40) {
        memcpy(v + 36, "acsp", 4);
    }
    (void)pc_buf_append(&b, v, len, NULL);
    free(v);
    return b;
}

static const char SAMPLE_XMP[] =
    "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
    "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"/></x:xmpmeta><?xpacket end=\"w\"?>";

static inline void set_blob(pc_meta_blob *b, const void *d, size_t n) { (void)pc_meta_blob_set(b, d, n, NULL); }

/* Encode with default options (empty buffer on failure, which a CHECK reports). */
static inline pc_buf enc(const pc_cimage *img, pc_cformat f, const pc_cencode_opts *o)
{
    pc_cencode_opts d = pc_cencode_opts_default();
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    if (pc_cencode(img, f, o != NULL ? o : &d, &b, &e) != PC_OK) {
        PC_T_FAIL("encode %s %s %s: %s", pc_cformat_debug(f), pc_clayout_name(img->layout),
                  pc_csample_name(img->sample), e.msg);
    }
    return b;
}

static inline pc_cimage dec(const uint8_t *b, size_t n)
{
    pc_cimage img;
    pc_error e;
    if (pc_cdecode(b, n, &img, &e) != PC_OK) {
        PC_T_FAIL("decode: %s", e.msg);
        pc_cimage_init(&img);
    }
    return img;
}

static inline pc_cimage dec_with(const uint8_t *b, size_t n, const pc_cdecode_opts *o)
{
    pc_cimage img;
    pc_error e;
    if (pc_cdecode_with(b, n, o, &img, &e) != PC_OK) {
        PC_T_FAIL("decode: %s", e.msg);
        pc_cimage_init(&img);
    }
    return img;
}

static inline bool has_warning(const pc_cimage *img, pc_cwarning_kind k)
{
    size_t i;
    for (i = 0; i < img->nwarnings; i++) {
        if (img->warnings[i].kind == k) {
            return true;
        }
    }
    return false;
}

static inline bool find_bytes(const uint8_t *hay, size_t n, const void *needle, size_t k)
{
    size_t i;
    for (i = 0; i + k <= n; i++) {
        if (memcmp(hay + i, needle, k) == 0) {
            return true;
        }
    }
    return false;
}

static inline size_t count_bytes(const uint8_t *hay, size_t n, const void *needle, size_t k)
{
    size_t i;
    size_t c = 0;
    for (i = 0; i + k <= n; i++) {
        c += memcmp(hay + i, needle, k) == 0;
    }
    return c;
}

#endif
