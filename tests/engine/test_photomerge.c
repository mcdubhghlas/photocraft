/* File > Automate > Photomerge and Merge to HDR Pro (port of the crates/engine/src/photo_cmds.rs
 * tests: photomerge_open_documents_reposition_and_perspective, photomerge_cylindrical_with_fill,
 * hdr_merge_from_synthetic_exposures) plus graceful-failure checks (empty / mismatched / wrong
 * param counts must return an error, never crash). Exercises the panorama + HDR primitives end to
 * end through the engine. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/exif.h"
#include "base/blob.h"
#include "base/buf.h"
#include "color/color.h"
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

static const pc_document *active_doc(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

/* ---- synthetic scene (photo_cmds.rs tests::scene) ----------------------------------------- */

typedef struct rgba {
    float v[4];
} rgba;

static double scene_rnd(uint64_t *z)
{
    *z = *z * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(*z >> 33) / (double)(1u << 31);
}

/* A textured RGB scene (deterministic blobs on a gradient). */
static rgba *scene(size_t w, size_t h, uint64_t seed)
{
    rgba *px = pc_malloc_array(w * h, sizeof *px);
    if (px == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < w * h; i++) {
        float x = (float)(i % w) / (float)w;
        px[i].v[0] = 0.3f + 0.2f * x;
        px[i].v[1] = 0.35f;
        px[i].v[2] = 0.4f - 0.1f * x;
        px[i].v[3] = 1.0f;
    }
    uint64_t z = seed;
    for (size_t k = 0; k < w * h / 700; k++) {
        double cx = scene_rnd(&z) * (double)w, cy = scene_rnd(&z) * (double)h;
        double r = 3.0 + scene_rnd(&z) * 9.0;
        float col[4] = {(float)scene_rnd(&z), (float)scene_rnd(&z), (float)scene_rnd(&z), 1.0f};
        size_t y0 = (size_t)(cy - r < 0.0 ? 0.0 : cy - r);
        size_t y1 = (size_t)(cy + r) + 1;
        if (y1 > h) {
            y1 = h;
        }
        size_t x0 = (size_t)(cx - r < 0.0 ? 0.0 : cx - r);
        size_t x1 = (size_t)(cx + r) + 1;
        if (x1 > w) {
            x1 = w;
        }
        for (size_t y = y0; y < y1; y++) {
            for (size_t x = x0; x < x1; x++) {
                if (hypot((double)x - cx, (double)y - cy) < r) {
                    memcpy(px[y * w + x].v, col, sizeof col);
                }
            }
        }
    }
    return px;
}

/* A rectangular crop of the scene, scaled by `gain` and forced opaque. */
static rgba *crop_scene(const rgba *px, size_t sw, size_t x0, size_t y0, size_t w, size_t h, float gain)
{
    rgba *out = pc_malloc_array(w * h, sizeof *out);
    if (out == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < w * h; i++) {
        const rgba *q = &px[(y0 + i / w) * sw + x0 + i % w];
        out[i].v[0] = q->v[0] * gain;
        out[i].v[1] = q->v[1] * gain;
        out[i].v[2] = q->v[2] * gain;
        out[i].v[3] = 1.0f;
    }
    return out;
}

/* Add an RGB document filled from the pixel array; returns it active. */
static void add_doc(pc_engine_session *s, const char *name, const rgba *px, size_t w, size_t h,
                    pc_sample_type depth, const pc_algo_camera_info *exif)
{
    pc_document doc;
    CHECK(pc_document_init(&doc, name, pc_size_make((uint32_t)w, (uint32_t)h), PC_COLOR_MODE_RGB, depth,
                           NULL) == PC_OK);
    if (exif != NULL) {
        pc_buf buf = PC_BUF_INIT;
        CHECK(pc_algo_exif_build(exif, &buf, NULL) == PC_OK);
        pc_blob *blob = NULL;
        CHECK(pc_blob_new(buf.data, buf.len, &blob, NULL) == PC_OK);
        doc.metadata.exif = blob;
        pc_buf_fini(&buf);
    }
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    size_t nch = pc_pixel_format_channels(fmt);
    float *data = pc_malloc_array(w * h * nch, sizeof *data);
    CHECK(data != NULL);
    for (size_t i = 0; i < w * h; i++) {
        for (size_t c = 0; c < nch && c < 4; c++) {
            data[i * nch + c] = px[i].v[c];
        }
    }
    pc_layer bg;
    CHECK(pc_layer_raster(&bg, "Background", fmt, NULL) == PC_OK);
    pc_surface *surf = pc_layer_surface_mut(&bg);
    CHECK(pc_surface_write_region(surf, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), data, w * h * nch,
                                  NULL) == PC_OK);
    free(data);
    pc_layer_id id = 0;
    CHECK(pc_document_insert_above(&doc, NULL, &bg, &id, NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
}

static yyjson_mut_val *root_of(pc_json_value *out)
{
    return yyjson_mut_doc_get_root(out->doc);
}

/* ---- Photomerge: Reposition (gain compensation) and Perspective --------------------------- */

static void test_photomerge_reposition(void)
{
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t di = 0; di < 3; di++) {
        size_t sw = 520, sh = 220;
        rgba *sc = scene(sw, sh, 5);
        CHECK(sc != NULL);
        pc_engine_session *s = NULL;
        CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
        struct {
            size_t x0;
            float g;
        } crops[3] = {{0, 1.0f}, {130, 1.15f}, {260, 1.0f}};
        for (size_t k = 0; k < 3; k++) {
            rgba *c = crop_scene(sc, sw, crops[k].x0, 10, 220, 200, crops[k].g);
            CHECK(c != NULL);
            char nm[16];
            (void)snprintf(nm, sizeof nm, "img%zu", k);
            add_doc(s, nm, c, 220, 200, depths[di], NULL);
            free(c);
        }
        pc_json_value out = PC_JSON_VALUE_INIT;
        CHECK(pc_engine_session_execute_json(
                  s, "file.automate.photomerge",
                  "{\"useOpenDocuments\":true,\"layout\":\"reposition\"}", &out, NULL) == PC_OK);
        yyjson_mut_val *r = root_of(&out);
        yyjson_mut_val *placed = yyjson_mut_obj_get(r, "placed");
        CHECK(yyjson_mut_arr_size(placed) == 3);
        int64_t w = (int64_t)yyjson_mut_get_num(yyjson_mut_obj_get(r, "width"));
        int64_t h = (int64_t)yyjson_mut_get_num(yyjson_mut_obj_get(r, "height"));
        CHECK(llabs(w - 480) <= 2 && llabs(h - 200) <= 2);

        const pc_document *d = active_doc(s);
        CHECK(d->nlayers == 3);
        for (size_t i = 0; i < d->nlayers; i++) {
            CHECK(d->layers[i].has_mask);
        }
        /* Masks partition the panorama: every covered pixel is shown by exactly one layer. */
        struct {
            int32_t x, y;
        } pts[4] = {{20, 100}, {200, 50}, {380, 150}, {470, 120}};
        for (size_t pi = 0; pi < 4; pi++) {
            float shown = 0.0f;
            for (size_t i = 0; i < d->nlayers; i++) {
                float a[4];
                pc_surface_rgba(pc_layer_surface(&d->layers[i]), pts[pi].x, pts[pi].y, a);
                shown += pc_layer_mask_value(&d->layers[i].mask, pts[pi].x, pts[pi].y) * a[3];
            }
            CHECK_NEAR(shown, 1.0f, 0.02f);
        }
        /* Gain compensation: the brighter middle image gets the higher gain. */
        yyjson_mut_val *gains = yyjson_mut_obj_get(yyjson_mut_obj_get(r, "photometric"), "gains");
        CHECK(gains != NULL && yyjson_mut_arr_size(gains) == 3);
        double g0 = yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_arr_get(gains, 0), 0));
        double g1 = yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_arr_get(gains, 1), 0));
        CHECK(g1 > g0 * 1.05);
        pc_json_value_fini(&out);
        pc_engine_session_free(s);
        free(sc);
    }
}

static void test_photomerge_perspective_noblend(void)
{
    size_t sw = 520, sh = 220;
    rgba *sc = scene(sw, sh, 8);
    CHECK(sc != NULL);
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    size_t xs[2] = {0, 180};
    for (size_t k = 0; k < 2; k++) {
        rgba *c = crop_scene(sc, sw, xs[k], 10, 260, 200, 1.0f);
        CHECK(c != NULL);
        char nm[16];
        (void)snprintf(nm, sizeof nm, "p%zu", k);
        add_doc(s, nm, c, 260, 200, PC_SAMPLE_TYPE_U8, NULL);
        free(c);
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "file.automate.photomerge",
              "{\"useOpenDocuments\":true,\"layout\":\"perspective\",\"blend\":false}", &out, NULL) ==
          PC_OK);
    yyjson_mut_val *r = root_of(&out);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(r, "layout")), "perspective");
    int64_t w = (int64_t)yyjson_mut_get_num(yyjson_mut_obj_get(r, "width"));
    CHECK(llabs(w - 440) <= 8);
    const pc_document *d = active_doc(s);
    for (size_t i = 0; i < d->nlayers; i++) {
        CHECK(!d->layers[i].has_mask); /* no blending -> plain warped layers */
    }
    pc_json_value_fini(&out);

    /* An unknown layout, and an empty session, both error (never crash). */
    CHECK(exec(s, "file.automate.photomerge",
               "{\"useOpenDocuments\":true,\"layout\":\"zigzag\"}") != PC_OK);
    pc_engine_session_free(s);
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(exec(empty, "file.automate.photomerge", "{}") != PC_OK);
    pc_engine_session_free(empty);
    free(sc);
}

/* ---- Photomerge: Cylindrical + content-aware fill ----------------------------------------- */

static void test_photomerge_cylindrical_fill(void)
{
    size_t sw = 900, sh = 260;
    rgba *sc = scene(sw, sh, 21);
    CHECK(sc != NULL);
    double f = 260.0;
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    size_t tw = 260, th = 200;
    double pans[3] = {-0.45, 0.0, 0.45};
    for (size_t k = 0; k < 3; k++) {
        rgba *px = pc_malloc_array(tw * th, sizeof *px);
        CHECK(px != NULL);
        for (size_t i = 0; i < tw * th; i++) {
            double dx = (double)(i % tw) - (double)tw / 2.0;
            double dy = (double)(i / tw) - (double)th / 2.0;
            double u = (double)sw / 2.0 + f * (atan2(dx, f) + pans[k]);
            double v = (double)sh / 2.0 + f * dy / hypot(dx, f);
            double uu = round(u);
            double vv = round(v);
            uu = uu < 0.0 ? 0.0 : (uu > (double)sw - 1.0 ? (double)sw - 1.0 : uu);
            vv = vv < 0.0 ? 0.0 : (vv > (double)sh - 1.0 ? (double)sh - 1.0 : vv);
            px[i] = sc[(size_t)vv * sw + (size_t)uu];
        }
        char nm[16];
        (void)snprintf(nm, sizeof nm, "c%zu", k);
        add_doc(s, nm, px, tw, th, PC_SAMPLE_TYPE_U8, NULL);
        free(px);
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "file.automate.photomerge",
              "{\"useOpenDocuments\":true,\"layout\":\"cylindrical\",\"contentAwareFill\":true}", &out,
              NULL) == PC_OK);
    yyjson_mut_val *r = root_of(&out);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(r, "layout")), "cylindrical");
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(r, "placed")) == 3);
    const pc_document *d = active_doc(s);
    CHECK(d->nlayers >= 1);
    const pc_layer *top = &d->layers[d->nlayers - 1]; /* layers bottom-to-top */
    CHECK_EQ_STR(pc_str_c(&top->name), "Content-Aware Fill");
    /* The fill layer is opaque at the bow-tie corners. */
    pc_rect b = pc_document_bounds(d);
    float p0[4], p1[4];
    pc_surface_rgba(pc_layer_surface(top), 1, 1, p0);
    pc_surface_rgba(pc_layer_surface(top), b.x1 - 2, b.y1 - 2, p1);
    CHECK(p0[3] > 0.99f && p1[3] > 0.99f);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
    free(sc);
}

/* ---- Merge to HDR Pro --------------------------------------------------------------------- */

static float srgb_enc(float v)
{
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static void test_hdr_merge(void)
{
    size_t w = 96, h = 64;
    float (*rad)[3] = pc_malloc_array(w * h, sizeof *rad);
    CHECK(rad != NULL);
    for (size_t i = 0; i < w * h; i++) {
        float x = (float)(i % w) / (float)w;
        float b = 0.003f * powf(2.0f, x * 11.0f) * (1.0f + 0.2f * sinf((float)(i / w) * 0.3f));
        rad[i][0] = b;
        rad[i][1] = b * 0.9f;
        rad[i][2] = b * 0.7f;
    }
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    float dts[3] = {0.25f, 1.0f, 4.0f};
    for (size_t k = 0; k < 3; k++) {
        rgba *px = pc_malloc_array(w * h, sizeof *px);
        CHECK(px != NULL);
        for (size_t i = 0; i < w * h; i++) {
            for (int c = 0; c < 3; c++) {
                float v = rad[i][c] * dts[k];
                px[i].v[c] = srgb_enc(v > 1.0f ? 1.0f : v);
            }
            px[i].v[3] = 1.0f;
        }
        pc_algo_camera_info info;
        memset(&info, 0, sizeof info);
        info.exposure_time.set = true;
        info.exposure_time.v = (double)dts[k] / 100.0;
        info.f_number.set = true;
        info.f_number.v = 8.0;
        info.iso.set = true;
        info.iso.v = 100.0;
        char nm[16];
        (void)snprintf(nm, sizeof nm, "e%zu", k);
        add_doc(s, nm, px, w, h, PC_SAMPLE_TYPE_U8, &info);
        free(px);
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.automate.mergeToHdrPro",
                                         "{\"useOpenDocuments\":true}", &out, NULL) == PC_OK);
    yyjson_mut_val *r = root_of(&out);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(r, "exposureSource")), "exif");
    yyjson_mut_val *ev = yyjson_mut_obj_get(r, "ev");
    double ev1 = yyjson_mut_get_num(yyjson_mut_arr_get(ev, 1));
    double ev2 = yyjson_mut_get_num(yyjson_mut_arr_get(ev, 2));
    CHECK_NEAR(ev1, 2.0, 1e-6);
    CHECK_NEAR(ev2, 4.0, 1e-6);
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(r, "stops")) > 7.0);
    const pc_document *d = active_doc(s);
    CHECK(d->depth == PC_SAMPLE_TYPE_F32);
    /* Linear output: ratios across the ramp follow the radiance. */
    float a[4], bb[4];
    pc_surface_rgba(pc_layer_surface(&d->layers[0]), 20, 30, a);
    pc_surface_rgba(pc_layer_surface(&d->layers[0]), 80, 30, bb);
    double expect = (double)rad[30 * w + 80][1] / (double)rad[30 * w + 20][1];
    CHECK(fabs(log((double)bb[1] / (double)a[1] / expect)) < 0.3);
    size_t outdoc = (size_t)yyjson_mut_get_num(yyjson_mut_obj_get(r, "document"));
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_close(s, outdoc));

    /* 8-bit tone-mapped output with explicit EVs, each method. */
    const char *methods[4] = {"localAdaptation", "exposureGamma", "highlightCompression",
                              "equalizeHistogram"};
    for (size_t m = 0; m < 4; m++) {
        char params[256];
        (void)snprintf(params, sizeof params,
                       "{\"paths\":[],\"useOpenDocuments\":true,\"exposures\":[0,2,4],\"mode\":\"8\","
                       "\"method\":\"%s\"}",
                       methods[m]);
        pc_json_value o2 = PC_JSON_VALUE_INIT;
        CHECK(pc_engine_session_execute_json(s, "file.automate.mergeToHdrPro", params, &o2, NULL) ==
              PC_OK);
        yyjson_mut_val *r2 = root_of(&o2);
        CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(r2, "exposureSource")), "params");
        const pc_document *d2 = active_doc(s);
        CHECK(d2->depth == PC_SAMPLE_TYPE_U8);
        size_t od = (size_t)yyjson_mut_get_num(yyjson_mut_obj_get(r2, "document"));
        pc_json_value_fini(&o2);
        CHECK(pc_engine_session_close(s, od));
    }
    /* Wrong number of exposures is an error. */
    CHECK(exec(s, "file.automate.mergeToHdrPro",
               "{\"useOpenDocuments\":true,\"exposures\":[0,1]}") != PC_OK);
    pc_engine_session_free(s);
    free(rad);
}

/* ---- graceful failure --------------------------------------------------------------------- */

static void test_graceful_failure(void)
{
    /* Empty session: both commands error, never crash. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(exec(s, "file.automate.photomerge", "{\"useOpenDocuments\":true}") != PC_OK);
    CHECK(exec(s, "file.automate.mergeToHdrPro", "{\"useOpenDocuments\":true}") != PC_OK);
    CHECK(exec(s, "file.automate.photomerge", "{\"paths\":[]}") != PC_OK);
    CHECK(exec(s, "file.automate.mergeToHdrPro", "{\"paths\":\"/nonexistent-dir-xyz\"}") != PC_OK);

    /* One open document: Photomerge / HDR need two or more. */
    rgba *px = scene(40, 40, 1);
    CHECK(px != NULL);
    add_doc(s, "solo", px, 40, 40, PC_SAMPLE_TYPE_U8, NULL);
    free(px);
    CHECK(exec(s, "file.automate.photomerge", "{\"useOpenDocuments\":true}") != PC_OK);
    CHECK(exec(s, "file.automate.mergeToHdrPro", "{\"useOpenDocuments\":true}") != PC_OK);

    /* Two exposures of different sizes: HDR rejects them. */
    rgba *a = scene(40, 40, 2);
    rgba *b = scene(48, 30, 3);
    CHECK(a != NULL && b != NULL);
    pc_engine_session *s2 = NULL;
    CHECK(pc_engine_session_new(&s2, NULL) == PC_OK);
    add_doc(s2, "a", a, 40, 40, PC_SAMPLE_TYPE_U8, NULL);
    add_doc(s2, "b", b, 48, 30, PC_SAMPLE_TYPE_U8, NULL);
    free(a);
    free(b);
    CHECK(exec(s2, "file.automate.mergeToHdrPro", "{\"useOpenDocuments\":true}") != PC_OK);
    /* A nonsense mode and an out-of-range ghostBase are errors, not crashes. */
    CHECK(exec(s2, "file.automate.mergeToHdrPro",
               "{\"useOpenDocuments\":true,\"exposures\":[0,1],\"mode\":\"x\"}") != PC_OK);
    pc_engine_session_free(s2);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_photomerge_reposition);
    RUN(test_photomerge_perspective_noblend);
    RUN(test_photomerge_cylindrical_fill);
    RUN(test_hdr_merge);
    RUN(test_graceful_failure);
    return pc_t_finish();
}
