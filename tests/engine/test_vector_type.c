/* E11 cross-module primitives and relocated content hooks (vector_cmds.c, type_cmds.c): the
 * shape/text transform and refresh primitives other engine chunks and canvas_geom depend on.
 *
 * These exercise the primitives directly (no Session), which is also how they are reached by the
 * canvas-geometry hooks. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "doc/doc.h"
#include "doc/text.h"
#include "doc/vector.h"
#include "engine/type_cmds.h"
#include "engine/vector_cmds.h"
#include "harness.h"

/* A unit-square shape with a live rectangle at (0,0,10,10). Caller frees with pc_shape_layer_fini. */
static void make_rect_shape(pc_shape_layer *sh)
{
    memset(sh, 0, sizeof *sh);
    const double pts[4][2] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    pc_subpath sp;
    CHECK(pc_subpath_polygon(pts, 4, &sp, NULL) == PC_OK);
    sh->path.subpaths = malloc(sizeof(pc_subpath));
    sh->path.subpaths[0] = sp;
    sh->path.nsubpaths = 1;
    sh->path.fill_rule = PC_FILL_RULE_NONZERO;
    sh->path.inverted = false;
    sh->has_live = true;
    sh->live.kind = PC_LIVE_RECT;
    sh->live.rect[0] = 0;
    sh->live.rect[1] = 0;
    sh->live.rect[2] = 10;
    sh->live.rect[3] = 10;
}

static void test_transform_shape_axis_aligned_keeps_live(void)
{
    pc_shape_layer sh;
    make_rect_shape(&sh);
    /* scale x2, translate (5, 7): axis-aligned positive scale -> live kept and scaled. */
    pc_affine a = {{2, 0, 0, 2, 5, 7}};
    pc_engine_transform_shape(&sh, &a);
    CHECK(sh.has_live);
    CHECK(sh.live.kind == PC_LIVE_RECT);
    CHECK_NEAR(sh.live.rect[0], 5.0, 1e-9);
    CHECK_NEAR(sh.live.rect[1], 7.0, 1e-9);
    CHECK_NEAR(sh.live.rect[2], 20.0, 1e-9);
    CHECK_NEAR(sh.live.rect[3], 20.0, 1e-9);
    /* the path's first anchor moved too */
    CHECK_NEAR(sh.path.subpaths[0].knots[0].anchor.x, 5.0, 1e-9);
    CHECK_NEAR(sh.path.subpaths[0].knots[0].anchor.y, 7.0, 1e-9);
    pc_shape_layer_fini(&sh);
}

static void test_transform_shape_rotation_drops_live(void)
{
    pc_shape_layer sh;
    make_rect_shape(&sh);
    /* 45-degree rotation: not axis-aligned, not a flip/turn -> live cleared. */
    double c = cos(0.7853981633974483), s = sin(0.7853981633974483);
    pc_affine a = {{c, s, -s, c, 0, 0}};
    pc_engine_transform_shape(&sh, &a);
    CHECK(!sh.has_live);
    pc_shape_layer_fini(&sh);
}

static void test_transform_shape_flip_keeps_ellipse_live(void)
{
    pc_shape_layer sh;
    make_rect_shape(&sh);
    sh.live.kind = PC_LIVE_ELLIPSE;
    /* Horizontal flip about x=20: m = [-1,0,0,1, 40,0]. m1,m2 ~ 0 -> keep live via the box map. */
    pc_affine a = {{-1, 0, 0, 1, 40, 0}};
    pc_engine_transform_shape(&sh, &a);
    CHECK(sh.has_live);
    CHECK(sh.live.kind == PC_LIVE_ELLIPSE);
    /* box corners (0,0)->(40,0) and (10,10)->(30,10): min x 30, w 10 */
    CHECK_NEAR(sh.live.rect[0], 30.0, 1e-9);
    CHECK_NEAR(sh.live.rect[2], 10.0, 1e-9);
    pc_shape_layer_fini(&sh);
}

static void test_map_path_clone_and_finite(void)
{
    pc_shape_layer sh;
    make_rect_shape(&sh);
    pc_path out;
    /* NULL map = a clone. */
    CHECK(pc_engine_map_path(&sh.path, NULL, NULL, &out, NULL) == PC_OK);
    CHECK(pc_path_eq(&out, &sh.path));
    pc_path_fini(&out);
    pc_shape_layer_fini(&sh);
}

static void test_refresh_shape_sets_cache(void)
{
    pc_document doc;
    CHECK(pc_document_init(&doc, "t", pc_size_make(32, 32), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) ==
          PC_OK);
    pc_shape_layer sh;
    make_rect_shape(&sh);
    sh.has_fill = true;
    sh.fill = pc_fill_solid(pc_color_rgb(1.0f, 0.0f, 0.0f));
    CHECK(!sh.has_cache);
    pc_engine_refresh_shape(&doc, &sh);
    CHECK(sh.has_cache);
    pc_shape_layer_fini(&sh);
    pc_document_fini(&doc);
}

static void test_translate_vectors_moves_shape(void)
{
    pc_document doc;
    CHECK(pc_document_init(&doc, "t", pc_size_make(64, 64), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) ==
          PC_OK);
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SHAPE;
    make_rect_shape(&c.u.shape);
    pc_layer l;
    CHECK(pc_layer_init(&l, "S", &c, NULL) == PC_OK);
    pc_layer_id id;
    CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);
    pc_layer *lm = pc_document_layer_mut(&doc, id);
    pc_engine_translate_vectors(&doc, lm, 3.0, 4.0);
    CHECK_NEAR(lm->content.u.shape.path.subpaths[0].knots[0].anchor.x, 3.0, 1e-9);
    CHECK_NEAR(lm->content.u.shape.path.subpaths[0].knots[0].anchor.y, 4.0, 1e-9);
    CHECK(lm->content.u.shape.has_live);
    CHECK_NEAR(lm->content.u.shape.live.rect[0], 3.0, 1e-9);
    pc_document_fini(&doc);
}

static void test_norm_text(void)
{
    size_t n = 0;
    char *s = pc_engine_norm_text("a\r\nb\rc\nd", 8, &n);
    CHECK(s != NULL);
    CHECK_EQ_UINT(n, 7);
    CHECK_EQ_STR(s, "a\nb\nc\nd");
    free(s);
}

/* Build a one-run text layer carrying `text`. */
static void make_text(pc_text_layer *t, const char *text)
{
    pc_text_layer_default(t);
    CHECK(pc_str_set_c(&t->text, text, NULL) == PC_OK);
    CHECK(pc_text_layer_sync_summary(t, NULL) == PC_OK);
}

static void test_replace_text_insert(void)
{
    pc_text_layer t;
    make_text(&t, "Hello");
    /* insert " there" after "Hello" */
    pc_engine_replace_text(&t, 5, 5, " there", 6);
    CHECK_EQ_STR(pc_str_c(&t.text), "Hello there");
    /* replace "Hello" with "Hi" */
    pc_engine_replace_text(&t, 0, 5, "Hi", 2);
    CHECK_EQ_STR(pc_str_c(&t.text), "Hi there");
    /* delete " there" */
    pc_engine_replace_text(&t, 2, 8, "", 0);
    CHECK_EQ_STR(pc_str_c(&t.text), "Hi");
    pc_text_layer_fini(&t);
}

static void test_replace_text_normalises_breaks(void)
{
    pc_text_layer t;
    make_text(&t, "ab");
    pc_engine_replace_text(&t, 1, 1, "X\r\nY", 4);
    CHECK_EQ_STR(pc_str_c(&t.text), "aX\nYb");
    pc_text_layer_fini(&t);
}

static void test_check_text_unlocked(void)
{
    pc_document doc;
    CHECK(pc_document_init(&doc, "t", pc_size_make(16, 16), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) ==
          PC_OK);
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_TEXT;
    make_text(&c.u.text, "hi");
    pc_layer l;
    CHECK(pc_layer_init(&l, "T", &c, NULL) == PC_OK);
    pc_layer_id id;
    CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);
    CHECK(pc_engine_check_text_unlocked(&doc, id, NULL) == PC_OK);
    /* lock all -> refused */
    pc_layer *lm = pc_document_layer_mut(&doc, id);
    lm->locks.all = true;
    pc_error e;
    CHECK(pc_engine_check_text_unlocked(&doc, id, &e) == PC_ERR_INVALID);
    CHECK_CONTAINS(e.msg, "is locked");
    /* an unknown layer id */
    CHECK(pc_engine_check_text_unlocked(&doc, 999999, NULL) != PC_OK);
    pc_document_fini(&doc);
}

static void test_hostile_replace_text(void)
{
    pc_text_layer t;
    make_text(&t, "abc");
    /* out-of-range and reversed ranges must not crash */
    pc_engine_replace_text(&t, 100, 200, "x", 1);
    pc_engine_replace_text(&t, 2, 0, "y", 1);
    CHECK(t.text.len > 0);
    pc_text_layer_fini(&t);
}

int main(int argc, char **argv)
{
    RUN(test_transform_shape_axis_aligned_keeps_live);
    RUN(test_transform_shape_rotation_drops_live);
    RUN(test_transform_shape_flip_keeps_ellipse_live);
    RUN(test_map_path_clone_and_finite);
    RUN(test_refresh_shape_sets_cache);
    RUN(test_translate_vectors_moves_shape);
    RUN(test_norm_text);
    RUN(test_replace_text_insert);
    RUN(test_replace_text_normalises_breaks);
    RUN(test_check_text_unlocked);
    RUN(test_hostile_replace_text);
    return pc_t_finish();
}
