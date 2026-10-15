/* File-menu commands and the exported file-I/O primitives (ported from the reachable parts of
 * crates/engine/src/file_cmds and file_cmds/tests): Close All / Others, Revert, Save a Copy, Open
 * As, Place Embedded / Linked, Fit Image, Conditional Mode Change, Layers to Files, Color Lookup
 * Tables, and the View-menu guide items. Every registered command also gets an adversarial-params
 * sweep (REWRITE_PLAN rule 9: never crash, return an error).
 *
 * The exact encoded bytes are exercised by the codecs/io tests; here we assert the structural
 * outcomes the engine owns (documents created/closed, file round-trips, result fields) and that
 * the primitives route documents through src/io + src/format. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/buf.h"
#include "color/color.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "ops/ops.h"
#include "raster/surface.h"
#include "engine/engine_internal.h"
#include "engine/prefs.h"
#include "io/io.h"
#include "harness.h"

static const pc_document *doc_of(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d != NULL ? pc_doc_arc_get(d->doc) : NULL;
}

static size_t ndocs(pc_engine_session *s)
{
    size_t n = 0;
    (void)pc_engine_session_documents(s, &n);
    return n;
}

static pc_status new_doc(pc_engine_session *s, const char *json)
{
    pc_json_value out;
    pc_status rc = pc_engine_session_execute_json(s, "file.new", json, &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    return rc;
}

/* An edit appending a transparent raster layer with a red square at (10,5)-(20,15). */
static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ctx;
    pc_layer l;
    pc_status rc = pc_layer_raster(&l, "Layer 1", pc_document_pixel_format(doc), err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface *surf = pc_layer_surface_mut(&l);
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t nch = pc_from_rgba(pc_surface_format(surf), red, enc);
    rc = pc_surface_fill_rect(surf, pc_rect_make(10, 5, 20, 15), enc, nch, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    pc_layer_id id;
    const pc_layer_id *above = *ha ? active : NULL;
    rc = pc_document_insert_above(doc, above, &l, &id, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    *ha = true;
    *active = id;
    return PC_OK;
}

static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(new_doc(s, "{\"width\":40,\"height\":20,\"background\":\"white\"}") == PC_OK);
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);
    return s;
}

static void tmp_path(char *buf, size_t cap, const char *name)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0') {
        dir = "/tmp";
    }
    (void)snprintf(buf, cap, "%s/pc-e3f-%ld-%s", dir, (long)getpid(), name);
}

/* Result-field helpers (the result is a yyjson_mut object). */
static yyjson_mut_val *field(const pc_json_value *v, const char *key)
{
    yyjson_mut_val *root = pc_json_value_root(v);
    return root != NULL ? yyjson_mut_obj_get(root, key) : NULL;
}

/* ---- primitives --------------------------------------------------------------------------- */

static void test_primitives_roundtrip(void)
{
    pc_engine_session *s = session();
    const pc_document *doc = doc_of(s);
    CHECK(doc != NULL);

    char path[512];
    tmp_path(path, sizeof path, "prim.png");

    /* encode -> write -> read -> import round-trips a 40x20 image. */
    pc_buf bytes = PC_BUF_INIT;
    pc_error err = {0};
    CHECK(pc_engine_encode(doc, path, NULL, &bytes, NULL, &err) == PC_OK);
    CHECK(bytes.len > 0);
    CHECK(pc_engine_write_file(path, bytes.data, bytes.len, &err) == PC_OK);

    pc_buf back = PC_BUF_INIT;
    CHECK(pc_engine_read_file(path, &back, &err) == PC_OK);
    CHECK(back.len == bytes.len);

    pc_document imported;
    CHECK(pc_engine_import(path, back.data, back.len, &imported, &err) == PC_OK);
    CHECK_EQ_UINT(imported.size.width, 40);
    CHECK_EQ_UINT(imported.size.height, 20);
    pc_document_fini(&imported);

    /* flattened returns a surface covering the canvas. */
    pc_surface flat;
    CHECK(pc_engine_flattened(doc, pc_document_pixel_format(doc), &flat, &err) == PC_OK);
    pc_surface_fini(&flat);

    pc_buf_fini(&back);
    pc_buf_fini(&bytes);
    (void)remove(path);
    pc_engine_session_free(s);
}

static void test_read_file_missing_is_error(void)
{
    pc_buf b = PC_BUF_INIT;
    pc_error err = {0};
    CHECK(pc_engine_read_file("/no/such/photocraft/file.bin", &b, &err) != PC_OK);
    pc_buf_fini(&b);
    /* Importing garbage must fail gracefully, never crash. */
    pc_document d;
    uint8_t junk[8] = {0xde, 0xad, 0xbe, 0xef, 0, 1, 2, 3};
    CHECK(pc_engine_import("x.png", junk, sizeof junk, &d, &err) != PC_OK);
}

/* ---- close -------------------------------------------------------------------------------- */

static void test_close_all_and_others(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    for (int i = 0; i < 3; i++) {
        CHECK(new_doc(s, "{\"width\":8,\"height\":8}") == PC_OK);
    }
    CHECK_EQ_UINT(ndocs(s), 3);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.closeAll", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(field(&out, "closed")), 3);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(ndocs(s), 0);

    for (int i = 0; i < 3; i++) {
        CHECK(new_doc(s, "{\"width\":8,\"height\":8}") == PC_OK);
    }
    CHECK(pc_engine_session_execute_json(s, "file.closeOthers", "{\"document\":1}", &out, NULL) ==
          PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(field(&out, "closed")), 2);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(ndocs(s), 1);
    pc_engine_session_free(s);
}

/* ---- save a copy -------------------------------------------------------------------------- */

static void test_save_a_copy(void)
{
    pc_engine_session *s = session();
    char path[512];
    tmp_path(path, sizeof path, "copy.png");
    char p[640];
    (void)snprintf(p, sizeof p, "{\"path\":\"%s\",\"layers\":false}", path);
    pc_json_value out;
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "file.saveACopy", p, &out, &err) == PC_OK);
    CHECK(field(&out, "path") != NULL);
    pc_json_value_fini(&out);

    /* The copy is a valid image on disk; a copy never changes the document's identity. */
    pc_buf b = PC_BUF_INIT;
    CHECK(pc_engine_read_file(path, &b, &err) == PC_OK);
    pc_document d;
    CHECK(pc_engine_import(path, b.data, b.len, &d, &err) == PC_OK);
    CHECK_EQ_UINT(d.size.width, 40);
    pc_document_fini(&d);
    pc_buf_fini(&b);
    /* Save a Copy does not set the document's save path. */
    CHECK(pc_engine_session_active(s)->path == NULL);
    (void)remove(path);
    pc_engine_session_free(s);

    /* Missing path is a bad-params error, not a crash. */
    pc_engine_session *s2 = session();
    CHECK(pc_engine_session_execute_json(s2, "file.saveACopy", "{}", &out, NULL) != PC_OK);
    pc_engine_session_free(s2);
}

/* ---- open as + revert --------------------------------------------------------------------- */

static void test_open_as_and_revert(void)
{
    /* Write a PNG, open it (the document gets a path), paint, then Revert reloads the file. */
    pc_engine_session *seed = session();
    char path[512];
    tmp_path(path, sizeof path, "revert.png");
    pc_buf bytes = PC_BUF_INIT;
    pc_error err = {0};
    CHECK(pc_engine_encode(doc_of(seed), path, NULL, &bytes, NULL, &err) == PC_OK);
    CHECK(pc_engine_write_file(path, bytes.data, bytes.len, &err) == PC_OK);
    pc_buf_fini(&bytes);
    pc_engine_session_free(seed);

    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char p[640];
    (void)snprintf(p, sizeof p, "{\"path\":\"%s\"}", path);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.openAs", p, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_active(s)->path != NULL);
    size_t layers0 = doc_of(s)->nlayers;

    /* Paint a new layer, then revert. */
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);
    CHECK(doc_of(s)->nlayers == layers0 + 1);
    CHECK(pc_engine_doc_state_is_dirty(pc_engine_session_active(s)));

    CHECK(pc_engine_session_execute_json(s, "file.revert", "{}", &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->nlayers == layers0);
    CHECK(!pc_engine_doc_state_is_dirty(pc_engine_session_active(s)));

    (void)remove(path);
    pc_engine_session_free(s);
}

/* ---- place -------------------------------------------------------------------------------- */

static void test_place_embedded(void)
{
    /* A small source image on disk. */
    pc_engine_session *seed = NULL;
    CHECK(pc_engine_session_new(&seed, NULL) == PC_OK);
    CHECK(new_doc(seed, "{\"width\":10,\"height\":10,\"background\":\"black\"}") == PC_OK);
    char src[512];
    tmp_path(src, sizeof src, "src.png");
    pc_buf bytes = PC_BUF_INIT;
    pc_error err = {0};
    CHECK(pc_engine_encode(doc_of(seed), src, NULL, &bytes, NULL, &err) == PC_OK);
    CHECK(pc_engine_write_file(src, bytes.data, bytes.len, &err) == PC_OK);
    pc_buf_fini(&bytes);
    pc_engine_session_free(seed);

    pc_engine_session *s = session();
    size_t before = doc_of(s)->nlayers;
    char p[640];
    (void)snprintf(p, sizeof p, "{\"path\":\"%s\"}", src);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.placeEmbedded", p, &out, &err) == PC_OK);
    /* Default preference: placed as a smart object; a new layer is added. */
    CHECK(yyjson_mut_get_bool(field(&out, "smartObject")) == true);
    CHECK(yyjson_mut_get_uint(field(&out, "layer")) > 0);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->nlayers == before + 1);

    /* smartObject:false places a plain pixel layer. */
    (void)snprintf(p, sizeof p, "{\"path\":\"%s\",\"smartObject\":false}", src);
    CHECK(pc_engine_session_execute_json(s, "file.placeEmbedded", p, &out, &err) == PC_OK);
    CHECK(yyjson_mut_get_bool(field(&out, "smartObject")) == false);
    pc_json_value_fini(&out);

    (void)remove(src);
    pc_engine_session_free(s);
}

/* ---- automate ----------------------------------------------------------------------------- */

static void test_fit_image(void)
{
    pc_engine_session *s = session(); /* 40x20 */
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.automate.fitImage",
                                         "{\"width\":20,\"height\":20}", &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(field(&out, "changed")) == true);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(doc_of(s)->size.width, 20);
    CHECK_EQ_UINT(doc_of(s)->size.height, 10);

    /* dontEnlarge keeps a smaller image unchanged. */
    CHECK(pc_engine_session_execute_json(
              s, "file.automate.fitImage", "{\"width\":400,\"height\":400,\"dontEnlarge\":true}",
              &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(field(&out, "changed")) == false);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_conditional_mode_change(void)
{
    pc_engine_session *s = session(); /* rgb */
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.automate.conditionalModeChange",
                                         "{\"to\":\"grayscale\"}", &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(field(&out, "changed")) == true);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->mode == PC_COLOR_MODE_GRAYSCALE);

    /* Already grayscale: no change. */
    CHECK(pc_engine_session_execute_json(s, "file.automate.conditionalModeChange",
                                         "{\"to\":\"gray\"}", &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(field(&out, "changed")) == false);
    pc_json_value_fini(&out);

    /* from-filter that doesn't match the current mode: no change. */
    CHECK(pc_engine_session_execute_json(s, "file.automate.conditionalModeChange",
                                         "{\"from\":[\"cmyk\"],\"to\":\"rgb\"}", &out, NULL) ==
          PC_OK);
    CHECK(yyjson_mut_get_bool(field(&out, "changed")) == false);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* ---- export ------------------------------------------------------------------------------- */

static void test_layers_to_files(void)
{
    pc_engine_session *s = session(); /* Background + Layer 1 = 2 top-level layers */
    char dir[512];
    tmp_path(dir, sizeof dir, "l2f");
    (void)mkdir(dir, 0777);
    char p[640];
    (void)snprintf(p, sizeof p, "{\"dir\":\"%s\",\"format\":\"png\"}", dir);
    pc_json_value out;
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "file.export.layersToFiles", p, &out, &err) == PC_OK);
    yyjson_mut_val *files = field(&out, "files");
    CHECK(files != NULL && yyjson_mut_is_arr(files));
    CHECK_EQ_UINT(yyjson_mut_arr_size(files), 2);
    /* Each named file exists and reopens. */
    size_t idx, max;
    yyjson_mut_val *fv;
    yyjson_mut_arr_foreach(files, idx, max, fv)
    {
        const char *fp = yyjson_mut_get_str(fv);
        CHECK(fp != NULL);
        pc_buf b = PC_BUF_INIT;
        CHECK(pc_engine_read_file(fp, &b, &err) == PC_OK);
        pc_buf_fini(&b);
        (void)remove(fp);
    }
    pc_json_value_fini(&out);
    (void)rmdir(dir);
    pc_engine_session_free(s);
}

static void test_color_lookup_tables(void)
{
    pc_engine_session *s = session();
    /* Without an adjustment layer the command is disabled. */
    CHECK(!pc_engine_session_is_enabled(s, "file.export.colorLookupTables"));
    /* Add an Invert adjustment layer, then export a .cube. */
    pc_json_value tmp;
    CHECK(pc_engine_session_execute_json(s, "layer.newAdjustmentLayer.invert", "{}", &tmp, NULL) ==
          PC_OK);
    pc_json_value_fini(&tmp);
    CHECK(pc_engine_session_is_enabled(s, "file.export.colorLookupTables"));

    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.export.colorLookupTables", "{\"size\":5}", &out,
                                         NULL) == PC_OK);
    const char *cube = yyjson_mut_get_str(field(&out, "cube"));
    CHECK(cube != NULL);
    CHECK_CONTAINS(cube, "LUT_3D_SIZE 5");
    CHECK_EQ_UINT(yyjson_mut_get_uint(field(&out, "layerCount")), 1);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* ---- guides ------------------------------------------------------------------------------- */

static void test_guides(void)
{
    pc_engine_session *s = session();
    /* A shape layer so New Guides From Shape has a path. */
    pc_json_value sh;
    pc_status rc = pc_engine_session_execute_json(
        s, "shape.create", "{\"kind\":\"rect\",\"rect\":[4,6,20,8]}", &sh, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&sh);
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, "view.newGuidesFromShape", "{}", &out, NULL) ==
              PC_OK);
        CHECK(field(&out, "bounds") != NULL);
        pc_json_value_fini(&out);
        /* Clearing canvas guides removes the ones just added. */
        CHECK(pc_engine_session_execute_json(s, "view.clearCanvasGuides", "{}", &out, NULL) ==
              PC_OK);
        CHECK(yyjson_mut_get_uint(field(&out, "cleared")) > 0);
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

/* ---- file info (XMP) ---------------------------------------------------------------------- */

static void test_file_info(void)
{
    pc_engine_session *s = session();
    pc_json_value out;
    /* Read with no XMP: empty fields, status unknown. */
    CHECK(pc_engine_session_execute_json(s, "file.fileInfo", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(field(&out, "copyrightStatus")), "unknown");
    CHECK(yyjson_mut_obj_get(pc_json_value_root(&out), "camera") != NULL);
    pc_json_value_fini(&out);

    /* Write fields; they round-trip through the document's XMP. */
    CHECK(pc_engine_session_execute_json(
              s, "file.fileInfo",
              "{\"title\":\"My & <Photo>\",\"author\":\"Ada; Grace\","
              "\"keywords\":[\"sky\",\"sea\"],\"copyright\":\"(c) me\","
              "\"copyrightStatus\":\"copyrighted\",\"copyrightUrl\":\"http://example.com/x\"}",
              &out, NULL) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(field(&out, "title")), "My & <Photo>");
    CHECK_EQ_STR(yyjson_mut_get_str(field(&out, "author")), "Ada; Grace");
    yyjson_mut_val *kw = field(&out, "keywords");
    CHECK(kw != NULL && yyjson_mut_is_arr(kw) && yyjson_mut_arr_size(kw) == 2);
    CHECK_EQ_STR(yyjson_mut_get_str(field(&out, "copyrightStatus")), "copyrighted");
    CHECK_EQ_STR(yyjson_mut_get_str(field(&out, "copyrightUrl")), "http://example.com/x");
    pc_json_value_fini(&out);

    /* A fresh read returns the same values (persisted in the document's metadata). */
    CHECK(pc_engine_session_execute_json(s, "file.fileInfo", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(field(&out, "title")), "My & <Photo>");
    CHECK_EQ_STR(yyjson_mut_get_str(field(&out, "copyrightStatus")), "copyrighted");
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* ---- paths to illustrator ----------------------------------------------------------------- */

static void test_paths_to_illustrator(void)
{
    pc_engine_session *s = session();
    /* No paths yet: the command is disabled. */
    CHECK(!pc_engine_session_is_enabled(s, "file.export.pathsToIllustrator"));
    /* Make a work path (a closed triangle). */
    pc_json_value tmp;
    pc_status rc = pc_engine_session_execute_json(
        s, "path.set",
        "{\"name\":\"work\",\"path\":{\"subpaths\":[{\"closed\":true,\"knots\":[[0,0],[10,0],[10,10]]}]}}",
        &tmp, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&tmp);
        CHECK(pc_engine_session_is_enabled(s, "file.export.pathsToIllustrator"));
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, "file.export.pathsToIllustrator", "{\"paths\":\"work\"}",
                                             &out, NULL) == PC_OK);
        const char *ai = yyjson_mut_get_str(field(&out, "ai"));
        CHECK(ai != NULL);
        CHECK_CONTAINS(ai, "%!PS-Adobe");
        CHECK_CONTAINS(ai, " m\n");
        CHECK(yyjson_mut_get_uint(field(&out, "paths")) == 1);
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

/* ---- document presets / recent ------------------------------------------------------------ */

/* A fresh session loaded from s's serialised preferences (reload round-trip). */
static pc_engine_session *reload_prefs(pc_engine_session *s)
{
    char *text = pc_engine_session_prefs_to_json(s);
    CHECK(text != NULL);
    pc_engine_session *next = NULL;
    CHECK(pc_engine_session_new(&next, NULL) == PC_OK);
    CHECK(pc_engine_session_load_prefs_json(next, text, strlen(text), NULL) == PC_OK);
    free(text);
    return next;
}

static void test_document_presets(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    const char *settings =
        "{\"width\":16,\"height\":24,\"resolution\":254.5,\"mode\":\"lab\",\"depth\":16,"
        "\"background\":\"backgroundColor\",\"backgroundColor\":[0.125,0.375,0.625],"
        "\"unit\":\"mm\",\"resolutionUnit\":\"cm\"}";
    char save[1024];
    (void)snprintf(save, sizeof save, "{\"name\":\"  Product square  \",\"settings\":%s}", settings);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "document.presets.save", save, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);

    /* Duplicate name (case-insensitive, trimmed) is refused; bad settings refused. */
    CHECK(pc_engine_session_execute_json(s, "document.presets.save", save, &out, NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(
              s, "document.presets.save",
              "{\"name\":\"Bad\",\"settings\":{\"width\":0,\"height\":1}}", &out, NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "document.presets.save", "{\"name\":\"  \"}", &out,
                                         NULL) != PC_OK);

    /* Survives a preferences reload; get returns params usable by file.new, no document created. */
    pc_engine_session *r = reload_prefs(s);
    pc_engine_session_free(s);
    CHECK(pc_engine_session_execute_json(r, "document.presets.get", "{\"name\":\"product SQUARE\"}",
                                         &out, NULL) == PC_OK);
    yyjson_mut_val *params = yyjson_mut_obj_get(pc_json_value_root(&out), "params");
    CHECK(params != NULL);
    /* The params have no name field and carry the saved values. */
    CHECK(yyjson_mut_obj_get(params, "name") == NULL);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(params, "width")) == 16);
    pc_json_value_fini(&out);
    CHECK(ndocs(r) == 0);

    CHECK(pc_engine_session_execute_json(r, "document.presets.list", "{}", &out, NULL) == PC_OK);
    CHECK(yyjson_mut_arr_size(field(&out, "presets")) == 1);
    pc_json_value_fini(&out);

    /* Delete it (case-insensitive) and confirm it's gone. */
    CHECK(pc_engine_session_execute_json(r, "document.presets.delete", "{\"name\":\" product SQUARE \"}",
                                         &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(r, "document.presets.get", "{\"name\":\"product square\"}",
                                         &out, NULL) != PC_OK);
    pc_engine_session_free(r);
}

static void test_recent_documents(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out;
    for (uint32_t w = 1; w <= 25; w++) {
        char p[160];
        (void)snprintf(p, sizeof p, "{\"settings\":{\"width\":%u,\"height\":10}}", w);
        CHECK(pc_engine_session_execute_json(s, "document.recent.add", p, &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
    }
    /* The same document again (other units) moves to the front, not duplicated. */
    CHECK(pc_engine_session_execute_json(
              s, "document.recent.add", "{\"settings\":{\"width\":10,\"height\":10,\"unit\":\"mm\"}}",
              &out, NULL) == PC_OK);
    pc_json_value_fini(&out);

    pc_engine_session *r = reload_prefs(s);
    pc_engine_session_free(s);
    CHECK(pc_engine_session_execute_json(r, "document.recent.list", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *rec = field(&out, "recent");
    CHECK(yyjson_mut_arr_size(rec) == 20); /* capped at MAX_RECENT */
    /* Newest first: 10 (moved up), then 25, 24. */
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_arr_get(rec, 0), "width")) == 10);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_arr_get(rec, 1), "width")) == 25);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_arr_get(rec, 2), "width")) == 24);
    pc_json_value_fini(&out);
    /* Bad input is an error and changes nothing. */
    CHECK(pc_engine_session_execute_json(r, "document.recent.add",
                                         "{\"settings\":{\"width\":0,\"height\":1}}", &out, NULL) !=
          PC_OK);
    pc_engine_session_free(r);
}

/* ---- adversarial params ------------------------------------------------------------------- */

static void run_adversarial(const char *id)
{
    const char *params[] = {
        "{}",
        "null",
        "{\"path\":5,\"document\":-1}",
        "{\"path\":\"\",\"scale\":\"big\",\"center\":[1]}",
        "{\"width\":-1,\"height\":[1,2],\"dpxBits\":7}",
        "{\"to\":42,\"from\":{},\"size\":99999}",
        "{\"dir\":[],\"format\":9,\"layer\":\"x\",\"document\":1e308}",
        "{\"path\":\"/dev/null/cannot\",\"as\":[1,2]}",
    };
    for (size_t i = 0; i < sizeof params / sizeof params[0]; i++) {
        pc_engine_session *s = session();
        pc_json_value out;
        pc_error err = {0};
        pc_status rc = pc_engine_session_execute_json(s, id, params[i], &out, &err);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
        pc_engine_session_free(s);

        pc_engine_session *empty = NULL;
        CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
        err = (pc_error){0};
        rc = pc_engine_session_execute_json(empty, id, params[i], &out, &err);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
        pc_engine_session_free(empty);
    }
}

static void test_adversarial(void)
{
    const char *ids[] = {
        "file.closeAll",
        "file.closeOthers",
        "file.revert",
        "file.saveACopy",
        "file.openAs",
        "file.placeEmbedded",
        "file.placeLinked",
        "file.automate.fitImage",
        "file.automate.conditionalModeChange",
        "file.export.layersToFiles",
        "file.export.colorLookupTables",
        "view.newGuidesFromShape",
        "view.clearCanvasGuides",
        "file.fileInfo",
        "document.presets.list",
        "document.presets.get",
        "document.presets.save",
        "document.presets.delete",
        "document.recent.list",
        "document.recent.add",
        "file.export.pathsToIllustrator",
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        CHECK(pc_engine_command_find(ids[i]) != NULL);
        run_adversarial(ids[i]);
    }
}

int main(int argc, char **argv)
{
    RUN(test_primitives_roundtrip);
    RUN(test_read_file_missing_is_error);
    RUN(test_close_all_and_others);
    RUN(test_save_a_copy);
    RUN(test_open_as_and_revert);
    RUN(test_place_embedded);
    RUN(test_fit_image);
    RUN(test_conditional_mode_change);
    RUN(test_layers_to_files);
    RUN(test_color_lookup_tables);
    RUN(test_file_info);
    RUN(test_guides);
    RUN(test_paths_to_illustrator);
    RUN(test_document_presets);
    RUN(test_recent_documents);
    RUN(test_adversarial);
    return pc_t_finish();
}
