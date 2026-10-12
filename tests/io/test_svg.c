/* SVG import (port of crates/io/tests/svg.rs and the unit tests of crates/io/src/svg.rs): shapes,
 * groups, transforms, curves, gradients, rasterised fallbacks, images, the size limit, sniffing
 * and malformed input; plus the parser's own tables and hostile input. */
#include "harness.h"
#include "io/common.h"
#include "io/svg_filter.h"
#include "io/svg_internal.h"
#include "io/svg_ts.h"
#include "io/svg_text.h"

#include <pthread.h>

#define NS "xmlns=\"http://www.w3.org/2000/svg\""

static pc_io_import_result open_svg(const char *svg)
{
    pc_io_import_result r;
    pc_error e;
    pc_status st = pc_io_import("drawing.svg", (const uint8_t *)svg, strlen(svg), &r, &e);
    if (st != PC_OK) {
        (void)fprintf(stderr, "open failed: %s\n%s\n", e.msg, svg);
        abort();
    }
    return r;
}

static void px(const pc_document *d, int32_t x, int32_t y, float out[4])
{
    pc_compose_buffer b;
    const float *p;
    must(pc_compose_flatten(d, &b, NULL));
    p = pc_compose_buffer_get(&b, x, y);
    memcpy(out, p, 4 * sizeof(float));
    free(b.px);
}

static bool close4(const float a[4], const float b0, const float b1, const float b2, const float b3)
{
    return fabsf(a[0] - b0) < 0.03f && fabsf(a[1] - b1) < 0.03f && fabsf(a[2] - b2) < 0.03f &&
           fabsf(a[3] - b3) < 0.03f;
}

static bool any_warning(const pc_io_import_result *r, const char *needle)
{
    return pc_io_warnings_any(&r->warnings, needle);
}

static void test_basic_shapes(void)
{
    pc_io_import_result r =
        open_svg("<svg " NS " width=\"100\" height=\"60\">"
                 "<rect id=\"box\" x=\"10\" y=\"10\" width=\"30\" height=\"20\" fill=\"#ff0000\"/>"
                 "<circle cx=\"70\" cy=\"30\" r=\"15\" fill=\"rgb(0,0,255)\" stroke=\"#000\" "
                 "stroke-width=\"4\" stroke-linecap=\"round\" stroke-linejoin=\"bevel\"/></svg>");
    const pc_document *d = &r.document;
    float p[4];
    CHECK_EQ_UINT(d->size.width, 100u);
    CHECK_EQ_UINT(d->size.height, 60u);
    CHECK_EQ_UINT(d->nlayers, 2u);
    if (d->nlayers == 2) {
        const pc_shape_layer *sh = &d->layers[0].content.u.shape;
        const pc_shape_layer *c = &d->layers[1].content.u.shape;
        CHECK_EQ_STR(pc_str_c(&d->layers[0].name), "box");
        CHECK_EQ_STR(pc_str_c(&d->layers[1].name), "Shape 1");
        CHECK(d->layers[0].content.kind == PC_LAYER_SHAPE);
        CHECK(sh->has_fill && sh->fill.kind == PC_FILL_SOLID && sh->fill.u.solid.c[0] == 1.0f &&
              sh->fill.u.solid.c[1] == 0.0f && sh->fill.u.solid.alpha == 1.0f);
        CHECK(!sh->has_stroke);
        CHECK_EQ_UINT(sh->path.nsubpaths, 1u);
        CHECK(sh->path.nsubpaths == 1 && sh->path.subpaths[0].closed);
        CHECK(d->layers[1].content.kind == PC_LAYER_SHAPE && c->has_stroke);
        CHECK_NEAR(c->stroke.width, 4.0, 1e-3);
        CHECK(c->stroke.cap == PC_LINE_CAP_ROUND && c->stroke.join == PC_LINE_JOIN_BEVEL);
        CHECK(c->stroke.paint.kind == PC_FILL_SOLID && c->stroke.paint.u.solid.c[0] == 0.0f &&
              c->stroke.paint.u.solid.alpha == 1.0f);
    }
    px(d, 20, 20, p);
    CHECK(close4(p, 1, 0, 0, 1));
    px(d, 70, 30, p);
    CHECK(close4(p, 0, 0, 1, 1));
    px(d, 55, 30, p);
    CHECK(close4(p, 0, 0, 0, 1));
    px(d, 3, 3, p);
    CHECK(p[3] < 0.01f);
    px(d, 95, 55, p);
    CHECK(p[3] < 0.01f);
    CHECK_EQ_UINT(r.warnings.n, 0u);
    pc_io_import_result_fini(&r);
}

static void test_groups(void)
{
    pc_io_import_result r =
        open_svg("<svg " NS " width=\"40\" height=\"40\"><g transform=\"translate(5,5)\">"
                 "<g id=\"art\" opacity=\"0.5\" style=\"mix-blend-mode:multiply\">"
                 "<rect width=\"10\" height=\"10\" fill=\"#00ff00\"/>"
                 "<g id=\"inner\"><rect x=\"10\" width=\"10\" height=\"10\" fill=\"#0000ff\"/></g>"
                 "</g></g></svg>");
    const pc_document *d = &r.document;
    float p[4];
    CHECK_EQ_UINT(d->nlayers, 1u);
    if (d->nlayers == 1) {
        const pc_layer *art = &d->layers[0];
        CHECK_EQ_STR(pc_str_c(&art->name), "art");
        CHECK_NEAR(art->opacity, 0.5, 1e-6);
        CHECK(art->blend == PC_BLEND_MULTIPLY);
        CHECK(art->content.kind == PC_LAYER_GROUP);
        if (art->content.kind == PC_LAYER_GROUP && art->content.u.group.nchildren == 2) {
            CHECK(art->content.u.group.children[0].content.kind == PC_LAYER_SHAPE);
            CHECK_EQ_STR(pc_str_c(&art->content.u.group.children[1].name), "inner");
        } else {
            CHECK(false);
        }
    }
    px(d, 3, 3, p);
    CHECK(p[3] < 0.01f);
    px(d, 7, 7, p);
    CHECK(p[1] > 0.9f && fabsf(p[3] - 0.5f) < 0.03f);
    pc_io_import_result_fini(&r);
}

static void test_transforms_curves_fill_rules(void)
{
    pc_io_import_result r = open_svg(
        "<svg " NS " width=\"60\" height=\"60\">"
        "<g transform=\"translate(10,20) scale(2)\"><rect width=\"5\" height=\"5\" fill=\"black\"/></g>"
        "<path d=\"M30 10 Q 40 0 50 10 L 50 20 Z\" fill=\"black\"/>"
        "<path d=\"M0 40 h20 v20 h-20 z M5 45 h10 v10 h-10 z\" fill=\"black\" fill-rule=\"evenodd\"/>"
        "</svg>");
    const pc_document *d = &r.document;
    float p[4];
    CHECK_EQ_UINT(d->nlayers, 3u);
    if (d->nlayers == 3) {
        const pc_subpath *s = &d->layers[0].content.u.shape.path.subpaths[0];
        double xmin = 1e9;
        double xmax = -1e9;
        double ymin = 1e9;
        double ymax = -1e9;
        size_t i;
        const pc_knot *k;
        const pc_path *ring;
        for (i = 0; i < s->nknots; i++) {
            xmin = fmin(xmin, s->knots[i].anchor.x);
            xmax = fmax(xmax, s->knots[i].anchor.x);
            ymin = fmin(ymin, s->knots[i].anchor.y);
            ymax = fmax(ymax, s->knots[i].anchor.y);
        }
        CHECK(xmin == 10.0 && xmax == 20.0 && ymin == 20.0 && ymax == 30.0);
        CHECK_EQ_UINT(d->layers[1].content.u.shape.path.subpaths[0].nknots, 3u);
        k = d->layers[1].content.u.shape.path.subpaths[0].knots;
        CHECK(!(k[0].out_ctrl.x == k[0].anchor.x && k[0].out_ctrl.y == k[0].anchor.y));
        CHECK(!(k[1].in_ctrl.x == k[1].anchor.x && k[1].in_ctrl.y == k[1].anchor.y));
        CHECK(k[1].out_ctrl.x == k[1].anchor.x && k[1].out_ctrl.y == k[1].anchor.y);
        ring = &d->layers[2].content.u.shape.path;
        CHECK(ring->fill_rule == PC_FILL_RULE_EVENODD);
        CHECK_EQ_UINT(ring->nsubpaths, 2u);
    }
    px(d, 2, 42, p);
    CHECK(p[3] > 0.99f);
    px(d, 10, 50, p);
    CHECK(p[3] < 0.01f);
    pc_io_import_result_fini(&r);
}

static void test_gradients(void)
{
    pc_io_import_result r =
        open_svg("<svg " NS " width=\"100\" height=\"20\"><defs>"
                 "<linearGradient id=\"g\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\">"
                 "<stop offset=\"0\" stop-color=\"#ff0000\"/>"
                 "<stop offset=\"1\" stop-color=\"#0000ff\" stop-opacity=\"0.5\"/>"
                 "</linearGradient></defs><rect width=\"100\" height=\"20\" fill=\"url(#g)\"/></svg>");
    const pc_shape_layer *sh = &r.document.layers[0].content.u.shape;
    float l[4];
    float rr[4];
    CHECK(sh->has_fill && sh->fill.kind == PC_FILL_GRADIENT);
    if (sh->has_fill && sh->fill.kind == PC_FILL_GRADIENT) {
        CHECK_EQ_UINT(sh->fill.u.gradient.nstops, 2u);
        CHECK_NEAR(sh->fill.u.gradient.stops[1].color.alpha, 0.5, 1e-6);
        CHECK(fabsf(sh->fill.u.gradient.angle) < 1e-3f);
    }
    CHECK(any_warning(&r, "gradient"));
    px(&r.document, 2, 10, l);
    px(&r.document, 97, 10, rr);
    CHECK(l[0] > 0.8f && l[2] < 0.2f);
    CHECK(rr[2] > rr[0]);
    pc_io_import_result_fini(&r);
}

static void test_rasterised_fallbacks(void)
{
    pc_io_import_result r =
        open_svg("<svg " NS " width=\"60\" height=\"30\"><defs>"
                 "<clipPath id=\"c\"><rect width=\"10\" height=\"30\"/></clipPath>"
                 "<pattern id=\"p\" width=\"4\" height=\"4\" patternUnits=\"userSpaceOnUse\"><rect "
                 "width=\"2\" height=\"4\" fill=\"black\"/></pattern>"
                 "</defs><g id=\"clipped\" clip-path=\"url(#c)\"><rect width=\"30\" height=\"30\" "
                 "fill=\"#ff0000\"/></g>"
                 "<rect id=\"striped\" x=\"30\" width=\"30\" height=\"30\" fill=\"url(#p)\"/></svg>");
    const pc_document *d = &r.document;
    float p[4];
    CHECK_EQ_UINT(d->nlayers, 2u);
    if (d->nlayers == 2) {
        CHECK(d->layers[0].content.kind == PC_LAYER_RASTER);
        CHECK(d->layers[1].content.kind == PC_LAYER_RASTER);
    }
    px(d, 5, 15, p);
    CHECK(close4(p, 1, 0, 0, 1));
    px(d, 20, 15, p);
    CHECK(p[3] < 0.01f);
    px(d, 32, 15, p);
    CHECK(p[3] > 0.99f);
    px(d, 34, 15, p);
    CHECK(p[3] < 0.01f);
    CHECK(any_warning(&r, "clip-path") && any_warning(&r, "pattern"));
    pc_io_import_result_fini(&r);
}

/* A 2x2 PNG (red, green / blue, white) as a data URL. */
static char *png_data_url(const uint8_t *rgb)
{
    pc_cimage img;
    pc_buf png = PC_BUF_INIT;
    pc_buf b64 = PC_BUF_INIT;
    char *out;
    pc_cencode_opts o = pc_cencode_opts_default();
    must(pc_cimage_from_copy(2, 2, PC_CL_RGB, PC_CS_U8, rgb, 12, &img, NULL));
    must(pc_cencode(&img, PC_CF_PNG, &o, &png, NULL));
    must(pc_base64_encode(png.data, png.len, &b64, NULL));
    out = xmalloc(b64.len + 32);
    (void)snprintf(out, b64.len + 32, "data:image/png;base64,%s", (const char *)b64.data);
    pc_cimage_fini(&img);
    pc_buf_fini(&png);
    pc_buf_fini(&b64);
    return out;
}

static const uint8_t RGBW[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};

static void test_embedded_images(void)
{
    char *url = png_data_url(RGBW);
    char svg[4096];
    pc_io_import_result r;
    float p[4];
    (void)snprintf(
        svg, sizeof svg,
        "<svg " NS
        " width=\"20\" height=\"20\"><image id=\"photo\" x=\"0\" y=\"0\" width=\"20\" height=\"20\" "
        "href=\"%s\" style=\"image-rendering:pixelated\"/></svg>",
        url);
    r = open_svg(svg);
    CHECK_EQ_UINT(r.document.nlayers, 1u);
    if (r.document.nlayers == 1) {
        CHECK_EQ_STR(pc_str_c(&r.document.layers[0].name), "photo");
        CHECK(r.document.layers[0].content.kind == PC_LAYER_RASTER);
    }
    px(&r.document, 3, 3, p);
    CHECK(close4(p, 1, 0, 0, 1));
    px(&r.document, 16, 16, p);
    CHECK(close4(p, 1, 1, 1, 1));
    pc_io_import_result_fini(&r);
    free(url);
}

/* ---------------------------------------------------------------- text (I5b) */

/* Imports without asserting success (hostile text must never crash; it may fail cleanly). */
static void try_svg(const char *svg, size_t len)
{
    pc_io_import_result r;
    pc_error e;
    if (pc_io_import("t.svg", (const uint8_t *)svg, len, &r, &e) == PC_OK) {
        pc_io_import_result_fini(&r);
    }
    pc_t_checks++;
}

static bool has_family(const char *name)
{
    const svf_db *db = svf_db_get();
    svf_family f = {SVF_NAMED, name};
    return db != NULL && svf_db_query(db, &f, 1, 400, 5, SVF_STYLE_NORMAL) != SIZE_MAX;
}

static void test_text(void)
{
    pc_io_import_result r;
    /* A family missing everywhere (and generic families that may be missing too) imports cleanly:
     * either a fallback family draws it or the text is left out, never a failure. */
    r = open_svg("<svg " NS " width=\"60\" height=\"30\"><text x=\"5\" y=\"20\" "
                 "font-family=\"NoSuchFamilyAnywhere-9\">ab</text><rect width=\"2\" height=\"2\"/></svg>");
    CHECK(r.document.nlayers >= 1u);
    pc_io_import_result_fini(&r);
    if (!has_family("DejaVu Sans")) {
        (void)printf("test_text: DejaVu Sans not installed; rendering checks skipped\n");
        return;
    }
    /* hidden text: laid out, but no visible layer, so the reference's font warning */
    r = open_svg("<svg " NS " width=\"60\" height=\"30\"><text x=\"5\" y=\"20\" font-family=\"DejaVu Sans\" "
                 "visibility=\"hidden\">ab</text><rect width=\"2\" height=\"2\"/></svg>");
    CHECK_EQ_UINT(r.document.nlayers, 1u);
    CHECK(any_warning(&r, "font"));
    pc_io_import_result_fini(&r);
    /* visible text: a group named by its text holding the outlines */
    r = open_svg("<svg " NS " width=\"120\" height=\"60\"><text x=\"5\" y=\"45\" font-size=\"48\" "
                 "font-family=\"DejaVu Sans\" fill=\"black\">II</text></svg>");
    CHECK_EQ_UINT(r.document.nlayers, 1u);
    if (r.document.nlayers == 1u) {
        float p[4];
        CHECK_EQ_STR(r.document.layers[0].name.ptr, "II");
        CHECK(!any_warning(&r, "font"));
        px(&r.document, 10, 30, p);
        CHECK(p[3] > 0.9f);
        px(&r.document, 100, 5, p);
        CHECK(p[3] < 0.1f);
    }
    pc_io_import_result_fini(&r);
}

static char *repeat(const char *head, const char *unit, size_t n, const char *tail, size_t *len)
{
    size_t hl = strlen(head);
    size_t ul = strlen(unit);
    size_t tl = strlen(tail);
    char *s = malloc(hl + ul * n + tl + 1);
    size_t i;
    if (s == NULL) {
        abort();
    }
    memcpy(s, head, hl);
    for (i = 0; i < n; i++) {
        memcpy(s + hl + i * ul, unit, ul);
    }
    memcpy(s + hl + ul * n, tail, tl + 1);
    *len = hl + ul * n + tl;
    return s;
}

#define TH "<svg " NS " xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"64\" height=\"64\">"

static void test_text_hostile(void)
{
    static const char *const SVGS[] = {
        TH "<text font-size=\"0\">a</text><text font-size=\"-5\">b</text><text font-size=\"1e30\">c</text>"
           "<text font-size=\"1e39\">d</text><text font-size=\"nan\">e</text><text "
           "font-size=\"1e-30\">f</text></svg>",
        TH "<text letter-spacing=\"1e30\" word-spacing=\"-1e30\" x=\"1e38\" y=\"-1e38\">a b c</text>"
           "<text letter-spacing=\"-1e30\">x y</text><text textLength=\"1e30\">len</text>"
           "<text textLength=\"0.000001\" lengthAdjust=\"spacingAndGlyphs\">len</text></svg>",
        TH "<text dx=\"1e38 -1e38 1e38\" dy=\"nan 1e38\" rotate=\"1e30 -1e30 nan\">abc</text>"
           "<text baseline-shift=\"1e38\" dominant-baseline=\"bogus\">x<tspan "
           "baseline-shift=\"-1e38%\">y</tspan>"
           "</text></svg>",
        TH
        "<text><textPath href=\"#missing\">a</textPath></text><text><textPath href=\"#g\">b</textPath></text>"
        "<g id=\"g\"/><text id=\"t\"><textPath id=\"tp\" href=\"#tp\">c</textPath></text>"
        "<text><textPath href=\"#t\">d</textPath></text><path id=\"z\" d=\"M0 0\"/>"
        "<text><textPath href=\"#z\" startOffset=\"1e30%\">e</textPath></text>"
        "<path id=\"big\" d=\"M0 0 C1e30 1e30 -1e30 1e30 1e30 0\"/>"
        "<text><textPath href=\"#big\" startOffset=\"-50%\">fff</textPath></text>"
        "<path id=\"p\" d=\"M0 10 L60 10\" transform=\"scale(1e-30)\"/>"
        "<text><textPath href=\"#p\">g</textPath></text></svg>",
        TH "<text id=\"a\">x<tref href=\"#a\"/><tref xlink:href=\"#b\"/></text>"
           "<text id=\"b\">y<tref href=\"#a\"/></text><text><tref href=\"#\"/><tref "
           "href=\"bogus\"/></text></svg>",
        TH
        "<defs><pattern id=\"pt\" width=\"8\" height=\"8\" patternUnits=\"userSpaceOnUse\"><text "
        "y=\"6\">p</text>"
        "</pattern><mask id=\"m\"><text y=\"20\" fill=\"white\">m</text></mask><clipPath id=\"c\"><text "
        "y=\"30\">"
        "c</text></clipPath><marker id=\"mk\" markerWidth=\"10\" markerHeight=\"10\"><text y=\"8\">k</text>"
        "</marker></defs><rect width=\"64\" height=\"64\" fill=\"url(#pt)\" mask=\"url(#m)\" "
        "clip-path=\"url(#c)\"/><path d=\"M5 5 L50 50\" stroke=\"black\" marker-start=\"url(#mk)\"/></svg>",
        TH "<text>&#x85;a&#x2029;b&#x301;&#x200D;&#xFEFF;&#x202E;cd&#x2067;e&#xE000;&#x10FFFD;&#x1F600;"
           "</text><text>&#x301;&#x302;</text><text writing-mode=\"tb\">&#x3042;a&#x301;</text></svg>",
        TH "<text style=\"font: 12px/ ; font-family: ',,,'\">a</text><text "
           "font-family=\"'unterminated\">b</text>"
           "<text font-family=\"a b c d, ,\">c</text><text style=\"font: bold\">d</text>"
           "<text font-weight=\"bolder\"><tspan font-weight=\"lighter\"><tspan font-weight=\"999\">e</tspan>"
           "</tspan></text></svg>",
        TH "<text text-decoration=\"underline overline line-through\" font-size=\"1e-20\">u</text>"
           "<text text-decoration=\"underline\" letter-spacing=\"-100\">vv</text>"
           "<text writing-mode=\"tb\" text-decoration=\"underline\" rotate=\"90\">ww</text></svg>",
    };
    size_t i;
    size_t len;
    char *s;
    for (i = 0; i < sizeof SVGS / sizeof SVGS[0]; i++) {
        try_svg(SVGS[i], strlen(SVGS[i]));
    }
    /* 1 MB of characters: past SVT_MAX_CHARS, the element is dropped quickly */
    s = repeat(TH "<text x=\"1\" y=\"10\">", "a", (size_t)1 << 20, "</text></svg>", &len);
    try_svg(s, len);
    free(s);
    /* a long but allowed text and many tspans of alternating styles */
    s = repeat(TH "<text x=\"1\" y=\"10\" font-size=\"2\">", "ab cd ", 4000, "</text></svg>", &len);
    try_svg(s, len);
    free(s);
    s = repeat(TH "<text x=\"1\" y=\"10\" font-size=\"2\">",
               "<tspan fill=\"red\" font-size=\"3\">a</tspan><tspan font-weight=\"bold\" dy=\"1\">b</tspan>",
               10000, "</text></svg>", &len);
    try_svg(s, len);
    free(s);
    /* x positions on every character: one chunk each */
    {
        char *xs = repeat("", "1 ", 20000, "", &len);
        char *body = repeat("", "a", 20000, "", &len);
        size_t n = strlen(xs) + strlen(body) + 128;
        char *svg = malloc(n);
        if (svg == NULL) {
            abort();
        }
        (void)snprintf(svg, n, TH "<text x=\"%s\" y=\"5\" font-size=\"1\">%s</text></svg>", xs, body);
        try_svg(svg, strlen(svg));
        free(svg);
        free(xs);
        free(body);
    }
    /* a rotate list of 100k values */
    s = repeat(TH "<text x=\"1\" y=\"10\" rotate=\"", "15 ", 100000, "\">rotate me</text></svg>", &len);
    try_svg(s, len);
    free(s);
    /* nesting: 1,000 tspans deep works, 10,000 is past the XML parser's depth */
    s = repeat(TH "<text x=\"1\" y=\"10\">", "<tspan dx=\"0.1\">x", 1000, "", &len);
    {
        char *t = repeat(s, "</tspan>", 1000, "</text></svg>", &len);
        try_svg(t, len);
        free(t);
    }
    free(s);
    s = repeat(TH "<text>", "<tspan>", 10000, "deep", &len);
    {
        char *t = repeat(s, "</tspan>", 10000, "</text></svg>", &len);
        try_svg(t, len);
        free(t);
    }
    free(s);
    /* many trefs of one large text: the shared tref budget bounds the copies */
    {
        char *big = repeat("", "z", 1u << 20, "", &len);
        char *refs = repeat("", "<tref href=\"#src\"/>", 64, "", &len);
        size_t n = strlen(big) + strlen(refs) + 256;
        char *svg = malloc(n);
        if (svg == NULL) {
            abort();
        }
        (void)snprintf(svg, n, TH "<defs><text id=\"src\">%s</text></defs><text>%s</text></svg>", big, refs);
        try_svg(svg, strlen(svg));
        free(svg);
        free(big);
        free(refs);
    }
}

/* Text imports from several threads at once (the font database is shared) give the same result
 * as one import alone. */
typedef struct tjob {
    const char *svg;
    uint8_t *px;
    size_t n;
} tjob;

static void *text_thread(void *arg)
{
    tjob *j = arg;
    pc_io_import_result r;
    pc_compose_buffer b;
    pc_rgba8_image img;
    if (pc_io_import("t.svg", (const uint8_t *)j->svg, strlen(j->svg), &r, NULL) != PC_OK) {
        return NULL;
    }
    if (pc_compose_flatten(&r.document, &b, NULL) == PC_OK) {
        if (pc_compose_buffer_to_rgba8(&b, &img, NULL) == PC_OK) {
            j->px = img.pixels;
            j->n = (size_t)img.width * img.height * 4;
        }
        free(b.px);
    }
    pc_io_import_result_fini(&r);
    return NULL;
}

static void test_text_threads(void)
{
    static const char *SVG = TH "<text x=\"2\" y=\"20\" font-family=\"DejaVu Sans, Liberation Serif\" "
                                "font-size=\"14\" text-decoration=\"underline\">Ab&#x5D0;&#x301;</text>"
                                "<text font-family=\"Verdana\"><textPath href=\"#p\">curve</textPath></text>"
                                "<path id=\"p\" d=\"M2 50 Q32 20 60 50\" fill=\"none\"/></svg>";
    tjob one = {SVG, NULL, 0};
    tjob jobs[8];
    pthread_t th[8];
    size_t i;
    (void)text_thread(&one);
    for (i = 0; i < 8; i++) {
        jobs[i].svg = SVG;
        jobs[i].px = NULL;
        jobs[i].n = 0;
        CHECK(pthread_create(&th[i], NULL, text_thread, &jobs[i]) == 0);
    }
    for (i = 0; i < 8; i++) {
        (void)pthread_join(th[i], NULL);
        CHECK(jobs[i].n == one.n && (one.n == 0 || memcmp(jobs[i].px, one.px, one.n) == 0));
        free(jobs[i].px);
    }
    free(one.px);
}

static void test_size_and_shrink(void)
{
    pc_io_import_result r =
        open_svg("<svg " NS " viewBox=\"0 0 300 150\"><rect width=\"300\" height=\"150\"/></svg>");
    float p[4];
    int32_t w;
    int32_t h;
    CHECK_EQ_UINT(r.document.size.width, 300u);
    CHECK_EQ_UINT(r.document.size.height, 150u);
    pc_io_import_result_fini(&r);
    r = open_svg("<svg " NS " viewBox=\"0 0 100000 50000\"><rect x=\"50000\" width=\"50000\" "
                 "height=\"50000\" fill=\"black\"/></svg>");
    CHECK_EQ_UINT(r.document.size.width, PC_IO_SVG_MAX_SIDE);
    CHECK_EQ_UINT(r.document.size.height, PC_IO_SVG_MAX_SIDE / 2);
    CHECK(any_warning(&r, "scaled"));
    w = (int32_t)r.document.size.width;
    h = (int32_t)r.document.size.height;
    px(&r.document, w / 4, h / 2, p);
    CHECK(p[3] < 0.01f);
    px(&r.document, 3 * w / 4, h / 2, p);
    CHECK(p[3] > 0.99f);
    pc_io_import_result_fini(&r);
}

static void test_sniff_and_malformed(void)
{
    const char *svg = "<?xml version=\"1.0\"?>\n<svg " NS
                      " width=\"8\" height=\"8\"><rect width=\"8\" height=\"8\"/></svg>";
    static const char *const BAD[] = {"<svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect",
                                      "not xml at all", ""};
    pc_io_import_result r;
    pc_error e;
    size_t i;
    CHECK(pc_io_is_svg((const uint8_t *)svg, strlen(svg)));
    must(pc_io_import("mystery.bin", (const uint8_t *)svg, strlen(svg), &r, NULL));
    CHECK_EQ_UINT(r.document.nlayers, 1u);
    pc_io_import_result_fini(&r);
    for (i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        pc_status st = pc_io_import("x.svg", (const uint8_t *)BAD[i], strlen(BAD[i]), &r, &e);
        CHECK(st != PC_OK);
        if (st == PC_OK) {
            pc_io_import_result_fini(&r);
        } else {
            CHECK(pc_io_error_kind(st, &e) == PC_IO_ERR_SVG);
        }
    }
    r = open_svg("<svg " NS " width=\"8\" height=\"8\"/>");
    CHECK_EQ_UINT(r.document.nlayers, 0u);
    CHECK(any_warning(&r, "nothing"));
    pc_io_import_result_fini(&r);
}

static void test_rasterize(void)
{
    const char *svg = "<svg " NS " width=\"20\" height=\"10\"><rect x=\"0\" y=\"4\" width=\"20\" "
                      "height=\"0.5\" fill=\"black\"/></svg>";
    const char *huge =
        "<svg " NS " width=\"10000\" height=\"10000\"><rect width=\"10\" height=\"10\"/></svg>";
    pc_compose_buffer b;
    pc_error e;
    must(pc_io_svg_rasterize((const uint8_t *)svg, strlen(svg), 10.0f, &b, NULL));
    CHECK_EQ_UINT(pc_rect_width(b.rect), 200u);
    CHECK_EQ_UINT(pc_rect_height(b.rect), 100u);
    CHECK(pc_compose_buffer_get(&b, 100, 39)[3] < 0.01f);
    CHECK(pc_compose_buffer_get(&b, 100, 42)[3] > 0.99f);
    CHECK(pc_compose_buffer_get(&b, 100, 45)[3] < 0.01f);
    free(b.px);
    must(pc_io_svg_rasterize((const uint8_t *)svg, strlen(svg), 1.0f, &b, NULL));
    CHECK_EQ_UINT(pc_rect_width(b.rect), 20u);
    CHECK_EQ_UINT(pc_rect_height(b.rect), 10u);
    free(b.px);
    CHECK(pc_io_svg_rasterize((const uint8_t *)huge, strlen(huge), 64.0f, &b, &e) != PC_OK);
}

static void test_pcraft_round_trip(void)
{
    pc_io_import_result r =
        open_svg("<svg " NS " width=\"50\" height=\"50\"><rect id=\"a\" width=\"25\" height=\"25\" "
                 "fill=\"#123456\"/><circle cx=\"37\" cy=\"37\" r=\"10\" fill=\"#abcdef\"/></svg>");
    pc_io_export_result saved;
    pc_io_import_result back;
    float p1[4];
    float p2[4];
    must(pc_io_export(&r.document, "x.pcraft", NULL, &saved, NULL));
    must(pc_io_import("x.pcraft", saved.bytes, saved.len, &back, NULL));
    CHECK_EQ_UINT(back.document.nlayers, 2u);
    if (back.document.nlayers == 2) {
        CHECK(back.document.layers[0].content.kind == PC_LAYER_SHAPE);
        CHECK(back.document.layers[1].content.kind == PC_LAYER_SHAPE);
    }
    px(&back.document, 10, 10, p1);
    px(&r.document, 10, 10, p2);
    CHECK(close4(p1, p2[0], p2[1], p2[2], p2[3]));
    pc_io_export_result_fini(&saved);
    pc_io_import_result_fini(&back);
    pc_io_import_result_fini(&r);
}

static void test_local_files_never_read(void)
{
    static const uint8_t WHITE[12] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    char path[512];
    char *url;
    char svg[4096];
    pc_cimage img;
    pc_buf png = PC_BUF_INIT;
    pc_cencode_opts o = pc_cencode_opts_default();
    const char *hrefs[3];
    char fileurl[600];
    pc_io_import_result r;
    size_t i;
    FILE *f;
    (void)snprintf(path, sizeof path, "%s/secret.png", t_tmp());
    must(pc_cimage_from_copy(2, 2, PC_CL_RGB, PC_CS_U8, WHITE, 12, &img, NULL));
    must(pc_cencode(&img, PC_CF_PNG, &o, &png, NULL));
    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        CHECK(fwrite(png.data, 1, png.len, f) == png.len);
        (void)fclose(f);
    }
    (void)snprintf(fileurl, sizeof fileurl, "file://%s", path);
    hrefs[0] = path;
    hrefs[1] = fileurl;
    hrefs[2] = "secret.png";
    for (i = 0; i < 3; i++) {
        float p[4];
        size_t k;
        (void)snprintf(svg, sizeof svg,
                       "<svg " NS " width=\"20\" height=\"20\"><image x=\"0\" y=\"0\" width=\"20\" "
                       "height=\"20\" href=\"%s\"/></svg>",
                       hrefs[i]);
        r = open_svg(svg);
        for (k = 0; k < r.document.nlayers; k++) {
            CHECK(r.document.layers[k].content.kind != PC_LAYER_RASTER);
        }
        px(&r.document, 10, 10, p);
        CHECK(p[3] < 0.01f);
        pc_io_import_result_fini(&r);
    }
    url = png_data_url(RGBW);
    (void)snprintf(
        svg, sizeof svg,
        "<svg " NS " width=\"20\" height=\"20\"><image width=\"20\" height=\"20\" href=\"%s\"/></svg>", url);
    r = open_svg(svg);
    CHECK(r.document.nlayers == 1 && r.document.layers[0].content.kind == PC_LAYER_RASTER);
    pc_io_import_result_fini(&r);
    free(url);
    pc_cimage_fini(&img);
    pc_buf_fini(&png);
    (void)remove(path);
}

/* ---- svg.rs unit tests ---- */

static void test_sniffing(void)
{
    const char *bom = "\xef\xbb\xbf<?xml version=\"1.0\"?>\n<!-- hi -->\n<svg/>";
    CHECK(pc_io_is_svg((const uint8_t *)"<svg xmlns=\"http://www.w3.org/2000/svg\"/>", 41));
    CHECK(pc_io_is_svg((const uint8_t *)bom, strlen(bom)));
    CHECK(pc_io_is_svg((const uint8_t *)"<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"x\"><svg/>", 56));
    CHECK(
        !pc_io_is_svg((const uint8_t *)"<?xml version=\"1.0\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>", 58));
    CHECK(!pc_io_is_svg((const uint8_t *)"\x89PNG\r\n", 6));
    CHECK(!pc_io_is_svg((const uint8_t *)"", 0));
}

static void test_quads_become_cubics(void)
{
    /* The same path the Rust test builds with tiny-skia's PathBuilder, through the import. */
    pc_io_import_result r =
        open_svg("<svg " NS " width=\"40\" height=\"40\"><path d=\"M0 0 Q5 10 10 0 L0 0 Z M20 20 L30 20\" "
                 "stroke=\"black\" fill=\"none\"/></svg>");
    const pc_path *p = &r.document.layers[0].content.u.shape.path;
    CHECK_EQ_UINT(p->nsubpaths, 2u);
    if (p->nsubpaths == 2) {
        const pc_knot *a = &p->subpaths[0].knots[0];
        const pc_knot *b = &p->subpaths[0].knots[1];
        CHECK(p->subpaths[0].closed && !p->subpaths[1].closed);
        CHECK_EQ_UINT(p->subpaths[0].nknots, 2u);
        CHECK(fabs(a->out_ctrl.x - 10.0 / 3.0) < 1e-6 && fabs(a->out_ctrl.y - 20.0 / 3.0) < 1e-6);
        CHECK(fabs(b->in_ctrl.x - 20.0 / 3.0) < 1e-6 && fabs(b->in_ctrl.y - 20.0 / 3.0) < 1e-6);
        CHECK(b->anchor.x == 10.0 && b->anchor.y == 0.0);
        CHECK_EQ_UINT(p->subpaths[1].nknots, 2u);
    }
    pc_io_import_result_fini(&r);
}

static void test_tables_sorted(void)
{
    /* Every element and attribute name round-trips through the sorted lookup tables. */
    static const char *const E[] = {
        "a", "circle", "clipPath", "feGaussianBlur", "feImage", "linearGradient", "use", "svg", "tspan"};
    static const char *const A[] = {"clip-path",    "clipPathUnits", "fill", "font",      "font-family",
                                    "marker-end",   "markerHeight",  "mask", "mask-type", "maskUnits",
                                    "stroke-width", "style",         "x",    "x1",        "viewBox",
                                    "y2",           "href"};
    size_t i;
    for (i = 0; i < sizeof E / sizeof E[0]; i++) {
        CHECK(sv_eid_from(E[i]) != E_NONE);
    }
    for (i = 0; i < sizeof A / sizeof A[0]; i++) {
        CHECK(sv_aid_from(A[i]) != A_NONE);
        CHECK_EQ_STR(sv_aid_name(sv_aid_from(A[i])), A[i]);
    }
    {
        sv_color c;
        CHECK(sv_parse_color("aliceblue", &c) && c.r == 240 && c.b == 255);
        CHECK(sv_parse_color("yellowgreen", &c) && c.r == 154);
        CHECK(sv_parse_color("RED", &c) && c.r == 255 && c.g == 0);
        CHECK(sv_parse_color("transparent", &c) && c.a == 0);
    }
}

/* Markers (usvg marker.rs): instances at the vertices, recursion, the instance cap. */
static void test_markers(void)
{
    pc_io_import_result r;
    float p[4];
    char *big;
    size_t i;
    size_t n = 0;
    r = open_svg("<svg " NS " width=\"40\" height=\"20\"><defs><marker id=\"m\" markerWidth=\"4\" "
                 "markerHeight=\"4\" refX=\"2\" refY=\"2\" markerUnits=\"userSpaceOnUse\"><rect width=\"4\" "
                 "height=\"4\" fill=\"red\"/></marker></defs><path id=\"p\" d=\"M5 10 L20 10 L35 10\" "
                 "stroke=\"blue\" marker-start=\"url(#m)\" marker-mid=\"url(#m)\" marker-end=\"url(#m)\"/>"
                 "</svg>");
    /* the path, then three clipped marker groups rasterised as pixel layers */
    CHECK_EQ_UINT(r.document.nlayers, 4u);
    if (r.document.nlayers == 4) {
        CHECK_EQ_STR(pc_str_c(&r.document.layers[0].name), "p");
        CHECK(r.document.layers[1].content.kind == PC_LAYER_RASTER);
    }
    px(&r.document, 20, 9, p);
    CHECK(close4(p, 1, 0, 0, 1));
    px(&r.document, 35, 11, p);
    CHECK(close4(p, 1, 0, 0, 1));
    px(&r.document, 12, 10, p);
    CHECK(p[2] > 0.5f && p[0] < 0.1f);
    pc_io_import_result_fini(&r);
    /* a marker whose content uses itself, and markers inside a clip path, are ignored */
    r = open_svg("<svg " NS " width=\"20\" height=\"20\"><marker id=\"m\" markerUnits=\"userSpaceOnUse\">"
                 "<path d=\"M0 0 L3 3\" stroke=\"black\" marker-end=\"url(#m)\"/></marker><clipPath id=\"c\">"
                 "<path d=\"M0 0 L20 20 L0 20Z\" marker-end=\"url(#m)\"/></clipPath><path d=\"M2 2 L10 10\" "
                 "stroke=\"black\" marker-end=\"url(#m)\" clip-path=\"url(#c)\"/></svg>");
    CHECK(r.document.nlayers >= 1);
    pc_io_import_result_fini(&r);
    /* 200,000 vertices with a mid marker: bounded by the instance cap */
    big = xmalloc((size_t)1 << 23);
    n += (size_t)snprintf(big, 512,
                          "<svg " NS " width=\"100\" height=\"100\"><marker id=\"m\"><rect width=\"1\" "
                          "height=\"1\"/></marker><path stroke=\"black\" marker-mid=\"url(#m)\" d=\"M0 0");
    for (i = 0; i < 200000; i++) {
        n += (size_t)snprintf(big + n, 32, " L%zu %zu", i % 100, (i / 100) % 100);
    }
    (void)snprintf(big + n, 32, "\"/></svg>");
    {
        pc_error e;
        pc_status st = pc_io_import("big.svg", (const uint8_t *)big, strlen(big), &r, &e);
        CHECK(st == PC_OK || st == PC_ERR_NOMEM || st == PC_ERR_LIMIT);
        if (st == PC_OK) {
            CHECK(any_warning(&r, "more than"));
            pc_io_import_result_fini(&r);
        }
    }
    free(big);
    /* hostile marker geometry */
    r = open_svg(
        "<svg " NS " width=\"20\" height=\"20\"><marker id=\"m\" markerWidth=\"1e38\" markerHeight=\"1e38\" "
        "viewBox=\"0 0 1e-30 1e-30\" orient=\"1e30turn\" refX=\"NaN\"><circle r=\"1e30\"/></marker>"
        "<path d=\"M0 0 Q5 5 10 0 C 10 10 0 10 0 0 Z M3 3\" stroke-width=\"1e30\" marker-start=\"url(#m)\" "
        "marker-mid=\"url(#m)\" marker-end=\"url(#m)\"/></svg>");
    pc_io_import_result_fini(&r);
}

/* Nested SVG images (usvg load_sub_svg) and image decoding as resvg does it. */
static void test_nested_images(void)
{
    static const char INNER[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='10' "
        "height='10' fill='lime'/></svg>";
    pc_buf b64 = PC_BUF_INIT;
    char *svg;
    pc_io_import_result r;
    float p[4];
    must(pc_base64_encode((const uint8_t *)INNER, strlen(INNER), &b64, NULL));
    svg = xmalloc(b64.len + 1024);
    (void)snprintf(svg, b64.len + 1024,
                   "<svg " NS " width=\"20\" height=\"20\"><image id=\"n\" x=\"5\" y=\"5\" width=\"10\" "
                   "height=\"10\" href=\"data:image/svg+xml;base64,%.*s\"/></svg>",
                   (int)b64.len, (const char *)b64.data);
    r = open_svg(svg);
    CHECK_EQ_UINT(r.document.nlayers, 1u);
    px(&r.document, 10, 10, p);
    CHECK(close4(p, 0, 1, 0, 1));
    px(&r.document, 2, 2, p);
    CHECK(p[3] < 0.01f);
    pc_io_import_result_fini(&r);
    free(svg);
    pc_buf_fini(&b64);
    /* an image inside the nested drawing is ignored; a broken nested drawing drops the image */
    r = open_svg(
        "<svg " NS " width=\"20\" height=\"20\"><image width=\"10\" height=\"10\" "
        "href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='4' height='4'%3E"
        "%3Cimage width='4' height='4' href='data:image/svg+xml,%3Csvg/%3E'/%3E%3C/svg%3E\"/>"
        "<image width=\"5\" height=\"5\" href=\"data:image/svg+xml,%3Csvg\"/>"
        "<image width=\"5\" height=\"5\" href=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUv////////8=\"/>"
        "</svg>");
    pc_io_import_result_fini(&r);
}

/* ---------------------------------------------------------------- filters (I5b) */

typedef struct ftree {
    sv_arena a;
    sv_tree t;
    bool ok;
} ftree;

static void ftree_parse(ftree *f, const char *body)
{
    char svg[8192];
    char msg[256];
    (void)snprintf(
        svg, sizeof svg,
        "<svg " NS " xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"64\" height=\"64\">%s</svg>", body);
    sv_arena_init(&f->a);
    f->ok = sv_parse_tree(&f->a, (const uint8_t *)svg, strlen(svg), &f->t, msg, sizeof msg) == PC_OK;
    CHECK(f->ok);
}

static void ftree_fini(ftree *f)
{
    sv_arena_fini(&f->a);
}

static const sv_unode *child(const sv_tree *t, size_t i)
{
    return t->root != NULL && i < t->root->nchildren ? t->root->children[i] : NULL;
}

static bool rect_is(sv_rect r, float l, float t, float rr, float b)
{
    return fabsf(r.l - l) < 1e-4f && fabsf(r.t - t) < 1e-4f && fabsf(r.r - rr) < 1e-4f &&
           fabsf(r.b - b) < 1e-4f;
}

/* usvg's filter conversion: what is dropped, kept and shared, regions, inputs. */
static void test_filter_conversion(void)
{
    ftree f;
    const sv_unode *g;
    /* an invalid link drops the element; one valid filter among invalid ones keeps it */
    ftree_parse(&f, "<filter id=\"o\"><feOffset dx=\"1\"/></filter><rect width=\"9\" height=\"9\" "
                    "filter=\"url(#missing)\"/><filter id=\"e\"/><rect width=\"9\" height=\"9\" "
                    "filter=\"url(#e)\"/><rect x=\"10\" y=\"10\" width=\"20\" height=\"20\" "
                    "filter=\"url(#missing) url(#o)\"/>");
    CHECK_EQ_UINT(f.t.root->nchildren, 1u);
    g = child(&f.t, 0);
    CHECK(g != NULL && g->kind == UN_GROUP && g->nfilters == 1);
    if (g != NULL && g->nfilters == 1) {
        const sv_filter *fl = g->filters[0];
        CHECK(rect_is(fl->rect, 8, 8, 32, 32));
        CHECK(rect_is(g->layer_bbox, 8, 8, 32, 32));
        CHECK_EQ_UINT(fl->nprims, 1u);
        CHECK(fl->prims[0].kind == FK_OFFSET && fl->prims[0].in1.kind == FIN_SOURCE_GRAPHIC);
        CHECK_EQ_STR(fl->prims[0].result, "result1");
        CHECK(fl->prims[0].linear);
    }
    ftree_fini(&f);
    /* a malformed list is ignored (the element stays, unfiltered); "none" too */
    ftree_parse(&f, "<rect width=\"9\" height=\"9\" filter=\"blur(2), grayscale(1)\"/><rect width=\"9\" "
                    "height=\"9\" filter=\"none\"/><rect width=\"9\" height=\"9\" filter=\"blur(-1)\"/>");
    CHECK_EQ_UINT(f.t.root->nchildren, 3u);
    CHECK(child(&f.t, 0) != NULL && child(&f.t, 0)->kind == UN_PATH);
    ftree_fini(&f);
    /* filter functions: one primitive each, sRGB, a fixed region around the bounding box */
    ftree_parse(&f, "<rect x=\"10\" y=\"10\" width=\"20\" height=\"20\" filter=\"blur(2) grayscale(1) "
                    "drop-shadow(red 1px 2px)\"/>");
    g = child(&f.t, 0);
    CHECK(g != NULL && g->nfilters == 3);
    if (g != NULL && g->nfilters == 3) {
        CHECK(rect_is(g->filters[0]->rect, 0, 0, 40, 40));
        CHECK(rect_is(g->filters[1]->rect, 8, 8, 32, 32));
        CHECK(g->filters[0]->prims[0].kind == FK_GAUSSIAN_BLUR && !g->filters[0]->prims[0].linear);
        CHECK(g->filters[1]->prims[0].kind == FK_COLOR_MATRIX);
        CHECK(g->filters[2]->prims[0].kind == FK_DROP_SHADOW && g->filters[2]->prims[0].r == 255);
        CHECK(rect_is(g->layer_bbox, 0, 0, 40, 40));
    }
    ftree_fini(&f);
    /* userSpaceOnUse filters are shared; objectBoundingBox ones are per element */
    ftree_parse(&f,
                "<filter id=\"u\" filterUnits=\"userSpaceOnUse\"><feFlood/></filter><filter id=\"b\">"
                "<feFlood/></filter><rect width=\"9\" height=\"9\" filter=\"url(#u)\"/><rect width=\"9\" "
                "height=\"9\" filter=\"url(#u)\"/><rect width=\"9\" height=\"9\" filter=\"url(#b)\"/><rect "
                "width=\"9\" height=\"9\" filter=\"url(#b)\"/>");
    CHECK_EQ_UINT(f.t.root->nchildren, 4u);
    if (f.t.root->nchildren == 4) {
        CHECK(child(&f.t, 0)->filters[0] == child(&f.t, 1)->filters[0]);
        CHECK_EQ_UINT(child(&f.t, 0)->filters[0]->refs, 2u);
        CHECK(child(&f.t, 2)->filters[0] != child(&f.t, 3)->filters[0]);
        /* -10 % .. 120 % of the viewport for userSpaceOnUse */
        CHECK(rect_is(child(&f.t, 0)->filters[0]->rect, -6.4f, -6.4f, 70.4f, 70.4f));
    }
    ftree_fini(&f);
    /* in / in2 / result resolution: named, unknown (previous), SourceAlpha, BackgroundImage */
    ftree_parse(&f,
                "<filter id=\"f\"><feFlood result=\"a\"/><feOffset in=\"nosuch\"/><feBlend in=\"a\" "
                "in2=\"SourceAlpha\"/><feMerge><feMergeNode in=\"BackgroundAlpha\"/><feMergeNode/>"
                "</feMerge><feTile result=\"result2\" in=\"result3\"/><feOffset/></filter><rect width=\"9\" "
                "height=\"9\" filter=\"url(#f)\"/>");
    g = child(&f.t, 0);
    CHECK(g != NULL && g->nfilters == 1);
    if (g != NULL && g->nfilters == 1 && g->filters[0]->nprims == 6) {
        const sv_fprim *p = g->filters[0]->prims;
        CHECK_EQ_STR(p[0].result, "a");
        CHECK(p[1].in1.kind == FIN_REF && p[1].in1.ref == 0);
        CHECK_EQ_STR(p[1].result, "result2");
        CHECK(p[2].in1.ref == 0 && p[2].in2.kind == FIN_SOURCE_ALPHA);
        CHECK_EQ_STR(p[2].result, "result3");
        CHECK(p[3].ninputs == 2 && p[3].inputs[0].kind == FIN_SOURCE_GRAPHIC && p[3].inputs[1].ref == 2);
        CHECK(p[4].in1.ref == 2);
        CHECK_EQ_STR(p[4].result, "result2");
        CHECK(p[5].in1.ref == 4);
        CHECK_EQ_STR(p[5].result, "result6");
    } else {
        CHECK(false);
    }
    ftree_fini(&f);
    /* empty groups keep a userSpaceOnUse filter; an objectBoundingBox one drops them */
    ftree_parse(&f,
                "<filter id=\"u\" filterUnits=\"userSpaceOnUse\" x=\"1\" y=\"2\" width=\"3\" "
                "height=\"4\"><feFlood/></filter><filter id=\"b\"><feFlood/></filter><g filter=\"url(#u)\"/>"
                "<g filter=\"url(#b)\"/>");
    CHECK_EQ_UINT(f.t.root->nchildren, 1u);
    CHECK(child(&f.t, 0) != NULL && rect_is(child(&f.t, 0)->layer_bbox, 1, 2, 4, 6));
    ftree_fini(&f);
    /* feImage -> element -> the same filter: the element's filter is cut (usvg's fix) */
    ftree_parse(&f, "<filter id=\"f\"><feImage xlink:href=\"#r\"/></filter><rect id=\"r\" width=\"9\" "
                    "height=\"9\" filter=\"url(#f)\"/>");
    CHECK_EQ_UINT(f.t.root->nchildren, 1u);
    CHECK(child(&f.t, 0) != NULL && child(&f.t, 0)->nfilters == 0);
    ftree_fini(&f);
    /* primitive subregions, primitiveUnits objectBoundingBox, href inheritance */
    ftree_parse(&f,
                "<filter id=\"b\" primitiveUnits=\"objectBoundingBox\"><feOffset dx=\"0.5\" x=\"0.1\" "
                "width=\"0.5\"/><feFlood x=\"0.25\" y=\"0.5\" width=\"0.5\" height=\"0.5\"/></filter><filter "
                "id=\"c\" xlink:href=\"#b\" x=\"0\" width=\"1\"/><rect x=\"10\" y=\"10\" width=\"20\" "
                "height=\"40\" filter=\"url(#c)\"/>");
    g = child(&f.t, 0);
    CHECK(g != NULL && g->nfilters == 1 && g->filters[0]->nprims == 2);
    if (g != NULL && g->nfilters == 1 && g->filters[0]->nprims == 2) {
        const sv_fprim *p = g->filters[0]->prims;
        CHECK(rect_is(g->filters[0]->rect, 10, 6, 30, 54));
        CHECK(fabsf(p[0].dx - 10.0f) < 1e-4f);
        CHECK(rect_is(p[1].rect, 15, 30, 25, 50));
    }
    ftree_fini(&f);
}

/* Hostile filters: bounded work and memory, never a crash. */
static void test_filter_hostile(void)
{
    static const char *const BODIES[] = {
        "<filter id=\"f\"><feGaussianBlur stdDeviation=\"1e30\"/></filter>",
        "<filter id=\"f\"><feGaussianBlur stdDeviation=\"1e9 0.01\"/></filter>",
        "<filter id=\"f\"><feDropShadow stdDeviation=\"3e38\" dx=\"1e38\" dy=\"-1e38\"/></filter>",
        "<filter id=\"f\"><feMorphology radius=\"1e9\"/></filter>",
        "<filter id=\"f\"><feMorphology operator=\"dilate\" radius=\"1e39 3\"/></filter>",
        "<filter id=\"f\"><feMorphology radius=\"2147483648\"/></filter>",
        "<filter id=\"f\"><feConvolveMatrix order=\"3\" kernelMatrix=\"1 1 1 1 1 1 1 1 -8\"/></filter>",
        "<filter id=\"f\"><feConvolveMatrix order=\"3\" kernelMatrix=\"1 1 1 1 1 1 1 1 1\" "
        "divisor=\"0\"/></filter>",
        "<filter id=\"f\"><feConvolveMatrix order=\"100000 100000\" kernelMatrix=\"1\"/></filter>",
        "<filter id=\"f\"><feConvolveMatrix order=\"2\" targetX=\"-1e30\" kernelMatrix=\"1 2 3 "
        "4\"/></filter>",
        "<filter id=\"f\"><feTurbulence numOctaves=\"1e9\" baseFrequency=\"0.3\"/></filter>",
        "<filter id=\"f\"><feTurbulence numOctaves=\"40\" baseFrequency=\"1e30\" stitchTiles=\"stitch\" "
        "seed=\"-2147483648\"/></filter>",
        "<filter id=\"f\"><feTurbulence baseFrequency=\"1e39\" seed=\"1e30\"/></filter>",
        "<filter id=\"f\"><feDiffuseLighting surfaceScale=\"1e30\"><fePointLight "
        "z=\"1e38\"/></feDiffuseLighting>"
        "</filter>",
        "<filter id=\"f\"><feSpecularLighting surfaceScale=\"NaN\" specularExponent=\"128\"><feSpotLight "
        "x=\"1e38\" limitingConeAngle=\"1e30\" specularExponent=\"-1\"/></feSpecularLighting></filter>",
        "<filter id=\"f\"><feComponentTransfer><feFuncR type=\"gamma\" exponent=\"-1e30\"/><feFuncA "
        "type=\"discrete\" tableValues=\"1e38 -1e38\"/></feComponentTransfer></filter>",
        "<filter id=\"f\"><feDisplacementMap scale=\"1e38\" in2=\"SourceGraphic\"/></filter>",
        "<filter id=\"f\" filterUnits=\"userSpaceOnUse\" x=\"-1e9\" y=\"-1e9\" width=\"2e9\" height=\"2e9\">"
        "<feFlood/><feTile/></filter>",
        "<filter id=\"f\" x=\"-1e30\" width=\"3e30\"><feOffset dx=\"1e30\"/></filter>",
        "<filter id=\"f\"><feOffset result=\"a\" in=\"a\"/><feBlend in=\"a\" in2=\"a\"/></filter>",
        "<filter id=\"f\"><feImage xlink:href=\"#r\"/></filter><filter id=\"g\"><feImage xlink:href=\"#s\"/>"
        "</filter><rect id=\"s\" width=\"5\" height=\"5\" filter=\"url(#f)\"/>",
        "<filter id=\"f\"><feImage xlink:href=\"#q\"/><feImage xlink:href=\"#q\"/></filter><filter id=\"g\">"
        "<feImage xlink:href=\"#q2\"/><feImage xlink:href=\"#q2\"/></filter><g id=\"q\" filter=\"url(#g)\">"
        "<rect width=\"3\" height=\"3\"/></g><g id=\"q2\" filter=\"url(#f)\"><rect width=\"3\" "
        "height=\"3\"/></g>",
        "<filter id=\"f\"><feFlood/></filter><g id=\"u\" filter=\"url(#f)\"><use href=\"#u\"/></g>",
        "<filter id=\"f\" filterUnits=\"userSpaceOnUse\"><feImage href=\"data:image/png;base64,AAAA\"/>"
        "<feImage href=\"file:///etc/passwd\"/></filter>",
    };
    size_t i;
    for (i = 0; i < sizeof BODIES / sizeof BODIES[0]; i++) {
        char svg[4096];
        int n =
            snprintf(svg, sizeof svg,
                     "<svg " NS " xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"64\" height=\"64\">%s"
                     "<rect id=\"r\" x=\"10\" y=\"10\" width=\"30\" height=\"30\" fill=\"#a50\" "
                     "filter=\"url(#f)\"/><g filter=\"url(#g)\"><rect width=\"5\" height=\"5\"/></g></svg>",
                     BODIES[i]);
        try_svg(svg, (size_t)n);
    }
    /* thousands of primitives */
    {
        size_t len;
        char *svg =
            repeat("<svg " NS " width=\"64\" height=\"64\"><filter id=\"f\">", "<feOffset dx=\"1\"/>", 5000,
                   "</filter><rect width=\"30\" height=\"30\" filter=\"url(#f)\"/></svg>", &len);
        try_svg(svg, len);
        free(svg);
        svg = repeat("<svg " NS " width=\"64\" height=\"64\"><filter id=\"f\">",
                     "<feGaussianBlur stdDeviation=\"0.5\" result=\"x\"/><feMerge><feMergeNode in=\"x\"/>"
                     "<feMergeNode in=\"x\"/></feMerge>",
                     2000, "</filter><rect width=\"30\" height=\"30\" filter=\"url(#f)\"/></svg>", &len);
        try_svg(svg, len);
        free(svg);
    }
}

/* Filter output does not depend on the thread count (single-threaded code; checked anyway). */
static void test_filter_threads(void)
{
    static const char SVG[] =
        "<svg " NS " width=\"40\" height=\"40\"><filter id=\"f\"><feTurbulence baseFrequency=\"0.1\" "
        "numOctaves=\"3\"/><feGaussianBlur stdDeviation=\"2\"/><feComposite in2=\"SourceAlpha\" "
        "operator=\"in\"/>"
        "</filter><rect x=\"5\" y=\"5\" width=\"30\" height=\"30\" filter=\"url(#f)\"/></svg>";
    pc_io_import_result a;
    pc_io_import_result b;
    pc_compose_buffer fa;
    pc_compose_buffer fb;
    setenv("PHOTOCRAFT_THREADS", "1", 1);
    a = open_svg(SVG);
    setenv("PHOTOCRAFT_THREADS", "8", 1);
    b = open_svg(SVG);
    unsetenv("PHOTOCRAFT_THREADS");
    must(pc_compose_flatten(&a.document, &fa, NULL));
    must(pc_compose_flatten(&b.document, &fb, NULL));
    CHECK(memcmp(&fa.rect, &fb.rect, sizeof fa.rect) == 0);
    if (memcmp(&fa.rect, &fb.rect, sizeof fa.rect) == 0 && fa.px != NULL) {
        size_t n = (size_t)(fa.rect.x1 - fa.rect.x0) * (size_t)(fa.rect.y1 - fa.rect.y0);
        CHECK(memcmp(fa.px, fb.px, n * 4 * sizeof(float)) == 0);
    }
    free(fa.px);
    free(fb.px);
    pc_io_import_result_fini(&a);
    pc_io_import_result_fini(&b);
}

static void test_hostile(void)
{
    static const char *const SVGS[] = {
        /* deep nesting */
        NULL,
        /* recursive use / patterns / clip paths */
        "<svg " NS " width=\"10\" height=\"10\"><g id=\"a\"><use href=\"#b\"/></g><g id=\"b\"><use "
        "href=\"#a\"/></g></svg>",
        "<svg " NS " width=\"10\" height=\"10\"><pattern id=\"p\" width=\"2\" height=\"2\"><rect width=\"1\" "
        "height=\"1\" "
        "fill=\"url(#p)\"/></pattern><rect width=\"10\" height=\"10\" fill=\"url(#p)\"/></svg>",
        "<svg " NS " width=\"10\" height=\"10\"><clipPath id=\"c\" clip-path=\"url(#c)\"><rect width=\"5\" "
        "height=\"5\"/>"
        "</clipPath><rect width=\"10\" height=\"10\" clip-path=\"url(#c)\"/></svg>",
        /* huge numbers and arcs */
        "<svg " NS
        " width=\"10\" height=\"10\"><path d=\"M0 0 A1e30 1e30 0 1 1 1e38 1e38 L-1e38 3 Z\" stroke=\"red\"/>"
        "<circle r=\"1e38\" cx=\"5\" cy=\"5\" fill=\"url(#none) blue\"/><rect width=\"1e-40\" "
        "height=\"3\"/></svg>",
        "<svg " NS
        " width=\"10\" height=\"10\"><g transform=\"scale(1e-30)\"><rect width=\"1e30\" height=\"1e30\"/></g>"
        "<g transform=\"matrix(1e20 0 0 1e20 1e30 1e30)\"><rect width=\"1\" height=\"1\" "
        "stroke=\"black\"/></g></svg>",
        /* entities */
        "<!DOCTYPE svg [<!ENTITY a \"&b;&b;&b;&b;&b;&b;&b;&b;\"><!ENTITY b \"&c;&c;&c;&c;&c;&c;&c;&c;\">"
        "<!ENTITY c \"&d;&d;&d;&d;&d;&d;&d;&d;\"><!ENTITY d \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\">]>"
        "<svg " NS " width=\"10\" height=\"10\"><text>&a;&a;&a;&a;</text></svg>",
        "<!DOCTYPE svg [<!ENTITY a \"&a;\">]><svg " NS "><text>&a;</text></svg>",
        /* masks, filters, dashes, gradients with odd stops */
        "<svg " NS
        " width=\"10\" height=\"10\"><mask id=\"m\"><rect width=\"5\" height=\"5\" fill=\"white\"/></mask>"
        "<filter id=\"f\"><feGaussianBlur stdDeviation=\"2\"/></filter><linearGradient id=\"g\">"
        "<stop offset=\"1\"/><stop offset=\"0\" stop-color=\"red\"/><stop/></linearGradient>"
        "<g mask=\"url(#m)\" filter=\"url(#f) blur(3)\"><rect width=\"10\" height=\"10\" fill=\"url(#g)\" "
        "stroke=\"black\" stroke-dasharray=\"0.0001 1e30\"/></g></svg>",
        /* CSS */
        "<svg " NS " width=\"10\" height=\"10\"><style>* g g g g g g g g g g g g g g g g g g g g rect "
        "{fill:red !important}"
        " .a > .b + rect, #x[y~=z]:first-child{stroke: url(\"#q\") } @media x { rect {} }</style>"
        "<rect class=\"a b\" width=\"5\" height=\"5\" style=\"fill: green; ;;stroke-width: 3px "
        "!important\"/></svg>",
        /* not UTF-8, NUL */
        "<svg \xff/>",
    };
    char *deep;
    size_t i;
    size_t n = 3000;
    deep = xmalloc(n * 10 + 200);
    strcpy(deep, "<svg " NS " width=\"10\" height=\"10\">");
    for (i = 0; i < n; i++) {
        strcat(deep, "<g>");
    }
    strcat(deep, "<rect width=\"5\" height=\"5\"/>");
    for (i = 0; i < n; i++) {
        strcat(deep, "</g>");
    }
    strcat(deep, "</svg>");
    for (i = 0; i < sizeof SVGS / sizeof SVGS[0]; i++) {
        const char *s = SVGS[i] != NULL ? SVGS[i] : deep;
        pc_io_import_result r;
        pc_compose_buffer b;
        pc_error e;
        if (pc_io_import("h.svg", (const uint8_t *)s, strlen(s), &r, &e) == PC_OK) {
            pc_compose_buffer fb;
            if (pc_compose_flatten(&r.document, &fb, NULL) == PC_OK) {
                free(fb.px);
            }
            pc_io_import_result_fini(&r);
        }
        if (pc_io_svg_rasterize((const uint8_t *)s, strlen(s), 2.0f, &b, &e) == PC_OK) {
            free(b.px);
        }
        CHECK(true);
    }
    free(deep);
    {
        static const uint8_t NUL[] = "<svg xmlns=\"http://www.w3.org/2000/svg\">\0</svg>";
        pc_io_import_result r;
        pc_error e;
        CHECK(pc_io_import("n.svg", NUL, sizeof NUL - 1, &r, &e) != PC_OK);
    }
}

/* ---- the tiny-skia rasteriser port (svg_raster.c, svg_ts_*.c) ---- */

typedef struct cov_sink {
    uint8_t *a; /* w * h, last alpha written */
    uint32_t w, h;
    size_t spans, pixels;
} cov_sink;

static void cov_span(void *ctx, uint32_t x, uint32_t y, uint32_t w, uint8_t a)
{
    cov_sink *c = ctx;
    uint32_t k;
    c->spans++;
    c->pixels += w;
    for (k = 0; k < w && c->a != NULL; k++) {
        if (y < c->h && x + k < c->w) {
            c->a[(size_t)y * c->w + x + k] = a;
        }
    }
}

static void sink_init(cov_sink *c, sv_spans *s, uint32_t w, uint32_t h, bool store)
{
    memset(c, 0, sizeof *c);
    c->w = w;
    c->h = h;
    c->a = store ? calloc((size_t)w * h, 1) : NULL;
    s->span = cov_span;
    s->ctx = c;
}

/* A finished builder as an sv_path (borrowing its arrays). */
static bool as_path(const ts_pb *b, sv_path *out)
{
    ts_path p;
    if (!ts_pb_finish(b, &p)) {
        return false;
    }
    out->verbs = b->verbs;
    out->nverbs = b->nv;
    out->pts = b->pts;
    out->npts = b->np;
    out->bounds = p.bounds;
    return true;
}

static sv_stroke stroke_of(float w, sv_cap cap, sv_join join)
{
    sv_stroke s;
    memset(&s, 0, sizeof s);
    s.width = w;
    s.miterlimit = 4.0f;
    s.cap = cap;
    s.join = join;
    s.opacity = 1.0f;
    return s;
}

static uint8_t at(const cov_sink *c, uint32_t x, uint32_t y)
{
    return c->a[(size_t)y * c->w + x];
}

static void test_raster_coverage(void)
{
    ts_pb b;
    sv_path p;
    sv_cov cov;
    cov_sink c;
    sv_spans s;
    sv_stroke st;
    uint32_t x;
    uint32_t y;
    bool sym = true;
    /* a pixel-aligned rectangle: full coverage inside, none outside */
    ts_pb_init(&b);
    ts_pb_move_to(&b, 2, 3);
    ts_pb_line_to(&b, 7, 3);
    ts_pb_line_to(&b, 7, 9);
    ts_pb_line_to(&b, 2, 9);
    ts_pb_close(&b);
    CHECK(as_path(&b, &p));
    must(sv_fill_cov(&p, ts_identity(), false, true, 10, 12, &cov));
    for (y = 0; y < 12; y++) {
        for (x = 0; x < 10; x++) {
            bool in = x >= 2 && x < 7 && y >= 3 && y < 9;
            CHECK_EQ_UINT(cov.a[y * 10 + x], in ? 255u : 0u);
        }
    }
    sv_cov_fini(&cov);
    /* half-pixel edges: two of the four supersamples, 128 like tiny-skia */
    must(sv_fill_cov(&p, ts_translate(0.5f, 0.0f), false, true, 10, 12, &cov));
    CHECK_EQ_UINT(cov.a[5 * 10 + 2], 128u);
    CHECK_EQ_UINT(cov.a[5 * 10 + 7], 128u);
    CHECK_EQ_UINT(cov.a[5 * 10 + 4], 255u);
    sv_cov_fini(&cov);
    /* a circle centred on a pixel corner covers mirror-image pixels alike */
    ts_pb_clear(&b);
    ts_pb_push_circle(&b, 8.0f, 8.0f, 5.3f);
    CHECK(as_path(&b, &p));
    must(sv_fill_cov(&p, ts_identity(), false, true, 16, 16, &cov));
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            sym = sym && cov.a[y * 16 + x] == cov.a[y * 16 + 15 - x];
        }
    }
    CHECK(sym);
    CHECK_EQ_UINT(cov.a[8 * 16 + 8], 255u);
    CHECK_EQ_UINT(cov.a[0], 0u);
    sv_cov_fini(&cov);
    /* two overlapping squares wound the same way: non-zero fills the overlap, even-odd doesn't */
    ts_pb_clear(&b);
    ts_pb_move_to(&b, 0, 0);
    ts_pb_line_to(&b, 6, 0);
    ts_pb_line_to(&b, 6, 6);
    ts_pb_line_to(&b, 0, 6);
    ts_pb_close(&b);
    ts_pb_move_to(&b, 3, 3);
    ts_pb_line_to(&b, 9, 3);
    ts_pb_line_to(&b, 9, 9);
    ts_pb_line_to(&b, 3, 9);
    ts_pb_close(&b);
    CHECK(as_path(&b, &p));
    must(sv_fill_cov(&p, ts_identity(), false, true, 10, 10, &cov));
    CHECK_EQ_UINT(cov.a[4 * 10 + 4], 255u);
    sv_cov_fini(&cov);
    must(sv_fill_cov(&p, ts_identity(), true, true, 10, 10, &cov));
    CHECK_EQ_UINT(cov.a[4 * 10 + 4], 0u);
    CHECK_EQ_UINT(cov.a[1 * 10 + 1], 255u);
    sv_cov_fini(&cov);
    /* caps: a horizontal 4 px stroke from x 4 to 12 at y 8 */
    ts_pb_clear(&b);
    ts_pb_move_to(&b, 4, 8);
    ts_pb_line_to(&b, 12, 8);
    CHECK(as_path(&b, &p));
    st = stroke_of(4.0f, CAP_BUTT, JOIN_MITER);
    sink_init(&c, &s, 16, 16, true);
    must(sv_stroke_path(&p, ts_identity(), &st, true, 16, 16, &s));
    CHECK_EQ_UINT(at(&c, 8, 7), 255u);
    CHECK_EQ_UINT(at(&c, 13, 8), 0u);
    CHECK_EQ_UINT(at(&c, 8, 10), 0u);
    free(c.a);
    st.cap = CAP_SQUARE;
    sink_init(&c, &s, 16, 16, true);
    must(sv_stroke_path(&p, ts_identity(), &st, true, 16, 16, &s));
    CHECK_EQ_UINT(at(&c, 13, 8), 255u);
    CHECK_EQ_UINT(at(&c, 2, 6), 255u);
    free(c.a);
    st.cap = CAP_ROUND;
    sink_init(&c, &s, 16, 16, true);
    must(sv_stroke_path(&p, ts_identity(), &st, true, 16, 16, &s));
    CHECK(at(&c, 13, 8) > 0 && at(&c, 13, 8) < 255);
    CHECK(at(&c, 13, 6) < at(&c, 13, 8)); /* the round cap's corner is only partly covered */
    free(c.a);
    /* joins: a right angle; miter fills the outer corner, bevel cuts it */
    ts_pb_clear(&b);
    ts_pb_move_to(&b, 4, 12);
    ts_pb_line_to(&b, 4, 4);
    ts_pb_line_to(&b, 12, 4);
    CHECK(as_path(&b, &p));
    st = stroke_of(4.0f, CAP_BUTT, JOIN_MITER);
    sink_init(&c, &s, 16, 16, true);
    must(sv_stroke_path(&p, ts_identity(), &st, true, 16, 16, &s));
    CHECK_EQ_UINT(at(&c, 2, 2), 255u);
    free(c.a);
    st.join = JOIN_BEVEL;
    sink_init(&c, &s, 16, 16, true);
    must(sv_stroke_path(&p, ts_identity(), &st, true, 16, 16, &s));
    CHECK(at(&c, 2, 2) < 255);
    free(c.a);
    /* dashes: 2 on, 2 off along a horizontal line */
    {
        float dash[2] = {2.0f, 2.0f};
        ts_pb_clear(&b);
        ts_pb_move_to(&b, 0, 8);
        ts_pb_line_to(&b, 16, 8);
        CHECK(as_path(&b, &p));
        st = stroke_of(2.0f, CAP_BUTT, JOIN_MITER);
        st.dash = dash;
        st.ndash = 2;
        sink_init(&c, &s, 16, 16, true);
        must(sv_stroke_path(&p, ts_identity(), &st, true, 16, 16, &s));
        CHECK_EQ_UINT(at(&c, 0, 7), 255u);
        CHECK_EQ_UINT(at(&c, 1, 8), 255u);
        CHECK_EQ_UINT(at(&c, 2, 8), 0u);
        CHECK_EQ_UINT(at(&c, 4, 8), 255u);
        free(c.a);
    }
    /* hairlines: a half-pixel stroke is a one-pixel line at half coverage (in the paint) */
    CHECK(sv_hairline_coverage(0.5f, ts_identity()) == 0.5f);
    CHECK(sv_hairline_coverage(0.5f, ts_scale(3.0f, 3.0f)) < 0.0f);
    CHECK(sv_hairline_coverage(0.0f, ts_scale(30.0f, 3.0f)) == 1.0f);
    ts_pb_clear(&b);
    ts_pb_move_to(&b, 2, 5.5f);
    ts_pb_line_to(&b, 14, 5.5f);
    CHECK(as_path(&b, &p));
    st = stroke_of(0.5f, CAP_BUTT, JOIN_MITER);
    sink_init(&c, &s, 16, 16, true);
    must(sv_stroke_path(&p, ts_identity(), &st, true, 16, 16, &s));
    CHECK_EQ_UINT(at(&c, 8, 5), 255u);
    CHECK_EQ_UINT(at(&c, 8, 4), 0u);
    CHECK_EQ_UINT(at(&c, 8, 6), 0u);
    free(c.a);
    ts_pb_fini(&b);
}

static void test_raster_hostile(void)
{
    static const float BAD[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, 3e38f, 1e-30f};
    ts_pb b;
    sv_path p;
    cov_sink c;
    sv_spans s;
    sv_stroke st;
    size_t i;
    size_t k;
    pc_status r;
    sink_init(&c, &s, 64, 64, true);
    ts_pb_init(&b);
    /* non-finite and huge coordinates, transforms, widths and miter limits */
    for (i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        sv_ts tss[3];
        tss[0] = ts_identity();
        tss[1] = ts_scale(BAD[i], 2.0f);
        tss[2] = ts_row(1.0f, BAD[i], 0.5f, 1.0f, BAD[i], 3.0f);
        ts_pb_clear(&b);
        ts_pb_move_to(&b, 1, 1);
        ts_pb_line_to(&b, BAD[i], 30);
        ts_pb_quad_to(&b, 5, BAD[i], 40, 40);
        ts_pb_cubic_to(&b, BAD[i], 0, 10, BAD[i], 2, 60);
        ts_pb_close(&b);
        /* bounds computed by hand: the builder refuses non-finite paths */
        p.verbs = b.verbs;
        p.nverbs = b.nv;
        p.pts = b.pts;
        p.npts = b.np;
        p.bounds.l = 1.0f;
        p.bounds.t = 0.0f;
        p.bounds.r = BAD[i];
        p.bounds.b = 60.0f;
        for (k = 0; k < 3; k++) {
            sv_join j;
            (void)sv_fill_path(&p, tss[k], false, true, 64, 64, &s);
            (void)sv_fill_path(&p, tss[k], true, false, 64, 64, &s);
            for (j = JOIN_MITER; j <= JOIN_BEVEL; j++) {
                st = stroke_of(k == 0 ? 3.0f : fabsf(BAD[i]), (sv_cap)(j % 3), j);
                st.miterlimit = BAD[i];
                (void)sv_stroke_path(&p, tss[k], &st, true, 64, 64, &s);
                st.width = 0.5f;
                (void)sv_stroke_path(&p, tss[k], &st, j % 2 == 0, 64, 64, &s);
            }
        }
        (void)sv_fill_rect(p.bounds, tss[1], true, 64, 64, &s);
    }
    /* degenerate curves: all points equal, collinear controls, a cusp, with every cap */
    ts_pb_clear(&b);
    ts_pb_move_to(&b, 10, 10);
    ts_pb_quad_to(&b, 10, 10, 10, 10);
    ts_pb_cubic_to(&b, 10, 10, 10, 10, 10, 10);
    ts_pb_cubic_to(&b, 20, 10, 5, 10, 30, 10);
    ts_pb_cubic_to(&b, 50, 30, 10, 30, 50, 10);
    ts_pb_close(&b);
    ts_pb_close(&b);
    ts_pb_move_to(&b, 3, 3);
    CHECK(as_path(&b, &p));
    for (k = 0; k < 3; k++) {
        float dash[4] = {0.0f, 3.0f, 1e-6f, 2.0f};
        st = stroke_of(4.0f, (sv_cap)k, JOIN_ROUND);
        must(sv_stroke_path(&p, ts_identity(), &st, true, 64, 64, &s));
        st.dash = dash;
        st.ndash = 4;
        must(sv_stroke_path(&p, ts_identity(), &st, true, 64, 64, &s));
    }
    free(c.a);
    /* a million tiny dashes give up (the reference's dash-count limit); big targets are tiled */
    {
        size_t n = 1000000;
        float *dash = malloc(n * sizeof *dash);
        CHECK(dash != NULL);
        if (dash != NULL) {
            for (i = 0; i < n; i++) {
                dash[i] = 1e-6f;
            }
            st = stroke_of(2.0f, CAP_ROUND, JOIN_ROUND);
            st.dash = dash;
            st.ndash = n;
            sink_init(&c, &s, 0, 0, false);
            must(sv_stroke_path(&p, ts_identity(), &st, true, 64, 64, &s));
            free(dash);
        }
        ts_pb_clear(&b);
        ts_pb_push_circle(&b, 8200.0f, 30.0f, 20.0f);
        CHECK(as_path(&b, &p));
        sink_init(&c, &s, 0, 0, false);
        must(sv_fill_path(&p, ts_identity(), false, true, 16384, 64, &s));
        CHECK(c.pixels > 1000);
        st = stroke_of(0.0f, CAP_SQUARE, JOIN_MITER);
        must(sv_stroke_path(&p, ts_identity(), &st, true, 16384, 64, &s));
        must(sv_fill_path(&p, ts_scale(1.0f, 400.0f), false, true, 64, 1u << 22, &s));
    }
    /* a ten-million-point path: refused or drawn within the caps, in bounded time */
    {
        size_t n = 10000000;
        ts_pb_clear(&b);
        ts_pb_move_to(&b, 0, 0);
        for (i = 1; i < n && !b.failed; i++) {
            ts_pb_line_to(&b, (float)(i % 61), (float)((i * 7) % 59));
        }
        if (as_path(&b, &p)) {
            sink_init(&c, &s, 0, 0, false);
            r = sv_fill_path(&p, ts_identity(), false, true, 64, 64, &s);
            CHECK(r == PC_OK || r == PC_ERR_LIMIT);
            st = stroke_of(2.0f, CAP_ROUND, JOIN_ROUND);
            r = sv_stroke_path(&p, ts_identity(), &st, true, 64, 64, &s);
            CHECK(r == PC_OK || r == PC_ERR_LIMIT);
            st.width = 0.5f;
            r = sv_stroke_path(&p, ts_identity(), &st, true, 64, 64, &s);
            CHECK(r == PC_OK || r == PC_ERR_LIMIT);
        }
    }
    ts_pb_fini(&b);
}

int main(int argc, char **argv)
{
    RUN(test_basic_shapes);
    RUN(test_groups);
    RUN(test_transforms_curves_fill_rules);
    RUN(test_gradients);
    RUN(test_rasterised_fallbacks);
    RUN(test_embedded_images);
    RUN(test_text);
    RUN(test_text_hostile);
    RUN(test_text_threads);
    RUN(test_size_and_shrink);
    RUN(test_sniff_and_malformed);
    RUN(test_rasterize);
    RUN(test_pcraft_round_trip);
    RUN(test_local_files_never_read);
    RUN(test_sniffing);
    RUN(test_quads_become_cubics);
    RUN(test_tables_sorted);
    RUN(test_markers);
    RUN(test_nested_images);
    RUN(test_hostile);
    RUN(test_raster_coverage);
    RUN(test_raster_hostile);
    RUN(test_filter_conversion);
    RUN(test_filter_hostile);
    RUN(test_filter_threads);
    return pc_t_finish();
}
