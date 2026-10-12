/* Smaller PSD integration tests of crates/io/tests, ported one Rust #[test] per C test:
 *  - channels.rs: alpha / spot channels, channel options and the Quick Mask through PSD;
 *  - lab16.rs: 16-bit Lab uses Photoshop's chroma scale, Lab round trips at every depth;
 *  - large_psb.rs: an undecodable channel leaves its layer empty; the ~12 GB flattened 29000^2
 *    PSB (#[ignore] in Rust) runs only with PHOTOCRAFT_SLOW_TESTS=1;
 *  - layer_labels.rs: label <-> lclr indices, label round trips, a truncated lclr;
 *  - effects_multi.rs: several strokes / shadows and joined shape components round trip;
 *  - adjust_blend_roundtrip.rs: every adjustment kind and every blend mode through PSD. */
#include "harness.h"
#include "psd_rt.h"
#include "../psd/testgen.h"

#include "io/adjust_map.h"
#include "io/blocks.h"
#include "psd/builder.h"
#include "vector/vector.h"

static const pc_sample_type DEPTHS[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};

static bool color_eq(const pc_color *a, pc_color b)
{
    return a->mode == b.mode && a->c[0] == b.c[0] && a->c[1] == b.c[1] && a->c[2] == b.c[2] &&
           a->c[3] == b.c[3] && a->alpha == b.alpha;
}

static float px_at(const pc_surface *s, int32_t x, int32_t y, size_t c)
{
    float p[PC_RASTER_MAX_CHANNELS];
    pc_surface_read_pixel(s, x, y, p);
    return p[c];
}

static size_t count_resources(const pc_psd_file *f, uint16_t id)
{
    size_t n = 0;
    for (size_t i = 0; i < f->nresources; i++) {
        n += f->resources[i].id == id;
    }
    return n;
}

static pc_layer adj_layer(const char *name, const pc_adjustment *a)
{
    pc_layer_content c;
    pc_layer l;
    c.kind = PC_LAYER_ADJUSTMENT;
    must(pc_adjustment_clone(a, &c.u.adjustment, NULL));
    must(pc_layer_init(&l, name, &c, NULL));
    return l;
}

/* ================================ channels.rs ================================ */

static pc_surface channel_surface(pc_sample_type depth, int32_t x1)
{
    pc_surface s;
    const float one = 1.0f;
    pc_surface_init(&s, (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, depth, false});
    must(pc_surface_fill_rect(&s, pc_rect_make(0, 0, x1, 8), &one, 1, NULL));
    return s;
}

static pc_alpha_channel new_channel(const char *name, pc_surface s)
{
    pc_alpha_channel c;
    must(pc_alpha_channel_init(&c, name, &s, NULL));
    return c;
}

static void test_channel_options_spot_and_quick_mask_roundtrip(void)
{
    for (size_t k = 0; k < 3; k++) {
        pc_sample_type depth = DEPTHS[k];
        pc_document d;
        pc_document back;
        pc_psd_file f;
        pc_psd_file f2;
        pc_io_warnings w;
        must(pc_document_with_background(&d, "c", pc_size_make(16, 8), PC_COLOR_MODE_RGB, depth,
                                         PC_COLOR_WHITE, NULL));
        d.channels = xmalloc(2 * sizeof *d.channels);
        d.channels[0] = new_channel("Alpha 1", channel_surface(depth, 4));
        d.channels[0].color = pc_color_rgb(0.0f, 0.0f, 1.0f);
        d.channels[0].opacity = 0.7f;
        d.channels[0].indicates = PC_INDICATES_SELECTED_AREAS;
        d.channels[1] = new_channel("PANTONE X", channel_surface(depth, 8));
        d.channels[1].has_spot = true;
        d.channels[1].spot_color = pc_color_rgb(1.0f, 0.0f, 1.0f);
        d.channels[1].spot_solidity = 0.25f;
        d.nchannels = 2;
        d.quick_mask = new_channel("Quick Mask", channel_surface(depth, 12));
        d.has_quick_mask = true;

        must(pc_io_document_to_psd(&d, &f, NULL));
        pc_io_warnings_init(&w);
        must(pc_io_psd_to_document(&f, &back, &w, NULL));
        CHECK_EQ_UINT(w.n, 0);
        for (size_t i = 0; i < w.n; i++) {
            (void)fprintf(stderr, "  warning: %s\n", w.v[i]);
        }
        CHECK_EQ_UINT(back.nchannels, 2);
        if (back.nchannels == 2) {
            const pc_alpha_channel *a = &back.channels[0];
            const pc_alpha_channel *s = &back.channels[1];
            CHECK_EQ_STR(pc_str_c(&a->name), "Alpha 1");
            CHECK(color_eq(&a->color, pc_color_rgb(0.0f, 0.0f, 1.0f)));
            CHECK(a->indicates == PC_INDICATES_SELECTED_AREAS);
            CHECK_NEAR(a->opacity, 0.7f, 1e-6);
            CHECK(pc_surface_sample_channel(&a->surface, 2, 2, 0) == 1.0f);
            CHECK(pc_surface_sample_channel(&a->surface, 6, 2, 0) == 0.0f);
            CHECK_EQ_STR(pc_str_c(&s->name), "PANTONE X");
            CHECK(s->has_spot && color_eq(&s->spot_color, pc_color_rgb(1.0f, 0.0f, 1.0f)) &&
                  s->spot_solidity == 0.25f);
        }
        CHECK(back.has_quick_mask);
        if (back.has_quick_mask) {
            CHECK(pc_surface_sample_channel(&back.quick_mask.surface, 10, 2, 0) == 1.0f);
            CHECK(pc_surface_sample_channel(&back.quick_mask.surface, 14, 2, 0) == 0.0f);
        }
        /* Re-exporting doesn't duplicate the display-info resources. */
        must(pc_io_document_to_psd(&back, &f2, NULL));
        CHECK_EQ_UINT(count_resources(&f2, 1077), 1);
        CHECK_EQ_UINT(count_resources(&f2, 1022), 1);
        pc_psd_file_fini(&f2);
        pc_psd_file_fini(&f);
        pc_io_warnings_fini(&w);
        pc_document_fini(&back);
        pc_document_fini(&d);
    }

    {
        pc_document full;
        pc_io_export_result out;
        pc_psd_file file;
        must(pc_document_with_background(&full, "full", pc_size_make(1, 1), PC_COLOR_MODE_RGB,
                                         PC_SAMPLE_TYPE_U8, PC_COLOR_WHITE, NULL));
        full.channels = xmalloc(53 * sizeof *full.channels);
        for (size_t i = 0; i < 53; i++) {
            char name[32];
            pc_surface s;
            pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
            (void)snprintf(name, sizeof name, "Alpha %zu", i);
            full.channels[i] = new_channel(name, s);
        }
        full.nchannels = 53;
        {
            pc_surface s;
            pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
            full.quick_mask = new_channel("Quick Mask", s);
            full.has_quick_mask = true;
        }
        must(pc_io_export(&full, "full.psd", NULL, &out, NULL));
        CHECK(pc_io_warnings_any(&out.warnings, "Quick Mask"));
        must(pc_psd_file_parse(out.bytes, out.len, &file, NULL));
        CHECK_EQ_UINT(file.header.channels, 56);
        CHECK_EQ_UINT(count_resources(&file, 1022), 0);
        pc_psd_file_fini(&file);
        pc_io_export_result_fini(&out);
        pc_document_fini(&full);
    }
}

/* ================================ lab16.rs ================================ */

/* a* = 52.29, b* = -85.08 (a gradient stop of psd-tools 4x4_16bit_lab) on the document scale. */
static const float LAB_A = (52.29f + 128.0f) / 255.0f;
static const float LAB_B = (-85.08f + 128.0f) / 255.0f;

static pc_document lab_doc(pc_sample_type depth)
{
    pc_document d = t_doc("lab", 3, 2, PC_COLOR_MODE_LAB, depth);
    pc_surface s;
    pc_layer l;
    float px[24];
    for (size_t i = 0; i < 6; i++) {
        px[4 * i] = 0.1907f;
        px[4 * i + 1] = LAB_A;
        px[4 * i + 2] = LAB_B;
        px[4 * i + 3] = 1.0f;
    }
    pc_surface_init(&s, (pc_pixel_format){PC_COLOR_MODE_LAB, depth, true});
    must(pc_surface_write_region(&s, pc_rect_make(0, 0, 3, 2), px, 24, NULL));
    must(pc_io_raster_layer("px", &s, &l, NULL));
    t_push(&d, &l);
    return d;
}

static int32_t be16(const uint8_t *b, size_t i)
{
    return (int32_t)(((uint32_t)b[2 * i] << 8) | b[2 * i + 1]);
}

static void test_lab16_chroma_uses_photoshops_scale(void)
{
    pc_document doc = lab_doc(PC_SAMPLE_TYPE_U16);
    pc_buf out = rt_export(&doc, "x.psd", NULL);
    pc_psd_file file;
    pc_psd_bytes m = {NULL, 0};
    pc_psd_bytes plane = {NULL, 0};
    const size_t n = 6;
    int32_t a;
    int32_t b;
    must(pc_psd_file_parse(out.data, out.len, &file, NULL));
    CHECK_EQ_UINT(file.header.depth, 16);
    /* Merged image: planes L, a, b. */
    must(pc_psd_file_decode_merged(&file, &m, NULL));
    a = be16(m.data, n);
    b = be16(m.data, 2 * n);
    CHECK(abs(a - (32768 + (int32_t)roundf(256.0f * 52.29f))) <= 1);
    CHECK(abs(b - (32768 - (int32_t)roundf(256.0f * 85.08f))) <= 1);
    if (abs(a - (32768 + (int32_t)roundf(256.0f * 52.29f))) > 1 ||
        abs(b - (32768 - (int32_t)roundf(256.0f * 85.08f))) > 1) {
        (void)fprintf(stderr, "  a = %d, b = %d\n", (int)a, (int)b);
    }
    /* Layer channel 1 (a*) the same. */
    CHECK(file.has_layer_info && file.layer_info.n >= 1);
    if (file.has_layer_info && file.layer_info.n >= 1) {
        must(pc_psd_layer_decode_channel(&file.layer_info.layers[0], 1, 16, file.header.version, &plane,
                                         NULL));
        CHECK(abs(be16(plane.data, 0) - a) <= 1);
    }
    /* And back: the document values survive. */
    {
        pc_document back = rt_import("x.psd", &out);
        const pc_surface *s = back.nlayers > 0 ? pc_layer_surface(&back.layers[0]) : NULL;
        CHECK(s != NULL);
        if (s != NULL) {
            CHECK_NEAR(px_at(s, 1, 1, 1), LAB_A, 2e-5);
            CHECK_NEAR(px_at(s, 1, 1, 2), LAB_B, 2e-5);
        }
        pc_document_fini(&back);
    }
    pc_psd_bytes_fini(&plane);
    pc_psd_bytes_fini(&m);
    pc_psd_file_fini(&file);
    pc_buf_fini(&out);
    pc_document_fini(&doc);
}

static void test_lab_round_trips_at_every_depth(void)
{
    for (size_t k = 0; k < 3; k++) {
        pc_sample_type depth = DEPTHS[k];
        pc_document doc = lab_doc(depth);
        pc_buf out = rt_export(&doc, "x.psd", NULL);
        pc_document back = rt_import("x.psd", &out);
        const pc_surface *s = back.nlayers > 0 ? pc_layer_surface(&back.layers[0]) : NULL;
        double tol = depth == PC_SAMPLE_TYPE_U8 ? 0.5 / 255.0 + 1e-6 : 2e-5;
        pc_compose_buffer fa;
        pc_compose_buffer fb2;
        CHECK(s != NULL);
        if (s != NULL) {
            CHECK_NEAR(px_at(s, 2, 0, 1), LAB_A, tol);
            CHECK_NEAR(px_at(s, 2, 0, 2), LAB_B, tol);
        }
        must(pc_compose_flatten(&doc, &fa, NULL));
        must(pc_compose_flatten(&back, &fb2, NULL));
        {
            float worst = 0.0f;
            for (size_t i = 0; i < fa.len && i < fb2.len; i++) {
                for (int c = 0; c < 4; c++) {
                    worst = fmaxf(worst, fabsf(fa.px[i][c] - fb2.px[i][c]));
                }
            }
            if (worst > 1.0f / 255.0f) {
                PC_T_FAIL("%s: composite differs by %g", pc_sample_type_name(depth), (double)worst);
            }
            pc_t_checks++;
        }
        pc_compose_buffer_fini(&fa);
        pc_compose_buffer_fini(&fb2);
        pc_document_fini(&back);
        pc_buf_fini(&out);
        pc_document_fini(&doc);
    }
}

/* ================================ large_psb.rs ================================ */

/* A layer whose red channel is damaged: the warning says "treated as empty", and the layer must
 * be empty, not opaque black (the transparency channel is still fine). */
static void test_undecodable_channel_leaves_the_layer_empty(void)
{
    static const pc_psd_version VERSIONS[2] = {PC_PSD_V1, PC_PSD_V2};
    uint8_t px[64];
    memset(px, 200, sizeof px);
    for (size_t v = 0; v < 2; v++) {
        pc_psd_builder *b = pc_psd_builder_new(4, 4);
        pc_psd_layer_spec s =
            pc_psd_layer_spec_make("Damaged", 0, 0, 4, 4, (pc_psd_pixels){PC_PSD_PX_RGBA8, px, sizeof px});
        pc_psd_file file;
        pc_document doc;
        pc_io_warnings w;
        bool truncated = false;
        bool warned = false;
        if (b == NULL) {
            abort();
        }
        pc_psd_builder_version(b, VERSIONS[v]);
        pc_psd_builder_compression(b, PC_PSD_RLE);
        must(pc_psd_builder_push_layer(b, &s, NULL));
        must(pc_psd_builder_build(b, &file, NULL));
        for (size_t i = 0;
             file.has_layer_info && file.layer_info.n > 0 && i < file.layer_info.layers[0].nchannels; i++) {
            pc_psd_channel *c = &file.layer_info.layers[0].channels[i];
            if (c->id == 0 && c->data.len > 1) {
                c->data.len = 1;
                truncated = true;
            }
        }
        CHECK(truncated);
        pc_io_warnings_init(&w);
        must(pc_io_psd_to_document(&file, &doc, &w, NULL));
        for (size_t i = 0; i < w.n; i++) {
            warned = warned ||
                     (strstr(w.v[i], "Damaged") != NULL && strstr(w.v[i], "could not be decoded") != NULL);
        }
        CHECK(warned);
        if (!warned) {
            for (size_t i = 0; i < w.n; i++) {
                (void)fprintf(stderr, "  warning: %s\n", w.v[i]);
            }
        }
        CHECK(doc.nlayers >= 1 && pc_layer_surface(&doc.layers[0]) != NULL);
        if (doc.nlayers >= 1 && pc_layer_surface(&doc.layers[0]) != NULL) {
            float a = px_at(pc_layer_surface(&doc.layers[0]), 1, 1, 3);
            if (a != 0.0f) {
                PC_T_FAIL("version %d: the damaged layer must be transparent (alpha %g)", (int)VERSIONS[v],
                          (double)a);
            }
            pc_t_checks++;
        }
        pc_io_warnings_fini(&w);
        pc_document_fini(&doc);
        pc_psd_file_fini(&file);
        pc_psd_builder_free(b);
    }
}

/* A flattened 29000^2 RGB PSB: its merged image decodes to 2.52 GB, past the old 2 GiB cap.
 * Needs about 12 GB of memory: runs only with PHOTOCRAFT_SLOW_TESTS=1 (#[ignore] in Rust). */
static void test_flattened_psb_with_a_merged_image_over_2_gib_opens(void)
{
    const uint32_t side = 29000;
    const char *slow = getenv("PHOTOCRAFT_SLOW_TESTS");
    pc_psd_file file;
    pc_document doc;
    pc_io_warnings w;
    if (slow == NULL || strcmp(slow, "1") != 0) {
        (void)printf("  skipped %s (set PHOTOCRAFT_SLOW_TESTS=1; allocates ~12 GB)\n", pc_t_name);
        return;
    }
    file = tg_merged_only(PC_PSD_V2, PC_PSD_MODE_RGB, 8, PC_PSD_RLE, side, side);
    CHECK(file.image_data.len > 0);
    pc_io_warnings_init(&w);
    must(pc_io_psd_to_document(&file, &doc, &w, NULL));
    CHECK_EQ_UINT(w.n, 0);
    CHECK_EQ_UINT(doc.nlayers, 1);
    if (doc.nlayers == 1 && pc_layer_surface(&doc.layers[0]) != NULL) {
        const pc_surface *s = pc_layer_surface(&doc.layers[0]);
        const size_t pts[4][2] = {{0, 0}, {side - 1, 0}, {side / 2, side / 2}, {side - 2, side - 1}};
        for (size_t p = 0; p < 4; p++) {
            size_t x = pts[p][0];
            size_t y = pts[p][1];
            float got[PC_RASTER_MAX_CHANNELS];
            pc_surface_read_pixel(s, (int32_t)x, (int32_t)y, got);
            for (size_t c = 0; c < 3; c++) {
                /* testgen pattern plane at 8 bits, channel c seeded with c. */
                uint8_t want = (x / 4 + y) % 3 == 0 ? (uint8_t)(c * 17) : (uint8_t)(x * 3 + y * 7 + c);
                CHECK_EQ_UINT((uint8_t)lroundf(got[c] * 255.0f), want);
            }
            CHECK(got[3] == 1.0f);
        }
    }
    pc_io_warnings_fini(&w);
    pc_document_fini(&doc);
    pc_psd_file_fini(&file);
}

/* ================================ layer_labels.rs ================================ */

static void test_layer_labels_keep_the_psd_indices(void)
{
    static const pc_label_color labels[12] = {
        PC_LABEL_NONE,   PC_LABEL_RED,  PC_LABEL_ORANGE,  PC_LABEL_YELLOW, PC_LABEL_GREEN,   PC_LABEL_BLUE,
        PC_LABEL_VIOLET, PC_LABEL_GRAY, PC_LABEL_SEAFOAM, PC_LABEL_INDIGO, PC_LABEL_MAGENTA, PC_LABEL_FUCHSIA,
    };
    static const uint16_t out_of_range[3] = {12, 255, UINT16_MAX};
    for (uint16_t i = 0; i < 12; i++) {
        CHECK_EQ_UINT(pc_io_label_index(labels[i]), i);
        CHECK(pc_io_label_from_index(i) == labels[i]);
    }
    for (size_t i = 0; i < 3; i++) {
        CHECK(pc_io_label_from_index(out_of_range[i]) == PC_LABEL_NONE);
    }
}

static void test_layer_labels_psd_roundtrip_all_colors_and_depths(void)
{
    for (size_t k = 0; k < 3; k++) {
        pc_sample_type depth = DEPTHS[k];
        pc_document doc = t_doc("Labels", 2, 2, PC_COLOR_MODE_RGB, depth);
        pc_buf bytes;
        pc_document back;
        pc_walk wa;
        pc_walk wb;
        for (size_t i = 0; i < PC_LABEL_COLOR_COUNT; i++) {
            pc_label_color color = PC_LABEL_COLOR_ALL[i];
            pc_layer *child = xmalloc(sizeof *child);
            pc_layer group;
            must(pc_layer_raster(child, "Child", pc_document_pixel_format(&doc), NULL));
            child->label = color;
            must(pc_layer_group(&group, pc_label_color_label(color), child, 1, NULL));
            group.label = color;
            t_push(&doc, &group);
        }
        bytes = rt_export(&doc, "labels.psd", NULL);
        back = rt_import("labels.psd", &bytes);
        must(pc_document_walk(&doc, &wa, NULL));
        must(pc_document_walk(&back, &wb, NULL));
        CHECK_EQ_UINT(wb.len, wa.len);
        for (size_t i = 0; i < wa.len && i < wb.len; i++) {
            const pc_layer *x = wa.items[i].layer;
            const pc_layer *y = wb.items[i].layer;
            if (!pc_str_eq(&x->name, &y->name) || x->label != y->label) {
                PC_T_FAIL("%s: layer %zu: (%s, %s) vs (%s, %s)", pc_sample_type_name(depth), i,
                          pc_str_c(&x->name), pc_label_color_name(x->label), pc_str_c(&y->name),
                          pc_label_color_name(y->label));
            }
            pc_t_checks++;
        }
        CHECK(back.depth == depth);
        pc_walk_fini(&wa);
        pc_walk_fini(&wb);
        pc_document_fini(&back);
        pc_buf_fini(&bytes);
        pc_document_fini(&doc);
    }
}

static void test_layer_labels_truncated_sheet_color_is_not_a_label(void)
{
    static const uint8_t data[1] = {0};
    pc_psd_block b;
    must(pc_psd_block_init(&b, "lclr", data, 1, NULL));
    CHECK(pc_psd_block_has_typed(&b));
    CHECK(pc_psd_block_check_typed(&b, NULL) != PC_OK);
    pc_psd_block_fini(&b);
}

/* ================================ effects_multi.rs ================================ */

static pc_effect stroke_fx(float size, pc_stroke_position position, pc_fx_paint paint, float opacity)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_STROKE;
    e.u.stroke.common = pc_fx_common_new(PC_BLEND_NORMAL, opacity);
    e.u.stroke.size = size;
    e.u.stroke.position = position;
    e.u.stroke.paint = paint;
    return e;
}

static pc_fx_paint color_paint(pc_color c)
{
    pc_fx_paint p;
    memset(&p, 0, sizeof p);
    p.kind = PC_FX_PAINT_COLOR;
    p.u.color = c;
    return p;
}

static pc_document fx_roundtrip(const pc_document *d)
{
    pc_buf b = rt_export(d, "x.psd", NULL);
    pc_document back = rt_import("x.psd", &b);
    pc_buf_fini(&b);
    return back;
}

/* effects_multi.rs max_diff: premultiplied by alpha in every channel (alpha squared too). */
static float fx_max_diff(const pc_document *a, const pc_document *b)
{
    pc_compose_buffer x;
    pc_compose_buffer y;
    float m = 0.0f;
    must(pc_compose_flatten(a, &x, NULL));
    must(pc_compose_flatten(b, &y, NULL));
    CHECK_EQ_UINT(x.len, y.len);
    for (size_t i = 0; i < x.len && i < y.len; i++) {
        for (int c = 0; c < 4; c++) {
            m = fmaxf(m, fabsf(x.px[i][c] * x.px[i][3] - y.px[i][c] * y.px[i][3]));
        }
    }
    pc_compose_buffer_fini(&x);
    pc_compose_buffer_fini(&y);
    return m;
}

static void test_several_strokes_and_shadows_roundtrip(void)
{
    static const float red[4] = {0.8f, 0.2f, 0.1f, 1.0f};
    pc_document d;
    pc_document back;
    pc_layer l;
    pc_fx_paint gp;
    pc_effect *fx;
    must(pc_document_with_background(&d, "t", pc_size_make(64, 48), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                     PC_COLOR_WHITE, NULL));
    must(pc_layer_raster(&l, "blob", PC_PIXEL_FORMAT_RGBA8, NULL));
    must(pc_surface_fill_rect(pc_layer_surface_mut(&l), pc_rect_make(20, 14, 44, 34), red, 4, NULL));
    memset(&gp, 0, sizeof gp);
    gp.kind = PC_FX_PAINT_GRADIENT;
    must(pc_gradient_default(&gp.u.gradient, NULL));
    free(gp.u.gradient.stops);
    gp.u.gradient.stops = xmalloc(2 * sizeof *gp.u.gradient.stops);
    gp.u.gradient.stops[0] = (pc_color_stop){0.0f, pc_color_rgb(0.0f, 0.0f, 1.0f)};
    gp.u.gradient.stops[1] = (pc_color_stop){1.0f, pc_color_rgb(1.0f, 1.0f, 0.0f)};
    gp.u.gradient.nstops = 2;
    gp.u.gradient.angle = 30.0f;
    fx = xmalloc(6 * sizeof *fx);
    fx[0] = stroke_fx(2.0f, PC_STROKE_OUTSIDE, color_paint(pc_color_rgb(1.0f, 1.0f, 1.0f)), 1.0f);
    fx[1] = stroke_fx(5.0f, PC_STROKE_OUTSIDE, gp, 0.8f);
    fx[2] = stroke_fx(3.0f, PC_STROKE_INSIDE, color_paint(pc_color_rgb(0.0f, 0.5f, 0.0f)), 1.0f);
    fx[3] = stroke_fx(4.0f, PC_STROKE_CENTER, color_paint(pc_color_rgb(0.0f, 0.0f, 0.0f)), 0.5f);
    pc_effect_default_drop_shadow(&fx[4]);
    pc_effect_default_drop_shadow(&fx[5]);
    fx[5].u.shadow.distance = 9.0f;
    fx[5].u.shadow.angle = 45.0f;
    fx[5].u.shadow.use_global_light = false;
    l.effects.items = fx;
    l.effects.nitems = 6;
    t_push(&d, &l);
    back = fx_roundtrip(&d);
    CHECK(back.nlayers >= 2);
    if (back.nlayers >= 2) {
        const pc_effects *e = &back.layers[1].effects;
        static const float sizes[4] = {2.0f, 5.0f, 3.0f, 4.0f};
        static const pc_stroke_position pos[4] = {PC_STROKE_OUTSIDE, PC_STROKE_OUTSIDE, PC_STROKE_INSIDE,
                                                  PC_STROKE_CENTER};
        const pc_stroke_fx *strokes[8];
        size_t ns = 0;
        size_t nshadows = 0;
        for (size_t i = 0; i < e->nitems; i++) {
            if (e->items[i].kind == PC_FX_STROKE && ns < 8) {
                strokes[ns++] = &e->items[i].u.stroke;
            }
            nshadows += e->items[i].kind == PC_FX_DROP_SHADOW;
        }
        CHECK_EQ_UINT(ns, 4);
        if (ns == 4) {
            for (size_t i = 0; i < 4; i++) {
                if (strokes[i]->size != sizes[i] || strokes[i]->position != pos[i]) {
                    PC_T_FAIL("stroke %zu: (%g, %d) vs (%g, %d)", i, (double)strokes[i]->size,
                              (int)strokes[i]->position, (double)sizes[i], (int)pos[i]);
                }
                pc_t_checks++;
            }
            CHECK(strokes[1]->paint.kind == PC_FX_PAINT_GRADIENT);
            CHECK_NEAR(strokes[3]->common.opacity, 0.5, 1e-3);
        }
        CHECK_EQ_UINT(nshadows, 2);
    }
    {
        float m = fx_max_diff(&d, &back);
        if (!(m <= 1.0f / 255.0f + 1e-5f)) {
            PC_T_FAIL("render changed by %g", (double)m);
        }
        pc_t_checks++;
    }
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void px1(const pc_document *doc, int32_t x, int32_t y, float out[4])
{
    pc_compose_buffer b;
    must(pc_compose_render(doc, pc_rect_from_xywh(x, y, 1, 1), &b, NULL));
    memcpy(out, b.px[0], 4 * sizeof(float));
    pc_compose_buffer_fini(&b);
}

static void test_joined_shape_components_roundtrip(void)
{
    static const double outer_pts[4][2] = {{5.0, 5.0}, {35.0, 5.0}, {35.0, 35.0}, {5.0, 35.0}};
    static const double hole_pts[4][2] = {{12.0, 12.0}, {12.0, 28.0}, {28.0, 28.0}, {28.0, 12.0}};
    pc_document d;
    pc_document back;
    pc_layer_content c;
    pc_shape_layer *sh;
    pc_layer l;
    float p[4];
    must(pc_document_with_background(&d, "t", pc_size_make(40, 40), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                     PC_COLOR_WHITE, NULL));
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SHAPE;
    sh = &c.u.shape;
    sh->path.subpaths = xmalloc(2 * sizeof *sh->path.subpaths);
    must(pc_subpath_polygon(outer_pts, 4, &sh->path.subpaths[0], NULL));
    must(pc_subpath_polygon(hole_pts, 4, &sh->path.subpaths[1], NULL));
    sh->path.subpaths[1].op = PC_PATH_JOIN;
    sh->path.nsubpaths = 2;
    sh->path.fill_rule = PC_FILL_RULE_NONZERO;
    sh->path.inverted = false;
    sh->has_fill = true;
    sh->fill = pc_fill_solid(pc_color_rgb(0.2f, 0.4f, 0.9f));
    must(pc_vec_render_shape(sh, PC_PIXEL_FORMAT_RGBA8, pc_document_bounds(&d), &sh->cache, NULL));
    sh->has_cache = true;
    must(pc_layer_init(&l, "ring", &c, NULL));
    l.effects.items = xmalloc(sizeof *l.effects.items);
    l.effects.items[0] =
        stroke_fx(2.0f, PC_STROKE_OUTSIDE, color_paint(pc_color_rgb(1.0f, 0.0f, 0.0f)), 1.0f);
    l.effects.nitems = 1;
    t_push(&d, &l);
    /* The hole is stroked too (the outline is the joined component). */
    px1(&d, 20, 20, p);
    CHECK(p[1] > 0.9f); /* hole shows the background */
    px1(&d, 20, 13, p);
    CHECK(p[0] > 0.9f && p[1] < 0.1f); /* stroke inside the hole */
    if (!(p[0] > 0.9f && p[1] < 0.1f)) {
        (void)fprintf(stderr, "  stroke inside the hole: [%g, %g, %g, %g]\n", (double)p[0], (double)p[1],
                      (double)p[2], (double)p[3]);
    }
    back = fx_roundtrip(&d);
    CHECK(back.nlayers >= 2 && back.layers[1].content.kind == PC_LAYER_SHAPE);
    if (back.nlayers >= 2 && back.layers[1].content.kind == PC_LAYER_SHAPE) {
        const pc_path *path = &back.layers[1].content.u.shape.path;
        CHECK_EQ_UINT(path->nsubpaths, 2);
        if (path->nsubpaths == 2) {
            CHECK(path->subpaths[0].op == PC_PATH_COMBINE);
            CHECK(path->subpaths[1].op == PC_PATH_JOIN);
        }
    }
    {
        float m = fx_max_diff(&d, &back);
        if (!(m <= 1.0f / 255.0f + 1e-5f)) {
            PC_T_FAIL("render changed by %g", (double)m);
        }
        pc_t_checks++;
    }
    pc_document_fini(&back);
    pc_document_fini(&d);
}

/* ================================ adjust_blend_roundtrip.rs ================================ */

#define ADJ_TOL (1.0f / 255.0f + 1e-5f)
#define NADJ 28

static float q16(uint16_t v)
{
    return (float)v / 65535.0f;
}

typedef struct export_import_out {
    pc_document back;
    pc_io_warnings warnings;
    pc_psd_file file;
} export_import_out;

static export_import_out export_import(const pc_document *doc)
{
    export_import_out o;
    pc_io_export_result r;
    pc_error e;
    if (pc_io_export(doc, "x.psd", NULL, &r, &e) != PC_OK) {
        (void)fprintf(stderr, "export: %s\n", e.msg);
        abort();
    }
    must(pc_psd_file_parse(r.bytes, r.len, &o.file, NULL));
    {
        pc_buf b = PC_BUF_INIT;
        must(pc_buf_append(&b, r.bytes, r.len, NULL));
        o.back = rt_import("x.psd", &b);
        pc_buf_fini(&b);
    }
    o.warnings = r.warnings;
    memset(&r.warnings, 0, sizeof r.warnings);
    pc_io_export_result_fini(&r);
    return o;
}

static void export_import_fini(export_import_out *o)
{
    pc_document_fini(&o->back);
    pc_io_warnings_fini(&o->warnings);
    pc_psd_file_fini(&o->file);
}

static void composite_eq(const pc_document *a, const pc_document *b, const char *ctx)
{
    pc_compose_buffer x;
    pc_compose_buffer y;
    must(pc_compose_flatten(a, &x, NULL));
    must(pc_compose_flatten(b, &y, NULL));
    if (!pc_rect_eq(x.rect, y.rect)) {
        PC_T_FAIL("%s: composite rect", ctx);
    } else {
        float m = rt_max_diff(&x, &y);
        if (!(m <= ADJ_TOL)) {
            PC_T_FAIL("%s: composite differs by %g (> 1/255)", ctx, (double)m);
        }
    }
    pc_t_checks++;
    pc_compose_buffer_fini(&x);
    pc_compose_buffer_fini(&y);
}

static pc_curve curve_of(const uint8_t (*pts)[2], size_t n)
{
    pc_curve c;
    c.pts = xmalloc(n * sizeof *c.pts);
    c.len = n;
    for (size_t i = 0; i < n; i++) {
        c.pts[i].input = g255(pts[i][0]);
        c.pts[i].output = g255(pts[i][1]);
    }
    return c;
}

static pc_levels_channel lc(int a, int b, float gamma)
{
    return (pc_levels_channel){g255(a), g255(b), gamma, g255(5), g255(250)};
}

/* LutFile::from_fn(title, size, f).data as a blob of floats. */
static pc_blob *lut_blob(size_t size, void (*f)(const float in[3], float out[3]))
{
    float m = (float)((size < 2 ? 2 : size) - 1);
    size_t n = size * size * size * 3;
    float *data = xmalloc(n * sizeof *data);
    size_t i = 0;
    pc_blob *blob = NULL;
    for (size_t b = 0; b < size; b++) {
        for (size_t g = 0; g < size; g++) {
            for (size_t r = 0; r < size; r++) {
                float in[3] = {(float)r / m, (float)g / m, (float)b / m};
                float o[3];
                f(in, o);
                for (int c = 0; c < 3; c++) {
                    data[i++] = o[c] < 0.0f ? 0.0f : o[c] > 1.0f ? 1.0f : o[c];
                }
            }
        }
    }
    must(pc_blob_new(data, n * sizeof *data, &blob, NULL));
    free(data);
    return blob;
}

static void look_fn(const float c[3], float out[3])
{
    out[0] = c[1];
    out[1] = c[2] * 0.5f;
    out[2] = 1.0f - c[0];
}

static void corner_fn(const float c[3], float out[3])
{
    float v = c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f ? 1.0f : 0.0f;
    out[0] = v;
    out[1] = v;
    out[2] = v;
}

static void gm_stops(pc_adjustment *a, const pc_gradient_map_stop *s, size_t n, bool reverse)
{
    a->kind = PC_ADJ_GRADIENT_MAP;
    a->u.gradient_map.stops = n > 0 ? xmalloc(n * sizeof *s) : NULL;
    if (n > 0) {
        memcpy(a->u.gradient_map.stops, s, n * sizeof *s);
    }
    a->u.gradient_map.nstops = n;
    a->u.gradient_map.reverse = reverse;
    a->u.gradient_map.dither = false;
}

/* A compile-time guard like the Rust match: a new adjustment kind must be added below. */
_Static_assert(PC_ADJ_KIND_COUNT == 17, "add the new adjustment kind to adjustments()");

/* Every adjustment kind with representative settings, and whether its PSD encoding is lossless
 * (the document model comes back equal) or only renders the same. */
static void adjustments(pc_adjustment a[NADJ], bool lossless[NADJ])
{
    static const uint8_t m3[3][2] = {{0, 0}, {128, 150}, {255, 255}};
    static const uint8_t r2[2][2] = {{0, 10}, {255, 255}};
    static const uint8_t g2[2][2] = {{0, 0}, {255, 245}};
    static const uint8_t b3[3][2] = {{0, 0}, {64, 32}, {255, 255}};
    size_t n = 0;
    memset(a, 0, NADJ * sizeof *a);
    for (size_t i = 0; i < NADJ; i++) {
        lossless[i] = true;
    }
    a[n].kind = PC_ADJ_BRIGHTNESS_CONTRAST;
    a[n].u.brightness_contrast.brightness = 30.0f;
    a[n++].u.brightness_contrast.contrast = -20.0f;
    a[n].kind = PC_ADJ_BRIGHTNESS_CONTRAST;
    a[n].u.brightness_contrast.brightness = -40.0f;
    a[n].u.brightness_contrast.contrast = 50.0f;
    a[n++].u.brightness_contrast.legacy = true;
    a[n].kind = PC_ADJ_LEVELS;
    a[n].u.levels.master = lc(10, 240, 1.2f);
    a[n].u.levels.per_channel[0] = lc(0, 255, 1.0f);
    a[n].u.levels.per_channel[1] = lc(5, 250, 0.8f);
    a[n].u.levels.per_channel[2] = lc(20, 200, 1.5f);
    a[n].u.levels.space = PC_TONE_RGB;
    a[n++].u.levels.black = pc_levels_channel_default();
    a[n].kind = PC_ADJ_CURVES;
    a[n].u.curves.master = curve_of(m3, 3);
    a[n].u.curves.per_channel[0] = curve_of(r2, 2);
    a[n].u.curves.per_channel[1] = curve_of(g2, 2);
    a[n].u.curves.per_channel[2] = curve_of(b3, 3);
    a[n++].u.curves.space = PC_TONE_RGB;
    a[n].kind = PC_ADJ_EXPOSURE;
    a[n].u.exposure.exposure = 0.75f;
    a[n].u.exposure.offset = -0.0125f;
    a[n++].u.exposure.gamma = 0.9f;
    a[n].kind = PC_ADJ_VIBRANCE;
    a[n].u.vibrance.vibrance = 60.0f;
    a[n++].u.vibrance.saturation = -25.0f;
    a[n].kind = PC_ADJ_HUE_SATURATION;
    a[n].u.hue_saturation.hue = 40.0f;
    a[n].u.hue_saturation.saturation = -30.0f;
    a[n].u.hue_saturation.lightness = 10.0f;
    pc_hue_range_defaults(a[n++].u.hue_saturation.ranges);
    a[n].kind = PC_ADJ_HUE_SATURATION;
    a[n].u.hue_saturation.hue = 200.0f;
    a[n].u.hue_saturation.saturation = 50.0f;
    a[n].u.hue_saturation.lightness = -10.0f;
    a[n].u.hue_saturation.colorize = true;
    pc_hue_range_defaults(a[n++].u.hue_saturation.ranges);
    {
        const float s[3] = {20.0f, -10.0f, 5.0f};
        const float m[3] = {-15.0f, 10.0f, 30.0f};
        const float h[3] = {0.0f, 5.0f, -20.0f};
        a[n].kind = PC_ADJ_COLOR_BALANCE;
        memcpy(a[n].u.color_balance.shadows, s, sizeof s);
        memcpy(a[n].u.color_balance.midtones, m, sizeof m);
        memcpy(a[n].u.color_balance.highlights, h, sizeof h);
        a[n++].u.color_balance.preserve_luminosity = true;
    }
    a[n].kind = PC_ADJ_COLOR_BALANCE;
    a[n].u.color_balance.shadows[0] = 60.0f;
    a[n].u.color_balance.highlights[2] = 40.0f;
    a[n++].u.color_balance.preserve_luminosity = false;
    {
        const float w1[6] = {40.0f, 60.0f, 40.0f, 60.0f, 20.0f, 80.0f};
        const float w2[6] = {70.0f, 20.0f, 50.0f, -10.0f, 90.0f, 130.0f};
        a[n].kind = PC_ADJ_BLACK_WHITE;
        memcpy(a[n++].u.black_white.weights, w1, sizeof w1);
        a[n].kind = PC_ADJ_BLACK_WHITE;
        memcpy(a[n].u.black_white.weights, w2, sizeof w2);
        a[n].u.black_white.has_tint = true;
        a[n].u.black_white.tint[0] = 0.9f;
        a[n].u.black_white.tint[1] = 0.7f;
        a[n++].u.black_white.tint[2] = 0.5f;
    }
    a[n].kind = PC_ADJ_PHOTO_FILTER;
    a[n].u.photo_filter.color[0] = q16(60620);
    a[n].u.photo_filter.color[1] = q16(35455);
    a[n].u.photo_filter.color[2] = q16(0);
    a[n].u.photo_filter.density = 0.4f;
    a[n++].u.photo_filter.preserve_luminosity = true;
    a[n].kind = PC_ADJ_PHOTO_FILTER;
    a[n].u.photo_filter.color[0] = q16(0);
    a[n].u.photo_filter.color[1] = q16(39321);
    a[n].u.photo_filter.color[2] = q16(65535);
    a[n].u.photo_filter.density = 0.3f;
    a[n++].u.photo_filter.preserve_luminosity = false;
    {
        const float m1[3][4] = {
            {0.5f, 0.3f, 0.2f, 0.0f}, {0.1f, 0.8f, 0.1f, 0.05f}, {0.0f, 0.2f, 0.9f, -0.05f}};
        const float m2[3][4] = {{0.4f, 0.4f, 0.2f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
        a[n].kind = PC_ADJ_CHANNEL_MIXER;
        memcpy(a[n++].u.channel_mixer.matrix, m1, sizeof m1);
        a[n].kind = PC_ADJ_CHANNEL_MIXER;
        memcpy(a[n].u.channel_mixer.matrix, m2, sizeof m2);
        a[n++].u.channel_mixer.monochrome = true;
    }
    a[n].kind = PC_ADJ_COLOR_LOOKUP;
    must(pc_str_set_c(&a[n].u.color_lookup.name, "Look.cube", NULL));
    a[n].u.color_lookup.lut = lut_blob(5, look_fn);
    a[n++].u.color_lookup.size = 5;
    /* No table chosen: written as an identity cube (renders the same, comes back with a table). */
    a[n].kind = PC_ADJ_COLOR_LOOKUP;
    lossless[n++] = false;
    a[n++].kind = PC_ADJ_INVERT;
    a[n].kind = PC_ADJ_POSTERIZE;
    a[n++].u.posterize.levels = 4;
    a[n].kind = PC_ADJ_THRESHOLD;
    a[n++].u.threshold.level = g255(140);
    {
        const pc_gradient_map_stop s1[3] = {
            {0.0f, {q16(6554), 0.0f, q16(19661)}}, {0.5f, {1.0f, 0.2f, 0.0f}}, {1.0f, {1.0f, 1.0f, 0.8f}}};
        const pc_gradient_map_stop s2[2] = {{0.0f, {0.0f, 0.0f, 1.0f}}, {1.0f, {1.0f, 1.0f, 0.0f}}};
        const pc_gradient_map_stop s3[1] = {{0.4f, {0.2f, 0.6f, 0.4f}}};
        gm_stops(&a[n++], s1, 3, false);
        gm_stops(&a[n++], s2, 2, true);
        /* Fewer than two stops: written as the equivalent two-stop gradient. */
        gm_stops(&a[n], NULL, 0, false);
        lossless[n++] = false;
        gm_stops(&a[n], s3, 1, false);
        lossless[n++] = false;
    }
    a[n].kind = PC_ADJ_SELECTIVE_COLOR;
    a[n].u.selective_color.relative = true;
    for (size_t r = 0; r < 9; r++) {
        a[n].u.selective_color.adjustments[r][0] = (float)r * 10.0f - 40.0f;
        a[n].u.selective_color.adjustments[r][1] = 5.0f;
        a[n].u.selective_color.adjustments[r][2] = -30.0f;
        a[n].u.selective_color.adjustments[r][3] = 20.0f;
    }
    n++;
    a[n].kind = PC_ADJ_SELECTIVE_COLOR;
    a[n].u.selective_color.relative = false;
    for (size_t r = 0; r < 9; r++) {
        a[n].u.selective_color.adjustments[r][1] = (float)r * 5.0f;
        a[n].u.selective_color.adjustments[r][3] = -10.0f;
    }
    n++;
    /* A block this version cannot read (a truncated Selective Color) is kept verbatim. */
    a[n].kind = PC_ADJ_UNSUPPORTED;
    must(pc_str_set_c(&a[n].u.unsupported.psd_key, "selc", NULL));
    must(pc_blob_new((const uint8_t[4]){0, 1, 0, 0}, 4, &a[n++].u.unsupported.raw, NULL));
    if (n != NADJ) {
        abort();
    }
}

/* A colourful base, a clipping base, and adj three times: plain; at half opacity with a mask
 * and a blend mode; clipped to a pixel layer. */
static pc_document adjustment_doc(const pc_adjustment *adj, pc_sample_type depth)
{
    pc_document d = t_doc("adj", 32, 20, PC_COLOR_MODE_RGB, depth);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    pc_layer l = t_raster("Background", fmt, pc_document_bounds(&d), 1, false);
    t_push(&d, &l);
    l = adj_layer("plain", adj);
    l.opacity = 1.0f;
    t_push(&d, &l);
    l = adj_layer("masked", adj);
    l.opacity = g255(128);
    l.blend = PC_BLEND_MULTIPLY;
    l.has_mask = true;
    l.mask.surface = t_mask_surface(depth, pc_rect_make(4, 3, 20, 15), 1.0f, 3);
    l.mask.enabled = true;
    l.mask.linked = true;
    l.mask.density = 1.0f;
    l.mask.feather = 0.0f;
    t_push(&d, &l);
    l = t_raster("clip base", fmt, pc_rect_make(10, 2, 28, 18), 9, true);
    t_push(&d, &l);
    l = adj_layer("clipped", adj);
    l.clipped = true;
    l.fill_opacity = g255(200);
    t_push(&d, &l);
    return d;
}

static bool has_adjustment_block(const pc_psd_layer *l)
{
    for (size_t i = 0; i < l->nblocks; i++) {
        for (size_t k = 0; k < 16; k++) {
            if (memcmp(l->blocks[i].key, PC_IO_ADJUSTMENT_KEYS[k], 4) == 0) {
                return true;
            }
        }
    }
    return false;
}

static void check_adjustment(size_t idx, const pc_adjustment *adj, bool lossless, pc_sample_type depth)
{
    char ctx[128];
    pc_document d = adjustment_doc(adj, depth);
    export_import_out o = export_import(&d);
    size_t with_adj = 0;
    (void)snprintf(ctx, sizeof ctx, "#%zu %s @ %s", idx, pc_adjustment_label(adj),
                   pc_sample_type_name(depth));
    if (pc_io_warnings_any(&o.warnings, "adjustment")) {
        PC_T_FAIL("%s: export warned", ctx);
        for (size_t i = 0; i < o.warnings.n; i++) {
            (void)fprintf(stderr, "  warning: %s\n", o.warnings.v[i]);
        }
    }
    pc_t_checks++;
    /* The adjustment layers carry an adjustment block, never an empty pixel layer. */
    for (size_t i = 0; o.file.has_layer_info && i < o.file.layer_info.n; i++) {
        with_adj += has_adjustment_block(&o.file.layer_info.layers[i]);
    }
    if (with_adj != 3) {
        PC_T_FAIL("%s: adjustment blocks written: %zu", ctx, with_adj);
    }
    pc_t_checks++;
    if (o.back.nlayers != d.nlayers) {
        PC_T_FAIL("%s: layer count %zu vs %zu", ctx, o.back.nlayers, d.nlayers);
    }
    pc_t_checks++;
    for (size_t i = 0; i < d.nlayers && i < o.back.nlayers; i++) {
        const pc_layer *x = &d.layers[i];
        const pc_layer *y = &o.back.layers[i];
        if (x->content.kind == PC_LAYER_ADJUSTMENT && y->content.kind == PC_LAYER_ADJUSTMENT) {
            const pc_adjustment *a = &x->content.u.adjustment;
            const pc_adjustment *b = &y->content.u.adjustment;
            if (lossless ? !pc_adjustment_eq(a, b) : a->kind != b->kind) {
                PC_T_FAIL("%s: %s: adjustment %s", ctx, pc_str_c(&x->name), lossless ? "differs" : "kind");
            }
            if (x->blend != y->blend || x->opacity != y->opacity || x->fill_opacity != y->fill_opacity ||
                x->clipped != y->clipped) {
                PC_T_FAIL("%s: %s: blend/opacity/fill/clipped (%s %g %g %d vs %s %g %g %d)", ctx,
                          pc_str_c(&x->name), pc_blend_name(x->blend), (double)x->opacity,
                          (double)x->fill_opacity, x->clipped, pc_blend_name(y->blend), (double)y->opacity,
                          (double)y->fill_opacity, y->clipped);
            }
            if (x->has_mask != y->has_mask) {
                PC_T_FAIL("%s: %s mask", ctx, pc_str_c(&x->name));
            }
        } else if (!(x->content.kind == PC_LAYER_RASTER && y->content.kind == PC_LAYER_RASTER)) {
            PC_T_FAIL("%s: %s became %s", ctx, pc_layer_content_kind_name(&x->content),
                      pc_layer_content_kind_name(&y->content));
        }
        pc_t_checks++;
    }
    if (lossless) {
        rt_docs_eq(&d, &o.back);
    }
    composite_eq(&d, &o.back, ctx);
    /* A second generation is a fixed point for the document model. */
    {
        export_import_out again = export_import(&o.back);
        for (size_t i = 0; i < o.back.nlayers && i < again.back.nlayers; i++) {
            const pc_layer *x = &o.back.layers[i];
            const pc_layer *y = &again.back.layers[i];
            if (x->content.kind == PC_LAYER_ADJUSTMENT && y->content.kind == PC_LAYER_ADJUSTMENT) {
                if (!pc_adjustment_eq(&x->content.u.adjustment, &y->content.u.adjustment)) {
                    PC_T_FAIL("%s: %s: second generation", ctx, pc_str_c(&x->name));
                }
                pc_t_checks++;
            }
        }
        export_import_fini(&again);
    }
    export_import_fini(&o);
    pc_document_fini(&d);
}

static void every_adjustment_kind(pc_sample_type depth)
{
    pc_adjustment a[NADJ];
    bool lossless[NADJ];
    adjustments(a, lossless);
    for (size_t i = 0; i < NADJ; i++) {
        check_adjustment(i, &a[i], lossless[i], depth);
        pc_adjustment_fini(&a[i]);
    }
}

static void test_every_adjustment_kind_round_trips_8bit(void)
{
    every_adjustment_kind(PC_SAMPLE_TYPE_U8);
}

static void test_every_adjustment_kind_round_trips_16bit(void)
{
    every_adjustment_kind(PC_SAMPLE_TYPE_U16);
}

/* Photoshop's clrL has no interpolation setting, so tetrahedral comes back trilinear: the save
 * must say so (#1347). Trilinear saves without a warning. */
static void test_color_lookup_tetrahedral_warns_on_psd_save(void)
{
    pc_blob *lut = lut_blob(2, corner_fn);
    for (int t = 0; t < 2; t++) {
        bool tetrahedral = t == 0;
        pc_adjustment adj;
        pc_document d = t_doc("lut", 8, 8, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        pc_layer l = t_raster("Background", pc_document_pixel_format(&d), pc_document_bounds(&d), 1, false);
        export_import_out o;
        bool warned = false;
        const pc_layer_content *content;
        t_push(&d, &l);
        memset(&adj, 0, sizeof adj);
        adj.kind = PC_ADJ_COLOR_LOOKUP;
        must(pc_str_set_c(&adj.u.color_lookup.name, "test.cube", NULL));
        adj.u.color_lookup.lut = pc_blob_retain(lut);
        adj.u.color_lookup.size = 2;
        adj.u.color_lookup.tetrahedral = tetrahedral;
        l = adj_layer("Look", &adj);
        pc_adjustment_fini(&adj);
        t_push(&d, &l);
        o = export_import(&d);
        for (size_t i = 0; i < o.warnings.n; i++) {
            const char *w = o.warnings.v[i];
            warned = warned || (strstr(w, "\"Look\"") != NULL && strstr(w, "tetrahedral") != NULL &&
                                strstr(w, "trilinear") != NULL);
        }
        if (warned != tetrahedral) {
            PC_T_FAIL("tetrahedral=%d: warned=%d", tetrahedral, warned);
            for (size_t i = 0; i < o.warnings.n; i++) {
                (void)fprintf(stderr, "  warning: %s\n", o.warnings.v[i]);
            }
        }
        pc_t_checks++;
        content = o.back.nlayers > 1 ? &o.back.layers[1].content : NULL;
        if (content == NULL || content->kind != PC_LAYER_ADJUSTMENT ||
            content->u.adjustment.kind != PC_ADJ_COLOR_LOOKUP ||
            content->u.adjustment.u.color_lookup.lut == NULL ||
            content->u.adjustment.u.color_lookup.size != 2) {
            PC_T_FAIL("Color Lookup lost: %s",
                      content != NULL ? pc_layer_content_kind_name(content) : "(none)");
        } else {
            CHECK(!content->u.adjustment.u.color_lookup.tetrahedral); /* PSD reopens as trilinear */
        }
        pc_t_checks++;
        export_import_fini(&o);
        pc_document_fini(&d);
    }
    pc_blob_release(lut);
}

/* One document per blend mode: the mode on a pixel layer (with fill opacity and a mask), on a
 * clipped layer, on a group, inside a pass-through group, and on an adjustment layer. */
static pc_document blend_doc(pc_blend_mode mode)
{
    pc_document d = t_doc("blend", 24, 16, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    pc_layer l = t_raster("Background", fmt, pc_document_bounds(&d), 1, false);
    pc_layer *kids;
    pc_adjustment inv;
    t_push(&d, &l);
    l = t_raster("layer", fmt, pc_rect_make(2, 1, 20, 12), 2, true);
    l.blend = mode;
    l.fill_opacity = g255(150);
    l.opacity = g255(220);
    l.has_mask = true;
    l.mask.surface = t_mask_surface(PC_SAMPLE_TYPE_U8, pc_rect_make(3, 2, 15, 10), 1.0f, 4);
    l.mask.enabled = true;
    l.mask.linked = true;
    l.mask.density = 1.0f;
    l.mask.feather = 0.0f;
    t_push(&d, &l);
    l = t_raster("clipped", fmt, pc_rect_make(0, 0, 12, 16), 3, true);
    l.clipped = true;
    l.blend = mode;
    t_push(&d, &l);
    kids = xmalloc(sizeof *kids);
    kids[0] = t_raster("in pass-through", fmt, pc_rect_make(8, 4, 24, 16), 4, true);
    kids[0].blend = mode;
    must(pc_layer_group(&l, "pass-through", kids, 1, NULL));
    t_push(&d, &l);
    kids = xmalloc(sizeof *kids);
    kids[0] = t_raster("group child", fmt, pc_rect_make(4, 6, 18, 14), 5, true);
    must(pc_layer_group(&l, "group", kids, 1, NULL));
    l.blend = mode;
    l.opacity = g255(200);
    t_push(&d, &l);
    memset(&inv, 0, sizeof inv);
    inv.kind = PC_ADJ_INVERT;
    l = adj_layer("adjustment", &inv);
    l.blend = mode;
    l.opacity = g255(100);
    t_push(&d, &l);
    return d;
}

static void test_every_blend_mode_round_trips(void)
{
    for (size_t i = 0; i <= PC_BLEND_LAYER_MODE_COUNT; i++) {
        pc_blend_mode mode = i < PC_BLEND_LAYER_MODE_COUNT ? PC_BLEND_LAYER_MODES[i] : PC_BLEND_PASS_THROUGH;
        if (mode == PC_BLEND_PASS_THROUGH) {
            /* Pass-through is a group-only mode; blend_doc's "pass-through" group covers it, and a
             * pass-through group with an adjustment inside checks it affects the backdrop. */
            pc_document d = blend_doc(PC_BLEND_NORMAL);
            pc_pixel_format fmt = pc_document_pixel_format(&d);
            pc_layer *kids = xmalloc(2 * sizeof *kids);
            pc_adjustment inv;
            pc_layer g;
            export_import_out o;
            bool pass = false;
            memset(&inv, 0, sizeof inv);
            inv.kind = PC_ADJ_INVERT;
            kids[0] = t_raster("px", fmt, pc_rect_make(0, 0, 6, 6), 6, true);
            kids[1] = adj_layer("inside", &inv);
            must(pc_layer_group(&g, "pass-through adj", kids, 2, NULL));
            t_push(&d, &g);
            o = export_import(&d);
            for (size_t k = 0; o.file.has_layer_info && k < o.file.layer_info.n; k++) {
                pass = pass || memcmp(o.file.layer_info.layers[k].blend, "pass", 4) == 0;
            }
            CHECK(pass); /* pass key written */
            rt_docs_eq(&d, &o.back);
            composite_eq(&d, &o.back, "pass-through");
            export_import_fini(&o);
            pc_document_fini(&d);
            continue;
        }
        {
            pc_document d = blend_doc(mode);
            export_import_out o = export_import(&d);
            int before = pc_t_failed_checks;
            rt_docs_eq(&d, &o.back);
            composite_eq(&d, &o.back, pc_blend_label(mode));
            if (pc_t_failed_checks != before) {
                (void)fprintf(stderr, "  (blend mode %s)\n", pc_blend_label(mode));
            }
            export_import_fini(&o);
            pc_document_fini(&d);
        }
    }
}

int main(int argc, char **argv)
{
    RUN(test_channel_options_spot_and_quick_mask_roundtrip);
    RUN(test_lab16_chroma_uses_photoshops_scale);
    RUN(test_lab_round_trips_at_every_depth);
    RUN(test_undecodable_channel_leaves_the_layer_empty);
    RUN(test_flattened_psb_with_a_merged_image_over_2_gib_opens);
    RUN(test_layer_labels_keep_the_psd_indices);
    RUN(test_layer_labels_psd_roundtrip_all_colors_and_depths);
    RUN(test_layer_labels_truncated_sheet_color_is_not_a_label);
    RUN(test_several_strokes_and_shadows_roundtrip);
    RUN(test_joined_shape_components_roundtrip);
    RUN(test_every_adjustment_kind_round_trips_8bit);
    RUN(test_every_adjustment_kind_round_trips_16bit);
    RUN(test_color_lookup_tetrahedral_warns_on_psd_save);
    RUN(test_every_blend_mode_round_trips);
    return pc_t_finish();
}
