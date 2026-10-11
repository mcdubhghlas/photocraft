/* The manifest model, migrations and conversion helpers (the #[cfg(test)] modules of
 * crates/format/src/manifest.rs, migrate.rs and autosave.rs), plus the every-field round trip
 * behind the doc-field guard (src/format/convert_save.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "format/bridge.h"
#include "format/convert.h"
#include "format/manifest.h"
#include "harness.h"
#include "parity_docs.h"

/* ---- migrate.rs ---- */

static yyjson_mut_doc *mdoc(const char *json)
{
    yyjson_doc *d = NULL;
    yyjson_mut_doc *m = NULL;
    if (pc_json_parse(json, strlen(json), &d, NULL) == PC_OK) {
        m = yyjson_doc_mut_copy(d, NULL);
    }
    yyjson_doc_free(d);
    CHECK(m != NULL);
    return m;
}

static yyjson_mut_val *get(yyjson_mut_val *o, const char *k)
{
    return yyjson_mut_obj_get(o, k);
}

static pc_status rename_title(yyjson_mut_doc *d, yyjson_mut_val *root, pc_error *err)
{
    yyjson_mut_val *doc = get(root, "document");
    yyjson_mut_val *t = yyjson_mut_obj_remove_key(doc, "title");
    (void)err;
    if (!yyjson_mut_obj_put(doc, yyjson_mut_str(d, "name"), t != NULL ? t : yyjson_mut_null(d))) {
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

static pc_status add_field(yyjson_mut_doc *d, yyjson_mut_val *root, pc_error *err)
{
    (void)err;
    return yyjson_mut_obj_put(get(root, "document"), yyjson_mut_str(d, "added"), yyjson_mut_true(d))
               ? PC_OK
               : PC_ERR_NOMEM;
}

static const pc_format_migrate_step STEPS[2] = {rename_title, add_field};

static void test_chained_steps_run_in_order(void)
{
    yyjson_mut_doc *d = mdoc("{\"format_version\": 1, \"document\": {\"title\": \"old\"}}");
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    uint32_t found = 0;
    CHECK(pc_format_migrate_with(d, root, STEPS, 2, 3, &found, NULL) == PC_OK);
    CHECK_EQ_UINT(found, 1);
    CHECK_EQ_UINT(yyjson_mut_get_uint(get(root, "format_version")), 3);
    CHECK(yyjson_mut_equals_str(get(get(root, "document"), "name"), "old"));
    CHECK(yyjson_mut_is_true(get(get(root, "document"), "added")));
    yyjson_mut_doc_free(d);
}

static void test_partial_chain_from_middle(void)
{
    yyjson_mut_doc *d = mdoc("{\"format_version\": 2, \"document\": {}}");
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    uint32_t found = 0;
    CHECK(pc_format_migrate_with(d, root, STEPS, 2, 3, &found, NULL) == PC_OK);
    CHECK_EQ_UINT(found, 2);
    CHECK(yyjson_mut_is_true(get(get(root, "document"), "added")));
    CHECK(get(get(root, "document"), "name") == NULL);
    yyjson_mut_doc_free(d);
}

static void test_too_new_and_invalid_versions(void)
{
    static const struct {
        const char *json;
        const char *msg;
    } cases[] = {
        {"{\"format_version\": 99}",
         "bundle written by a newer version (format 99, this build reads up to 1)"},
        {"{\"format_version\": 0}", "corrupt .pcraft bundle: format_version 0 is invalid"},
        {"{}", "corrupt .pcraft bundle: manifest has no format_version"},
        {"[1]", "corrupt .pcraft bundle: manifest has no format_version"},
        {"{\"format_version\": -1}", "corrupt .pcraft bundle: manifest has no format_version"},
        {"{\"format_version\": 1.0}", "corrupt .pcraft bundle: manifest has no format_version"},
        {"{\"format_version\": 4294967296}", "corrupt .pcraft bundle: format_version out of range"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        yyjson_mut_doc *d = mdoc(cases[i].json);
        uint32_t found = 7;
        pc_error e;
        pc_status st =
            pc_format_migrate_with(d, yyjson_mut_doc_get_root(d), NULL, 0, PC_FORMAT_VERSION, &found, &e);
        CHECK(st != PC_OK);
        CHECK_EQ_STR(e.msg, cases[i].msg);
        CHECK_EQ_UINT(found, 7);
        yyjson_mut_doc_free(d);
    }
    {
        yyjson_mut_doc *d = mdoc("{\"format_version\": 99}");
        uint32_t found = 0;
        pc_error e;
        pc_status st = pc_format_migrate_with(d, yyjson_mut_doc_get_root(d), NULL, 0, 1, &found, &e);
        CHECK(pc_format_error_kind(st, &e) == PC_FORMAT_ERR_TOO_NEW);
        yyjson_mut_doc_free(d);
        d = mdoc("{\"format_version\": 1}");
        st = pc_format_migrate_with(d, yyjson_mut_doc_get_root(d), NULL, 0, 2, &found, &e);
        CHECK(pc_format_error_kind(st, &e) == PC_FORMAT_ERR_UNSUPPORTED);
        CHECK_EQ_STR(e.msg, "unsupported .pcraft feature: no migration from format 1");
        yyjson_mut_doc_free(d);
    }
}

static void test_current_version_is_noop(void)
{
    yyjson_mut_doc *d = mdoc("{\"format_version\": 1}");
    uint32_t found = 0;
    CHECK(pc_format_migrate_with(d, yyjson_mut_doc_get_root(d), NULL, 0, PC_FORMAT_VERSION, &found, NULL) ==
          PC_OK);
    CHECK_EQ_UINT(found, PC_FORMAT_VERSION);
    yyjson_mut_doc_free(d);
}

/* ---- manifest.rs channel_tests ---- */

static const char EMPTY_MANIFEST_HEAD[] =
    "{\"format_version\":1,\"generator\":\"g\",\"document\":{\"id\":1,\"name\":\"d\",\"size\":{\"width\":1,"
    "\"height\":1},\"resolution_dpi\":72.0,\"mode\":\"Rgb\",\"depth\":\"U8\",\"layers\":[],\"guides\":"
    "{\"horizontal\":[],\"vertical\":[]},\"metadata\":{\"psd_resources\":[],\"psd_global_blocks\":[]},"
    "\"global_light\":{\"angle\":120.0,\"altitude\":30.0},\"channels\":";

static pc_format_manifest *parse_with_channels(const char *channels, pc_error *e)
{
    pc_buf b = PC_BUF_INIT;
    pc_format_manifest *m = NULL;
    (void)pc_buf_append_str(&b, EMPTY_MANIFEST_HEAD, NULL);
    (void)pc_buf_append_str(&b, channels, NULL);
    (void)pc_buf_append_str(&b, "}}", NULL);
    if (pc_format_manifest_parse(b.data, b.len, &m, e) != PC_OK) {
        m = NULL;
    }
    pc_buf_fini(&b);
    return m;
}

static void test_older_channels_load_with_default_options(void)
{
    pc_error e;
    pc_format_manifest *m = parse_with_channels(
        "[{\"name\":\"Alpha "
        "1\",\"surface\":{\"format\":{\"mode\":\"Grayscale\",\"sample\":\"U8\",\"alpha\":false},"
        "\"default\":\"00\",\"tiles\":[]},\"spot\":null}]",
        &e);
    if (m == NULL) {
        PC_T_FAIL("%s", e.msg);
        return;
    }
    {
        const pc_mf_channel *c = &m->document.channels[0];
        pc_color def = PC_ALPHA_CHANNEL_DEFAULT_COLOR;
        CHECK(m->document.nchannels == 1);
        CHECK(c->color.mode == def.mode && c->color.alpha == def.alpha &&
              memcmp(c->color.c, def.c, sizeof def.c) == 0);
        CHECK(c->opacity == 0.5f);
        CHECK(c->indicates == PC_INDICATES_MASKED_AREAS);
        CHECK(!c->has_spot);
    }
    /* The rest of DocM's defaults: empty collections, Photoshop's measurement, basic style. */
    CHECK(m->document.npaths == 0 && !m->document.has_work_path && m->document.nlayer_comps == 0);
    CHECK(pc_measurement_is_empty(&m->document.measurement));
    CHECK(pc_text_styles_is_empty(&m->document.text_styles));
    CHECK(pc_slices_is_empty(&m->document.slices) && !m->has_thumbnail && !m->has_composite);
    pc_format_manifest_free(m);
    /* A missing required field names itself; spot is an Option (missing = None). */
    m = parse_with_channels("[{\"name\":\"A\"}]", &e);
    CHECK(m == NULL);
    CHECK_EQ_STR(e.msg, "manifest JSON: missing field `surface`");
}

/* ---- helpers ---- */

static void test_unhex_follows_from_str_radix(void)
{
    uint8_t out[8];
    size_t n = 0;
    CHECK(pc_format_unhex("00ff7F", 6, out, sizeof out, &n) && n == 3 && out[1] == 0xff && out[2] == 0x7f);
    CHECK(pc_format_unhex("+f+0", 4, out, sizeof out, &n) && n == 2 && out[0] == 15 && out[1] == 0);
    CHECK(pc_format_unhex("", 0, out, sizeof out, &n) && n == 0);
    CHECK(!pc_format_unhex("0", 1, out, sizeof out, &n));
    CHECK(!pc_format_unhex("++", 2, out, sizeof out, &n));
    CHECK(!pc_format_unhex("f+", 2, out, sizeof out, &n));
    CHECK(!pc_format_unhex("-1", 2, out, sizeof out, &n));
    CHECK(!pc_format_unhex(" 1", 2, out, sizeof out, &n));
    CHECK(!pc_format_unhex("\xc3\xa9", 2, out, sizeof out, &n));
    /* Only validated past the buffer: the decoded length is still reported. */
    CHECK(pc_format_unhex("0102030405", 10, out, 2, &n) && n == 5 && out[0] == 1 && out[1] == 2);
}

static void test_reserve_ids(void)
{
    uint64_t now = pc_layer_id_fresh();
    CHECK(pc_format_reserve_ids_through(0));
    CHECK(pc_format_reserve_ids_through(now + 1000));
    CHECK(pc_layer_id_fresh() > now + 1000);
    CHECK(!pc_format_reserve_ids_through(now + 100000000));
    CHECK(pc_layer_id_fresh() < now + 100000000);
}

static void test_check_nesting_bounds(void)
{
    pc_document d;
    pc_error e;
    t_nested_doc(PC_MAX_GROUP_DEPTH, PC_SAMPLE_TYPE_U8, &d);
    CHECK(pc_format_check_nesting(&d, NULL) == PC_OK);
    pc_document_fini(&d);
    t_nested_doc(PC_MAX_GROUP_DEPTH + 1, PC_SAMPLE_TYPE_U8, &d);
    CHECK(pc_format_check_nesting(&d, &e) == PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "limit exceeded: layer groups nested deeper than 100");
    pc_document_fini(&d);
    /* An empty group one level past the limit is fine (the reference checks non-empty lists). */
    t_nested_doc(PC_MAX_GROUP_DEPTH, PC_SAMPLE_TYPE_U8, &d);
    {
        pc_layer *l = &d.layers[0];
        while (l->content.kind == PC_LAYER_GROUP && l->content.u.group.nchildren > 0 &&
               l->content.u.group.children[0].content.kind == PC_LAYER_GROUP) {
            l = &l->content.u.group.children[0];
        }
        {
            pc_layer empty;
            pc_layer *g =
                realloc(l->content.u.group.children, (l->content.u.group.nchildren + 1) * sizeof *g);
            CHECK(pc_layer_group(&empty, "empty", NULL, 0, NULL) == PC_OK);
            if (g != NULL) {
                l->content.u.group.children = g;
                g[l->content.u.group.nchildren++] = empty;
            }
        }
    }
    CHECK(pc_format_check_nesting(&d, NULL) == PC_OK);
    pc_document_fini(&d);
}

/* Parse errors keep the reference's order: JSON syntax, then the version, then the model. */
static void test_parse_error_order(void)
{
    static const struct {
        const char *json;
        const char *msg;
    } cases[] = {
        {"{\"format_version\": 2, \"generator\": 5",
         "manifest JSON: EOF while parsing an object at line 1 column 36"},
        {"{\"format_version\": 2, \"generator\": 5}",
         "bundle written by a newer version (format 2, this build reads up to 1)"},
        {"{\"format_version\": 1, \"generator\": 5}",
         "manifest JSON: invalid type: integer `5`, expected a string"},
        {"{\"format_version\": 1, \"generator\": \"g\"}", "manifest JSON: missing field `document`"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_format_manifest *m = NULL;
        pc_error e;
        CHECK(pc_format_manifest_parse((const uint8_t *)cases[i].json, strlen(cases[i].json), &m, &e) !=
              PC_OK);
        CHECK(m == NULL);
        if (strncmp(cases[i].msg, "manifest JSON: EOF", 18) == 0) {
            CHECK(strncmp(e.msg, "manifest JSON: ", 15) == 0); /* base's parser words it (§7 B2) */
        } else {
            CHECK_EQ_STR(e.msg, cases[i].msg);
        }
    }
}

/* ---- the every-field round trip ---- */

/* Every pc_document and pc_layer field set away from its default (and every layer kind), so a
 * field the manifest dropped would show as a difference. */
static void test_every_field_roundtrips(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_layer *paint;
    pc_error e;
    pd_extras(&d);
    /* No dangling references (the loader remaps those), so the document comes back equal. */
    d.layer_comps[0].states[2].layer = 101;
    d.slices.list[2].layer = 102;
    paint = &d.layers[1];
    paint->excluded_channels = 3;
    paint->has_link_group = true;
    paint->link_group = 9;
    T_JSON(pc_blend_if, J_BLEND_IF, &paint->blend_if);
    T_JSON(pc_advanced_blending, J_ADVANCED, &paint->advanced);
    paint->effects.has_reference = true;
    paint->effects.reference[0] = 1.25;
    paint->effects.reference[1] = -2.0;
    paint->has_fill_cache = true;
    T_JSON(pc_fill, T_SOLID_FILL, &paint->fill_cache.fill);
    pc_surface_init(&paint->fill_cache.surface, pc_document_pixel_format(&d));
    t_scribble(&paint->fill_cache.surface, 3, false);
    {
        pc_surface *frames = calloc(2, sizeof *frames);
        for (size_t i = 0; frames != NULL && i < 2; i++) {
            pc_surface_init(&frames[i], pc_document_pixel_format(&d));
            t_scribble(&frames[i], 40 + i, false);
        }
        pc_video_data_init(&paint->video, frames, frames != NULL ? 2 : 0, 12.5f);
        paint->has_video = true;
        paint->video.source.is_file = true;
        t_str(&paint->video.source.path, "/v");
    }
    CHECK(pc_format_save_to_bytes(&d, NULL, &b, &e) == PC_OK);
    if (pc_format_load_from_bytes(b.data, b.len, &back, &e) == PC_OK) {
        CHECK(pc_document_eq(&back, &d));
        /* pc_video_data_eq compares shape only: check the frame pixels too. */
        CHECK(back.layers[1].video.nframes == 2 &&
              pc_surface_eq(&back.layers[1].video.frames[1], &d.layers[1].video.frames[1]));
        pc_document_fini(&back);
    } else {
        PC_T_FAIL("%s", e.msg);
    }
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

/* ---- autosave.rs unit test ----
 *
 * The reference records a thread that could not start where last_result reports it. A thread
 * start can't be made to fail portably here; what can be checked is the message for a worker
 * that is not running (after a flush) and the sanitiser the paths depend on. */
static void test_sanitize_keys(void)
{
    static const struct {
        const char *in;
        size_t len;
        const char *out;
    } cases[] = {
        {"doc-1", 5, "doc-1"}, {"x y/z", 5, "x_y_z"}, {"", 0, "untitled"},   {"\xc3\xa9", 2, "_"},
        {"..", 2, "__"},       {"a\0b", 3, "a_b"},    {"\xff\xfe", 2, "__"}, {"A.b", 3, "A_b"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_str s = PC_STR_INIT;
        CHECK(pc_autosave_sanitize_key(cases[i].in, cases[i].len, &s, NULL) == PC_OK);
        CHECK(pc_str_eq_c(&s, cases[i].out));
        pc_str_fini(&s);
    }
}

static void test_recovery_info_json(void)
{
    pc_recovery_info i;
    pc_recovery_info back;
    pc_buf json = PC_BUF_INIT;
    memset(&i, 0, sizeof i);
    t_str(&i.key, "k");
    t_str(&i.document_name, "n");
    i.saved_at = 5;
    i.revision = 6;
    CHECK(pc_recovery_info_to_json(&i, &json, NULL) == PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&json),
                 "{\n  \"key\": \"k\",\n  \"document_name\": \"n\",\n  \"original_path\": null,\n"
                 "  \"saved_at\": 5,\n  \"revision\": 6\n}");
    CHECK(pc_recovery_info_from_json(json.data, json.len, &back, NULL) == PC_OK);
    CHECK(pc_str_eq(&back.key, &i.key) && back.saved_at == 5 && back.revision == 6 &&
          !back.has_original_path);
    pc_recovery_info_fini(&back);
    CHECK(pc_recovery_info_from_json((const uint8_t *)"{\"key\":\"k\"}", 11, &back, NULL) != PC_OK);
    pc_recovery_info_fini(&i);
    pc_buf_fini(&json);
}

int main(int argc, char **argv)
{
    RUN(test_chained_steps_run_in_order);
    RUN(test_partial_chain_from_middle);
    RUN(test_too_new_and_invalid_versions);
    RUN(test_current_version_is_noop);
    RUN(test_older_channels_load_with_default_options);
    RUN(test_unhex_follows_from_str_radix);
    RUN(test_reserve_ids);
    RUN(test_check_nesting_bounds);
    RUN(test_parse_error_order);
    RUN(test_every_field_roundtrips);
    RUN(test_sanitize_keys);
    RUN(test_recovery_info_json);
    return pc_t_finish();
}
