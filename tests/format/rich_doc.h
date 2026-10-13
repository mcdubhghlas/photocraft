/* Synthetic documents exercising every part of the model (port of
 * crates/format/tests/common/mod.rs), shared by the .pcraft document tests.
 *
 * Doc-type values the reference builds with constructors (effects, adjustments, paths, text
 * runs, ...) are given here as their serde JSON (doc/json.h reads them), so the documents are
 * the reference's value for value; test_parity2.c checks that the manifests they save to are
 * byte-identical to the reference's. Every builder reports failures through the test harness
 * and returns a usable (possibly partial) document. */
#ifndef PC_TESTS_FORMAT_RICH_DOC_H
#define PC_TESTS_FORMAT_RICH_DOC_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "doc/doc.h"
#include "doc/json.h"
#include "harness.h"

/* Parse `text` and read it as T into *out (zeroed first). */
#define T_JSON(T, text, out)                                                                                 \
    do {                                                                                                     \
        yyjson_doc *t_jd_ = NULL;                                                                            \
        memset((out), 0, sizeof *(out));                                                                     \
        if (pc_json_parse((text), strlen(text), &t_jd_, NULL) != PC_OK ||                                    \
            T##_from_json(yyjson_doc_get_root(t_jd_), (out), NULL) != PC_OK) {                               \
            PC_T_FAIL("cannot read %s from JSON", #T);                                                       \
        }                                                                                                    \
        yyjson_doc_free(t_jd_);                                                                              \
    } while (0)

/* Parse a JSON array of T into a malloc'd array (*ptr, *n). */
#define T_JSON_ARRAY(T, text, ptr, n)                                                                        \
    do {                                                                                                     \
        yyjson_doc *t_jd_ = NULL;                                                                            \
        *(ptr) = NULL;                                                                                       \
        *(n) = 0;                                                                                            \
        if (pc_json_parse((text), strlen(text), &t_jd_, NULL) == PC_OK) {                                    \
            yyjson_val *t_a_ = yyjson_doc_get_root(t_jd_);                                                   \
            size_t t_cnt_ = yyjson_arr_size(t_a_);                                                           \
            *(ptr) = t_cnt_ > 0 ? calloc(t_cnt_, sizeof **(ptr)) : NULL;                                     \
            for (size_t t_i_ = 0; t_i_ < t_cnt_ && *(ptr) != NULL; t_i_++) {                                 \
                if (T##_from_json(yyjson_arr_get(t_a_, t_i_), &(*(ptr))[t_i_], NULL) != PC_OK) {             \
                    PC_T_FAIL("cannot read %s[%zu] from JSON", #T, t_i_);                                    \
                    break;                                                                                   \
                }                                                                                            \
                *(n) = t_i_ + 1;                                                                             \
            }                                                                                                \
        } else {                                                                                             \
            PC_T_FAIL("bad JSON array for %s", #T);                                                          \
        }                                                                                                    \
        yyjson_doc_free(t_jd_);                                                                              \
    } while (0)

#define T_COLOR(c) "{\"mode\":\"Rgb\",\"c\":" c ",\"alpha\":1.0}"

static const char T_VECTOR_PATH[] =
    "{\"subpaths\":[{\"closed\":true,\"knots\":["
    "{\"anchor\":{\"x\":1.5,\"y\":2.25},\"in_ctrl\":{\"x\":1.5,\"y\":2.25},\"out_ctrl\":{\"x\":1.5,\"y\":2."
    "25},"
    "\"smooth\":false},"
    "{\"anchor\":{\"x\":40.0,\"y\":5.0},\"in_ctrl\":{\"x\":30.0,\"y\":0.0},\"out_ctrl\":{\"x\":50.0,\"y\":10."
    "0},"
    "\"smooth\":true},"
    "{\"anchor\":{\"x\":10.0,\"y\":60.0},\"in_ctrl\":{\"x\":10.0,\"y\":60.0},\"out_ctrl\":{\"x\":10.0,\"y\":"
    "60.0},"
    "\"smooth\":false}],\"op\":\"Combine\"},"
    "{\"closed\":true,\"knots\":[{\"anchor\":{\"x\":5.0,\"y\":5.0},\"in_ctrl\":{\"x\":5.0,\"y\":5.0},"
    "\"out_ctrl\":{\"x\":5.0,\"y\":5.0},\"smooth\":false},{\"anchor\":{\"x\":9.0,\"y\":5.0},"
    "\"in_ctrl\":{\"x\":9.0,\"y\":5.0},\"out_ctrl\":{\"x\":9.0,\"y\":5.0},\"smooth\":false},"
    "{\"anchor\":{\"x\":9.0,\"y\":9.0},\"in_ctrl\":{\"x\":9.0,\"y\":9.0},\"out_ctrl\":{\"x\":9.0,\"y\":9.0},"
    "\"smooth\":false}],\"op\":\"Subtract\"},"
    "{\"closed\":false,\"knots\":[{\"anchor\":{\"x\":0.0,\"y\":0.0},\"in_ctrl\":{\"x\":0.0,\"y\":0.0},"
    "\"out_ctrl\":{\"x\":0.0,\"y\":0.0},\"smooth\":false},{\"anchor\":{\"x\":3.0,\"y\":4.0},"
    "\"in_ctrl\":{\"x\":3.0,\"y\":4.0},\"out_ctrl\":{\"x\":3.0,\"y\":4.0},\"smooth\":false}],\"op\":"
    "\"Exclude\"},"
    "{\"closed\":true,\"knots\":[{\"anchor\":{\"x\":0.0,\"y\":0.0},\"in_ctrl\":{\"x\":0.0,\"y\":0.0},"
    "\"out_ctrl\":{\"x\":0.0,\"y\":0.0},\"smooth\":false},{\"anchor\":{\"x\":3.0,\"y\":4.0},"
    "\"in_ctrl\":{\"x\":3.0,\"y\":4.0},\"out_ctrl\":{\"x\":3.0,\"y\":4.0},\"smooth\":false},"
    "{\"anchor\":{\"x\":3.0,\"y\":0.0},\"in_ctrl\":{\"x\":3.0,\"y\":0.0},\"out_ctrl\":{\"x\":3.0,\"y\":0.0},"
    "\"smooth\":false}],\"op\":\"Intersect\"}],\"fill_rule\":\"NonZero\",\"inverted\":false}";

static const char T_PAINT_EFFECTS[] =
    "[{\"ColorOverlay\":{\"common\":{\"enabled\":true,\"blend\":\"Normal\",\"opacity\":0.8},"
    "\"color\":" T_COLOR(
        "[1.0,0.2,0.1,0.0]") "}},"
                             "{\"GradientOverlay\":{\"common\":{\"enabled\":true,\"blend\":\"Normal\","
                             "\"opacity\":0.8},"
                             "\"gradient\":{\"stops\":[[0.0," T_COLOR("[0.0,0.0,0.0,0.0]") "],[1.0," T_COLOR(
                                 "[1.0,1.0,1.0,0.0]") "]],"
                                                      "\"opacity_stops\":[],\"style\":\"Linear\",\"angle\":"
                                                      "90.0,\"scale\":1.0,\"reverse\":false,\"align\":true,"
                                                      "\"offset\":[0.0,0.0]},\"dither\":true}}]";

static const char T_HUE_SAT[] =
    "{\"HueSaturation\":{\"hue\":30.0,\"saturation\":-10.0,\"lightness\":5.0,\"colorize\":false,\"ranges\":["
    "{\"hue\":0.0,\"saturation\":0.0,\"lightness\":0.0,\"bounds\":[-45.0,-15.0,15.0,45.0]},"
    "{\"hue\":0.0,\"saturation\":0.0,\"lightness\":0.0,\"bounds\":[15.0,45.0,75.0,105.0]},"
    "{\"hue\":0.0,\"saturation\":0.0,\"lightness\":0.0,\"bounds\":[75.0,105.0,135.0,165.0]},"
    "{\"hue\":0.0,\"saturation\":0.0,\"lightness\":0.0,\"bounds\":[135.0,165.0,195.0,225.0]},"
    "{\"hue\":0.0,\"saturation\":0.0,\"lightness\":0.0,\"bounds\":[195.0,225.0,255.0,285.0]},"
    "{\"hue\":0.0,\"saturation\":0.0,\"lightness\":0.0,\"bounds\":[255.0,285.0,315.0,345.0]}]}}";

static const char T_SOLID_FILL[] = "{\"Solid\":{\"mode\":\"Rgb\",\"c\":[0.1,0.2,0.3,0.0],\"alpha\":0.9}}";

static const char T_GRADIENT_FILL[] =
    "{\"Gradient\":{\"stops\":[[0.0," T_COLOR("[0.0,0.0,0.0,0.0]") "],[1.0," T_COLOR(
        "[1.0,1.0,1.0,0.0]") "]],"
                             "\"angle\":45.0,\"scale\":1.5,\"style\":\"Linear\",\"reverse\":true,\"opacity_"
                             "stops\":[[0.0,1.0],[1.0,0.25]],"
                             "\"midpoints\":[0.3],\"offset\":[0.1,-0.2],\"dither\":true,\"align\":false}}";

#define T_CHAR_STYLE(kerning, kern)                                                                          \
    "{\"font_family\":\"\",\"font_style\":\"\",\"postscript_name\":null,\"weight\":400,\"italic\":false,"    \
    "\"size_pt\":12.0,\"color\":" T_COLOR(                                                                   \
        "[0.0,0.0,0.0,0.0]") ",\"tracking\":0.0,\"leading_pt\":null,"                                        \
                             "\"baseline_shift_pt\":0.0,\"horizontal_scale\":1.0,\"vertical_scale\":1.0,"    \
                             "\"underline\":false,"                                                          \
                             "\"strikethrough\":false,\"faux_bold\":false,\"faux_italic\":false,"            \
                             "\"kerning\":\"" kerning "\","                                                  \
                             "\"kern\":" kern                                                                \
                             ",\"caps\":\"Normal\",\"ligatures\":true,\"discretionary_ligatures\":false,"    \
                             "\"features\":[],\"variations\":[],\"language\":null}"

static const char T_TEXT_RUNS[] = "[{\"len\":1,\"style\":" T_CHAR_STYLE(
    "Off", "120.0") "},"
                    "{\"len\":5,\"style\":" T_CHAR_STYLE(
                        "Metrics", "0.0") "},"
                                          "{\"len\":9,\"style\":" T_CHAR_STYLE("Optical", "0.0") "}]";

static const char T_PARAGRAPHS[] =
    "[{\"len\":15,\"style\":{\"align\":\"Left\",\"first_line_indent_pt\":0.0,\"start_indent_pt\":0.0,"
    "\"end_indent_pt\":0.0,\"space_before_pt\":0.0,\"space_after_pt\":0.0,\"auto_leading\":1.2,"
    "\"direction\":\"Auto\",\"hyphenate\":false}}]";

static const char T_SHAPE_STROKE[] = "{\"width\":2.5,\"paint\":{\"Solid\":" T_COLOR(
    "[0.0,0.5,1.0,0.0]") "},\"opacity\":0.75,\"align\":\"Outside\","
                         "\"cap\":\"Round\",\"join\":\"Bevel\",\"miter_limit\":4.0,\"dashes\":[2.0,1.0],"
                         "\"dash_offset\":0.5}";

static const char T_SMART_FILTERS[] =
    "[{\"command\":\"filter.blur.gaussian\",\"params\":{\"radius\":2.5,\"nested\":{\"a\":[1,2,3]}},"
    "\"blend\":\"Screen\",\"opacity\":0.6,\"visible\":true}]";

static const char T_DUOTONE[] =
    "{\"inks\":[{\"name\":\"Black\",\"color\":[0.0,0.0,0.0],\"curve\":[{\"input\":0.0,\"output\":0.0},"
    "{\"input\":1.0,\"output\":1.0}]},{\"name\":\"PANTONE 151 C\",\"color\":[1.0,0.5,0.0],"
    "\"curve\":[{\"input\":0.0,\"output\":0.0},{\"input\":1.0,\"output\":1.0}]}],\"psd_raw\":[1,2,3]}";

/* ---- deterministic pixels and bytes ---- */

typedef struct t_rng {
    uint64_t s;
} t_rng;

static inline uint64_t t_rng_next(t_rng *r)
{
    uint64_t z;
    r->s += 0x9E3779B97F4A7C15ull;
    z = r->s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static inline float t_rng_f(t_rng *r)
{
    return (float)(t_rng_next(r) >> 40) / (float)(1u << 24);
}

/* Deterministic pixels at negative coordinates and in far tiles; float surfaces get HDR values. */
static inline void t_scribble(pc_surface *s, uint64_t seed, bool hdr)
{
    static const int32_t at[4][2] = {{-12, -7}, {0, 0}, {250, 250}, {600, 30}};
    t_rng r = {seed};
    size_t ch = pc_surface_channels(s);
    for (size_t k = 0; k < 4; k++) {
        for (int32_t dy = 0; dy < 9; dy++) {
            for (int32_t dx = 0; dx < 11; dx++) {
                float px[PC_RASTER_MAX_CHANNELS];
                for (size_t c = 0; c < ch; c++) {
                    px[c] = hdr ? t_rng_f(&r) * 4.0f - 1.0f : t_rng_f(&r);
                }
                CHECK(pc_surface_write_pixel(s, at[k][0] + dx, at[k][1] + dy, px, ch, NULL) == PC_OK);
            }
        }
    }
}

static inline pc_blob *t_blob(uint8_t seed, size_t n)
{
    pc_blob *b = NULL;
    CHECK(pc_blob_alloc(n, &b, NULL) == PC_OK);
    if (b != NULL) {
        uint8_t *p = pc_blob_data_mut(b);
        for (size_t i = 0; i < n; i++) {
            p[i] = (uint8_t)((uint8_t)i * 31u + seed);
        }
    }
    return b;
}

static inline void t_vector_path(pc_path *out)
{
    T_JSON(pc_path, T_VECTOR_PATH, out);
}

static inline void t_str(pc_str *s, const char *c)
{
    CHECK(pc_str_set_c(s, c, NULL) == PC_OK);
}

/* A layer with the given content (moved). */
static inline void t_layer(pc_layer *out, const char *name, pc_layer_content *c)
{
    if (pc_layer_init(out, name, c, NULL) != PC_OK) {
        PC_T_FAIL("pc_layer_init %s", name);
        memset(out, 0, sizeof *out);
        pc_layer_content_fini(c);
    }
}

/* Appends *l (moved) to the document's root layers. */
static inline void t_push_layer(pc_document *d, pc_layer *l)
{
    pc_layer *g = realloc(d->layers, (d->nlayers + 1) * sizeof *g);
    if (g == NULL) {
        PC_T_FAIL("out of memory");
        pc_layer_fini(l);
        return;
    }
    d->layers = g;
    d->layers[d->nlayers++] = *l;
    memset(l, 0, sizeof *l);
}

static inline void t_surface_clone(const pc_surface *s, pc_surface *out)
{
    CHECK(pc_surface_clone(s, out, NULL) == PC_OK);
}

/* ---- rich_doc ---- */

static inline void t_rich_doc(pc_color_mode mode, pc_sample_type depth, pc_document *d)
{
    bool hdr = depth == PC_SAMPLE_TYPE_F32;
    pc_size size = {300, 280};
    pc_pixel_format pf;
    pc_layer paint, inner, nested, adj, group, fill, grad, text, shape, smart, linked;
    pc_layer_content c;
    pc_surface cache;
    pc_surface ch;
    pc_layer *kids;
    pc_pixel_format chf = PC_PIXEL_FORMAT_GRAY8;

    if (pc_document_with_background(d, "Rich", size, mode, depth, PC_COLOR_WHITE, NULL) != PC_OK) {
        PC_T_FAIL("pc_document_with_background");
        memset(d, 0, sizeof *d);
        return;
    }
    d->resolution_dpi = 300.0f;
    d->icc_profile = t_blob(1, 500);
    pf = pc_document_pixel_format(d);

    CHECK(pc_layer_raster(&paint, "Paint", pf, NULL) == PC_OK);
    t_scribble(pc_layer_surface_mut(&paint), 1, hdr);
    paint.blend = PC_BLEND_MULTIPLY;
    paint.opacity = 0.75f;
    paint.fill_opacity = 0.5f;
    paint.clipped = true;
    paint.label = PC_LABEL_VIOLET;
    paint.locks.pixels = true;
    pc_layer_mask_hide_all(&paint.mask);
    paint.has_mask = true;
    t_scribble(&paint.mask.surface, 2, false);
    paint.mask.density = 0.8f;
    paint.mask.feather = 3.5f;
    paint.mask.linked = false;
    T_JSON_ARRAY(pc_effect, T_PAINT_EFFECTS, &paint.effects.items, &paint.effects.nitems);
    paint.effects.psd_raw = t_blob(3, 64);
    paint.psd_blocks = calloc(2, sizeof *paint.psd_blocks);
    if (paint.psd_blocks != NULL) {
        memcpy(paint.psd_blocks[0].key, "vmsk", 4);
        paint.psd_blocks[0].data = t_blob(4, 40);
        memcpy(paint.psd_blocks[1].key, "lnsr", 4);
        paint.psd_blocks[1].data = t_blob(5, 4);
        paint.npsd_blocks = 2;
    }
    paint.has_psd_id = true;
    paint.psd_id = 42;

    CHECK(pc_layer_raster(&inner, "Inner", pf, NULL) == PC_OK);
    t_scribble(pc_layer_surface_mut(&inner), 6, hdr);
    kids = calloc(1, sizeof *kids);
    if (kids != NULL) {
        kids[0] = inner;
    }
    CHECK(pc_layer_group(&nested, "Nested", kids, 1, NULL) == PC_OK);
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_ADJUSTMENT;
    T_JSON(pc_adjustment, T_HUE_SAT, &c.u.adjustment);
    t_layer(&adj, "Hue/Sat", &c);
    adj.visible = false;
    kids = calloc(2, sizeof *kids);
    if (kids != NULL) {
        kids[0] = nested;
        kids[1] = adj;
    }
    CHECK(pc_layer_group(&group, "Group", kids, 2, NULL) == PC_OK);
    group.content.u.group.expanded = false;

    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_FILL;
    T_JSON(pc_fill, T_SOLID_FILL, &c.u.fill);
    t_layer(&fill, "Fill", &c);
    fill.has_fill_cache = true;
    T_JSON(pc_fill, T_SOLID_FILL, &fill.fill_cache.fill);
    pc_surface_init(&fill.fill_cache.surface, pf);
    t_scribble(&fill.fill_cache.surface, 7, false);
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_FILL;
    T_JSON(pc_fill, T_GRADIENT_FILL, &c.u.fill);
    t_layer(&grad, "Gradient", &c);

    pc_surface_init(&cache, pf);
    t_scribble(&cache, 8, false);
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_TEXT;
    pc_text_layer_default(&c.u.text);
    t_str(&c.u.text.text, "Hello \xe2\x9c\x93 world");
    t_str(&c.u.text.font_family, "Inter");
    c.u.text.size_pt = 24.0f;
    c.u.text.color = pc_color_rgb(0.2f, 0.4f, 0.6f);
    c.u.text.transform = pc_affine_translate(10.5, 20.25);
    c.u.text.has_cache = true;
    t_surface_clone(&cache, &c.u.text.cache);
    c.u.text.psd_raw = t_blob(9, 100);
    T_JSON_ARRAY(pc_text_run, T_TEXT_RUNS, &c.u.text.runs, &c.u.text.nruns);
    T_JSON_ARRAY(pc_paragraph_run, T_PARAGRAPHS, &c.u.text.paragraphs, &c.u.text.nparagraphs);
    c.u.text.shape.is_box = true;
    c.u.text.shape.x = 1.0f;
    c.u.text.shape.y = 2.0f;
    c.u.text.shape.width = 100.0f;
    c.u.text.shape.height = 50.0f;
    c.u.text.orientation = PC_ORIENT_VERTICAL;
    c.u.text.antialias = PC_AA_CRISP;
    c.u.text.has_warp = true;
    T_JSON(pc_text_warp,
           "{\"style\":\"warpArc\",\"value\":25.0,\"horizontal_distortion\":0.0,\"vertical_distortion\":0.0,"
           "\"horizontal\":false}",
           &c.u.text.warp);
    t_layer(&text, "Type", &c);

    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SHAPE;
    t_vector_path(&c.u.shape.path);
    c.u.shape.has_fill = true;
    c.u.shape.fill.kind = PC_FILL_SOLID;
    T_JSON(pc_fill, "{\"Solid\":" T_COLOR("[0.0,0.0,0.0,0.0]") "}", &c.u.shape.fill);
    c.u.shape.has_stroke = true;
    T_JSON(pc_shape_stroke, T_SHAPE_STROKE, &c.u.shape.stroke);
    c.u.shape.has_live = true;
    T_JSON(pc_live_shape, "{\"kind\":\"rect\",\"rect\":[1.0,2.0,30.0,40.0],\"radii\":[4.0,0.0,4.0,0.0]}",
           &c.u.shape.live);
    c.u.shape.has_cache = true;
    t_surface_clone(&cache, &c.u.shape.cache);
    c.u.shape.psd_raw = t_blob(10, 30);
    t_layer(&shape, "Shape", &c);
    shape.label = PC_LABEL_GREEN;

    {
        pc_path vp;
        t_vector_path(&vp);
        pc_vector_mask_init(&paint.vector_mask, &vp);
        paint.has_vector_mask = true;
        paint.vector_mask.enabled = false;
        paint.vector_mask.density = 0.4f;
        paint.vector_mask.feather = 3.0f;
        paint.vector_mask.path.inverted = true;
    }

    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SMART;
    {
        pc_smart_object *s = &c.u.smart;
        static const double bounds[4] = {0.0, 0.0, 48.0, 24.0};
        static const double persp[9] = {1.0, 0.1, 2.0, 0.05, 1.0, 3.0, 0.001, 0.0005, 1.0};
        float one = 1.0f;
        pc_mesh mesh;
        s->contents_id = pc_smart_contents_id_fresh();
        t_str(&s->source.file_name, "inner.png");
        s->source.bytes = t_blob(11, 2000);
        s->transform = pc_affine_scale(0.5);
        T_JSON_ARRAY(pc_smart_filter, T_SMART_FILTERS, &s->smart_filters, &s->nsmart_filters);
        s->has_cache = true;
        s->cache = cache; /* moved */
        s->filters_enabled = false;
        s->has_filter_mask = true;
        pc_surface_init_default(&s->filter_mask.surface, PC_PIXEL_FORMAT_GRAY8, &one, 1);
        s->filter_mask.enabled = true;
        s->filter_mask.linked = false;
        s->filter_mask.density = 0.75f;
        s->filter_mask.feather = 0.0f;
        CHECK(pc_mesh_identity(bounds, 2, 1, &mesh, NULL) == PC_OK);
        pc_warp_custom(&s->warp, &mesh, bounds);
        s->has_warp = true;
        s->has_stack_mode = true;
        T_JSON(pc_stack_mode, "\"Median\"", &s->stack_mode);
        s->has_perspective = true;
        memcpy(s->perspective, persp, sizeof persp);
    }
    t_layer(&smart, "Smart", &c);
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SMART;
    c.u.smart.contents_id = pc_smart_contents_id_fresh();
    c.u.smart.source.linked = true;
    t_str(&c.u.smart.source.path, "/tmp/linked.psd");
    c.u.smart.transform = PC_AFFINE_IDENTITY;
    c.u.smart.psd_raw = t_blob(12, 8);
    c.u.smart.filters_enabled = true;
    t_layer(&linked, "Linked", &c);

    t_push_layer(d, &paint);
    t_push_layer(d, &group);
    t_push_layer(d, &fill);
    t_push_layer(d, &grad);
    t_push_layer(d, &text);
    t_push_layer(d, &shape);
    t_push_layer(d, &smart);
    t_push_layer(d, &linked);

    chf.sample = depth;
    pc_surface_init(&ch, chf);
    t_scribble(&ch, 13, false);
    d->channels = calloc(2, sizeof *d->channels);
    if (d->channels != NULL) {
        pc_surface s1, s2;
        t_surface_clone(&ch, &s1);
        t_surface_clone(&ch, &s2);
        CHECK(pc_alpha_channel_init(&d->channels[0], "Alpha 1", &s1, NULL) == PC_OK);
        d->channels[0].color = pc_color_rgb(0.0f, 0.5f, 1.0f);
        d->channels[0].opacity = 0.35f;
        d->channels[0].indicates = PC_INDICATES_SELECTED_AREAS;
        CHECK(pc_alpha_channel_init(&d->channels[1], "Spot", &s2, NULL) == PC_OK);
        d->channels[1].has_spot = true;
        d->channels[1].spot_color = pc_color_rgb(1.0f, 0.0f, 0.5f);
        d->channels[1].spot_solidity = 0.7f;
        d->nchannels = 2;
    }
    CHECK(pc_alpha_channel_init(&d->quick_mask, "Quick Mask", &ch, NULL) == PC_OK);
    d->has_quick_mask = true;
    {
        static const char PNAME[] = "$$$/Patterns/Test=Scribble";
        pc_pixel_format pat_f = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true};
        pc_surface pat;
        pc_surface_init(&pat, pat_f);
        t_scribble(&pat, 21, false);
        d->patterns = calloc(1, sizeof *d->patterns);
        if (d->patterns != NULL &&
            pc_pattern_new(PNAME, sizeof PNAME - 1, &pat, 32, 24, &d->patterns[0], NULL) == PC_OK) {
            d->npatterns = 1;
        } else {
            PC_T_FAIL("pc_pattern_new");
            pc_surface_fini(&pat);
        }
    }
    d->has_color_table = true;
    T_JSON(pc_color_table, "{\"colors\":[[0,0,0],[255,128,0]],\"transparent\":1}", &d->color_table);
    d->has_duotone = true;
    T_JSON(pc_duotone, T_DUOTONE, &d->duotone);
    pc_guides_fini(&d->guides);
    T_JSON(pc_guides, "{\"horizontal\":[10.0,20.5],\"vertical\":[100.25]}", &d->guides);
    pc_surface_init(&d->selection, PC_PIXEL_FORMAT_GRAY8);
    t_scribble(&d->selection, 14, false);
    d->has_selection = true;
    d->metadata.has_xmp = true;
    t_str(&d->metadata.xmp, "<x:xmpmeta/>");
    d->metadata.exif = t_blob(15, 64);
    d->metadata.psd_resources = calloc(1, sizeof *d->metadata.psd_resources);
    if (d->metadata.psd_resources != NULL) {
        d->metadata.psd_resources[0].id = 1036;
        t_str(&d->metadata.psd_resources[0].name, "thumb");
        d->metadata.psd_resources[0].data = t_blob(16, 20);
        d->metadata.npsd_resources = 1;
    }
    d->metadata.psd_global_blocks = calloc(1, sizeof *d->metadata.psd_global_blocks);
    if (d->metadata.psd_global_blocks != NULL) {
        memcpy(d->metadata.psd_global_blocks[0].signature, "8BIM", 4);
        memcpy(d->metadata.psd_global_blocks[0].key, "Patt", 4);
        d->metadata.psd_global_blocks[0].data = t_blob(17, 50);
        d->metadata.npsd_global_blocks = 1;
    }
    d->global_light.angle = 120.0f;
    d->global_light.altitude = 30.0f;
    d->paths = calloc(2, sizeof *d->paths);
    if (d->paths != NULL) {
        t_str(&d->paths[0].name, "Path 1");
        t_vector_path(&d->paths[0].path);
        d->paths[0].psd_raw = t_blob(18, 52);
        t_str(&d->paths[1].name, "Empty");
        d->npaths = 2;
    }
    t_vector_path(&d->work_path);
    d->work_path.fill_rule = PC_FILL_RULE_EVENODD;
    d->has_work_path = true;
    t_str(&d->clipping_path.name, "Path 1");
    d->clipping_path.flatness = 2.0f;
    d->has_clipping_path = true;
}

/* ---- ids and nesting ---- */

/* Deepest group nesting (0 without groups). */
static inline size_t t_group_depth(const pc_layer *layers, size_t n)
{
    size_t best = 0;
    for (size_t i = 0; i < n; i++) {
        if (layers[i].content.kind == PC_LAYER_GROUP) {
            size_t d =
                1 + t_group_depth(layers[i].content.u.group.children, layers[i].content.u.group.nchildren);
            if (d > best) {
                best = d;
            }
        }
    }
    return best;
}

/* t_rich_doc with its layers wrapped in groups until they are nested `levels` deep. */
static inline void t_nested_doc(size_t levels, pc_sample_type depth, pc_document *d)
{
    t_rich_doc(PC_COLOR_MODE_RGB, depth, d);
    for (size_t i = t_group_depth(d->layers, d->nlayers); i < levels; i++) {
        char name[32];
        pc_layer g;
        (void)snprintf(name, sizeof name, "Level %zu", i);
        if (pc_layer_group(&g, name, d->layers, d->nlayers, NULL) != PC_OK) {
            PC_T_FAIL("pc_layer_group");
            return;
        }
        d->layers = calloc(1, sizeof *d->layers);
        if (d->layers == NULL) {
            pc_layer_fini(&g);
            d->nlayers = 0;
            return;
        }
        d->layers[0] = g;
        d->nlayers = 1;
    }
}

static inline void t_renumber_layers(pc_layer *layers, size_t n, uint64_t *next)
{
    for (size_t i = 0; i < n; i++) {
        layers[i].id = (*next)++;
        if (layers[i].content.kind == PC_LAYER_GROUP) {
            t_renumber_layers(layers[i].content.u.group.children, layers[i].content.u.group.nchildren, next);
        }
    }
}

/* Smart-object contents ids by first appearance in pre-order: 5000, 5001, ..., shared ids staying
 * shared (map: old ids seen so far, *n of them, at most cap). Fresh ids depend on how many the
 * process minted before, so the parity vectors normalise them (format2_vectors.rs
 * renumber_contents). */
static inline void t_renumber_contents_in(pc_layer *layers, size_t n, uint64_t *map, size_t *nmap, size_t cap)
{
    for (size_t i = 0; i < n; i++) {
        pc_layer_content *c = &layers[i].content;
        if (c->kind == PC_LAYER_GROUP) {
            t_renumber_contents_in(c->u.group.children, c->u.group.nchildren, map, nmap, cap);
        } else if (c->kind == PC_LAYER_SMART) {
            size_t k = 0;
            while (k < *nmap && map[k] != c->u.smart.contents_id) {
                k++;
            }
            if (k == *nmap && *nmap < cap) {
                map[(*nmap)++] = c->u.smart.contents_id;
            }
            c->u.smart.contents_id = 5000 + (uint64_t)k;
        }
    }
}

static inline void t_renumber_contents(pc_document *d)
{
    uint64_t map[256];
    size_t n = 0;
    t_renumber_contents_in(d->layers, d->nlayers, map, &n, sizeof map / sizeof map[0]);
}

/* The parity vectors' renumbering: document 7, layers 100, 101, ... in pre-order; contents ids
 * as t_renumber_contents. */
static inline void t_renumber(pc_document *d)
{
    uint64_t next = 100;
    d->id = 7;
    t_renumber_layers(d->layers, d->nlayers, &next);
    t_renumber_contents(d);
}

#endif
