/* A smart object's filter mask: mix / unmix (port of crates/compose/src/filter_mask.rs). */

#include "compose/filter_mask.h"

#include <stdlib.h>

#include "base/mem.h"
#include "base/num.h"
#include "color/color.h"
#include "geom/geom.h"

/* The mask value below which a pixel's filtered value is taken to be unknown: unmix gives the
 * unfiltered value there, which the mask shows (almost) none of anyway. */
#define HIDDEN (1.0f / 1024.0f)

/* True when neither surface has a tile overlapping `area` and, for a format without alpha, both
 * surfaces share the default pixel: such a tile stays as it already is in the clone. */
static bool bare_tile(const pc_surface *a, const pc_surface *b, pc_rect area, bool alpha, bool same_default)
{
    bool bare = !pc_surface_has_tiles_in(a, area) && !pc_surface_has_tiles_in(b, area);
    return bare && (alpha || same_default);
}

/* Shared skeleton of mix (invert == false) and unmix (invert == true): tile by tile over the
 * union of the two surfaces, combining per pixel through the mask. */
static pc_status combine(const pc_surface *base, const pc_surface *other, const pc_layer_mask *mask,
                         bool invert, pc_surface *out, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(base);
    pc_rect whole = pc_rect_union(pc_surface_content_bounds(base), pc_surface_content_bounds(other));
    pc_surface o;
    pc_tile first;
    pc_tile last;
    size_t n;
    bool alpha;
    bool same_default;
    pc_status st = pc_surface_clone(other, &o, err);
    if (st != PC_OK) {
        return st;
    }
    if (pc_rect_is_empty(whole) || !pc_pixel_format_eq(pc_surface_format(other), fmt)) {
        *out = o;
        return PC_OK;
    }
    n = pc_pixel_format_channels(fmt);
    alpha = fmt.alpha;
    same_default = memcmp(pc_surface_default_bytes(base), pc_surface_default_bytes(other),
                          pc_pixel_format_bytes_per_pixel(fmt)) == 0;
    if (!pc_rect_tile_range(whole, &first, &last)) {
        *out = o;
        return PC_OK;
    }
    for (int32_t ty = first.ty; st == PC_OK && ty <= last.ty; ty++) {
        for (int32_t tx = first.tx; st == PC_OK && tx <= last.tx; tx++) {
            pc_rect area = pc_rect_intersect(pc_tile_rect((pc_tile){tx, ty}), whole);
            size_t w;
            size_t h;
            size_t px;
            float *b = NULL;
            float *f = NULL;
            float *k = NULL;
            float *res = NULL;
            size_t blen = 0;
            size_t flen = 0;
            if (pc_rect_is_empty(area) || bare_tile(base, other, area, alpha, same_default)) {
                continue;
            }
            w = pc_rect_width(area);
            h = pc_rect_height(area);
            px = w * h; /* within a single tile: at most PC_TILE_SIZE^2, no overflow */
            st = pc_surface_read_region(base, area, &b, &blen, err);
            if (st == PC_OK) {
                st = pc_surface_read_region(other, area, &f, &flen, err);
            }
            if (st == PC_OK) {
                k = pc_malloc_array(px == 0 ? 1 : px, sizeof *k);
                if (k == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
                }
            }
            if (st == PC_OK) {
                st = pc_layer_mask_values_into(mask, area, k, px, err);
            }
            if (st == PC_OK && blen >= px * n && flen >= px * n) {
                res = pc_malloc_array(blen == 0 ? 1 : blen, sizeof *res);
                if (res == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
                }
            } else if (st == PC_OK) {
                st = pc_fail(err, PC_ERR_INVALID, "compose: filter-mask region size mismatch");
            }
            for (size_t i = 0; st == PC_OK && i < px; i++) {
                const float *bp = &b[i * n];
                const float *xp = &f[i * n];
                float *op = &res[i * n];
                float m = pc_clampf(k[i], 0.0f, 1.0f);
                if (!invert) {
                    /* mix: `other` (filtered) over `base` through m. */
                    if (!alpha) {
                        for (size_t c = 0; c < n; c++) {
                            op[c] = bp[c] + (xp[c] - bp[c]) * m;
                        }
                        continue;
                    }
                    {
                        size_t a = n - 1;
                        float oa = bp[a] + (xp[a] - bp[a]) * m;
                        op[a] = oa;
                        for (size_t c = 0; c < a; c++) {
                            float pm = bp[c] * bp[a] + (xp[c] * xp[a] - bp[c] * bp[a]) * m;
                            op[c] = oa > 0.0f ? pc_clampf(pm / oa, 0.0f, 1.0f) : 0.0f;
                        }
                    }
                    continue;
                }
                /* unmix: recover the filtered pixels that mix with `base` through m to `other`. */
                if (m < HIDDEN) {
                    /* A transparent base pixel (Photoshop's cache keeps white there) stays empty,
                     * so the stored pixels don't grow past the content. */
                    if (!alpha || bp[n - 1] > 0.0f) {
                        for (size_t c = 0; c < n; c++) {
                            op[c] = bp[c];
                        }
                    } else {
                        for (size_t c = 0; c < n; c++) {
                            op[c] = 0.0f;
                        }
                    }
                    continue;
                }
                if (!alpha) {
                    for (size_t c = 0; c < n; c++) {
                        op[c] = pc_clampf(bp[c] + (xp[c] - bp[c]) / m, 0.0f, 1.0f);
                    }
                    continue;
                }
                {
                    size_t a = n - 1;
                    float fa = pc_clampf(bp[a] + (xp[a] - bp[a]) / m, 0.0f, 1.0f);
                    if (fa <= 0.0f) {
                        for (size_t c = 0; c < n; c++) {
                            op[c] = 0.0f;
                        }
                        continue;
                    }
                    op[a] = fa;
                    for (size_t c = 0; c < a; c++) {
                        float pm = bp[c] * bp[a] + (xp[c] * xp[a] - bp[c] * bp[a]) / m;
                        op[c] = pc_clampf(pm / fa, 0.0f, 1.0f);
                    }
                }
            }
            if (st == PC_OK) {
                st = pc_surface_write_region(&o, area, res, px * n, err);
            }
            free(b);
            free(f);
            free(k);
            free(res);
        }
    }
    if (st != PC_OK) {
        pc_surface_fini(&o);
        return st;
    }
    pc_surface_prune(&o);
    *out = o;
    return PC_OK;
}

pc_status pc_compose_filter_mask_mix(const pc_surface *base, const pc_surface *filtered,
                                     const pc_layer_mask *mask, pc_surface *out, pc_error *err)
{
    return combine(base, filtered, mask, false, out, err);
}

pc_status pc_compose_filter_mask_unmix(const pc_surface *base, const pc_surface *mixed,
                                       const pc_layer_mask *mask, pc_surface *out, pc_error *err)
{
    return combine(base, mixed, mask, true, out, err);
}
