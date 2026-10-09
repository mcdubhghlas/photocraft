#include "convert.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "base/rmath.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "cms/transform.h"

struct pc_cmyk_space {
    atomic_size_t refs;
    pc_transform *to_srgb;
    pc_transform *from_srgb;
    uint64_t id;
};

pc_cmyk_space *pc_cmyk_space_retain(pc_cmyk_space *s)
{
    if (s != NULL) {
        atomic_fetch_add(&s->refs, 1);
    }
    return s;
}

void pc_cmyk_space_release(pc_cmyk_space *s)
{
    if (s != NULL && atomic_fetch_sub(&s->refs, 1) == 1) {
        pc_transform_release(s->to_srgb);
        pc_transform_release(s->from_srgb);
        free(s);
    }
}

uint64_t pc_cmyk_space_id(const pc_cmyk_space *s)
{
    return s != NULL ? s->id : 0;
}

static pc_cmyk_space *build(const uint8_t *icc, size_t len)
{
    pc_profile p;
    uint64_t h = 0;
    uint64_t coated = 0;
    pc_cmyk_space *s = NULL;
    const pc_profile *srgb = pc_builtin_profile(PC_BUILTIN_SRGB);
    const pc_profile *cc = pc_builtin_profile(PC_BUILTIN_COATED_CMYK);
    if (srgb == NULL || cc == NULL || pc_profile_parse(icc, len, &p, NULL) != PC_OK) {
        return NULL;
    }
    if (p.space != PC_CS_CMYK || pc_profile_content_hash(&p, &h, NULL) != PC_OK ||
        pc_profile_content_hash(cc, &coated, NULL) != PC_OK || h == coated) {
        goto done;
    }
    s = calloc(1, sizeof *s);
    if (s == NULL) {
        goto done;
    }
    atomic_init(&s->refs, 1);
    s->id = h;
    if (pc_transform_new(&p, srgb, PC_INTENT_RELATIVE, true, &s->to_srgb, NULL) != PC_OK ||
        pc_transform_new(srgb, &p, PC_INTENT_RELATIVE, true, &s->from_srgb, NULL) != PC_OK) {
        pc_cmyk_space_release(s);
        s = NULL;
    }
done:
    pc_profile_fini(&p);
    return s;
}

/* The reference caches by allocation (Arc pointer, dropped with the bytes); C has no shared
 * byte owner, so the cache is keyed by content and bounded. Negative results are cached too. */
enum { SPACE_CACHE_MAX = 16 };
typedef struct space_entry {
    uint8_t *bytes;
    size_t len;
    pc_cmyk_space *space; /* NULL: not a usable document CMYK profile */
} space_entry;
static space_entry space_cache[SPACE_CACHE_MAX];
static size_t space_cache_n;
static size_t space_cache_next; /* FIFO eviction slot once full */
static pthread_mutex_t space_lock = PTHREAD_MUTEX_INITIALIZER;

/* Looks `icc` up; on a hit stores a new reference (or NULL) in *out. Caller holds the lock. */
static bool cache_find(const uint8_t *icc, size_t len, pc_cmyk_space **out)
{
    size_t i;
    for (i = 0; i < space_cache_n; i++) {
        if (space_cache[i].len == len && memcmp(space_cache[i].bytes, icc, len) == 0) {
            *out = pc_cmyk_space_retain(space_cache[i].space);
            return true;
        }
    }
    return false;
}

pc_cmyk_space *pc_cmyk_space_for_profile(const uint8_t *icc, size_t len)
{
    pc_cmyk_space *s = NULL;
    pc_cmyk_space *hit = NULL;
    uint8_t *copy;
    space_entry *e;
    if (icc == NULL || len == 0) {
        return NULL;
    }
    (void)pthread_mutex_lock(&space_lock);
    if (cache_find(icc, len, &hit)) {
        (void)pthread_mutex_unlock(&space_lock);
        return hit;
    }
    (void)pthread_mutex_unlock(&space_lock);
    s = build(icc, len); /* outside the lock: building links two transforms */
    copy = malloc(len);
    (void)pthread_mutex_lock(&space_lock);
    if (cache_find(icc, len, &hit)) { /* another thread got there first: share its object */
        (void)pthread_mutex_unlock(&space_lock);
        free(copy);
        pc_cmyk_space_release(s);
        return hit;
    }
    if (copy != NULL) {
        memcpy(copy, icc, len);
        if (space_cache_n < SPACE_CACHE_MAX) {
            e = &space_cache[space_cache_n++];
        } else {
            e = &space_cache[space_cache_next];
            space_cache_next = (space_cache_next + 1) % SPACE_CACHE_MAX;
            free(e->bytes);
            pc_cmyk_space_release(e->space);
        }
        e->bytes = copy;
        e->len = len;
        e->space = pc_cmyk_space_retain(s);
    }
    (void)pthread_mutex_unlock(&space_lock);
    return s;
}

static _Thread_local pc_cmyk_space *active_space;

pc_cmyk_space *pc_cmyk_space_enter(pc_cmyk_space *space)
{
    pc_cmyk_space *prev = active_space;
    active_space = pc_cmyk_space_retain(space);
    return prev;
}

void pc_cmyk_space_leave(pc_cmyk_space *prev)
{
    pc_cmyk_space_release(active_space);
    active_space = prev;
}

pc_cmyk_space *pc_cmyk_space_active(void)
{
    return active_space;
}

/* The built-in links always succeed (the tests use them); NULL only guards against a cms
 * regression, and then the conversions fall back to the naive formulas. */
static pc_transform *builtin_to_srgb;
static pc_transform *builtin_from_srgb;
static pthread_once_t builtin_once = PTHREAD_ONCE_INIT;

static void build_builtin(void)
{
    const pc_profile *srgb = pc_builtin_profile(PC_BUILTIN_SRGB);
    const pc_profile *cmyk = pc_builtin_profile(PC_BUILTIN_COATED_CMYK);
    if (srgb == NULL || cmyk == NULL) {
        return;
    }
    if (pc_transform_new(cmyk, srgb, PC_INTENT_RELATIVE, true, &builtin_to_srgb, NULL) != PC_OK) {
        builtin_to_srgb = NULL;
    }
    if (pc_transform_new(srgb, cmyk, PC_INTENT_RELATIVE, true, &builtin_from_srgb, NULL) != PC_OK) {
        builtin_from_srgb = NULL;
    }
}

static const pc_transform *builtin(bool to_srgb)
{
    if (pthread_once(&builtin_once, build_builtin) != 0) {
        return NULL;
    }
    return to_srgb ? builtin_to_srgb : builtin_from_srgb;
}

void pc_cmyk_to_rgb(const float cmyk[4], float out[3])
{
    const pc_transform *t = active_space != NULL ? active_space->to_srgb : builtin(true);
    float c[4];
    memcpy(c, cmyk, sizeof c); /* out may alias */
    if (t == NULL) {
        pc_cmyk_to_rgb_naive(c, out);
        return;
    }
    pc_transform_eval_fast(t, c, out);
}

void pc_rgb_to_cmyk(const float rgb[3], float out[4])
{
    const pc_transform *t = active_space != NULL ? active_space->from_srgb : builtin(false);
    float v[3];
    if (t == NULL) {
        memcpy(v, rgb, sizeof v);
        pc_rgb_to_cmyk_naive(v, out);
        return;
    }
    v[0] = pc_clampf(rgb[0], 0.0f, 1.0f);
    v[1] = pc_clampf(rgb[1], 0.0f, 1.0f);
    v[2] = pc_clampf(rgb[2], 0.0f, 1.0f);
    pc_transform_eval_fast(t, v, out);
}

float pc_srgb_to_linear(float v)
{
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

float pc_linear_to_srgb(float v)
{
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

void pc_cmyk_to_rgb_naive(const float c[4], float out[3])
{
    float k = 1.0f - c[3];
    float o[3];
    o[0] = (1.0f - c[0]) * k;
    o[1] = (1.0f - c[1]) * k;
    o[2] = (1.0f - c[2]) * k;
    memcpy(out, o, sizeof o);
}

void pc_rgb_to_cmyk_naive(const float rgb[3], float out[4])
{
    float k = 1.0f - pc_rs_maxf(pc_rs_maxf(rgb[0], rgb[1]), rgb[2]);
    float d;
    float o[4];
    if (k >= 1.0f - 1e-6f) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    d = 1.0f - k;
    o[0] = (1.0f - rgb[0] - k) / d;
    o[1] = (1.0f - rgb[1] - k) / d;
    o[2] = (1.0f - rgb[2] - k) / d;
    o[3] = k;
    memcpy(out, o, sizeof o);
}

const float PC_COLOR_D50[3] = {0.96422f, 1.0f, 0.82521f};
const float PC_SRGB_TO_XYZ_D50[3][3] = {
    {0.436074f, 0.385064f, 0.143080f}, {0.222504f, 0.716878f, 0.060618f}, {0.013932f, 0.097104f, 0.714173f}};
const float PC_XYZ_D50_TO_SRGB[3][3] = {
    {3.133856f, -1.616867f, -0.490615f}, {-0.978768f, 1.916142f, 0.033454f}, {0.071945f, -0.228991f, 1.405243f}};

void pc_color_mat3_mul(const float m[3][3], const float v[3], float out[3])
{
    float o[3];
    int i;
    for (i = 0; i < 3; i++) {
        o[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    }
    memcpy(out, o, sizeof o);
}

/* The reference writes 3.0 * (6.0f32 / 29.0).powi(2) and (6.0f32 / 29.0).powi(3). LLVM folds
 * powi of constants as (float)pow((double)x, n), so the cube is 0x1.22355p-7, one ulp above
 * x * x * x in float. Values printed by `rustref color-vectors` (lab-k, lab-eps). */
#define SIX29 (6.0f / 29.0f)
#define FOUR29 (4.0f / 29.0f)
static const float LAB_K = 0x1.07005p-3f;   /* 3 * (6/29)^2, bits 3e038028 */
static const float LAB_EPS = 0x1.22355p-7f; /* (6/29)^3, bits 3c111aa8 */

static float finv(float t)
{
    return t > SIX29 ? t * t * t : LAB_K * (t - FOUR29);
}

void pc_lab_to_srgb(const float lab[3], float out[3])
{
    float fy = (lab[0] + 16.0f) / 116.0f;
    float fx = fy + lab[1] / 500.0f;
    float fz = fy - lab[2] / 200.0f;
    float xyz[3];
    float lin[3];
    int i;
    xyz[0] = finv(fx) * PC_COLOR_D50[0];
    xyz[1] = finv(fy) * PC_COLOR_D50[1];
    xyz[2] = finv(fz) * PC_COLOR_D50[2];
    pc_color_mat3_mul(PC_XYZ_D50_TO_SRGB, xyz, lin);
    for (i = 0; i < 3; i++) {
        out[i] = pc_linear_to_srgb(pc_clampf(lin[i], 0.0f, 1.0f));
    }
}

static float lab_f(float t)
{
    return t > LAB_EPS ? pc_cbrtf(t) : t / LAB_K + FOUR29;
}

void pc_srgb_to_lab(const float rgb[3], float out[3])
{
    float lin[3];
    float xyz[3];
    float fx;
    float fy;
    float fz;
    lin[0] = pc_srgb_to_linear(rgb[0]);
    lin[1] = pc_srgb_to_linear(rgb[1]);
    lin[2] = pc_srgb_to_linear(rgb[2]);
    pc_color_mat3_mul(PC_SRGB_TO_XYZ_D50, lin, xyz);
    fx = lab_f(xyz[0] / PC_COLOR_D50[0]);
    fy = lab_f(xyz[1] / PC_COLOR_D50[1]);
    fz = lab_f(xyz[2] / PC_COLOR_D50[2]);
    out[0] = 116.0f * fy - 16.0f;
    out[1] = 500.0f * (fx - fy);
    out[2] = 200.0f * (fy - fz);
}

float pc_rgb_to_gray(const float rgb[3])
{
    return 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
}
