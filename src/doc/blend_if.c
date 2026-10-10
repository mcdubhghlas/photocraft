/* Blend If ranges and Advanced Blending (port of crates/doc/src/blend_if.rs and advanced.rs). */
#include "doc/blend_if.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"

/* Entries pc_blend_if_set will grow to. The reference has no limit (it would abort on an
 * impossible allocation); a layer has at most a few dozen channels, so anything near this is
 * hostile input and is refused with PC_ERR_LIMIT instead of allocating gigabytes. */
#define BLEND_IF_MAX_RANGES ((size_t)1 << 16)

bool pc_blend_range_eq(pc_blend_range a, pc_blend_range b)
{
    return a.black[0] == b.black[0] && a.black[1] == b.black[1] && a.white[0] == b.white[0] &&
           a.white[1] == b.white[1];
}

bool pc_blend_range_is_full(pc_blend_range r)
{
    return pc_blend_range_eq(r, PC_BLEND_RANGE_FULL);
}

pc_blend_range pc_blend_range_from_bytes(const uint8_t b[4])
{
    pc_blend_range r;
    r.black[0] = b[0];
    r.black[1] = b[1];
    r.white[0] = b[2];
    r.white[1] = b[3];
    return r;
}

void pc_blend_range_to_bytes(pc_blend_range r, uint8_t out[4])
{
    out[0] = r.black[0];
    out[1] = r.black[1];
    out[2] = r.white[0];
    out[3] = r.white[1];
}

/* 0 at or below lo, 1 at or above hi (a hard step when hi <= lo); NaN x stays NaN on a ramp. */
static float ramp(uint8_t lo8, uint8_t hi8, float x)
{
    float lo = (float)lo8;
    float hi = (float)hi8;
    if (hi <= lo) {
        return x >= lo ? 1.0f : 0.0f;
    }
    return pc_clampf((x - lo) / (hi - lo), 0.0f, 1.0f);
}

float pc_blend_range_weight(pc_blend_range r, float v)
{
    float lo = ramp(r.black[0], r.black[1], v);
    /* Mirror for the white point: 1 at or below its low half, 0 above its high half. */
    float hi = ramp((uint8_t)(255 - r.white[1]), (uint8_t)(255 - r.white[0]), 255.0f - v);
    return lo * hi;
}

void pc_blend_if_fini(pc_blend_if *b)
{
    if (b == NULL) {
        return;
    }
    free(b->ranges);
    b->ranges = NULL;
    b->nranges = 0;
}

pc_status pc_blend_if_clone(const pc_blend_if *b, pc_blend_if *out, pc_error *err)
{
    pc_blend_if t = {NULL, 0};
    if (b->nranges > 0) {
        t.ranges = pc_malloc_array(b->nranges, sizeof *t.ranges);
        if (t.ranges == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memcpy(t.ranges, b->ranges, b->nranges * sizeof *t.ranges);
        t.nranges = b->nranges;
    }
    *out = t;
    return PC_OK;
}

bool pc_blend_if_eq(const pc_blend_if *a, const pc_blend_if *b)
{
    size_t i;
    if (a->nranges != b->nranges) {
        return false;
    }
    for (i = 0; i < a->nranges; i++) {
        if (!pc_blend_range_eq(a->ranges[i][0], b->ranges[i][0]) ||
            !pc_blend_range_eq(a->ranges[i][1], b->ranges[i][1])) {
            return false;
        }
    }
    return true;
}

static bool pair_is_full(const pc_blend_range p[2])
{
    return pc_blend_range_is_full(p[0]) && pc_blend_range_is_full(p[1]);
}

bool pc_blend_if_is_default(const pc_blend_if *b)
{
    size_t i;
    for (i = 0; i < b->nranges; i++) {
        if (!pair_is_full(b->ranges[i])) {
            return false;
        }
    }
    return true;
}

void pc_blend_if_get(const pc_blend_if *b, size_t i, pc_blend_range out[2])
{
    if (i < b->nranges) {
        out[0] = b->ranges[i][0];
        out[1] = b->ranges[i][1];
    } else {
        out[0] = PC_BLEND_RANGE_FULL;
        out[1] = PC_BLEND_RANGE_FULL;
    }
}

/* Drop trailing all-full entries (freeing the array when nothing is left). */
static void trim(pc_blend_if *b)
{
    while (b->nranges > 0 && pair_is_full(b->ranges[b->nranges - 1])) {
        b->nranges--;
    }
    if (b->nranges == 0) {
        free(b->ranges);
        b->ranges = NULL;
    }
}

pc_status pc_blend_if_set(pc_blend_if *b, size_t i, const pc_blend_range pair[2], pc_error *err)
{
    pc_blend_range(*grown)[2];
    size_t k;
    if (i < b->nranges) {
        b->ranges[i][0] = pair[0];
        b->ranges[i][1] = pair[1];
        trim(b);
        return PC_OK;
    }
    if (pair_is_full(pair)) {
        /* Growing with full entries and setting a full one: the trim removes all of them again
         * (and any full entries already at the end), so skip the allocation. */
        trim(b);
        return PC_OK;
    }
    if (i >= BLEND_IF_MAX_RANGES) {
        return pc_fail(err, PC_ERR_LIMIT, "blend-if channel index %zu is too large", i);
    }
    grown = pc_malloc_array(i + 1, sizeof *grown);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (b->nranges > 0) {
        memcpy(grown, b->ranges, b->nranges * sizeof *grown);
    }
    for (k = b->nranges; k < i; k++) {
        grown[k][0] = PC_BLEND_RANGE_FULL;
        grown[k][1] = PC_BLEND_RANGE_FULL;
    }
    grown[i][0] = pair[0];
    grown[i][1] = pair[1];
    free(b->ranges);
    b->ranges = grown;
    b->nranges = i + 1; /* the last entry is not full: nothing to trim */
    return PC_OK;
}

/* ---- advanced blending ---- */

uint8_t pc_knockout_to_psd(pc_knockout k)
{
    switch (k) {
    case PC_KNOCKOUT_NONE:
        return 0;
    case PC_KNOCKOUT_SHALLOW:
        return 1;
    case PC_KNOCKOUT_DEEP:
        return 2;
    }
    return 2;
}

pc_knockout pc_knockout_from_psd(uint8_t v)
{
    switch (v) {
    case 0:
        return PC_KNOCKOUT_NONE;
    case 1:
        return PC_KNOCKOUT_SHALLOW;
    default:
        return PC_KNOCKOUT_DEEP;
    }
}

const char *pc_knockout_name(pc_knockout k)
{
    switch (k) {
    case PC_KNOCKOUT_NONE:
        return "none";
    case PC_KNOCKOUT_SHALLOW:
        return "shallow";
    case PC_KNOCKOUT_DEEP:
        return "deep";
    }
    return "deep";
}

/* s[0..len) equals the lower-case ASCII word w, ignoring ASCII case (to_ascii_lowercase leaves
 * every non-ASCII byte alone, so those never match). */
static bool eq_ascii_ci(const char *s, size_t len, const char *w)
{
    size_t i;
    if (strlen(w) != len) {
        return false;
    }
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != w[i]) {
            return false;
        }
    }
    return true;
}

bool pc_knockout_from_name(const char *s, size_t len, pc_knockout *out)
{
    static const pc_knockout ALL[3] = {PC_KNOCKOUT_NONE, PC_KNOCKOUT_SHALLOW, PC_KNOCKOUT_DEEP};
    size_t i;
    if (s == NULL) {
        return false;
    }
    for (i = 0; i < 3; i++) {
        if (eq_ascii_ci(s, len, pc_knockout_name(ALL[i]))) {
            *out = ALL[i];
            return true;
        }
    }
    return false;
}

pc_advanced_blending pc_advanced_blending_default(void)
{
    pc_advanced_blending a;
    a.knockout = PC_KNOCKOUT_NONE;
    a.blend_interior = false;
    a.blend_clipped = true;
    a.transparency_shapes = true;
    a.layer_mask_hides_effects = false;
    a.vector_mask_hides_effects = false;
    return a;
}

bool pc_advanced_blending_eq(const pc_advanced_blending *a, const pc_advanced_blending *b)
{
    return a->knockout == b->knockout && a->blend_interior == b->blend_interior &&
           a->blend_clipped == b->blend_clipped && a->transparency_shapes == b->transparency_shapes &&
           a->layer_mask_hides_effects == b->layer_mask_hides_effects &&
           a->vector_mask_hides_effects == b->vector_mask_hides_effects;
}

bool pc_advanced_blending_is_default(const pc_advanced_blending *a)
{
    pc_advanced_blending d = pc_advanced_blending_default();
    return pc_advanced_blending_eq(a, &d);
}
