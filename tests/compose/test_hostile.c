/* Never crash (AGENTS.md, REWRITE_PLAN.md §4): hostile documents and parameters give a status,
 * never a crash or a hang; results don't depend on the thread count (D6); concurrent renders
 * share the caches safely (run under TSan). */
#define _XOPEN_SOURCE 700 /* popen / pclose (results of child processes) */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"

/* A chain of `depth` nested groups around one red layer. */
static pc_document deep_doc(size_t depth)
{
    pc_document d = ch_doc(16, 16, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer cur = ch_solid("leaf", pc_rect_make(2, 2, 12, 12), 1, 0, 0, 1);
    size_t i;
    for (i = 0; i < depth; i++) {
        cur = ch_group("g", &cur, 1);
        if (i % 2 == 0) {
            cur.blend = PC_BLEND_NORMAL;
        }
    }
    ch_push(&d, cur);
    return d;
}

static void test_deep_groups(void)
{
    pc_document legal = deep_doc(PC_MAX_GROUP_DEPTH);
    pc_document deep = deep_doc(10000);
    pc_compose_buffer b;
    pc_error e;
    pc_rect r;
    pc_document p;
    /* Every legal document renders. */
    CHECK(pc_compose_flatten(&legal, &b, &e) == PC_OK);
    CHECK(ch_close4(pc_compose_buffer_get(&b, 5, 5), (const float[4]){1, 0, 0, 1}));
    pc_compose_buffer_fini(&b);
    /* Deeper than the compositor descends: a clean error. */
    CHECK(pc_compose_flatten(&deep, &b, &e) == PC_ERR_LIMIT);
    CHECK_CONTAINS(e.msg, "nested");
    CHECK(pc_compose_render_tiled(&deep, pc_rect_make(0, 0, 16, 16), 4, NULL, &b, &e) == PC_ERR_LIMIT);
    /* Queries walk iteratively or answer conservatively. */
    CHECK(pc_rect_eq(pc_compose_layer_bounds(&deep.layers[0], pc_document_bounds(&deep)),
                     pc_rect_make(2, 2, 12, 12)));
    CHECK(pc_compose_transparent_outside(&deep.layers[0]));
    CHECK(!pc_compose_composite_bounds(&deep.layers[0], pc_document_bounds(&deep), &r));
    CHECK(!pc_compose_change_bounds(&deep.layers[0], pc_document_bounds(&deep), &r));
    CHECK(pc_compose_proxy_faithful(&deep));
    CHECK(pc_compose_proxy_document(&deep, 2, &p, NULL) == PC_OK);
    pc_document_fini(&p);
    pc_document_fini(&legal);
    pc_document_fini(&deep);
}

static void test_absurd_numbers(void)
{
    const float vals[6] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -0.0f};
    size_t i;
    for (i = 0; i < 6; i++) {
        pc_document d = ch_doc_white(20, 12);
        pc_layer *l = ch_push(&d, ch_solid("a", pc_rect_make(1, 1, 15, 9), vals[i], 0.5f, vals[i], 0.7f));
        pc_layer *m;
        const double pts[3][2] = {{1, 1}, {vals[i], 5}, {9, 1e300}};
        pc_path p = ch_polygon(pts, 3);
        pc_compose_buffer b;
        pc_rgba8_image t;
        pc_surface s;
        const pc_blend_range pair[2] = {{{200, 10}, {5, 1}}, {{255, 255}, {0, 0}}};
        l->opacity = vals[i];
        l->fill_opacity = vals[(i + 1) % 6];
        l->blend = PC_BLEND_LAYER_MODES[(i * 5) % PC_BLEND_LAYER_MODE_COUNT];
        pc_layer_mask_reveal_all(&l->mask);
        l->has_mask = true;
        l->mask.density = vals[(i + 2) % 6];
        l->mask.feather = vals[(i + 3) % 6];
        pc_vector_mask_init(&l->vector_mask, &p);
        l->has_vector_mask = true;
        l->vector_mask.density = vals[(i + 4) % 6];
        l->vector_mask.feather = vals[i];
        l->excluded_channels = 0xffffffffu;
        CHECK(pc_blend_if_set(&l->blend_if, 2, pair, NULL) == PC_OK);
        m = ch_push(&d, ch_solid("clip", pc_rect_make(0, 0, 20, 12), 0.1f, vals[i], 0.3f, vals[i]));
        m->clipped = true;
        m->blend = PC_BLEND_DISSOLVE;
        m->opacity = vals[(i + 5) % 6];
        CHECK(pc_compose_flatten(&d, &b, NULL) == PC_OK);
        pc_compose_buffer_fini(&b);
        CHECK(pc_compose_render_tiled(&d, pc_rect_make(-5, -5, 30, 30), 3, NULL, &b, NULL) == PC_OK);
        pc_compose_buffer_fini(&b);
        CHECK(pc_compose_thumbnail(&d, 7, &t, NULL) == PC_OK);
        pc_rgba8_image_fini(&t);
        CHECK(pc_compose_flatten_to_surface(&d, PC_PIXEL_FORMAT_CMYKA8, (const float[3]){vals[i], 0, 1}, &s,
                                            NULL) == PC_OK);
        pc_surface_fini(&s);
        pc_document_fini(&d);
    }
}

static void test_empty_documents(void)
{
    pc_document d = ch_doc(0, 0, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_compose_buffer b;
    pc_rgba8_image t;
    pc_surface s;
    pc_document p;
    CHECK(pc_compose_flatten(&d, &b, NULL) == PC_OK && b.len == 0);
    pc_compose_buffer_fini(&b);
    CHECK(pc_compose_thumbnail(&d, 32, &t, NULL) == PC_OK && t.width == 1 && t.height == 1);
    pc_rgba8_image_fini(&t);
    CHECK(pc_compose_thumbnail(&d, 0, &t, NULL) == PC_OK);
    pc_rgba8_image_fini(&t);
    CHECK(pc_compose_flatten_to_surface(&d, PC_PIXEL_FORMAT_RGBA8, NULL, &s, NULL) == PC_OK);
    CHECK(pc_surface_tile_count(&s) == 0);
    pc_surface_fini(&s);
    CHECK(pc_compose_render_reduced_damage(&d, 5, 5, pc_rect_make(0, 0, 9, 9), &b, NULL) == PC_OK);
    CHECK(pc_rect_is_empty(b.rect));
    pc_compose_buffer_fini(&b);
    CHECK(pc_compose_proxy_document(&d, 0, &p, NULL) == PC_OK);
    pc_document_fini(&p);
    /* A document with an empty group and an empty raster layer. */
    ch_push(&d, ch_group("empty", NULL, 0));
    ch_push(&d, ch_raster("nothing", PC_PIXEL_FORMAT_RGBA16));
    CHECK(pc_compose_render(&d, pc_rect_make(0, 0, 3, 3), &b, NULL) == PC_OK);
    CHECK(b.len == 9 && b.px[4][3] == 0.0f);
    pc_compose_buffer_fini(&b);
    pc_document_fini(&d);
}

static void test_huge_bounds_and_odd_tiles(void)
{
    pc_document d = ch_doc_white(30, 30);
    pc_compose_buffer b;
    pc_error e;
    pc_rgba8_image t;
    pc_document big = ch_doc(60000, 60000, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    CHECK(pc_compose_render(&d, pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), &b, &e) ==
          PC_ERR_LIMIT);
    CHECK(pc_compose_render(&d, pc_rect_make(0, 0, 70000, 70000), &b, &e) == PC_ERR_LIMIT);
    CHECK(pc_compose_render_reduced_rect(&d, pc_rect_make(INT32_MIN, 0, INT32_MAX, 1), 4, 1, &b, &e) ==
          PC_ERR_LIMIT);
    CHECK(pc_compose_render_layer(&d.layers[0], pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), &b,
                                  &e) == PC_ERR_LIMIT);
    /* Tile sizes below 1 count as 1. */
    CHECK(pc_compose_render_tiled(&d, pc_document_bounds(&d), 0, NULL, &b, NULL) == PC_OK);
    CHECK(ch_close4(pc_compose_buffer_get(&b, 29, 29), (const float[4]){1, 1, 1, 1}));
    CHECK(pc_compose_buffer_get(&b, 30, 0) == NULL);
    pc_compose_buffer_fini(&b);
    CHECK(pc_compose_render_tiled(&d, pc_document_bounds(&d), -7, NULL, &b, NULL) == PC_OK);
    pc_compose_buffer_fini(&b);
    /* A huge but empty document thumbnails from its proxy in no time. */
    ch_push(&big, ch_solid("dot", pc_rect_make(100, 100, 30000, 200), 0, 0, 1, 1));
    CHECK(pc_compose_thumbnail(&big, 64, &t, NULL) == PC_OK && t.width == 64 && t.height == 64);
    pc_rgba8_image_fini(&t);
    pc_document_fini(&big);
    pc_document_fini(&d);
}

static void test_mismatched_formats(void)
{
    /* Layers, masks and channels in formats that don't match the document (or each other). */
    const pc_color_mode modes[5] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_CMYK, PC_COLOR_MODE_LAB,
                                    PC_COLOR_MODE_MULTICHANNEL, PC_COLOR_MODE_BITMAP};
    size_t i;
    for (i = 0; i < 5; i++) {
        pc_document d = ch_doc(17, 9, modes[i], PC_SAMPLE_TYPE_U16);
        pc_layer cm = ch_raster("cmyk", PC_PIXEL_FORMAT_CMYKA8);
        pc_layer *l;
        const float k[5] = {0.2f, 0.4f, 0.1f, 0.3f, 1.0f};
        const float rgba[4] = {0.5f, 0.25f, 0.75f, 1.0f};
        pc_alpha_channel *ch = calloc(1, sizeof *ch);
        pc_surface s;
        pc_compose_buffer b;
        bool found;
        ch_fill(&cm, pc_rect_make(0, 0, 9, 9), k, 5);
        ch_push(&d, cm);
        l = ch_push(&d, ch_solid("rgb", pc_rect_make(4, 0, 17, 9), 0.9f, 0.1f, 0.2f, 0.6f));
        /* A mask stored as RGBA instead of grey. */
        pc_surface_init(&l->mask.surface, PC_PIXEL_FORMAT_RGBA8);
        CHECK(pc_surface_fill_rect(&l->mask.surface, pc_rect_make(0, 0, 8, 9), rgba, 4, NULL) == PC_OK);
        l->mask.enabled = true;
        l->mask.density = 1.0f;
        l->mask.feather = 2.0f;
        l->has_mask = true;
        /* A spot channel stored as RGBA. */
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 17, 9), rgba, 4, NULL) == PC_OK);
        CHECK(ch != NULL && pc_alpha_channel_init(ch, "spot", &s, NULL) == PC_OK);
        ch->has_spot = true;
        ch->spot_color = pc_color_rgb(0, 1, 0);
        ch->spot_solidity = 0.5f;
        d.channels = ch;
        d.nchannels = 1;
        CHECK(pc_compose_flatten(&d, &b, NULL) == PC_OK);
        pc_compose_buffer_fini(&b);
        CHECK(pc_compose_combined_mask(l, pc_document_bounds(&d), &found, &s, NULL) == PC_OK && found);
        pc_surface_fini(&s);
        pc_document_fini(&d);
    }
}

typedef struct cancel_ctx {
    atomic_int calls;
    int after;
} cancel_ctx;

/* Cancels once more than `after` checks were made (tiles check concurrently). */
static bool cancel_after(void *ctx)
{
    cancel_ctx *c = ctx;
    return atomic_fetch_add(&c->calls, 1) + 1 > c->after;
}

static pc_status count_sink(void *ctx, pc_compose_buffer *band, pc_error *err)
{
    (void)band;
    (void)err;
    (*(int *)ctx)++;
    return PC_OK;
}

static void test_cancellation(void)
{
    pc_document d = ch_doc_white(600, 600);
    pc_compose_buffer b;
    pc_error e;
    cancel_ctx c;
    pc_interrupt intr = {cancel_after, NULL, &c};
    int bands = 0;
    atomic_init(&c.calls, 0);
    c.after = 0;
    ch_push(&d, ch_solid("a", pc_rect_make(0, 0, 600, 600), 0.2f, 0.3f, 0.4f, 0.5f));
    CHECK(pc_compose_render_tiled(&d, pc_document_bounds(&d), 64, &intr, &b, &e) == PC_ERR_CANCELLED);
    CHECK_CONTAINS(e.msg, "cancel");
    atomic_store(&c.calls, 0);
    c.after = 5;
    CHECK(pc_compose_render_tiled(&d, pc_document_bounds(&d), 64, &intr, &b, &e) == PC_ERR_CANCELLED);
    atomic_store(&c.calls, 0);
    c.after = 1000000;
    CHECK(pc_compose_render_tiled(&d, pc_document_bounds(&d), 64, &intr, &b, &e) == PC_OK);
    pc_compose_buffer_fini(&b);
    atomic_store(&c.calls, 0);
    c.after = 0;
    CHECK(pc_compose_render_bands(&d, pc_document_bounds(&d), 256, &intr, count_sink, &bands, &e) ==
          PC_ERR_CANCELLED);
    CHECK(bands == 0);
    pc_document_fini(&d);
}

/* ---- threads ---- */

/* A document exercising masks (feathered and vector), clipping, groups, Dissolve, text gamma. */
static pc_document busy_doc(void)
{
    pc_document d = ch_doc_white(700, 530);
    pc_layer *l = ch_push(&d, ch_solid("a", pc_rect_make(5, 5, 600, 400), 1, 0.2f, 0.1f, 0.8f));
    const double pts[4][2] = {{10, 10}, {650.5, 40}, {500, 520.25}, {30, 300}};
    pc_path p = ch_polygon(pts, 4);
    const float q = 0.25f;
    pc_layer in[2];
    pc_layer g;
    pc_layer_mask_reveal_all(&l->mask);
    l->has_mask = true;
    l->mask.feather = 6.0f;
    CHECK(pc_surface_fill_rect(&l->mask.surface, pc_rect_make(0, 0, 300, 530), &q, 1, NULL) == PC_OK);
    l = ch_push(&d, ch_solid("clip", pc_rect_make(30, 0, 700, 530), 0, 1, 0, 1));
    l->clipped = true;
    l->blend = PC_BLEND_DISSOLVE;
    l->opacity = 0.6f;
    in[0] = ch_solid("in", pc_rect_make(50, 20, 690, 500), 0.2f, 0.3f, 0.9f, 1);
    in[1] = ch_solid("in2", pc_rect_make(250, 120, 400, 300), 0.9f, 0.9f, 0.1f, 0.5f);
    in[1].blend = PC_BLEND_OVERLAY;
    g = ch_group("g", in, 2);
    g.opacity = 0.7f;
    pc_vector_mask_init(&g.vector_mask, &p);
    g.has_vector_mask = true;
    ch_push(&d, g);
    return d;
}

static uint64_t hash_buf(const pc_compose_buffer *b)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = (const uint8_t *)b->px;
    size_t i;
    for (i = 0; i < b->len * 16; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* With --hashes: print the hashes of a few renders (run by test_thread_counts in children). */
static int print_hashes(void)
{
    pc_document d = busy_doc();
    pc_compose_buffer b;
    if (pc_compose_flatten(&d, &b, NULL) != PC_OK) {
        return 1;
    }
    printf("%016llx", (unsigned long long)hash_buf(&b));
    pc_compose_buffer_fini(&b);
    if (pc_compose_render_tiled(&d, pc_rect_make(-3, 7, 690, 401), 61, NULL, &b, NULL) != PC_OK) {
        return 1;
    }
    printf(" %016llx", (unsigned long long)hash_buf(&b));
    pc_compose_buffer_fini(&b);
    if (pc_compose_render_reduced_in_bands(&d, pc_document_bounds(&d), 97, 61, NULL, 256, &b, NULL) !=
        PC_OK) {
        return 1;
    }
    printf(" %016llx\n", (unsigned long long)hash_buf(&b));
    pc_compose_buffer_fini(&b);
    pc_document_fini(&d);
    return 0;
}

static const char *self_path;

static bool child_hashes(const char *threads, char *out, size_t cap)
{
    char cmd[4096];
    FILE *f;
    size_t n;
    if (snprintf(cmd, sizeof cmd, "PHOTOCRAFT_THREADS=%s '%s' --hashes", threads, self_path) >=
        (int)sizeof cmd) {
        return false;
    }
    f = popen(cmd, "r");
    if (f == NULL) {
        return false;
    }
    n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    return pclose(f) == 0 && n > 0;
}

static void test_thread_counts(void)
{
    char one[256];
    char eight[256];
    CHECK(child_hashes("1", one, sizeof one));
    CHECK(child_hashes("8", eight, sizeof eight));
    CHECK_EQ_STR(one, eight);
}

typedef struct job {
    const pc_document *d;
    uint64_t hash;
    pc_status st;
} job;

static void *render_job(void *arg)
{
    job *j = arg;
    pc_compose_buffer b;
    j->st = pc_compose_flatten(j->d, &b, NULL);
    if (j->st == PC_OK) {
        j->hash = hash_buf(&b);
        pc_compose_buffer_fini(&b);
    }
    return NULL;
}

static void test_concurrent_renders_share_caches(void)
{
    pc_document d = busy_doc();
    job jobs[4];
    pthread_t th[4];
    int i;
    pc_compose_purge_mask_cache();
    pc_compose_purge_bounds_cache();
    for (i = 0; i < 4; i++) {
        jobs[i].d = &d;
        jobs[i].hash = 0;
        jobs[i].st = PC_ERR_INVALID;
        CHECK(pthread_create(&th[i], NULL, render_job, &jobs[i]) == 0);
    }
    for (i = 0; i < 4; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
        CHECK(jobs[i].st == PC_OK);
        CHECK(jobs[i].hash == jobs[0].hash);
    }
    CHECK(pc_compose_purge_effect_cache() == 0);
    pc_compose_set_effect_cache_budget(5);
    CHECK(pc_compose_effect_cache_budget() == ((size_t)1 << 20));
    pc_compose_set_effect_cache_budget(PC_COMPOSE_DEFAULT_FX_CACHE_BUDGET);
    CHECK(pc_compose_effect_cache_bytes() == 0);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--hashes") == 0) {
        return print_hashes();
    }
    self_path = argv[0];
    RUN(test_deep_groups);
    RUN(test_absurd_numbers);
    RUN(test_empty_documents);
    RUN(test_huge_bounds_and_odd_tiles);
    RUN(test_mismatched_formats);
    RUN(test_cancellation);
    RUN(test_thread_counts);
    RUN(test_concurrent_renders_share_caches);
    return pc_t_finish();
}
