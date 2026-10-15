/* E-tail BATCH: the file batch / automate pipeline (batch_cmds.c). Behaviour round-trips over real
 * temp files plus adversarial params for every registered command (never crash, REWRITE_PLAN rule 9). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/file.h"
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

static uint64_t ruint(pc_json_value *o, const char *k)
{
    yyjson_mut_val *r = pc_json_value_root(o);
    return r != NULL ? yyjson_mut_get_uint(yyjson_mut_obj_get(r, k)) : 0;
}

static size_t rarrlen(pc_json_value *o, const char *k)
{
    yyjson_mut_val *r = pc_json_value_root(o);
    yyjson_mut_val *a = r != NULL ? yyjson_mut_obj_get(r, k) : NULL;
    return a != NULL ? yyjson_mut_arr_size(a) : 0;
}

static const char *rstr(pc_json_value *o, const char *k)
{
    yyjson_mut_val *r = pc_json_value_root(o);
    return r != NULL ? yyjson_mut_get_str(yyjson_mut_obj_get(r, k)) : NULL;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d != NULL ? pc_doc_arc_get(d->doc) : NULL;
}

/* Base temp directory (writable; /dev/shm under the sanitiser runs). */
static void base_dir(char *out, size_t cap, const char *name)
{
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/dev/shm/pc-build";
    }
    (void)snprintf(out, cap, "%s/pc-batch-%d-%s", base, (int)getpid(), name);
    (void)pc_mkdir_p(out, NULL);
}

/* Write a solid-colour PNG at dir/name; the full path goes to out[cap]. */
static void make_png(const char *dir, const char *name, int w, int h, const char *hex, char *out,
                     size_t cap)
{
    pc_engine_session *t = NULL;
    CHECK(pc_engine_session_new(&t, NULL) == PC_OK);
    char buf[160];
    (void)snprintf(buf, sizeof buf, "{\"width\":%d,\"height\":%d,\"background\":\"%s\"}", w, h, hex);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(t, "file.new", buf, &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    (void)snprintf(out, cap, "%s/%s", dir, name);
    const pc_engine_doc_state *d = pc_engine_session_active(t);
    CHECK(d != NULL);
    CHECK(pc_engine_save_doc(pc_doc_arc_get(d->doc), out, NULL, NULL, NULL) == PC_OK);
    pc_engine_session_free(t);
}

static bool is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* ---- Load Files into Stack + Image Processor + Batch --------------------------------------- */

static void test_load_image_processor_batch(void)
{
    char dir[256];
    base_dir(dir, sizeof dir, "lib");
    char in[300];
    (void)snprintf(in, sizeof in, "%s/in", dir);
    (void)pc_mkdir_p(in, NULL);
    char a[400], b[400], notes[400];
    make_png(in, "a.png", 40, 20, "#ff0000", a, sizeof a);
    make_png(in, "b.png", 30, 30, "#0000ff", b, sizeof b);
    (void)snprintf(notes, sizeof notes, "%s/notes.txt", in);
    CHECK(pc_engine_write_file(notes, "not an image", 12, NULL) == PC_OK);

    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);

    /* Load Files into Stack: one layer per file, widest/tallest size, first file at the bottom. */
    char p[900];
    (void)snprintf(p, sizeof p, "{\"paths\":[\"%s\",\"%s\"]}", a, b);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.scripts.loadFilesIntoStack", p, &o, NULL) == PC_OK);
    CHECK_EQ_UINT(ruint(&o, "layers"), 2);
    pc_json_value_fini(&o);
    const pc_document *d = doc_of(s);
    CHECK(d != NULL && d->size.width == 40 && d->size.height == 30);
    CHECK(d->nlayers == 2);
    CHECK_EQ_STR(d->layers[0].name.ptr, "a.png");

    /* Image Processor: fit into 20x20 JPEGs, never enlarging. */
    char out[300];
    (void)snprintf(out, sizeof out, "%s/jpg", dir);
    (void)snprintf(p, sizeof p,
                   "{\"input\":\"%s\",\"output\":\"%s\",\"format\":\"jpg\",\"width\":20,\"height\":20}",
                   in, out);
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.scripts.imageProcessor", p, &o2, NULL) == PC_OK);
    CHECK_EQ_UINT(rarrlen(&o2, "files"), 2);
    CHECK_EQ_UINT(rarrlen(&o2, "errors"), 0);
    pc_json_value_fini(&o2);
    char fitted[512];
    (void)snprintf(fitted, sizeof fitted, "%s/a.jpg", out);
    CHECK(is_file(fitted));

    /* Batch: run an action (invert) over the folder, save PNGs. */
    (void)snprintf(out, sizeof out, "%s/png", dir);
    (void)snprintf(p, sizeof p,
                   "{\"steps\":[[\"image.adjustments.invert\",{}]],\"input\":\"%s\",\"output\":\"%s\","
                   "\"format\":\"png\"}",
                   in, out);
    pc_json_value o3 = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.automate.batch", p, &o3, NULL) == PC_OK);
    CHECK_EQ_UINT(rarrlen(&o3, "files"), 2);
    CHECK_EQ_UINT(rarrlen(&o3, "errors"), 0);
    pc_json_value_fini(&o3);
    char inv[512];
    (void)snprintf(inv, sizeof inv, "%s/a.png", out);
    CHECK(is_file(inv));
    /* The inverted red is cyan: re-import and read a pixel. */
    pc_buf bytes = PC_BUF_INIT;
    CHECK(pc_engine_read_file(inv, &bytes, NULL) == PC_OK);
    pc_document back;
    CHECK(pc_engine_import("a.png", bytes.data, bytes.len, &back, NULL) == PC_OK);
    pc_buf_fini(&bytes);
    pc_surface flat;
    CHECK(pc_engine_flattened(&back, PC_PIXEL_FORMAT_RGBA32F, &flat, NULL) == PC_OK);
    float px[4] = {0};
    CHECK(pc_surface_read_rgba_into(&flat, pc_rect_make(1, 1, 2, 2), (float (*)[4])px, 1, NULL) ==
          PC_OK);
    CHECK(px[0] < 0.02f && px[1] > 0.98f && px[2] > 0.98f);
    pc_surface_fini(&flat);
    pc_document_fini(&back);

    /* A recorded Quick Export is skipped (Batch owns the destination). */
    (void)snprintf(out, sizeof out, "%s/quick", dir);
    (void)snprintf(p, sizeof p,
                   "{\"steps\":[[\"image.adjustments.invert\",{}],[\"file.export.quickExport\",{}]],"
                   "\"input\":\"%s\",\"output\":\"%s\",\"format\":\"png\"}",
                   in, out);
    pc_json_value o4 = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.automate.batch", p, &o4, NULL) == PC_OK);
    CHECK_EQ_UINT(rarrlen(&o4, "files"), 2);
    CHECK_EQ_UINT(rarrlen(&o4, "errors"), 0);
    pc_json_value_fini(&o4);

    /* An unknown command in the action is refused up front. */
    (void)snprintf(p, sizeof p,
                   "{\"steps\":[[\"no.such.command\",{}]],\"input\":\"%s\",\"output\":\"%s\"}", in,
                   out);
    CHECK(exec(s, "file.automate.batch", p) != PC_OK);
    pc_engine_session_free(s);
}

/* ---- OutputClaims: inputs that share an output name ---------------------------------------- */

static void test_output_name_collisions(void)
{
    char dir[256];
    base_dir(dir, sizeof dir, "collide");
    char in[300];
    (void)snprintf(in, sizeof in, "%s/in", dir);
    (void)pc_mkdir_p(in, NULL);
    char path[400];
    make_png(in, "a.png", 8, 8, "#ff0000", path, sizeof path);
    make_png(in, "a.jpg", 8, 8, "#00ff00", path, sizeof path); /* shares stem "a" */
    make_png(in, "B.png", 8, 8, "#00ff00", path, sizeof path);
    make_png(in, "b.tif", 8, 8, "#00ff00", path, sizeof path); /* "B.png"/"b.tif" collide case-wise */

    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char out[300], p[700];
    (void)snprintf(out, sizeof out, "%s/jpg", dir);
    (void)snprintf(p, sizeof p, "{\"steps\":[],\"format\":\"jpg\",\"input\":\"%s\",\"output\":\"%s\"}",
                   in, out);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.automate.batch", p, &o, NULL) == PC_OK);
    CHECK_EQ_UINT(rarrlen(&o, "files"), 2);
    CHECK_EQ_UINT(rarrlen(&o, "errors"), 2);
    /* Each error names the collision. */
    yyjson_mut_val *root = pc_json_value_root(&o);
    yyjson_mut_val *errs = yyjson_mut_obj_get(root, "errors");
    size_t idx, max;
    yyjson_mut_val *e;
    yyjson_mut_arr_foreach(errs, idx, max, e) {
        const char *msg = yyjson_mut_get_str(yyjson_mut_obj_get(e, "error"));
        CHECK(msg != NULL);
        CHECK_CONTAINS(msg, "not written");
    }
    pc_json_value_fini(&o);

    /* Keeping each file's format, nothing collides: 4 files, 0 errors. */
    (void)snprintf(out, sizeof out, "%s/same", dir);
    (void)snprintf(p, sizeof p, "{\"steps\":[],\"input\":\"%s\",\"output\":\"%s\"}", in, out);
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.automate.batch", p, &o2, NULL) == PC_OK);
    CHECK_EQ_UINT(rarrlen(&o2, "files"), 4);
    CHECK_EQ_UINT(rarrlen(&o2, "errors"), 0);
    pc_json_value_fini(&o2);
    pc_engine_session_free(s);
}

/* ---- Statistics ---------------------------------------------------------------------------- */

static void test_statistics(void)
{
    char dir[256];
    base_dir(dir, sizeof dir, "stats");
    char f0[400], f1[400], f2[400];
    make_png(dir, "img0.png", 16, 12, "#141414", f0, sizeof f0);
    make_png(dir, "img1.png", 16, 12, "#3c3c3c", f1, sizeof f1);
    make_png(dir, "img2.png", 16, 12, "#646464", f2, sizeof f2);

    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char p[1300];
    (void)snprintf(p, sizeof p, "{\"mode\":\"mean\",\"input\":[\"%s\",\"%s\",\"%s\"]}", f0, f1, f2);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.scripts.statistics", p, &o, NULL) == PC_OK);
    CHECK_EQ_UINT(ruint(&o, "images"), 3);
    CHECK_EQ_STR(rstr(&o, "mode"), "mean");
    pc_json_value_fini(&o);
    const pc_document *d = doc_of(s);
    CHECK(d != NULL && d->nlayers == 1);
    CHECK(d->layers[0].content.kind == PC_LAYER_SMART);

    /* An unknown stack mode is refused. */
    (void)snprintf(p, sizeof p, "{\"mode\":\"average\",\"input\":\"%s\"}", dir);
    CHECK(exec(s, "file.scripts.statistics", p) != PC_OK);
    pc_engine_session_free(s);
}

/* ---- Contact Sheet II ---------------------------------------------------------------------- */

static void test_contact_sheet(void)
{
    char dir[256];
    base_dir(dir, sizeof dir, "contact");
    char path[400];
    for (int i = 0; i < 5; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "img%d.png", i);
        make_png(dir, name, 60, 30, "#808080", path, sizeof path);
    }
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char p[700];
    (void)snprintf(p, sizeof p,
                   "{\"input\":\"%s\",\"units\":\"pixels\",\"width\":400,\"height\":300,"
                   "\"resolution\":72,\"columns\":2,\"rows\":2,\"depth\":8,\"fontSize\":10,"
                   "\"rotateForBestFit\":true}",
                   dir);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.automate.contactSheetII", p, &o, NULL) == PC_OK);
    CHECK_EQ_UINT(ruint(&o, "pages"), 2);
    CHECK_EQ_UINT(ruint(&o, "images"), 5);
    /* The first sheet is 400x300 with a thumbnail and a caption per image. */
    yyjson_mut_val *root = pc_json_value_root(&o);
    yyjson_mut_val *docs = yyjson_mut_obj_get(root, "documents");
    uint64_t first = yyjson_mut_get_uint(yyjson_mut_arr_get(docs, 0));
    pc_json_value_fini(&o);
    size_t ndocs = 0;
    const pc_engine_doc_state *all = pc_engine_session_documents(s, &ndocs);
    CHECK((size_t)first < ndocs);
    const pc_document *d = pc_doc_arc_get(all[first].doc);
    CHECK(d->size.width == 400 && d->size.height == 300);
    size_t raster = 0, text = 0;
    for (size_t i = 0; i < d->nlayers; i++) {
        if (d->layers[i].content.kind == PC_LAYER_RASTER) {
            raster++;
        } else if (d->layers[i].content.kind == PC_LAYER_TEXT) {
            text++;
        }
    }
    CHECK(raster == 5); /* the white Background layer plus four thumbnails on the first 2x2 page */
    CHECK(text == 4);   /* a caption per image */

    /* Flattened, caption-free variant: one layer. */
    (void)snprintf(p, sizeof p,
                   "{\"input\":\"%s\",\"units\":\"pixels\",\"width\":200,\"height\":200,"
                   "\"columns\":3,\"rows\":2,\"caption\":false,\"flatten\":true}",
                   dir);
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    pc_error e2 = {0};
    pc_status rc2 = pc_engine_session_execute_json(s, "file.automate.contactSheetII", p, &o2, &e2);
    if (rc2 != PC_OK) {
        PC_T_FAIL("contactSheetII flatten: %s", e2.msg);
    }
    yyjson_mut_val *r2 = pc_json_value_root(&o2);
    uint64_t fd = yyjson_mut_get_uint(yyjson_mut_arr_get(yyjson_mut_obj_get(r2, "documents"), 0));
    pc_json_value_fini(&o2);
    all = pc_engine_session_documents(s, &ndocs);
    CHECK((size_t)fd < ndocs);
    CHECK(pc_doc_arc_get(all[fd].doc)->nlayers == 1);
    pc_engine_session_free(s);
}

/* ---- Create Droplet + Run Droplet ---------------------------------------------------------- */

static void test_droplet(void)
{
    char dir[256];
    base_dir(dir, sizeof dir, "droplet");
    char path[400];
    make_png(dir, "img0.png", 40, 20, "#202020", path, sizeof path);
    make_png(dir, "img1.png", 40, 20, "#404040", path, sizeof path);

    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char dp[300], outdir[300], p[700];
    (void)snprintf(dp, sizeof dp, "%s/Rotate", dir);
    (void)snprintf(outdir, sizeof outdir, "%s/out", dir);
    (void)snprintf(p, sizeof p,
                   "{\"path\":\"%s\",\"steps\":[[\"image.imageRotation.90cw\",{}]],\"format\":\"png\","
                   "\"output\":\"%s\"}",
                   dp, outdir);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.automate.createDroplet", p, &o, NULL) == PC_OK);
    const char *written = rstr(&o, "path");
    CHECK(written != NULL);
    char droplet[400];
    (void)snprintf(droplet, sizeof droplet, "%s", written);
    pc_json_value_fini(&o);
    CHECK_CONTAINS(droplet, "Rotate.pcdroplet");
    CHECK(is_file(droplet));

    /* Run it over the input folder: one rotated output per image. */
    (void)snprintf(p, sizeof p, "{\"droplet\":\"%s\",\"input\":[\"%s\"]}", droplet, dir);
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.automate.runDroplet", p, &o2, NULL) == PC_OK);
    CHECK_EQ_UINT(rarrlen(&o2, "files"), 2);
    pc_json_value_fini(&o2);
    char rotated[512];
    (void)snprintf(rotated, sizeof rotated, "%s/img0.png", outdir);
    CHECK(is_file(rotated));
    /* It rotated 40x20 -> 20x40. */
    pc_buf bytes = PC_BUF_INIT;
    CHECK(pc_engine_read_file(rotated, &bytes, NULL) == PC_OK);
    pc_document back;
    CHECK(pc_engine_import("img0.png", bytes.data, bytes.len, &back, NULL) == PC_OK);
    pc_buf_fini(&bytes);
    CHECK(back.size.width == 20 && back.size.height == 40);
    pc_document_fini(&back);

    /* A bad step id is refused. */
    (void)snprintf(p, sizeof p, "{\"path\":\"%s\",\"steps\":[[\"bogus\",{}]]}", dp);
    CHECK(exec(s, "file.automate.createDroplet", p) != PC_OK);
    pc_engine_session_free(s);
}

/* ---- Lens Correction batch ----------------------------------------------------------------- */

static void test_lens_batch(void)
{
    char dir[256];
    base_dir(dir, sizeof dir, "lens");
    char in[300];
    (void)snprintf(in, sizeof in, "%s/in", dir);
    (void)pc_mkdir_p(in, NULL);
    char path[400];
    make_png(in, "shot0.png", 64, 48, "#7f7f7f", path, sizeof path);
    make_png(in, "shot1.png", 64, 48, "#7f7f7f", path, sizeof path);

    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char out[300], p[700];
    (void)snprintf(out, sizeof out, "%s/out", dir);
    (void)snprintf(p, sizeof p,
                   "{\"input\":\"%s\",\"output\":\"%s\",\"vignetteAmount\":60,"
                   "\"edge\":\"edgeExtension\"}",
                   in, out);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.automate.lensCorrection", p, &o, NULL) == PC_OK);
    CHECK_EQ_UINT(rarrlen(&o, "files"), 2);
    CHECK_EQ_UINT(rarrlen(&o, "errors"), 0);
    pc_json_value_fini(&o);
    char res[512];
    (void)snprintf(res, sizeof res, "%s/shot0.png", out);
    CHECK(is_file(res));
    pc_engine_session_free(s);
}

/* ---- adversarial params (never crash, always a status) ------------------------------------- */

static void test_adversarial(void)
{
    static const char *const IDS[] = {
        "file.automate.batch",      "file.scripts.imageProcessor", "file.scripts.loadFilesIntoStack",
        "file.scripts.statistics",  "file.automate.contactSheetII", "file.automate.createDroplet",
        "file.automate.runDroplet", "file.automate.lensCorrection"};
    static const char *const HOSTILE[] = {
        "{}",
        "null",
        "{\"steps\":5,\"input\":42,\"output\":false}",
        "{\"input\":\"/no/such/folder/really\",\"output\":\"/no/such/either\"}",
        "{\"input\":[],\"output\":\"/dev/shm/pc-build/pc-batch-adv\"}",
        "{\"input\":[1,2,3],\"output\":123}",
        "{\"steps\":[[42,{}]],\"input\":\"/tmp\",\"output\":\"/tmp\"}",
        "{\"path\":\"\",\"steps\":\"nope\"}",
        "{\"droplet\":\"/no/such.pcdroplet\",\"input\":[\"/tmp\"]}",
        "{\"input\":\"/dev/shm\",\"columns\":-5,\"rows\":0,\"width\":-1,\"height\":1e30,\"depth\":99}",
        "{\"mode\":12345,\"input\":{}}",
        "{\"paths\":{},\"createSmartObject\":\"yes\"}"};
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        for (size_t j = 0; j < sizeof HOSTILE / sizeof HOSTILE[0]; j++) {
            pc_engine_session *s = NULL;
            CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error err = {0};
            /* Any status is fine; the requirement is no crash / no UB / no abort. */
            (void)pc_engine_session_execute_json(s, IDS[i], HOSTILE[j], &out, &err);
            pc_json_value_fini(&out);
            pc_engine_session_free(s);
        }
    }
}

int main(int argc, char **argv)
{
    RUN(test_load_image_processor_batch);
    RUN(test_output_name_collisions);
    RUN(test_statistics);
    RUN(test_contact_sheet);
    RUN(test_droplet);
    RUN(test_lens_batch);
    RUN(test_adversarial);
    return pc_t_finish();
}
