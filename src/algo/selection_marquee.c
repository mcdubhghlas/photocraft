/* Marquee shapes built straight into tiles, and the tile-wise combination of two selections
 * (port of crates/algo/src/selection/marquee.rs, #2888, part of #211).
 *
 * Both produce exactly the bytes the per-pixel versions did (an ellipse written pixel by pixel,
 * a combination read and written through whole-area float buffers), but work a tile at a time on
 * the global pool, keep only the tiles that hold something (as pc_surface_prune would leave them)
 * and share one tile for every fully selected one. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/selection.h"
#include "base/mem.h"
#include "base/num.h"
#include "color/color.h"

#define TS ((size_t)PC_TILE_SIZE)

/* The byte a GRAY8 surface stores for v, and the value it reads for b. */
static uint8_t encode8(float v)
{
    uint8_t b = 0;
    pc_write_sample(&b, 1, PC_SAMPLE_TYPE_U8, 0, v);
    return b;
}
static float decode8(uint8_t b)
{
    return pc_read_sample(&b, 1, PC_SAMPLE_TYPE_U8, 0);
}

/* A shared tile of the borrowed surface (reference counts are atomic). */
static pc_raster_tile *share(const pc_raster_tile *t)
{
    return pc_raster_tile_retain((pc_raster_tile *)(uintptr_t)t);
}

/* ---- ellipse ---- */

typedef struct ellipse {
    float cx, cy, rx, ry;
} ellipse;

static bool e_inside(const ellipse *e, float x, float y)
{
    float dx = (x - e->cx) / e->rx, dy = (y - e->cy) / e->ry;
    return dx * dx + dy * dy <= 1.0f;
}

/* Every point the coverage samples is inside: the four corners (anti-aliased) or the centre. */
static bool e_full(const ellipse *e, int32_t x, int32_t y, bool aa)
{
    float fx = (float)x, fy = (float)y;
    if (!aa) {
        return e_inside(e, fx + 0.5f, fy + 0.5f);
    }
    return e_inside(e, fx, fy) && e_inside(e, fx + 1.0f, fy) && e_inside(e, fx, fy + 1.0f) &&
           e_inside(e, fx + 1.0f, fy + 1.0f);
}

/* Coverage in sixteenths: 16 when full, else (anti-aliased) the 4x4 sub-samples inside. */
static uint8_t e_coverage(const ellipse *e, int32_t x, int32_t y, bool aa)
{
    float fx = (float)x, fy = (float)y;
    uint8_t n = 0, i;
    if (e_full(e, x, y, aa)) {
        return 16;
    }
    if (!aa) {
        return 0;
    }
    for (i = 0; i < 16; i++) {
        if (e_inside(e, fx + ((float)(i % 4) + 0.5f) / 4.0f, fy + ((float)(i / 4) + 0.5f) / 4.0f)) {
            n++;
        }
    }
    return n;
}

/* One row: nothing outside lo..hi, fully selected in full_lo..full_hi, edges in between. */
typedef struct row_span {
    int32_t lo, full_lo, full_hi, hi;
} row_span;

static int32_t px_of(double v, pc_rect cut)
{
    return pc_f64_to_i32(pc_clampd(v, (double)cut.x0, (double)cut.x1));
}

static double half_width(double d, double rx, double ry)
{
    double t = 1.0 - (d / ry) * (d / ry);
    return t >= 0.0 ? rx * sqrt(t) : -1.0;
}

/* Bounds row y from the exact (f64) ellipse, widened by a margin covering the f32 tests'
 * rounding, then checked with the exact tests; any doubt evaluates the whole row. */
static row_span span_of(const ellipse *e, int32_t y, pc_rect cut, bool aa)
{
    row_span exact = {cut.x0, cut.x1, cut.x1, cut.x1}, r;
    double cx = (double)e->cx, cy = (double)e->cy, rx = (double)e->rx, ry = (double)e->ry;
    double ya = (double)y, yb = (double)y + 1.0, near, far, outer, inner, m;
    bool ok;
    near = cy < ya ? ya - cy : fmax(cy - yb, 0.0);
    far = fmax(fabs(ya - cy), fabs(yb - cy));
    outer = fmax(half_width(near, rx, ry), 0.0);
    inner = half_width(far, rx, ry);
    /* Near the top and bottom a rounding error d in the test moves the edge by up to rx sqrt(d). */
    m = 2.0 + rx * 1e-3;
    if (!(isfinite(outer) && isfinite(inner) && isfinite(m))) {
        return exact;
    }
    r.lo = px_of(floor(cx - outer - m), cut);
    r.hi = px_of(floor(cx + outer + m) + 1.0, cut);
    r.full_lo = px_of(ceil(cx - inner + m), cut);
    r.full_hi = px_of(floor(cx + inner - m), cut);
    if (inner < 0.0 || r.full_lo >= r.full_hi) {
        r.full_lo = r.hi;
        r.full_hi = r.hi;
    }
    ok = (r.lo <= cut.x0 || e_coverage(e, r.lo - 1, y, aa) == 0) &&
         (r.hi >= cut.x1 || e_coverage(e, r.hi, y, aa) == 0) &&
         (r.full_lo >= r.full_hi || (e_full(e, r.full_lo, y, aa) && e_full(e, r.full_hi - 1, y, aa)));
    return ok ? r : exact;
}

typedef struct ell_ctx {
    ellipse e;
    pc_rect cut;
    bool aa;
    row_span *rows;
    const pc_surface *s;
    pc_tile *coords;
    pc_tile_entry *built; /* tile NULL: nothing there */
    const pc_raster_tile *full;
    uint8_t enc[17], one;
} ell_ctx;

static pc_status ell_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    ell_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        c->rows[i] = span_of(&c->e, c->cut.y0 + (int32_t)i, c->cut, c->aa);
    }
    return PC_OK;
}

static int32_t max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}
static int32_t min32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static pc_status ell_tiles(void *ctx, size_t chunk, size_t begin, size_t end)
{
    ell_ctx *c = ctx;
    size_t k;
    (void)chunk;
    for (k = begin; k < end; k++) {
        pc_tile tc = c->coords[k];
        pc_rect rect = pc_tile_rect(tc), tr = pc_rect_intersect(rect, c->cut);
        pc_raster_tile *tile = NULL;
        uint8_t *d = NULL;
        int32_t y, x;
        bool whole = pc_rect_eq(tr, rect);
        c->built[k].coord = tc;
        c->built[k].tile = NULL;
        for (y = tr.y0; y < tr.y1 && whole; y++) {
            const row_span *sp = &c->rows[y - c->cut.y0];
            whole = sp->full_lo <= tr.x0 && sp->full_hi >= tr.x1;
        }
        if (whole) {
            c->built[k].tile = share(c->full);
            continue;
        }
        for (y = tr.y0; y < tr.y1; y++) {
            const row_span *sp = &c->rows[y - c->cut.y0];
            size_t row = (size_t)(y - rect.y0) * TS;
            int32_t a = max32(sp->full_lo, tr.x0), b = min32(sp->full_hi, tr.x1);
            int32_t lists[2][2] = {{max32(sp->lo, tr.x0), min32(sp->full_lo, tr.x1)},
                                   {max32(sp->full_hi, tr.x0), min32(sp->hi, tr.x1)}};
            int li;
            if (a < b) {
                if (d == NULL) {
                    float zero = 0.0f;
                    if (pc_surface_solid_tile(c->s, &zero, 1, &tile, NULL) != PC_OK ||
                        (d = pc_raster_tile_make_mut(&tile)) == NULL) {
                        pc_raster_tile_release(tile);
                        return PC_ERR_NOMEM;
                    }
                }
                memset(d + row + (size_t)(a - rect.x0), c->one, (size_t)(b - a));
            }
            for (li = 0; li < 2; li++) {
                for (x = lists[li][0]; x < lists[li][1]; x++) {
                    uint8_t n = e_coverage(&c->e, x, y, c->aa);
                    if (n == 0) {
                        continue;
                    }
                    if (d == NULL) {
                        float zero = 0.0f;
                        if (pc_surface_solid_tile(c->s, &zero, 1, &tile, NULL) != PC_OK ||
                            (d = pc_raster_tile_make_mut(&tile)) == NULL) {
                            pc_raster_tile_release(tile);
                            return PC_ERR_NOMEM;
                        }
                    }
                    d[row + (size_t)(x - rect.x0)] = c->enc[n];
                }
            }
        }
        c->built[k].tile = tile;
    }
    return PC_OK;
}

/* Collects the tiles covering r (row by row). */
static pc_status tile_coords(pc_rect r, pc_tile **out, size_t *n, pc_error *err)
{
    pc_tile first, last, tc, *v;
    size_t cnt, nx, ny, i = 0;
    *out = NULL;
    *n = 0;
    if (!pc_rect_tile_range(r, &first, &last)) {
        return PC_OK;
    }
    nx = (size_t)((int64_t)last.tx - first.tx + 1);
    ny = (size_t)((int64_t)last.ty - first.ty + 1);
    if (!pc_size_mul(nx, ny, &cnt) || cnt > PC_SURFACE_MAX_TILE_SPAN) {
        return pc_fail(err, PC_ERR_LIMIT, "selection spans too many tiles");
    }
    v = pc_malloc_array(cnt, sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a selection");
    }
    for (tc.ty = first.ty; tc.ty <= last.ty; tc.ty++) {
        for (tc.tx = first.tx; tc.tx <= last.tx; tc.tx++) {
            v[i++] = tc;
        }
    }
    *out = v;
    *n = cnt;
    return PC_OK;
}

/* Puts the non-empty built tiles into s, consuming every entry. */
static pc_status put_built(pc_surface *s, pc_tile_entry *built, size_t n, pc_error *err)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++) {
        if (built[i].tile != NULL) {
            built[k++] = built[i];
        }
    }
    return pc_surface_put_tiles(s, built, k, err);
}

pc_status pc_algo_selection_ellipse_surface(pc_rect r, pc_rect clip, bool anti_alias, pc_surface *out,
                                            pc_error *err)
{
    pc_surface s;
    ell_ctx c;
    pc_raster_tile *full = NULL;
    size_t nt = 0, nrows, i;
    float one = 1.0f;
    pc_status st;
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    memset(&c, 0, sizeof c);
    c.cut = pc_rect_intersect(r, clip);
    if (pc_rect_is_empty(c.cut)) {
        *out = s;
        return PC_OK;
    }
    c.e.cx = (float)((int64_t)r.x0 + (int64_t)r.x1) / 2.0f;
    c.e.cy = (float)((int64_t)r.y0 + (int64_t)r.y1) / 2.0f;
    c.e.rx = (float)pc_rect_width(r) / 2.0f;
    c.e.ry = (float)pc_rect_height(r) / 2.0f;
    c.aa = anti_alias;
    c.s = &s;
    for (i = 0; i < 17; i++) {
        c.enc[i] = encode8((float)i / 16.0f);
    }
    c.one = encode8(1.0f);
    nrows = pc_rect_height(c.cut);
    if ((st = tile_coords(c.cut, &c.coords, &nt, err)) != PC_OK) {
        return st;
    }
    c.rows = pc_malloc_array(nrows, sizeof *c.rows);
    c.built = pc_calloc_array(nt ? nt : 1, sizeof *c.built);
    if (c.rows == NULL || c.built == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for an elliptical selection");
        goto done;
    }
    if ((st = pc_surface_solid_tile(&s, &one, 1, &full, err)) != PC_OK) {
        goto done;
    }
    c.full = full;
    (void)pc_algo_par_for(nrows, 16, ell_rows, &c);
    st = pc_algo_par_for(nt, 1, ell_tiles, &c);
    if (st != PC_OK) {
        st = pc_fail(err, st, "out of memory for an elliptical selection");
        goto done;
    }
    st = put_built(&s, c.built, nt, err);
    nt = 0;
done:
    if (c.built != NULL) {
        for (i = 0; i < nt; i++) {
            pc_raster_tile_release(c.built[i].tile);
        }
    }
    free(c.built);
    free(c.rows);
    free(c.coords);
    pc_raster_tile_release(full);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}

/* ---- combining two selections ---- */

typedef struct comb_ctx {
    const pc_surface *a, *b, *s;
    pc_rect area;
    const uint8_t *lut;
    uint8_t da, db;
    bool keeps_a, keeps_b;
    const pc_raster_tile *full;
    pc_tile *coords;
    pc_tile_entry *built;
} comb_ctx;

static uint8_t pair(const comb_ctx *c, uint8_t x, uint8_t y)
{
    return c->lut[(size_t)x << 8 | y];
}

static pc_status comb_tiles(void *ctx, size_t chunk, size_t begin, size_t end)
{
    comb_ctx *c = ctx;
    size_t k;
    (void)chunk;
    for (k = begin; k < end; k++) {
        pc_tile tc = c->coords[k];
        pc_rect rect = pc_tile_rect(tc), tr = pc_rect_intersect(rect, c->area);
        const pc_raster_tile *ta = pc_surface_tile(c->a, tc), *tb = pc_surface_tile(c->b, tc), *pass = NULL;
        pc_raster_tile *tile = NULL;
        uint8_t *d, lo, hi = 0;
        size_t x0, x1, ly, i;
        c->built[k].coord = tc;
        c->built[k].tile = NULL;
        if (pc_rect_eq(tr, rect)) {
            if (ta != NULL && tb == NULL && c->keeps_a) {
                pass = ta;
            } else if (ta == NULL && tb != NULL && c->keeps_b) {
                pass = tb;
            } else if (ta == NULL && tb == NULL && pair(c, c->da, c->db) == 0) {
                continue;
            }
            if (pass != NULL) {
                const uint8_t *pb = pc_raster_tile_bytes(pass);
                size_t len = pc_raster_tile_len(pass);
                for (i = 0; i < len && pb[i] == 0; i++) {
                }
                if (i < len) {
                    c->built[k].tile = share(pass);
                }
                continue;
            }
        }
        {
            float zero = 0.0f;
            if (pc_surface_solid_tile(c->s, &zero, 1, &tile, NULL) != PC_OK ||
                (d = pc_raster_tile_make_mut(&tile)) == NULL) {
                pc_raster_tile_release(tile);
                return PC_ERR_NOMEM;
            }
        }
        lo = pc_rect_eq(tr, rect) ? UINT8_MAX : 0;
        x0 = (size_t)(tr.x0 - rect.x0);
        x1 = (size_t)(tr.x1 - rect.x0);
        for (ly = (size_t)(tr.y0 - rect.y0); ly < (size_t)(tr.y1 - rect.y0); ly++) {
            const uint8_t *sa = ta != NULL ? pc_raster_tile_bytes(ta) + ly * TS : NULL;
            const uint8_t *sb = tb != NULL ? pc_raster_tile_bytes(tb) + ly * TS : NULL;
            for (i = x0; i < x1; i++) {
                uint8_t v = pair(c, sa != NULL ? sa[i] : c->da, sb != NULL ? sb[i] : c->db);
                d[ly * TS + i] = v;
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
        }
        if (hi == 0) {
            pc_raster_tile_release(tile);
        } else if (lo == UINT8_MAX) {
            pc_raster_tile_release(tile);
            c->built[k].tile = share(c->full);
        } else {
            c->built[k].tile = tile;
        }
    }
    return PC_OK;
}

pc_status pc_algo_selection_combine_surfaces(const pc_surface *a, const pc_surface *b, pc_rect area,
                                             pc_algo_combine_fn f, void *fctx, pc_surface *out, pc_error *err)
{
    pc_surface s;
    comb_ctx c;
    uint8_t *lut = NULL;
    pc_raster_tile *full = NULL;
    size_t nt = 0, i;
    float one = 1.0f;
    pc_status st = PC_OK;
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    if (pc_rect_is_empty(area)) {
        *out = s;
        return PC_OK;
    }
    if (!pc_pixel_format_eq(pc_surface_format(a), PC_PIXEL_FORMAT_GRAY8) ||
        !pc_pixel_format_eq(pc_surface_format(b), PC_PIXEL_FORMAT_GRAY8)) {
        float *ra = NULL, *rb = NULL;
        size_t la = 0, lb = 0;
        if ((st = pc_surface_read_region(a, area, &ra, &la, err)) == PC_OK &&
            (st = pc_surface_read_region(b, area, &rb, &lb, err)) == PC_OK) {
            size_t n = la < lb ? la : lb;
            for (i = 0; i < n; i++) {
                ra[i] = f(fctx, ra[i], rb[i]);
            }
            if (n == (size_t)pc_rect_width(area) * pc_rect_height(area)) {
                st = pc_surface_write_region(&s, area, ra, n, err);
                pc_surface_prune(&s);
            }
        }
        free(ra);
        free(rb);
        if (st != PC_OK) {
            pc_surface_fini(&s);
            return st;
        }
        *out = s;
        return PC_OK;
    }
    memset(&c, 0, sizeof c);
    lut = malloc(65536);
    if (lut == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory combining selections");
    }
    for (i = 0; i < 65536; i++) {
        lut[i] = encode8(f(fctx, decode8((uint8_t)(i >> 8)), decode8((uint8_t)i)));
    }
    c.a = a;
    c.b = b;
    c.s = &s;
    c.area = area;
    c.lut = lut;
    c.da = pc_surface_default_bytes(a)[0];
    c.db = pc_surface_default_bytes(b)[0];
    c.keeps_a = c.keeps_b = true;
    for (i = 0; i < 256; i++) {
        c.keeps_a &= pair(&c, (uint8_t)i, c.db) == i;
        c.keeps_b &= pair(&c, c.da, (uint8_t)i) == i;
    }
    if ((st = tile_coords(area, &c.coords, &nt, err)) != PC_OK) {
        goto done;
    }
    c.built = pc_calloc_array(nt ? nt : 1, sizeof *c.built);
    if (c.built == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory combining selections");
        goto done;
    }
    if ((st = pc_surface_solid_tile(&s, &one, 1, &full, err)) != PC_OK) {
        goto done;
    }
    c.full = full;
    st = pc_algo_par_for(nt, 1, comb_tiles, &c);
    if (st != PC_OK) {
        st = pc_fail(err, st, "out of memory combining selections");
        goto done;
    }
    st = put_built(&s, c.built, nt, err);
    nt = 0;
done:
    if (c.built != NULL) {
        for (i = 0; i < nt; i++) {
            pc_raster_tile_release(c.built[i].tile);
        }
    }
    free(c.built);
    free(c.coords);
    free(lut);
    pc_raster_tile_release(full);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}
