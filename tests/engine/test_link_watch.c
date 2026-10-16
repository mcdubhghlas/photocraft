/* Linked smart objects follow their files (E6, port of crates/engine/src/link_watch.rs tests):
 * layer.smartObjects.updateChanged stamps the active document's linked files and re-renders the
 * ones whose size or modification time moved, one Update Modified Content undo step per file.
 *
 * The reference's background worker thread (Session::start_link_scan / poll_link_scan, the desktop
 * app's two-second / window-focus look) belongs to the desktop shell (Phase 8) and is deferred
 * (REWRITE_PLAN.md §7), so its test (a_background_look_updates_changed_files) is not ported; the
 * synchronous apply exercised here is the detection path that thread feeds into, unchanged. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "base/json.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "geom/geom.h"
#include "harness.h"
#include "ops/ops.h"

static const char *tmp_dir(void)
{
    const char *d = getenv("TMPDIR");
    return (d != NULL && d[0] != '\0') ? d : "/tmp";
}

static void ok(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    pc_json_value_fini(&out);
}

/* Run updateChanged; report the number of updated layers and the first id. */
static void update_changed(pc_engine_session *s, size_t *n, uint64_t *first)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, "layer.smartObjects.updateChanged", "{}", &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    yyjson_mut_val *root = pc_json_value_root(&out);
    yyjson_mut_val *arr = root != NULL ? yyjson_mut_obj_get(root, "updated") : NULL;
    size_t cnt = arr != NULL ? yyjson_mut_arr_size(arr) : 0;
    if (n != NULL) {
        *n = cnt;
    }
    if (first != NULL && cnt > 0) {
        *first = yyjson_mut_get_uint(yyjson_mut_arr_get(arr, 0));
    }
    pc_json_value_fini(&out);
}

/* Write a solid-colour w x h PNG at `path` (a fresh session: file.new + Save a Copy). */
static void make_png(const char *path, uint32_t w, uint32_t h, const char *hex)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char json[256];
    snprintf(json, sizeof json, "{\"width\":%u,\"height\":%u,\"background\":\"%s\"}", w, h, hex);
    ok(s, "file.new", json);
    snprintf(json, sizeof json, "{\"path\":\"%s\",\"layers\":false}", path);
    ok(s, "file.saveACopy", json);
    pc_engine_session_free(s);
}

static size_t read_all(const char *path, uint8_t **buf)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    CHECK(len > 0);
    uint8_t *b = malloc((size_t)len);
    CHECK(b != NULL);
    CHECK(fread(b, 1, (size_t)len, f) == (size_t)len);
    fclose(f);
    *buf = b;
    return (size_t)len;
}

/* Write `len` bytes to `path`, after a short sleep so the modification time moves even on coarse
 * file systems (the reference's save()). */
static void save_bytes(const char *path, const uint8_t *buf, size_t len)
{
    struct timespec ts = {0, 30L * 1000L * 1000L};
    nanosleep(&ts, NULL);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(buf, 1, len, f) == len);
    fclose(f);
}

static void save_png(const char *path, uint32_t w, uint32_t h, const char *hex)
{
    char tmp[560];
    snprintf(tmp, sizeof tmp, "%s.src.png", path);
    make_png(tmp, w, h, hex);
    uint8_t *bytes = NULL;
    size_t len = read_all(tmp, &bytes);
    save_bytes(path, bytes, len);
    free(bytes);
    unlink(tmp);
}

/* A 40x40 white document with the file at `path` placed linked -> (session, layer id). */
static pc_engine_session *placed(const char *path, pc_layer_id *id)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    ok(s, "file.new", "{\"width\":40,\"height\":40}");
    char json[640];
    snprintf(json, sizeof json, "{\"path\":\"%s\"}", path);
    ok(s, "file.placeLinked", json);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL && d->has_active_layer);
    *id = d->active_layer;
    return s;
}

/* The composited centre pixel (document 20,20) of the active document. */
static void centre(pc_engine_session *s, float out[3])
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_compose_buffer b = {0};
    CHECK(pc_compose_render(doc, pc_rect_make(20, 20, 21, 21), &b, NULL) == PC_OK);
    const float *p = pc_compose_buffer_get(&b, 20, 20);
    CHECK(p != NULL);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    pc_compose_buffer_fini(&b);
}

static bool is_rgb(const float c[3], float r, float g, float b)
{
    return fabsf(c[0] - r) < 0.02f && fabsf(c[1] - g) < 0.02f && fabsf(c[2] - b) < 0.02f;
}

/* A path in the per-process temp dir, with a distinct name per test (tests may run in parallel). */
static void art_path(char *out, size_t cap, const char *tag)
{
    snprintf(out, cap, "%s/pc_linkwatch_%s_%ld.png", tmp_dir(), tag, (long)getpid());
}

static void test_a_linked_smart_object_follows_its_file(void)
{
    char path[512];
    art_path(path, sizeof path, "follow");
    make_png(path, 10, 10, "#ff0000");
    pc_layer_id id = 0;
    pc_engine_session *s = placed(path, &id);

    float c[3];
    centre(s, c);
    CHECK(is_rgb(c, 1, 0, 0));

    /* First look: remembered, nothing to update. */
    size_t n = 1;
    update_changed(s, &n, NULL);
    CHECK(n == 0);

    save_png(path, 10, 10, "#0000ff");
    uint64_t first = 0;
    update_changed(s, &n, &first);
    CHECK(n == 1 && first == id);
    centre(s, c);
    CHECK(is_rgb(c, 0, 0, 1));

    /* An Update Modified Content step: undoing it is respected until the file changes again. */
    CHECK(pc_engine_session_undo(s));
    centre(s, c);
    CHECK(is_rgb(c, 1, 0, 0));
    update_changed(s, &n, NULL);
    CHECK(n == 0);
    centre(s, c);
    CHECK(is_rgb(c, 1, 0, 0));

    pc_engine_session_free(s);
    unlink(path);
}

static void test_a_file_still_being_written_is_tried_again(void)
{
    char path[512];
    art_path(path, sizeof path, "partial");
    make_png(path, 10, 10, "#ff0000");
    pc_layer_id id = 0;
    pc_engine_session *s = placed(path, &id);
    size_t n;
    update_changed(s, &n, NULL);

    /* Another app has only written the start of the file so far. */
    char full_path[560];
    snprintf(full_path, sizeof full_path, "%s.full.png", path);
    make_png(full_path, 10, 10, "#0000ff");
    uint8_t *full = NULL;
    size_t flen = read_all(full_path, &full);
    CHECK(flen > 16);
    save_bytes(path, full, 16);

    update_changed(s, &n, NULL);
    CHECK(n == 0); /* the partial file can't be decoded: no update */
    float c[3];
    centre(s, c);
    CHECK(is_rgb(c, 1, 0, 0)); /* the last good pixels stay */

    save_bytes(path, full, flen);
    uint64_t first = 0;
    update_changed(s, &n, &first);
    CHECK(n == 1 && first == id);
    centre(s, c);
    CHECK(is_rgb(c, 0, 0, 1));

    free(full);
    unlink(full_path);
    pc_engine_session_free(s);
    unlink(path);
}

static void test_a_document_in_the_back_catches_up_in_front(void)
{
    char path[512];
    art_path(path, sizeof path, "back");
    make_png(path, 10, 10, "#ff0000");
    pc_layer_id id = 0;
    pc_engine_session *s = placed(path, &id);
    size_t n;
    update_changed(s, &n, NULL);

    /* A second document comes to the front. */
    ok(s, "file.new", "{\"width\":10,\"height\":10}");
    save_png(path, 10, 10, "#0000ff");
    update_changed(s, &n, NULL);
    CHECK(n == 0); /* the file belongs to the backgrounded document */

    CHECK(pc_engine_session_set_active(s, 0));
    uint64_t first = 0;
    update_changed(s, &n, &first);
    CHECK(n == 1 && first == id);
    float c[3];
    centre(s, c);
    CHECK(is_rgb(c, 0, 0, 1));

    pc_engine_session_free(s);
    unlink(path);
}

/* Graceful failure: no crash on a missing document, a deleted/corrupt linked file, or hostile
 * params (Rule 9). */
static void test_graceful_failure(void)
{
    /* No document: the command errors, it does not crash. */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(empty, "layer.smartObjects.updateChanged"));
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(empty, "layer.smartObjects.updateChanged", "{}", &out, &err) != PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(empty);

    /* A linked file that is deleted: no update, no crash. */
    char path[512];
    art_path(path, sizeof path, "gone");
    make_png(path, 10, 10, "#00ff00");
    pc_layer_id id = 0;
    pc_engine_session *s = placed(path, &id);
    size_t n;
    update_changed(s, &n, NULL);
    unlink(path);
    update_changed(s, &n, NULL);
    CHECK(n == 0); /* the missing file keeps its last stamp, nothing re-renders */

    /* The file comes back corrupt (changed, but undecodable): no update, no crash. */
    save_bytes(path, (const uint8_t *)"not a png at all, just junk bytes", 33);
    update_changed(s, &n, NULL);
    CHECK(n == 0);

    /* Hostile params are ignored (the command takes none). */
    static const char *HOSTILE[] = {"null", "[]", "123", "\"x\"", "{\"layer\":999}", "{\"path\":[1]}"};
    for (size_t i = 0; i < sizeof HOSTILE / sizeof HOSTILE[0]; i++) {
        pc_json_value o = PC_JSON_VALUE_INIT;
        pc_error e = {0};
        (void)pc_engine_session_execute_json(s, "layer.smartObjects.updateChanged", HOSTILE[i], &o, &e);
        pc_json_value_fini(&o);
    }
    pc_engine_session_free(s);
    unlink(path);
    CHECK(true);
}

static void test_registration(void)
{
    char path[512];
    art_path(path, sizeof path, "reg");
    make_png(path, 10, 10, "#ff0000");
    pc_layer_id id = 0;
    pc_engine_session *s = placed(path, &id);
    /* The command is registered and enabled with a document open. */
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.updateChanged"));
    pc_engine_session_free(s);
    unlink(path);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registration);
    RUN(test_a_linked_smart_object_follows_its_file);
    RUN(test_a_file_still_being_written_is_tried_again);
    RUN(test_a_document_in_the_back_catches_up_in_front);
    RUN(test_graceful_failure);
    return pc_t_finish();
}
