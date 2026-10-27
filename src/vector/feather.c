/* Mask feathering for the shape renderer: the Gaussian blur Properties > Feather applies to a
 * shape layer's own path as its fill's soft mask (port of crates/vector/src/feather.rs). It is the
 * same extended-box Gaussian the compositor (src/compose/masks.c) blurs a vector mask with, so a
 * shape's feather and an equal vector-mask feather match exactly. Pure f32 planes. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "vector/internal.h"

#define FEATHER_SIGMA 1.0f
#define MAX_FEATHER_SIGMA (1000.0f * FEATHER_SIGMA) /* Photoshop's feather tops out at 1000 px */

float pc_vec_feather_sigma(float px)
{
    return isfinite(px) ? pc_clampf(px * FEATHER_SIGMA, 0.0f, MAX_FEATHER_SIGMA) : 0.0f;
}

pc_rect pc_vec_feather_grow(pc_rect r, float sigma)
{
    int32_t m;
    if (pc_rect_is_empty(r)) {
        return r;
    }
    m = pc_i32_sat_add(pc_f32_to_i32(ceilf(sigma * 3.0f)), 2);
    return pc_rect_make(pc_i32_sat_sub(r.x0, m), pc_i32_sat_sub(r.y0, m), pc_i32_sat_add(r.x1, m),
                        pc_i32_sat_add(r.y1, m));
}

pc_rect pc_vec_feather_path_area(const pc_path *path, float sigma)
{
    double cb[4];
    pc_rect r;
    if (!pc_path_control_bounds(path, cb)) {
        return PC_RECT_EMPTY;
    }
    r = pc_rect_make(pc_i32_sat_sub(pc_f64_to_i32(floor(cb[0])), 2), pc_i32_sat_sub(pc_f64_to_i32(floor(cb[1])), 2),
                     pc_i32_sat_add(pc_f64_to_i32(ceil(cb[2])), 2), pc_i32_sat_add(pc_f64_to_i32(ceil(cb[3])), 2));
    return pc_vec_feather_grow(r, sigma);
}

/* One box pass: weight 1 on +-r around each sample and `end` (0..1) on the two samples just past
 * the window, so a fractional variance can be matched exactly. */
typedef struct box_pass {
    size_t r;
    float end;
    bool active;
} box_pass;

/* The box radius and fractional end weight whose single pass has variance sigma^2 / 3. */
static box_pass box_pass_for_sigma(float sigma)
{
    box_pass bp = {0, 0.0f, false};
    double sm;
    double v;
    double rf;
    double nn;
    double end;
    double r0;
    size_t r;
    if (!isfinite(sigma) || sigma <= 0.0f) {
        return bp;
    }
    /* Feathers top out at 1000 px; the cap keeps the radius arithmetic far from overflow. */
    sm = (double)(sigma < MAX_FEATHER_SIGMA ? sigma : MAX_FEATHER_SIGMA);
    v = sm * sm / 3.0;
    /* The largest whole radius whose box variance r(r+1)/3 is at most v. */
    r0 = floor((sqrt(12.0 * v + 1.0) - 1.0) / 2.0);
    if (!(r0 > 0.0)) {
        r0 = 0.0;
    }
    r = (size_t)r0;
    while (r > 0 && (double)r * ((double)r + 1.0) / 3.0 > v) {
        r--;
    }
    while ((double)(r + 1) * ((double)(r + 1) + 1.0) / 3.0 <= v) {
        r++;
    }
    rf = (double)r;
    nn = 2.0 * (double)r + 1.0;
    end = (nn * (rf * (rf + 1.0) / 3.0 - v)) / (2.0 * v - 2.0 * (rf + 1.0) * (rf + 1.0));
    /* f64::clamp(0, 1): NaN stays NaN. */
    end = end < 0.0 ? 0.0 : (end > 1.0 ? 1.0 : end);
    bp.r = r;
    bp.end = (float)end;
    bp.active = true;
    return bp;
}

/* One box pass over a line: reads the pre-pass values from `scratch` (a copy of `line`) and writes
 * the blurred result into `line` (edges clamp). */
static void blur_line(float *line, float *scratch, size_t len, const box_pass *bp)
{
    int64_t radius = (int64_t)bp->r;
    int64_t last = (int64_t)len - 1;
    float acc = -0.0f; /* Iterator::sum starts at -0.0 */
    float n;
    float below;
    int64_t j;
    size_t i;
    if (len == 0) {
        return;
    }
    memcpy(scratch, line, len * sizeof *line);
#define GET(ix) scratch[(size_t)((ix) < 0 ? 0 : ((ix) > last ? last : (ix)))]
    for (j = -radius; j <= radius; j++) {
        acc += GET(j);
    }
    n = (float)(2 * bp->r + 1) + 2.0f * bp->end;
    below = GET(-radius - 1);
    for (i = 0; i < len; i++) {
        int64_t ii = (int64_t)i;
        float above = GET(ii + radius + 1);
        float leaving = GET(ii - radius);
        line[i] = (acc + bp->end * (below + above)) / n;
        acc += above - leaving;
        below = leaving;
    }
#undef GET
}

/* Approximate Gaussian blur of a w x h plane: three box passes per axis (edges clamp), the exact
 * variance sigma^2. Processed one line at a time, preserving each line's floating-point order so
 * the result is bit-identical to the compositor's vector-mask feather. */
pc_status pc_vec_feather_gaussian(float *v, size_t w, size_t h, float sigma, pc_error *err)
{
    box_pass bp = box_pass_for_sigma(sigma);
    size_t len_max = w > h ? w : h;
    float *work;
    float *scratch;
    int axis;
    if (!bp.active || w == 0 || h == 0) {
        return PC_OK;
    }
    work = pc_malloc_array(len_max, sizeof *work);
    scratch = pc_malloc_array(len_max, sizeof *scratch);
    if (work == NULL || scratch == NULL) {
        free(work);
        free(scratch);
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    for (axis = 0; axis < 2; axis++) {
        size_t lines = axis ? w : h;
        size_t line_len = axis ? h : w;
        size_t k;
        for (k = 0; k < lines; k++) {
            size_t i;
            int rep;
            for (i = 0; i < line_len; i++) {
                work[i] = axis ? v[i * w + k] : v[k * w + i];
            }
            for (rep = 0; rep < 3; rep++) {
                blur_line(work, scratch, line_len, &bp);
            }
            for (i = 0; i < line_len; i++) {
                if (axis) {
                    v[i * w + k] = work[i];
                } else {
                    v[k * w + i] = work[i];
                }
            }
        }
    }
    free(work);
    free(scratch);
    return PC_OK;
}
