/* Free Transform / Transform (port of crates/engine/src/transform_cmds.rs, E4 · deferred
 * transform-engine follow-up, E-tail ET1).
 *
 * The scriptable edit.transform command projectively transforms a layer (its pixels, a linked
 * mask and vector mask, type / shape / smart geometry), the selection outline, or - when a
 * "target" names one - a lone unlinked mask, an alpha channel or the Quick Mask. The frame-based
 * presets (edit.transform.{rotate180,rotate90Cw,rotate90Ccw,flipHorizontal,flipVertical}) build an
 * exact pixel-grid matrix and dispatch to it; they live in extra_cmds.rs in the reference but were
 * deferred there until edit.transform existed, and are registered here (no other chunk registers
 * them). The reference's UI-only transform-tool commands (edit.freeTransform, edit.transform.scale
 * / rotate / skew / distort / perspective, edit.freeTransformCopy) are shell view-state, not engine
 * commands (menus.rs UI_COMMANDS), so they are not here.
 *
 * Numbers follow the reference exactly (f32/f64 as in Rust; warps route through src/algo transform
 * which uses Keys bicubic a = -0.75). Nothing crashes on hostile params: non-finite matrices,
 * singular transforms and degenerate quads are rejected, and a result past the pixel budget is
 * refused before anything is built. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/params.h"
#include "algo/transform.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "doc/text.h"
#include "doc/vector.h"
#include "geom/geom.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "allocation.h"
#include "channel_cmds.h"
#include "engine_internal.h"
#include "extra_cmds.h"
#include "fill_cmds.h"
#include "layer_cmds.h"
#include "smart_cmds.h"
#include "transform_cmds.h"
#include "type_cmds.h"
#include "vector_cmds.h"

/* `top` (straight alpha) over `dst` (straight RGBA, source-over), over top's content bounds
 * (transform_cmds::composite_over; the float-RGBA drop path used by the floating selection). */
void pc_engine_transform_composite_over(pc_surface *dst, const pc_surface *top, pc_error *err)
{
    pc_rect b = pc_surface_content_bounds(top);
    if (pc_rect_is_empty(b)) {
        return;
    }
    size_t w = pc_rect_width(b), h = pc_rect_height(b);
    size_t count = w * h;
    float (*t)[4] = malloc(count * sizeof(float[4]));
    float (*d)[4] = malloc(count * sizeof(float[4]));
    if (t == NULL || d == NULL) {
        free(t);
        free(d);
        if (err != NULL) {
            (void)pc_fail(err, PC_ERR_NOMEM, "compositing pixels");
        }
        return;
    }
    if (pc_surface_read_rgba_into(top, b, t, count, NULL) != PC_OK ||
        pc_surface_read_rgba_into(dst, b, d, count, NULL) != PC_OK) {
        free(t);
        free(d);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        float ta = t[i][3], da = d[i][3];
        float oa = ta + da * (1.0f - ta);
        if (oa > 0.0f) {
            for (int c = 0; c < 3; c++) {
                d[i][c] = (t[i][c] * ta + d[i][c] * da * (1.0f - ta)) / oa;
            }
        }
        d[i][3] = oa;
    }
    pc_pixel_format fmt = pc_surface_format(dst);
    size_t n = pc_pixel_format_channels(fmt);
    float *raw = malloc(count * (n == 0 ? 1 : n) * sizeof(float));
    if (raw == NULL) {
        free(t);
        free(d);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        float enc[PC_RASTER_MAX_CHANNELS];
        size_t m = pc_from_rgba(fmt, d[i], enc);
        for (size_t c = 0; c < n; c++) {
            raw[i * n + c] = c < m ? enc[c] : 0.0f;
        }
    }
    (void)pc_surface_write_region(dst, b, raw, count * n, NULL);
    free(raw);
    free(t);
    free(d);
}

/* ---- transform_bounds hook (moved from hooks.c, E4) --------------------------------------- */

/* Layers a transform moves as a whole, even with a selection (transform_cmds::moves_whole). */
static bool moves_whole(const pc_layer *l)
{
    switch (l->content.kind) {
    case PC_LAYER_GROUP:
    case PC_LAYER_TEXT:
    case PC_LAYER_SHAPE:
    case PC_LAYER_SMART:
        return true;
    default:
        return false;
    }
}

/* A group moves all its descendants (transform_cmds::whole_layer_bounds). */
static pc_rect whole_layer_bounds(const pc_layer *l)
{
    pc_rect content = PC_RECT_EMPTY;
    if (pc_layer_is_group(l)) {
        size_t n = 0;
        const pc_layer *kids = pc_layer_children(l, &n);
        for (size_t i = 0; i < n; i++) {
            pc_rect b = whole_layer_bounds(&kids[i]);
            content = pc_rect_is_empty(content) ? b : pc_rect_union(content, b);
        }
    } else {
        const pc_surface *surf = pc_layer_surface(l);
        if (surf != NULL) {
            content = pc_surface_content_bounds(surf);
        }
    }
    if (pc_rect_is_empty(content) && l->has_mask) {
        content = pc_surface_content_bounds(&l->mask.surface);
    }
    return content;
}

pc_rect pc_engine_hook_transform_bounds(const pc_document *doc, const pc_layer *l)
{
    pc_rect content = whole_layer_bounds(l);
    if (doc->has_selection && !moves_whole(l)) {
        return pc_rect_intersect(content, pc_surface_content_bounds(&doc->selection));
    }
    return content;
}

/* ======================================================================================= *
 * Helpers shared by the transform engine.
 * ======================================================================================= */

/* std::f64::consts::PI (M_PI is not ISO C). */
static const double PI = 3.14159265358979323846;

static pc_pixel_format with_alpha_fmt(pc_pixel_format f)
{
    f.alpha = true;
    return f;
}

static float surf_default0(const pc_surface *s)
{
    float d[PC_RASTER_MAX_CHANNELS];
    pc_surface_default_pixel(s, d);
    return d[0];
}

/* Normal-blend `top` over `dst` (both straight alpha, same format), in the surfaces' own channels
 * (transform_cmds::composite_over). */
static pc_status composite_over_native(pc_surface *dst, const pc_surface *top, pc_error *err)
{
    pc_rect b = pc_surface_content_bounds(top);
    if (pc_rect_is_empty(b)) {
        return PC_OK;
    }
    size_t n = pc_pixel_format_channels(pc_surface_format(dst));
    if (n == 0) {
        return PC_OK;
    }
    size_t a = n - 1;
    float *t = NULL, *d = NULL;
    size_t tl = 0, dl = 0;
    pc_status rc = pc_engine_alloc_read_region(top, b, "compositing transformed pixels", &t, &tl, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_alloc_read_region(dst, b, "compositing transformed pixels", &d, &dl, err);
    if (rc != PC_OK) {
        free(t);
        return rc;
    }
    size_t count = tl / n;
    for (size_t i = 0; i < count; i++) {
        float *dp = &d[i * n];
        const float *tp = &t[i * n];
        float ta = tp[a], da = dp[a];
        float oa = ta + da * (1.0f - ta);
        if (oa > 0.0f) {
            for (size_t c = 0; c < a; c++) {
                dp[c] = (tp[c] * ta + dp[c] * da * (1.0f - ta)) / oa;
            }
        }
        dp[a] = oa;
    }
    rc = pc_surface_write_region(dst, b, d, dl, err);
    free(t);
    free(d);
    return rc;
}

pc_status pc_engine_transform_composite_over_native(pc_surface *dst, const pc_surface *top,
                                                    pc_error *err)
{
    return composite_over_native(dst, top, err);
}

/* Split a layer surface by a selection: (selected pixels, everything else), both with alpha
 * (transform_cmds::split_selected). *lifted and *rest are initialised on success. */
static pc_status split_selected(const pc_surface *surf, const pc_surface *sel, pc_surface *lifted,
                                pc_surface *rest, pc_error *err)
{
    pc_pixel_format wa = with_alpha_fmt(pc_surface_format(surf));
    pc_surface_init(lifted, wa);
    pc_status rc = pc_surface_convert(surf, wa, rest, err);
    if (rc != PC_OK) {
        pc_surface_fini(lifted);
        return rc;
    }
    pc_rect src = pc_surface_content_bounds(surf);
    if (pc_rect_is_empty(src)) {
        return PC_OK;
    }
    size_t n = pc_pixel_format_channels(wa);
    if (n == 0) { /* an alpha format always has >= 2 channels; keeps the divisor provably non-zero */
        pc_surface_fini(lifted);
        pc_surface_fini(rest);
        return pc_fail(err, PC_ERR_INVALID, "surface has no channels");
    }
    float *px = NULL, *lp = NULL;
    size_t len = 0;
    rc = pc_engine_alloc_read_region(rest, src, "transforming selected pixels", &px, &len, err);
    if (rc != PC_OK) {
        goto fail;
    }
    rc = pc_engine_alloc_filled(len, sizeof(float), "transforming selected pixels", (void **)&lp, err);
    if (rc != PC_OK) {
        goto fail;
    }
    memcpy(lp, px, len * sizeof(float));
    float clear[PC_RASTER_MAX_CHANNELS];
    pc_surface_default_pixel(rest, clear);
    if (clear[n - 1] > 0.0f) {
        for (size_t c = 0; c < n; c++) {
            clear[c] = 0.0f;
        }
    }
    uint32_t w = pc_rect_width(src);
    size_t count = len / n;
    for (size_t i = 0; i < count; i++) {
        int32_t x = src.x0 + (int32_t)(i % w), y = src.y0 + (int32_t)(i / w);
        float k = pc_surface_sample_channel(sel, x, y, 0);
        lp[i * n + (n - 1)] *= k;
        px[i * n + (n - 1)] *= 1.0f - k;
        if (px[i * n + (n - 1)] <= 0.0f) {
            for (size_t c = 0; c < n; c++) {
                px[i * n + c] = clear[c];
            }
        }
    }
    rc = pc_surface_write_region(lifted, src, lp, len, err);
    if (rc != PC_OK) {
        goto fail;
    }
    pc_surface_prune(lifted);
    rc = pc_surface_write_region(rest, src, px, len, err);
    if (rc != PC_OK) {
        goto fail;
    }
    free(px);
    free(lp);
    return PC_OK;
fail:
    free(px);
    free(lp);
    pc_surface_fini(lifted);
    pc_surface_fini(rest);
    return rc;
}

/* transform_cmds::warp_gray. */
pc_status pc_engine_transform_warp_gray(const pc_surface *s, const pc_algo_homography *h,
                                        pc_algo_interp interp, pc_surface *out, pc_error *err)
{
    float def = surf_default0(s);
    pc_pixel_format fmt = pc_surface_format(s);
    pc_rect src = pc_surface_content_bounds(s);
    pc_surface_init_default(out, fmt, &def, 1);
    if (pc_rect_is_empty(src)) {
        return PC_OK;
    }
    pc_pixel_format wa = with_alpha_fmt(fmt);
    pc_surface tmp;
    pc_surface_init(&tmp, wa);
    float *v = NULL, *lifted = NULL, *old = NULL, *flat = NULL;
    size_t vlen = 0;
    pc_surface warped = {0};
    bool have_warp = false;
    pc_status rc = pc_engine_alloc_read_region(s, src, "transforming a mask or channel", &v, &vlen, err);
    if (rc != PC_OK) {
        goto done;
    }
    rc = pc_engine_alloc_filled(vlen * 2, sizeof(float), "transforming a mask or channel", (void **)&lifted, err);
    if (rc != PC_OK) {
        goto done;
    }
    for (size_t i = 0; i < vlen; i++) {
        lifted[i * 2] = v[i];
        lifted[i * 2 + 1] = 1.0f;
    }
    rc = pc_surface_write_region(&tmp, src, lifted, vlen * 2, err);
    if (rc != PC_OK) {
        goto done;
    }
    rc = pc_algo_warp_surface(&tmp, src, h, interp, &warped, err);
    if (rc != PC_OK) {
        goto done;
    }
    have_warp = true;
    /* Clear the old content region to the default, then composite the warped content over it.
     * Two sparse regions, not one dense block, so a far-away result costs only its own tiles. */
    rc = pc_engine_alloc_filled(vlen, sizeof(float), "transforming a mask or channel", (void **)&old, err);
    if (rc != PC_OK) {
        goto done;
    }
    for (size_t i = 0; i < vlen; i++) {
        old[i] = def;
    }
    rc = pc_surface_write_region(out, src, old, vlen, err);
    if (rc != PC_OK) {
        goto done;
    }
    pc_rect b = pc_surface_content_bounds(&warped);
    if (!pc_rect_is_empty(b)) {
        float *pw = NULL;
        size_t pl = 0;
        rc = pc_engine_alloc_read_region(&warped, b, "flattening a transformed mask or channel", &pw, &pl, err);
        if (rc != PC_OK) {
            goto done;
        }
        size_t fc = pl / 2;
        rc = pc_engine_alloc_filled(fc, sizeof(float), "flattening a transformed mask or channel", (void **)&flat, err);
        if (rc != PC_OK) {
            free(pw);
            goto done;
        }
        for (size_t i = 0; i < fc; i++) {
            flat[i] = pw[i * 2] * pw[i * 2 + 1] + def * (1.0f - pw[i * 2 + 1]);
        }
        free(pw);
        rc = pc_surface_write_region(out, b, flat, fc, err);
        if (rc != PC_OK) {
            goto done;
        }
    }
    pc_surface_prune(out);
done:
    free(v);
    free(lifted);
    free(old);
    free(flat);
    pc_surface_fini(&tmp);
    if (have_warp) {
        pc_surface_fini(&warped);
    }
    if (rc != PC_OK) {
        pc_surface_fini(out);
    }
    return rc;
}

/* transform_cmds::warp_gray_selected: only the selected values of a single grey channel move (the
 * vacated area reads as the default); whole-surface warp_gray for anything that is not one. */
static pc_status warp_gray_selected(const pc_surface *s, const pc_surface *sel,
                                    const pc_algo_homography *h, pc_algo_interp interp,
                                    pc_surface *out, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(s);
    if (fmt.alpha || pc_pixel_format_channels(fmt) != 1) {
        return pc_engine_transform_warp_gray(s, h, interp, out, err);
    }
    float def = surf_default0(s);
    pc_pixel_format wa = with_alpha_fmt(fmt);
    pc_surface lifted;
    pc_surface_init(&lifted, wa);
    pc_status rc = pc_surface_clone(s, out, err); /* `rest` */
    if (rc != PC_OK) {
        pc_surface_fini(&lifted);
        return rc;
    }
    pc_rect area = pc_surface_content_bounds(sel);
    float *v = NULL, *lp = NULL, *rp = NULL;
    pc_surface warped = {0};
    bool have_warp = false;
    if (pc_rect_is_empty(area)) {
        goto done_ok;
    }
    size_t vlen = 0;
    rc = pc_engine_alloc_read_region(s, area, "transforming a selected mask or channel", &v, &vlen, err);
    if (rc != PC_OK) {
        goto fail;
    }
    rc = pc_engine_alloc_filled(vlen * 2, sizeof(float), "transforming a selected mask or channel", (void **)&lp, err);
    if (rc != PC_OK) {
        goto fail;
    }
    rc = pc_engine_alloc_filled(vlen, sizeof(float), "transforming a selected mask or channel", (void **)&rp, err);
    if (rc != PC_OK) {
        goto fail;
    }
    uint32_t w = pc_rect_width(area);
    for (size_t i = 0; i < vlen; i++) {
        int32_t x = area.x0 + (int32_t)(i % w), y = area.y0 + (int32_t)(i / w);
        float k = pc_surface_sample_channel(sel, x, y, 0);
        lp[i * 2] = v[i];
        lp[i * 2 + 1] = k;
        rp[i] = v[i] * (1.0f - k) + def * k;
    }
    pc_surface tmp;
    pc_surface_init(&tmp, wa);
    rc = pc_surface_write_region(&tmp, area, lp, vlen * 2, err);
    if (rc == PC_OK) {
        pc_surface_prune(&tmp);
        rc = pc_surface_write_region(out, area, rp, vlen, err);
    }
    if (rc == PC_OK) {
        pc_surface_prune(out);
        pc_rect src2 = pc_surface_content_bounds(&tmp);
        if (!pc_rect_is_empty(src2)) {
            rc = pc_algo_warp_surface(&tmp, src2, h, interp, &warped, err);
            have_warp = (rc == PC_OK);
        }
    }
    pc_surface_fini(&tmp);
    if (rc != PC_OK) {
        goto fail;
    }
    if (have_warp) {
        pc_rect b = pc_surface_content_bounds(&warped);
        if (!pc_rect_is_empty(b)) {
            float *moved = NULL, *under = NULL, *flat = NULL;
            size_t ml = 0, ul = 0;
            rc = pc_engine_alloc_read_region(&warped, b, "compositing transformed mask pixels", &moved, &ml, err);
            if (rc == PC_OK) {
                rc = pc_engine_alloc_read_region(out, b, "compositing transformed mask pixels", &under, &ul, err);
            }
            if (rc == PC_OK) {
                rc = pc_engine_alloc_filled(ul, sizeof(float), "compositing transformed mask pixels", (void **)&flat, err);
            }
            if (rc == PC_OK) {
                for (size_t i = 0; i < ul; i++) {
                    flat[i] = moved[i * 2] * moved[i * 2 + 1] + under[i] * (1.0f - moved[i * 2 + 1]);
                }
                rc = pc_surface_write_region(out, b, flat, ul, err);
            }
            free(moved);
            free(under);
            free(flat);
            if (rc != PC_OK) {
                goto fail;
            }
        }
    }
    pc_surface_prune(out);
done_ok:
    free(v);
    free(lp);
    free(rp);
    pc_surface_fini(&lifted);
    if (have_warp) {
        pc_surface_fini(&warped);
    }
    return PC_OK;
fail:
    free(v);
    free(lp);
    free(rp);
    pc_surface_fini(&lifted);
    if (have_warp) {
        pc_surface_fini(&warped);
    }
    pc_surface_fini(out);
    return rc;
}

/* ---- pixel budget (#3773) ----------------------------------------------------------------- */

/* Bounding box of the four mapped corners of `src` (rounded out, clamped to +-2^20); false when a
 * corner is non-finite or the map is not invertible (transform_cmds::warp_output_bounds). */
static bool warp_output_bounds(pc_rect src, const pc_algo_homography *h, pc_rect *out)
{
    if (pc_rect_is_empty(src)) {
        return false;
    }
    pc_algo_homography inv;
    if (!pc_algo_homography_inverse(h, &inv)) {
        return false;
    }
    int32_t xs[4] = {src.x0, src.x1, src.x1, src.x0};
    int32_t ys[4] = {src.y0, src.y0, src.y1, src.y1};
    double minx = 0, maxx = 0, miny = 0, maxy = 0;
    for (int i = 0; i < 4; i++) {
        double ox, oy;
        pc_algo_homography_apply(h, (double)xs[i], (double)ys[i], &ox, &oy);
        if (!isfinite(ox) || !isfinite(oy)) {
            return false;
        }
        if (i == 0) {
            minx = maxx = ox;
            miny = maxy = oy;
        } else {
            minx = fmin(minx, ox);
            maxx = fmax(maxx, ox);
            miny = fmin(miny, oy);
            maxy = fmax(maxy, oy);
        }
    }
    /* The reference clamps to +-2^20 then does a saturating float->int cast; clamp both ends to
     * that window before the cast, which keeps the budget sound and avoids the C UB of casting an
     * out-of-range double to int32 (a 1e300 scale, say). */
    const double lim = (double)(1 << 20);
    int32_t bx0 = (int32_t)fmin(fmax(floor(minx), -lim), lim) - 1;
    int32_t by0 = (int32_t)fmin(fmax(floor(miny), -lim), lim) - 1;
    int32_t bx1 = (int32_t)fmin(fmax(ceil(maxx), -lim), lim) + 1;
    int32_t by1 = (int32_t)fmin(fmax(ceil(maxy), -lim), lim) + 1;
    *out = pc_rect_make(bx0, by0, bx1, by1);
    return true;
}

/* Refuse a warp of the content inside `src` by `h` whose result would exceed the pixel budget,
 * before anything is built (transform_cmds::check_budget). */
static pc_status check_budget(pc_rect src, const pc_algo_homography *h, const char *what, pc_error *err)
{
    pc_rect b;
    if (!warp_output_bounds(src, h, &b)) {
        return PC_OK;
    }
    uint64_t w = pc_rect_width(b), ht = pc_rect_height(b);
    if (w * ht > PC_ENGINE_MAX_TRANSFORM_PIXELS) {
        char detail[192];
        (void)snprintf(detail, sizeof detail,
                       "the transformed %s would be %llu x %llu px, more than the %llu px limit", what,
                       (unsigned long long)w, (unsigned long long)ht,
                       (unsigned long long)PC_ENGINE_MAX_TRANSFORM_PIXELS);
        return pc_engine_alloc_error(err, "transforming pixels", detail);
    }
    return PC_OK;
}

/* ---- locks -------------------------------------------------------------------------------- */

/* The locks a transform of `l` meets: its own and `group`'s. A Background's position /
 * transparency locks do not count (it becomes a normal layer first). */
static pc_locks transform_locks(const pc_layer *l, pc_locks group, bool background)
{
    pc_locks own = l->locks;
    if (background) {
        own.position = false;
        own.transparency = false;
    }
    return pc_locks_union(own, group);
}

pc_locks pc_engine_transform_group_locks(const pc_document *doc, pc_layer_id id)
{
    pc_layer_path path;
    pc_locks def = {false, false, false, false, false};
    if (pc_document_path_of(doc, id, &path, NULL) != PC_OK) {
        return def;
    }
    pc_locks g = (path.len == 0) ? def : pc_document_locks_at(doc, path.idx, path.len - 1);
    pc_layer_path_fini(&path);
    return g;
}

bool pc_engine_transform_locked(const pc_document *doc, pc_layer_id id)
{
    const pc_layer *root = pc_document_layer(doc, id);
    if (root == NULL) {
        return false;
    }
    pc_locks group = pc_engine_transform_group_locks(doc, id);
    bool background = pc_document_is_background(doc, id);
    /* Explicit stack rather than recursion: documents can nest deeply. */
    struct frame {
        const pc_layer *l;
        pc_locks group;
        bool background;
    };
    struct frame stack[PC_MAX_GROUP_DEPTH + 2];
    size_t top = 0;
    stack[top++] = (struct frame){root, group, background};
    while (top > 0) {
        struct frame f = stack[--top];
        pc_locks locks = transform_locks(f.l, f.group, f.background);
        if (locks.position || locks.all) {
            return true;
        }
        if (f.l->content.kind == PC_LAYER_GROUP) {
            const pc_group *g = &f.l->content.u.group;
            for (size_t i = 0; i < g->nchildren && top < PC_MAX_GROUP_DEPTH + 2; i++) {
                stack[top++] = (struct frame){&g->children[i], locks, false};
            }
        }
    }
    return false;
}

/* ---- the layer transform ------------------------------------------------------------------ */

/* check_budget for every warp transform_layer would make of `l` (for a group, of every layer
 * inside it), so a transform that does not fit is refused before any layer is built
 * (transform_cmds::check_layer_budget). */
static pc_status check_layer_budget_rec(const pc_surface *sel, const pc_layer *l,
                                        const pc_algo_homography *h, pc_error *err)
{
    switch (l->content.kind) {
    case PC_LAYER_GROUP: {
        const pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            pc_status rc = check_layer_budget_rec(NULL, &g->children[i], h, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        break;
    }
    case PC_LAYER_TEXT:
    case PC_LAYER_SHAPE:
        break;
    case PC_LAYER_SMART: {
        const pc_smart_object *sm = &l->content.u.smart;
        if (sm->has_cache) {
            pc_status rc = check_budget(pc_surface_content_bounds(&sm->cache), h, "smart object", err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        if (sm->has_filter_mask) {
            pc_status rc = check_budget(pc_surface_content_bounds(&sm->filter_mask.surface), h,
                                        "smart filter mask", err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        break;
    }
    default: {
        const pc_surface *surf = pc_layer_surface(l);
        if (surf != NULL) {
            char what[64];
            (void)snprintf(what, sizeof what, "layer \"%.*s\"", (int)l->name.len, l->name.ptr);
            pc_status rc = check_budget(pc_surface_content_bounds(surf), h, what, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        break;
    }
    }
    if (l->has_mask && l->mask.linked) {
        /* With a selection only its region of the mask moves (warp_gray_selected). */
        pc_rect src = (sel != NULL && !moves_whole(l)) ? pc_surface_content_bounds(sel)
                                                       : pc_surface_content_bounds(&l->mask.surface);
        pc_status rc = check_budget(src, h, "layer mask", err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return PC_OK;
}

static pc_status apply_transform(const pc_surface *doc_sel, pc_locks group, pc_layer *l,
                                 const pc_algo_homography *h, const pc_affine *affine,
                                 pc_algo_interp interp, pc_error *err)
{
    pc_status rc = pc_engine_alloc_checkpoint("transforming pixels", err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_locks locks = transform_locks(l, group, false);
    if (locks.position || locks.all) {
        return pc_engine_err_other(err, "layer \"%.*s\" is locked", (int)l->name.len, l->name.ptr);
    }
    bool identity = pc_algo_homography_is_identity(h);
    if (identity) {
        if (l->content.kind == PC_LAYER_GROUP) {
            pc_group *g = &l->content.u.group;
            for (size_t i = 0; i < g->nchildren; i++) {
                rc = apply_transform(NULL, locks, &g->children[i], h, affine, interp, err);
                if (rc != PC_OK) {
                    return rc;
                }
            }
        }
        return PC_OK;
    }
    /* With a selection only the selected pixels move, and so only the same region of a linked
     * mask (#205). Groups, type, shapes and smart objects move whole. */
    const pc_surface *mask_sel = moves_whole(l) ? NULL : doc_sel;
    switch (l->content.kind) {
    case PC_LAYER_GROUP: {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            rc = apply_transform(NULL, locks, &g->children[i], h, affine, interp, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        break;
    }
    case PC_LAYER_TEXT: {
        if (affine == NULL) {
            return pc_engine_err_other(
                err, "Distort and Perspective need rasterized type (Type > Rasterize Type Layer)");
        }
        pc_text_layer *t = &l->content.u.text;
        t->transform = pc_affine_mul(*affine, t->transform);
        break;
    }
    case PC_LAYER_SHAPE: {
        pc_shape_layer *sh = &l->content.u.shape;
        if (affine != NULL) {
            pc_engine_transform_shape(sh, affine);
        } else {
            rc = pc_engine_map_shape(sh, h, NULL, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        break;
    }
    case PC_LAYER_SMART: {
        pc_smart_object *sm = &l->content.u.smart;
        if (affine != NULL) {
            pc_engine_smart_transform_placement(sm, *affine);
        } else {
            pc_algo_homography placement;
            pc_engine_smart_placement(sm, &placement);
            pc_algo_homography np = pc_algo_homography_mul(h, &placement);
            pc_engine_smart_set_placement(sm, &np);
        }
        if (sm->has_cache) {
            pc_surface c;
            rc = pc_algo_warp_surface(&sm->cache, pc_surface_content_bounds(&sm->cache), h, interp, &c, err);
            if (rc != PC_OK) {
                return rc;
            }
            pc_surface_fini(&sm->cache);
            sm->cache = c;
        }
        if (sm->has_filter_mask) {
            pc_surface m;
            rc = pc_engine_transform_warp_gray(&sm->filter_mask.surface, h, interp, &m, err);
            if (rc != PC_OK) {
                return rc;
            }
            pc_surface_fini(&sm->filter_mask.surface);
            sm->filter_mask.surface = m;
        }
        break;
    }
    default: {
        pc_surface *surf = pc_layer_surface_mut(l);
        if (surf != NULL) {
            pc_rect src = pc_surface_content_bounds(surf);
            if (doc_sel != NULL) {
                pc_surface lifted, rest, moved;
                rc = split_selected(surf, doc_sel, &lifted, &rest, err);
                if (rc != PC_OK) {
                    return rc;
                }
                rc = pc_algo_warp_surface(&lifted, src, h, interp, &moved, err);
                pc_surface_fini(&lifted);
                if (rc != PC_OK) {
                    pc_surface_fini(&rest);
                    return rc;
                }
                rc = composite_over_native(&rest, &moved, err);
                pc_surface_fini(&moved);
                if (rc != PC_OK) {
                    pc_surface_fini(&rest);
                    return rc;
                }
                pc_surface_prune(&rest);
                pc_surface_fini(surf);
                *surf = rest;
            } else {
                pc_surface moved;
                rc = pc_algo_warp_surface(surf, src, h, interp, &moved, err);
                if (rc != PC_OK) {
                    return rc;
                }
                pc_surface_fini(surf);
                *surf = moved;
            }
        }
        break;
    }
    }
    if (l->has_mask && l->mask.linked) {
        pc_surface m;
        rc = (mask_sel != NULL) ? warp_gray_selected(&l->mask.surface, mask_sel, h, interp, &m, err)
                                : pc_engine_transform_warp_gray(&l->mask.surface, h, interp, &m, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&l->mask.surface);
        l->mask.surface = m;
    }
    if (l->has_vector_mask && l->vector_mask.linked) {
        pc_path np;
        if (affine != NULL) {
            rc = pc_path_transform(&l->vector_mask.path, *affine, &np, err);
        } else {
            rc = pc_engine_map_path(&l->vector_mask.path, h, NULL, &np, err);
        }
        if (rc != PC_OK) {
            return rc;
        }
        pc_path_fini(&l->vector_mask.path);
        l->vector_mask.path = np;
    }
    return PC_OK;
}

pc_status pc_engine_transform_layer(const pc_surface *doc_sel, pc_locks group, pc_layer *l,
                                    const pc_algo_homography *h, const pc_affine *affine,
                                    pc_algo_interp interp, pc_error *err)
{
    if (!pc_algo_homography_is_identity(h)) {
        pc_status rc = check_layer_budget_rec(doc_sel, l, h, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return apply_transform(doc_sel, group, l, h, affine, interp, err);
}

/* Re-render type / shape / smart caches after the geometry change (transform_cmds::refresh_text):
 * type and smart layers re-render from their new transform, shapes from their mapped path. */
static void refresh_rec(const pc_document *snapshot, pc_layer *l)
{
    switch (l->content.kind) {
    case PC_LAYER_TEXT:
        pc_engine_hook_refresh_text(snapshot, l);
        break;
    case PC_LAYER_SHAPE:
        pc_engine_hook_refresh_shape(snapshot, l);
        break;
    case PC_LAYER_SMART:
        pc_engine_hook_refresh_smart(snapshot, l);
        break;
    case PC_LAYER_GROUP: {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            refresh_rec(snapshot, &g->children[i]);
        }
        break;
    }
    default:
        break;
    }
}

/* ======================================================================================= *
 * Command surface.
 * ======================================================================================= */

#define TRANSFORM_PARAMS                                                                            \
    "{\"layer\":id?,\"rect\":[x0,y0,x1,y1]? (source frame; default = layer content \xE2\x88\xA9 "   \
    "selection),\"quad\":[[x,y]\xC3\x97" "4]? (where the frame's corners go, clockwise from "        \
    "top-left),\"matrix\":[a,b,c,d,e,f]? (affine alternative),\"scaleX\":%?,\"scaleY\":%?,"         \
    "\"rotate\":deg?,\"dx\":px?,\"dy\":px? (relative alternative: scale and rotate about the "      \
    "frame's centre, then move, as a Photoshop action records a Free Transform),"                   \
    "\"interpolation\":\"bicubic|bilinear|nearest\"=\"bicubic\",\"target\":\"pixels\"|\"mask\"|"    \
    "\"quickMask\"|{\"channel\":i}=Channels panel target (an unlinked mask, an alpha channel or "   \
    "the Quick Mask transforms alone; a linked mask moves with its layer)}"

static pc_status bad(pc_error *err, const char *msg)
{
    return pc_engine_err_other(err, "%s", msg);
}

/* The effective transform target: the explicit "target" param, or - when neither a target nor a
 * layer is named - the Channels panel's targeted alpha channel / Quick Mask (#2835). */
static void effective_target(pc_engine_session *s, yyjson_val *p, pc_engine_pixel_target *tgt,
                             size_t *alpha_idx)
{
    size_t ai = 0;
    pc_engine_pixel_target t = pc_engine_target_of(p, &ai);
    if (t == PC_ENGINE_PIXTGT_PIXELS && pc_json_get(p, "target") == NULL && pc_json_get(p, "layer") == NULL) {
        bool quick = false;
        size_t pai = 0;
        if (pc_engine_targeted_channel(s, &quick, &pai)) {
            t = quick ? PC_ENGINE_PIXTGT_QUICK_MASK : PC_ENGINE_PIXTGT_ALPHA;
            ai = pai;
        }
    }
    *tgt = t;
    *alpha_idx = ai;
}

/* The lone surface a transform moves by itself (an unlinked mask, an alpha channel, the Quick
 * Mask), or NULL when the target is the layer (pixels, or a linked mask that moves with it).
 * `has_layer`/`layer` give the active layer for the Mask target. */
static pc_status lone_target(const pc_document *doc, bool has_layer, pc_layer_id layer,
                             pc_engine_pixel_target tgt, size_t alpha_idx, const pc_surface **out,
                             pc_error *err)
{
    *out = NULL;
    switch (tgt) {
    case PC_ENGINE_PIXTGT_PIXELS:
        return PC_OK;
    case PC_ENGINE_PIXTGT_MASK: {
        if (!has_layer) {
            return pc_engine_err_other(err, "no active layer");
        }
        const pc_layer *l = pc_document_layer(doc, layer);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, layer);
        }
        if (!l->has_mask) {
            return pc_engine_err_other(err, "layer has no mask");
        }
        if (!l->mask.linked) {
            *out = &l->mask.surface;
        }
        return PC_OK;
    }
    case PC_ENGINE_PIXTGT_ALPHA:
        if (alpha_idx >= doc->nchannels) {
            return pc_engine_err_other(err, "no alpha channel %zu (document has %zu)", alpha_idx,
                                       doc->nchannels);
        }
        *out = &doc->channels[alpha_idx].surface;
        return PC_OK;
    case PC_ENGINE_PIXTGT_QUICK_MASK:
        if (!doc->has_quick_mask) {
            return pc_engine_err_other(err, "not in Quick Mask mode");
        }
        *out = &doc->quick_mask.surface;
        return PC_OK;
    }
    return PC_OK;
}

/* The frame a transform of a lone target starts from: its painted content (the whole canvas when
 * nothing is painted), within the selection (transform_cmds::target_bounds). */
static pc_rect target_bounds(const pc_document *doc, const pc_surface *surf)
{
    pc_rect content = pc_surface_content_bounds(surf);
    if (pc_rect_is_empty(content)) {
        content = pc_document_bounds(doc);
    }
    if (doc->has_selection) {
        return pc_rect_intersect(content, pc_surface_content_bounds(&doc->selection));
    }
    return content;
}

/* The relative form (scaleX, scaleY in %, rotate in degrees cw, dx, dy in px), as Photoshop
 * records a Free Transform. *present is false when none is given. */
static pc_status relative_matrix(yyjson_val *p, const double rect[4], pc_algo_homography *out,
                                 bool *present, pc_error *err)
{
    *present = false;
    double vals[5];
    bool has[5] = {false, false, false, false, false};
    const char *keys[5] = {"scaleX", "scaleY", "rotate", "dx", "dy"};
    for (int i = 0; i < 5; i++) {
        yyjson_val *v = pc_json_get(p, keys[i]);
        if (v == NULL || yyjson_is_null(v)) {
            continue;
        }
        double d;
        if (!pc_json_as_f64(v, &d) || !isfinite(d)) {
            char msg[64];
            (void)snprintf(msg, sizeof msg, "`%s` must be a finite number", keys[i]);
            return bad(err, msg);
        }
        vals[i] = d;
        has[i] = true;
    }
    if (!has[0] && !has[1] && !has[2] && !has[3] && !has[4]) {
        return PC_OK;
    }
    double sx = (has[0] ? vals[0] : 100.0) / 100.0;
    double sy = (has[1] ? vals[1] : 100.0) / 100.0;
    if (fabs(sx) < 1e-6 || fabs(sy) < 1e-6) {
        return bad(err, "`scaleX` and `scaleY` must not be 0");
    }
    double rot = has[2] ? vals[2] : 0.0;
    double dx = has[3] ? vals[3] : 0.0;
    double dy = has[4] ? vals[4] : 0.0;
    double cx = (rect[0] + rect[2]) / 2.0, cy = (rect[1] + rect[3]) / 2.0;
    double rad = rot * (PI / 180.0);
    double sinr = sin(rad), cosr = cos(rad);
    double a = sx * cosr, b = sx * sinr, c = -sy * sinr, d = sy * cosr;
    double e = cx + dx - a * cx - c * cy;
    double f = cy + dy - b * cx - d * cy;
    *out = (pc_algo_homography){{a, c, e, b, d, f, 0.0, 0.0, 1.0}};
    *present = true;
    return PC_OK;
}

/* p["quad"] as four [x, y] corners; false (treated as absent) when malformed. */
static bool quad_param(yyjson_val *p, double q[4][2])
{
    yyjson_val *a = pc_json_get(p, "quad");
    if (a == NULL || !yyjson_is_arr(a) || yyjson_arr_size(a) != 4) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        yyjson_val *c = yyjson_arr_get(a, i);
        if (c == NULL || !yyjson_is_arr(c) || yyjson_arr_size(c) < 2) {
            return false;
        }
        if (!pc_json_as_f64(yyjson_arr_get(c, 0), &q[i][0]) ||
            !pc_json_as_f64(yyjson_arr_get(c, 1), &q[i][1])) {
            return false;
        }
    }
    return true;
}

struct transform_ctx {
    bool has_id;
    pc_layer_id id;
    bool lone;
    pc_engine_pixel_target tgt;
    size_t alpha_idx;
    pc_algo_homography h;
    bool has_affine;
    pc_affine affine;
    pc_algo_interp interp;
};

/* Replace doc->selection with its warp by `h`, dropping it when it collapses. */
static pc_status warp_selection_outline(pc_document *doc, const pc_algo_homography *h, pc_error *err)
{
    if (!doc->has_selection) {
        return PC_OK;
    }
    pc_surface w;
    pc_status rc = pc_engine_transform_warp_gray(&doc->selection, h, PC_ALGO_INTERP_BILINEAR, &w, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_rect_is_empty(pc_surface_content_bounds(&w))) {
        pc_surface_fini(&w);
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
        return PC_OK;
    }
    pc_surface_fini(&doc->selection);
    doc->selection = w;
    return PC_OK;
}

static pc_status transform_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                pc_error *err)
{
    (void)has_active;
    (void)active;
    struct transform_ctx *c = vctx;
    bool identity = pc_algo_homography_is_identity(&c->h);
    const pc_affine *affine = c->has_affine ? &c->affine : NULL;

    if (doc->has_selection && !identity) {
        pc_status rc = check_budget(pc_surface_content_bounds(&doc->selection), &c->h, "selection", err);
        if (rc != PC_OK) {
            return rc;
        }
    }

    if (c->lone) {
        if (identity) {
            return PC_OK;
        }
        /* Resolve the mutable lone surface in this (copied) document. */
        pc_surface *surf = NULL;
        switch (c->tgt) {
        case PC_ENGINE_PIXTGT_MASK: {
            pc_layer *l = c->has_id ? pc_document_layer_mut(doc, c->id) : NULL;
            if (l == NULL || !l->has_mask) {
                return pc_engine_err_other(err, "layer has no mask");
            }
            surf = &l->mask.surface;
            break;
        }
        case PC_ENGINE_PIXTGT_ALPHA:
            if (c->alpha_idx >= doc->nchannels) {
                return pc_engine_err_other(err, "no alpha channel %zu", c->alpha_idx);
            }
            surf = &doc->channels[c->alpha_idx].surface;
            break;
        case PC_ENGINE_PIXTGT_QUICK_MASK:
            if (!doc->has_quick_mask) {
                return pc_engine_err_other(err, "not in Quick Mask mode");
            }
            surf = &doc->quick_mask.surface;
            break;
        default:
            return pc_engine_err_other(err, "no lone target");
        }
        pc_rect src =
            doc->has_selection ? pc_surface_content_bounds(&doc->selection) : pc_surface_content_bounds(surf);
        pc_status rc = check_budget(src, &c->h, "mask or channel", err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface w;
        rc = doc->has_selection
                 ? warp_gray_selected(surf, &doc->selection, &c->h, c->interp, &w, err)
                 : pc_engine_transform_warp_gray(surf, &c->h, c->interp, &w, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(surf);
        *surf = w;
        return warp_selection_outline(doc, &c->h, err);
    }

    if (!c->has_id) {
        return pc_engine_err_other(err, "no active layer");
    }
    const pc_layer *lc = pc_document_layer(doc, c->id);
    if (lc == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    bool is_group = pc_layer_is_group(lc);
    pc_locks group = pc_engine_transform_group_locks(doc, c->id);
    /* Photoshop turns the Background into a normal layer before transforming it. */
    pc_engine_background_to_layer(doc, c->id);
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_status rc =
        pc_engine_transform_layer(is_group ? NULL : sel, group, l, &c->h, affine, c->interp, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!identity) {
        pc_document snap;
        rc = pc_document_clone(doc, &snap, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_layer *l2 = pc_document_layer_mut(doc, c->id);
        if (l2 != NULL) {
            refresh_rec(&snap, l2);
        }
        pc_document_fini(&snap);
        rc = warp_selection_outline(doc, &c->h, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return PC_OK;
}

/* result_doc {"rect": [x0, y0, x1, y1]} (the source frame, as f64). */
static pc_status transform_result(pc_json_value *out, const double rect[4], pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (int i = 0; i < 4; i++) {
        yyjson_mut_arr_add_real(d, arr, rect[i]);
    }
    yyjson_mut_obj_add_val(d, root, "rect", arr);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_transform(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);

    /* The targeted layer: an explicit "layer" param, else the active layer. */
    bool has_id = false;
    pc_layer_id id = 0;
    bool explicit_present = false;
    if (pc_engine_explicit_layer(s, p, &explicit_present, &id, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (explicit_present) {
        has_id = true;
    } else if (d->has_active_layer) {
        has_id = true;
        id = d->active_layer;
    }

    pc_engine_pixel_target tgt;
    size_t alpha_idx;
    effective_target(s, p, &tgt, &alpha_idx);

    const pc_surface *lone_surf = NULL;
    pc_status rc = lone_target(doc, has_id, id, tgt, alpha_idx, &lone_surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool lone = lone_surf != NULL;
    bool channel_target = (tgt == PC_ENGINE_PIXTGT_ALPHA || tgt == PC_ENGINE_PIXTGT_QUICK_MASK);
    if (lone && channel_target) {
        has_id = false; /* a channel / Quick Mask transforms without a layer */
    } else if (!has_id) {
        return pc_engine_err_other(err, "no active layer");
    }

    /* The source frame. */
    double rect[4];
    yyjson_val *rv = pc_json_get(p, "rect");
    if (rv != NULL && yyjson_is_arr(rv) && yyjson_arr_size(rv) == 4) {
        for (size_t i = 0; i < 4; i++) {
            if (!pc_json_as_f64(yyjson_arr_get(rv, i), &rect[i])) {
                rect[i] = 0.0;
            }
        }
    } else {
        pc_rect b = PC_RECT_EMPTY;
        if (lone) {
            b = target_bounds(doc, lone_surf);
        } else if (has_id) {
            const pc_layer *l = pc_document_layer(doc, id);
            if (l == NULL) {
                return pc_engine_err_no_layer(err, id);
            }
            b = pc_engine_hook_transform_bounds(doc, l);
        }
        if (pc_rect_is_empty(b)) {
            return pc_engine_err_other(err, "nothing to transform");
        }
        rect[0] = (double)b.x0;
        rect[1] = (double)b.y0;
        rect[2] = (double)b.x1;
        rect[3] = (double)b.y1;
    }

    /* The transform: a quad, an affine matrix, or the relative form. */
    pc_algo_homography h;
    double q[4][2];
    yyjson_val *mv = pc_json_get(p, "matrix");
    bool rel_present = false;
    if (quad_param(p, q)) {
        if (!pc_algo_homography_rect_to_quad(rect, (const double(*)[2])q, &h)) {
            return bad(err, "degenerate quad");
        }
    } else if (mv != NULL && yyjson_is_arr(mv) && yyjson_arr_size(mv) == 6) {
        double m[6];
        for (size_t i = 0; i < 6; i++) {
            if (!pc_json_as_f64(yyjson_arr_get(mv, i), &m[i])) {
                return bad(err, "matrix must be [a, b, c, d, e, f]");
            }
        }
        h = (pc_algo_homography){{m[0], m[2], m[4], m[1], m[3], m[5], 0.0, 0.0, 1.0}};
    } else if (mv != NULL && yyjson_is_arr(mv)) {
        return bad(err, "matrix must be [a, b, c, d, e, f]");
    } else {
        rc = relative_matrix(p, rect, &h, &rel_present, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (!rel_present) {
            return bad(err, "pass `quad` (where the rect's corners go), `matrix`, or "
                            "`scaleX`/`scaleY`/`rotate`/`dx`/`dy`");
        }
    }

    pc_algo_homography inv;
    if (!pc_algo_homography_inverse(&h, &inv)) {
        return bad(err, "the transform collapses the layer");
    }

    struct transform_ctx ctx = {0};
    ctx.has_id = has_id;
    ctx.id = id;
    ctx.lone = lone;
    ctx.tgt = tgt;
    ctx.alpha_idx = alpha_idx;
    ctx.h = h;
    const double *hm = h.m;
    if (fabs(hm[6]) < 1e-12 && fabs(hm[7]) < 1e-12) {
        ctx.has_affine = true;
        ctx.affine = (pc_affine){{hm[0] / hm[8], hm[3] / hm[8], hm[1] / hm[8], hm[4] / hm[8],
                                  hm[2] / hm[8], hm[5] / hm[8]}};
    }
    yyjson_val *iv = pc_json_get(p, "interpolation");
    size_t ilen = 0;
    const char *istr = iv != NULL ? pc_json_as_str(iv, &ilen) : NULL;
    ctx.interp = pc_algo_interp_parse(istr != NULL ? istr : "bicubic");

    rc = pc_engine_session_edit(s, "Free Transform", transform_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return transform_result(out, rect, err);
}

/* ---- frame-based presets (extra_cmds.rs::transform_preset, deferred there until now) ------ */

/* Dispatch a 90/180-degree turn or a flip to edit.transform as an exact pixel permutation. */
static pc_status transform_preset(pc_engine_session *s, yyjson_val *p, const char *kind,
                                  pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);

    bool has_id = d->has_active_layer;
    pc_layer_id id = d->has_active_layer ? d->active_layer : 0;

    pc_engine_pixel_target tgt;
    size_t alpha_idx;
    effective_target(s, p, &tgt, &alpha_idx);
    const pc_surface *lone_surf = NULL;
    pc_status rc = lone_target(doc, has_id, id, tgt, alpha_idx, &lone_surf, err);
    if (rc != PC_OK) {
        return rc;
    }

    pc_rect b;
    char target_json[64];
    target_json[0] = '\0';
    if (lone_surf != NULL) {
        b = target_bounds(doc, lone_surf);
        switch (tgt) {
        case PC_ENGINE_PIXTGT_MASK:
            (void)snprintf(target_json, sizeof target_json, "\"target\":\"mask\",");
            break;
        case PC_ENGINE_PIXTGT_QUICK_MASK:
            (void)snprintf(target_json, sizeof target_json, "\"target\":\"quickMask\",");
            break;
        case PC_ENGINE_PIXTGT_ALPHA:
            (void)snprintf(target_json, sizeof target_json, "\"target\":{\"channel\":%zu},", alpha_idx);
            break;
        default:
            break;
        }
    } else {
        if (!has_id) {
            return pc_engine_err_other(err, "no active layer");
        }
        const pc_layer *l = pc_document_layer(doc, id);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, id);
        }
        b = pc_engine_hook_transform_bounds(doc, l);
        (void)snprintf(target_json, sizeof target_json, "\"layer\":%llu,", (unsigned long long)id);
    }
    if (pc_rect_is_empty(b)) {
        return pc_engine_err_other(err, "nothing to transform");
    }

    /* Integer offsets keep these exact pixel permutations on the grid (nearest sampling). */
    double sx = (double)b.x0 + (double)b.x1, sy = (double)b.y0 + (double)b.y1;
    double cx = sx / 2.0, cy = sy / 2.0;
    double m[6];
    if (strcmp(kind, "rotate180") == 0) {
        m[0] = -1.0; m[1] = 0.0; m[2] = 0.0; m[3] = -1.0; m[4] = sx; m[5] = sy;
    } else if (strcmp(kind, "rotate90Cw") == 0) {
        m[0] = 0.0; m[1] = 1.0; m[2] = -1.0; m[3] = 0.0; m[4] = round(cx + cy); m[5] = round(cy - cx);
    } else if (strcmp(kind, "rotate90Ccw") == 0) {
        m[0] = 0.0; m[1] = -1.0; m[2] = 1.0; m[3] = 0.0; m[4] = round(cx - cy); m[5] = round(cx + cy);
    } else if (strcmp(kind, "flipHorizontal") == 0) {
        m[0] = -1.0; m[1] = 0.0; m[2] = 0.0; m[3] = 1.0; m[4] = sx; m[5] = 0.0;
    } else { /* flipVertical */
        m[0] = 1.0; m[1] = 0.0; m[2] = 0.0; m[3] = -1.0; m[4] = 0.0; m[5] = sy;
    }
    char mbuf[6][PC_JSON_NUM_MAX];
    for (int i = 0; i < 6; i++) {
        (void)pc_json_format_f64(m[i], mbuf[i]);
    }
    char params[384];
    (void)snprintf(params, sizeof params,
                   "{%s\"matrix\":[%s,%s,%s,%s,%s,%s],\"interpolation\":\"nearest\"}", target_json,
                   mbuf[0], mbuf[1], mbuf[2], mbuf[3], mbuf[4], mbuf[5]);
    return pc_engine_session_execute_json(s, "edit.transform", params, out, err);
}

static pc_status run_rotate180(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return transform_preset(s, p, "rotate180", out, err);
}
static pc_status run_rotate90cw(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return transform_preset(s, p, "rotate90Cw", out, err);
}
static pc_status run_rotate90ccw(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return transform_preset(s, p, "rotate90Ccw", out, err);
}
static pc_status run_flip_h(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return transform_preset(s, p, "flipHorizontal", out, err);
}
static pc_status run_flip_v(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return transform_preset(s, p, "flipVertical", out, err);
}

#define PRESET_PARAMS                                                                              \
    "{\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}=Channels panel target (an "     \
    "unlinked mask, an alpha channel or the Quick Mask turns alone)}"

static const char *const MENU_EDIT_TRANSFORM[] = {"Edit", "Transform"};

const pc_engine_command *pc_engine_transform_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"edit.transform", "Free Transform", NULL, 0, NULL, TRANSFORM_PARAMS, pc_engine_has_layer,
         run_transform, true},
        {"edit.transform.rotate180", "Rotate 180\xC2\xB0", MENU_EDIT_TRANSFORM, 2, NULL, PRESET_PARAMS,
         pc_engine_has_layer, run_rotate180, true},
        {"edit.transform.rotate90Cw", "Rotate 90\xC2\xB0 Clockwise", MENU_EDIT_TRANSFORM, 2, NULL,
         PRESET_PARAMS, pc_engine_has_layer, run_rotate90cw, true},
        {"edit.transform.rotate90Ccw", "Rotate 90\xC2\xB0 Counter Clockwise", MENU_EDIT_TRANSFORM, 2,
         NULL, PRESET_PARAMS, pc_engine_has_layer, run_rotate90ccw, true},
        {"edit.transform.flipHorizontal", "Flip Horizontal", MENU_EDIT_TRANSFORM, 2, NULL,
         PRESET_PARAMS, pc_engine_has_layer, run_flip_h, true},
        {"edit.transform.flipVertical", "Flip Vertical", MENU_EDIT_TRANSFORM, 2, NULL, PRESET_PARAMS,
         pc_engine_has_layer, run_flip_v, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
