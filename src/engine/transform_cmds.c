/* Free Transform foundation (partial port of crates/engine/src/transform_cmds.rs, E4).
 *
 * This slice ports the two cross-module primitives the sibling E4 modules need now: composite_over
 * (a straight-alpha piece over a surface, used by the floating-selection drop) and the
 * transform_bounds hook (the Free-Transform frame of a layer, used by snapping). The full transform
 * commands (edit.freeTransform / transform.* / transform.selection) are deferred: they need
 * extra_cmds::background_to_layer (E12), the smart-object / vector / type placement refresh (wired)
 * and the edit_job path -- tracked for an E4 follow-up. */
#include <stdlib.h>

#include "doc/doc.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "engine_internal.h"
#include "fill_cmds.h"

/* `top` (straight alpha) over `dst` (straight RGBA, source-over), over top's content bounds
 * (transform_cmds::composite_over). */
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
    /* Write the straight RGBA back through the surface's own format. */
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
