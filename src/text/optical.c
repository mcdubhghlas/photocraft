/* Optical kerning (port of crates/text/src/optical.rs; see optical.h). f32 arithmetic follows
 * the reference expression by expression. */
#include "text/optical.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/rmath.h"

#define Y_LO (-0.25f)
#define Y_HI 1.0f
#define BAND_LO (-0.1f)
#define BAND_HI 1.0f
#define DEPTH 0.04f
#define NEAR 0.1f
#define C0 (-0.0033f)
#define C_MIN 0.5384f
#define C_EXCESS 0.5526f
#define C_NEAR (-0.1612f)
#define LIMIT 400.0f
#define MAX_PROFILES 50000

float pc_optical_sample_y(size_t i)
{
    return Y_LO + (Y_HI - Y_LO) * ((float)i + 0.5f) / (float)PC_OPTICAL_SAMPLES;
}

bool pc_optical_profile_is_empty(const pc_optical_profile *p)
{
    size_t i;
    for (i = 0; i < PC_OPTICAL_SAMPLES; i++) {
        if (p->has_left[i]) {
            return false;
        }
    }
    return true;
}

/* Line segments of a flattened outline (font units). */
typedef struct flatten {
    float (*segs)[4];
    size_t n, cap;
    float sx, sy, ax, ay;
    bool nomem;
} flatten;

static void fl_line(flatten *f, float x, float y)
{
    if (f->nomem) {
        return;
    }
    if (f->n == f->cap) {
        float (*g)[4] = pc_grow_array(f->segs, &f->cap, f->n + 1, sizeof *g);
        if (g == NULL) {
            f->nomem = true;
            return;
        }
        f->segs = g;
    }
    f->segs[f->n][0] = f->ax;
    f->segs[f->n][1] = f->ay;
    f->segs[f->n][2] = x;
    f->segs[f->n][3] = y;
    f->n++;
    f->ax = x;
    f->ay = y;
}

static void fl_move(void *c, float x, float y)
{
    flatten *f = c;
    f->sx = f->ax = x;
    f->sy = f->ay = y;
}

static void fl_lineto(void *c, float x, float y)
{
    fl_line(c, x, y);
}

static void fl_quad(void *c, float cx, float cy, float x, float y)
{
    flatten *f = c;
    float x0 = f->ax, y0 = f->ay;
    int i;
    for (i = 1; i <= 8; i++) {
        float t = (float)i / 8.0f, u = 1.0f - t;
        fl_line(f, u * u * x0 + 2.0f * u * t * cx + t * t * x, u * u * y0 + 2.0f * u * t * cy + t * t * y);
    }
}

static void fl_curve(void *c, float c0x, float c0y, float c1x, float c1y, float x, float y)
{
    flatten *f = c;
    float x0 = f->ax, y0 = f->ay;
    int i;
    for (i = 1; i <= 12; i++) {
        float t = (float)i / 12.0f, u = 1.0f - t;
        float a = u * u * u, b = 3.0f * u * u * t, cc = 3.0f * u * t * t, d = t * t * t;
        fl_line(f, a * x0 + b * c0x + cc * c1x + d * x, a * y0 + b * c0y + cc * c1y + d * y);
    }
}

static void fl_close(void *c)
{
    flatten *f = c;
    if (f->sx != f->ax || f->sy != f->ay) {
        fl_line(f, f->sx, f->sy);
    }
}

pc_status pc_optical_profile_of(pc_font *font, const int16_t *coords, size_t ncoords, uint32_t gid,
                                pc_optical_profile *out, pc_error *err)
{
    float upem, advance = 0.0f, adv;
    flatten fl;
    pc_outline_pen pen;
    pc_status st;
    size_t i, k;
    pc_optical_profile p;
    if (font == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "optical: invalid arguments");
    }
    upem = (float)pc_font_units_per_em(font);
    if (!(upem > 0.0f)) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "optical: font has no units per em");
    }
    if (pc_font_advance(font, gid, coords, ncoords, &adv)) {
        advance = adv;
    }
    advance = advance / upem;
    memset(&fl, 0, sizeof fl);
    pen = (pc_outline_pen){fl_move, fl_lineto, fl_quad, fl_curve, fl_close, &fl};
    st = pc_font_draw(font, gid, NULL, coords, ncoords, &pen, err);
    if (st == PC_OK && fl.nomem) {
        st = pc_fail(err, PC_ERR_NOMEM, "optical: out of memory");
    }
    if (st != PC_OK) {
        free(fl.segs);
        return st == PC_ERR_NOMEM
                   ? st
                   : pc_fail(err, PC_ERR_NOT_FOUND, "optical: no outline for glyph %u", (unsigned)gid);
    }
    memset(&p, 0, sizeof p);
    p.advance = advance;
    for (i = 0; i < PC_OPTICAL_SAMPLES; i++) {
        float y = pc_optical_sample_y(i) * upem;
        float lo = INFINITY, hi = -INFINITY;
        for (k = 0; k < fl.n; k++) {
            float x0 = fl.segs[k][0], y0 = fl.segs[k][1], x1 = fl.segs[k][2], y1 = fl.segs[k][3];
            if ((y0 <= y && y < y1) || (y1 <= y && y < y0)) {
                float x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
                lo = pc_rs_minf(lo, x);
                hi = pc_rs_maxf(hi, x);
            }
        }
        if (isfinite(lo) && isfinite(hi)) {
            p.has_left[i] = true;
            p.left[i] = lo / upem;
            p.has_right[i] = true;
            p.right[i] = advance - hi / upem;
        }
    }
    free(fl.segs);
    *out = p;
    return PC_OK;
}

bool pc_optical_pair_features(const pc_optical_profile *l, const pc_optical_profile *r, float out[2])
{
    float gaps[PC_OPTICAL_SAMPLES], mn = INFINITY, excess = -0.0f;
    size_t i, n = 0;
    for (i = 0; i < PC_OPTICAL_SAMPLES; i++) {
        float y = pc_optical_sample_y(i);
        if (y < BAND_LO || y > BAND_HI) {
            continue;
        }
        if (l->has_right[i] && r->has_left[i]) {
            gaps[n++] = l->right[i] + r->left[i];
        }
    }
    if (n == 0) {
        return false;
    }
    for (i = 0; i < n; i++) {
        mn = pc_rs_minf(mn, gaps[i]);
    }
    /* Iterator::sum of f32 starts at -0.0. */
    for (i = 0; i < n; i++) {
        excess += pc_rs_minf_k(gaps[i] - mn, DEPTH);
    }
    out[0] = mn;
    out[1] = excess / (float)n;
    return true;
}

float pc_optical_size_adjust(float size_pt)
{
    float s;
    if (!isfinite(size_pt) || size_pt <= 0.0f) {
        return 0.0f;
    }
    s = size_pt < 15.0f ? 15.0f : size_pt > 95.0f ? 95.0f : size_pt;
    return 11.0f * logf(95.0f / s);
}

float pc_optical_kern_from_features(const float f[2], const float r[2])
{
    float k = C0 + C_MIN * (r[0] - f[0]) + C_EXCESS * (r[1] - f[1]) +
              C_NEAR * (pc_rs_minf_k(r[0], NEAR) - pc_rs_minf_k(f[0], NEAR));
    k = k * 1000.0f;
    if (!isfinite(k)) {
        return 0.0f;
    }
    return k < -LIMIT ? -LIMIT : k > LIMIT ? LIMIT : k;
}

/* ------------------------------------------------------------------------------------------ */
/* Cache: open addressing on 64-bit keys. */

typedef struct pentry {
    uint64_t fk;
    uint32_t gid;
    bool used, ok;
    pc_optical_profile p;
} pentry;

typedef struct rentry {
    uint64_t fk;
    bool used, ok;
    float f[2];
} rentry;

struct pc_optical_cache {
    pentry *prof;
    size_t pcap, pn;
    rentry *ref;
    size_t rcap, rn;
};

pc_status pc_optical_cache_new(pc_optical_cache **out, pc_error *err)
{
    pc_optical_cache *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "optical: out of memory");
    }
    *out = c;
    return PC_OK;
}

void pc_optical_cache_free(pc_optical_cache *c)
{
    if (c != NULL) {
        free(c->prof);
        free(c->ref);
        free(c);
    }
}

static uint64_t mix(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

/* (font id, face index, coordinate bits) -> key, like the reference's font_key hash. */
static uint64_t font_key(pc_font *font, const int16_t *coords, size_t n)
{
    uint64_t h = mix(pc_font_id(font) ^ ((uint64_t)pc_font_index(font) << 48));
    size_t i;
    for (i = 0; i < n; i++) {
        h = mix(h ^ (uint64_t)(uint16_t)coords[i] ^ ((uint64_t)i << 20));
    }
    return h;
}

static bool grow_prof(pc_optical_cache *c)
{
    size_t ncap = c->pcap == 0 ? 256 : c->pcap * 2, i;
    pentry *n = pc_calloc_array(ncap, sizeof *n);
    if (n == NULL) {
        return false;
    }
    for (i = 0; i < c->pcap; i++) {
        if (c->prof[i].used) {
            size_t j = (size_t)mix(c->prof[i].fk ^ c->prof[i].gid) & (ncap - 1);
            while (n[j].used) {
                j = (j + 1) & (ncap - 1);
            }
            n[j] = c->prof[i];
        }
    }
    free(c->prof);
    c->prof = n;
    c->pcap = ncap;
    return true;
}

/* The cached profile result (computing it on a miss); NULL when out of memory. */
static const pentry *get_profile(pc_optical_cache *c, uint64_t fk, pc_font *font, const int16_t *coords,
                                 size_t n, uint32_t gid)
{
    size_t j;
    pc_error e;
    pc_status st;
    if (c->pn > MAX_PROFILES) {
        memset(c->prof, 0, c->pcap * sizeof *c->prof);
        c->pn = 0;
    }
    if ((c->pn + 1) * 2 > c->pcap && !grow_prof(c)) {
        return NULL;
    }
    j = (size_t)mix(fk ^ gid) & (c->pcap - 1);
    while (c->prof[j].used) {
        if (c->prof[j].fk == fk && c->prof[j].gid == gid) {
            return &c->prof[j];
        }
        j = (j + 1) & (c->pcap - 1);
    }
    st = pc_optical_profile_of(font, coords, n, gid, &c->prof[j].p, &e);
    if (st == PC_ERR_NOMEM) {
        return NULL;
    }
    c->prof[j].fk = fk;
    c->prof[j].gid = gid;
    c->prof[j].used = true;
    c->prof[j].ok = st == PC_OK;
    c->pn++;
    return &c->prof[j];
}

static bool get_reference(pc_optical_cache *c, uint64_t fk, pc_font *font, const int16_t *coords, size_t n,
                          float out[2])
{
    static const uint32_t CHARS[4] = {'o', 'O', 'n', 'H'};
    size_t j, i;
    rentry r = {fk, true, false, {0.0f, 0.0f}};
    pc_sfnt sf;
    pc_sfnt_cmap cm;
    if (c->rcap > 0) {
        j = (size_t)mix(fk) & (c->rcap - 1);
        while (c->ref[j].used) {
            if (c->ref[j].fk == fk) {
                out[0] = c->ref[j].f[0];
                out[1] = c->ref[j].f[1];
                return c->ref[j].ok;
            }
            j = (j + 1) & (c->rcap - 1);
        }
    }
    pc_font_sfnt(font, &sf);
    pc_sfnt_cmap_skrifa(&sf, &cm);
    for (i = 0; i < 4 && !r.ok; i++) {
        uint32_t g = pc_sfnt_cmap_map(&cm, CHARS[i]);
        const pentry *p;
        pc_optical_profile prof;
        if (g == 0) {
            continue;
        }
        p = get_profile(c, fk, font, coords, n, g);
        if (p == NULL) {
            return false; /* out of memory: no answer, nothing cached */
        }
        if (!p->ok) {
            continue;
        }
        prof = p->p;
        r.ok = pc_optical_pair_features(&prof, &prof, r.f);
    }
    if ((c->rn + 1) * 2 > c->rcap) {
        size_t ncap = c->rcap == 0 ? 16 : c->rcap * 2, k;
        rentry *nt = pc_calloc_array(ncap, sizeof *nt);
        if (nt == NULL) {
            return false;
        }
        for (k = 0; k < c->rcap; k++) {
            if (c->ref[k].used) {
                size_t m = (size_t)mix(c->ref[k].fk) & (ncap - 1);
                while (nt[m].used) {
                    m = (m + 1) & (ncap - 1);
                }
                nt[m] = c->ref[k];
            }
        }
        free(c->ref);
        c->ref = nt;
        c->rcap = ncap;
    }
    j = (size_t)mix(fk) & (c->rcap - 1);
    while (c->ref[j].used) {
        j = (j + 1) & (c->rcap - 1);
    }
    c->ref[j] = r;
    c->rn++;
    out[0] = r.f[0];
    out[1] = r.f[1];
    return r.ok;
}

bool pc_optical_cache_pair(pc_optical_cache *c, pc_font *font, const int16_t *coords, size_t ncoords,
                           uint32_t left, uint32_t right, float *out)
{
    uint64_t fk;
    const pentry *pl, *pr;
    pc_optical_profile l, r;
    float f[2], ref[2];
    if (c == NULL || font == NULL || out == NULL || (ncoords > 0 && coords == NULL)) {
        return false;
    }
    fk = font_key(font, coords, ncoords);
    pl = get_profile(c, fk, font, coords, ncoords, left);
    if (pl == NULL || !pl->ok) {
        return false;
    }
    l = pl->p; /* copy: the next lookup may move the table */
    pr = get_profile(c, fk, font, coords, ncoords, right);
    if (pr == NULL || !pr->ok) {
        return false;
    }
    r = pr->p;
    if (!pc_optical_pair_features(&l, &r, f)) {
        return false;
    }
    if (!get_reference(c, fk, font, coords, ncoords, ref)) {
        return false;
    }
    *out = pc_optical_kern_from_features(f, ref);
    return true;
}
