/* ET3: transform/file-dependent image commands -- Image Rotation > Arbitrary
 * (image.rotation.arbitrary), the rotated Crop (image.crop with an angle) and Straighten Layer
 * (image.analysis.straightenLayer). Behaviour plus adversarial params (never crash). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "doc/comps.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "raster/surface.h"
#include "harness.h"

static pc_status exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    pc_json_value_fini(&out);
    return rc;
}

static pc_engine_session *mk(int w, int h, const char *bg)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char buf[128];
    (void)snprintf(buf, sizeof buf, "{\"width\":%d,\"height\":%d,\"background\":\"%s\"}", w, h, bg);
    CHECK(exec(s, "file.new", buf) == PC_OK);
    return s;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

static size_t past_len(pc_engine_session *s)
{
    return pc_history_past_len(pc_engine_session_active(s)->history);
}

/* ---- rotated_size (mode_cmds::rotated_size) ------------------------------------------------ */

static void test_rotated_size(void)
{
    pc_size a = pc_engine_rotated_size(pc_size_make(40, 20), 90.0);
    CHECK(a.width == 20 && a.height == 40); /* a right angle swaps the sides exactly */
    pc_size b = pc_engine_rotated_size(pc_size_make(40, 20), -90.0);
    CHECK(b.width == 20 && b.height == 40);
    pc_size c = pc_engine_rotated_size(pc_size_make(100, 100), 45.0);
    CHECK(c.width > 100 && c.height > 100); /* a 45 deg turn grows the canvas */
    pc_size d = pc_engine_rotated_size(pc_size_make(10, 10), 0.0);
    CHECK(d.width == 10 && d.height == 10);
    /* Never produces a zero side. */
    pc_size e = pc_engine_rotated_size(pc_size_make(1, 1), 30.0);
    CHECK(e.width >= 1 && e.height >= 1);
}

/* ---- image.rotation.arbitrary -------------------------------------------------------------- */

static void test_rotate_right_angle(void)
{
    pc_engine_session *s = mk(40, 20, "white");
    size_t past = past_len(s);
    /* 90 deg clockwise swaps the sides and makes one undo step. */
    CHECK(exec(s, "image.rotation.arbitrary", "{\"angle\":90,\"direction\":\"cw\"}") == PC_OK);
    CHECK(doc_of(s)->size.width == 20 && doc_of(s)->size.height == 40);
    CHECK(past_len(s) == past + 1);
    /* The Background stays opaque over the full canvas (white fill). */
    float p[4];
    pc_surface_rgba(pc_layer_surface(&doc_of(s)->layers[0]), 1, 1, p);
    CHECK(p[3] == 1.0f);
    /* Four 90 deg turns return to the original size. */
    for (int i = 0; i < 3; i++) {
        CHECK(exec(s, "image.rotation.arbitrary", "{\"angle\":90}") == PC_OK);
    }
    CHECK(doc_of(s)->size.width == 40 && doc_of(s)->size.height == 20);
    pc_engine_session_free(s);
}

static void test_rotate_direction(void)
{
    /* cw by 90 and ccw by 90 both swap sides but are not the same rotation; still never crash. */
    pc_engine_session *cw = mk(30, 10, "white");
    pc_engine_session *ccw = mk(30, 10, "white");
    CHECK(exec(cw, "image.rotation.arbitrary", "{\"angle\":90,\"direction\":\"cw\"}") == PC_OK);
    CHECK(exec(ccw, "image.rotation.arbitrary", "{\"angle\":90,\"direction\":\"ccw\"}") == PC_OK);
    CHECK(doc_of(cw)->size.width == 10 && doc_of(cw)->size.height == 30);
    CHECK(doc_of(ccw)->size.width == 10 && doc_of(ccw)->size.height == 30);
    pc_engine_session_free(cw);
    pc_engine_session_free(ccw);
}

static void test_rotate_noop(void)
{
    pc_engine_session *s = mk(16, 12, "white");
    size_t past = past_len(s);
    /* A zero (or full-turn) rotation changes nothing and records no step. */
    CHECK(exec(s, "image.rotation.arbitrary", "{\"angle\":0}") == PC_OK);
    CHECK(exec(s, "image.rotation.arbitrary", "{\"angle\":360}") == PC_OK);
    CHECK(doc_of(s)->size.width == 16 && doc_of(s)->size.height == 12);
    CHECK(past_len(s) == past);
    pc_engine_session_free(s);
}

static void test_rotate_grows_and_moves_layer(void)
{
    pc_engine_session *s = mk(40, 40, "transparent");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":20,\"height\":40}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#808080\"}") == PC_OK);
    CHECK(exec(s, "select.deselect", "{}") == PC_OK);
    /* A 30 deg turn grows the canvas to fit. */
    CHECK(exec(s, "image.rotation.arbitrary", "{\"angle\":30}") == PC_OK);
    CHECK(doc_of(s)->size.width > 40 && doc_of(s)->size.height > 40);
    pc_engine_session_free(s);
}

static void test_rotate_moves_locked_layer(void)
{
    /* A canvas rotation moves even a position-locked layer (with_released_movement_locks). */
    pc_engine_session *s = mk(30, 20, "transparent");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#3366cc\"}") == PC_OK);
    CHECK(exec(s, "layer.lockLayers", "{\"position\":true}") == PC_OK);
    CHECK(exec(s, "image.rotation.arbitrary", "{\"angle\":90}") == PC_OK);
    CHECK(doc_of(s)->size.width == 20 && doc_of(s)->size.height == 30);
    pc_engine_session_free(s);
}

static void test_rotate_warps_selection(void)
{
    pc_engine_session *s = mk(20, 20, "white");
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":10,\"height\":20}") == PC_OK);
    CHECK(doc_of(s)->has_selection);
    CHECK(exec(s, "image.rotation.arbitrary", "{\"angle\":90}") == PC_OK);
    /* The selection follows the canvas and is still present. */
    CHECK(doc_of(s)->has_selection);
    pc_engine_session_free(s);
}

/* ---- rotated crop -------------------------------------------------------------------------- */

static void test_rotated_crop_round_trip(void)
{
    pc_engine_session *s = mk(40, 20, "white");
    size_t past = past_len(s);
    CHECK(exec(s, "image.crop",
               "{\"x\":5,\"y\":2,\"width\":12,\"height\":8,\"angle\":45}") == PC_OK);
    CHECK(doc_of(s)->size.width == 12 && doc_of(s)->size.height == 8);
    CHECK(past_len(s) == past + 1); /* rotation + crop collapse into one step */
    CHECK(pc_engine_session_undo(s));
    CHECK(doc_of(s)->size.width == 40 && doc_of(s)->size.height == 20);
    pc_engine_session_free(s);
}

/* ---- Straighten Layer ---------------------------------------------------------------------- */

static void test_straighten(void)
{
    pc_engine_session *s = mk(40, 30, "white");
    /* No ruler: refused gracefully. */
    CHECK(exec(s, "image.analysis.straightenLayer", "{}") != PC_OK);
    CHECK(exec(s, "image.analysis.rulerTool", "{\"start\":[0,0],\"end\":[40,10]}") == PC_OK);
    size_t past = past_len(s);
    /* crop=true rotates the canvas level and crops to the image (one undo step); clears the ruler. */
    CHECK(exec(s, "image.analysis.straightenLayer", "{\"crop\":true}") == PC_OK);
    CHECK(past_len(s) == past + 1);
    CHECK(!doc_of(s)->measurement.has_ruler);
    pc_engine_session_free(s);
}

static void test_straighten_no_crop(void)
{
    pc_engine_session *s = mk(40, 30, "white");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#222222\"}") == PC_OK);
    CHECK(exec(s, "image.analysis.rulerTool", "{\"start\":[0,0],\"end\":[40,8]}") == PC_OK);
    pc_size before = doc_of(s)->size;
    /* crop=false transforms the active layer level without changing the canvas size. */
    CHECK(exec(s, "image.analysis.straightenLayer", "{\"crop\":false}") == PC_OK);
    CHECK(doc_of(s)->size.width == before.width && doc_of(s)->size.height == before.height);
    CHECK(!doc_of(s)->measurement.has_ruler);
    pc_engine_session_free(s);
}

/* ---- artboards ----------------------------------------------------------------------------- */

static const pc_artboard *active_artboard_of(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    pc_layer_id ab;
    if (!pc_document_artboard_of(doc_of(s), d->active_layer, &ab)) {
        return NULL;
    }
    return pc_layer_artboard(pc_document_layer(doc_of(s), ab));
}

static void test_new_artboard(void)
{
    pc_engine_session *s = mk(400, 300, "white");
    CHECK(!pc_document_has_artboards(doc_of(s)));
    /* A board from a preset, placed at the origin (the first one). */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "layer.new.artboard", "{\"preset\":\"A4\",\"name\":\"Board A\"}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_document_has_artboards(doc_of(s)));
    const pc_artboard *a = active_artboard_of(s);
    CHECK(a != NULL);
    CHECK(pc_rect_width(a->rect) == 595 && pc_rect_height(a->rect) == 842); /* A4 */
    /* A second board goes to the right of the first with a 100 px gap. */
    CHECK(exec(s, "layer.new.artboard", "{\"width\":200,\"height\":200}") == PC_OK);
    const pc_artboard *b = active_artboard_of(s);
    CHECK(b != NULL && b->rect.x0 == 595 + 100);
    /* The canvas grew to hold both boards. */
    CHECK(doc_of(s)->size.width >= 595 + 100 + 200);
    /* Unknown preset refused. */
    CHECK(exec(s, "layer.new.artboard", "{\"preset\":\"nope\"}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_artboard_from_layers(void)
{
    pc_engine_session *s = mk(300, 300, "transparent");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "select.rect", "{\"x\":10,\"y\":10,\"width\":50,\"height\":40}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#808080\"}") == PC_OK);
    CHECK(exec(s, "select.deselect", "{}") == PC_OK);
    CHECK(exec(s, "layer.new.artboardFromLayers", "{\"name\":\"Wrapped\"}") == PC_OK);
    CHECK(pc_document_has_artboards(doc_of(s)));
    const pc_artboard *a = active_artboard_of(s);
    CHECK(a != NULL);
    /* The board bounds the painted content. */
    CHECK(pc_rect_width(a->rect) >= 50 && pc_rect_height(a->rect) >= 40);
    /* The selection is already in an artboard: refused. */
    CHECK(exec(s, "layer.new.artboardFromLayers", "{}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_artboard_from_group(void)
{
    pc_engine_session *s = mk(300, 200, "transparent");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#334455\"}") == PC_OK);
    CHECK(exec(s, "layer.new.groupFromLayers", "{}") == PC_OK);
    /* The active layer is now a plain top-level group. */
    CHECK(exec(s, "layer.new.artboardFromGroup", "{\"background\":\"black\"}") == PC_OK);
    const pc_artboard *a = active_artboard_of(s);
    CHECK(a != NULL && a->background.kind == PC_ARTBOARD_BLACK);
    pc_engine_session_free(s);
}

static void test_artboard_set_and_add_beside(void)
{
    pc_engine_session *s = mk(400, 400, "white");
    CHECK(exec(s, "layer.new.artboard", "{\"x\":0,\"y\":0,\"width\":100,\"height\":100}") == PC_OK);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    pc_layer_id board = d->active_layer;
    /* Edit the board: move + resize + custom background. */
    CHECK(exec(s, "layer.artboard.set",
               "{\"x\":20,\"y\":30,\"width\":150,\"height\":120,\"background\":\"custom\","
               "\"color\":[0.2,0.4,0.6]}") == PC_OK);
    const pc_artboard *a = active_artboard_of(s);
    CHECK(a != NULL && a->rect.x0 == 20 && a->rect.y0 == 30);
    CHECK(pc_rect_width(a->rect) == 150 && pc_rect_height(a->rect) == 120);
    CHECK(a->background.kind == PC_ARTBOARD_CUSTOM);
    /* Add a board to the right; it is a new, selected artboard. */
    size_t before = 0;
    pc_artboard_ref *refs = NULL;
    CHECK(pc_document_artboards(doc_of(s), &refs, &before, NULL) == PC_OK);
    free(refs);
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"side\":\"right\"}", (unsigned long long)board);
    CHECK(exec(s, "layer.artboard.addBeside", buf) == PC_OK);
    size_t after = 0;
    CHECK(pc_document_artboards(doc_of(s), &refs, &after, NULL) == PC_OK);
    free(refs);
    CHECK(after == before + 1);
    /* Duplicate beside (copy with contents). */
    CHECK(exec(s, "layer.artboard.addBeside", "{\"side\":\"bottom\",\"duplicate\":true}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_artboards_export(void)
{
    pc_engine_session *s = mk(200, 200, "white");
    CHECK(exec(s, "layer.new.artboard", "{\"x\":0,\"y\":0,\"width\":80,\"height\":60,\"name\":\"One\"}") ==
          PC_OK);
    CHECK(exec(s, "layer.new.artboard", "{\"width\":80,\"height\":60,\"name\":\"Two\"}") == PC_OK);
    const char *dir = "/dev/shm/pc-build/et3-artboards";
    (void)mkdir("/dev/shm/pc-build", 0777);
    (void)mkdir(dir, 0777);
    char buf[256];
    (void)snprintf(buf, sizeof buf, "{\"dir\":\"%s\",\"format\":\"png\",\"prefix\":\"ab\"}", dir);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.export.artboardsToFiles", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    /* One PDF with a page per board. */
    (void)snprintf(buf, sizeof buf, "{\"path\":\"%s/boards.pdf\"}", dir);
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.export.artboardsToPdf", buf, &o2, NULL) == PC_OK);
    pc_json_value_fini(&o2);
    FILE *f = fopen("/dev/shm/pc-build/et3-artboards/boards.pdf", "rb");
    CHECK(f != NULL);
    if (f != NULL) {
        char hdr[5] = {0};
        CHECK(fread(hdr, 1, 4, f) == 4);
        CHECK(memcmp(hdr, "%PDF", 4) == 0);
        fclose(f);
    }
    /* Export with no artboards is refused. */
    pc_engine_session *empty = mk(50, 50, "white");
    CHECK(exec(empty, "file.export.artboardsToPdf", "{\"path\":\"/dev/shm/pc-build/x.pdf\"}") != PC_OK);
    pc_engine_session_free(empty);
    pc_engine_session_free(s);
}

/* ---- Crop and Straighten Photos ------------------------------------------------------------ */

static void test_crop_and_straighten(void)
{
    /* A uniform scan has no photos: a graceful error. */
    pc_engine_session *s = mk(120, 120, "white");
    CHECK(exec(s, "file.automate.cropAndStraightenPhotos", "{}") != PC_OK);
    /* Paint one dark rectangle in the middle (a "photo" on a white flatbed). */
    CHECK(exec(s, "select.rect", "{\"x\":30,\"y\":30,\"width\":60,\"height\":45}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#303030\"}") == PC_OK);
    CHECK(exec(s, "select.deselect", "{}") == PC_OK);
    size_t before = 0;
    (void)pc_engine_session_documents(s, &before);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.automate.cropAndStraightenPhotos", "{}", &out, NULL) ==
          PC_OK);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(out.doc);
    yyjson_mut_val *photos = yyjson_mut_obj_get(root, "photos");
    CHECK(photos != NULL && yyjson_mut_arr_size(photos) >= 1);
    pc_json_value_fini(&out);
    /* Each found photo became its own new document. */
    size_t after = 0;
    (void)pc_engine_session_documents(s, &after);
    CHECK(after > before);
    pc_engine_session_free(s);
}

/* ---- adversarial: never crash, always a status --------------------------------------------- */

static void test_adversarial(void)
{
    static const char *const cmds[] = {
        "image.rotation.arbitrary", "image.crop", "image.analysis.straightenLayer",
        "layer.new.artboard", "layer.new.artboardFromGroup", "layer.new.artboardFromLayers",
        "layer.artboard.set", "layer.artboard.addBeside", "file.export.artboardsToFiles",
        "file.export.artboardsToPdf", "file.automate.cropAndStraightenPhotos",
        "file.automate.photomerge", "file.automate.mergeToHdrPro",
    };
    static const char *const params[] = {
        "{}", "null", "7", "\"x\"", "[]",
        "{\"angle\":1e308}", "{\"angle\":null}", "{\"angle\":-1e308}",
        "{\"angle\":90,\"direction\":\"sideways\"}", "{\"angle\":99999}",
        "{\"angle\":45,\"interpolation\":\"wat\"}",
        "{\"x\":0,\"y\":0,\"width\":-5,\"height\":10,\"angle\":30}",
        "{\"x\":0,\"y\":0,\"width\":5,\"height\":10,\"angle\":30}",
        "{\"angle\":30}", "{\"crop\":\"yes\"}", "{\"angle\":[1,2]}",
        "{\"x\":2147483647,\"y\":2147483647,\"width\":100,\"height\":100,\"angle\":17}",
        "{\"rect\":[0,0,-5,10]}", "{\"rect\":[1,2,3]}", "{\"preset\":123}",
        "{\"side\":\"diagonal\"}", "{\"background\":\"rainbow\"}", "{\"color\":\"#zz\"}",
        "{\"width\":99999999999,\"height\":99999999999}", "{\"dir\":\"/dev/shm/pc-build\"}",
        "{\"path\":\"/dev/shm/pc-build/adv.pdf\"}", "{\"moveContents\":false,\"x\":2000000000}",
    };
    pc_engine_session *s = mk(24, 24, "white");
    (void)exec(s, "layer.new.layer", "{}");
    (void)exec(s, "image.analysis.rulerTool", "{\"start\":[0,0],\"end\":[20,7]}");
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            (void)exec(s, cmds[i], params[j]); /* must return a status, never crash */
        }
    }
    pc_engine_session_free(s);
    /* Same on an empty session. */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            (void)exec(empty, cmds[i], params[j]);
        }
    }
    pc_engine_session_free(empty);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_rotated_size);
    RUN(test_rotate_right_angle);
    RUN(test_rotate_direction);
    RUN(test_rotate_noop);
    RUN(test_rotate_grows_and_moves_layer);
    RUN(test_rotate_moves_locked_layer);
    RUN(test_rotate_warps_selection);
    RUN(test_rotated_crop_round_trip);
    RUN(test_straighten);
    RUN(test_straighten_no_crop);
    RUN(test_new_artboard);
    RUN(test_artboard_from_layers);
    RUN(test_artboard_from_group);
    RUN(test_artboard_set_and_add_beside);
    RUN(test_artboards_export);
    RUN(test_crop_and_straighten);
    RUN(test_adversarial);
    return pc_t_finish();
}
