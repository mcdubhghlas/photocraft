/* The documents `rustref format2-vectors` saves (tools/rustref/src/format2_vectors.rs docs()
 * and bad_docs()), built the same way in C: t_rich_doc and friends from rich_doc.h, and the
 * shared JSON literals below (copied verbatim from the Rust module). */
#ifndef PC_TESTS_FORMAT_PARITY_DOCS_H
#define PC_TESTS_FORMAT_PARITY_DOCS_H

#include <math.h>
#include <stdio.h>

#include "raster/pixel.h"
#include "rich_doc.h"

static const char J_VARIABLES[] =
    "{\"defs\":[{\"name\":\"show\",\"layer\":101,\"kind\":{\"type\":\"visibility\"}},{\"name\":\"titl"
    "e\",\"layer\":104,\"kind\":{\"type\":\"textReplacement\"}},{\"name\":\"photo\",\"layer\":100,\"k"
    "ind\":{\"type\":\"pixelReplacement\",\"method\":\"fit\",\"align\":\"center\",\"clip\":false}}],"
    "\"data_sets\":[{\"name\":\"Set 1\",\"values\":[{\"variable\":\"show\",\"kind\":\"visibility\",\""
    "value\":true},{\"variable\":\"title\",\"kind\":\"text\",\"value\":\"Hi\"}]}],\"active\":0}";
static const char J_TIMELINE[] =
    "{\"fps\":25.0,\"duration\":48,\"current\":3,\"work_start\":1,\"work_end\":40}";
static const char J_MEASUREMENT[] =
    "{\"scale\":{\"pixel_length\":72.0,\"logical_length\":1.0,\"units\":\"inches\"},\"count_groups\":"
    "[{\"name\":\"Cells\",\"color\":{\"mode\":\"Rgb\",\"c\":[0.0,0.5,1.0,0.0],\"alpha\":1.0},\"marker"
    "_size\":3,\"label_size\":12,\"visible\":true,\"points\":[[1.0,2.0],[5.5,6.5]]}],\"active_count_g"
    "roup\":0,\"ruler\":{\"start\":[1.0,1.0],\"end\":[30.0,20.0],\"protractor\":[1.0,25.0]}}";
static const char J_NOTES[] =
    "[{\"author\":\"A\",\"color\":{\"mode\":\"Rgb\",\"c\":[1.0,1.0,0.51,0.0],\"alpha\":1.0},\"text\":"
    "\"Fix the sky\\nplease\",\"position\":[10.0,12.0],\"popup\":[10.0,30.0,200.0,150.0],\"open\":tru"
    "e,\"modified\":\"\"}]";
static const char J_TEXT_STYLES[] =
    "{\"character\":[{\"id\":1,\"name\":\"Big\",\"attrs\":{\"size_pt\":40.0,\"underline\":true}}],\"p"
    "aragraph\":[{\"id\":7,\"name\":\"Head\",\"para_attrs\":{\"align\":\"Center\"},\"char_attrs\":{\""
    "size_pt\":20.0,\"weight\":700}}],\"basic\":{\"id\":0,\"name\":\"Basic Paragraph\",\"para_attrs\""
    ":{},\"char_attrs\":{}},\"psd_raw\":{\"k\":[1,2]}}";
static const char J_SLICES[] =
    "{\"groupName\":\"Rich\",\"list\":[{\"id\":1,\"groupId\":0,\"origin\":\"user\",\"layer\":null,\"n"
    "ame\":\"Rich_01\",\"rect\":{\"x0\":0,\"y0\":0,\"x1\":10,\"y1\":10},\"kind\":\"image\",\"url\":\""
    "\",\"target\":\"\",\"message\":\"\",\"alt\":\"\",\"cellTextIsHtml\":false,\"cellText\":\"\",\"ho"
    "rizontalAlign\":0,\"verticalAlign\":0,\"background\":null,\"outsets\":[0,0,0,0]},{\"id\":2,\"gro"
    "upId\":0,\"origin\":\"layer\",\"layer\":101,\"name\":\"Rich_02\",\"rect\":{\"x0\":5,\"y0\":5,\"x"
    "1\":20,\"y1\":30},\"kind\":\"image\",\"url\":\"u\",\"target\":\"\",\"message\":\"\",\"alt\":\"\""
    ",\"cellTextIsHtml\":false,\"cellText\":\"\",\"horizontalAlign\":0,\"verticalAlign\":0,\"backgrou"
    "nd\":[255,1,2,3],\"outsets\":[1,2,3,4]},{\"id\":3,\"groupId\":0,\"origin\":\"layer\",\"layer\":9"
    "99,\"name\":\"Dangling\",\"rect\":{\"x0\":1,\"y0\":2,\"x1\":3,\"y1\":4},\"kind\":\"noImage\",\"u"
    "rl\":\"\",\"target\":\"\",\"message\":\"\",\"alt\":\"\",\"cellTextIsHtml\":false,\"cellText\":\""
    "\",\"horizontalAlign\":0,\"verticalAlign\":0,\"background\":null,\"outsets\":[0,0,0,0]}]}";
static const char J_ARTBOARD[] =
    "{\"rect\":{\"x0\":-1,\"y0\":-2,\"x1\":300,\"y1\":400},\"background\":{\"custom\":{\"mode\":\"Rgb"
    "\",\"c\":[0.1,0.2,0.3,0.0],\"alpha\":0.5}},\"preset\":\"iPhone 8/7/6\"}";
static const char J_BEVEL[] =
    "{\"BevelEmboss\":{\"enabled\":true,\"style\":\"Emboss\",\"technique\":\"ChiselHard\",\"depth\":1"
    ".2,\"up\":false,\"size\":9.0,\"soften\":1.0,\"angle\":30.0,\"altitude\":40.0,\"use_global_light"
    "\":false,\"gloss_contour\":\"Linear\",\"highlight\":{\"enabled\":true,\"blend\":\"Screen\",\"opa"
    "city\":0.7},\"highlight_color\":{\"mode\":\"Rgb\",\"c\":[1.0,1.0,1.0,0.0],\"alpha\":1.0},\"shado"
    "w\":{\"enabled\":true,\"blend\":\"Multiply\",\"opacity\":0.6},\"shadow_color\":{\"mode\":\"Rgb\""
    ",\"c\":[0.0,0.0,0.0,0.0],\"alpha\":1.0},\"contour\":{\"contour\":\"Linear\",\"range\":0.3,\"anti"
    "_alias\":true},\"texture\":{\"name\":\"p\",\"id\":\"i\",\"scale\":0.5,\"depth\":-1.5,\"invert\":"
    "true,\"link\":false,\"phase\":[1.0,2.0]}}}";
static const char J_BLEND_IF[] =
    "{\"ranges\":[[{\"black\":[20,60],\"white\":[255,255]},{\"black\":[0,0],\"white\":[255,255]}],[{"
    "\"black\":[0,0],\"white\":[255,255]},{\"black\":[0,0],\"white\":[255,255]}],[{\"black\":[0,0],\""
    "white\":[255,255]},{\"black\":[0,0],\"white\":[255,255]}],[{\"black\":[0,0],\"white\":[255,255]}"
    ",{\"black\":[0,0],\"white\":[180,220]}]]}";
static const char J_ADVANCED[] =
    "{\"knockout\":\"deep\",\"blendInterior\":true,\"blendClipped\":false,\"transparencyShapes\":fals"
    "e,\"layerMaskHidesEffects\":true,\"vectorMaskHidesEffects\":true}";
static const char J_LIVE_SHAPES[] =
    "[{\"kind\":\"ellipse\",\"rect\":[10.0,10.0,20.0,30.0]},{\"kind\":\"polygon\",\"rect\":[0.0,0.0,8"
    ".0,8.0],\"sides\":5,\"star_ratio\":0.5},{\"kind\":\"line\",\"from\":[0.0,0.0],\"to\":[-1.0,1e-7]"
    ",\"weight\":2.0}]";
static const char J_ADJUSTMENTS[] =
    "[{\"Exposure\":{\"exposure\":0.5,\"offset\":0.01,\"gamma\":1.2}},{\"Invert\":null},{\"Posterize"
    "\":{\"levels\":4}},{\"Threshold\":{\"level\":128}}]";

typedef struct named_doc {
    char name[32];
    pc_document doc;
} named_doc;

static inline named_doc *nd_add(named_doc *v, size_t *n, const char *name)
{
    named_doc *d = &v[(*n)++];
    (void)snprintf(d->name, sizeof d->name, "%s", name);
    return d;
}

static inline void pd_effects_fx(pc_effects *fx)
{
    memset(fx, 0, sizeof *fx);
    T_JSON_ARRAY(pc_effect,
                 "[{\"ColorOverlay\":{\"common\":{\"enabled\":true,\"blend\":\"Normal\",\"opacity\":0.8},"
                 "\"color\":" T_COLOR("[1.0,0.2,0.1,0.0]") "}}]",
                 &fx->items, &fx->nitems);
    fx->psd_raw = t_blob(30, 9);
    fx->has_reference = true;
    fx->reference[0] = 1.5;
    fx->reference[1] = 2.5;
}

static inline void pd_extras(pc_document *d)
{
    pc_layer_comp *c;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, d);
    t_renumber(d);
    d->layer_comps = calloc(2, sizeof *d->layer_comps);
    if (d->layer_comps == NULL) {
        return;
    }
    d->nlayer_comps = 2;
    c = &d->layer_comps[0];
    c->id = 1;
    t_str(&c->name, "Comp A");
    t_str(&c->comment, "first");
    c->apply_visibility = c->apply_position = c->apply_appearance = true;
    c->states = calloc(3, sizeof *c->states);
    if (c->states != NULL) {
        c->nstates = 3;
        c->states[0].layer = 100;
        c->states[0].has_visible = true;
        c->states[0].visible = false;
        c->states[0].has_position = true;
        c->states[0].position[0] = 3;
        c->states[0].position[1] = -4;
        c->states[0].has_appearance = true;
        c->states[0].appearance.blend = PC_BLEND_SCREEN;
        c->states[0].appearance.opacity = 0.5f;
        c->states[0].appearance.fill_opacity = 0.25f;
        pd_effects_fx(&c->states[0].appearance.effects);
        c->states[1].layer = 104;
        c->states[1].has_visible = true;
        c->states[1].visible = true;
        c->states[2].layer = 5000;
        c->states[2].has_position = true;
    }
    c = &d->layer_comps[1];
    c->id = 2;
    t_str(&c->name, "B");
    c->apply_position = true;
    d->has_last_applied_comp = true;
    d->last_applied_comp = 2;
    c = &d->last_document_state;
    d->has_last_document_state = true;
    t_str(&c->name, "Last Document State");
    c->apply_visibility = c->apply_position = c->apply_appearance = true;
    c->states = calloc(1, sizeof *c->states);
    if (c->states != NULL) {
        c->nstates = 1;
        c->states[0].layer = 102;
        c->states[0].has_visible = true;
        c->states[0].visible = true;
        c->states[0].has_position = true;
        c->states[0].position[0] = 1;
        c->states[0].position[1] = 2;
    }
    pc_variables_fini(&d->variables);
    T_JSON(pc_variables, J_VARIABLES, &d->variables);
    d->has_timeline = true;
    T_JSON(pc_timeline, J_TIMELINE, &d->timeline);
    pc_measurement_fini(&d->measurement);
    T_JSON(pc_measurement, J_MEASUREMENT, &d->measurement);
    T_JSON_ARRAY(pc_note, J_NOTES, &d->notes, &d->nnotes);
    pc_text_styles_fini(&d->text_styles);
    T_JSON(pc_text_styles, J_TEXT_STYLES, &d->text_styles);
    pc_slices_fini(&d->slices);
    T_JSON(pc_slices, J_SLICES, &d->slices);
    d->layers[2].content.u.group.has_artboard = true;
    T_JSON(pc_artboard, J_ARTBOARD, &d->layers[2].content.u.group.artboard);
}

static inline void pd_blending(pc_document *d)
{
    pc_layer *l0;
    pc_effect *g;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, d);
    l0 = &d->layers[0];
    l0->excluded_channels = 5;
    g = realloc(l0->effects.items, (l0->effects.nitems + 1) * sizeof *g);
    if (g != NULL) {
        l0->effects.items = g;
        T_JSON(pc_effect, J_BEVEL, &g[l0->effects.nitems]);
        l0->effects.nitems++;
    }
    l0->effects.has_reference = true;
    l0->effects.reference[0] = -3.25;
    l0->effects.reference[1] = 7.0;
    pc_blend_if_fini(&l0->blend_if);
    T_JSON(pc_blend_if, J_BLEND_IF, &l0->blend_if);
    T_JSON(pc_advanced_blending, J_ADVANCED, &l0->advanced);
    d->layers[1].advanced.knockout = PC_KNOCKOUT_SHALLOW;
    d->layers[1].has_link_group = true;
    d->layers[1].link_group = 3;
    d->layers[2].has_link_group = true;
    d->layers[2].link_group = 3;
}

static inline void pd_video(pc_document *d)
{
    pc_size size = {8, 6};
    pc_pixel_format fmt;
    pc_layer l;
    pc_surface *frames;
    CHECK(pc_document_init(d, "Vid", size, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    d->has_timeline = true;
    d->timeline = pc_timeline_new(3, 24.0f);
    fmt = pc_document_pixel_format(d);
    CHECK(pc_layer_raster(&l, "Clip", fmt, NULL) == PC_OK);
    frames = calloc(3, sizeof *frames);
    for (uint32_t i = 0; frames != NULL && i < 3; i++) {
        float v = (float)i / 2.0f;
        float rgba[4] = {v, 0.0f, 1.0f - v, 1.0f};
        float px[PC_RASTER_MAX_CHANNELS];
        size_t n = pc_from_rgba(fmt, rgba, px);
        pc_rect r = {0, 0, 8, 6};
        pc_surface_init(&frames[i], fmt);
        CHECK(pc_surface_fill_rect(&frames[i], r, px, n, NULL) == PC_OK);
    }
    pc_video_data_init(&l.video, frames, frames != NULL ? 3 : 0, 24.0f);
    l.has_video = true;
    l.video.source.is_file = true;
    t_str(&l.video.source.path, "/tmp/seq");
    l.video.show_altered = false;
    t_push_layer(d, &l);
}

static inline void pd_labels(pc_sample_type depth, pc_document *d)
{
    pc_size size = {2, 2};
    CHECK(pc_document_init(d, "Labels", size, PC_COLOR_MODE_RGB, depth, NULL) == PC_OK);
    for (size_t i = 0; i < PC_LABEL_COLOR_COUNT; i++) {
        pc_label_color color = PC_LABEL_COLOR_ALL[i];
        pc_layer *kids = calloc(1, sizeof *kids);
        pc_layer g;
        if (kids == NULL) {
            return;
        }
        CHECK(pc_layer_raster(&kids[0], "Child", pc_document_pixel_format(d), NULL) == PC_OK);
        kids[0].label = color;
        CHECK(pc_layer_group(&g, pc_label_color_label(color), kids, 1, NULL) == PC_OK);
        g.label = color;
        t_push_layer(d, &g);
    }
}

static inline void pd_shapes(pc_document *d)
{
    pc_size size = {64, 64};
    pc_live_shape *lives = NULL;
    pc_adjustment *adjs = NULL;
    size_t nl = 0;
    size_t na = 0;
    pc_layer_content c;
    pc_layer l;
    CHECK(pc_document_init(d, "Shapes", size, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    T_JSON_ARRAY(pc_live_shape, J_LIVE_SHAPES, &lives, &nl);
    for (size_t i = 0; i < nl; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "S%u", (unsigned)i);
        memset(&c, 0, sizeof c);
        c.kind = PC_LAYER_SHAPE;
        t_vector_path(&c.u.shape.path);
        c.u.shape.has_live = true;
        c.u.shape.live = lives[i];
        t_layer(&l, name, &c);
        t_push_layer(d, &l);
    }
    free(lives);
    T_JSON_ARRAY(pc_adjustment, J_ADJUSTMENTS, &adjs, &na);
    for (size_t i = 0; i < na; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "A%u", (unsigned)i);
        memset(&c, 0, sizeof c);
        c.kind = PC_LAYER_ADJUSTMENT;
        c.u.adjustment = adjs[i];
        t_layer(&l, name, &c);
        t_push_layer(d, &l);
    }
    free(adjs);
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SMART;
    {
        static const double bounds[4] = {0.0, 0.0, 10.0, 5.0};
        c.u.smart.transform = PC_AFFINE_IDENTITY;
        c.u.smart.filters_enabled = true;
        c.u.smart.has_warp = true;
        pc_warp_preset(&c.u.smart.warp, PC_WARP_ARC_LOWER, 33.5, bounds);
        c.u.smart.warp.h_distort = -12.0;
        c.u.smart.warp.v_distort = 4.0;
        c.u.smart.warp.vertical = true;
    }
    t_layer(&l, "Preset warp", &c);
    t_push_layer(d, &l);
    d->channels = calloc(1, sizeof *d->channels);
    if (d->channels != NULL) {
        float half = 0.5f;
        pc_surface s;
        pc_surface_init_default(&s, PC_PIXEL_FORMAT_GRAY8, &half, 1);
        CHECK(pc_alpha_channel_init(&d->channels[0], "Empty", &s, NULL) == PC_OK);
        d->nchannels = 1;
    }
}

static inline void pd_exhausted(pc_document *d)
{
    pc_size size = {2, 1};
    pc_layer_content c;
    pc_layer l;
    CHECK(pc_document_init(d, "exhausted", size, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    d->slices.list = calloc(1, sizeof *d->slices.list);
    if (d->slices.list != NULL) {
        pc_slice_default(&d->slices.list[0]);
        d->slices.list[0].id = UINT32_MAX;
        d->slices.len = 1;
    }
    pc_text_styles_fini(&d->text_styles);
    T_JSON(
        pc_text_styles,
        "{\"character\":[{\"id\":4294967295,\"name\":\"\",\"attrs\":{}}],\"paragraph\":[{\"id\":4294967295,"
        "\"name\":\"\",\"para_attrs\":{},\"char_attrs\":{}}],\"basic\":{\"id\":0,\"name\":\"Basic "
        "Paragraph\","
        "\"para_attrs\":{},\"char_attrs\":{}},\"psd_raw\":null}",
        &d->text_styles);
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_TEXT;
    pc_text_layer_default(&c.u.text);
    t_str(&c.u.text.text, "ab");
    T_JSON_ARRAY(pc_text_run,
                 "[{\"len\":1,\"style\":" T_CHAR_STYLE(
                     "Metrics", "0.0") "},"
                                       "{\"len\":18446744073709551615,\"style\":{\"size_pt\":24.0}}]",
                 &c.u.text.runs, &c.u.text.nruns);
    T_JSON_ARRAY(pc_paragraph_run, "[{\"len\":1,\"style\":{}},{\"len\":18446744073709551615,\"style\":{}}]",
                 &c.u.text.paragraphs, &c.u.text.nparagraphs);
    t_layer(&l, "Text", &c);
    t_push_layer(d, &l);
}

static inline void pd_small(pc_document *d)
{
    pc_size size = {20, 10};
    pc_layer l;
    float a = 0.5f;
    float b = 0.25f;
    CHECK(pc_document_init(d, "Small", size, PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    CHECK(pc_layer_raster(&l, "A", pc_document_pixel_format(d), NULL) == PC_OK);
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&l), 3, 4, &a, 1, NULL) == PC_OK);
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&l), 300, 4, &b, 1, NULL) == PC_OK);
    l.psd_blocks = calloc(1, sizeof *l.psd_blocks);
    if (l.psd_blocks != NULL) {
        memcpy(l.psd_blocks[0].key, "lnsr", 4);
        l.psd_blocks[0].data = t_blob(5, 4);
        l.npsd_blocks = 1;
    }
    t_push_layer(d, &l);
    d->icc_profile = t_blob(1, 10);
}

/* docs(), ids renumbered. v holds at least 32 entries. */
static inline size_t pd_docs(named_doc *v)
{
    static const char *const MODES[4] = {"rgb", "gray", "cmyk", "lab"};
    static const pc_color_mode MODEV[4] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                           PC_COLOR_MODE_LAB};
    static const char *const DEPTHS[3] = {"u8", "u16", "f32"};
    static const pc_sample_type DEPTHV[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    size_t n = 0;
    pc_size one = {1, 1};
    for (size_t m = 0; m < 4; m++) {
        for (size_t k = 0; k < 3; k++) {
            char name[32];
            (void)snprintf(name, sizeof name, "rich-%s-%s", MODES[m], DEPTHS[k]);
            t_rich_doc(MODEV[m], DEPTHV[k], &nd_add(v, &n, name)->doc);
        }
    }
    CHECK(pc_document_init(&nd_add(v, &n, "empty")->doc, "empty", one, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                           NULL) == PC_OK);
    pd_small(&nd_add(v, &n, "small")->doc);
    pd_extras(&nd_add(v, &n, "extras")->doc);
    pd_blending(&nd_add(v, &n, "blending")->doc);
    pd_video(&nd_add(v, &n, "video")->doc);
    for (size_t k = 0; k < 3; k++) {
        char name[32];
        (void)snprintf(name, sizeof name, "labels-%s", DEPTHS[k]);
        pd_labels(DEPTHV[k], &nd_add(v, &n, name)->doc);
    }
    pd_shapes(&nd_add(v, &n, "shapes")->doc);
    pd_exhausted(&nd_add(v, &n, "exhausted")->doc);
    t_nested_doc(40, PC_SAMPLE_TYPE_U8, &nd_add(v, &n, "deep40")->doc);
    t_nested_doc(PC_MAX_GROUP_DEPTH, PC_SAMPLE_TYPE_U16, &nd_add(v, &n, "deep100")->doc);
    for (size_t i = 0; i < n; i++) {
        t_renumber(&v[i].doc);
    }
    return n;
}

/* An Exposure adjustment layer on top (insert_above(None, ..)). */
static inline void pd_push_exposure(pc_document *d, const char *json)
{
    pc_layer_content c;
    pc_layer l;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_ADJUSTMENT;
    T_JSON(pc_adjustment, json, &c.u.adjustment);
    t_layer(&l, "adj", &c);
    t_push_layer(d, &l);
}

/* bad_docs(): saves the reference refuses. v holds at least 8 entries. */
static inline size_t pd_bad_docs(named_doc *v)
{
    size_t n = 0;
    pc_document *d;
    d = &nd_add(v, &n, "nan-exposure")->doc;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, d);
    pd_push_exposure(d, "{\"Exposure\":{\"exposure\":0.0,\"offset\":0.0,\"gamma\":1.0}}");
    d->layers[d->nlayers - 1].content.u.adjustment.u.exposure.exposure = NAN;
    d = &nd_add(v, &n, "inf-gamma")->doc;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, d);
    pd_push_exposure(d, "{\"Exposure\":{\"exposure\":0.0,\"offset\":0.0,\"gamma\":1.0}}");
    d->layers[d->nlayers - 1].content.u.adjustment.u.exposure.gamma = -INFINITY;
    d = &nd_add(v, &n, "inf-dpi")->doc;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, d);
    d->resolution_dpi = INFINITY;
    d = &nd_add(v, &n, "nan-perspective")->doc;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, d);
    {
        static const double p[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0};
        memcpy(d->layers[7].content.u.smart.perspective, p, sizeof p);
        d->layers[7].content.u.smart.perspective[8] = NAN;
        d->layers[7].content.u.smart.has_perspective = true;
    }
    d = &nd_add(v, &n, "inf-reference")->doc;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, d);
    d->layers[0].effects.has_reference = true;
    d->layers[0].effects.reference[0] = INFINITY;
    d->layers[0].effects.reference[1] = 0.0;
    d = &nd_add(v, &n, "nan-guide")->doc;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, d);
    {
        float *g = realloc(d->guides.vertical, (d->guides.nvertical + 1) * sizeof *g);
        if (g != NULL) {
            g[d->guides.nvertical++] = NAN;
            d->guides.vertical = g;
        }
    }
    d = &nd_add(v, &n, "nan-spot")->doc;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, d);
    d->channels[1].spot_solidity = NAN;
    t_nested_doc(PC_MAX_GROUP_DEPTH + 1, PC_SAMPLE_TYPE_U8, &nd_add(v, &n, "deep101")->doc);
    for (size_t i = 0; i < n; i++) {
        t_renumber(&v[i].doc);
    }
    return n;
}

#endif
