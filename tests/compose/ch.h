/* Shared helpers of the tests/compose executables (header-only; each test is one TU). */
#ifndef PC_TESTS_COMPOSE_CH_H
#define PC_TESTS_COMPOSE_CH_H

#include <stdlib.h>
#include <string.h>

#include "compose/compose.h"
#include "harness.h"

#define CH_E (2.0f / 255.0f)

static inline bool ch_close4(const float *a, const float b[4])
{
    int i;
    if (a == NULL) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        if (!(fabsf(a[i] - b[i]) <= CH_E)) {
            return false;
        }
    }
    return true;
}

static inline pc_document ch_doc_white(uint32_t w, uint32_t h)
{
    pc_document d;
    if (pc_document_with_background(&d, "t", pc_size_make(w, h), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                    PC_COLOR_WHITE, NULL) != PC_OK) {
        abort(); /* test setup only */
    }
    return d;
}

static inline pc_document ch_doc(uint32_t w, uint32_t h, pc_color_mode mode, pc_sample_type depth)
{
    pc_document d;
    if (pc_document_init(&d, "d", pc_size_make(w, h), mode, depth, NULL) != PC_OK) {
        abort();
    }
    return d;
}

static inline pc_layer ch_raster(const char *name, pc_pixel_format f)
{
    pc_layer l;
    if (pc_layer_raster(&l, name, f, NULL) != PC_OK) {
        abort();
    }
    return l;
}

static inline void ch_fill(pc_layer *l, pc_rect r, const float *px, size_t n)
{
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(l), r, px, n, NULL) == PC_OK);
}

static inline pc_layer ch_solid(const char *name, pc_rect r, float cr, float cg, float cb, float ca)
{
    pc_layer l = ch_raster(name, PC_PIXEL_FORMAT_RGBA8);
    const float px[4] = {cr, cg, cb, ca};
    ch_fill(&l, r, px, 4);
    return l;
}

/* Appends at the top of the root (takes over *l). */
static inline pc_layer *ch_push(pc_document *d, pc_layer l)
{
    pc_layer_id id;
    if (pc_document_insert_above(d, NULL, &l, &id, NULL) != PC_OK) {
        abort();
    }
    return &d->layers[d->nlayers - 1];
}

/* A group of n layers (taken over); Pass Through like Layer::group. */
static inline pc_layer ch_group(const char *name, const pc_layer *children, size_t n)
{
    pc_layer g;
    pc_layer *arr = NULL;
    if (n > 0) {
        arr = malloc(n * sizeof *arr);
        if (arr == NULL) {
            abort();
        }
        memcpy(arr, children, n * sizeof *arr);
    }
    if (pc_layer_group(&g, name, arr, n, NULL) != PC_OK) {
        abort();
    }
    return g;
}

/* The composite pixel at (x, y) (zeros and a failed check when rendering fails). */
static inline void ch_px(const pc_document *d, int32_t x, int32_t y, float out[4])
{
    pc_compose_buffer b;
    pc_error e;
    memset(out, 0, 4 * sizeof(float));
    if (pc_compose_render(d, pc_rect_from_xywh(x, y, 1, 1), &b, &e) != PC_OK) {
        PC_T_FAIL("render failed: %s", e.msg);
        return;
    }
    memcpy(out, b.px[0], 4 * sizeof(float));
    pc_compose_buffer_fini(&b);
}

static inline bool ch_px_close(const pc_document *d, int32_t x, int32_t y, float r, float g, float b, float a)
{
    float p[4];
    const float want[4] = {r, g, b, a};
    ch_px(d, x, y, p);
    if (!ch_close4(p, want)) {
        (void)fprintf(stderr, "  pixel (%d,%d) = %g %g %g %g, want %g %g %g %g\n", x, y, (double)p[0],
                      (double)p[1], (double)p[2], (double)p[3], (double)r, (double)g, (double)b, (double)a);
        return false;
    }
    return true;
}

static inline pc_compose_buffer ch_render_tiled(const pc_document *d, pc_rect r, int32_t tile)
{
    pc_compose_buffer b;
    pc_error e;
    if (pc_compose_render_tiled(d, r, tile, NULL, &b, &e) != PC_OK) {
        PC_T_FAIL("render failed: %s", e.msg);
        memset(&b, 0, sizeof b);
    }
    return b;
}

static inline pc_path ch_polygon(const double (*pts)[2], size_t n)
{
    pc_path p;
    pc_subpath *s = calloc(1, sizeof *s);
    memset(&p, 0, sizeof p);
    if (s == NULL || pc_subpath_polygon(pts, n, s, NULL) != PC_OK) {
        abort();
    }
    p.subpaths = s;
    p.nsubpaths = 1;
    return p;
}

#endif
