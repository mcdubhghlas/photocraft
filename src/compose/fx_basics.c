/* The small parts of effects.rs and adjust.rs the compositor core uses directly (C1):
 * has_effects, margin and Transfer::for_document. The effect maps are in fx_*.c (C3). */
#include <math.h>

#include "base/num.h"
#include "base/rmath.h"
#include "compose/internal.h"

bool pc_compose_has_effects(const pc_layer *layer)
{
    size_t i;
    if (!layer->effects.enabled) {
        return false;
    }
    for (i = 0; i < layer->effects.nitems; i++) {
        if (pc_effect_enabled(&layer->effects.items[i])) {
            return true;
        }
    }
    return false;
}

int32_t pc_compose_effects_margin(const pc_layer *layer)
{
    float m = 0.0f;
    size_t i;
    for (i = 0; i < layer->effects.nitems; i++) {
        const pc_effect *e = &layer->effects.items[i];
        float r;
        if (!pc_effect_enabled(e)) {
            continue;
        }
        switch (e->kind) {
        case PC_FX_DROP_SHADOW:
        case PC_FX_INNER_SHADOW:
            r = e->u.shadow.distance + e->u.shadow.size;
            break;
        case PC_FX_OUTER_GLOW:
        case PC_FX_INNER_GLOW:
            r = e->u.glow.size;
            break;
        case PC_FX_STROKE:
            r = e->u.stroke.size;
            break;
        case PC_FX_SATIN:
            r = e->u.satin.distance + e->u.satin.size;
            break;
        case PC_FX_BEVEL_EMBOSS:
            r = e->u.bevel.size + e->u.bevel.soften;
            break;
        default:
            r = 0.0f;
            break;
        }
        if (isfinite(r)) {
            m = pc_rs_maxf(m, r);
        }
    }
    /* Bounded so malformed values can't request huge buffers. m is finite and >= 0 here. */
    return (int32_t)ceilf(pc_clampf(m, 0.0f, PC_COMPOSE_FX_MAX_REACH)) + 2;
}

pc_compose_transfer pc_compose_transfer_for_document(pc_color_mode mode, pc_sample_type depth)
{
    if (depth == PC_SAMPLE_TYPE_F32) {
        return (pc_compose_transfer){PC_COMPOSE_TRANSFER_GAMMA, 1.0f};
    }
    switch (mode) {
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_DUOTONE:
    case PC_COLOR_MODE_BITMAP:
        return (pc_compose_transfer){PC_COMPOSE_TRANSFER_GAMMA, 1.732f};
    default:
        return (pc_compose_transfer){PC_COMPOSE_TRANSFER_SRGB, 0.0f};
    }
}
