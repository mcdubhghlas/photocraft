/* The Patch, Content-Aware Move and Remove tools (port of crates/engine/src/retouch_cmds/{patch,
 * content_aware_move,remove}.rs). Included by retouch_cmds.c after its shared helpers.
 *
 * §7 (ET4): the reference runs these three as cancellable background jobs (crate::jobs::run) with
 * progress; here, following the C §7 precedent set by the filter / smart-filter commands, they run
 * SYNCHRONOUSLY through pc_engine_session_edit. The committed pixels are byte-identical (the worker
 * still computes on a document copy and the result is written in as one undo step); only the
 * progress/cancellation of the reference's job is dropped (the algo interrupt is NULL). The live
 * previews (patch::preview, ContentAwarePreview, heal_region_coarse) are UI-only helpers, not
 * commands, and are not ported here. */

#include "algo/content_aware.h"
#include "algo/remove.h"
#include "algo/resample.h"
#include "algo/transform.h"
#include "transform_cmds.h" /* pc_engine_transform_warp_gray */

/* Largest selection / stroke, in pixels of its bounding box (all three tools). */
#define CA_MAX_PIXELS ((uint64_t)16 << 20)
/* Cap on the sampling margin around the content/stroke. */
#define CA_MAX_MARGIN ((int32_t)512)

/* The pixels a step reads and writes: a layer (has_layer false = the targeted alpha channel / Quick
 * Mask), and whether reading samples the visible composite (Sample All Layers). */
typedef struct ca_target {
    bool has_layer;
    pc_layer_id layer;
    bool all_layers;
} ca_target;

/* The stroke paints layer pixels (not a mask or channel), so other layers can be sampled. */
static bool ca_targets_pixels(yyjson_val *p)
{
    return pc_engine_target_of(p, NULL) == PC_ENGINE_PIXTGT_PIXELS;
}

/* The layer `p` targets (has_layer false for an alpha channel or the Quick Mask), as parse_brush. */
static pc_status ca_target_layer(pc_engine_session *s, yyjson_val *p, bool *has_layer, pc_layer_id *layer,
                                 pc_error *err)
{
    if (pc_engine_is_channel_target(p)) {
        *has_layer = false;
        *layer = 0;
        return PC_OK;
    }
    uint64_t id;
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv != NULL && pc_json_as_u64(lv, &id)) {
        *has_layer = true;
        *layer = (pc_layer_id)id;
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    *has_layer = true;
    *layer = d->active_layer;
    return PC_OK;
}

/* Integer drag offset, rejecting non-finite and absurd values (patch::offset). */
static pc_status ca_offset(const char *cmd, yyjson_val *p, int32_t *odx, int32_t *ody, pc_error *err)
{
    double dx, dy;
    if (!point_p(p, "offset", &dx, &dy)) {
        return pc_engine_err_bad_params(err, cmd, "missing `offset` ([dx, dy])");
    }
    if (!isfinite(dx) || !isfinite(dy) || fabs(dx) >= 1e7 || fabs(dy) >= 1e7) {
        return pc_engine_err_bad_params(err, cmd, "`offset` must be two finite numbers");
    }
    *odx = (int32_t)round(dx);
    *ody = (int32_t)round(dy);
    return PC_OK;
}

/* Optional whole-number param in lo..=hi; absent or null gives default (patch::level). */
static pc_status ca_level(yyjson_val *p, const char *cmd, const char *k, int def, int lo, int hi, int *out,
                          pc_error *err)
{
    yyjson_val *v = pc_json_get(p, k);
    double x;
    if (v == NULL || yyjson_is_null(v)) {
        *out = def;
        return PC_OK;
    }
    if (pc_json_as_f64(v, &x) && isfinite(x) && x >= (double)lo && x <= (double)hi) {
        *out = (int)lround(x);
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, cmd, "`%s` must be a number %d..%d", k, lo, hi);
}

/* Selected area within the canvas: the selection's content bounds, or the whole canvas for an
 * inverted selection (patch::selection_area). */
static pc_rect ca_selection_area(const pc_surface *sel, pc_rect canvas)
{
    float def[PC_RASTER_MAX_CHANNELS];
    pc_surface_default_pixel(sel, def);
    pc_rect b = def[0] > 0.0f ? canvas : pc_surface_content_bounds(sel);
    return pc_rect_intersect(b, canvas);
}

/* Selection coverage over `rect` (row-major, 0..1): *out is w*h owned floats (patch::coverage). */
static pc_status ca_coverage(const pc_surface *sel, pc_rect rect, float **out, size_t *out_n, pc_error *err)
{
    pc_paint_region r = {0};
    pc_status rc = pc_paint_region_read(sel, rect, &r, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t n = pc_paint_region_width(&r) * pc_paint_region_height(&r);
    size_t ch = r.ch > 0 ? r.ch : 1;
    float *c = malloc((n > 0 ? n : 1) * sizeof *c);
    if (c == NULL) {
        pc_paint_region_fini(&r);
        return pc_fail(err, PC_ERR_NOMEM, "coverage");
    }
    for (size_t i = 0; i < n; i++) {
        float v = (r.data != NULL && i * ch < r.len) ? r.data[i * ch] : 0.0f;
        c[i] = clampf(v, 0.0f, 1.0f);
    }
    pc_paint_region_fini(&r);
    *out = c;
    *out_n = n;
    return PC_OK;
}

/* Selection coverage over `rect` as a mask (set wherever it is above zero): *out owned bools. */
static pc_status ca_mask(const pc_surface *sel, pc_rect rect, bool **out, size_t *out_n, pc_error *err)
{
    float *cov = NULL;
    size_t n = 0;
    pc_status rc = ca_coverage(sel, rect, &cov, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool *m = malloc((n > 0 ? n : 1) * sizeof *m);
    if (m == NULL) {
        free(cov);
        return pc_fail(err, PC_ERR_NOMEM, "mask");
    }
    for (size_t i = 0; i < n; i++) {
        m[i] = cov[i] > 0.0f;
    }
    free(cov);
    *out = m;
    *out_n = n;
    return PC_OK;
}

/* The pixels a step works on: the targeted surface, or (Sample All Layers) the visible composite
 * encoded in the target's format (content_aware_move::read). */
static pc_status ca_read(pc_document *doc, ca_target t, yyjson_val *p, pc_rect rect, pc_paint_region *out,
                         pc_error *err)
{
    pc_surface *surf = NULL;
    bool lock = false;
    pc_status rc = pc_engine_target_surface(doc, t.has_layer, t.layer, p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (t.all_layers) {
        return composite_region(doc, false, 0, SAMPLE_ALL, rect, pc_surface_format(surf), out, err);
    }
    return pc_paint_region_read(surf, rect, out, err);
}

/* Format and alpha index of the target surface. */
static pc_status ca_target_format(pc_document *doc, ca_target t, yyjson_val *p, pc_pixel_format *fmt,
                                  bool *has_alpha, size_t *alpha, pc_error *err)
{
    pc_surface *surf = NULL;
    bool lock = false;
    pc_status rc = pc_engine_target_surface(doc, t.has_layer, t.layer, p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    *fmt = pc_surface_format(surf);
    size_t a = SIZE_MAX;
    *has_alpha = pc_paint_alpha_index(*fmt, &a);
    *alpha = a;
    return PC_OK;
}

/* Write `out` over `mask` into the target, replacing the pixels. With the transparency lock, alpha
 * is kept and transparent pixels stay as they are (content_aware_move::replace). Shared with
 * Remove. *rect gets the written rectangle. */
static pc_status ca_replace(pc_document *doc, ca_target t, yyjson_val *p, const pc_paint_region *out,
                            const bool *mask, size_t mask_len, pc_rect *rect, pc_error *err)
{
    pc_surface *surf = NULL;
    bool lock = false;
    pc_status rc = pc_engine_target_surface(doc, t.has_layer, t.layer, p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t ai = SIZE_MAX;
    bool has_a = pc_paint_alpha_index(fmt, &ai);
    pc_paint_region px = {0};
    rc = pc_paint_region_read(surf, out->rect, &px, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t n = out->ch;
    if (px.len != out->len || mask_len * n != px.len) {
        pc_paint_region_fini(&px);
        return pc_engine_err_other(err, "internal error: fill buffers disagree in size");
    }
    size_t count = (n > 0) ? px.len / n : 0;
    for (size_t i = 0; i < count; i++) {
        if (!mask[i]) {
            continue;
        }
        float *dst = &px.data[i * n];
        const float *src = &out->data[i * n];
        if (lock && has_a && ai < n) {
            if (dst[ai] > 0.0f) {
                for (size_t c = 0; c < n; c++) {
                    if (c != ai) {
                        dst[c] = src[c];
                    }
                }
            }
        } else {
            memcpy(dst, src, n * sizeof(float));
        }
    }
    rc = pc_paint_region_write(&px, surf, err);
    *rect = out->rect;
    pc_paint_region_fini(&px);
    return rc;
}

/* ---- the edge band and colour fit (content_aware_move helpers) ---------------------------- */

/* The cells of a w x h mask whose whole (2r+1)^2 neighbourhood (as far as it lies on the grid) is
 * set: a square erosion through a summed-area table (content_aware_move::erode). *out owned bools. */
static pc_status ca_erode(size_t w, size_t h, const bool *m, size_t r, bool **out, pc_error *err)
{
    size_t n = w * h;
    bool *o = malloc((n > 0 ? n : 1) * sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "erode");
    }
    if (r == 0) {
        memcpy(o, m, n * sizeof(bool));
        *out = o;
        return PC_OK;
    }
    size_t w1 = w + 1;
    uint32_t *sat = calloc((w1 * (h + 1)) > 0 ? w1 * (h + 1) : 1, sizeof(uint32_t));
    if (sat == NULL) {
        free(o);
        return pc_fail(err, PC_ERR_NOMEM, "erode sat");
    }
    for (size_t y = 0; y < h; y++) {
        uint32_t row = 0;
        for (size_t x = 0; x < w; x++) {
            row += m[y * w + x] ? 1u : 0u;
            sat[(y + 1) * w1 + x + 1] = sat[y * w1 + x + 1] + row;
        }
    }
    for (size_t y = 0; y < h; y++) {
        size_t y0 = y > r ? y - r : 0, y1 = (y + r + 1 < h) ? y + r + 1 : h;
        for (size_t x = 0; x < w; x++) {
            if (!m[y * w + x]) {
                o[y * w + x] = false;
                continue;
            }
            size_t x0 = x > r ? x - r : 0, x1 = (x + r + 1 < w) ? x + r + 1 : w;
            uint32_t set = sat[y1 * w1 + x1] + sat[y0 * w1 + x0] - sat[y0 * w1 + x1] - sat[y1 * w1 + x0];
            o[y * w + x] = (size_t)set == (x1 - x0) * (y1 - y0);
        }
    }
    free(sat);
    *out = o;
    return PC_OK;
}

/* Width of the edge band re-synthesised at the new place (content_aware_move::band_width). */
static size_t ca_band_width(int structure, uint32_t min_side)
{
    float side = (float)min_side;
    float w;
    if (structure <= 1) {
        w = side * 0.3f > 12.0f ? side * 0.3f : 12.0f;
    } else if (structure == 2) {
        w = side * 0.18f > 9.0f ? side * 0.18f : 9.0f;
    } else {
        w = 6.0f;
    }
    size_t r = (size_t)lroundf(w);
    return r < 128 ? r : 128;
}

/* A ring of `w` pixels just outside `m` within the grid (content_aware_move::outer_ring). */
static pc_status ca_outer_ring(size_t gw, size_t gh, const bool *m, size_t w, bool **out, pc_error *err)
{
    size_t n = gw * gh;
    bool *inside = malloc((n > 0 ? n : 1) * sizeof *inside);
    if (inside == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "ring");
    }
    for (size_t i = 0; i < n; i++) {
        inside[i] = !m[i];
    }
    bool *er = NULL;
    pc_status rc = ca_erode(gw, gh, inside, w, &er, err);
    free(inside);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < n; i++) {
        er[i] = !er[i] && !m[i];
    }
    *out = er;
    return PC_OK;
}

/* FillOptions: the fill's own colour adaptation, seeded so the result is reproducible
 * (content_aware_move::fill_options). */
static pc_algo_ca_fill_options ca_fill_options(void)
{
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    o.seed = 0x00ca4e00u;
    return o;
}

/* Content landing at a window: the content's middle is copied, its level fitted to the new
 * surroundings by `color`, and a band inside the selection's edge is re-synthesised from the
 * surroundings (content_aware_move::land). Writes the result into the target. *dmg = the rectangle
 * it changed. `selected`, `avoid` and `content` all cover `win` (w*h). */
static pc_status ca_land(pc_document *doc, ca_target t, yyjson_val *p, pc_rect win, const bool *selected,
                         const pc_paint_region *content, const bool *avoid, int structure, int color,
                         pc_rect *dmg, pc_error *err)
{
    size_t w = pc_rect_width(win), h = pc_rect_height(win), hw = w * h;
    pc_paint_region here = {0};
    pc_status rc = ca_read(doc, t, p, win, &here, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t n = here.ch;
    /* Bounding box of the selected pixels. */
    size_t sx0 = w, sy0 = h, sx1 = 0, sy1 = 0;
    for (size_t i = 0; i < hw; i++) {
        if (!selected[i]) {
            continue;
        }
        size_t x = i % (w > 0 ? w : 1), y = i / (w > 0 ? w : 1);
        if (x < sx0) sx0 = x;
        if (y < sy0) sy0 = y;
        if (x + 1 > sx1) sx1 = x + 1;
        if (y + 1 > sy1) sy1 = y + 1;
    }
    uint32_t sw = (uint32_t)(sx1 > sx0 ? sx1 - sx0 : 0);
    uint32_t sh = (uint32_t)(sy1 > sy0 ? sy1 - sy0 : 0);
    uint32_t side = sw < sh ? sw : sh;
    bool *core = NULL, *ring = NULL;
    rc = ca_erode(w, h, selected, ca_band_width(structure, side), &core, err);
    if (rc != PC_OK) {
        pc_paint_region_fini(&here);
        return rc;
    }
    rc = ca_outer_ring(w, h, selected, 6, &ring, err);
    if (rc != PC_OK) {
        pc_paint_region_fini(&here);
        free(core);
        return rc;
    }
    /* Colour: the content's surroundings against the new ones, both just outside the selection. */
    size_t ringc = 0;
    for (size_t i = 0; i < hw; i++) {
        if (ring[i]) {
            ringc++;
        }
    }
    float *from = malloc((ringc * n > 0 ? ringc * n : 1) * sizeof *from);
    float *to = malloc((ringc * n > 0 ? ringc * n : 1) * sizeof *to);
    float (*gb)[2] = malloc((n > 0 ? n : 1) * sizeof *gb);
    if (from == NULL || to == NULL || gb == NULL) {
        pc_paint_region_fini(&here);
        free(core);
        free(ring);
        free(from);
        free(to);
        free(gb);
        return pc_fail(err, PC_ERR_NOMEM, "colour fit");
    }
    for (size_t i = 0, k = 0; i < hw; i++) {
        if (!ring[i]) {
            continue;
        }
        memcpy(&from[k * n], &content->data[i * n], n * sizeof(float));
        memcpy(&to[k * n], &here.data[i * n], n * sizeof(float));
        k++;
    }
    rc = pc_algo_ca_level_transform(n, from, ringc * n, to, ringc * n, pc_algo_ca_color_amount((uint8_t)color),
                                    gb, err);
    free(from);
    free(to);
    pc_pixel_format fmt;
    bool has_a = false;
    size_t alpha = SIZE_MAX;
    if (rc == PC_OK) {
        rc = ca_target_format(doc, t, p, &fmt, &has_a, &alpha, err);
    }
    if (rc != PC_OK) {
        pc_paint_region_fini(&here);
        free(core);
        free(ring);
        free(gb);
        return rc;
    }
    free(ring);
    /* `here` becomes the working buffer: the copied/fitted content over the kept core. */
    for (size_t i = 0; i < hw; i++) {
        if (!core[i]) {
            continue;
        }
        for (size_t c = 0; c < n; c++) {
            float v = content->data[i * n + c];
            here.data[i * n + c] = (has_a && c == alpha) ? v : gb[c][0] * v + gb[c][1];
        }
    }
    free(gb);
    clamp_samples(fmt, here.data, here.len);
    /* Structure: the band between the kept core and the selection's edge is re-synthesised. */
    bool *band = malloc((hw > 0 ? hw : 1) * sizeof *band);
    if (band == NULL) {
        pc_paint_region_fini(&here);
        free(core);
        return pc_fail(err, PC_ERR_NOMEM, "band");
    }
    bool any_band = false;
    for (size_t i = 0; i < hw; i++) {
        band[i] = selected[i] && !core[i];
        any_band = any_band || band[i];
    }
    free(core);
    if (any_band) {
        bool *allowed = malloc((hw > 0 ? hw : 1) * sizeof *allowed);
        if (allowed == NULL) {
            pc_paint_region_fini(&here);
            free(band);
            return pc_fail(err, PC_ERR_NOMEM, "allowed");
        }
        for (size_t i = 0; i < hw; i++) {
            allowed[i] = !avoid[i];
        }
        pc_algo_ca_fill_options opts = ca_fill_options();
        opts.color_adaptation =
            color == 0 ? pc_algo_ca_color_level("default") : pc_algo_ca_color_amount((uint8_t)color);
        float *filled = NULL;
        rc = pc_algo_ca_fill(w, h, n, here.data, here.len, band, allowed, hw, &opts, NULL, &filled, err);
        free(allowed);
        free(band);
        if (rc != PC_OK) {
            pc_paint_region_fini(&here);
            return rc;
        }
        free(here.data);
        here.data = filled;
    } else {
        free(band);
    }
    rc = ca_replace(doc, t, p, &here, selected, hw, dmg, err);
    pc_paint_region_fini(&here);
    return rc;
}

/* ---- Transform On Drop (content_aware_move placement helpers) ----------------------------- */

static pc_algo_homography ca_translate_h(double x, double y)
{
    return (pc_algo_homography){{1.0, 0.0, x, 0.0, 1.0, y, 0.0, 0.0, 1.0}};
}

/* scaleX/scaleY (%) and rotate (deg): the map from a source point to where it lands, about the
 * centre of `area` then moved by `offset`. *has_map false when the params leave the content as it
 * is (content_aware_move::placement). */
static pc_status ca_placement(yyjson_val *p, const char *cmd, pc_rect area, int32_t dx, int32_t dy,
                              bool *has_map, pc_algo_homography *out, pc_error *err)
{
    double sx = 100.0, sy = 100.0, deg = 0.0;
    const char *keys[2] = {"scaleX", "scaleY"};
    double *dsts[2] = {&sx, &sy};
    for (int i = 0; i < 2; i++) {
        yyjson_val *v = pc_json_get(p, keys[i]);
        double x;
        if (v != NULL && !yyjson_is_null(v)) {
            if (!pc_json_as_f64(v, &x) || !isfinite(x)) {
                return pc_engine_err_bad_params(err, cmd, "`%s` must be a finite number", keys[i]);
            }
            double a = fabs(x);
            if (!(a >= 1.0 && a <= 10000.0)) {
                return pc_engine_err_bad_params(err, cmd, "`%s` must be 1..10000 %% (or -10000..-1 to flip)",
                                                keys[i]);
            }
            *dsts[i] = x;
        }
    }
    yyjson_val *rv = pc_json_get(p, "rotate");
    if (rv != NULL && !yyjson_is_null(rv)) {
        if (!pc_json_as_f64(rv, &deg) || !isfinite(deg)) {
            return pc_engine_err_bad_params(err, cmd, "`rotate` must be a finite number");
        }
        if (fabs(deg) > 36000.0) {
            return pc_engine_err_bad_params(err, cmd, "`rotate` must be -36000..36000 degrees");
        }
    }
    sx /= 100.0;
    sy /= 100.0;
    deg = deg - 360.0 * floor(deg / 360.0); /* rem_euclid(deg, 360) */
    if (sx == 1.0 && sy == 1.0 && deg == 0.0) {
        *has_map = false;
        return PC_OK;
    }
    double cx = (double)(area.x0 + area.x1) / 2.0, cy = (double)(area.y0 + area.y1) / 2.0;
    double rad = deg * (3.14159265358979323846 / 180.0);
    double ca = cos(rad), sa = sin(rad);
    pc_algo_homography rot = {{ca, -sa, 0.0, sa, ca, 0.0, 0.0, 0.0, 1.0}};
    pc_algo_homography scale = {{sx, 0.0, 0.0, 0.0, sy, 0.0, 0.0, 0.0, 1.0}};
    pc_algo_homography t1 = ca_translate_h(cx + (double)dx, cy + (double)dy);
    pc_algo_homography t2 = ca_translate_h(-cx, -cy);
    pc_algo_homography m = pc_algo_homography_mul(&t1, &rot);
    m = pc_algo_homography_mul(&m, &scale);
    m = pc_algo_homography_mul(&m, &t2);
    *has_map = true;
    *out = m;
    return PC_OK;
}

/* The pixel bounds of `r` mapped by `h` (content_aware_move::mapped_bounds). */
static pc_rect ca_mapped_bounds(const pc_algo_homography *h, pc_rect r)
{
    int cx[4] = {r.x0, r.x1, r.x1, r.x0};
    int cy[4] = {r.y0, r.y0, r.y1, r.y1};
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int i = 0; i < 4; i++) {
        double ox, oy;
        pc_algo_homography_apply(h, (double)cx[i], (double)cy[i], &ox, &oy);
        if (ox < x0) x0 = ox;
        if (oy < y0) y0 = oy;
        if (ox > x1) x1 = ox;
        if (oy > y1) y1 = oy;
    }
    if (!(isfinite(x0) && isfinite(y0) && isfinite(x1) && isfinite(y1))) {
        return PC_RECT_EMPTY;
    }
    double lo = -1e8, hi = 1e8;
    double a = floor(x0 + 1e-6), b = floor(y0 + 1e-6), c = ceil(x1 - 1e-6), d = ceil(y1 - 1e-6);
    a = a < lo ? lo : a > hi ? hi : a;
    b = b < lo ? lo : b > hi ? hi : b;
    c = c < lo ? lo : c > hi ? hi : c;
    d = d < lo ? lo : d > hi ? hi : d;
    return pc_rect_make((int32_t)a, (int32_t)b, (int32_t)c, (int32_t)d);
}

/* `src` resampled (bilinear, edges clamped) through `inv` (destination -> source) over `rect`: the
 * content as it lands (content_aware_move::resample). *out owned (pc_paint_region_fini). */
static pc_status ca_resample(const pc_paint_region *src, const pc_algo_homography *inv, pc_rect rect,
                             pc_paint_region *out, pc_error *err)
{
    size_t sw = pc_rect_width(src->rect), sh = pc_rect_height(src->rect), n = src->ch;
    pc_status rc = pc_paint_region_new(rect, n, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (sw == 0 || sh == 0) {
        return PC_OK;
    }
    size_t rw = pc_rect_width(rect);
    size_t count = (n > 0) ? out->len / n : 0;
    for (size_t i = 0; i < count; i++) {
        int32_t qx = rect.x0 + (int32_t)(i % (rw > 0 ? rw : 1));
        int32_t qy = rect.y0 + (int32_t)(i / (rw > 0 ? rw : 1));
        double usx, usy;
        pc_algo_homography_apply(inv, (double)qx + 0.5, (double)qy + 0.5, &usx, &usy);
        double fx = usx - 0.5 - (double)src->rect.x0;
        double fy = usy - 0.5 - (double)src->rect.y0;
        fx = fx < 0.0 ? 0.0 : fx > (double)(sw - 1) ? (double)(sw - 1) : fx;
        fy = fy < 0.0 ? 0.0 : fy > (double)(sh - 1) ? (double)(sh - 1) : fy;
        if (!(isfinite(fx) && isfinite(fy))) {
            continue;
        }
        size_t x0 = (size_t)floor(fx), y0 = (size_t)floor(fy);
        size_t x1 = x0 + 1 < sw ? x0 + 1 : sw - 1, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        float tx = (float)(fx - (double)x0), ty = (float)(fy - (double)y0);
        float *dst = &out->data[i * n];
        for (size_t c = 0; c < n; c++) {
            float p00 = src->data[(y0 * sw + x0) * n + c];
            float p10 = src->data[(y0 * sw + x1) * n + c];
            float p01 = src->data[(y1 * sw + x0) * n + c];
            float p11 = src->data[(y1 * sw + x1) * n + c];
            float top = p00 * (1.0f - tx) + p10 * tx;
            float bot = p01 * (1.0f - tx) + p11 * tx;
            dst[c] = top * (1.0f - ty) + bot * ty;
        }
    }
    return PC_OK;
}

/* The selection's shape as it lands over `rect`: `cov` (over `area`) sampled through `inv`,
 * nearest, set wherever above zero (content_aware_move::landed_mask). *out owned bools. */
static pc_status ca_landed_mask(const float *cov, size_t ncov, pc_rect area, const pc_algo_homography *inv,
                                pc_rect rect, bool **out, pc_error *err)
{
    size_t rw = pc_rect_width(rect), rh = pc_rect_height(rect), n = rw * rh;
    size_t aw = pc_rect_width(area);
    bool *m = malloc((n > 0 ? n : 1) * sizeof *m);
    if (m == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "landed mask");
    }
    for (size_t i = 0; i < n; i++) {
        int32_t qx = rect.x0 + (int32_t)(i % (rw > 0 ? rw : 1));
        int32_t qy = rect.y0 + (int32_t)(i / (rw > 0 ? rw : 1));
        double sx, sy;
        pc_algo_homography_apply(inv, (double)qx + 0.5, (double)qy + 0.5, &sx, &sy);
        double x = floor(sx), y = floor(sy);
        if (!(x >= (double)area.x0 && x < (double)area.x1 && y >= (double)area.y0 && y < (double)area.y1)) {
            m[i] = false;
            continue;
        }
        size_t j = (size_t)((int32_t)y - area.y0) * aw + (size_t)((int32_t)x - area.x0);
        m[i] = j < ncov && cov[j] > 0.0f;
    }
    *out = m;
    return PC_OK;
}

/* Where a fill samples around `r`: Photoshop's Content-Aware window, at most `margin` pixels
 * around `r` (content_aware_move::window_within). */
static pc_rect ca_window_within(pc_rect r, pc_rect canvas, int32_t margin)
{
    return pc_rect_intersect(pc_engine_fill_sampling_window(r, canvas), pc_rect_inflate(r, margin));
}

/* ============================================================================================ */
/* Patch                                                                                         */
/* ============================================================================================ */

struct patch_plan {
    bool has_layer;
    pc_layer_id layer;
    pc_rect canvas;
    pc_rect area;
    int32_t dst_dx, dst_dy;
    int32_t src_dx, src_dy;
    bool destination;
    int32_t offx, offy;
};

static pc_status patch_plan(pc_engine_session *s, yyjson_val *p, const char *cmd, struct patch_plan *out,
                            pc_error *err)
{
    size_t len = 0;
    const char *mode = string_p(p, "mode", "source", &len);
    bool destination;
    if (len == 6 && memcmp(mode, "source", 6) == 0) {
        destination = false;
    } else if (len == 11 && memcmp(mode, "destination", 11) == 0) {
        destination = true;
    } else {
        return pc_engine_err_bad_params(err, cmd, "unknown mode `%.*s` (source|destination)", (int)len, mode);
    }
    int32_t dx, dy;
    pc_status rc = ca_offset(cmd, p, &dx, &dy, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool has_layer;
    pc_layer_id layer;
    if ((rc = ca_target_layer(s, p, &has_layer, &layer, err)) != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect canvas = pc_document_bounds(doc);
    if (!doc->has_selection) {
        return pc_engine_err_bad_params(err, cmd, "select the area to patch first");
    }
    pc_rect area = ca_selection_area(&doc->selection, canvas);
    if (pc_rect_is_empty(area)) {
        return pc_engine_err_bad_params(err, cmd, "the selection is empty");
    }
    uint64_t pixels = (uint64_t)pc_rect_width(area) * (uint64_t)pc_rect_height(area);
    if (pixels > CA_MAX_PIXELS) {
        return pc_engine_err_bad_params(err, cmd, "the selection is too large to patch (%llu pixels, at most %llu)",
                                        (unsigned long long)pixels, (unsigned long long)CA_MAX_PIXELS);
    }
    if (dx == 0 && dy == 0) {
        return pc_engine_err_bad_params(err, cmd, "drag the selection to the area to sample from");
    }
    if (!pc_rect_contains_rect(canvas, pc_rect_translate(area, dx, dy))) {
        return pc_engine_err_bad_params(err, cmd, "the dragged patch must stay inside the canvas");
    }
    out->has_layer = has_layer;
    out->layer = layer;
    out->canvas = canvas;
    out->area = area;
    out->destination = destination;
    out->offx = dx;
    out->offy = dy;
    if (destination) {
        out->dst_dx = dx;
        out->dst_dy = dy;
        out->src_dx = -dx;
        out->src_dy = -dy;
    } else {
        out->dst_dx = 0;
        out->dst_dy = 0;
        out->src_dx = dx;
        out->src_dy = dy;
    }
    return PC_OK;
}

/* Heal `surf` at the selection (shifted by dst) with texture read `src` away (patch_surface,
 * step 1 = exact). *dmg = the damaged rectangle. */
static pc_status patch_surface(pc_surface *surf, const pc_surface *sel, const struct patch_plan *pl, bool lock,
                               pc_rect *dmg, pc_error *err)
{
    pc_rect dst_area = pc_rect_translate(pl->area, pl->dst_dx, pl->dst_dy);
    pc_rect g = pc_rect_intersect(pc_rect_intersect(pc_rect_inflate(dst_area, 2), pl->canvas),
                                  pc_rect_translate(pl->canvas, -pl->src_dx, -pl->src_dy));
    if (pc_rect_is_empty(g)) {
        *dmg = PC_RECT_EMPTY;
        return PC_OK;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    pc_paint_region dst = {0}, src = {0};
    pc_status rc = pc_paint_region_read(surf, g, &dst, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_paint_region_read(surf, pc_rect_translate(g, pl->src_dx, pl->src_dy), &src, err);
    if (rc != PC_OK) {
        pc_paint_region_fini(&dst);
        return rc;
    }
    src.rect = g; /* relabel (same width/height) */
    float *cov = NULL;
    size_t ncov = 0;
    rc = ca_coverage(sel, pc_rect_translate(g, -pl->dst_dx, -pl->dst_dy), &cov, &ncov, err);
    if (rc != PC_OK) {
        pc_paint_region_fini(&dst);
        pc_paint_region_fini(&src);
        return rc;
    }
    bool *mask = malloc((ncov > 0 ? ncov : 1) * sizeof *mask);
    if (mask == NULL) {
        pc_paint_region_fini(&dst);
        pc_paint_region_fini(&src);
        free(cov);
        return pc_fail(err, PC_ERR_NOMEM, "patch mask");
    }
    for (size_t i = 0; i < ncov; i++) {
        mask[i] = cov[i] > 0.0f;
    }
    pc_paint_region healed = {0};
    rc = heal_region(fmt, &src, &dst, mask, &healed, err);
    free(mask);
    pc_paint_region_fini(&dst);
    pc_paint_region_fini(&src);
    if (rc != PC_OK) {
        free(cov);
        return rc;
    }
    rc = pc_paint_apply_coverage(surf, g, cov, ncov, 1.0f, NULL, lock, &healed, PC_BLEND_NORMAL, dmg, err);
    free(cov);
    pc_paint_region_fini(&healed);
    return rc;
}

struct patch_work {
    const struct patch_plan *pl;
};

static pc_status patch_work_fn(void *tool, const pc_document *pre, pc_surface *surf, const pc_surface *sel,
                               bool lock, pc_rect *dmg, pc_error *err)
{
    (void)pre;
    struct patch_work *c = tool;
    if (sel == NULL) {
        return pc_engine_err_bad_params(err, "paint.patch", "select the area to patch first");
    }
    return patch_surface(surf, sel, c->pl, lock, dmg, err);
}

/* Patch: Content-Aware (patch_content_aware): the selection is filled from where it was dragged.
 * Runs synchronously (§7). */
struct patch_ca_work {
    struct patch_plan pl;
    ca_target target;
    int structure;
    int color;
    yyjson_val *p;
    pc_rect dmg; /* out */
};

static pc_status patch_ca_cb(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct patch_ca_work *c = vctx;
    if (!doc->has_selection) {
        return pc_engine_err_bad_params(err, "paint.patch", "select the area to patch first");
    }
    pc_rect win = ca_window_within(c->pl.area, c->pl.canvas, CA_MAX_MARGIN);
    pc_paint_region content = {0};
    pc_status rc = ca_read(doc, c->target, c->p, pc_rect_translate(win, c->pl.offx, c->pl.offy), &content, err);
    if (rc != PC_OK) {
        return rc;
    }
    content.rect = win;
    bool *selected = NULL;
    size_t nsel = 0;
    rc = ca_mask(&doc->selection, win, &selected, &nsel, err);
    if (rc != PC_OK) {
        pc_paint_region_fini(&content);
        return rc;
    }
    bool *avoid = calloc(nsel > 0 ? nsel : 1, sizeof(bool));
    if (avoid == NULL) {
        pc_paint_region_fini(&content);
        free(selected);
        return pc_fail(err, PC_ERR_NOMEM, "avoid");
    }
    rc = ca_land(doc, c->target, c->p, win, selected, &content, avoid, c->structure, c->color, &c->dmg, err);
    pc_paint_region_fini(&content);
    free(selected);
    free(avoid);
    return rc;
}

static pc_status patch_content_aware(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "paint.patch";
    struct patch_ca_work c = {0};
    pc_status rc = patch_plan(s, p, CMD, &c.pl, err);
    if (rc != PC_OK) {
        return rc;
    }
    if ((rc = ca_level(p, CMD, "structure", 4, 1, 7, &c.structure, err)) != PC_OK ||
        (rc = ca_level(p, CMD, "color", 0, 0, 10, &c.color, err)) != PC_OK) {
        return rc;
    }
    c.target.has_layer = c.pl.has_layer;
    c.target.layer = c.pl.layer;
    c.target.all_layers = flag_p(p, "sampleAllLayers", false) && ca_targets_pixels(p);
    c.p = p;
    c.dmg = PC_RECT_EMPTY;
    rc = pc_engine_session_edit(s, "Patch (Content-Aware)", patch_ca_cb, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->last_damage = c.dmg;
        st->has_last_damage = true;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *da = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, da, c.dmg.x0);
    yyjson_mut_arr_add_int(d, da, c.dmg.y0);
    yyjson_mut_arr_add_int(d, da, (int64_t)pc_rect_width(c.dmg));
    yyjson_mut_arr_add_int(d, da, (int64_t)pc_rect_height(c.dmg));
    yyjson_mut_obj_add_val(d, root, "damage", da);
    yyjson_mut_val *off = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, off, c.pl.offx);
    yyjson_mut_arr_add_int(d, off, c.pl.offy);
    yyjson_mut_obj_add_val(d, root, "offset", off);
    yyjson_mut_obj_add_bool(d, root, "contentAware", true);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status patch_run(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "paint.patch";
    if (flag_p(p, "contentAware", false)) {
        return patch_content_aware(s, p, out, err);
    }
    struct patch_plan pl = {0};
    pc_status rc = patch_plan(s, p, CMD, &pl, err);
    if (rc != PC_OK) {
        return rc;
    }
    struct patch_work pw = {&pl};
    struct retouch_edit ed = {pl.has_layer, pl.layer, p, &pw, patch_work_fn, PC_RECT_EMPTY};
    pc_rect dmg = PC_RECT_EMPTY;
    rc = run_stroke(s, pl.destination ? "Patch (Destination)" : "Patch", &ed, &dmg, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *da = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, da, dmg.x0);
    yyjson_mut_arr_add_int(d, da, dmg.y0);
    yyjson_mut_arr_add_int(d, da, (int64_t)pc_rect_width(dmg));
    yyjson_mut_arr_add_int(d, da, (int64_t)pc_rect_height(dmg));
    yyjson_mut_obj_add_val(d, root, "damage", da);
    yyjson_mut_val *off = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, off, pl.offx);
    yyjson_mut_arr_add_int(d, off, pl.offy);
    yyjson_mut_obj_add_val(d, root, "offset", off);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status patch_enabled(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = pc_engine_has_paintable(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !pc_doc_arc_get(d->doc)->has_selection) {
        return pc_fail(reason, PC_ERR_INVALID, "select the area to patch first");
    }
    return PC_OK;
}

/* ============================================================================================ */
/* Content-Aware Move                                                                            */
/* ============================================================================================ */

enum ca_move_mode { CA_MOVE, CA_EXTEND };

struct move_plan {
    ca_target target;
    pc_rect canvas;
    pc_rect area;
    int32_t dx, dy;
    bool has_map;
    pc_algo_homography map;
    pc_rect dest;
    enum ca_move_mode mode;
    int structure;
    int color;
    int32_t margin;
};

static pc_status move_plan(pc_engine_session *s, yyjson_val *p, const char *cmd, int32_t margin,
                           struct move_plan *out, pc_error *err)
{
    size_t len = 0;
    const char *mode = string_p(p, "mode", "move", &len);
    if (len == 4 && memcmp(mode, "move", 4) == 0) {
        out->mode = CA_MOVE;
    } else if (len == 6 && memcmp(mode, "extend", 6) == 0) {
        out->mode = CA_EXTEND;
    } else {
        return pc_engine_err_bad_params(err, cmd, "unknown mode `%.*s` (move|extend)", (int)len, mode);
    }
    pc_status rc;
    if ((rc = ca_level(p, cmd, "structure", 4, 1, 7, &out->structure, err)) != PC_OK ||
        (rc = ca_level(p, cmd, "color", 0, 0, 10, &out->color, err)) != PC_OK ||
        (rc = ca_offset(cmd, p, &out->dx, &out->dy, err)) != PC_OK) {
        return rc;
    }
    bool has_layer;
    pc_layer_id layer;
    if ((rc = ca_target_layer(s, p, &has_layer, &layer, err)) != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    out->canvas = pc_document_bounds(doc);
    if (!doc->has_selection) {
        return pc_engine_err_bad_params(err, cmd, "select the area to move first");
    }
    out->area = ca_selection_area(&doc->selection, out->canvas);
    if (pc_rect_is_empty(out->area)) {
        return pc_engine_err_bad_params(err, cmd, "the selection is empty");
    }
    uint64_t pixels = (uint64_t)pc_rect_width(out->area) * (uint64_t)pc_rect_height(out->area);
    if (pixels > CA_MAX_PIXELS) {
        return pc_engine_err_bad_params(err, cmd, "the selection is too large to move (%llu pixels, at most %llu)",
                                        (unsigned long long)pixels, (unsigned long long)CA_MAX_PIXELS);
    }
    if ((rc = ca_placement(p, cmd, out->area, out->dx, out->dy, &out->has_map, &out->map, err)) != PC_OK) {
        return rc;
    }
    if (!out->has_map) {
        if (out->dx == 0 && out->dy == 0) {
            return pc_engine_err_bad_params(err, cmd, "drag the selection to where the content should go");
        }
        pc_rect dest = pc_rect_translate(out->area, out->dx, out->dy);
        if (!pc_rect_contains_rect(out->canvas, dest)) {
            return pc_engine_err_bad_params(err, cmd, "the moved selection must stay inside the canvas");
        }
        out->dest = dest;
    } else {
        pc_rect landed = ca_mapped_bounds(&out->map, out->area);
        uint64_t lp = (uint64_t)pc_rect_width(landed) * (uint64_t)pc_rect_height(landed);
        if (lp > CA_MAX_PIXELS) {
            return pc_engine_err_bad_params(err, cmd,
                                            "the transformed selection is too large to move (%llu pixels, at most %llu)",
                                            (unsigned long long)lp, (unsigned long long)CA_MAX_PIXELS);
        }
        pc_rect dest = pc_rect_intersect(landed, out->canvas);
        if (pc_rect_is_empty(dest)) {
            return pc_engine_err_bad_params(err, cmd, "the transformed selection must land on the canvas");
        }
        out->dest = dest;
    }
    out->target.has_layer = has_layer;
    out->target.layer = layer;
    out->target.all_layers = flag_p(p, "sampleAllLayers", false) && ca_targets_pixels(p);
    out->margin = margin;
    return PC_OK;
}

/* The move, run on `doc` (a copy). *dmg = the rectangle changed. On a transform, *moved_sel holds
 * the warped selection (the caller owns it); else *has_moved_sel is false (content_aware_move::run_move).
 * The caller writes the surface back; here the target surface in `doc` already holds the result. */
static pc_status move_run(pc_document *doc, const struct move_plan *pl, yyjson_val *p, pc_rect *dmg,
                          pc_error *err)
{
    int32_t dx = pl->dx, dy = pl->dy;
    pc_algo_ca_fill_options opts = ca_fill_options();
    bool has_inv = false;
    pc_algo_homography inv;
    if (pl->has_map) {
        if (!pc_algo_homography_inverse(&pl->map, &inv)) {
            return pc_engine_err_bad_params(err, "paint.contentAwareMove", "the transform collapses the selection");
        }
        has_inv = true;
    }
    float *cov = NULL;
    size_t ncov = 0;
    if (has_inv) {
        pc_status rc = ca_coverage(&doc->selection, pl->area, &cov, &ncov, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    /* 1. The content at its new place. */
    pc_rect win = ca_window_within(pl->dest, pl->canvas, pl->margin);
    pc_paint_region content = {0};
    bool *moved = NULL;
    size_t nmoved = 0;
    pc_status rc;
    if (!has_inv) {
        rc = ca_read(doc, pl->target, p, pc_rect_translate(win, -dx, -dy), &content, err);
        if (rc == PC_OK) {
            content.rect = win;
            rc = ca_mask(&doc->selection, pc_rect_translate(win, -dx, -dy), &moved, &nmoved, err);
        }
    } else {
        pc_rect src = pc_rect_intersect(pc_rect_intersect(ca_mapped_bounds(&inv, pc_rect_inflate(pl->dest, 8)),
                                                          pc_rect_inflate(pl->area, pl->margin)),
                                        pl->canvas);
        if (pc_rect_is_empty(src)) {
            src = pl->area;
        }
        pc_paint_region srcreg = {0};
        rc = ca_read(doc, pl->target, p, src, &srcreg, err);
        if (rc == PC_OK) {
            rc = ca_resample(&srcreg, &inv, win, &content, err);
            pc_paint_region_fini(&srcreg);
        }
        if (rc == PC_OK) {
            rc = ca_landed_mask(cov, ncov, pl->area, &inv, win, &moved, err);
            nmoved = pc_rect_width(win) * pc_rect_height(win);
        }
    }
    if (rc != PC_OK) {
        free(cov);
        pc_paint_region_fini(&content);
        free(moved);
        return rc;
    }
    bool *old_place = NULL;
    size_t nold = 0;
    rc = ca_mask(&doc->selection, win, &old_place, &nold, err);
    if (rc != PC_OK) {
        free(cov);
        pc_paint_region_fini(&content);
        free(moved);
        return rc;
    }
    pc_rect damage = PC_RECT_EMPTY;
    rc = ca_land(doc, pl->target, p, win, moved, &content, old_place, pl->structure, pl->color, &damage, err);
    pc_paint_region_fini(&content);
    free(moved);
    free(old_place);
    if (rc != PC_OK) {
        free(cov);
        return rc;
    }
    /* 2. Move: fill the place the content left. */
    if (pl->mode == CA_MOVE) {
        pc_rect win2 = ca_window_within(pl->area, pl->canvas, pl->margin);
        size_t w = pc_rect_width(win2), h = pc_rect_height(win2), hw = w * h;
        pc_paint_region img = {0};
        rc = ca_read(doc, pl->target, p, win2, &img, err);
        bool *landed = NULL;
        if (rc == PC_OK) {
            if (!has_inv) {
                size_t nl = 0;
                rc = ca_mask(&doc->selection, pc_rect_translate(win2, -dx, -dy), &landed, &nl, err);
            } else {
                rc = ca_landed_mask(cov, ncov, pl->area, &inv, win2, &landed, err);
            }
        }
        bool *hole = NULL, *sel_m = NULL;
        size_t nsel = 0;
        if (rc == PC_OK) {
            rc = ca_mask(&doc->selection, win2, &sel_m, &nsel, err);
        }
        bool any_hole = false;
        if (rc == PC_OK) {
            hole = malloc((hw > 0 ? hw : 1) * sizeof *hole);
            if (hole == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "hole");
            } else {
                for (size_t i = 0; i < hw; i++) {
                    hole[i] = sel_m[i] && !landed[i];
                    any_hole = any_hole || hole[i];
                }
            }
        }
        if (rc == PC_OK && any_hole) {
            bool *allowed = malloc((hw > 0 ? hw : 1) * sizeof *allowed);
            if (allowed == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "allowed");
            } else {
                for (size_t i = 0; i < hw; i++) {
                    allowed[i] = !landed[i];
                }
                float *data = NULL;
                rc = pc_algo_ca_fill(w, h, img.ch, img.data, img.len, hole, allowed, hw, &opts, NULL, &data, err);
                free(allowed);
                if (rc == PC_OK) {
                    pc_paint_region fillreg = {win2, img.ch, data, img.len};
                    pc_rect r2 = PC_RECT_EMPTY;
                    rc = ca_replace(doc, pl->target, p, &fillreg, hole, hw, &r2, err);
                    free(data);
                    if (rc == PC_OK) {
                        damage = pc_rect_union(damage, r2);
                    }
                }
            }
        }
        free(hole);
        free(sel_m);
        free(landed);
        pc_paint_region_fini(&img);
    }
    free(cov);
    *dmg = damage;
    return rc;
}

struct move_apply {
    struct move_plan pl;
    yyjson_val *p;
    pc_document *work; /* the computed result lives here; moved into the real doc */
    bool has_moved_sel;
    pc_surface moved_sel;
    pc_rect dmg;
};

static pc_status move_apply_cb(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct move_apply *c = vctx;
    /* Copy the computed target surface from the work document into the real one. */
    pc_surface *src = NULL;
    bool lock = false;
    pc_status rc =
        pc_engine_target_surface(c->work, c->pl.target.has_layer, c->pl.target.layer, c->p, &src, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface copy = {0};
    if ((rc = pc_surface_clone(src, &copy, err)) != PC_OK) {
        return rc;
    }
    pc_surface *dst = NULL;
    rc = pc_engine_target_surface(doc, c->pl.target.has_layer, c->pl.target.layer, c->p, &dst, &lock, err);
    if (rc != PC_OK) {
        pc_surface_fini(&copy);
        return rc;
    }
    pc_surface_fini(dst);
    *dst = copy;
    /* The selection follows the content, ready for another drag. */
    if (c->pl.has_map) {
        if (c->has_moved_sel) {
            pc_rect cb = pc_surface_content_bounds(&c->moved_sel);
            pc_surface_fini(&doc->selection);
            if (pc_rect_is_empty(cb)) {
                doc->has_selection = false;
                doc->selection = (pc_surface){0};
                pc_surface_fini(&c->moved_sel);
            } else {
                doc->selection = c->moved_sel;
                doc->has_selection = true;
            }
            c->has_moved_sel = false; /* ownership moved (or freed) */
        }
    } else if (doc->has_selection) {
        pc_surface shifted = {0};
        if (pc_algo_translate_surface(&doc->selection, c->pl.dx, c->pl.dy, &shifted, err) == PC_OK) {
            pc_surface_fini(&doc->selection);
            doc->selection = shifted;
        }
    }
    return PC_OK;
}

static pc_status content_aware_move(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "paint.contentAwareMove";
    struct move_apply c = {0};
    c.p = p;
    pc_status rc = move_plan(s, p, CMD, CA_MAX_MARGIN, &c.pl, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_document work = {0};
    if ((rc = pc_document_clone(pc_doc_arc_get(d->doc), &work, err)) != PC_OK) {
        return rc;
    }
    if (!work.has_selection) {
        pc_document_fini(&work);
        return pc_engine_err_bad_params(err, CMD, "select the area to move first");
    }
    rc = move_run(&work, &c.pl, p, &c.dmg, err);
    if (rc != PC_OK) {
        pc_document_fini(&work);
        return rc;
    }
    /* The selection follows the transformed content (warp_gray); computed before the edit. */
    if (c.pl.has_map) {
        rc = pc_engine_transform_warp_gray(&work.selection, &c.pl.map, PC_ALGO_INTERP_BILINEAR, &c.moved_sel,
                                           err);
        if (rc != PC_OK) {
            pc_document_fini(&work);
            return rc;
        }
        c.has_moved_sel = true;
    }
    c.work = &work;
    const char *label = c.pl.mode == CA_MOVE ? "Content-Aware Move" : "Content-Aware Extend";
    rc = pc_engine_session_edit(s, label, move_apply_cb, &c, err);
    if (c.has_moved_sel) {
        pc_surface_fini(&c.moved_sel);
    }
    pc_document_fini(&work);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->last_damage = c.dmg;
        st->has_last_damage = true;
    }
    yyjson_mut_doc *dj = NULL;
    if (pc_engine_result_doc(out, &dj, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(dj);
    yyjson_mut_val *da = yyjson_mut_arr(dj);
    yyjson_mut_arr_add_int(dj, da, c.dmg.x0);
    yyjson_mut_arr_add_int(dj, da, c.dmg.y0);
    yyjson_mut_arr_add_int(dj, da, (int64_t)pc_rect_width(c.dmg));
    yyjson_mut_arr_add_int(dj, da, (int64_t)pc_rect_height(c.dmg));
    yyjson_mut_obj_add_val(dj, root, "damage", da);
    yyjson_mut_val *off = yyjson_mut_arr(dj);
    yyjson_mut_arr_add_int(dj, off, c.pl.dx);
    yyjson_mut_arr_add_int(dj, off, c.pl.dy);
    yyjson_mut_obj_add_val(dj, root, "offset", off);
    yyjson_mut_obj_add_strcpy(dj, root, "mode", c.pl.mode == CA_MOVE ? "move" : "extend");
    if (c.pl.has_map) {
        yyjson_mut_obj_add_bool(dj, root, "transformed", true);
    }
    yyjson_mut_doc_set_root(dj, root);
    return PC_OK;
}

static pc_status move_enabled(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = pc_engine_has_paintable(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !pc_doc_arc_get(d->doc)->has_selection) {
        return pc_fail(reason, PC_ERR_INVALID, "select the area to move first");
    }
    return PC_OK;
}

/* ============================================================================================ */
/* Remove                                                                                        */
/* ============================================================================================ */

#define REMOVE_GROW ((size_t)2)
#define REMOVE_SEED ((uint64_t)0x0052e30e)

/* Three quarters of the larger side, at least 32 pixels; a margin beyond i32 saturates. */
static int32_t remove_sampling_margin(pc_rect area)
{
    uint64_t ext = pc_rect_width(area) > pc_rect_height(area) ? pc_rect_width(area) : pc_rect_height(area);
    uint64_t m = ext * 3 / 4;
    int32_t v = m > (uint64_t)INT32_MAX ? INT32_MAX : (int32_t)m;
    return v > 32 ? v : 32;
}

/* The stroke's bounding box (points +- the brush radius). */
static pc_rect remove_stroke_bounds(const pc_paint_stroke *stroke)
{
    double r = (double)stroke->brush.size / 2.0 + 2.0;
    double x0 = DBL_MAX, y0 = DBL_MAX, x1 = -DBL_MAX, y1 = -DBL_MAX;
    for (size_t i = 0; i < stroke->count; i++) {
        double qx = stroke->points[i].x, qy = stroke->points[i].y;
        if (qx < x0) x0 = qx;
        if (qy < y0) y0 = qy;
        if (qx > x1) x1 = qx;
        if (qy > y1) y1 = qy;
    }
    return pc_rect_make((int32_t)floor(x0 - r), (int32_t)floor(y0 - r), (int32_t)ceil(x1 + r),
                        (int32_t)ceil(y1 + r));
}

struct remove_plan {
    bool has_layer;
    pc_layer_id layer;
    pc_paint_stroke *strokes;
    size_t nstrokes;
    pc_rect window;
    bool close_loops;
    bool all_layers;
    bool new_layer;
};

static void remove_plan_fini(struct remove_plan *pl)
{
    for (size_t i = 0; i < pl->nstrokes; i++) {
        pc_paint_stroke_fini(&pl->strokes[i]);
    }
    free(pl->strokes);
    pl->strokes = NULL;
    pl->nstrokes = 0;
}

/* The strokes of the request: `strokes` (several) or the one in `points`. */
static pc_status remove_parse_strokes(pc_engine_session *s, yyjson_val *p, const char *cmd,
                                      struct remove_plan *pl, pc_error *err)
{
    yyjson_val *list = pc_json_get(p, "strokes");
    if (list == NULL) {
        pc_paint_stroke stroke = {0};
        bool has_layer;
        pc_layer_id layer;
        pc_status rc = parse_brush(s, p, cmd, &stroke, &has_layer, &layer, err);
        if (rc != PC_OK) {
            return rc;
        }
        pl->strokes = malloc(sizeof *pl->strokes);
        if (pl->strokes == NULL) {
            pc_paint_stroke_fini(&stroke);
            return pc_fail(err, PC_ERR_NOMEM, "strokes");
        }
        pl->strokes[0] = stroke;
        pl->nstrokes = 1;
        pl->has_layer = has_layer;
        pl->layer = layer;
        return PC_OK;
    }
    if (!yyjson_is_arr(list)) {
        return pc_engine_err_bad_params(err, cmd, "`strokes` is a list of point lists");
    }
    size_t n = yyjson_arr_size(list);
    if (n == 0) {
        return pc_engine_err_bad_params(err, cmd, "`strokes` is empty");
    }
    pl->strokes = calloc(n, sizeof *pl->strokes);
    if (pl->strokes == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "strokes");
    }
    /* Build a mutable copy of `p` with "strokes" removed and "points" set per entry. */
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL) {
        free(pl->strokes);
        pl->strokes = NULL;
        return pc_fail(err, PC_ERR_NOMEM, "strokes doc");
    }
    pc_status rc = PC_OK;
    size_t i = 0;
    size_t idx;
    size_t max;
    yyjson_val *points;
    yyjson_arr_foreach(list, idx, max, points)
    {
        yyjson_mut_val *mp = yyjson_val_mut_copy(md, p);
        yyjson_mut_val *mpts = yyjson_val_mut_copy(md, points);
        if (mp == NULL || mpts == NULL || !yyjson_mut_is_obj(mp)) {
            rc = pc_fail(err, PC_ERR_NOMEM, "strokes copy");
            break;
        }
        yyjson_mut_obj_remove_key(mp, "strokes");
        yyjson_mut_obj_remove_key(mp, "points");
        yyjson_mut_obj_add_val(md, mp, "points", mpts);
        yyjson_mut_doc_set_root(md, mp);
        /* Re-parse through an immutable view so parse_brush sees a yyjson_val. */
        yyjson_doc *idoc = yyjson_mut_doc_imut_copy(md, NULL);
        if (idoc == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "strokes imut");
            break;
        }
        yyjson_val *one = yyjson_doc_get_root(idoc);
        pc_paint_stroke stroke = {0};
        bool has_layer;
        pc_layer_id layer;
        rc = parse_brush(s, one, cmd, &stroke, &has_layer, &layer, err);
        yyjson_doc_free(idoc);
        if (rc != PC_OK) {
            break;
        }
        pl->strokes[i++] = stroke;
        pl->has_layer = has_layer;
        pl->layer = layer;
    }
    pl->nstrokes = i;
    yyjson_mut_doc_free(md);
    if (rc != PC_OK) {
        remove_plan_fini(pl);
    }
    return rc;
}

static pc_status remove_make_plan(pc_engine_session *s, yyjson_val *p, const char *cmd, struct remove_plan *pl,
                                  pc_error *err)
{
    pc_status rc = remove_parse_strokes(s, p, cmd, pl, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        remove_plan_fini(pl);
        return pc_engine_err_no_document(err);
    }
    pc_rect canvas = pc_document_bounds(pc_doc_arc_get(d->doc));
    pc_rect bounds = remove_stroke_bounds(&pl->strokes[0]);
    for (size_t i = 1; i < pl->nstrokes; i++) {
        bounds = pc_rect_union(bounds, remove_stroke_bounds(&pl->strokes[i]));
    }
    uint64_t pixels = (uint64_t)pc_rect_width(bounds) * (uint64_t)pc_rect_height(bounds);
    if (pixels > CA_MAX_PIXELS) {
        remove_plan_fini(pl);
        return pc_engine_err_bad_params(err, cmd, "the stroke is too large to remove (%llu pixels, at most %llu)",
                                        (unsigned long long)pixels, (unsigned long long)CA_MAX_PIXELS);
    }
    pc_rect area = pc_rect_intersect(bounds, canvas);
    if (pc_rect_is_empty(area)) {
        remove_plan_fini(pl);
        return pc_engine_err_bad_params(err, cmd, "the stroke is outside the canvas");
    }
    int32_t m = remove_sampling_margin(area);
    pl->window = pc_rect_intersect(pc_rect_inflate(area, m < CA_MAX_MARGIN ? m : CA_MAX_MARGIN), canvas);
    pl->close_loops = flag_p(p, "closeLoops", true);
    pl->all_layers = flag_p(p, "sampleAllLayers", false) && ca_targets_pixels(p);
    pl->new_layer = flag_p(p, "newLayer", false) && ca_targets_pixels(p);
    return PC_OK;
}

/* The hole over the plan's window (remove::hole). *out owned bools (w*h). */
static pc_status remove_hole(const struct remove_plan *pl, const pc_surface *sel, bool **out, pc_error *err)
{
    pc_rect win = pl->window;
    size_t w = pc_rect_width(win), h = pc_rect_height(win), hw = w * h;
    bool *m = calloc(hw > 0 ? hw : 1, sizeof(bool));
    if (m == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "hole");
    }
    for (size_t k = 0; k < pl->nstrokes; k++) {
        pc_rect bounds = PC_RECT_EMPTY;
        float *cov = NULL;
        size_t ncov = 0;
        pc_status rc = pc_paint_stroke_coverage(&pl->strokes[k], &bounds, &cov, &ncov, err);
        if (rc != PC_OK) {
            free(m);
            return rc;
        }
        size_t bw = pc_rect_width(bounds);
        for (size_t i = 0; i < ncov; i++) {
            int32_t x = bounds.x0 + (int32_t)(i % (bw > 0 ? bw : 1));
            int32_t y = bounds.y0 + (int32_t)(i / (bw > 0 ? bw : 1));
            if (cov[i] > 0.0f && pc_rect_contains(win, x, y)) {
                size_t j = (size_t)(y - win.y0) * w + (size_t)(x - win.x0);
                if (j < hw) {
                    m[j] = true;
                }
            }
        }
        free(cov);
    }
    if (pl->close_loops) {
        bool *cl = NULL;
        pc_status rc = pc_algo_remove_close_loops(w, h, m, hw, &cl, err);
        if (rc != PC_OK) {
            free(m);
            return rc;
        }
        free(m);
        m = cl;
    }
    bool *dl = NULL;
    pc_status rc = pc_algo_remove_dilate(w, h, m, hw, REMOVE_GROW, &dl, err);
    if (rc != PC_OK) {
        free(m);
        return rc;
    }
    free(m);
    m = dl;
    if (sel != NULL) {
        float *cov = NULL;
        size_t ncov = 0;
        rc = ca_coverage(sel, win, &cov, &ncov, err);
        if (rc != PC_OK) {
            free(m);
            return rc;
        }
        for (size_t i = 0; i < hw && i < ncov; i++) {
            m[i] = m[i] && cov[i] > 0.0f;
        }
        free(cov);
    }
    *out = m;
    return PC_OK;
}

/* The fill on a new layer above `id`: the hole's pixels, transparent elsewhere
 * (remove::fill_to_new_layer). *rect / *nid out. */
static pc_status remove_fill_new_layer(pc_document *doc, bool has_layer, pc_layer_id id,
                                       const pc_paint_region *out, const bool *hole, size_t hole_len,
                                       pc_rect *rect, pc_layer_id *nid, pc_error *err)
{
    pc_pixel_format base = pc_document_pixel_format(doc);
    pc_pixel_format fmt = {base.mode, base.sample, true};
    size_t n = pc_pixel_format_channels(fmt);
    size_t colours = n > 0 ? n - 1 : 0;
    if (out->ch != n && out->ch != colours) {
        return pc_engine_err_other(err, "internal error: the fill's channels don't match the document");
    }
    float *data = calloc((hole_len * n) > 0 ? hole_len * n : 1, sizeof(float));
    if (data == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "new layer fill");
    }
    for (size_t i = 0; i < hole_len; i++) {
        if (!hole[i]) {
            continue;
        }
        for (size_t c = 0; c < colours; c++) {
            data[i * n + c] = out->data[i * out->ch + c];
        }
        if (n > 0) {
            data[i * n + (n - 1)] = (out->ch == n) ? out->data[i * out->ch + (out->ch - 1)] : 1.0f;
        }
    }
    pc_str name = {0};
    pc_status rc = pc_document_next_layer_name(doc, "Layer", &name, err);
    if (rc != PC_OK) {
        free(data);
        return rc;
    }
    pc_layer l = {0};
    rc = pc_layer_raster(&l, pc_str_c(&name), fmt, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        free(data);
        return rc;
    }
    pc_surface *surf = pc_layer_surface_mut(&l);
    if (surf != NULL) {
        rc = pc_surface_write_region(surf, out->rect, data, hole_len * n, err);
    }
    free(data);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    const pc_layer_id *above = has_layer ? &id : NULL;
    rc = pc_document_insert_above(doc, above, &l, nid, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    *rect = out->rect;
    return PC_OK;
}

/* Compute the fill on `doc` (a snapshot): *out holds the filled region + hole, or *changed=false
 * when the layer's own window is fully transparent (remove::run_remove). */
static pc_status remove_compute(pc_document *doc, const struct remove_plan *pl, yyjson_val *p, bool *changed,
                                pc_paint_region *out, bool **hole_out, size_t *hole_len, pc_error *err)
{
    const char *CMD = "paint.remove";
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    bool *hole = NULL;
    pc_status rc = remove_hole(pl, sel, &hole, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_rect win = pl->window;
    size_t w = pc_rect_width(win), h = pc_rect_height(win), hw = w * h;
    bool any = false, all = true;
    for (size_t i = 0; i < hw; i++) {
        any = any || hole[i];
        all = all && hole[i];
    }
    if (!any) {
        free(hole);
        return pc_engine_err_bad_params(err, CMD, "the stroke doesn't cover anything that can change (check the selection)");
    }
    if (all) {
        free(hole);
        return pc_engine_err_bad_params(err, CMD, "nothing is left around the stroke to fill it from");
    }
    pc_surface *surf = NULL;
    bool lock = false;
    rc = pc_engine_target_surface(doc, pl->has_layer, pl->layer, p, &surf, &lock, err);
    if (rc != PC_OK) {
        free(hole);
        return rc;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    pc_paint_region img = {0};
    if (pl->all_layers) {
        rc = composite_region(doc, false, 0, SAMPLE_ALL, win, fmt, &img, err);
    } else {
        rc = pc_paint_region_read(surf, win, &img, err);
    }
    if (rc != PC_OK) {
        free(hole);
        return rc;
    }
    /* A no-op without the fill or a history step, as the other retouch tools on an empty layer. */
    if (!pl->all_layers) {
        size_t a = SIZE_MAX;
        bool has_a = pc_paint_alpha_index(fmt, &a);
        if (has_a && a < img.ch) {
            bool empty = true;
            size_t cnt = img.ch > 0 ? img.len / img.ch : 0;
            for (size_t i = 0; i < cnt; i++) {
                if (img.data[i * img.ch + a] > 0.0f) {
                    empty = false;
                    break;
                }
            }
            if (empty) {
                free(hole);
                pc_paint_region_fini(&img);
                *changed = false;
                return PC_OK;
            }
        }
    }
    float *data = NULL;
    rc = pc_algo_remove(w, h, img.ch, img.data, img.len, hole, hw, REMOVE_SEED, NULL, &data, err);
    if (rc != PC_OK) {
        free(hole);
        pc_paint_region_fini(&img);
        return rc;
    }
    clamp_samples(fmt, data, img.len);
    out->rect = win;
    out->ch = img.ch;
    out->data = data;
    out->len = img.len;
    pc_paint_region_fini(&img);
    *hole_out = hole;
    *hole_len = hw;
    *changed = true;
    return PC_OK;
}

struct remove_apply {
    pc_paint_region out;
    bool *hole;
    size_t hole_len;
    bool has_layer;
    pc_layer_id layer;
    yyjson_val *p;
    bool new_layer;
    pc_rect dmg;   /* out */
    pc_layer_id nid; /* out */
    bool made_layer;
};

static pc_status remove_apply_cb(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    struct remove_apply *c = vctx;
    if (c->new_layer) {
        pc_status rc = remove_fill_new_layer(doc, c->has_layer, c->layer, &c->out, c->hole, c->hole_len,
                                             &c->dmg, &c->nid, err);
        if (rc != PC_OK) {
            return rc;
        }
        *ha = true;
        *a = c->nid;
        c->made_layer = true;
        return PC_OK;
    }
    (void)ha;
    (void)a;
    ca_target t = {c->has_layer, c->layer, false};
    return ca_replace(doc, t, c->p, &c->out, c->hole, c->hole_len, &c->dmg, err);
}

static pc_status remove_run(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "paint.remove";
    struct remove_plan pl = {0};
    pc_status rc = remove_make_plan(s, p, CMD, &pl, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        remove_plan_fini(&pl);
        return pc_engine_err_no_document(err);
    }
    pc_document work = {0};
    if ((rc = pc_document_clone(pc_doc_arc_get(d->doc), &work, err)) != PC_OK) {
        remove_plan_fini(&pl);
        return rc;
    }
    struct remove_apply c = {0};
    bool changed = false;
    rc = remove_compute(&work, &pl, p, &changed, &c.out, &c.hole, &c.hole_len, err);
    pc_document_fini(&work);
    remove_plan_fini(&pl);
    if (rc != PC_OK) {
        return rc;
    }
    if (!changed) {
        yyjson_mut_doc *dj = NULL;
        if (pc_engine_result_doc(out, &dj, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(dj);
        yyjson_mut_obj_add_bool(dj, root, "changed", false);
        yyjson_mut_doc_set_root(dj, root);
        return PC_OK;
    }
    c.has_layer = pl.has_layer;
    c.layer = pl.layer;
    c.p = p;
    c.new_layer = pl.new_layer;
    c.dmg = PC_RECT_EMPTY;
    rc = pc_engine_session_edit(s, "Remove", remove_apply_cb, &c, err);
    pc_paint_region_fini(&c.out);
    free(c.hole);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->last_damage = c.dmg;
        st->has_last_damage = true;
    }
    yyjson_mut_doc *dj = NULL;
    if (pc_engine_result_doc(out, &dj, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(dj);
    yyjson_mut_val *da = yyjson_mut_arr(dj);
    yyjson_mut_arr_add_int(dj, da, c.dmg.x0);
    yyjson_mut_arr_add_int(dj, da, c.dmg.y0);
    yyjson_mut_arr_add_int(dj, da, (int64_t)pc_rect_width(c.dmg));
    yyjson_mut_arr_add_int(dj, da, (int64_t)pc_rect_height(c.dmg));
    yyjson_mut_obj_add_val(dj, root, "damage", da);
    if (c.made_layer) {
        yyjson_mut_obj_add_uint(dj, root, "layer", c.nid);
    }
    yyjson_mut_doc_set_root(dj, root);
    return PC_OK;
}
