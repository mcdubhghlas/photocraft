/* Font metrics for the layouter (see fmetrics.h). */
#include "text/fmetrics.h"

#include <hb-ot.h>
#include <hb.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "base/rmath.h"
#include "text/sfnt.h"

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

static int16_t bes16(const uint8_t *p)
{
    return (int16_t)pc_u32_as_i32((uint32_t)be16(p) | ((p[0] & 0x80u) != 0 ? 0xFFFF0000u : 0u));
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* A table whose fixed part (min bytes) is present; read-fonts fails to parse a shorter one. */
static bool table(const pc_sfnt *s, uint32_t tag, size_t min, const uint8_t **p, size_t *n)
{
    return pc_sfnt_table(s, tag, p, n) && *n >= min;
}

static bool all_zero(const int16_t *c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (c[i] != 0) {
            return false;
        }
    }
    return true;
}

/* OS/2 length read-fonts requires for its version. */
static size_t os2_min_len(uint16_t version)
{
    return version >= 5 ? 100 : version >= 2 ? 96 : version >= 1 ? 86 : 78;
}

/* MVAR deltas through HarfBuzz (font units). */
typedef struct mvar {
    hb_blob_t *blob;
    hb_face_t *face;
    hb_font_t *font;
} mvar;

static bool mvar_open(pc_font *f, const int16_t *coords, size_t n, mvar *m)
{
    size_t len, i;
    const uint8_t *data = pc_font_data(f, &len);
    int buf[64];
    memset(m, 0, sizeof *m);
    if (n > 64 || len > (size_t)0x7FFFFFFF) {
        return false;
    }
    for (i = 0; i < n; i++) {
        buf[i] = coords[i];
    }
    m->blob = hb_blob_create((const char *)data, (unsigned)len, HB_MEMORY_MODE_READONLY, NULL, NULL);
    m->face = hb_face_create(m->blob, pc_font_index(f));
    m->font = hb_font_create(m->face);
    hb_font_set_var_coords_normalized(m->font, buf, (unsigned)n);
    return true;
}

static void mvar_close(mvar *m)
{
    hb_font_destroy(m->font);
    hb_face_destroy(m->face);
    hb_blob_destroy(m->blob);
}

static float mvar_delta(mvar *m, hb_ot_metrics_tag_t tag, float scale)
{
    return hb_ot_metrics_get_variation(m->font, tag) * scale;
}

void pl_font_metrics(pc_font *f, const float *size, const int16_t *coords, size_t ncoords, pl_metrics *out)
{
    pc_sfnt s;
    const uint8_t *p;
    size_t n;
    float scale;
    bool used_typo = false, has_os2 = false;
    const uint8_t *os2 = NULL;
    pl_metrics m;
    memset(&m, 0, sizeof m);
    pc_font_sfnt(f, &s);
    if (table(&s, PC_SFNT_TAG('h', 'e', 'a', 'd'), 54, &p, &n)) {
        m.units_per_em = be16(p + 18);
    }
    scale = size != NULL && m.units_per_em != 0 ? *size / (float)m.units_per_em : 1.0f;
    if (table(&s, PC_SFNT_TAG('p', 'o', 's', 't'), 32, &p, &n)) {
        m.has_underline = true;
        m.underline_offset = (float)bes16(p + 8) * scale;
        m.underline_thickness = (float)bes16(p + 10) * scale;
    }
    if (pc_sfnt_table(&s, PC_SFNT_TAG('O', 'S', '/', '2'), &p, &n) && n >= 2 && n >= os2_min_len(be16(p))) {
        uint16_t version = be16(p);
        os2 = p;
        has_os2 = true;
        if ((be16(p + 62) & 0x80u) != 0) { /* USE_TYPO_METRICS */
            m.ascent = (float)bes16(p + 68) * scale;
            m.descent = (float)bes16(p + 70) * scale;
            m.leading = (float)bes16(p + 72) * scale;
            used_typo = true;
        }
        if (version >= 2) {
            m.has_cap_height = true;
            m.cap_height = (float)bes16(p + 88) * scale;
        }
        m.has_strikeout = true;
        m.strikeout_offset = (float)bes16(p + 28) * scale;
        m.strikeout_thickness = (float)bes16(p + 26) * scale;
    }
    if (!used_typo) {
        if (table(&s, PC_SFNT_TAG('h', 'h', 'e', 'a'), 36, &p, &n)) {
            m.ascent = (float)bes16(p + 4) * scale;
            m.descent = (float)bes16(p + 6) * scale;
            m.leading = (float)bes16(p + 8) * scale;
        }
        if (m.ascent == 0.0f && m.descent == 0.0f && has_os2) {
            if (bes16(os2 + 68) != 0 || bes16(os2 + 70) != 0) {
                m.ascent = (float)bes16(os2 + 68) * scale;
                m.descent = (float)bes16(os2 + 70) * scale;
                m.leading = (float)bes16(os2 + 72) * scale;
            } else {
                m.ascent = (float)be16(os2 + 74) * scale;
                m.descent = -((float)be16(os2 + 76) * scale);
            }
        }
    }
    if (ncoords > 0 && !all_zero(coords, ncoords) && pc_sfnt_has_table(&s, PC_SFNT_TAG('M', 'V', 'A', 'R'))) {
        mvar mv;
        if (mvar_open(f, coords, ncoords, &mv)) {
            m.ascent += mvar_delta(&mv, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, scale);
            m.descent += mvar_delta(&mv, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, scale);
            m.leading += mvar_delta(&mv, HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP, scale);
            if (m.has_cap_height) {
                m.cap_height += mvar_delta(&mv, HB_OT_METRICS_TAG_CAP_HEIGHT, scale);
            }
            if (m.has_underline) {
                m.underline_offset += mvar_delta(&mv, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, scale);
                m.underline_thickness += mvar_delta(&mv, HB_OT_METRICS_TAG_UNDERLINE_SIZE, scale);
            }
            if (m.has_strikeout) {
                m.strikeout_offset += mvar_delta(&mv, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, scale);
                m.strikeout_thickness += mvar_delta(&mv, HB_OT_METRICS_TAG_STRIKEOUT_SIZE, scale);
            }
            mvar_close(&mv);
        }
    }
    *out = m;
}

/* ---- glyph bounds ---- */

/* font-types Fixed division and mul_div (FreeType's FT_DivFix / FT_MulDiv). */
static int32_t fixed_div(int32_t a, int32_t b)
{
    bool neg = (a < 0) != (b < 0);
    uint64_t au = a < 0 ? (uint64_t)(-(int64_t)a) : (uint64_t)a;
    uint64_t bu = b < 0 ? (uint64_t)(-(int64_t)b) : (uint64_t)b;
    uint32_t q = bu == 0 ? 0x7FFFFFFFu : (uint32_t)(((au << 16) + (bu >> 1)) / bu);
    return neg ? pc_u32_as_i32(0u - q) : pc_u32_as_i32(q);
}

static int32_t fixed_mul_div(int32_t s, int32_t a, int32_t b)
{
    int sign = 1;
    uint64_t su = (uint64_t)(int64_t)s, au = (uint64_t)(int64_t)a, bu = (uint64_t)(int64_t)b, r;
    if (s < 0) {
        su = 0u - su;
        sign = -1;
    }
    if (a < 0) {
        au = 0u - au;
        sign = -sign;
    }
    if (b < 0) {
        bu = 0u - bu;
        sign = -sign;
    }
    r = bu > 0 ? (su * au + (bu >> 1)) / bu : 0x7FFFFFFFu;
    return sign < 0 ? pc_u32_as_i32(0u - (uint32_t)r) : pc_u32_as_i32((uint32_t)r);
}

typedef struct bounds_pen {
    bool has;
    float ymax;
} bounds_pen;

static void bp_add(bounds_pen *b, float y)
{
    if (b->has) {
        b->ymax = pc_rs_maxf(b->ymax, y);
    } else {
        b->has = true;
        b->ymax = y;
    }
}

static void bp_move(void *c, float x, float y)
{
    (void)x;
    bp_add(c, y);
}
static void bp_quad(void *c, float cx, float cy, float x, float y)
{
    (void)cx;
    (void)x;
    bp_add(c, cy);
    bp_add(c, y);
}
static void bp_curve(void *c, float ax, float ay, float bx, float by, float x, float y)
{
    (void)ax;
    (void)bx;
    (void)x;
    bp_add(c, ay);
    bp_add(c, by);
    bp_add(c, y);
}
static void bp_close(void *c)
{
    (void)c;
}

static bool outline_ymax(pc_font *f, const float *size, const int16_t *coords, size_t ncoords, uint32_t gid,
                         float *out)
{
    bounds_pen b = {false, 0.0f};
    pc_outline_pen pen = {bp_move, bp_move, bp_quad, bp_curve, bp_close, &b};
    if (pc_font_draw(f, gid, size, coords, ncoords, &pen, NULL) != PC_OK || !b.has) {
        return false;
    }
    *out = b.ymax;
    return true;
}

bool pl_glyph_bounds_ymax(pc_font *f, float size, const int16_t *coords, size_t ncoords, uint32_t gid,
                          float *ymax)
{
    pc_sfnt s;
    const uint8_t *head, *loca, *glyf;
    size_t nh, nl, ng, a, b;
    uint16_t upem;
    int32_t scale;
    pc_font_sfnt(f, &s);
    if (pc_sfnt_has_table(&s, PC_SFNT_TAG('g', 'v', 'a', 'r')) ||
        pc_sfnt_has_table(&s, PC_SFNT_TAG('C', 'F', 'F', ' ')) ||
        pc_sfnt_has_table(&s, PC_SFNT_TAG('C', 'F', 'F', '2'))) {
        if (!ncoords || all_zero(coords, ncoords)) {
            ncoords = 0;
        }
        return outline_ymax(f, &size, coords, ncoords, gid, ymax);
    }
    if (!table(&s, PC_SFNT_TAG('h', 'e', 'a', 'd'), 54, &head, &nh) ||
        !pc_sfnt_table(&s, PC_SFNT_TAG('l', 'o', 'c', 'a'), &loca, &nl) ||
        !pc_sfnt_table(&s, PC_SFNT_TAG('g', 'l', 'y', 'f'), &glyf, &ng)) {
        return false;
    }
    if (gid >= pc_sfnt_num_glyphs(&s)) {
        return false;
    }
    if (be16(head + 50) == 0) {
        if ((size_t)gid * 2 + 4 > nl) {
            return false;
        }
        a = (size_t)be16(loca + 2 * gid) * 2;
        b = (size_t)be16(loca + 2 * gid + 2) * 2;
    } else {
        if ((size_t)gid * 4 + 8 > nl) {
            return false;
        }
        a = be32(loca + 4 * gid);
        b = be32(loca + 4 * gid + 4);
    }
    upem = be16(head + 18);
    /* skrifa's fixed_linear_scale: Fixed::from_bits((ppem * 64.) as i32) / Fixed::from_bits(upem). */
    scale = upem > 0 ? fixed_div(pc_f32_to_i32(size * 64.0f), (int32_t)upem) : 0x10000 * 64;
    if (a == b) {
        *ymax = 0.0f; /* an empty glyph has an empty bounding box */
        return true;
    }
    if (a > b || b > ng || b - a < 10) {
        return false;
    }
    *ymax = (float)fixed_mul_div(scale, bes16(glyf + a + 8), 64) * (1.0f / 65536.0f);
    return true;
}

float pl_glyph_advance_px(pc_font *f, float size, const int16_t *coords, size_t ncoords, uint32_t gid)
{
    uint16_t upem = pc_font_units_per_em(f);
    float units;
    int32_t scale;
    if (!pc_font_advance(f, gid, coords, ncoords, &units)) {
        return 0.0f;
    }
    scale = upem > 0 ? fixed_div(pc_f32_to_i32(size * 64.0f), (int32_t)upem) : 0x10000 * 64;
    return (float)fixed_mul_div(scale, pc_f32_to_i32(units), 64) * (1.0f / 65536.0f);
}

uint32_t pl_skrifa_cmap(pc_font *f, uint32_t cp)
{
    pc_sfnt s;
    pc_sfnt_cmap c;
    pc_font_sfnt(f, &s);
    pc_sfnt_cmap_skrifa(&s, &c);
    return c.sub != NULL ? pc_sfnt_cmap_map(&c, cp) : 0;
}

bool pl_vertical_origin_units(pc_font *f, const int16_t *coords, size_t ncoords, uint32_t gid, float *out)
{
    pc_sfnt s;
    const uint8_t *p, *vm;
    size_t n, nvm, nlong, idx;
    int16_t tsb;
    float top;
    pc_font_sfnt(f, &s);
    if (table(&s, PC_SFNT_TAG('V', 'O', 'R', 'G'), 8, &p, &n)) {
        size_t count = be16(p + 6), lo = 0, hi;
        int16_t v = bes16(p + 4);
        if (8 + count * 4 > n) {
            count = 0; /* a cut-off record array reads as empty */
        }
        hi = count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            uint16_t g = be16(p + 8 + 4 * mid);
            if (g == gid) {
                v = bes16(p + 8 + 4 * mid + 2);
                break;
            }
            if (g < gid) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        *out = (float)v;
        return true;
    }
    if (!table(&s, PC_SFNT_TAG('v', 'h', 'e', 'a'), 36, &p, &n) ||
        !pc_sfnt_table(&s, PC_SFNT_TAG('v', 'm', 't', 'x'), &vm, &nvm)) {
        return false;
    }
    nlong = be16(p + 34);
    if (nlong * 4 > nvm) {
        return false;
    }
    if (gid < nlong) {
        tsb = bes16(vm + 4 * gid + 2);
    } else {
        size_t nglyphs = pc_sfnt_num_glyphs(&s);
        size_t ntsb = nglyphs > nlong ? nglyphs - nlong : 0;
        idx = gid - nlong;
        if (idx >= ntsb || nlong * 4 + ntsb * 2 > nvm) {
            return false;
        }
        tsb = bes16(vm + 4 * nlong + 2 * idx);
    }
    if (!ncoords || all_zero(coords, ncoords)) {
        ncoords = 0;
    }
    if (!outline_ymax(f, NULL, coords, ncoords, gid, &top)) {
        return false;
    }
    *out = top + (float)tsb;
    return true;
}
