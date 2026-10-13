/* Mode-native flattening of plain CMYK layer stacks (port of crates/compose/src/native.rs, #3778).
 *
 * The compositor works in display RGB, so flattening a CMYK document through it reads every ink
 * value through the profile and separates it again: even a lone Background comes back with
 * different ink. When every visible layer is a plain raster layer in Normal mode (no masks,
 * effects, clipping or advanced blending), the composite needs no colour conversion at all:
 * Normal mixes ink values linearly, as Photoshop does in CMYK, so it is worked out on the stored
 * values and an opaque layer keeps them exactly. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "compose/internal.h"

/* Process channels of a CMYK pixel. */
#define INKS 4

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

/* A layer the native path can composite: a visible raster layer the compositor would only blend
 * Normally at its opacity. NULL otherwise. */
static const pc_surface *plain(const pc_layer *layer)
{
    const pc_surface *s;
    if (layer->content.kind != PC_LAYER_RASTER) {
        return NULL;
    }
    s = &layer->content.u.raster;
    if (layer->blend != PC_BLEND_NORMAL || layer->clipped || (layer->has_mask && layer->mask.enabled) ||
        (layer->has_vector_mask && layer->vector_mask.enabled) || pc_compose_has_effects(layer) ||
        layer->excluded_channels != 0 || !pc_blend_if_is_default(&layer->blend_if) ||
        !pc_advanced_blending_is_default(&layer->advanced) || layer->has_video ||
        pc_surface_format(s).mode != PC_COLOR_MODE_CMYK) {
        return NULL;
    }
    return s;
}

typedef struct native_layer {
    const pc_surface *s;
    float opacity;
} native_layer;

static bool default_all_zero(const pc_surface *s)
{
    float dp[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_surface_channels(s);
    size_t i;
    pc_surface_default_pixel(s, dp);
    for (i = 0; i < n; i++) {
        if (dp[i] != 0.0f) {
            return false;
        }
    }
    return true;
}

/* One band: straight C, M, Y, K, alpha per pixel in acc (zeroed), from the stack. */
static pc_status mix_band(const native_layer *stack, size_t nstack, pc_rect band, float (*acc)[INKS + 1],
                          size_t npx, float *src, pc_error *err)
{
    size_t l;
    for (l = 0; l < nstack; l++) {
        const pc_surface *surf = stack[l].s;
        float opacity = stack[l].opacity;
        pc_pixel_format sf = pc_surface_format(surf);
        size_t sn = pc_pixel_format_channels(sf);
        size_t i;
        pc_status st;
        if (opacity <= 0.0f || (pc_rect_is_empty(pc_rect_intersect(pc_surface_content_bounds(surf), band)) &&
                                default_all_zero(surf))) {
            continue;
        }
        st = pc_surface_read_region_into(surf, band, src, npx * sn, err);
        if (st != PC_OK) {
            return st;
        }
        for (i = 0; i < npx; i++) {
            const float *s = src + i * sn;
            float *d = acc[i];
            float sa = (sf.alpha && sn > INKS ? s[INKS] : 1.0f) * opacity;
            float oa;
            float keep;
            int c;
            if (sa <= 0.0f) {
                continue;
            }
            oa = sa + d[INKS] * (1.0f - sa);
            keep = d[INKS] * (1.0f - sa);
            for (c = 0; c < INKS; c++) {
                /* An opaque pixel replaces what is below exactly (no rounding through the mix). */
                d[c] = sa >= 1.0f ? s[c] : (s[c] * sa + d[c] * keep) / oa;
            }
            d[INKS] = oa;
        }
    }
    return PC_OK;
}

pc_status pc_compose_flatten_cmyk(const pc_document *doc, pc_rect rect, pc_pixel_format fmt, bool paper,
                                  bool *done, pc_surface *out, pc_error *err)
{
    native_layer *stack = NULL;
    size_t nstack = 0;
    size_t i;
    size_t n;
    size_t w;
    int32_t rows;
    int32_t y;
    float (*acc)[INKS + 1] = NULL;
    float *src = NULL;
    float *data = NULL;
    size_t band_px;
    pc_surface s;
    pc_status st = PC_OK;
    *done = false;
    if (doc->mode != PC_COLOR_MODE_CMYK || fmt.mode != PC_COLOR_MODE_CMYK) {
        return PC_OK;
    }
    for (i = 0; i < doc->nlayers; i++) {
        if (doc->layers[i].clipped) {
            return PC_OK;
        }
    }
    if (doc->nlayers > 0) {
        stack = pc_malloc_array(doc->nlayers, sizeof *stack);
        if (stack == NULL) {
            return nomem(err);
        }
    }
    for (i = 0; i < doc->nlayers; i++) {
        const pc_layer *layer = &doc->layers[i];
        const pc_surface *ps;
        if (!layer->visible) {
            continue;
        }
        ps = plain(layer);
        if (ps == NULL) {
            free(stack);
            return PC_OK;
        }
        stack[nstack].s = ps;
        stack[nstack].opacity = pc_clampf(layer->opacity * layer->fill_opacity, 0.0f, 1.0f);
        nstack++;
    }
    pc_surface_init(&s, fmt);
    if (pc_rect_is_empty(rect)) {
        free(stack);
        *out = s;
        *done = true;
        return PC_OK;
    }
    n = pc_pixel_format_channels(fmt);
    w = pc_cmp_w(rect);
    rows = pc_compose_band_rows_for(pc_rect_width(rect), 0);
    /* Every band has at most `rows` rows, so one set of buffers serves them all. */
    y = pc_i32_sat_add(rect.y0, rows);
    st =
        pc_cmp_rect_pixels(pc_rect_make(rect.x0, rect.y0, rect.x1, y < rect.y1 ? y : rect.y1), &band_px, err);
    if (st == PC_OK) {
        acc = pc_malloc_array(band_px, sizeof *acc);
        src = pc_malloc_array(band_px, PC_RASTER_MAX_CHANNELS * sizeof *src);
        data = pc_malloc_array(band_px, PC_RASTER_MAX_CHANNELS * sizeof *data);
        if (acc == NULL || src == NULL || data == NULL) {
            st = nomem(err);
        }
    }
    y = rect.y0;
    while (st == PC_OK && y < rect.y1) {
        int32_t y1 = pc_i32_sat_add(y, rows);
        pc_rect band;
        size_t npx;
        if (y1 > rect.y1) {
            y1 = rect.y1;
        }
        band = pc_rect_make(rect.x0, y, rect.x1, y1);
        npx = w * (size_t)pc_rect_height(band);
        memset(acc, 0, npx * sizeof *acc);
        st = mix_band(stack, nstack, band, acc, npx, src, err);
        for (i = 0; st == PC_OK && i < npx; i++) {
            /* White paper carries no ink, so flattening onto it scales the ink by coverage. */
            const float *p = acc[i];
            float *o = data + i * n;
            float a = p[INKS];
            float k = paper && a < 1.0f ? a : 1.0f;
            int c;
            for (c = 0; c < INKS; c++) {
                o[c] = p[c] * k;
            }
            if (fmt.alpha) {
                o[INKS] = paper ? 1.0f : a;
            }
        }
        if (st == PC_OK) {
            st = pc_surface_write_region(&s, band, data, npx * n, err);
        }
        y = y1;
    }
    free(stack);
    free(acc);
    free(src);
    free(data);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    pc_surface_prune(&s);
    *out = s;
    *done = true;
    return PC_OK;
}
