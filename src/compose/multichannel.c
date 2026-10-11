/* Display of Multichannel documents: every spot (ink) channel printed over white paper (port of
 * crates/compose/src/multichannel.rs).
 *
 * Ink density v of an ink with display colour c and solidity s turns the colour underneath p into
 * (1 - s v) p (1 - v + v c) + s v c: transparent inks multiply like overprinted process inks,
 * opaque ones cover what is below. */
#include <stdlib.h>

#include "base/mem.h"
#include "base/num.h"
#include "compose/internal.h"

pc_status pc_compose_inks(const pc_document *doc, pc_rect rect, pc_compose_buffer *out, pc_error *err)
{
    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_compose_buffer buf;
    float *vals = NULL;
    size_t ch;
    pc_status st = pc_compose_buffer_filled(rect, white, &buf, err);
    if (st != PC_OK) {
        return st;
    }
    for (ch = 0; ch < doc->nchannels && st == PC_OK; ch++) {
        const pc_alpha_channel *c = &doc->channels[ch];
        float rgb[3];
        float s;
        size_t n;
        size_t total;
        size_t i;
        int j;
        if (!c->has_spot) {
            continue;
        }
        if (pc_rect_is_empty(pc_rect_intersect(pc_surface_content_bounds(&c->surface), rect))) {
            continue;
        }
        pc_color_to_rgb(&c->spot_color, rgb);
        s = pc_clampf(c->spot_solidity, 0.0f, 1.0f);
        n = pc_surface_channels(&c->surface);
        if (n == 0 || !pc_size_mul(buf.len, n, &total)) {
            continue;
        }
        free(vals);
        vals = pc_malloc_array(total, sizeof *vals);
        if (vals == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
            break;
        }
        st = pc_surface_read_region_into(&c->surface, rect, vals, total, err);
        for (i = 0; st == PC_OK && i < buf.len; i++) {
            float *p = buf.px[i];
            float v = pc_clampf(vals[i * n], 0.0f, 1.0f);
            float keep;
            if (v <= 0.0f) {
                continue;
            }
            keep = 1.0f - s * v;
            for (j = 0; j < 3; j++) {
                p[j] = keep * p[j] * (1.0f - v + v * rgb[j]) + s * v * rgb[j];
            }
        }
    }
    free(vals);
    if (st != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return st;
    }
    *out = buf;
    return PC_OK;
}

pc_status pc_cmp_backdrop(const pc_document *doc, pc_rect rect, pc_compose_buffer *out, pc_error *err)
{
    if (doc->mode == PC_COLOR_MODE_MULTICHANNEL) {
        return pc_compose_inks(doc, rect, out, err);
    }
    return pc_compose_buffer_transparent(rect, out, err);
}
