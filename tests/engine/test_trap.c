/* Image > Trap (port of crates/engine/src/trap_cmds.rs tests, E9). */
#include <stdlib.h>
#include <string.h>

#include "base/str.h"
#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

/* Paint cyan | magenta halves on the Background of a fresh CMYK document, then add it. */
static pc_engine_session *cmyk_painted(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(8, 4), PC_COLOR_MODE_CMYK,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(0, 0, 0, 0), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    size_t ch = fmt.mode == PC_COLOR_MODE_CMYK ? pc_color_mode_channels(PC_COLOR_MODE_CMYK) : 4;
    if (fmt.alpha) {
        ch += 1;
    }
    pc_layer *bg = pc_document_background_mut(&doc);
    CHECK(bg != NULL);
    pc_surface *surf = pc_layer_surface_mut(bg);
    CHECK(surf != NULL);
    float *data = calloc(8 * 4 * ch, sizeof *data);
    CHECK(data != NULL);
    for (size_t y = 0; y < 4; y++) {
        for (size_t x = 0; x < 8; x++) {
            float *px = &data[(y * 8 + x) * ch];
            if (x < 4) {
                px[0] = 1.0f;
            } else {
                px[1] = 1.0f;
            }
            if (fmt.alpha) {
                px[ch - 1] = 1.0f;
            }
        }
    }
    CHECK(pc_surface_write_region(surf, pc_rect_make(0, 0, 8, 4), data, 8 * 4 * ch, NULL) == PC_OK);
    free(data);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static void test_traps_a_cmyk_document(void)
{
    pc_engine_session *s = cmyk_painted();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.trap", "{\"width\":1}", &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "trapped")));
    pc_json_value_fini(&out);

    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *bg = pc_document_background(doc);
    const pc_surface *surf = pc_layer_surface(bg);
    size_t ch = pc_surface_channels(surf);
    float *px = NULL;
    size_t len = 0;
    CHECK(pc_surface_read_region(surf, pc_rect_make(0, 0, 8, 4), &px, &len, NULL) == PC_OK);
    /* The seam now has overlapping inks (Rust assertions). */
    CHECK(px[3 * ch + 1] > 0.5f); /* magenta trapped into the cyan edge */
    CHECK(px[4 * ch] > 0.5f);     /* cyan trapped into the magenta edge */
    free(px);

    /* One history step named "Trap". */
    size_t n = pc_history_entry_count(d->history);
    CHECK(n > 0);
    CHECK_EQ_STR(pc_str_c(pc_history_entry(d->history, n - 1)), "Trap");

    pc_engine_session_free(s);
}

static void test_rejects_non_cmyk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(8, 4), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.trap", "{\"width\":1}", &out, NULL) != PC_OK);
    pc_engine_session_free(s);
}

/* Hostile params must never crash (Rule 9). */
static void test_hostile_params(void)
{
    pc_engine_session *s = cmyk_painted();
    const char *cases[] = {"{}", "{\"width\":-5}", "{\"width\":1e300}", "{\"width\":\"x\"}",
                           "{\"width\":null}", "null", "[]"};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_json_value out;
        pc_status rc = pc_engine_session_execute_json(s, "image.trap", cases[i], &out, NULL);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_traps_a_cmyk_document);
    RUN(test_rejects_non_cmyk);
    RUN(test_hostile_params);
    return pc_t_finish();
}
