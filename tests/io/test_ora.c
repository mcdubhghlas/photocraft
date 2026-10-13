/* OpenRaster (.ora) import runtime tests (port of crates/io/src/ora/tests.rs, R10c): a document
 * round-trips through export and import keeping its layers, groups, blend modes, opacity,
 * visibility and locks; other applications' stacks (Krita/MyPaint/GIMP compositing ids, auto
 * isolation, fractional offsets, unknown elements) import with the right blends and warnings; and
 * malformed, truncated, oversized or non-ORA archives fail without a crash. The structural
 * assertions are the values ora/tests.rs asserts on the Rust side, so this is parity with the
 * reference. Synthetic archives are built with the format/zip.h writer and PNGs encoded by the
 * codecs. */
#include "harness.h"

#include "common.h"

#include "codecs/codecs.h"
#include "format/zip.h"
#include "io/ora.h"

#define ORA_MIME "image/openraster"

/* ---- synthetic archives ---- */

/* A solid w x h RGBA8 PNG. */
static pc_buf solid_png(uint32_t w, uint32_t h, const uint8_t rgba[4])
{
    size_t n = (size_t)w * h * 4;
    uint8_t *data = xmalloc(n);
    pc_cimage img;
    pc_buf out = PC_BUF_INIT;
    size_t i;
    for (i = 0; i < n; i += 4) {
        memcpy(data + i, rgba, 4);
    }
    must(pc_cimage_from_copy(w, h, PC_CL_RGBA, PC_CS_U8, data, n, &img, NULL));
    free(data);
    must(pc_cencode(&img, PC_CF_PNG, NULL, &out, NULL));
    pc_cimage_fini(&img);
    return out;
}

/* A w x h 16-bit grayscale PNG (sample i -> (i * 1000) as u16). */
static pc_buf gray16_png(uint32_t w, uint32_t h)
{
    size_t n = (size_t)w * h;
    uint16_t *data = xmalloc(n * 2);
    pc_cimage img;
    pc_buf out = PC_BUF_INIT;
    size_t i;
    for (i = 0; i < n; i++) {
        data[i] = (uint16_t)(i * 1000);
    }
    must(pc_cimage_from_copy(w, h, PC_CL_GRAY, PC_CS_U16, data, n * 2, &img, NULL));
    free(data);
    must(pc_cencode(&img, PC_CF_PNG, NULL, &out, NULL));
    pc_cimage_fini(&img);
    return out;
}

typedef struct named_png {
    const char *name;
    pc_buf data;
} named_png;

/* An ORA archive from a stack.xml and named PNG entries (mimetype first and stored). */
static pc_buf ora_archive(const char *stack, const named_png *images, size_t nimages)
{
    pc_zip_writer z;
    pc_buf out = PC_BUF_INIT;
    size_t i;
    pc_zip_writer_init(&z);
    must(pc_zip_writer_add(&z, "mimetype", 8, (const uint8_t *)ORA_MIME, strlen(ORA_MIME), NULL));
    must(pc_zip_writer_add(&z, "stack.xml", 9, (const uint8_t *)stack, strlen(stack), NULL));
    for (i = 0; i < nimages; i++) {
        must(pc_zip_writer_add(&z, images[i].name, strlen(images[i].name), images[i].data.data,
                               images[i].data.len, NULL));
    }
    must(pc_zip_writer_finish(&z, &out, NULL));
    pc_zip_writer_fini(&z);
    return out;
}

/* ---- round-trip helpers (export then import, compare structure and pixels) ---- */

/* A raster layer holding a w x h ramp at (x, y). Alpha is 0.25 + 0.75*v so no pixel is fully
 * transparent (the Rust block()): straight-alpha PNG round trips then compare bit-exactly. */
static pc_layer ora_block(const char *name, pc_pixel_format fmt, int x, int y, uint32_t w, uint32_t h,
                          uint32_t seed)
{
    size_t ch = pc_pixel_format_channels(fmt);
    size_t count = (size_t)w * h * ch;
    float *vals = xmalloc(count * sizeof *vals);
    pc_surface s;
    pc_layer l;
    uint32_t i;
    uint32_t j;
    size_t k = 0;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            size_t c;
            for (c = 0; c < ch; c++) {
                float v = (float)((i * 7 + j * 13 + (uint32_t)c * 29 + seed * 31) % 256) / 255.0f;
                vals[k++] = (c == ch - 1 && fmt.alpha) ? 0.25f + 0.75f * v : v;
            }
        }
    }
    pc_surface_init(&s, fmt);
    must(pc_surface_write_region(&s, pc_rect_make(x, y, x + (int)w, y + (int)h), vals, count, NULL));
    pc_surface_prune(&s);
    free(vals);
    must(pc_io_raster_layer(name, &s, &l, NULL));
    pc_surface_fini(&s);
    return l;
}

/* The Rust test's doc(): five top layers, two of them groups, with varied blends and locks. */
static pc_document ora_doc(pc_sample_type depth)
{
    pc_pixel_format fmt = {PC_COLOR_MODE_RGB, depth, true};
    pc_document d = t_doc("t.ora", 70, 50, PC_COLOR_MODE_RGB, depth);
    pc_layer base = ora_block("Base", fmt, 0, 0, 70, 50, 1);
    pc_layer off = ora_block("Off canvas", fmt, -12, 30, 40, 30, 2);
    pc_layer hidden = ora_block("Hidden", fmt, 10, 10, 5, 5, 3);
    pc_layer group;
    pc_layer through;
    pc_layer *iso_kids = xmalloc(2 * sizeof(pc_layer));
    pc_layer *thru_kids = xmalloc(1 * sizeof(pc_layer));
    base.locks.transparency = true;
    off.blend = PC_BLEND_MULTIPLY;
    off.opacity = 0.5f;
    hidden.visible = false;
    iso_kids[0] = ora_block("Inner screen", fmt, 20, 5, 30, 20, 4);
    iso_kids[0].blend = PC_BLEND_SCREEN;
    iso_kids[1] = ora_block("Inner plain", fmt, 5, 5, 8, 8, 5);
    must(pc_layer_group(&group, "Isolated", iso_kids, 2, NULL));
    group.blend = PC_BLEND_OVERLAY;
    group.opacity = 0.75f;
    thru_kids[0] = ora_block("Through child", fmt, 40, 20, 25, 25, 6);
    thru_kids[0].blend = PC_BLEND_LINEAR_LIGHT;
    thru_kids[0].locks.all = true;
    must(pc_layer_group(&through, "Pass <&> \"through\"", thru_kids, 1, NULL));
    t_push(&d, &base);
    t_push(&d, &off);
    t_push(&d, &hidden);
    t_push(&d, &group);
    t_push(&d, &through);
    return d;
}

static void assert_same(const pc_layer *a, size_t na, const pc_layer *b, size_t nb);

static void assert_layer_same(const pc_layer *a, const pc_layer *b)
{
    const pc_surface *sa = pc_layer_surface(a);
    const pc_surface *sb = pc_layer_surface(b);
    size_t nca = 0;
    size_t ncb = 0;
    const pc_layer *ca = pc_layer_children(a, &nca);
    const pc_layer *cb = pc_layer_children(b, &ncb);
    CHECK(pc_str_eq_c(&a->name, pc_str_c(&b->name)));
    CHECK_EQ_INT(a->visible, b->visible);
    CHECK_EQ_INT(a->blend, b->blend);
    CHECK_NEAR(a->opacity, b->opacity, 1e-6f);
    CHECK_EQ_INT(a->locks.all, b->locks.all);
    CHECK_EQ_INT(a->locks.transparency, b->locks.transparency);
    if (ca != NULL || cb != NULL) {
        assert_same(ca, nca, cb, ncb);
    } else if (sa != NULL && sb != NULL) {
        pc_rect ra = pc_surface_content_bounds(sa);
        pc_rect rb = pc_surface_content_bounds(sb);
        CHECK(pc_rect_eq(ra, rb));
        if (!pc_rect_is_empty(ra) && pc_rect_eq(ra, rb)) {
            uint8_t *pa = NULL;
            uint8_t *pb = NULL;
            size_t la = 0;
            size_t lb = 0;
            must(pc_surface_to_interleaved(sa, ra, &pa, &la, NULL));
            must(pc_surface_to_interleaved(sb, rb, &pb, &lb, NULL));
            CHECK_EQ_UINT(la, lb);
            if (la == lb) {
                CHECK_EQ_MEM(pa, pb, la);
            }
            free(pa);
            free(pb);
        }
    } else {
        CHECK(sa == NULL && sb == NULL);
    }
}

static void assert_same(const pc_layer *a, size_t na, const pc_layer *b, size_t nb)
{
    size_t i;
    CHECK_EQ_UINT(na, nb);
    if (na != nb) {
        return;
    }
    for (i = 0; i < na; i++) {
        assert_layer_same(&a[i], &b[i]);
    }
}

static void test_round_trip_layers_groups(void)
{
    pc_sample_type depths[2] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16};
    size_t di;
    for (di = 0; di < 2; di++) {
        pc_document d = ora_doc(depths[di]);
        pc_io_export_result ex;
        pc_io_import_result back;
        must(pc_io_ora_export(&d, &ex, NULL));
        CHECK_EQ_UINT(ex.warnings.n, 0);
        must(pc_io_import("back.ora", ex.bytes, ex.len, &back, NULL));
        CHECK_EQ_UINT(back.warnings.n, 0);
        CHECK_EQ_UINT(back.document.size.width, d.size.width);
        CHECK_EQ_UINT(back.document.size.height, d.size.height);
        CHECK_EQ_INT(back.document.depth, depths[di]);
        assert_same(d.layers, d.nlayers, back.document.layers, back.document.nlayers);
        pc_io_import_result_fini(&back);
        pc_io_export_result_fini(&ex);
        pc_document_fini(&d);
    }
}

static void test_archive_layout(void)
{
    pc_document d = ora_doc(PC_SAMPLE_TYPE_U8);
    pc_io_export_result ex;
    pc_zip_reader zip;
    static const char *const entries[] = {"stack.xml", "mergedimage.png", "Thumbnails/thumbnail.png"};
    size_t i;
    must(pc_io_ora_export(&d, &ex, NULL));
    /* A stored `mimetype` first, readable at a fixed offset. */
    CHECK(ex.len >= 54);
    CHECK_EQ_MEM(ex.bytes + 30, "mimetype", 8);
    CHECK_EQ_MEM(ex.bytes + 38, ORA_MIME, 16);
    CHECK(pc_io_is_ora(ex.bytes, ex.len));
    must(pc_zip_reader_open(&zip, ex.bytes, ex.len, NULL));
    for (i = 0; i < 3; i++) {
        CHECK(pc_zip_find(&zip, entries[i], strlen(entries[i])) != NULL);
    }
    {
        pc_buf merged = PC_BUF_INIT;
        pc_cimage img;
        must(pc_zip_read_by_name(&zip, "mergedimage.png", 1u << 20, &merged, NULL));
        must(pc_cdecode(merged.data, merged.len, &img, NULL));
        CHECK_EQ_UINT(img.width, 70);
        CHECK_EQ_UINT(img.height, 50);
        pc_cimage_fini(&img);
        pc_buf_fini(&merged);
    }
    {
        pc_buf xml = PC_BUF_INIT;
        must(pc_zip_read_by_name(&zip, "stack.xml", 1u << 20, &xml, NULL));
        must(pc_buf_append(&xml, "\0", 1, NULL));
        CHECK_CONTAINS((const char *)xml.data, "name=\"Pass &lt;&amp;&gt; &quot;through&quot;\"");
        CHECK_CONTAINS((const char *)xml.data, "isolation=\"auto\"");
        CHECK_CONTAINS((const char *)xml.data, "composite-op=\"krita:linear light\"");
        pc_buf_fini(&xml);
    }
    pc_zip_reader_close(&zip);
    pc_io_export_result_fini(&ex);
    pc_document_fini(&d);
}

/* ---- import of other applications' stacks ---- */

static const pc_layer *find_top(const pc_document *d, const char *name)
{
    size_t i;
    for (i = 0; i < d->nlayers; i++) {
        if (pc_str_eq_c(&d->layers[i].name, name)) {
            return &d->layers[i];
        }
    }
    return NULL;
}

static void test_reads_other_applications_stacks(void)
{
    static const char xml[] =
        "<?xml version='1.0' encoding='UTF-8'?>\n"
        "<image version=\"0.0.3\" w=\"8\" h=\"6\" xres=\"150\" yres=\"150\">\n"
        " <stack opacity=\"1\" name=\"root\" isolation=\"isolate\" composite-op=\"svg:src-over\">\n"
        "  <layer name=\"Plus\" src=\"data/a.png\" x=\"2.4\" y=\"1\" composite-op=\"svg:plus\" "
        "opacity=\"0.5\" selected=\"true\"/>\n"
        "  <stack name=\"Folder\" isolation=\"auto\" composite-op=\"krita:darker color\">\n"
        "   <layer name=\"Inside\" src=\"data/b.png\" composite-op=\"krita:hard mix\" "
        "visibility=\"hidden\" alpha-preserve=\"true\"/>\n"
        "  </stack>\n"
        "  <stack name=\"Through\"><layer src=\"data/b.png\"/></stack>\n"
        "  <text name=\"Caption\">hello</text>\n"
        "  <layer name=\"Gray16\" src=\"data/g.png\" composite-op=\"svg:soft-light\" edit-locked=\"true\"/>\n"
        "  <layer name=\"Strange\" src=\"data/a.png\" composite-op=\"svg:dst-in\"/>\n"
        "  <layer name=\"Lost\" src=\"data/missing.png\"/>\n"
        " </stack>\n"
        "</image>";
    uint8_t red[4] = {255, 0, 0, 255};
    uint8_t blue[4] = {0, 0, 255, 128};
    named_png imgs[3] = {{"data/a.png", solid_png(3, 2, red)},
                         {"data/b.png", solid_png(8, 6, blue)},
                         {"data/g.png", gray16_png(4, 4)}};
    pc_buf bytes = ora_archive(xml, imgs, 3);
    pc_io_import_result r;
    const pc_layer *plus;
    const pc_layer *folder;
    const pc_layer *gray;
    static const char *const order[6] = {"Lost", "Strange", "Gray16", "Through", "Folder", "Plus"};
    size_t i;
    /* Detected by content, whatever the name says. */
    must(pc_io_import("no extension", bytes.data, bytes.len, &r, NULL));
    CHECK_EQ_UINT(r.document.size.width, 8);
    CHECK_EQ_UINT(r.document.size.height, 6);
    CHECK_EQ_INT(r.document.depth, PC_SAMPLE_TYPE_U16);
    CHECK_NEAR(r.document.resolution_dpi, 150.0f, 1e-3f);
    CHECK_EQ_UINT(r.document.nlayers, 6);
    for (i = 0; i < 6 && i < r.document.nlayers; i++) {
        CHECK(pc_str_eq_c(&r.document.layers[i].name, order[i]));
    }
    plus = find_top(&r.document, "Plus");
    CHECK(plus != NULL);
    if (plus != NULL) {
        const pc_surface *s = pc_layer_surface(plus);
        CHECK_EQ_INT(plus->blend, PC_BLEND_LINEAR_DODGE);
        CHECK_NEAR(plus->opacity, 0.5f, 1e-6f);
        CHECK(s != NULL);
        if (s != NULL) {
            CHECK(pc_rect_eq(pc_surface_content_bounds(s), pc_rect_make(2, 1, 5, 3)));
        }
    }
    folder = find_top(&r.document, "Folder");
    CHECK(folder != NULL);
    if (folder != NULL) {
        size_t nc = 0;
        const pc_layer *kids = pc_layer_children(folder, &nc);
        CHECK_EQ_INT(folder->blend, PC_BLEND_DARKER_COLOR); /* a blend isolates an auto group */
        CHECK(kids != NULL && nc >= 1);
        if (kids != NULL && nc >= 1) {
            CHECK(!kids[0].visible);
            CHECK(kids[0].locks.transparency);
            CHECK_EQ_INT(kids[0].blend, PC_BLEND_HARD_MIX);
        }
    }
    CHECK(find_top(&r.document, "Through") != NULL);
    if (find_top(&r.document, "Through") != NULL) {
        CHECK_EQ_INT(find_top(&r.document, "Through")->blend, PC_BLEND_PASS_THROUGH);
    }
    gray = find_top(&r.document, "Gray16");
    CHECK(gray != NULL);
    if (gray != NULL) {
        const pc_surface *s = pc_layer_surface(gray);
        pc_pixel_format f;
        CHECK(gray->locks.all);
        CHECK_EQ_INT(gray->blend, PC_BLEND_SOFT_LIGHT);
        CHECK(s != NULL);
        if (s != NULL) {
            f = pc_surface_format(s);
            CHECK_EQ_INT(f.mode, PC_COLOR_MODE_RGB);
            CHECK_EQ_INT(f.sample, PC_SAMPLE_TYPE_U16);
            CHECK(f.alpha);
        }
    }
    CHECK(find_top(&r.document, "Strange") != NULL);
    if (find_top(&r.document, "Strange") != NULL) {
        CHECK_EQ_INT(find_top(&r.document, "Strange")->blend, PC_BLEND_NORMAL);
    }
    CHECK(find_top(&r.document, "Lost") != NULL);
    if (find_top(&r.document, "Lost") != NULL) {
        const pc_surface *s = pc_layer_surface(find_top(&r.document, "Lost"));
        CHECK(s != NULL && pc_rect_is_empty(pc_surface_content_bounds(s)));
    }
    CHECK(pc_io_warnings_any(&r.warnings, "svg:dst-in"));
    CHECK(pc_io_warnings_any(&r.warnings, "<text>"));
    CHECK(pc_io_warnings_any(&r.warnings, "missing from the archive"));
    CHECK(pc_io_warnings_any(&r.warnings, "krita:hard mix"));
    CHECK(pc_io_warnings_any(&r.warnings, "svg:soft-light"));
    pc_io_import_result_fini(&r);
    for (i = 0; i < 3; i++) {
        pc_buf_fini(&imgs[i].data);
    }
    pc_buf_fini(&bytes);
}

static void test_preserves_whitespace_names(void)
{
    static const char xml[] =
        "<image w=\"2\" h=\"2\"><stack>\n"
        "      <stack name=\" Folder \"><layer name=\" Pixel \" src=\"a.png\"/></stack>\n"
        "      <layer name=\"  \" src=\"a.png\"/>\n"
        "    </stack></image>";
    uint8_t red[4] = {255, 0, 0, 255};
    named_png imgs[1] = {{"a.png", solid_png(2, 2, red)}};
    pc_buf bytes = ora_archive(xml, imgs, 1);
    pc_io_import_result r;
    const pc_layer *folder;
    bool blank = false;
    size_t i;
    must(pc_io_import("names.ora", bytes.data, bytes.len, &r, NULL));
    folder = find_top(&r.document, " Folder ");
    CHECK(folder != NULL); /* group name is preserved */
    if (folder != NULL) {
        size_t nc = 0;
        const pc_layer *kids = pc_layer_children(folder, &nc);
        CHECK(kids != NULL && nc >= 1 && pc_str_eq_c(&kids[0].name, " Pixel "));
    }
    for (i = 0; i < r.document.nlayers; i++) {
        if (pc_str_eq_c(&r.document.layers[i].name, "Layer 3")) {
            blank = true;
        }
    }
    CHECK(blank); /* blank names keep the fallback */
    pc_io_import_result_fini(&r);
    pc_buf_fini(&imgs[0].data);
    pc_buf_fini(&bytes);
}

/* ---- grayscale / CMYK ---- */

static void test_grayscale_rgb_and_cmyk_refused(void)
{
    pc_document g = t_doc("g.ora", 70, 50, PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8);
    pc_document c = t_doc("c.ora", 70, 50, PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8);
    pc_layer gray = t_raster("Gray", (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, true},
                             pc_rect_make(0, 0, 10, 10), 0, true);
    pc_io_export_result ex;
    pc_io_import_result back;
    pc_error e;
    t_push(&g, &gray);
    must(pc_io_ora_export(&g, &ex, NULL));
    CHECK(pc_io_warnings_any(&ex.warnings, "grayscale"));
    must(pc_io_import("g.ora", ex.bytes, ex.len, &back, NULL));
    CHECK_EQ_INT(back.document.mode, PC_COLOR_MODE_RGB);
    /* A CMYK document cannot be saved as OpenRaster. */
    CHECK(pc_io_ora_export(&c, &ex, &e) != PC_OK);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&ex);
    pc_document_fini(&g);
    pc_document_fini(&c);
}

/* ---- never-crash ---- */

static pc_status try_import(const char *name, const uint8_t *b, size_t n)
{
    pc_io_import_result r;
    pc_error e;
    pc_status st = pc_io_import(name, b, n, &r, &e);
    if (st == PC_OK) {
        pc_io_import_result_fini(&r);
    } else {
        CHECK(e.msg[0] != '\0');
    }
    return st;
}

static void test_malformed_archives_fail_cleanly(void)
{
    uint8_t c123[4] = {1, 2, 3, 4};
    pc_buf png = solid_png(2, 2, c123);
    named_png a[1] = {{"a.png", png}};
    /* Each case must error (never crash). */
    static const char *const xmls[] = {
        "<image w='2' h='2'><stack>",                                                /* bad xml */
        "<picture w='2' h='2'><stack/></picture>",                                   /* wrong root */
        "<image w='2' h='2'/>",                                                      /* no root stack */
        "<image w='0' h='2'><stack/></image>",                                       /* zero width */
        "<image w='2.5' h='2'><stack/></image>",                                     /* fractional */
        "<image w='300000' h='300000'><stack/></image>",                            /* huge canvas */
        "<image w='2' h='2'><stack><layer src='a.png' opacity='lots'/></stack></image>", /* bad opacity */
        "<image w='2' h='2'><stack><layer src='a.png' opacity='NaN'/></stack></image>",  /* nan opacity */
        "<image w='2' h='2'><stack><layer src='a.png' x='99999999999'/></stack></image>", /* far offset */
    };
    size_t i;
    for (i = 0; i < sizeof xmls / sizeof xmls[0]; i++) {
        pc_buf bytes = ora_archive(xmls[i], a, 1);
        CHECK(try_import("x.ora", bytes.data, bytes.len) != PC_OK);
        pc_buf_fini(&bytes);
    }
    /* Not a zip. */
    CHECK(try_import("x.ora", (const uint8_t *)"definitely not a zip archive at all", 35) != PC_OK);
    /* A valid-looking stack whose image data is garbage. */
    {
        uint8_t junk[7] = {'g', 'a', 'r', 'b', 'a', 'g', 'e'};
        named_png bad[1] = {{"a.png", PC_BUF_INIT}};
        pc_buf bytes;
        must(pc_buf_append(&bad[0].data, junk, 7, NULL));
        bytes = ora_archive("<image w='2' h='2'><stack><layer src='a.png'/></stack></image>", bad, 1);
        CHECK(try_import("x.ora", bytes.data, bytes.len) != PC_OK);
        pc_buf_fini(&bad[0].data);
        pc_buf_fini(&bytes);
    }
    /* An archive with a mimetype but no stack.xml. */
    {
        pc_zip_writer z;
        pc_buf out = PC_BUF_INIT;
        pc_zip_writer_init(&z);
        must(pc_zip_writer_add(&z, "mimetype", 8, (const uint8_t *)ORA_MIME, strlen(ORA_MIME), NULL));
        must(pc_zip_writer_finish(&z, &out, NULL));
        pc_zip_writer_fini(&z);
        CHECK(try_import("x.ora", out.data, out.len) != PC_OK);
        pc_buf_fini(&out);
    }
    /* Deeply nested groups are refused (not overflowed). */
    {
        pc_buf deep = PC_BUF_INIT;
        pc_buf bytes;
        int k;
        must(pc_buf_append(&deep, "<image w='2' h='2'><stack>", 26, NULL));
        for (k = 0; k < 200; k++) {
            must(pc_buf_append(&deep, "<stack>", 7, NULL));
        }
        for (k = 0; k < 200; k++) {
            must(pc_buf_append(&deep, "</stack>", 8, NULL));
        }
        must(pc_buf_append(&deep, "</stack></image>\0", 17, NULL));
        bytes = ora_archive((const char *)deep.data, a, 1);
        CHECK(try_import("x.ora", bytes.data, bytes.len) != PC_OK);
        pc_buf_fini(&deep);
        pc_buf_fini(&bytes);
    }
    pc_buf_fini(&png);
}

static void test_layer_and_pixel_budgets(void)
{
    /* More than ORA_MAX_NODES (8192) nodes is refused. */
    pc_buf many = PC_BUF_INIT;
    pc_buf bytes;
    int k;
    must(pc_buf_append(&many, "<image w='2' h='2'><stack>", 26, NULL));
    for (k = 0; k < 8193; k++) {
        must(pc_buf_append(&many, "<stack/>", 8, NULL));
    }
    must(pc_buf_append(&many, "</stack></image>\0", 17, NULL));
    bytes = ora_archive((const char *)many.data, NULL, 0);
    CHECK(try_import("x.ora", bytes.data, bytes.len) != PC_OK);
    pc_buf_fini(&many);
    pc_buf_fini(&bytes);
    /* A tiny PNG forged to a vast declared size is refused by the decoder limits, not allocated. */
    {
        uint8_t zero[4] = {0, 0, 0, 0};
        pc_buf forged = solid_png(1, 1, zero);
        named_png img[1] = {{"a.png", forged}};
        pc_buf arc;
        if (forged.len >= 24) {
            static const uint8_t ihdr[8] = {0, 0, 0x75, 0x30, 0, 0, 0x75, 0x30};
            memcpy(forged.data + 16, ihdr, 8);
        }
        arc = ora_archive("<image w='2' h='2'><stack><layer src='a.png'/></stack></image>", img, 1);
        CHECK(try_import("x.ora", arc.data, arc.len) != PC_OK);
        pc_buf_fini(&forged);
        pc_buf_fini(&arc);
    }
}

static void test_truncated_and_corrupted_never_panic(void)
{
    pc_document d = ora_doc(PC_SAMPLE_TYPE_U8);
    pc_io_export_result ex;
    size_t step;
    size_t cut;
    uint32_t state = 0x9e3779b9u;
    int iter;
    uint8_t *c;
    must(pc_io_ora_export(&d, &ex, NULL));
    step = ex.len / 300 > 0 ? ex.len / 300 : 1;
    for (cut = 0; cut < ex.len; cut += step) {
        (void)try_import("x.ora", ex.bytes, cut);
    }
    c = xmalloc(ex.len);
    for (iter = 0; iter < 300; iter++) {
        int f;
        memcpy(c, ex.bytes, ex.len);
        for (f = 0; f < 4; f++) {
            size_t i;
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            i = (size_t)state % ex.len;
            c[i] ^= (uint8_t)((state >> 24) | 1u);
        }
        (void)try_import("x.ora", c, ex.len);
    }
    free(c);
    pc_io_export_result_fini(&ex);
    pc_document_fini(&d);
}

static void test_zip_mimetype_leading_stored_entry(void)
{
    size_t ml = 999;
    const uint8_t *m;
    pc_buf ok = ora_archive("<image/>", NULL, 0);
    m = pc_io_zip_mimetype(ok.data, ok.len, &ml);
    CHECK(m != NULL && ml == strlen(ORA_MIME) && memcmp(m, ORA_MIME, ml) == 0);
    CHECK(pc_io_zip_mimetype((const uint8_t *)"PK\x03\x04", 4, &ml) == NULL);
    CHECK(pc_io_zip_mimetype((const uint8_t *)"", 0, &ml) == NULL);
    /* Regression: a local header whose name_len (143) exceeds "mimetype" must be rejected by the
     * length check before the compare (an earlier version read past the 8-byte literal). */
    {
        uint8_t h[200];
        memset(h, 0, sizeof h);
        h[0] = 'P';
        h[1] = 'K';
        h[2] = 3;
        h[3] = 4;
        /* method 0 at offset 8; declared size 8 at offset 18; name_len 143 at offset 26. */
        h[26] = 143;
        memcpy(h + 30, "mimetype", 8);
        CHECK(pc_io_zip_mimetype(h, sizeof h, &ml) == NULL);
    }
    /* mimetype not first: not recognised. */
    {
        pc_zip_writer z;
        pc_buf out = PC_BUF_INIT;
        pc_zip_writer_init(&z);
        must(pc_zip_writer_add(&z, "stack.xml", 9, (const uint8_t *)"x", 1, NULL));
        must(pc_zip_writer_add(&z, "mimetype", 8, (const uint8_t *)ORA_MIME, strlen(ORA_MIME), NULL));
        must(pc_zip_writer_finish(&z, &out, NULL));
        pc_zip_writer_fini(&z);
        CHECK(pc_io_zip_mimetype(out.data, out.len, &ml) == NULL);
        pc_buf_fini(&out);
    }
    pc_buf_fini(&ok);
}

int main(int argc, char **argv)
{
    RUN(test_round_trip_layers_groups);
    RUN(test_archive_layout);
    RUN(test_reads_other_applications_stacks);
    RUN(test_preserves_whitespace_names);
    RUN(test_grayscale_rgb_and_cmyk_refused);
    RUN(test_malformed_archives_fail_cleanly);
    RUN(test_layer_and_pixel_budgets);
    RUN(test_truncated_and_corrupted_never_panic);
    RUN(test_zip_mimetype_leading_stored_entry);
    return pc_t_finish();
}
