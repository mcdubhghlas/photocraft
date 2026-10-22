/* Camera Raw-style continuous RGB histogram ribbons. Port of crates/ui-egui/src/rgb_histogram.rs.
 * See rgb_histogram.h for the model. Pure geometry; no SDL/GL/Nuklear. */
#include "ui/rgb_histogram.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>

#include "base/mem.h"
#include "base/num.h"

#define BINS PC_UI_HIST_BINS

/* Hard cap: the reference bounds the mesh at ~257 spans x 4 cuts x 3 bands; 1<<16 quads is a safe
 * ceiling that still refuses an unbounded allocation on hostile input. */
#define PC_UI_HIST_MAX_QUADS ((size_t)1 << 16)

void pc_ui_hist_mesh_init(pc_ui_hist_mesh *mesh)
{
    if (!mesh) {
        return;
    }
    mesh->quads = NULL;
    mesh->count = 0;
    mesh->cap = 0;
    mesh->overflow = 0;
}

void pc_ui_hist_mesh_free(pc_ui_hist_mesh *mesh)
{
    if (!mesh) {
        return;
    }
    free(mesh->quads);
    mesh->quads = NULL;
    mesh->count = 0;
    mesh->cap = 0;
    mesh->overflow = 0;
}

void pc_ui_hist_mesh_clear(pc_ui_hist_mesh *mesh)
{
    if (!mesh) {
        return;
    }
    mesh->count = 0;
    mesh->overflow = 0;
}

/* f32 clamp with the same NaN behaviour as the reference's hand-written clamp: NaN stays NaN. */
static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void pc_ui_rgb_histogram_curves(const pc_algo_rgb_histogram *h, float out[3][BINS])
{
    if (!out) {
        return;
    }
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < BINS; i++) {
            out[c][i] = 0.0f;
        }
        if (!h) {
            continue;
        }
        const uint64_t *bins = h->channels[c];
        for (int i = 0; i < BINS; i++) {
            float center = (float)bins[i];
            if (i == 0 || i == BINS - 1) {
                out[c][i] = center;
            } else {
                /* sample(j) = bins[clamp(j, 1, BINS-2)]; the reference clamps every tap. */
                int jm2 = i - 2;
                int jm1 = i - 1;
                int jp1 = i + 1;
                int jp2 = i + 2;
                jm2 = jm2 < 1 ? 1 : (jm2 > BINS - 2 ? BINS - 2 : jm2);
                jm1 = jm1 < 1 ? 1 : (jm1 > BINS - 2 ? BINS - 2 : jm1);
                jp1 = jp1 < 1 ? 1 : (jp1 > BINS - 2 ? BINS - 2 : jp1);
                jp2 = jp2 < 1 ? 1 : (jp2 > BINS - 2 ? BINS - 2 : jp2);
                out[c][i] = ((float)bins[jm2] + 4.0f * (float)bins[jm1] + 6.0f * center +
                             4.0f * (float)bins[jp1] + (float)bins[jp2]) /
                            16.0f;
            }
        }
    }
    /* One scale for RGB. Edge clipping spikes are preserved but excluded from flattening the
     * interior; a flat black/white image uses its edge peak. */
    float interior = 0.0f;
    for (int c = 0; c < 3; c++) {
        for (int i = 1; i < BINS - 1; i++) {
            interior = fmaxf(interior, out[c][i]);
        }
    }
    float peak = interior;
    if (!(peak > 0.0f)) {
        peak = 0.0f;
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < BINS; i++) {
                peak = fmaxf(peak, out[c][i]);
            }
        }
    }
    float scale = peak > 0.0f ? peak * 1.1f : 1.0f;
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < BINS; i++) {
            out[c][i] = fminf(out[c][i] / scale, 1.0f);
        }
    }
}

/* Append a trapezoid to the mesh; drop it (set overflow) past the hard cap. The four corners mirror
 * the reference `quad`: y = bottom - height * rect_height for each of bottom[0],top[0],top[1],bottom[1]. */
static void push_quad(pc_ui_hist_mesh *mesh, float ry_top, float ry_bottom, const float x[2],
                      const float bottom[2], const float top[2], pc_ui_color color)
{
    if (!mesh) {
        return;
    }
    if (mesh->count >= PC_UI_HIST_MAX_QUADS) {
        mesh->overflow = 1;
        return;
    }
    if (mesh->count >= mesh->cap) {
        void *grown = pc_grow_array(mesh->quads, &mesh->cap, mesh->count + 1, sizeof *mesh->quads);
        if (!grown) {
            mesh->overflow = 1;
            return;
        }
        mesh->quads = grown;
    }
    float rh = ry_bottom - ry_top;
    pc_ui_hist_quad *q = &mesh->quads[mesh->count++];
    q->p[0][0] = x[0];
    q->p[0][1] = ry_bottom - bottom[0] * rh;
    q->p[1][0] = x[0];
    q->p[1][1] = ry_bottom - top[0] * rh;
    q->p[2][0] = x[1];
    q->p[2][1] = ry_bottom - top[1] * rh;
    q->p[3][0] = x[1];
    q->p[3][1] = ry_bottom - bottom[1] * rh;
    q->color = color;
}

/* f32 total order comparison (matches Rust f32::total_cmp for the finite values used here). */
static int cmp_f32(const void *pa, const void *pb)
{
    float a = *(const float *)pa;
    float b = *(const float *)pb;
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

void pc_ui_rgb_histogram_span(pc_ui_hist_mesh *mesh, float rx0, float ry0, float rx1, float ry1,
                              const float x[2], const float a[3], const float b[3],
                              const pc_ui_tokens *t)
{
    (void)rx0;
    (void)rx1;
    if (!mesh || !x || !a || !b || !t) {
        return;
    }
    /* Split where two RGB curves cross so every subspan has a stable channel order. */
    float cuts[5] = {0.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    int n = 2;
    for (int i = 0; i < 3; i++) {
        for (int j = i + 1; j < 3; j++) {
            float denom = (b[i] - a[i]) - (b[j] - a[j]);
            if (fabsf(denom) > FLT_EPSILON) {
                float crossing = (a[j] - a[i]) / denom;
                if (crossing > 0.0f && crossing < 1.0f && n < 5) {
                    cuts[n++] = crossing;
                }
            }
        }
    }
    qsort(cuts, (size_t)n, sizeof cuts[0], cmp_f32);
    for (int k = 0; k + 1 < n; k++) {
        float start = cuts[k];
        float end = cuts[k + 1];
        if (end <= start) {
            continue;
        }
        float left[3];
        float right[3];
        for (int c = 0; c < 3; c++) {
            left[c] = a[c] + (b[c] - a[c]) * start;
            right[c] = a[c] + (b[c] - a[c]) * end;
        }
        /* Stable sort of the three channels by (left+right) ascending; ties keep channel order,
         * matching total_cmp on the finite sums the reference produces. */
        int order[3] = {0, 1, 2};
        float sum[3];
        for (int c = 0; c < 3; c++) {
            sum[c] = left[c] + right[c];
        }
        for (int p = 1; p < 3; p++) {
            int key = order[p];
            int q = p - 1;
            while (q >= 0 && sum[order[q]] > sum[key]) {
                order[q + 1] = order[q];
                q--;
            }
            order[q + 1] = key;
        }
        static const uint8_t bitof[3] = {1, 2, 4};
        float sx[2] = {x[0] + (x[1] - x[0]) * start, x[0] + (x[1] - x[0]) * end};
        float floor_h[2] = {0.0f, 0.0f};
        uint8_t mask = 7;
        for (int li = 0; li < 3; li++) {
            int channel = order[li];
            float top[2] = {left[channel], right[channel]};
            if (top[0] > floor_h[0] || top[1] > floor_h[1]) {
                push_quad(mesh, ry0, ry1, sx, floor_h, top, pc_ui_tokens_histogram_fill(t, mask));
            }
            floor_h[0] = top[0];
            floor_h[1] = top[1];
            mask = (uint8_t)(mask & ~bitof[channel]);
        }
    }
}

void pc_ui_rgb_histogram_build(pc_ui_hist_mesh *mesh, float x0, float y0, float x1, float y1,
                               const pc_algo_rgb_histogram *h, const pc_ui_tokens *t)
{
    if (!mesh) {
        return;
    }
    pc_ui_hist_mesh_clear(mesh);
    if (!t) {
        return;
    }
    int finite = isfinite(x0) && isfinite(y0) && isfinite(x1) && isfinite(y1);
    if (!finite || !(x1 > x0) || !(y1 > y0)) {
        return;
    }
    float curves[3][BINS];
    pc_ui_rgb_histogram_curves(h, curves);
    float width = x1 - x0;
    float prev_x = x0;
    float prev[3] = {curves[0][0], curves[1][0], curves[2][0]};
    for (int i = 0; i < BINS; i++) {
        float x = x0 + ((float)i + 0.5f) / (float)BINS * width;
        float next[3] = {curves[0][i], curves[1][i], curves[2][i]};
        float xr[2] = {prev_x, x};
        pc_ui_rgb_histogram_span(mesh, x0, y0, x1, y1, xr, prev, next, t);
        prev_x = x;
        prev[0] = next[0];
        prev[1] = next[1];
        prev[2] = next[2];
    }
    float xr[2] = {prev_x, x1};
    pc_ui_rgb_histogram_span(mesh, x0, y0, x1, y1, xr, prev, prev, t);
}

size_t pc_ui_rgb_histogram_outline(const float curves[3][BINS], int channel, float x0, float y0,
                                   float x1, float y1, float (*pts)[2], size_t max)
{
    if (!curves || !pts || channel < 0 || channel > 2) {
        return 0;
    }
    int finite = isfinite(x0) && isfinite(y0) && isfinite(x1) && isfinite(y1);
    if (!finite || !(x1 > x0) || !(y1 > y0)) {
        return 0;
    }
    const float *values = curves[channel];
    int any = 0;
    for (int i = 0; i < BINS; i++) {
        if (values[i] > 0.0f) {
            any = 1;
            break;
        }
    }
    if (!any) {
        return 0;
    }
    float width = x1 - x0;
    float height = y1 - y0;
    size_t count = 0;
    if (count < max) {
        pts[count][0] = x0;
        pts[count][1] = y1 - values[0] * height;
        count++;
    }
    for (int i = 0; i < BINS && count < max; i++) {
        pts[count][0] = x0 + ((float)i + 0.5f) / (float)BINS * width;
        pts[count][1] = y1 - values[i] * height;
        count++;
    }
    if (count < max) {
        pts[count][0] = x1;
        pts[count][1] = y1 - values[BINS - 1] * height;
        count++;
    }
    return count;
}

float pc_ui_rgb_histogram_sample_x(float x0, float x1, const float rgb[3])
{
    if (!rgb || !isfinite(x0) || !isfinite(x1) || !isfinite(rgb[0]) || !isfinite(rgb[1]) ||
        !isfinite(rgb[2])) {
        return -1.0f;
    }
    float r = clampf(rgb[0], 0.0f, 1.0f);
    float g = clampf(rgb[1], 0.0f, 1.0f);
    float b = clampf(rgb[2], 0.0f, 1.0f);
    float tone = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return x0 + tone * (x1 - x0);
}
