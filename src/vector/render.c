/* Compiling paths into rasterisers and rendering coverage, vector masks and shape layers into
 * sparse tiled surfaces (port of crates/vector/src/lib.rs). Only tiles that differ from the
 * surface default are allocated. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"
#include "raster/pixel.h"
#include "vector/internal.h"

/* ---- compiling ---- */

pc_status pc_vec_fill_rasterizer(const pc_path *path, double tol, pc_vec_rasterizer *out, pc_error *err)
{
    pc_vec_rasterizer r;
    pc_index_range *comps = NULL;
    size_t ncomps = 0;
    size_t c;
    pc_status st = pc_path_components(path, &comps, &ncomps, err);
    if (st != PC_OK) {
        return st;
    }
    pc_vec_rasterizer_init(&r, path->inverted);
    for (c = 0; c < ncomps && st == PC_OK; c++) {
        size_t n = comps[c].end - comps[c].start;
        pc_vec_polyline *lines = pc_calloc_array(n, sizeof *lines);
        pc_vec_poly *polys = pc_calloc_array(n, sizeof *polys);
        size_t i;
        size_t done = 0;
        if (lines == NULL || polys == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        for (i = 0; i < n && st == PC_OK; i++) {
            st = pc_vec_flatten_subpath(&path->subpaths[comps[c].start + i], tol, &lines[i], err);
            if (st == PC_OK) {
                done++;
                polys[i].pts = lines[i].pts;
                polys[i].len = lines[i].len;
            }
        }
        if (st == PC_OK) {
            st = pc_vec_rasterizer_add_component(&r, polys, n, pc_path_effective_op(path, comps[c].start),
                                                 path->fill_rule, err);
        }
        free(polys);
        if (lines != NULL) {
            pc_vec_polylines_free(lines, done);
        }
    }
    free(comps);
    if (st != PC_OK) {
        pc_vec_rasterizer_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

pc_status pc_vec_stroke_rasterizer(const pc_path *path, const pc_vec_stroke_style *style, double tol,
                                   pc_vec_rasterizer *out, pc_error *err)
{
    /* Stroke outlines tolerate a coarser flattening than fills (the offset hides chord error). */
    double ftol = pc_rs_max_k(pc_rs_min(tol * 4.0, pc_rs_max_k(style->width, 0.01) * 0.1), 1e-3);
    pc_vec_polyline *lines = NULL;
    size_t nlines = 0;
    pc_vec_polys polys = {NULL, 0, 0};
    pc_vec_rasterizer r;
    pc_status st = pc_vec_flatten_path(path, ftol, &lines, &nlines, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_vec_stroke_polygons(lines, nlines, style, tol, &polys, err);
    pc_vec_polylines_free(lines, nlines);
    if (st != PC_OK) {
        pc_vec_polys_fini(&polys);
        return st;
    }
    pc_vec_rasterizer_init(&r, false);
    st = pc_vec_rasterizer_add_component(&r, polys.v, polys.len, PC_PATH_COMBINE, PC_FILL_RULE_NONZERO, err);
    pc_vec_polys_fini(&polys);
    if (st != PC_OK) {
        pc_vec_rasterizer_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

pc_status pc_vec_path_coverage(const pc_path *path, pc_rect rect, float **out, size_t *len, pc_error *err)
{
    pc_vec_rasterizer r;
    pc_status st = pc_vec_fill_rasterizer(path, PC_VEC_DEFAULT_TOLERANCE, &r, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_vec_rasterizer_render(&r, rect, out, len, err);
    pc_vec_rasterizer_fini(&r);
    return st;
}

/* ---- vector masks ---- */

pc_status pc_vec_compiled_mask_new(const pc_vector_mask *m, pc_vec_compiled_mask *out, pc_error *err)
{
    pc_vec_compiled_mask c;
    memset(&c, 0, sizeof c);
    pc_vec_rasterizer_init(&c.raster, false);
    if (!m->enabled) {
        c.constant = 1.0f;
    } else if (pc_path_is_empty(&m->path)) {
        /* An empty Photoshop vector mask reveals all, unlike an empty fill path. */
        c.constant = m->path.inverted ? 0.0f : 1.0f;
    } else {
        pc_status st = pc_vec_fill_rasterizer(&m->path, PC_VEC_DEFAULT_TOLERANCE, &c.raster, err);
        if (st != PC_OK) {
            return st;
        }
        c.is_path = true;
    }
    if (m->enabled && m->density < 1.0f) {
        c.has_density = true;
        c.density = pc_clampf(m->density, 0.0f, 1.0f);
    }
    *out = c;
    return PC_OK;
}

void pc_vec_compiled_mask_fini(pc_vec_compiled_mask *m)
{
    if (m == NULL) {
        return;
    }
    pc_vec_rasterizer_fini(&m->raster);
    memset(m, 0, sizeof *m);
}

static pc_status pixel_count(pc_rect rect, size_t *n, pc_error *err)
{
    size_t w = pc_rect_width(rect);
    size_t h = pc_rect_height(rect);
    if (!pc_size_mul(w, h, n) || *n > PC_VEC_MAX_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "vector: render area of %zux%zu pixels is too large", w, h);
    }
    return PC_OK;
}

/* Rasteriser coverage over rect on the calling thread only. */
static pc_status render_sequential(const pc_vec_rasterizer *r, pc_rect rect, float **out, size_t *len,
                                   pc_error *err)
{
    size_t n;
    float *buf;
    pc_status st = pixel_count(rect, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n == 0) {
        *out = NULL;
        *len = 0;
        return PC_OK;
    }
    buf = pc_calloc_array(n, sizeof *buf);
    if (buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    st = pc_vec_rasterizer_render_into(r, rect, buf, n, err);
    if (st != PC_OK) {
        free(buf);
        return st;
    }
    *out = buf;
    *len = n;
    return PC_OK;
}

static pc_status compiled_mask_render(const pc_vec_compiled_mask *m, pc_rect rect, bool sequential,
                                      float **out, size_t *len, pc_error *err)
{
    float *v = NULL;
    size_t n = 0;
    size_t i;
    pc_status st;
    if (m->is_path) {
        st = sequential ? render_sequential(&m->raster, rect, &v, &n, err)
                        : pc_vec_rasterizer_render(&m->raster, rect, &v, &n, err);
        if (st != PC_OK) {
            return st;
        }
    } else {
        st = pixel_count(rect, &n, err);
        if (st != PC_OK) {
            return st;
        }
        if (n > 0) {
            v = pc_malloc_array(n, sizeof *v);
            if (v == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
            }
            for (i = 0; i < n; i++) {
                v[i] = m->constant;
            }
        }
    }
    if (m->has_density) {
        for (i = 0; i < n; i++) {
            v[i] = 1.0f - m->density * (1.0f - v[i]);
        }
    }
    *out = v;
    *len = n;
    return PC_OK;
}

pc_status pc_vec_compiled_mask_render(const pc_vec_compiled_mask *m, pc_rect rect, float **out, size_t *len,
                                      pc_error *err)
{
    return compiled_mask_render(m, rect, false, out, len, err);
}

pc_status pc_vec_compiled_mask_render_sequential(const pc_vec_compiled_mask *m, pc_rect rect, float **out,
                                                 size_t *len, pc_error *err)
{
    return compiled_mask_render(m, rect, true, out, len, err);
}

pc_status pc_vec_mask_values(const pc_vector_mask *m, pc_rect rect, float **out, size_t *len, pc_error *err)
{
    pc_vec_compiled_mask c;
    pc_status st = pc_vec_compiled_mask_new(m, &c, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_vec_compiled_mask_render(&c, rect, out, len, err);
    pc_vec_compiled_mask_fini(&c);
    return st;
}

/* ---- tiled rendering ---- */

/* One tile-aligned piece of a band. */
typedef struct piece {
    pc_rect r;
    float *data;
    size_t len;
} piece;

typedef struct band_result {
    piece *v;
    size_t len;
    pc_status st;
} band_result;

static void band_result_fini(band_result *b)
{
    size_t i;
    for (i = 0; i < b->len; i++) {
        free(b->v[i].data);
    }
    free(b->v);
    memset(b, 0, sizeof *b);
}

typedef bool (*keep_fn)(const float *px, size_t channels);

/* Splits a band buffer (channels floats per pixel) into tile-aligned pieces, skipping pieces
 * where keep is false for every pixel. Coordinates use 64-bit maths so bands next to INT32_MAX
 * cannot wrap (the reference would loop forever there in release builds). */
static pc_status band_tiles(pc_rect band, const float *data, size_t channels, keep_fn keep, band_result *out,
                            pc_error *err)
{
    size_t w = pc_rect_width(band);
    size_t h = pc_rect_height(band);
    int64_t x = (int64_t)pc_tile_containing(band.x0, band.y0).tx * PC_TILE_SIZE;
    size_t ncols = 0;
    size_t cap = 0;
    while (x < band.x1) {
        int64_t tx0 = x > band.x0 ? x : band.x0;
        int64_t tx1 = x + PC_TILE_SIZE < band.x1 ? x + PC_TILE_SIZE : band.x1;
        size_t tw = (size_t)(tx1 - tx0);
        size_t off = (size_t)(tx0 - band.x0);
        float *buf;
        bool any = false;
        size_t row;
        buf = pc_malloc_array(tw * h * channels, sizeof *buf);
        if (buf == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        for (row = 0; row < h; row++) {
            const float *src = data + (row * w + off) * channels;
            if (!any) {
                size_t i;
                for (i = 0; i < tw; i++) {
                    if (keep(src + i * channels, channels)) {
                        any = true;
                        break;
                    }
                }
            }
            memcpy(buf + row * tw * channels, src, tw * channels * sizeof *buf);
        }
        if (any) {
            if (out->len == cap) {
                void *nv = pc_grow_array(out->v, &cap, out->len + 1, sizeof *out->v);
                if (nv == NULL) {
                    free(buf);
                    return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
                }
                out->v = nv;
            }
            out->v[out->len].r = pc_rect_make((int32_t)tx0, band.y0, (int32_t)tx1, band.y1);
            out->v[out->len].data = buf;
            out->v[out->len].len = tw * h * channels;
            out->len++;
        } else {
            free(buf);
        }
        x += PC_TILE_SIZE;
        ncols++;
    }
    (void)ncols;
    return PC_OK;
}

typedef pc_status (*band_fn)(const void *ctx, pc_rect band, band_result *out, pc_error *err);

typedef struct bands_ctx {
    const pc_rect *bands;
    band_result *res;
    band_fn fn;
    const void *ctx;
} bands_ctx;

static pc_status bands_chunk(void *vctx, size_t chunk, size_t begin, size_t end)
{
    bands_ctx *c = vctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        pc_status st = c->fn(c->ctx, c->bands[i], &c->res[i], NULL);
        if (st != PC_OK) {
            band_result_fini(&c->res[i]);
        }
        c->res[i].st = st;
    }
    return PC_OK;
}

/* Runs fn over the tile-aligned bands of r (in parallel groups) and writes the resulting pieces
 * into s in band order. */
static pc_status render_bands(pc_surface *s, pc_rect r, band_fn fn, const void *ctx, pc_error *err)
{
    int64_t y;
    size_t group;
    pc_rect *bands;
    band_result *res;
    pc_pool *pool = pc_pool_global();
    pc_status st = PC_OK;
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    group = pc_pool_threads(pool) * 2;
    if (group < 2) {
        group = 2;
    }
    bands = pc_malloc_array(group, sizeof *bands);
    res = pc_calloc_array(group, sizeof *res);
    if (bands == NULL || res == NULL) {
        free(bands);
        free(res);
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    y = (int64_t)pc_tile_containing(r.x0, r.y0).ty * PC_TILE_SIZE;
    while (y < r.y1 && st == PC_OK) {
        size_t nb = 0;
        size_t i;
        bands_ctx c;
        /* Next group of bands. */
        while (nb < group && y < r.y1) {
            int64_t y0 = y > r.y0 ? y : r.y0;
            int64_t y1 = y + PC_TILE_SIZE < r.y1 ? y + PC_TILE_SIZE : r.y1;
            bands[nb++] = pc_rect_make(r.x0, (int32_t)y0, r.x1, (int32_t)y1);
            y += PC_TILE_SIZE;
        }
        c.bands = bands;
        c.res = res;
        c.fn = fn;
        c.ctx = ctx;
        (void)pc_pool_for(pool, nb, 1, bands_chunk, &c);
        for (i = 0; i < nb; i++) {
            size_t k;
            if (st == PC_OK && res[i].st != PC_OK) {
                st = res[i].st == PC_ERR_LIMIT ? pc_fail(err, PC_ERR_LIMIT, "vector: render band too large")
                                               : pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
            }
            for (k = 0; k < res[i].len && st == PC_OK; k++) {
                st = pc_surface_write_region(s, res[i].v[k].r, res[i].v[k].data, res[i].v[k].len, err);
            }
            band_result_fini(&res[i]);
        }
    }
    free(bands);
    free(res);
    return st;
}

/* ---- coverage surfaces ---- */

static bool keep_coverage(const float *px, size_t channels)
{
    (void)channels;
    return px[0] > 0.0f;
}

static pc_status coverage_band(const void *ctx, pc_rect band, band_result *out, pc_error *err)
{
    const pc_vec_rasterizer *r = ctx;
    float *cov = NULL;
    size_t n = 0;
    pc_status st = pc_vec_rasterizer_render(r, band, &cov, &n, err);
    if (st != PC_OK) {
        return st;
    }
    st = band_tiles(band, cov, 1, keep_coverage, out, err);
    free(cov);
    return st;
}

pc_status pc_vec_coverage_surface(const pc_vec_rasterizer *r, pc_pixel_format format, pc_rect clip,
                                  pc_surface *out, pc_error *err)
{
    pc_pixel_format fmt;
    pc_surface s;
    pc_rect area = PC_RECT_EMPTY;
    pc_status st;
    fmt.mode = PC_COLOR_MODE_GRAYSCALE;
    fmt.sample = format.sample;
    fmt.alpha = false;
    pc_surface_init(&s, fmt);
    if (pc_vec_rasterizer_is_inverted(r)) {
        area = clip;
    } else {
        pc_rect b;
        if (pc_vec_rasterizer_pixel_bounds(r, &b)) {
            area = pc_rect_intersect(b, clip);
        }
    }
    st = render_bands(&s, area, coverage_band, r, err);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}

/* ---- shape layers ---- */

void pc_vec_compiled_shape_fini(pc_vec_compiled_shape *s)
{
    if (s == NULL) {
        return;
    }
    pc_vec_rasterizer_fini(&s->fill);
    pc_vec_paint_fini(&s->fill_paint);
    pc_vec_rasterizer_fini(&s->stroke);
    pc_vec_paint_fini(&s->stroke_paint);
    pc_vec_rasterizer_fini(&s->align);
    memset(s, 0, sizeof *s);
}

pc_status pc_vec_compiled_shape_new(const pc_shape_layer *shape, double tol, pc_rect canvas,
                                    pc_vec_compiled_shape *out, pc_error *err)
{
    pc_vec_compiled_shape cs;
    double pb[4] = {0.0, 0.0, 0.0, 0.0};
    double cb[4];
    pc_vec_rasterizer area;
    bool all_closed = true;
    size_t i;
    pc_status st;
    memset(&cs, 0, sizeof cs);
    pc_vec_rasterizer_init(&cs.fill, false);
    pc_vec_rasterizer_init(&cs.stroke, false);
    pc_vec_rasterizer_init(&cs.align, false);
    if (!pc_path_control_bounds(&shape->path, pb)) {
        memset(pb, 0, sizeof pb);
    }
    cb[0] = (double)canvas.x0;
    cb[1] = (double)canvas.y0;
    cb[2] = (double)canvas.x1;
    cb[3] = (double)canvas.y1;
    st = pc_vec_fill_rasterizer(&shape->path, tol, &area, err);
    if (st != PC_OK) {
        return st;
    }
    if (shape->has_fill) {
        cs.has_bounds = pc_vec_rasterizer_pixel_bounds(&area, &cs.bounds);
        st = pc_vec_rasterizer_clone(&area, &cs.fill, err);
        if (st == PC_OK) {
            st = pc_vec_paint_from_fill(&shape->fill, pb, cb, &cs.fill_paint, err);
        }
        cs.has_fill = true;
    }
    for (i = 0; i < shape->path.nsubpaths; i++) {
        if (!shape->path.subpaths[i].closed) {
            all_closed = false;
        }
    }
    if (st == PC_OK && shape->has_stroke && shape->stroke.width > 0.0f && shape->stroke.opacity > 0.0f) {
        const pc_shape_stroke *s = &shape->stroke;
        pc_vec_stroke_style sty;
        pc_rect sb;
        bool has_sb;
        st = pc_vec_stroke_style_from(s, &sty, err);
        if (st == PC_OK) {
            if (all_closed && s->align != PC_STROKE_ALIGN_CENTER) {
                /* Inside/outside strokes: a centred stroke twice as wide, clipped by the fill. */
                sty.width *= 2.0;
                st = pc_vec_rasterizer_clone(&area, &cs.align, err);
                cs.has_align = true;
                cs.align_inside = s->align == PC_STROKE_ALIGN_INSIDE;
            }
            if (st == PC_OK) {
                st = pc_vec_stroke_rasterizer(&shape->path, &sty, tol, &cs.stroke, err);
            }
            pc_vec_stroke_style_fini(&sty);
        }
        if (st == PC_OK) {
            has_sb = pc_vec_rasterizer_pixel_bounds(&cs.stroke, &sb);
            if (cs.has_bounds && has_sb) {
                cs.bounds = pc_rect_union(cs.bounds, sb);
            } else if (has_sb) {
                cs.bounds = sb;
                cs.has_bounds = true;
            }
            st = pc_vec_paint_from_fill(&s->paint, pb, cb, &cs.stroke_paint, err);
            cs.stroke_opacity = pc_clampf(s->opacity, 0.0f, 1.0f);
            cs.has_stroke = true;
        }
    }
    pc_vec_rasterizer_fini(&area);
    if (st != PC_OK) {
        pc_vec_compiled_shape_fini(&cs);
        return st;
    }
    if (shape->path.inverted && shape->has_fill) {
        cs.bounds = pc_rect_make(INT32_MIN / 4, INT32_MIN / 4, INT32_MAX / 4, INT32_MAX / 4);
        cs.has_bounds = true;
    }
    *out = cs;
    return PC_OK;
}

bool pc_vec_compiled_shape_bounds(const pc_vec_compiled_shape *s, pc_rect *out)
{
    if (!s->has_bounds) {
        return false;
    }
    *out = s->bounds;
    return true;
}

pc_status pc_vec_compiled_shape_render_rgba(const pc_vec_compiled_shape *s, pc_rect rect, float (**out)[4],
                                            size_t *n, pc_error *err)
{
    size_t w = pc_rect_width(rect);
    size_t count;
    float (*px)[4] = NULL;
    float *cov = NULL;
    size_t ncov = 0;
    size_t i;
    pc_status st = pixel_count(rect, &count, err);
    if (st != PC_OK) {
        return st;
    }
    if (count == 0) {
        *out = NULL;
        *n = 0;
        return PC_OK;
    }
    px = pc_calloc_array(count, sizeof *px);
    if (px == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    if (s->has_fill) {
        st = pc_vec_rasterizer_render(&s->fill, rect, &cov, &ncov, err);
        if (st != PC_OK) {
            free(px);
            return st;
        }
        for (i = 0; i < count; i++) {
            if (cov[i] > 0.0f) {
                double x = (double)((int64_t)rect.x0 + (int64_t)(i % w)) + 0.5;
                double y = (double)((int64_t)rect.y0 + (int64_t)(i / w)) + 0.5;
                pc_vec_paint_at(&s->fill_paint, x, y, px[i]);
                px[i][3] *= cov[i];
            }
        }
        free(cov);
        cov = NULL;
    }
    if (s->has_stroke) {
        st = pc_vec_rasterizer_render(&s->stroke, rect, &cov, &ncov, err);
        if (st != PC_OK) {
            free(px);
            return st;
        }
        if (s->has_align) {
            float *a = NULL;
            size_t na = 0;
            st = pc_vec_rasterizer_render(&s->align, rect, &a, &na, err);
            if (st != PC_OK) {
                free(cov);
                free(px);
                return st;
            }
            for (i = 0; i < count; i++) {
                cov[i] *= s->align_inside ? a[i] : 1.0f - a[i];
            }
            free(a);
        }
        for (i = 0; i < count; i++) {
            float sp[4];
            float sa;
            float da;
            float al;
            float *o = px[i];
            double x;
            double y;
            if (cov[i] <= 0.0f) {
                continue;
            }
            x = (double)((int64_t)rect.x0 + (int64_t)(i % w)) + 0.5;
            y = (double)((int64_t)rect.y0 + (int64_t)(i / w)) + 0.5;
            pc_vec_paint_at(&s->stroke_paint, x, y, sp);
            sa = sp[3] * cov[i] * s->stroke_opacity;
            da = o[3];
            al = sa + da * (1.0f - sa);
            if (al > 0.0f) {
                int k;
                for (k = 0; k < 3; k++) {
                    o[k] = (sp[k] * sa + o[k] * da * (1.0f - sa)) / al;
                }
            }
            o[3] = al;
        }
        free(cov);
    }
    *out = px;
    *n = count;
    return PC_OK;
}

typedef struct shape_band_ctx {
    const pc_vec_compiled_shape *shape;
    pc_pixel_format format;
} shape_band_ctx;

static bool keep_alpha(const float *px, size_t channels)
{
    return px[channels - 1] > 0.0f;
}

static bool keep_any(const float *px, size_t channels)
{
    size_t i;
    for (i = 0; i < channels; i++) {
        if (px[i] != 0.0f) {
            return true;
        }
    }
    return false;
}

static pc_status shape_band(const void *vctx, pc_rect band, band_result *out, pc_error *err)
{
    const shape_band_ctx *c = vctx;
    size_t ch = pc_pixel_format_channels(c->format);
    float (*rgba)[4] = NULL;
    size_t n = 0;
    float *vals;
    size_t i;
    pc_status st = pc_vec_compiled_shape_render_rgba(c->shape, band, &rgba, &n, err);
    if (st != PC_OK) {
        return st;
    }
    vals = pc_calloc_array(n * ch > 0 ? n * ch : 1, sizeof *vals);
    if (vals == NULL) {
        free(rgba);
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    for (i = 0; i < n; i++) {
        if (rgba[i][3] > 0.0f) {
            float o[PC_RASTER_MAX_CHANNELS];
            size_t m = pc_from_rgba(c->format, rgba[i], o);
            memcpy(vals + i * ch, o, (m < ch ? m : ch) * sizeof *o);
        }
    }
    free(rgba);
    st = band_tiles(band, vals, ch, c->format.alpha ? keep_alpha : keep_any, out, err);
    free(vals);
    return st;
}

pc_status pc_vec_compiled_shape_render(const pc_vec_compiled_shape *s, pc_pixel_format format, pc_rect clip,
                                       pc_surface *out, pc_error *err)
{
    pc_surface surf;
    shape_band_ctx c;
    pc_status st;
    pc_surface_init(&surf, format);
    if (s->has_bounds) {
        c.shape = s;
        c.format = format;
        st = render_bands(&surf, pc_rect_intersect(s->bounds, clip), shape_band, &c, err);
        if (st != PC_OK) {
            pc_surface_fini(&surf);
            return st;
        }
    }
    *out = surf;
    return PC_OK;
}

pc_status pc_vec_render_shape(const pc_shape_layer *shape, pc_pixel_format format, pc_rect clip,
                              pc_surface *out, pc_error *err)
{
    pc_vec_compiled_shape cs;
    pc_status st = pc_vec_compiled_shape_new(shape, PC_VEC_DEFAULT_TOLERANCE, clip, &cs, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_vec_compiled_shape_render(&cs, format, clip, out, err);
    pc_vec_compiled_shape_fini(&cs);
    return st;
}
