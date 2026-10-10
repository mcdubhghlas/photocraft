/* pc_font: FreeType faces drawn the way skrifa draws them (see font.h).
 *
 * FreeType loads the glyph (simple and composite TrueType glyphs, gvar deltas, CFF charstrings);
 * this file turns its points into pen commands with a port of skrifa's FreeType-style
 * contour_to_path, so implied midpoints and contour starts are identical. With
 * FT_LOAD_NO_HINTING the TrueType driver scales with the unrounded 16.16 scale
 * FT_DivFix(ppem * 64, unitsPerEm) and FT_MulFix per point, which is exactly skrifa's
 * Scale26Dot6 (ttdriver.c picks `size->metrics`, not the ppem-rounded hinted metrics).
 * Sizes FreeType refuses (below half a pixel, or beyond its 16-bit ppem) are scaled here from
 * the font-unit outline with the same fixed-point arithmetic. */
#include "text/font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_ADVANCES_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"

struct pc_font {
    atomic_uint refs;
    uint64_t id;
    pc_blob *bytes;
    uint32_t index;
    pc_sfnt sfnt;
    uint16_t upem;
    uint32_t num_glyphs;
    pthread_mutex_t lock; /* guards everything below */
    FT_Library lib;
    FT_Face face;
    bool mm;         /* FreeType reports variation support */
    bool cff;        /* PostScript outlines (CFF/CFF2): see nop_filter */
    int16_t *coords; /* coordinates currently set on the face */
    size_t ncoords;
    int32_t size_bits; /* 26.6 size currently requested; 0 = none */
};

static atomic_uint_fast64_t next_id = 1;

pc_status pc_font_new(pc_blob *bytes, uint32_t index, pc_font **out, pc_error *err)
{
    pc_font *f;
    const uint8_t *data = pc_blob_data(bytes);
    size_t len = pc_blob_len(bytes);
    pc_sfnt s;
    if (bytes == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "font: no data");
    }
    if (!pc_sfnt_open(data, len, index, &s)) {
        return pc_fail(err, PC_ERR_FORMAT, "font: not a font file or no face %u", (unsigned)index);
    }
    if (index > 0xFFFFu) { /* FreeType keeps named-instance numbers in the upper bits */
        return pc_fail(err, PC_ERR_FORMAT, "font: face index %u out of range", (unsigned)index);
    }
    f = calloc(1, sizeof *f);
    if (f == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "font: out of memory");
    }
    if (pthread_mutex_init(&f->lock, NULL) != 0) {
        free(f);
        return pc_fail(err, PC_ERR_NOMEM, "font: cannot create a lock");
    }
    if (FT_Init_FreeType(&f->lib) != 0) {
        pthread_mutex_destroy(&f->lock);
        free(f);
        return pc_fail(err, PC_ERR_NOMEM, "font: cannot initialise FreeType");
    }
    if (FT_New_Memory_Face(f->lib, data, (FT_Long)len, (FT_Long)index, &f->face) != 0) {
        FT_Done_FreeType(f->lib);
        pthread_mutex_destroy(&f->lock);
        free(f);
        return pc_fail(err, PC_ERR_FORMAT, "font: FreeType cannot open face %u", (unsigned)index);
    }
    atomic_init(&f->refs, 1u);
    f->id = (uint64_t)atomic_fetch_add(&next_id, 1);
    f->bytes = pc_blob_retain(bytes);
    f->index = index;
    f->sfnt = s;
    f->upem = pc_sfnt_units_per_em(&s);
    f->num_glyphs = (uint32_t)f->face->num_glyphs;
    f->mm = FT_HAS_MULTIPLE_MASTERS(f->face) != 0;
    f->cff = pc_sfnt_has_table(&s, PC_SFNT_TAG('C', 'F', 'F', ' ')) ||
             pc_sfnt_has_table(&s, PC_SFNT_TAG('C', 'F', 'F', '2'));
    *out = f;
    return PC_OK;
}

pc_font *pc_font_retain(pc_font *f)
{
    if (f != NULL) {
        atomic_fetch_add(&f->refs, 1u);
    }
    return f;
}

void pc_font_release(pc_font *f)
{
    if (f == NULL || atomic_fetch_sub(&f->refs, 1u) != 1u) {
        return;
    }
    FT_Done_Face(f->face);
    FT_Done_FreeType(f->lib);
    pthread_mutex_destroy(&f->lock);
    pc_blob_release(f->bytes);
    free(f->coords);
    free(f);
}

const uint8_t *pc_font_data(const pc_font *f, size_t *len)
{
    if (len != NULL) {
        *len = pc_blob_len(f->bytes);
    }
    return pc_blob_data(f->bytes);
}

pc_blob *pc_font_blob(const pc_font *f)
{
    return f->bytes;
}
uint32_t pc_font_index(const pc_font *f)
{
    return f->index;
}
uint64_t pc_font_id(const pc_font *f)
{
    return f->id;
}
void pc_font_sfnt(const pc_font *f, pc_sfnt *out)
{
    *out = f->sfnt;
}
uint16_t pc_font_units_per_em(const pc_font *f)
{
    return f->upem;
}
uint32_t pc_font_num_glyphs(const pc_font *f)
{
    return f->num_glyphs;
}

/* ------------------------------------------------------------------------------------------ */
/* Fixed-point arithmetic of font-types 0.12 (identical to FreeType's FT_MulFix / FT_DivFix). */

static int32_t fixed_mul(int32_t a, int32_t b)
{
    int64_t ab = (int64_t)a * (int64_t)b;
    int64_t r = (ab + 0x8000 - (ab < 0 ? 1 : 0));
    /* Arithmetic shift of a signed value, written portably; then Rust's `as i32` truncation. */
    uint64_t sh = r < 0 ? ~(~(uint64_t)r >> 16) : (uint64_t)r >> 16;
    return pc_i32_from_bits((uint32_t)sh);
}

static int32_t fixed_div(int32_t a, int32_t b)
{
    bool neg = (a < 0) != (b < 0);
    uint64_t au = a < 0 ? (uint64_t)0 - (uint64_t)(int64_t)a : (uint64_t)a;
    uint64_t bu = b < 0 ? (uint64_t)0 - (uint64_t)(int64_t)b : (uint64_t)b;
    uint32_t q = bu == 0 ? 0x7FFFFFFFu : (uint32_t)(((au << 16) + (bu >> 1)) / bu);
    return neg ? pc_i32_from_bits(0u - q) : pc_i32_from_bits(q);
}

/* ------------------------------------------------------------------------------------------ */
/* skrifa's FreeType-style contour_to_path over 26.6 coordinates (bits). */

enum { TAG_ON, TAG_QUAD, TAG_CUBIC };

typedef struct cpt {
    int32_t x, y;
    int tag;
} cpt;

typedef struct emitter {
    const pc_outline_pen *pen;
    int state; /* 0 empty, 1 quad, 2 cubic, 3 two cubics */
    cpt a, b;
} emitter;

static float f26(int32_t v)
{
    return (float)v * (1.0f / 64.0f);
}

static int32_t mid(int32_t a, int32_t b)
{
    /* a.wrapping_add(b) / 2, truncating toward zero. */
    int32_t s = pc_i32_wrapping_add(a, b);
    return s / 2;
}

static cpt midpoint(cpt a, cpt b)
{
    return (cpt){mid(a.x, b.x), mid(a.y, b.y), b.tag};
}

static bool emit(emitter *e, cpt p)
{
    const pc_outline_pen *pen = e->pen;
    switch (e->state) {
    case 0:
        if (p.tag == TAG_QUAD) {
            e->a = p;
            e->state = 1;
        } else if (p.tag == TAG_CUBIC) {
            e->a = p;
            e->state = 2;
        } else {
            pen->line_to(pen->ctx, f26(p.x), f26(p.y));
        }
        return true;
    case 1:
        if (p.tag == TAG_QUAD) {
            cpt m = midpoint(e->a, p);
            pen->quad_to(pen->ctx, f26(e->a.x), f26(e->a.y), f26(m.x), f26(m.y));
            e->a = p;
        } else if (p.tag == TAG_CUBIC) {
            return false;
        } else {
            pen->quad_to(pen->ctx, f26(e->a.x), f26(e->a.y), f26(p.x), f26(p.y));
            e->state = 0;
        }
        return true;
    case 2:
        if (p.tag != TAG_CUBIC) {
            return false;
        }
        e->b = p;
        e->state = 3;
        return true;
    default:
        if (p.tag == TAG_QUAD) {
            return false;
        }
        if (p.tag == TAG_CUBIC) {
            cpt m = midpoint(e->b, p);
            pen->curve_to(pen->ctx, f26(e->a.x), f26(e->a.y), f26(e->b.x), f26(e->b.y), f26(m.x), f26(m.y));
            e->a = p;
            e->state = 2;
        } else {
            pen->curve_to(pen->ctx, f26(e->a.x), f26(e->a.y), f26(e->b.x), f26(e->b.y), f26(p.x), f26(p.y));
            e->state = 0;
        }
        return true;
    }
}

/* One contour of n points (n >= 1). False on a structural error (the pen may have received
 * part of the contour, as in the reference). */
static bool contour(const cpt *pts, size_t n, const pc_outline_pen *pen)
{
    emitter e = {pen, 0, {0, 0, 0}, {0, 0, 0}};
    cpt first = pts[0], last = pts[n - 1], start;
    size_t i = 0, end = n;
    if (first.tag == TAG_CUBIC) {
        return false;
    }
    if (first.tag == TAG_QUAD) {
        if (last.tag == TAG_ON) {
            start = last;
            end = n - 1; /* the last point is the start; don't emit it again */
        } else {
            start = midpoint(last, first);
        }
    } else {
        start = first;
        i = 1;
    }
    pen->move_to(pen->ctx, f26(start.x), f26(start.y));
    for (; i < end; i++) {
        if (!emit(&e, pts[i])) {
            return false;
        }
    }
    if (e.state != 0) {
        start.tag = TAG_ON;
        if (!emit(&e, start)) {
            return false;
        }
    }
    pen->close(pen->ctx);
    return true;
}

/* ------------------------------------------------------------------------------------------ */
/* CFF outlines pass through read-fonts' NopFilterSink in the reference: a line to the current
 * point is dropped, a contour's last line back to its start is dropped at close, and a contour
 * with nothing but degenerate commands disappears (no move, no close). */

typedef struct nop_filter {
    const pc_outline_pen *inner;
    int pending; /* 0 none, 1 move, 2 line, 3 curve */
    float p[6];
    bool has_start, open;
    float sx, sy;
} nop_filter;

static void nf_target(const nop_filter *f, float *x, float *y)
{
    if (f->pending == 3) {
        *x = f->p[4];
        *y = f->p[5];
    } else {
        *x = f->p[0];
        *y = f->p[1];
    }
}

static void nf_flush(nop_filter *f, bool for_close)
{
    const pc_outline_pen *in = f->inner;
    int k = f->pending;
    f->pending = 0;
    if (k == 1) {
        if (!for_close) {
            f->open = true;
            in->move_to(in->ctx, f->p[0], f->p[1]);
            f->has_start = true;
            f->sx = f->p[0];
            f->sy = f->p[1];
        }
    } else if (k == 2) {
        if (!for_close || !f->has_start || f->sx != f->p[0] || f->sy != f->p[1]) {
            in->line_to(in->ctx, f->p[0], f->p[1]);
        }
    } else if (k == 3) {
        in->curve_to(in->ctx, f->p[0], f->p[1], f->p[2], f->p[3], f->p[4], f->p[5]);
    }
}

static void nf_move(void *c, float x, float y)
{
    nop_filter *f = c;
    f->pending = 1;
    f->p[0] = x;
    f->p[1] = y;
}

static void nf_line(void *c, float x, float y)
{
    nop_filter *f = c;
    if (f->pending != 0) {
        float tx, ty;
        nf_target(f, &tx, &ty);
        if (tx == x && ty == y) {
            return;
        }
    }
    nf_flush(f, false);
    f->pending = 2;
    f->p[0] = x;
    f->p[1] = y;
}

static void nf_quad(void *c, float cx, float cy, float x, float y)
{
    /* CFF has no quadratic segments; forward unchanged after the pending element. */
    nop_filter *f = c;
    nf_flush(f, false);
    f->inner->quad_to(f->inner->ctx, cx, cy, x, y);
}

static void nf_curve(void *c, float a, float b, float d, float e, float x, float y)
{
    nop_filter *f = c;
    nf_flush(f, false);
    f->pending = 3;
    f->p[0] = a;
    f->p[1] = b;
    f->p[2] = d;
    f->p[3] = e;
    f->p[4] = x;
    f->p[5] = y;
}

static void nf_close(void *c)
{
    nop_filter *f = c;
    nf_flush(f, true);
    if (f->open) {
        f->inner->close(f->inner->ctx);
        f->open = false;
    }
}

/* ------------------------------------------------------------------------------------------ */

static int tag_of(unsigned char t)
{
    switch (FT_CURVE_TAG(t)) {
    case FT_CURVE_TAG_ON:
        return TAG_ON;
    case FT_CURVE_TAG_CUBIC:
        return TAG_CUBIC;
    default:
        return TAG_QUAD;
    }
}

static bool set_coords(pc_font *f, const int16_t *coords, size_t ncoords)
{
    FT_Fixed buf[64];
    size_t i, n = ncoords;
    if (!f->mm) {
        return true; /* static font: coordinates have no effect (as in skrifa) */
    }
    if (n == f->ncoords && (n == 0 || memcmp(coords, f->coords, n * sizeof *coords) == 0)) {
        return true;
    }
    if (n > sizeof buf / sizeof buf[0]) {
        n = sizeof buf / sizeof buf[0];
    }
    for (i = 0; i < n; i++) {
        buf[i] = (FT_Fixed)coords[i] * 4; /* F2Dot14 -> 16.16 */
    }
    if (FT_Set_Var_Blend_Coordinates(f->face, (FT_UInt)n, n > 0 ? buf : NULL) != 0) {
        return false;
    }
    {
        int16_t *c = NULL;
        if (n > 0) {
            c = pc_malloc_array(n, sizeof *c);
            if (c == NULL) {
                return false;
            }
            memcpy(c, coords, n * sizeof *c);
        }
        free(f->coords);
        f->coords = c;
        f->ncoords = n;
    }
    return true;
}

/* Converts FreeType's loaded outline: points are 26.6 bits already (scaled) or font units
 * (unscaled: shifted to 26.6, or manually scaled with `scale` when manual). */
/* A loaded outline copied out of FreeType (26.6 points), so pens run without the font lock
 * held and may draw other glyphs of the same font. */
typedef struct outline_copy {
    cpt *pts;
    size_t np;
    size_t *ends;
    size_t nc;
} outline_copy;

static pc_status copy_outline(const FT_Outline *o, int mode, int32_t scale, outline_copy *out, pc_error *err)
{
    size_t np = (size_t)(unsigned short)o->n_points, nc = (size_t)(unsigned short)o->n_contours, c;
    memset(out, 0, sizeof *out);
    if (np == 0 || nc == 0) {
        return PC_OK;
    }
    out->pts = pc_malloc_array(np, sizeof *out->pts);
    out->ends = pc_malloc_array(nc, sizeof *out->ends);
    if (out->pts == NULL || out->ends == NULL) {
        free(out->pts);
        free(out->ends);
        memset(out, 0, sizeof *out);
        return pc_fail(err, PC_ERR_NOMEM, "font: out of memory");
    }
    for (c = 0; c < np; c++) {
        int32_t x = (int32_t)o->points[c].x, y = (int32_t)o->points[c].y;
        if (mode == 1) { /* unscaled: F26Dot6::from_i32 */
            x = pc_i32_wrapping_mul(x, 64);
            y = pc_i32_wrapping_mul(y, 64);
        } else if (mode == 2) { /* manual scale */
            x = fixed_mul(x, scale);
            y = fixed_mul(y, scale);
        }
        out->pts[c] = (cpt){x, y, tag_of((unsigned char)o->tags[c])};
    }
    for (c = 0; c < nc; c++) {
        out->ends[c] = (size_t)(unsigned short)o->contours[c];
    }
    out->np = np;
    out->nc = nc;
    return PC_OK;
}

static pc_status emit_outline(const outline_copy *o, const pc_outline_pen *pen, pc_error *err)
{
    size_t c, start = 0;
    for (c = 0; c < o->nc; c++) {
        size_t end = o->ends[c];
        if (end < start || end >= o->np) {
            return pc_fail(err, PC_ERR_FORMAT, "font: contour end point out of order");
        }
        if (!contour(o->pts + start, end - start + 1, pen)) {
            return pc_fail(err, PC_ERR_FORMAT, "font: malformed contour");
        }
        start = end + 1;
    }
    return PC_OK;
}

pc_status pc_font_draw(pc_font *f, uint32_t gid, const float *size_px, const int16_t *coords, size_t ncoords,
                       const pc_outline_pen *pen, pc_error *err)
{
    FT_Int32 flags = FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM;
    int mode = 0;
    int32_t scale = 0;
    pc_status st;
    if (f == NULL || pen == NULL || (ncoords > 0 && coords == NULL)) {
        return pc_fail(err, PC_ERR_INVALID, "font: invalid arguments");
    }
    if (gid >= f->num_glyphs) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "font: no glyph %u", (unsigned)gid);
    }
    if (size_px != NULL && f->upem != 0) {
        /* skrifa: Fixed::from_bits((ppem * 64.) as i32) / Fixed::from_bits(upem). */
        int32_t bits = pc_f32_to_i32(*size_px * 64.0f);
        scale = fixed_div(bits, (int32_t)f->upem);
        pthread_mutex_lock(&f->lock);
        if (bits != f->size_bits) {
            FT_Size_RequestRec req;
            req.type = FT_SIZE_REQUEST_TYPE_NOMINAL;
            req.width = bits;
            req.height = bits;
            req.horiResolution = 0;
            req.vertResolution = 0;
            f->size_bits =
                (bits > 0 && FT_Request_Size(f->face, &req) == 0 && f->face->size->metrics.x_scale == scale)
                    ? bits
                    : 0;
        }
        mode = f->size_bits == bits && bits > 0 ? 0 : 2;
    } else {
        pthread_mutex_lock(&f->lock);
        mode = 1;
    }
    if (mode != 0) {
        flags |= FT_LOAD_NO_SCALE;
    }
    if (!set_coords(f, coords, ncoords)) {
        pthread_mutex_unlock(&f->lock);
        return pc_fail(err, PC_ERR_FORMAT, "font: cannot set variation coordinates");
    }
    if (FT_Load_Glyph(f->face, gid, flags) != 0) {
        pthread_mutex_unlock(&f->lock);
        return pc_fail(err, PC_ERR_FORMAT, "font: cannot load glyph %u", (unsigned)gid);
    }
    if (f->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        pthread_mutex_unlock(&f->lock);
        return pc_fail(err, PC_ERR_NOT_FOUND, "font: glyph %u has no outline", (unsigned)gid);
    }
    {
        outline_copy oc;
        bool cff = f->cff;
        st = copy_outline(&f->face->glyph->outline, mode, scale, &oc, err);
        pthread_mutex_unlock(&f->lock);
        if (st != PC_OK) {
            return st;
        }
        if (cff) {
            nop_filter nf;
            pc_outline_pen fp = {nf_move, nf_line, nf_quad, nf_curve, nf_close, &nf};
            memset(&nf, 0, sizeof nf);
            nf.inner = pen;
            st = emit_outline(&oc, &fp, err);
        } else {
            st = emit_outline(&oc, pen, err);
        }
        free(oc.pts);
        free(oc.ends);
    }
    return st;
}

bool pc_font_advance(pc_font *f, uint32_t gid, const int16_t *coords, size_t ncoords, float *out)
{
    int32_t adv;
    FT_Fixed a = 0;
    bool ok;
    if (f == NULL || out == NULL) {
        return false;
    }
    if (ncoords == 0 || !f->mm) {
        if (!pc_sfnt_advance(&f->sfnt, gid, &adv)) {
            return false;
        }
        *out = (float)adv;
        return true;
    }
    if (gid >= f->num_glyphs) {
        return false;
    }
    pthread_mutex_lock(&f->lock);
    ok = set_coords(f, coords, ncoords) && FT_Get_Advance(f->face, gid, FT_LOAD_NO_SCALE, &a) == 0;
    pthread_mutex_unlock(&f->lock);
    if (ok) {
        *out = (float)a;
    }
    return ok;
}
