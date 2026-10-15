/* The exported pc_engine_apply_filter_to_surface primitive (E9): the pixel function behind every
 * filter.* command, unblocking smart filters (E6), smart adjustments (E8) and the Filter Gallery.
 * Port of the contract of filters::apply_filter_to_surface. */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

#define W 32
#define H 24

static void paint_gradient(pc_surface *s)
{
    size_t n = pc_surface_channels(s);
    float *data = calloc((size_t)W * H * n, sizeof *data);
    CHECK(data != NULL);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float *px = &data[((size_t)y * W + (size_t)x) * n];
            float v = (float)((x * 7 + y * 11) % 13) / 12.0f;
            px[0] = v;
            px[1] = 1.0f - v;
            px[2] = (float)(x % 5) / 4.0f;
            if (n >= 4) {
                px[3] = 1.0f;
            }
        }
    }
    CHECK(pc_surface_write_region(s, pc_rect_make(0, 0, W, H), data, (size_t)W * H * n, NULL) == PC_OK);
    free(data);
}

static yyjson_val *parse(const char *json, yyjson_doc **doc)
{
    *doc = NULL;
    if (pc_json_parse(json, strlen(json), doc, NULL) != PC_OK) {
        return NULL;
    }
    return yyjson_doc_get_root(*doc);
}

/* A core filter changes the pixels and reports applied. */
static void test_core_filter_applies(void)
{
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    paint_gradient(&s);
    pc_rect area = pc_rect_make(0, 0, W, H);
    yyjson_doc *pd = NULL;
    yyjson_val *p = parse("{\"radius\":3}", &pd);
    CHECK(p != NULL);
    pc_surface out;
    bool applied = false;
    pc_error e;
    pc_error_clear(&e);
    pc_status rc = pc_engine_apply_filter_to_surface("filter.blur.gaussianBlur", p, &s, area, NULL, area,
                                                     &out, &applied, &e);
    CHECK(rc == PC_OK);
    CHECK(applied);
    /* Blur changed at least one pixel. */
    bool differ = false;
    for (int y = 0; y < H && !differ; y++) {
        for (int x = 0; x < W; x++) {
            float a[4], b[4];
            pc_surface_read_pixel(&s, x, y, a);
            pc_surface_read_pixel(&out, x, y, b);
            if (a[0] != b[0] || a[1] != b[1]) {
                differ = true;
                break;
            }
        }
    }
    CHECK(differ);
    pc_surface_fini(&out);
    yyjson_doc_free(pd);
    pc_surface_fini(&s);
}

/* An id that is not a pixel filter leaves applied false and out untouched (Rust None). */
static void test_unknown_id_not_applied(void)
{
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    paint_gradient(&s);
    pc_rect area = pc_rect_make(0, 0, W, H);
    pc_surface out;
    bool applied = true;
    pc_error e;
    pc_error_clear(&e);
    pc_status rc = pc_engine_apply_filter_to_surface("edit.nonsense", NULL, &s, area, NULL, area, &out,
                                                     &applied, &e);
    CHECK(rc == PC_OK);
    CHECK(!applied);
    pc_surface_fini(&s);
}

/* Null arguments and hostile params never crash. */
static void test_hostile(void)
{
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    paint_gradient(&s);
    pc_rect area = pc_rect_make(0, 0, W, H);
    pc_surface out;
    bool applied = false;
    /* NULL surf / out -> PC_ERR_INVALID, no crash. */
    CHECK(pc_engine_apply_filter_to_surface("filter.blur.gaussianBlur", NULL, NULL, area, NULL, area,
                                            &out, &applied, NULL) != PC_OK);
    CHECK(pc_engine_apply_filter_to_surface(NULL, NULL, &s, area, NULL, area, &out, &applied, NULL) !=
          PC_OK);
    const char *cases[] = {"null", "{\"radius\":1e308}", "{\"radius\":null}", "{\"amount\":true}"};
    const char *ids[] = {"filter.blur.gaussianBlur", "filter.other.offset", "filter.distort.wave"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof cases / sizeof cases[0]; j++) {
            yyjson_doc *pd = NULL;
            yyjson_val *p = parse(cases[j], &pd);
            applied = false;
            pc_status rc =
                pc_engine_apply_filter_to_surface(ids[i], p, &s, area, NULL, area, &out, &applied, NULL);
            if (rc == PC_OK && applied) {
                pc_surface_fini(&out);
            }
            yyjson_doc_free(pd);
        }
    }
    pc_surface_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_core_filter_applies);
    RUN(test_unknown_id_not_applied);
    RUN(test_hostile);
    return pc_t_finish();
}
