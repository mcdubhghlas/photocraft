/* Filter Gallery (port of crates/engine/src/gallery_cmds.rs tests, E9): the catalogue, a single
 * gallery filter, a filterGallery effect stack, validation and adversarial params. The layer is
 * painted directly so the test needs E1 + E9 + algo. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

#define W 40
#define H 32

static pc_engine_session *painted(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(W, H), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    pc_layer l;
    CHECK(pc_layer_raster(&l, "layer", fmt, NULL) == PC_OK);
    pc_layer_id id;
    CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);
    pc_layer *lm = pc_document_layer_mut(&doc, id);
    pc_surface *surf = pc_layer_surface_mut(lm);
    size_t n = pc_surface_channels(surf);
    float *data = calloc((size_t)W * H * n, sizeof *data);
    CHECK(data != NULL);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float v = (float)((x * 5 + y * 3) % 17) / 16.0f;
            float *px = &data[((size_t)y * W + (size_t)x) * n];
            px[0] = v;
            px[1] = 1.0f - v;
            px[2] = (float)(x % 4) / 3.0f;
            if (n >= 4) {
                px[3] = 1.0f;
            }
        }
    }
    CHECK(pc_surface_write_region(surf, pc_rect_make(0, 0, W, H), data, (size_t)W * H * n, NULL) == PC_OK);
    free(data);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static bool changed(pc_engine_session *s, const float *before)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_surface *surf = pc_layer_surface(pc_document_layer(doc, d->active_layer));
    float *now = NULL;
    size_t len = 0;
    if (pc_surface_read_region(surf, pc_rect_make(0, 0, W, H), &now, &len, NULL) != PC_OK) {
        return false;
    }
    bool differ = false;
    for (size_t i = 0; i < len; i++) {
        if (fabsf(now[i] - before[i]) > 1e-6f) {
            differ = true;
            break;
        }
    }
    free(now);
    return differ;
}

static float *snapshot(pc_engine_session *s, size_t *len)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_surface *surf = pc_layer_surface(pc_document_layer(doc, d->active_layer));
    float *px = NULL;
    CHECK(pc_surface_read_region(surf, pc_rect_make(0, 0, W, H), &px, len, NULL) == PC_OK);
    return px;
}

static void test_catalogue(void)
{
    pc_engine_session *s = painted();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "filter.filterGallery", "{\"list\":true}", &out, NULL) ==
          PC_OK);
    char *txt = out.doc != NULL ? yyjson_mut_write(out.doc, 0, NULL) : NULL;
    CHECK(txt != NULL);
    CHECK(strstr(txt, "categories") != NULL);
    CHECK(strstr(txt, "coloredPencil") != NULL);
    CHECK(strstr(txt, "filter.gallery.") != NULL);
    free(txt);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_single_gallery_filter_applies(void)
{
    pc_engine_session *s = painted();
    size_t len = 0;
    float *before = snapshot(s, &len);
    pc_json_value out;
    pc_status rc = pc_engine_session_execute_json(s, "filter.gallery.graphicPen", "{}", &out, NULL);
    CHECK(rc == PC_OK);
    pc_json_value_fini(&out);
    CHECK(changed(s, before));
    CHECK(pc_engine_session_undo(s));
    CHECK(!changed(s, before));
    free(before);
    pc_engine_session_free(s);
}

static void test_filter_gallery_stack(void)
{
    pc_engine_session *s = painted();
    size_t len = 0;
    float *before = snapshot(s, &len);
    pc_json_value out;
    const char *p =
        "{\"effects\":[{\"filter\":\"graphicPen\",\"params\":{}},{\"filter\":\"filmGrain\","
        "\"visible\":false},{\"filter\":\"posterEdges\"}]}";
    CHECK(pc_engine_session_execute_json(s, "filter.filterGallery", p, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(changed(s, before));
    free(before);
    pc_engine_session_free(s);
}

static void test_errors(void)
{
    pc_engine_session *s = painted();
    pc_json_value out;
    /* Empty effects stack. */
    CHECK(pc_engine_session_execute_json(s, "filter.filterGallery", "{\"effects\":[]}", &out, NULL) !=
          PC_OK);
    /* Missing filter key. */
    CHECK(pc_engine_session_execute_json(s, "filter.filterGallery", "{\"effects\":[{}]}", &out, NULL) !=
          PC_OK);
    /* Unknown gallery filter. */
    CHECK(pc_engine_session_execute_json(s, "filter.filterGallery",
                                         "{\"effects\":[{\"filter\":\"nope\"}]}", &out, NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_hostile_params(void)
{
    pc_engine_session *s = painted();
    const char *ids[] = {"filter.filterGallery", "filter.gallery.graphicPen", "filter.gallery.chrome",
                         "filter.gallery.stainedGlass"};
    const char *cases[] = {"null",
                           "[]",
                           "{\"effects\":42}",
                           "{\"effects\":[{\"filter\":123}]}",
                           "{\"effects\":[{\"filter\":\"glass\",\"params\":true}]}",
                           "{\"foreground\":\"#zzzzzz\"}",
                           "{\"foreground\":[1,2]}",
                           "{\"strokeLength\":1e308}"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof cases / sizeof cases[0]; j++) {
            pc_json_value out;
            if (pc_engine_session_execute_json(s, ids[i], cases[j], &out, NULL) == PC_OK) {
                pc_json_value_fini(&out);
            }
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_catalogue);
    RUN(test_single_gallery_filter_applies);
    RUN(test_filter_gallery_stack);
    RUN(test_errors);
    RUN(test_hostile_params);
    return pc_t_finish();
}
