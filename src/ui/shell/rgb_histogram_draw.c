/* Nuklear drawing for the RGB histogram ribbon. See rgb_histogram_draw.h.
 * The geometry is computed by the model (src/ui/rgb_histogram.c); this file only converts the
 * coloured quads to Nuklear fills and the per-channel curves to stroked polylines. */
#include "rgb_histogram_draw.h"

#include <math.h>

#include "ui/rgb_histogram.h"
#include "ui/widgets.h"

/* Nuklear rasterises to 16-bit vertices; keep every coordinate well within that range. */
static float clamp16(float v)
{
    if (!isfinite(v)) {
        return 0.0f;
    }
    if (v < -16000.0f) {
        return -16000.0f;
    }
    if (v > 16000.0f) {
        return 16000.0f;
    }
    return v;
}

void pc_ui_shell_rgb_histogram_paint(struct nk_context *ctx, struct nk_rect rect,
                                     const pc_algo_rgb_histogram *h, const pc_ui_tokens *t)
{
    if (!ctx || !h || !t) {
        return;
    }
    if (!isfinite(rect.x) || !isfinite(rect.y) || !(rect.w > 0.0f) || !(rect.h > 0.0f)) {
        return;
    }
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    if (!canvas) {
        return;
    }
    float x0 = rect.x;
    float y0 = rect.y;
    float x1 = rect.x + rect.w;
    float y1 = rect.y + rect.h;

    pc_ui_hist_mesh mesh;
    pc_ui_hist_mesh_init(&mesh);
    pc_ui_rgb_histogram_build(&mesh, x0, y0, x1, y1, h, t);
    for (size_t i = 0; i < mesh.count; i++) {
        const pc_ui_hist_quad *q = &mesh.quads[i];
        float pts[8] = {clamp16(q->p[0][0]), clamp16(q->p[0][1]), clamp16(q->p[1][0]),
                        clamp16(q->p[1][1]), clamp16(q->p[2][0]), clamp16(q->p[2][1]),
                        clamp16(q->p[3][0]), clamp16(q->p[3][1])};
        nk_fill_polygon(canvas, pts, 4, pc_ui_nk_color(q->color));
    }
    pc_ui_hist_mesh_free(&mesh);

    float curves[3][PC_UI_HIST_BINS];
    pc_ui_rgb_histogram_curves(h, curves);
    const float (*cc)[PC_UI_HIST_BINS] = (const float (*)[PC_UI_HIST_BINS])curves;
    static float line[(PC_UI_HIST_BINS + 2) * 2];
    for (int c = 0; c < 3; c++) {
        float pts[PC_UI_HIST_BINS + 2][2];
        size_t n = pc_ui_rgb_histogram_outline(cc, c, x0, y0, x1, y1, pts, PC_UI_HIST_BINS + 2);
        if (n < 2) {
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            line[i * 2] = clamp16(pts[i][0]);
            line[i * 2 + 1] = clamp16(pts[i][1]);
        }
        nk_stroke_polyline(canvas, line, (int)n, 1.0f,
                           pc_ui_nk_color(pc_ui_tokens_histogram_color(t, (uint8_t)(1 << c))));
    }
}
