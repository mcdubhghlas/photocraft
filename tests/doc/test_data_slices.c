/* Slice resolution against a document (port of slices.rs resolve tests, plus ordering, clipping
 * and naming checks). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/doc.h"
#include "harness.h"

static void new_doc(pc_document *d, const char *name, uint32_t w, uint32_t h)
{
    if (pc_document_init(d, name, pc_size_make(w, h), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) != PC_OK) {
        abort(); /* test-only */
    }
}

/* Appends a default user slice with the given id and rect. */
static pc_slice *add_slice(pc_document *d, uint32_t id, pc_rect r)
{
    pc_slice *grown = realloc(d->slices.list, (d->slices.len + 1) * sizeof *grown);
    pc_slice *s;
    if (grown == NULL) {
        abort();
    }
    d->slices.list = grown;
    s = &d->slices.list[d->slices.len++];
    pc_slice_default(s);
    s->id = id;
    s->rect = r;
    return s;
}

static void test_resolve_numbers_top_to_bottom(void)
{
    pc_document d;
    pc_resolved_slice *r = NULL;
    size_t n = 0;
    size_t i;
    const pc_resolved_slice *user = NULL;
    bool logo = false;
    new_doc(&d, "site.psd", 100, 80);
    CHECK(pc_slices_resolve(&d, &r, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK_EQ_STR(pc_str_c(&r[0].name), "site");
    CHECK(!r[0].has_id && r[0].origin == PC_SLICE_AUTO && r[0].kind == PC_SLICE_IMAGE && r[0].number == 1);
    pc_resolved_slices_free(r, n);

    add_slice(&d, 1, pc_rect_make(20, 10, 60, 30));
    CHECK(pc_slices_resolve(&d, &r, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 5);
    CHECK_EQ_STR(pc_str_c(&r[0].name), "site_01");
    for (i = 0; i < n; i++) {
        if (r[i].has_id && r[i].id == 1) {
            user = &r[i];
        }
    }
    CHECK(user != NULL);
    if (user != NULL) {
        CHECK_EQ_UINT(user->number, 3); /* after the top band and the left cell */
        CHECK(user->origin == PC_SLICE_USER);
        CHECK_EQ_STR(pc_str_c(&user->name), "site_03");
    }
    pc_resolved_slices_free(r, n);

    CHECK(pc_str_set_c(&d.slices.list[0].name, "logo", NULL) == PC_OK);
    CHECK(pc_slices_resolve(&d, &r, &n, NULL) == PC_OK);
    for (i = 0; i < n; i++) {
        logo = logo || pc_str_eq_c(&r[i].name, "logo");
    }
    CHECK(logo);
    pc_resolved_slices_free(r, n);
    pc_document_fini(&d);
}

static void test_resolve_order_clipping_and_names(void)
{
    pc_document d;
    pc_resolved_slice *r = NULL;
    size_t n = 0;
    pc_slice *s;
    new_doc(&d, "", 50, 50);
    /* Two stored slices with the same top-left keep their list order (stable sort); a slice
     * reaching past the canvas is clipped, one outside it disappears. */
    s = add_slice(&d, 7, pc_rect_make(0, 0, 10, 10));
    s->kind = PC_SLICE_NO_IMAGE;
    add_slice(&d, 3, pc_rect_make(0, 0, 20, 5));
    add_slice(&d, 9, pc_rect_make(40, 40, 90, 90));
    add_slice(&d, 11, pc_rect_make(60, 0, 70, 10));
    s = add_slice(&d, 7, pc_rect_make(30, 30, 31, 31)); /* duplicate id: lookups find the first */
    s->origin = PC_SLICE_LAYER;
    CHECK(pc_slices_resolve(&d, &r, &n, NULL) == PC_OK);
    CHECK(n > 4);
    if (n > 4) {
        CHECK(r[0].has_id && r[0].id == 7 && r[0].kind == PC_SLICE_NO_IMAGE);
        CHECK(r[1].has_id && r[1].id == 3);
        CHECK_EQ_STR(pc_str_c(&r[0].name), "Untitled_01");
        CHECK_EQ_STR(pc_str_c(&r[1].name), "Untitled_02");
    }
    {
        size_t i;
        size_t stored = 0;
        for (i = 0; i < n; i++) {
            CHECK_EQ_UINT(r[i].number, i + 1);
            if (r[i].has_id) {
                stored++;
                CHECK(r[i].id != 11);
                if (r[i].id == 9) {
                    CHECK(pc_rect_eq(r[i].rect, pc_rect_make(40, 40, 50, 50)));
                }
                if (r[i].id == 7) {
                    CHECK(r[i].origin == PC_SLICE_USER); /* both id-7 entries resolve to the first */
                }
            }
        }
        CHECK_EQ_UINT(stored, 4);
    }
    pc_resolved_slices_free(r, n);
    pc_document_fini(&d);
}

static void test_resolve_three_digit_numbers(void)
{
    pc_document d;
    pc_resolved_slice *r = NULL;
    size_t n = 0;
    uint32_t i;
    new_doc(&d, "grid.png", 120, 1);
    for (i = 0; i < 120; i++) {
        add_slice(&d, i + 1, pc_rect_make((int32_t)i, 0, (int32_t)i + 1, 1));
    }
    CHECK(pc_slices_resolve(&d, &r, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 120);
    if (n == 120) {
        CHECK_EQ_STR(pc_str_c(&r[8].name), "grid_09");
        CHECK_EQ_STR(pc_str_c(&r[119].name), "grid_120");
    }
    pc_resolved_slices_free(r, n);
    pc_document_fini(&d);
}

static void test_resolve_empty_canvas(void)
{
    pc_document d;
    pc_resolved_slice *r = NULL;
    size_t n = 5;
    new_doc(&d, "x", 0, 0);
    add_slice(&d, 1, pc_rect_make(0, 0, 10, 10));
    CHECK(pc_slices_resolve(&d, &r, &n, NULL) == PC_OK);
    CHECK(n == 0 && r == NULL);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_resolve_numbers_top_to_bottom);
    RUN(test_resolve_order_clipping_and_names);
    RUN(test_resolve_three_digit_numbers);
    RUN(test_resolve_empty_canvas);
    return pc_t_finish();
}
