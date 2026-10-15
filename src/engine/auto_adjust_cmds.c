/* Image > Auto Tone / Auto Contrast / Auto Color (port of auto_adjust in
 * crates/engine/src/edit_cmds.rs). Each builds a per-channel Levels adjustment from the layer's
 * histogram (0.1 % clipping, Photoshop's defaults) and applies it destructively through the
 * selection:
 *   - Auto Tone   stretches each channel to its own black/white points.
 *   - Auto Contrast stretches all channels to the combined histogram's points (keeps colour).
 *   - Auto Color  stretches per channel and neutralises the midtones onto the average median.
 *
 * Never crashes on any document/selection state (REWRITE_PLAN.md rule 9): the histogram samples
 * are clamped, divisions are guarded, and the adjustment is applied through the shared
 * compose-backed pc_engine_adjust_surface (bit-exact with the reference). */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "doc/adjust.h"
#include "geom/geom.h"
#include "raster/surface.h"

#include "brush_cmds.h" /* pc_engine_paint_surface */
#include "engine_internal.h"

/* Integer square root of a u64 (floor), matching Rust's u64::isqrt. */
static uint64_t isqrt_u64(uint64_t n)
{
    if (n == 0) {
        return 0;
    }
    uint64_t x = (uint64_t)sqrt((double)n);
    /* Correct for floating rounding at the boundary. */
    while (x > 0 && x > n / x) {
        x--;
    }
    while ((x + 1) <= n / (x + 1)) {
        x++;
    }
    return x;
}

/* Per-channel 8-bit histograms over `area`, skipping fully transparent pixels. */
static void histograms(const pc_surface *surf, pc_rect area, uint64_t h[3][256])
{
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < 256; i++) {
            h[c][i] = 0;
        }
    }
    uint32_t w = pc_rect_width(area);
    uint32_t ht = pc_rect_height(area);
    if (w == 0 || ht == 0) {
        return;
    }
    uint64_t area_px = (uint64_t)w * (uint64_t)ht;
    uint64_t step = area_px / 4000000u;
    if (step < 1) {
        step = 1;
    }
    uint64_t stride64 = isqrt_u64(step);
    if (stride64 < 1) {
        stride64 = 1;
    }
    int32_t stride = (stride64 > (uint64_t)INT32_MAX) ? INT32_MAX : (int32_t)stride64;
    uint8_t(*row)[4] = pc_malloc_array(w, sizeof *row);
    if (row == NULL) {
        return;
    }
    for (int32_t y = area.y0; y < area.y1; y += stride) {
        pc_rect r = pc_rect_make(area.x0, y, area.x1, y + 1);
        if (pc_surface_read_rgba8_into(surf, r, row, w, NULL) != PC_OK) {
            continue;
        }
        for (uint32_t x = 0; x < w; x += (uint32_t)stride) {
            if (row[x][3] == 0) {
                continue;
            }
            for (int c = 0; c < 3; c++) {
                h[c][row[x][c]]++;
            }
        }
    }
    free(row);
}

/* Black and white points with `clip` (fraction) clipped at each end. */
static void clip_points(const uint64_t h[256], double clip, float *lo_out, float *hi_out)
{
    uint64_t total = 0;
    for (int i = 0; i < 256; i++) {
        total += h[i];
    }
    if (total == 0) {
        *lo_out = 0.0f;
        *hi_out = 1.0f;
        return;
    }
    uint64_t lim = (uint64_t)((double)total * clip);
    uint64_t acc = 0;
    size_t lo = 0;
    while (lo < 254 && acc + h[lo] <= lim) {
        acc += h[lo];
        lo++;
    }
    acc = 0;
    size_t hi = 255;
    while (hi > lo + 1 && acc + h[hi] <= lim) {
        acc += h[hi];
        hi--;
    }
    *lo_out = (float)lo / 255.0f;
    *hi_out = (float)hi / 255.0f;
}

static float histogram_median(const uint64_t h[256])
{
    uint64_t total = 0;
    for (int i = 0; i < 256; i++) {
        total += h[i];
    }
    uint64_t acc = 0;
    for (int i = 0; i < 256; i++) {
        acc += h[i];
        if (acc * 2 >= total) {
            return (float)i / 255.0f;
        }
    }
    return 0.5f;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

struct auto_ctx {
    pc_adjustment adj;
    pc_layer_id id;
};

static pc_status edit_auto(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                           pc_error *err)
{
    (void)has_active;
    (void)active;
    struct auto_ctx *c = vctx;
    pc_surface *surf = NULL;
    pc_status rc = pc_engine_paint_surface(doc, c->id, NULL, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_engine_adjust_surface(surf, &c->adj, sel, doc->mode);
    return PC_OK;
}

/* kind: 0 = tone, 1 = contrast, 2 = color. */
static pc_status auto_adjust(pc_engine_session *s, int kind, const char *label, pc_json_value *out,
                             pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (!d->has_active_layer) {
        return pc_engine_err_other(err, "the active layer has no pixels");
    }
    pc_layer_id id = d->active_layer;
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, id);
    const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
    if (surf == NULL) {
        return pc_engine_err_other(err, "the active layer has no pixels");
    }
    pc_rect bounds = pc_document_bounds(doc);
    pc_rect base = doc->has_selection ? pc_surface_content_bounds(&doc->selection) : bounds;
    pc_rect area = pc_rect_intersect(base, bounds);

    uint64_t h[3][256];
    histograms(surf, area, h);
    const double clip = 0.001;

    pc_levels_channel per[3];
    for (int c = 0; c < 3; c++) {
        per[c] = pc_levels_channel_default();
    }
    if (kind == 1) { /* contrast: combined histogram, same points for all channels */
        uint64_t all[256];
        for (int i = 0; i < 256; i++) {
            all[i] = h[0][i] + h[1][i] + h[2][i];
        }
        float lo, hi;
        clip_points(all, clip, &lo, &hi);
        for (int c = 0; c < 3; c++) {
            per[c].in_black = lo;
            per[c].in_white = hi;
        }
    } else {
        for (int c = 0; c < 3; c++) {
            clip_points(h[c], clip, &per[c].in_black, &per[c].in_white);
        }
        if (kind == 2) { /* color: neutralise midtones onto the average median */
            float meds[3];
            for (int c = 0; c < 3; c++) {
                meds[c] = histogram_median(h[c]);
            }
            float target = (meds[0] + meds[1] + meds[2]) / 3.0f;
            for (int c = 0; c < 3; c++) {
                float denom = per[c].in_white - per[c].in_black;
                if (denom < 1e-3f) {
                    denom = 1e-3f;
                }
                float t = clampf((meds[c] - per[c].in_black) / denom, 0.02f, 0.98f);
                float want = clampf((target - per[c].in_black) / denom, 0.02f, 0.98f);
                per[c].gamma = clampf(logf(t) / logf(want), 0.1f, 9.99f);
            }
        }
    }

    struct auto_ctx ctx;
    ctx.id = id;
    ctx.adj.kind = PC_ADJ_LEVELS;
    ctx.adj.u.levels.master = pc_levels_channel_default();
    ctx.adj.u.levels.per_channel[0] = per[0];
    ctx.adj.u.levels.per_channel[1] = per[1];
    ctx.adj.u.levels.per_channel[2] = per[2];
    ctx.adj.u.levels.space = PC_TONE_RGB;
    ctx.adj.u.levels.black = pc_levels_channel_default();

    pc_status rc = pc_engine_session_edit(s, label, edit_auto, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

static pc_status has_pixels(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!st->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    if (l->content.kind == PC_LAYER_SMART) {
        return pc_fail(reason, PC_ERR_INVALID, "the smart object is not directly editable");
    }
    return pc_fail(reason, PC_ERR_INVALID, "the active layer has no pixels");
}

static pc_status run_tone(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return auto_adjust(s, 0, "Auto Tone", out, err);
}

static pc_status run_contrast(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return auto_adjust(s, 1, "Auto Contrast", out, err);
}

static pc_status run_color(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return auto_adjust(s, 2, "Auto Color", out, err);
}

static const char *const MENU_IMAGE[] = {"Image"};

static const pc_engine_command SPECS[] = {
    {"image.autoTone", "Auto Tone", MENU_IMAGE, 1, "Cmd+Shift+L", "{}", has_pixels, run_tone, true},
    {"image.autoContrast", "Auto Contrast", MENU_IMAGE, 1, "Cmd+Alt+Shift+L", "{}", has_pixels,
     run_contrast, true},
    {"image.autoColor", "Auto Color", MENU_IMAGE, 1, "Cmd+Shift+B", "{}", has_pixels, run_color, true},
};

const pc_engine_command *pc_engine_auto_adjust_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
