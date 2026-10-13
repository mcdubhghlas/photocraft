/* Selection masks, surfaces, magic wand and regions (port of crates/algo/src/selection.rs, part 1;
 * the Color Range and Modify functions are in selection_range.c). */
#include "algo/selection.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

pc_algo_selection_mode pc_algo_selection_mode_parse(const char *s)
{
    if (s == NULL) {
        return PC_ALGO_SELECTION_REPLACE;
    }
    if (strcmp(s, "add") == 0) {
        return PC_ALGO_SELECTION_ADD;
    }
    if (strcmp(s, "subtract") == 0) {
        return PC_ALGO_SELECTION_SUBTRACT;
    }
    if (strcmp(s, "intersect") == 0) {
        return PC_ALGO_SELECTION_INTERSECT;
    }
    return PC_ALGO_SELECTION_REPLACE;
}

const char *pc_algo_selection_mode_name(pc_algo_selection_mode m)
{
    switch (m) {
    case PC_ALGO_SELECTION_ADD:
        return "add";
    case PC_ALGO_SELECTION_SUBTRACT:
        return "subtract";
    case PC_ALGO_SELECTION_INTERSECT:
        return "intersect";
    case PC_ALGO_SELECTION_REPLACE:
    default:
        return "replace";
    }
}

void pc_algo_region_fini(pc_algo_region *r)
{
    if (r == NULL) {
        return;
    }
    free(r->mask);
    r->mask = NULL;
    r->len = 0;
    r->bbox = PC_RECT_EMPTY;
}

float pc_algo_region_at(const pc_algo_region *r, int32_t x, int32_t y)
{
    size_t i;
    if (!pc_rect_contains(r->bbox, x, y)) {
        return 0.0f;
    }
    i = (size_t)((int64_t)y - r->bbox.y0) * pc_rect_width(r->bbox) + (size_t)((int64_t)x - r->bbox.x0);
    if (r->mask == NULL || i >= r->len) {
        return 0.0f;
    }
    return (float)r->mask[i] / 255.0f;
}

pc_status pc_algo_region_clone(const pc_algo_region *r, pc_algo_region *out, pc_error *err)
{
    uint8_t *m = NULL;
    if (r->len > 0) {
        m = pc_malloc(r->len);
        if (m == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a region");
        }
        memcpy(m, r->mask, r->len);
    }
    out->bbox = r->bbox;
    out->mask = m;
    out->len = r->len;
    return PC_OK;
}

/* w * h of a rect, checked against PC_MAX_ALLOC / elem. */
static pc_status area_count(pc_rect r, size_t elem, size_t *n, pc_error *err)
{
    size_t c, bytes;
    if (!pc_size_mul((size_t)pc_rect_width(r), (size_t)pc_rect_height(r), &c) ||
        !pc_size_mul(c, elem, &bytes) || bytes > PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "selection area too large");
    }
    *n = c;
    return PC_OK;
}

/* ---- mask_from_surface ---- */

typedef struct gray8_read {
    const pc_surface *s;
    pc_rect area;
    size_t w;
    float lut[256];
    float dp;
    float *out;
} gray8_read;

static pc_status gray8_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    gray8_read *g = ctx;
    size_t yy;
    (void)chunk;
    for (yy = begin; yy < end; yy++) {
        int32_t y = g->area.y0 + (int32_t)yy;
        int32_t x = g->area.x0;
        float *row = g->out + yy * g->w;
        while (x < g->area.x1) {
            pc_tile tc = pc_tile_containing(x, y);
            pc_rect tr = pc_tile_rect(tc);
            int32_t x1 = tr.x1 < g->area.x1 ? tr.x1 : g->area.x1;
            if (x1 <= x) { /* the last tile column's edge wraps past INT32_MAX */
                x1 = g->area.x1;
            }
            const pc_raster_tile *t = pc_surface_tile(g->s, tc);
            size_t off = (size_t)(x - g->area.x0), n = (size_t)((int64_t)x1 - x), i;
            float *dst = row + off;
            const uint8_t *src = NULL;
            if (t != NULL) {
                size_t base = (size_t)(y - tr.y0) * PC_TILE_SIZE + (size_t)(x - tr.x0);
                if (base + n <= pc_raster_tile_len(t)) {
                    src = pc_raster_tile_bytes(t) + base;
                }
            }
            if (src != NULL) {
                for (i = 0; i < n; i++) {
                    dst[i] = g->lut[src[i]];
                }
            } else {
                for (i = 0; i < n; i++) {
                    dst[i] = g->dp;
                }
            }
            x = x1;
        }
    }
    return PC_OK;
}

pc_status pc_algo_selection_mask_from_surface(const pc_surface *s, pc_rect area, float **out, size_t *len,
                                              pc_error *err)
{
    size_t n, i;
    float *v;
    pc_status st = area_count(area, sizeof(float), &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n == 0) {
        *out = NULL;
        *len = 0;
        return PC_OK;
    }
    v = pc_calloc_array(n, sizeof(float));
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading a selection");
    }
    if (s == NULL) {
        /* No selection: all zero. */
    } else if (pc_pixel_format_eq(pc_surface_format(s), PC_PIXEL_FORMAT_GRAY8)) {
        /* Selections are GRAY8: copy tile bytes through a table, rows in parallel. */
        gray8_read g;
        float dpx[PC_RASTER_MAX_CHANNELS];
        g.s = s;
        g.area = area;
        g.w = pc_rect_width(area);
        for (i = 0; i < 256; i++) {
            g.lut[i] = (float)i / 255.0f;
        }
        pc_surface_default_pixel(s, dpx);
        g.dp = dpx[0];
        g.out = v;
        (void)pc_algo_par_for(pc_rect_height(area), 1, gray8_rows, &g);
    } else {
        size_t ch = pc_surface_channels(s);
        if (ch <= 1) {
            st = pc_surface_read_region_into(s, area, v, n, err);
        } else {
            float *all = NULL;
            size_t alen = 0;
            st = pc_surface_read_region(s, area, &all, &alen, err);
            if (st == PC_OK) {
                for (i = 0; i < n && i * ch < alen; i++) {
                    v[i] = all[i * ch];
                }
            }
            free(all);
        }
        if (st != PC_OK) {
            free(v);
            return st;
        }
    }
    *out = v;
    *len = n;
    return PC_OK;
}

/* 8-bit selection value of a coverage (clamped and rounded as GRAY8 stores it; NaN -> 0). */
static uint8_t coverage8(float v)
{
    return pc_f32_to_u8(pc_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/* A GRAY8 surface from 8-bit coverage over `area`, allocating only tiles that hold something;
 * fully selected tiles share one tile (copy-on-write), so the inside of a large selection costs
 * no memory per tile (#211). */
static pc_status surface_from_coverage8(const uint8_t *bytes, pc_rect area, pc_surface *out, pc_error *err)
{
    pc_surface s;
    pc_tile first, last, tc;
    pc_raster_tile *full = NULL;
    size_t w = pc_rect_width(area);
    pc_status st = PC_OK;
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    if (pc_rect_tile_range(area, &first, &last)) {
        for (tc.ty = first.ty; tc.ty <= last.ty; tc.ty++) {
            for (tc.tx = first.tx; tc.tx <= last.tx; tc.tx++) {
                pc_rect trect = pc_tile_rect(tc);
                pc_rect tr = pc_rect_intersect(trect, area);
                size_t lx = (size_t)(tr.x0 - area.x0), span = pc_rect_width(tr), i;
                int32_t y;
                bool any = false, all = pc_rect_eq(tr, trect);
                uint8_t *data = NULL;
                for (y = tr.y0; y < tr.y1 && (!any || all); y++) {
                    const uint8_t *row = bytes + (size_t)(y - area.y0) * w + lx;
                    for (i = 0; i < span; i++) {
                        any |= row[i] != 0;
                        all &= row[i] == 255;
                    }
                }
                if (!any) {
                    continue;
                }
                if (all) {
                    pc_tile_entry e;
                    if (full == NULL) {
                        float one = 1.0f;
                        st = pc_surface_solid_tile(&s, &one, 1, &full, err);
                        if (st != PC_OK) {
                            goto fail;
                        }
                    }
                    e.coord = tc;
                    e.tile = pc_raster_tile_retain(full);
                    st = pc_surface_put_tiles(&s, &e, 1, err);
                    if (st != PC_OK) {
                        goto fail;
                    }
                    continue;
                }
                st = pc_surface_tile_mut(&s, tc, &data, err);
                if (st != PC_OK) {
                    goto fail;
                }
                for (y = tr.y0; y < tr.y1; y++) {
                    size_t o = (size_t)(y - trect.y0) * PC_TILE_SIZE + (size_t)(tr.x0 - trect.x0);
                    memcpy(data + o, bytes + (size_t)(y - area.y0) * w + lx, span);
                }
            }
        }
    }
    pc_raster_tile_release(full);
    *out = s;
    return PC_OK;
fail:
    pc_raster_tile_release(full);
    pc_surface_fini(&s);
    return st;
}

typedef struct cov8_ctx {
    const float *m;
    size_t mlen;
    const float *prev;
    size_t plen;
    pc_algo_selection_mode mode;
    bool combine;
    size_t w;
    uint8_t *bytes;
} cov8_ctx;

static float q8(float v)
{
    return (float)coverage8(v) / 255.0f;
}

static pc_status cov8_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    cov8_ctx *c = ctx;
    size_t y, x;
    (void)chunk;
    for (y = begin; y < end; y++) {
        for (x = 0; x < c->w; x++) {
            size_t i = y * c->w + x;
            float v = i < c->mlen ? c->m[i] : 0.0f;
            if (c->combine) {
                float at = i < c->plen ? c->prev[i] : 0.0f;
                switch (c->mode) {
                case PC_ALGO_SELECTION_ADD:
                    v = pc_rs_maxf(at, q8(v));
                    break;
                case PC_ALGO_SELECTION_SUBTRACT:
                    v = pc_rs_maxf_k(at - q8(v), 0.0f);
                    break;
                case PC_ALGO_SELECTION_REPLACE:
                case PC_ALGO_SELECTION_INTERSECT:
                default:
                    v = pc_rs_minf(at, q8(v));
                    break;
                }
            }
            c->bytes[i] = coverage8(v);
        }
    }
    return PC_OK;
}

/* 8-bit coverage of `m` over area, combined with `prev` when combine is set. */
static pc_status to_coverage8(const float *m, size_t mlen, pc_rect area, const float *prev, size_t plen,
                              pc_algo_selection_mode mode, bool combine, uint8_t **out, size_t *n,
                              pc_error *err)
{
    cov8_ctx c;
    size_t cnt;
    pc_status st = area_count(area, 1, &cnt, err);
    if (st != PC_OK) {
        return st;
    }
    c.bytes = pc_calloc_array(cnt ? cnt : 1, 1);
    if (c.bytes == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory building a selection");
    }
    c.m = m;
    c.mlen = m != NULL ? mlen : 0;
    c.prev = prev;
    c.plen = prev != NULL ? plen : 0;
    c.mode = mode;
    c.combine = combine;
    c.w = pc_rect_width(area);
    (void)pc_algo_par_for(cnt ? pc_rect_height(area) : 0, 1, cov8_rows, &c);
    *out = c.bytes;
    *n = cnt;
    return PC_OK;
}

pc_status pc_algo_selection_mask_to_surface(const float *m, size_t len, pc_rect area, pc_surface *out,
                                            pc_error *err)
{
    uint8_t *bytes = NULL;
    size_t n = 0;
    pc_status st;
    if (pc_rect_is_empty(area)) {
        pc_surface_init(out, PC_PIXEL_FORMAT_GRAY8);
        return PC_OK;
    }
    st = to_coverage8(m, len, area, NULL, 0, PC_ALGO_SELECTION_REPLACE, false, &bytes, &n, err);
    if (st == PC_OK) {
        st = surface_from_coverage8(bytes, area, out, err);
    }
    free(bytes);
    return st;
}

pc_status pc_algo_selection_combine(const pc_surface *old, const float *m, size_t len, pc_rect area,
                                    pc_algo_selection_mode mode, pc_surface *out, bool *some, pc_error *err)
{
    uint8_t *bytes = NULL;
    float *prev = NULL;
    size_t n = 0, plen = 0, i;
    bool any = false;
    pc_status st;
    if (pc_rect_is_empty(area)) {
        pc_surface_init(out, PC_PIXEL_FORMAT_GRAY8);
        *some = false;
        return PC_OK;
    }
    /* Quantise to the 8-bit grid selections are stored on, so combining a mask with itself is
     * exact. Below half an 8-bit step is nothing. */
    if (mode != PC_ALGO_SELECTION_REPLACE) {
        st = pc_algo_selection_mask_from_surface(old, area, &prev, &plen, err);
        if (st != PC_OK) {
            return st;
        }
    }
    st = to_coverage8(m, len, area, prev, plen, mode, mode != PC_ALGO_SELECTION_REPLACE, &bytes, &n, err);
    free(prev);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n; i++) {
        if (bytes[i] != 0) {
            any = true;
            break;
        }
    }
    if (!any) {
        free(bytes);
        pc_surface_init(out, PC_PIXEL_FORMAT_GRAY8);
        *some = false;
        return PC_OK;
    }
    st = surface_from_coverage8(bytes, area, out, err);
    free(bytes);
    if (st == PC_OK) {
        *some = true;
    }
    return st;
}

/* ---- magic wand (float pixels) ---- */

static bool close4(const float a[4], const float b[4], float tol)
{
    int c;
    /* Fully transparent pixels match whatever RGB they hide. */
    if (a[3] == 0.0f && b[3] == 0.0f) {
        return true;
    }
    for (c = 0; c < 4; c++) {
        if (!(fabsf(a[c] - b[c]) * 255.0f <= tol + 1e-3f)) {
            return false;
        }
    }
    return true;
}

/* Softens hard mask edges: edge pixels become the 3x3 average. */
static pc_status antialias_f(float *m, size_t w, size_t h, pc_error *err)
{
    size_t n = w * h, x, y;
    float *src = pc_malloc_array(n ? n : 1, sizeof(float));
    if (src == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory anti-aliasing a selection");
    }
    memcpy(src, m, n * sizeof(float));
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t i = y * w + x;
            float sum = 0.0f, cnt = 0.0f;
            bool edge = false;
            int dy, dx;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int64_t xx = (int64_t)x + dx, yy = (int64_t)y + dy;
                    float v = (xx < 0 || yy < 0 || xx >= (int64_t)w || yy >= (int64_t)h)
                                  ? src[i]
                                  : src[(size_t)yy * w + (size_t)xx];
                    edge |= v != src[i];
                    sum += v;
                    cnt += 1.0f;
                }
            }
            if (edge) {
                /* Keep selected pixels mostly selected, add a soft fringe. */
                m[i] = src[i] > 0.5f ? 0.5f + 0.5f * sum / cnt : 0.5f * sum / cnt;
            }
        }
    }
    free(src);
    return PC_OK;
}

pc_status pc_algo_selection_magic_wand(const float (*px)[4], size_t px_len, pc_rect area, int32_t seed_x,
                                       int32_t seed_y, float tolerance, bool contiguous, bool anti_alias,
                                       float **out, size_t *len, pc_error *err)
{
    size_t w = pc_rect_width(area), h = pc_rect_height(area), n, si, i;
    float *m;
    const float *target;
    pc_status st = area_count(area, sizeof(float), &n, err);
    if (st != PC_OK) {
        return st;
    }
    m = pc_calloc_array(n ? n : 1, sizeof(float));
    if (m == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the magic wand");
    }
    if (!pc_rect_contains(area, seed_x, seed_y)) {
        *out = m;
        *len = n;
        return PC_OK;
    }
    if (px == NULL || px_len != n) {
        free(m);
        return pc_fail(err, PC_ERR_INVALID, "magic wand: %zu pixels for a %zux%zu area", px_len, w, h);
    }
    si = (size_t)(seed_y - area.y0) * w + (size_t)(seed_x - area.x0);
    target = px[si];
    if (contiguous) {
        /* Every pixel is pushed at most once (marked before the push). */
        size_t *stack = pc_malloc_array(n, sizeof(size_t)), top = 0;
        if (stack == NULL) {
            free(m);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for the magic wand");
        }
        stack[top++] = si;
        m[si] = 1.0f;
        while (top > 0) {
            size_t c = stack[--top], x = c % w, y = c / w, nb[4], k, cnt = 0;
            if (x > 0) {
                nb[cnt++] = c - 1;
            }
            if (x + 1 < w) {
                nb[cnt++] = c + 1;
            }
            if (y > 0) {
                nb[cnt++] = c - w;
            }
            if (y + 1 < h) {
                nb[cnt++] = c + w;
            }
            for (k = 0; k < cnt; k++) {
                size_t j = nb[k];
                if (m[j] == 0.0f && close4(px[j], target, tolerance)) {
                    m[j] = 1.0f;
                    stack[top++] = j;
                }
            }
        }
        free(stack);
    } else {
        for (i = 0; i < n; i++) {
            if (close4(px[i], target, tolerance)) {
                m[i] = 1.0f;
            }
        }
    }
    if (anti_alias) {
        st = antialias_f(m, w, h, err);
        if (st != PC_OK) {
            free(m);
            return st;
        }
    }
    *out = m;
    *len = n;
    return PC_OK;
}

typedef struct rgba8_ctx {
    const pc_surface *s;
    pc_rect area;
    size_t w, band;
    uint8_t (*img)[4];
    size_t n;
    pc_error *errs;
} rgba8_ctx;

static pc_status rgba8_band(void *ctx, size_t chunk, size_t begin, size_t end)
{
    rgba8_ctx *c = ctx;
    size_t bi;
    (void)chunk;
    for (bi = begin; bi < end; bi++) {
        size_t r0 = bi * c->band, rows = pc_rect_height(c->area) - r0;
        int32_t y0;
        if (rows > c->band) {
            rows = c->band;
        }
        y0 = c->area.y0 + (int32_t)r0;
        {
            pc_status st =
                pc_surface_read_rgba8_into(c->s, pc_rect_make(c->area.x0, y0, c->area.x1, y0 + (int32_t)rows),
                                           c->img + r0 * c->w, rows * c->w, NULL);
            if (st != PC_OK) {
                return st;
            }
        }
    }
    return PC_OK;
}

pc_status pc_algo_selection_rgba8_image(const pc_surface *s, pc_rect area, uint8_t (**out)[4], size_t *len,
                                        pc_error *err)
{
    rgba8_ctx c;
    size_t n, h = pc_rect_height(area);
    pc_status st = area_count(area, 4, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n == 0) {
        *out = NULL;
        *len = 0;
        return PC_OK;
    }
    c.img = pc_calloc_array(n, 4);
    if (c.img == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading pixels");
    }
    c.s = s;
    c.area = area;
    c.w = pc_rect_width(area);
    c.band = 256;
    c.n = n;
    c.errs = NULL;
    st = pc_algo_par_for((h + c.band - 1) / c.band, 1, rgba8_band, &c);
    if (st != PC_OK) {
        free(c.img);
        return pc_fail(err, st, "reading pixels failed");
    }
    *out = c.img;
    *len = n;
    return PC_OK;
}

/* ---- wand_region (8-bit pixels) ---- */

typedef struct wand8 {
    const uint8_t (*img)[4];
    const uint8_t *marks;
    uint8_t *mark_w;
    size_t w, h;
    uint8_t target[4];
    int32_t tol;
    /* global scan: per-row spans */
    size_t *span_a, *span_b;
    bool *span_ok;
    /* anti-aliased fill */
    size_t x0, y0, bw;
    bool anti_alias;
    uint8_t *mask;
} wand8;

static bool similar8(const wand8 *c, const uint8_t p[4])
{
    int k;
    /* Copying a selection can leave RGB under zero alpha: those invisible colours must not split
     * a transparent region into pieces. */
    if (c->target[3] == 0 && p[3] == 0) {
        return true;
    }
    for (k = 0; k < 4; k++) {
        int d = (int)p[k] - (int)c->target[k];
        if (d < 0) {
            d = -d;
        }
        if (d > c->tol) {
            return false;
        }
    }
    return true;
}

static pc_status wand_scan_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    wand8 *c = ctx;
    size_t y, x;
    (void)chunk;
    for (y = begin; y < end; y++) {
        bool ok = false;
        size_t a = 0, b = 0;
        for (x = 0; x < c->w; x++) {
            if (similar8(c, c->img[y * c->w + x])) {
                c->mark_w[y * c->w + x] = 255;
                if (!ok) {
                    a = x;
                }
                b = x;
                ok = true;
            }
        }
        c->span_ok[y] = ok;
        c->span_a[y] = a;
        c->span_b[y] = b;
    }
    return PC_OK;
}

static pc_status wand_fill_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    wand8 *c = ctx;
    size_t yy, xx;
    (void)chunk;
    for (yy = begin; yy < end; yy++) {
        size_t y = c->y0 + yy;
        uint8_t *out = c->mask + yy * c->bw;
        for (xx = 0; xx < c->bw; xx++) {
            size_t x = c->x0 + xx;
            uint8_t v = c->marks[y * c->w + x];
            uint32_t sum = 0;
            bool edge = false;
            int dy, dx;
            out[xx] = v;
            if (!c->anti_alias) {
                continue;
            }
            /* Same rule as the float anti-aliasing: edge pixels blend with their 3x3
             * neighbourhood; neighbours outside the area count as the pixel itself. */
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int64_t nx = (int64_t)x + dx, ny = (int64_t)y + dy;
                    uint8_t nv = (nx < 0 || ny < 0 || nx >= (int64_t)c->w || ny >= (int64_t)c->h)
                                     ? v
                                     : c->marks[(size_t)ny * c->w + (size_t)nx];
                    edge |= nv != v;
                    sum += nv;
                }
            }
            if (edge) {
                float avg = (float)sum / (9.0f * 255.0f);
                float f = v > 127 ? 0.5f + 0.5f * avg : 0.5f * avg;
                out[xx] = pc_f32_to_u8(roundf(f * 255.0f));
            }
        }
    }
    return PC_OK;
}

typedef struct xy {
    size_t x, y;
} xy;

pc_status pc_algo_selection_wand_region(const uint8_t (*img)[4], size_t img_len, pc_rect area, int32_t seed_x,
                                        int32_t seed_y, float tolerance, bool contiguous, bool anti_alias,
                                        pc_algo_region *out, pc_error *err)
{
    wand8 c;
    size_t w = pc_rect_width(area), h = pc_rect_height(area), n, si;
    size_t bx0 = SIZE_MAX, by0 = SIZE_MAX, bx1 = 0, by1 = 0;
    size_t pad, x0, y0, x1, y1, bh, mlen;
    uint8_t *marks;
    pc_status st;
    if (!pc_rect_contains(area, seed_x, seed_y)) {
        out->bbox = PC_RECT_EMPTY;
        out->mask = NULL;
        out->len = 0;
        return PC_OK;
    }
    st = area_count(area, 1, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (img == NULL || img_len != n) {
        return pc_fail(err, PC_ERR_INVALID, "wand region: %zu pixels for a %zux%zu area", img_len, w, h);
    }
    memset(&c, 0, sizeof c);
    c.img = img;
    c.w = w;
    c.h = h;
    si = (size_t)(seed_y - area.y0) * w + (size_t)(seed_x - area.x0);
    memcpy(c.target, img[si], 4);
    c.tol = pc_f32_to_i32(pc_rs_maxf_k(floorf(tolerance + 1e-3f), 0.0f));
    marks = pc_calloc_array(n, 1);
    if (marks == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the magic wand");
    }
    c.marks = marks;
    c.mark_w = marks;
    if (contiguous) {
        /* Span fill: fill a run, then scan the rows above and below it for new seeds. */
        xy *stack = NULL;
        size_t cap = 0, top = 0;
        stack = pc_grow_array(NULL, &cap, 64, sizeof(xy));
        if (stack == NULL) {
            free(marks);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for the magic wand");
        }
        stack[top].x = (size_t)(seed_x - area.x0);
        stack[top].y = (size_t)(seed_y - area.y0);
        top++;
        while (top > 0) {
            xy p = stack[--top];
            size_t row = p.y * w, l = p.x, r = p.x, k;
            int side;
            if (marks[row + p.x] != 0) {
                continue;
            }
            while (l > 0 && marks[row + l - 1] == 0 && similar8(&c, img[row + l - 1])) {
                l--;
            }
            while (r + 1 < w && marks[row + r + 1] == 0 && similar8(&c, img[row + r + 1])) {
                r++;
            }
            memset(marks + row + l, 255, r - l + 1);
            bx0 = bx0 < l ? bx0 : l;
            bx1 = bx1 > r ? bx1 : r;
            by0 = by0 < p.y ? by0 : p.y;
            by1 = by1 > p.y ? by1 : p.y;
            for (side = 0; side < 2; side++) {
                size_t ny = side == 0 ? p.y - 1 : p.y + 1, nrow; /* wraps for p.y == 0 */
                bool in_run = false;
                if (ny >= h) {
                    continue;
                }
                nrow = ny * w;
                for (k = l; k <= r; k++) {
                    bool ok = marks[nrow + k] == 0 && similar8(&c, img[nrow + k]);
                    if (ok && !in_run) {
                        if (top == cap) {
                            xy *g = pc_grow_array(stack, &cap, top + 1, sizeof(xy));
                            if (g == NULL) {
                                free(stack);
                                free(marks);
                                return pc_fail(err, PC_ERR_NOMEM, "out of memory for the magic wand");
                            }
                            stack = g;
                        }
                        stack[top].x = k;
                        stack[top].y = ny;
                        top++;
                    }
                    in_run = ok;
                }
            }
        }
        free(stack);
    } else {
        size_t y;
        c.span_a = pc_malloc_array(h, sizeof(size_t));
        c.span_b = pc_malloc_array(h, sizeof(size_t));
        c.span_ok = pc_malloc_array(h, sizeof(bool));
        if (c.span_a == NULL || c.span_b == NULL || c.span_ok == NULL) {
            free(c.span_a);
            free(c.span_b);
            free(c.span_ok);
            free(marks);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for the magic wand");
        }
        (void)pc_algo_par_for(h, 1, wand_scan_rows, &c);
        for (y = 0; y < h; y++) {
            if (c.span_ok[y]) {
                bx0 = bx0 < c.span_a[y] ? bx0 : c.span_a[y];
                bx1 = bx1 > c.span_b[y] ? bx1 : c.span_b[y];
                by0 = by0 < y ? by0 : y;
                by1 = by1 > y ? by1 : y;
            }
        }
        free(c.span_a);
        free(c.span_b);
        free(c.span_ok);
    }
    if (bx0 == SIZE_MAX) {
        free(marks);
        out->bbox = PC_RECT_EMPTY;
        out->mask = NULL;
        out->len = 0;
        return PC_OK;
    }
    /* One pixel of fringe for anti-aliasing, clamped to the area. */
    pad = anti_alias ? 1 : 0;
    x0 = bx0 >= pad ? bx0 - pad : 0;
    y0 = by0 >= pad ? by0 - pad : 0;
    x1 = bx1 + 1 + pad < w ? bx1 + 1 + pad : w;
    y1 = by1 + 1 + pad < h ? by1 + 1 + pad : h;
    c.x0 = x0;
    c.y0 = y0;
    c.bw = x1 - x0;
    bh = y1 - y0;
    mlen = c.bw * bh;
    c.anti_alias = anti_alias;
    c.mask = pc_malloc(mlen);
    if (c.mask == NULL) {
        free(marks);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the magic wand");
    }
    (void)pc_algo_par_for(bh, 1, wand_fill_rows, &c);
    free(marks);
    out->bbox = pc_rect_make(area.x0 + (int32_t)x0, area.y0 + (int32_t)y0, area.x0 + (int32_t)x1,
                             area.y0 + (int32_t)y1);
    out->mask = c.mask;
    out->len = mlen;
    return PC_OK;
}

/* ---- combine_region ---- */

/* The previous selection over b as 8-bit coverage (channel 0, clamped and rounded). */
static pc_status prev_u8(const pc_surface *s, pc_rect b, uint8_t **out, size_t *n, pc_error *err)
{
    float *v = NULL;
    size_t len = 0, ch = pc_surface_channels(s), cnt, i;
    uint8_t *o;
    pc_status st = pc_surface_read_region(s, b, &v, &len, err);
    if (st != PC_OK) {
        return st;
    }
    cnt = ch ? len / ch : 0;
    o = pc_malloc(cnt ? cnt : 1);
    if (o == NULL) {
        free(v);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory combining a selection");
    }
    for (i = 0; i < cnt; i++) {
        o[i] = pc_f32_to_u8(roundf(pc_clampf(v[i * ch], 0.0f, 1.0f) * 255.0f));
    }
    free(v);
    *out = o;
    *n = cnt;
    return PC_OK;
}

static pc_status from_bytes(const uint8_t *bytes, size_t n, pc_rect b, pc_surface *out, bool *some,
                            pc_error *err)
{
    size_t i;
    bool any = false;
    pc_status st;
    for (i = 0; i < n; i++) {
        if (bytes[i] != 0) {
            any = true;
            break;
        }
    }
    if (pc_rect_is_empty(b) || !any) {
        pc_surface_init(out, PC_PIXEL_FORMAT_GRAY8);
        *some = false;
        return PC_OK;
    }
    st = pc_surface_from_interleaved(PC_PIXEL_FORMAT_GRAY8, b, bytes, n, out, err);
    if (st != PC_OK) {
        return st;
    }
    pc_surface_prune(out);
    *some = true;
    return PC_OK;
}

pc_status pc_algo_selection_combine_region(const pc_surface *old, const pc_algo_region *region,
                                           pc_algo_selection_mode mode, pc_surface *out, bool *some,
                                           pc_error *err)
{
    static const pc_algo_region empty = {{0, 0, 0, 0}, NULL, 0};
    const pc_algo_region *nr = region != NULL ? region : &empty;
    pc_rect b = nr->bbox;
    size_t want = (size_t)pc_rect_width(b) * pc_rect_height(b), i, pn = 0;
    uint8_t *p = NULL;
    pc_status st;
    if (!pc_rect_is_empty(b) && (nr->mask == NULL || nr->len != want)) {
        return pc_fail(err, PC_ERR_INVALID, "region mask holds %zu bytes for %zu pixels", nr->len, want);
    }
    if (mode == PC_ALGO_SELECTION_REPLACE || (mode == PC_ALGO_SELECTION_ADD && old == NULL)) {
        return from_bytes(nr->mask, pc_rect_is_empty(b) ? 0 : nr->len, b, out, some, err);
    }
    if (old == NULL) {
        pc_surface_init(out, PC_PIXEL_FORMAT_GRAY8);
        *some = false;
        return PC_OK;
    }
    if (mode == PC_ALGO_SELECTION_INTERSECT) {
        if (pc_rect_is_empty(b)) {
            pc_surface_init(out, PC_PIXEL_FORMAT_GRAY8);
            *some = false;
            return PC_OK;
        }
        st = prev_u8(old, b, &p, &pn, err);
        if (st != PC_OK) {
            return st;
        }
        for (i = 0; i < pn && i < nr->len; i++) {
            p[i] = p[i] < nr->mask[i] ? p[i] : nr->mask[i];
        }
        st = from_bytes(p, pn < nr->len ? pn : nr->len, b, out, some, err);
        free(p);
        return st;
    }
    /* Add / Subtract with a previous selection. */
    {
        pc_surface s;
        float *v;
        size_t cnt;
        if (pc_rect_is_empty(b)) {
            st = pc_surface_clone(old, out, err);
            if (st == PC_OK) {
                *some = !pc_rect_is_empty(pc_surface_content_bounds(out));
            }
            return st;
        }
        st = prev_u8(old, b, &p, &pn, err);
        if (st != PC_OK) {
            return st;
        }
        cnt = pn < nr->len ? pn : nr->len;
        v = pc_malloc_array(cnt ? cnt : 1, sizeof(float));
        if (v == NULL) {
            free(p);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory combining a selection");
        }
        for (i = 0; i < cnt; i++) {
            uint8_t a = p[i], m = nr->mask[i];
            uint8_t r = mode == PC_ALGO_SELECTION_ADD ? (a > m ? a : m) : (uint8_t)(a > m ? a - m : 0);
            v[i] = (float)r / 255.0f;
        }
        free(p);
        st = pc_surface_clone(old, &s, err);
        if (st == PC_OK) {
            st = pc_surface_write_region(&s, b, v, cnt, err);
            if (st != PC_OK) {
                pc_surface_fini(&s);
            }
        }
        free(v);
        if (st != PC_OK) {
            return st;
        }
        pc_surface_prune(&s);
        *some = !pc_rect_is_empty(pc_surface_content_bounds(&s));
        *out = s;
        return PC_OK;
    }
}
