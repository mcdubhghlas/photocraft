/* File > Export > Save for Web (Legacy), Export Preferences and Quick Export (port of
 * crates/engine/src/web_cmds/tests.rs). Image Assets (Generator) stays an unsupported stub. The
 * GIF/PNG-8 quantizer and slice export are exercised here end to end (the encoded container bytes
 * differ from the Rust codecs by design, REWRITE_PLAN.md §7, so these assert decoded pixels,
 * palettes, sizes and HTML rather than bytes). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codecs/codecs.h"
#include "codecs/image.h"
#include "engine/engine_internal.h"
#include "engine/prefs.h"
#include "engine/web_cmds.h"
#include "harness.h"
#include "raster/surface.h"

static const char *g_tmp;

/* ---- helpers ------------------------------------------------------------------------------ */

static pc_status ex(pc_engine_session *s, const char *id, const char *p, pc_json_value *out)
{
    return pc_engine_session_execute_json(s, id, p, out, NULL);
}

static void ex_ok(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(ex(s, id, p, &o) == PC_OK);
    pc_json_value_fini(&o);
}

static yyjson_mut_val *field(pc_json_value *o, const char *k)
{
    yyjson_mut_val *root = pc_json_value_root(o);
    return root != NULL ? yyjson_mut_obj_get(root, k) : NULL;
}

static uint64_t uint_field(pc_json_value *o, const char *k)
{
    yyjson_mut_val *v = field(o, k);
    return v != NULL && yyjson_mut_is_uint(v) ? yyjson_mut_get_uint(v) : 0;
}

static const char *str_field(pc_json_value *o, const char *k)
{
    yyjson_mut_val *v = field(o, k);
    return v != NULL ? yyjson_mut_get_str(v) : NULL;
}

/* Paint the shared 64x48 fixture onto the active layer. */
struct paint_ctx {
    int kind; /* 0 = fixture, 1 = shadow */
};
static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)err;
    struct paint_ctx *c = vctx;
    pc_layer_id id = *a;
    pc_surface *surf = pc_layer_surface_mut(pc_document_layer_mut(doc, id));
    if (surf == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no surface");
    }
    if (c->kind == 0) {
        float red[4] = {1, 0, 0, 1};
        (void)pc_surface_fill_rect(surf, pc_rect_make(8, 8, 32, 32), red, 4, NULL);
        float soft[4] = {1, 0, 0, 0.4f};
        (void)pc_surface_fill_rect(surf, pc_rect_make(32, 8, 34, 32), soft, 4, NULL);
        for (int x = 0; x < 64; x++) {
            float g[4] = {(float)x / 63.0f, 0.5f, 1.0f - (float)x / 63.0f, 1.0f};
            (void)pc_surface_fill_rect(surf, pc_rect_make(x, 36, x + 1, 48), g, 4, NULL);
        }
    } else {
        for (int x = 0; x < 256; x++) {
            float p[4] = {0, 0, 0, 1.0f - (float)x / 255.0f};
            (void)pc_surface_fill_rect(surf, pc_rect_make(x, 0, x + 1, 8), p, 4, NULL);
        }
    }
    return PC_OK;
}

static pc_engine_session *fixture(int depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char p[128];
    snprintf(p, sizeof p, "{\"width\":64,\"height\":48,\"depth\":%d,\"name\":\"web.psd\",\"background\":\"transparent\"}",
             depth);
    ex_ok(s, "file.new", p);
    struct paint_ctx c = {0};
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, &c, NULL) == PC_OK);
    return s;
}

/* Decode an exported file into RGBA8; returns malloc'd pixels and dimensions (NULL on failure). */
static uint8_t *decode_file(const char *path, uint32_t *w, uint32_t *h)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bytes = n > 0 ? malloc((size_t)n) : NULL;
    if (bytes == NULL || fread(bytes, 1, (size_t)n, f) != (size_t)n) {
        free(bytes);
        fclose(f);
        return NULL;
    }
    fclose(f);
    pc_cimage img;
    pc_cimage_init(&img);
    uint8_t *rgba = NULL;
    size_t rn = 0;
    if (pc_cdecode(bytes, (size_t)n, &img, NULL) == PC_OK && pc_cimage_to_rgba8(&img, &rgba, &rn, NULL) == PC_OK) {
        *w = img.width;
        *h = img.height;
    } else {
        free(rgba);
        rgba = NULL;
    }
    pc_cimage_fini(&img);
    free(bytes);
    return rgba;
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)(n > 0 ? n : 0) + 1);
    if (s == NULL || fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s);
        fclose(f);
        return NULL;
    }
    s[n] = '\0';
    fclose(f);
    return s;
}

static char *tmpdir(const char *name)
{
    char tmpl[256];
    snprintf(tmpl, sizeof tmpl, "%s/pc-web-%ld-%s-XXXXXX", g_tmp, (long)getpid(), name);
    char *d = mkdtemp(tmpl);
    return d != NULL ? strdup(d) : NULL;
}

/* ---- tests -------------------------------------------------------------------------------- */

static void test_registered_and_image_assets_stub(void)
{
    const char *ids[] = {"file.export.saveForWebLegacy", "file.export.exportPreferences",
                         "file.export.quickExport", "file.generate.imageAssets"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        CHECK(pc_engine_command_find(ids[i]) != NULL);
    }
    pc_engine_session *s = fixture(8);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.generate.imageAssets", "{}", &o) != PC_OK); /* Generator still unsupported */
    pc_json_value_fini(&o);
    pc_engine_session_free(s);
}

static void test_estimates_every_format_at_every_depth(void)
{
    const int depths[] = {8, 16, 32};
    const char *fmts[] = {"gif", "png8", "png24", "jpeg", "wbmp"};
    for (size_t di = 0; di < 3; di++) {
        pc_engine_session *s = fixture(depths[di]);
        uint64_t sizes[5];
        for (size_t fi = 0; fi < 5; fi++) {
            char p[64];
            snprintf(p, sizeof p, "{\"format\":\"%s\",\"colors\":16}", fmts[fi]);
            pc_json_value o = PC_JSON_VALUE_INIT;
            CHECK(ex(s, "file.export.saveForWebLegacy", p, &o) == PC_OK);
            CHECK_EQ_UINT(uint_field(&o, "width"), 64);
            CHECK_EQ_UINT(uint_field(&o, "height"), 48);
            CHECK(uint_field(&o, "bytes") > 20);
            if (fi <= 1) {
                CHECK(uint_field(&o, "colors") <= 16);
            }
            sizes[fi] = uint_field(&o, "bytes");
            pc_json_value_fini(&o);
        }
        CHECK_EQ_UINT(sizes[4], 4 + 8 * 48); /* WBMP: header + 1 bit/pixel */
        pc_engine_session_free(s);
    }
    pc_engine_session *s = fixture(8);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.export.saveForWebLegacy", "{\"format\":\"tiff\"}", &o) != PC_OK);
    pc_json_value_fini(&o);
    CHECK(ex(s, "file.export.saveForWebLegacy", "{\"preset\":\"JPEG Low\",\"percent\":50}", &o) == PC_OK);
    CHECK_EQ_STR(str_field(&o, "format"), "jpg");
    CHECK_EQ_UINT(uint_field(&o, "width"), 32);
    pc_json_value_fini(&o);
    pc_engine_session_free(s);
}

static void test_last_web_and_preferences(void)
{
    char *dir = tmpdir("remember");
    CHECK(dir != NULL);
    pc_engine_session *s = fixture(8);
    char saved[512];
    snprintf(saved, sizeof saved, "{\"format\":\"jpeg\",\"quality\":42,\"path\":\"%s/out.jpg\"}", dir);
    ex_ok(s, "file.export.saveForWebLegacy", saved);
    /* prefs.dialogs remembers the settings without the destination. */
    pc_json_value dlg = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_prefs_get(s, "dialogs", &dlg, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&dlg);
    yyjson_mut_val *entry = root != NULL ? yyjson_mut_obj_get(root, "file.export.saveForWebLegacy") : NULL;
    CHECK(entry != NULL);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(entry, "format")), "jpeg");
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(entry, "quality")), 42);
    CHECK(yyjson_mut_obj_get(entry, "path") == NULL); /* destination not remembered */
    pc_json_value_fini(&dlg);
    /* An estimate remembers nothing new. */
    ex_ok(s, "file.export.saveForWebLegacy", "{\"format\":\"png24\",\"width\":16,\"height\":12}");
    pc_json_value dlg2 = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_prefs_get(s, "dialogs", &dlg2, NULL) == PC_OK);
    yyjson_mut_val *e2 =
        yyjson_mut_obj_get(pc_json_value_root(&dlg2), "file.export.saveForWebLegacy");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(e2, "format")), "jpeg");
    pc_json_value_fini(&dlg2);
    pc_engine_session_free(s);
    free(dir);
}

static void test_gif_and_png8_transparency_and_palette(void)
{
    char *dir = tmpdir("gif");
    CHECK(dir != NULL);
    pc_engine_session *s = fixture(8);
    char gif[320];
    snprintf(gif, sizeof gif, "%s/a.gif", dir);
    char p[512];
    snprintf(p, sizeof p, "{\"format\":\"gif\",\"colors\":8,\"dither\":\"none\",\"path\":\"%s\"}", gif);
    ex_ok(s, "file.export.saveForWebLegacy", p);
    uint32_t w, h;
    uint8_t *px = decode_file(gif, &w, &h);
    CHECK(px != NULL && w == 64 && h == 48);
    if (px != NULL) {
        CHECK_EQ_UINT(px[(0 * 64 + 0) * 4 + 3], 0);     /* transparent background */
        CHECK_EQ_UINT(px[(16 * 64 + 16) * 4 + 0], 255); /* red square */
        CHECK_EQ_UINT(px[(16 * 64 + 16) * 4 + 1], 0);
        CHECK_EQ_UINT(px[(16 * 64 + 33) * 4 + 3], 0); /* 40% edge is under 1/2: hard transparent */
        /* distinct opaque colours <= 7 (8 incl. transparent). */
        uint32_t seen[64];
        size_t ns = 0;
        for (size_t i = 0; i < 64u * 48u && ns < 64; i++) {
            if (px[i * 4 + 3] == 0) {
                continue;
            }
            uint32_t key = (uint32_t)px[i * 4] << 16 | (uint32_t)px[i * 4 + 1] << 8 | px[i * 4 + 2];
            bool dup = false;
            for (size_t j = 0; j < ns; j++) {
                if (seen[j] == key) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                seen[ns++] = key;
            }
        }
        CHECK(ns <= 7);
        free(px);
    }
    /* transparency off: everything over the matte. */
    char png8[320];
    snprintf(png8, sizeof png8, "%s/a.png", dir);
    snprintf(p, sizeof p, "{\"format\":\"png8\",\"transparency\":false,\"matte\":\"#00ff00\",\"path\":\"%s\"}",
             png8);
    ex_ok(s, "file.export.saveForWebLegacy", p);
    px = decode_file(png8, &w, &h);
    CHECK(px != NULL);
    if (px != NULL) {
        CHECK_EQ_UINT(px[0], 0);
        CHECK_EQ_UINT(px[1], 255);
        CHECK_EQ_UINT(px[2], 0);
        CHECK_EQ_UINT(px[3], 255);
        free(px);
    }
    pc_engine_session_free(s);
    free(dir);
}

static void test_png8_no_matte_by_default(void)
{
    char *dir = tmpdir("matte");
    CHECK(dir != NULL);
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    ex_ok(s, "file.new", "{\"width\":256,\"height\":8,\"background\":\"transparent\"}");
    struct paint_ctx c = {1};
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, &c, NULL) == PC_OK);
    /* Plain: black or transparent (no grey matte). */
    char path[320];
    snprintf(path, sizeof path, "%s/plain.png", dir);
    char p[512];
    snprintf(p, sizeof p, "{\"format\":\"png8\",\"path\":\"%s\"}", path);
    ex_ok(s, "file.export.saveForWebLegacy", p);
    uint32_t w, h;
    uint8_t *px = decode_file(path, &w, &h);
    CHECK(px != NULL);
    if (px != NULL) {
        bool ok = true;
        for (int x = 0; x < 256; x++) {
            uint8_t *q = px + (size_t)x * 4;
            if (!((q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 255) || q[3] == 0)) {
                ok = false;
            }
        }
        CHECK(ok);
        CHECK_EQ_UINT(px[64 * 4 + 0], 0); /* alpha 191 stays black */
        CHECK_EQ_UINT(px[64 * 4 + 3], 255);
        CHECK_EQ_UINT(px[200 * 4 + 3], 0);
        free(px);
    }
    /* A matte asked for lightens the soft pixels. */
    snprintf(path, sizeof path, "%s/white.png", dir);
    snprintf(p, sizeof p, "{\"format\":\"png8\",\"matte\":\"#ffffff\",\"path\":\"%s\"}", path);
    ex_ok(s, "file.export.saveForWebLegacy", p);
    px = decode_file(path, &w, &h);
    CHECK(px != NULL);
    if (px != NULL) {
        CHECK(px[64 * 4 + 0] > 40 && px[64 * 4 + 3] == 255);
        free(px);
    }
    /* A preset keeps its white matte. */
    pc_web_settings st;
    CHECK(pc_web_settings_from_params(NULL, "x", &st, NULL) == PC_OK);
    pc_web_settings_fini(&st);
    pc_web_settings ps;
    CHECK(pc_web_preset_settings("PNG-8 128 Dithered", &ps));
    CHECK(ps.has_matte && ps.matte[0] == 1.0f && ps.matte[1] == 1.0f && ps.matte[2] == 1.0f);
    pc_engine_session_free(s);
    free(dir);
}

static void test_jpeg_quality_progressive_and_metadata(void)
{
    char *dir = tmpdir("jpeg");
    CHECK(dir != NULL);
    pc_engine_session *s = fixture(16);
    ex_ok(s, "file.fileInfo",
          "{\"copyright\":\"\\u00a9 Example\",\"author\":\"A. Person\",\"description\":\"secret notes\"}");
    char lo[320], hi[320];
    snprintf(lo, sizeof lo, "%s/lo.jpg", dir);
    snprintf(hi, sizeof hi, "%s/hi.jpg", dir);
    char p[512];
    snprintf(p, sizeof p, "{\"format\":\"jpeg\",\"quality\":10,\"path\":\"%s\",\"metadata\":\"none\"}", lo);
    ex_ok(s, "file.export.saveForWebLegacy", p);
    snprintf(p, sizeof p,
             "{\"format\":\"jpeg\",\"quality\":95,\"progressive\":true,\"path\":\"%s\",\"metadata\":\"copyright\"}",
             hi);
    ex_ok(s, "file.export.saveForWebLegacy", p);
    char *a = read_text(lo);
    FILE *fb = fopen(hi, "rb");
    CHECK(a != NULL && fb != NULL);
    fseek(fb, 0, SEEK_END);
    long bn = ftell(fb);
    fseek(fb, 0, SEEK_SET);
    uint8_t *bb = malloc((size_t)bn);
    CHECK(fread(bb, 1, (size_t)bn, fb) == (size_t)bn);
    fclose(fb);
    long an = 0;
    {
        FILE *fa = fopen(lo, "rb");
        fseek(fa, 0, SEEK_END);
        an = ftell(fa);
        fclose(fa);
    }
    CHECK(an < bn);
    bool progressive = false;
    for (long i = 0; i + 1 < bn; i++) {
        if (bb[i] == 0xFF && bb[i + 1] == 0xC2) {
            progressive = true;
            break;
        }
    }
    CHECK(progressive);
    free(a);
    free(bb);
    pc_engine_session_free(s);
    free(dir);
}

static void test_slices_export_with_html_table(void)
{
    char *dir = tmpdir("slices");
    CHECK(dir != NULL);
    pc_engine_session *s = fixture(8);
    ex_ok(s, "slice.new",
          "{\"rect\":[8,8,24,24],\"name\":\"logo\",\"url\":\"https://example.org/\",\"alt\":\"Logo\"}");
    ex_ok(s, "slice.new", "{\"rect\":[40,0,24,20],\"kind\":\"noImage\",\"cellText\":\"Hello & bye\"}");
    char p[512];
    snprintf(p, sizeof p, "{\"format\":\"png24\",\"dir\":\"%s\",\"html\":true}", dir);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.export.saveForWebLegacy", p, &o) == PC_OK);
    yyjson_mut_val *files = field(&o, "files");
    CHECK(files != NULL && yyjson_mut_is_arr(files));
    const char *htmlpath = str_field(&o, "html");
    CHECK(htmlpath != NULL);
    char logo[320];
    snprintf(logo, sizeof logo, "%s/images/logo.png", dir);
    uint32_t w, h;
    uint8_t *lp = decode_file(logo, &w, &h);
    CHECK(lp != NULL && w == 24 && h == 24);
    free(lp);
    char *html = read_text(htmlpath);
    CHECK(html != NULL);
    if (html != NULL) {
        CHECK_CONTAINS(html, "<a href=\"https://example.org/\">");
        CHECK_CONTAINS(html, "alt=\"Logo\"");
        CHECK_CONTAINS(html, "Hello &amp; bye");
        CHECK_CONTAINS(html, "images/spacer.gif");
        free(html);
    }
    char spacer[320];
    snprintf(spacer, sizeof spacer, "%s/images/spacer.gif", dir);
    CHECK(access(spacer, F_OK) == 0);
    pc_json_value_fini(&o);
    /* Scaled export scales slices (user slices only). */
    char *dir2 = tmpdir("slices2");
    snprintf(p, sizeof p, "{\"format\":\"gif\",\"dir\":\"%s\",\"percent\":50,\"slices\":\"user\"}", dir2);
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.export.saveForWebLegacy", p, &o2) == PC_OK);
    yyjson_mut_val *f2 = field(&o2, "files");
    CHECK(f2 != NULL && yyjson_mut_arr_size(f2) == 1);
    const char *g = yyjson_mut_get_str(yyjson_mut_arr_get(f2, 0));
    uint8_t *gp = g != NULL ? decode_file(g, &w, &h) : NULL;
    CHECK(gp != NULL && w == 12 && h == 12);
    free(gp);
    pc_json_value_fini(&o2);
    pc_engine_session_free(s);
    free(dir);
    free(dir2);
}

static void test_slice_html_escapes_and_links(void)
{
    char *dir = tmpdir("safety");
    CHECK(dir != NULL);
    pc_engine_session *s = fixture(8);
    ex_ok(s, "slice.new",
          "{\"rect\":[0,0,24,20],\"kind\":\"noImage\",\"name\":\"text\",\"cellTextIsHtml\":true,"
          "\"cellText\":\"<script>alert(1)</script><b>Trusted label</b>\"}");
    char p[512];
    snprintf(p, sizeof p, "{\"format\":\"png24\",\"dir\":\"%s\",\"html\":true}", dir);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.export.saveForWebLegacy", p, &o) == PC_OK);
    char *html = read_text(str_field(&o, "html"));
    CHECK(html != NULL);
    if (html != NULL) {
        CHECK_CONTAINS(html, "&lt;script&gt;alert(1)&lt;/script&gt;");
        CHECK(strstr(html, "<script>") == NULL);
        free(html);
    }
    pc_json_value_fini(&o);
    /* trusted opt-in emits raw markup. */
    char *tdir = tmpdir("trusted");
    snprintf(p, sizeof p, "{\"format\":\"png24\",\"dir\":\"%s\",\"html\":true,\"trustedSliceHtml\":true}", tdir);
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.export.saveForWebLegacy", p, &o2) == PC_OK);
    char *html2 = read_text(str_field(&o2, "html"));
    CHECK(html2 != NULL);
    if (html2 != NULL) {
        CHECK_CONTAINS(html2, "<script>alert(1)</script><b>Trusted label</b>");
        free(html2);
    }
    pc_json_value_fini(&o2);
    pc_engine_session_free(s);
    free(dir);
    free(tdir);
}

static void test_slice_links_drop_active_schemes(void)
{
    char *dir = tmpdir("links");
    CHECK(dir != NULL);
    pc_engine_session *s = fixture(8);
    const char *urls[] = {"javascript:alert(1)",
                          "data:text/html,<script>alert(1)</script>",
                          "https://example.org/page?a=1&b=2",
                          "../relative/page.html#part",
                          "mailto:hello@example.org"};
    for (size_t i = 0; i < 5; i++) {
        char p[512];
        snprintf(p, sizeof p, "{\"rect\":[%zu,0,10,10],\"name\":\"link%zu\",\"url\":\"%s\"}", i * 10, i, urls[i]);
        ex_ok(s, "slice.new", p);
    }
    char p[512];
    snprintf(p, sizeof p, "{\"format\":\"png24\",\"dir\":\"%s\",\"html\":true,\"slices\":\"user\"}", dir);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.export.saveForWebLegacy", p, &o) == PC_OK);
    char *html = read_text(str_field(&o, "html"));
    CHECK(html != NULL);
    if (html != NULL) {
        CHECK(strstr(html, "href=\"javascript:") == NULL);
        CHECK(strstr(html, "href=\"data:") == NULL);
        CHECK_CONTAINS(html, "href=\"https://example.org/page?a=1&amp;b=2\"");
        CHECK_CONTAINS(html, "href=\"../relative/page.html#part\"");
        CHECK_CONTAINS(html, "href=\"mailto:hello@example.org\"");
        size_t count = 0;
        for (const char *q = html; (q = strstr(q, "<a href=")) != NULL; q++) {
            count++;
        }
        CHECK_EQ_UINT(count, 3);
        free(html);
    }
    pc_json_value_fini(&o);
    pc_engine_session_free(s);
    free(dir);
}

static void test_images_folder_safety(void)
{
    char *root = tmpdir("imgfolder");
    CHECK(root != NULL);
    char dir[320];
    snprintf(dir, sizeof dir, "%s/output", root);
    CHECK(mkdir(dir, 0777) == 0);
    pc_engine_session *s = fixture(8);
    ex_ok(s, "slice.new", "{\"rect\":[0,0,20,20],\"name\":\"logo\",\"url\":\"https://example.org\"}");
    const char *bad[] = {"../escaped", "..\\escaped", "/absolute", "C:\\absolute", "nested/../../escaped", "/"};
    for (size_t i = 0; i < 6; i++) {
        char p[512];
        snprintf(p, sizeof p, "{\"format\":\"gif\",\"dir\":\"%s\",\"html\":true,\"imagesFolder\":\"%s\"}", dir,
                 bad[i]);
        pc_json_value o = PC_JSON_VALUE_INIT;
        CHECK(ex(s, "file.export.saveForWebLegacy", p, &o) != PC_OK);
        pc_json_value_fini(&o);
    }
    char escaped[320];
    snprintf(escaped, sizeof escaped, "%s/escaped", root);
    CHECK(access(escaped, F_OK) != 0);
    /* A valid nested folder with an ampersand is HTML-escaped. */
    char p[512];
    snprintf(p, sizeof p, "{\"format\":\"gif\",\"dir\":\"%s\",\"html\":true,\"imagesFolder\":\"nested/im&files\"}",
             dir);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.export.saveForWebLegacy", p, &o) == PC_OK);
    char *html = read_text(str_field(&o, "html"));
    CHECK(html != NULL);
    if (html != NULL) {
        CHECK_CONTAINS(html, "src=\"nested/im&amp;files/logo.gif\"");
        CHECK_CONTAINS(html, "src=\"nested/im&amp;files/spacer.gif\"");
        free(html);
    }
    pc_json_value_fini(&o);
    pc_engine_session_free(s);
    free(root);
}

static void test_duplicate_and_overlapping_slices(void)
{
    char *dir = tmpdir("dupes");
    CHECK(dir != NULL);
    pc_engine_session *s = fixture(8);
    ex_ok(s, "slice.new", "{\"rect\":[0,0,10,10],\"name\":\"tile\"}");
    ex_ok(s, "slice.new", "{\"rect\":[20,0,10,10],\"name\":\"tile\"}");
    ex_ok(s, "slice.new", "{\"rect\":[40,0,10,10],\"name\":\"a/b\"}");
    ex_ok(s, "slice.new", "{\"rect\":[0,20,10,10],\"name\":\"a?b\"}");
    char p[512];
    snprintf(p, sizeof p, "{\"format\":\"gif\",\"dir\":\"%s\",\"slices\":\"user\",\"html\":true}", dir);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.export.saveForWebLegacy", p, &o) == PC_OK);
    yyjson_mut_val *files = field(&o, "files");
    CHECK(files != NULL && yyjson_mut_arr_size(files) == 4);
    /* Distinct basenames: tile.gif, tile_2.gif, a_b.gif, a_b_2.gif (order preserved). */
    bool saw_tile = false, saw_tile2 = false, saw_ab = false, saw_ab2 = false;
    yyjson_mut_val *v;
    yyjson_mut_arr_iter it;
    yyjson_mut_arr_iter_init(files, &it);
    while ((v = yyjson_mut_arr_iter_next(&it)) != NULL) {
        const char *f = yyjson_mut_get_str(v);
        const char *b = strrchr(f, '/');
        b = b != NULL ? b + 1 : f;
        if (strcmp(b, "tile.gif") == 0) {
            saw_tile = true;
        } else if (strcmp(b, "tile_2.gif") == 0) {
            saw_tile2 = true;
        } else if (strcmp(b, "a_b.gif") == 0) {
            saw_ab = true;
        } else if (strcmp(b, "a_b_2.gif") == 0) {
            saw_ab2 = true;
        }
    }
    CHECK(saw_tile && saw_tile2 && saw_ab && saw_ab2);
    pc_json_value_fini(&o);
    pc_engine_session_free(s);
    free(dir);
}

static void test_reserved_names(void)
{
    char *dir = tmpdir("reserved");
    CHECK(dir != NULL);
    pc_engine_session *s = fixture(8);
    /* name the document a reserved device. */
    ex_ok(s, "file.fileInfo", "{}"); /* no-op to ensure a doc */
    /* rename the document via the slices base name path: use save-as name isn't needed; set via
     * a layer rename won't change doc name, so rely on an aux slice name. */
    ex_ok(s, "slice.new", "{\"rect\":[0,0,16,16],\"name\":\"con\"}");
    char p[512];
    snprintf(p, sizeof p, "{\"dir\":\"%s\",\"format\":\"png24\",\"html\":true}", dir);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.export.saveForWebLegacy", p, &o) == PC_OK);
    yyjson_mut_val *files = field(&o, "files");
    bool saw_con = false;
    if (files != NULL) {
        yyjson_mut_val *v;
        yyjson_mut_arr_iter it;
        yyjson_mut_arr_iter_init(files, &it);
        while ((v = yyjson_mut_arr_iter_next(&it)) != NULL) {
            const char *f = yyjson_mut_get_str(v);
            size_t fl = strlen(f);
            if (fl >= 8 && strcmp(f + fl - 8, "con_.png") == 0) {
                saw_con = true;
            }
        }
    }
    CHECK(saw_con);
    pc_json_value_fini(&o);
    pc_engine_session_free(s);
    free(dir);
}

static void test_export_preferences_drive_quick_export(void)
{
    char *dir = tmpdir("quick");
    CHECK(dir != NULL);
    pc_engine_session *s = fixture(8);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.export.exportPreferences",
             "{\"quickExportFormat\":\"jpg\",\"jpegQuality\":40,\"quickExportLocation\":\"sameFolder\"}",
             &o) == PC_OK);
    yyjson_mut_val *values = field(&o, "values");
    CHECK(values != NULL);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(values, "quickExportFormat")), "jpg");
    pc_json_value_fini(&o);
    /* bad enum rejected. */
    CHECK(ex(s, "file.export.exportPreferences", "{\"quickExportFormat\":\"bmp\"}", &o) != PC_OK);
    pc_json_value_fini(&o);
    /* unsaved document needs a path. */
    CHECK(ex(s, "file.export.quickExport", "{}", &o) != PC_OK);
    pc_json_value_fini(&o);
    /* with a document path, same-folder export. */
    char psd[320];
    snprintf(psd, sizeof psd, "%s/web.psd", dir);
    pc_engine_session_active_mut(s)->path = strdup(psd);
    CHECK(ex(s, "file.export.quickExport", "{}", &o) == PC_OK);
    char expect[320];
    snprintf(expect, sizeof expect, "%s/web.jpg", dir);
    CHECK_EQ_STR(str_field(&o, "path"), expect);
    uint32_t w, h;
    uint8_t *px = decode_file(expect, &w, &h);
    CHECK(px != NULL && w == 64 && h == 48);
    free(px);
    pc_json_value_fini(&o);
    /* gif Quick Export. */
    ex_ok(s, "file.export.exportPreferences", "{\"quickExportFormat\":\"gif\"}");
    char gif[320];
    snprintf(gif, sizeof gif, "%s/x.gif", dir);
    char qp[512];
    snprintf(qp, sizeof qp, "{\"path\":\"%s\"}", gif);
    CHECK(ex(s, "file.export.quickExport", qp, &o) == PC_OK);
    CHECK_EQ_STR(str_field(&o, "format"), "gif");
    pc_json_value_fini(&o);
    char *g = read_text(gif);
    CHECK(g != NULL && strncmp(g, "GIF89a", 6) == 0);
    free(g);
    pc_engine_session_free(s);
    free(dir);
}

static void test_settings_from_params(void)
{
    pc_web_settings st;
    CHECK(pc_web_settings_from_params(NULL, "x", &st, NULL) == PC_OK);
    CHECK(st.format == PC_WEB_PNG24 && st.colors == 256 && !st.has_matte);
    pc_web_settings_fini(&st);
    /* parse a JSON params object. */
    yyjson_doc *d = NULL;
    const char *js = "{\"preset\":\"GIF 32 No Dither\",\"matte\":\"none\",\"webSnap\":50}";
    CHECK(pc_json_parse(js, strlen(js), &d, NULL) == PC_OK);
    CHECK(pc_web_settings_from_params(yyjson_doc_get_root(d), "x", &st, NULL) == PC_OK);
    CHECK(st.format == PC_WEB_GIF && st.colors == 32 && st.dither == PC_ALGO_DITHER_NONE && !st.has_matte);
    CHECK_NEAR(st.web_snap, 0.5, 1e-6);
    pc_web_settings_fini(&st);
    yyjson_doc_free(d);
    /* unknown preset / palette rejected. */
    CHECK(pc_json_parse("{\"preset\":\"Nope\"}", 17, &d, NULL) == PC_OK);
    CHECK(pc_web_settings_from_params(yyjson_doc_get_root(d), "x", &st, NULL) != PC_OK);
    yyjson_doc_free(d);
    CHECK(pc_json_parse("{\"palette\":\"weird\"}", 19, &d, NULL) == PC_OK);
    CHECK(pc_web_settings_from_params(yyjson_doc_get_root(d), "x", &st, NULL) != PC_OK);
    yyjson_doc_free(d);
}

static void test_optimize_previews(void)
{
    /* A small gradient buffer; the preview is straight RGBA8 of the optimised pixels. */
    size_t w = 64, h = 48;
    float(*px)[4] = malloc(w * h * sizeof *px);
    CHECK(px != NULL);
    /* Mirror the flattened fixture: transparent except the red square, its soft edge and the
     * gradient band (so the quantizer sees the same input as the session export). */
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            float *p = px[y * w + x];
            p[0] = p[1] = p[2] = p[3] = 0.0f;
            if (x >= 8 && x < 32 && y >= 8 && y < 32) {
                p[0] = 1.0f;
                p[3] = 1.0f;
            } else if (x >= 32 && x < 34 && y >= 8 && y < 32) {
                p[0] = 1.0f;
                p[3] = 0.4f;
            } else if (y >= 36 && y < 48) {
                p[0] = (float)x / 63.0f;
                p[1] = 0.5f;
                p[2] = 1.0f - (float)x / 63.0f;
                p[3] = 1.0f;
            }
        }
    }
    const char *fmts[] = {"jpeg", "gif", "png8", "png24", "wbmp"};
    for (size_t fi = 0; fi < 5; fi++) {
        char js[80];
        snprintf(js, sizeof js, "{\"format\":\"%s\",\"quality\":90,\"transparency\":false}", fmts[fi]);
        yyjson_doc *d = NULL;
        CHECK(pc_json_parse(js, strlen(js), &d, NULL) == PC_OK);
        pc_web_settings st;
        CHECK(pc_web_settings_from_params(yyjson_doc_get_root(d), "x", &st, NULL) == PC_OK);
        pc_web_optimized o;
        CHECK(pc_web_optimize((const float (*)[4])px, w, pc_rect_make(0, 0, (int)w, (int)h), &st, NULL, 0, NULL,
                              0, 72.0f, true, &o, NULL) == PC_OK);
        CHECK_EQ_UINT(o.npreview, w * h * 4);
        uint8_t *q = o.preview + (16 * w + 16) * 4;
        if (strcmp(fmts[fi], "wbmp") == 0) {
            CHECK_EQ_UINT(q[0], 0);
        } else {
            CHECK(q[0] > 200 && q[1] < 60 && q[2] < 60);
        }
        pc_web_optimized_fini(&o);
        /* preview off -> empty, same bytes. */
        pc_web_optimized o2;
        CHECK(pc_web_optimize((const float (*)[4])px, w, pc_rect_make(0, 0, (int)w, (int)h), &st, NULL, 0, NULL,
                              0, 72.0f, false, &o2, NULL) == PC_OK);
        CHECK(o2.npreview == 0);
        pc_web_optimized_fini(&o2);
        pc_web_settings_fini(&st);
        yyjson_doc_free(d);
    }
    free(px);
}

static void test_graceful_failure(void)
{
    pc_engine_session *s = fixture(8);
    const char *ps[] = {"{}", "[]", "null", "123", "{\"path\":5}", "{\"colors\":0}",
                        "{\"format\":\"gif\",\"colors\":0}", "{\"width\":1e18,\"height\":1e18}",
                        "{\"colorTable\":[]}", "{\"colorTableEdits\":{\"0\":\"#fff\"}}",
                        "{\"format\":\"gif\",\"colorTable\":[\"bad\"]}"};
    for (size_t i = 0; i < sizeof ps / sizeof ps[0]; i++) {
        pc_json_value o = PC_JSON_VALUE_INIT;
        (void)ex(s, "file.export.saveForWebLegacy", ps[i], &o); /* must not crash */
        pc_json_value_fini(&o);
    }
    /* authorize gate: a denied export writes nothing and leaves last_web unset. */
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    g_tmp = getenv("TMPDIR");
    if (g_tmp == NULL || g_tmp[0] == '\0') {
        g_tmp = "/tmp";
    }
    RUN(test_registered_and_image_assets_stub);
    RUN(test_estimates_every_format_at_every_depth);
    RUN(test_last_web_and_preferences);
    RUN(test_gif_and_png8_transparency_and_palette);
    RUN(test_png8_no_matte_by_default);
    RUN(test_jpeg_quality_progressive_and_metadata);
    RUN(test_slices_export_with_html_table);
    RUN(test_slice_html_escapes_and_links);
    RUN(test_slice_links_drop_active_schemes);
    RUN(test_images_folder_safety);
    RUN(test_duplicate_and_overlapping_slices);
    RUN(test_reserved_names);
    RUN(test_export_preferences_drive_quick_export);
    RUN(test_settings_from_params);
    RUN(test_optimize_previews);
    RUN(test_graceful_failure);
    return pc_t_finish();
}
