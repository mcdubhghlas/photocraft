/* Bit-exact parity with the Rust reference: tests/golden/ops-vectors.txt was written by
 * `rustref ops-vectors` (tools/rustref/src/ops_vectors.rs). Each scenario runs the same seeded
 * sequence of 80 history operations here and hashes the same observable state (labels, counts,
 * returned documents and layer targets, unique and pixel bytes, pixels at every edited point)
 * with FNV-1a; every line must match:
 *
 *   step SEED I OP PAST REDO UNIQUE PIXEL HASH       every step of seeds 1-4
 *   scenario SEED DEPTH STEPS HASH PAST REDO PIXEL   one line per scenario */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "ops/ops.h"

#define GOLDEN "tests/golden/ops-vectors.txt"
#define H0 0xcbf29ce484222325ull

static void fnv(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}

static void fnv_byte(uint64_t *h, uint8_t b)
{
    fnv(h, &b, 1);
}

static void u64le(uint64_t *h, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    fnv(h, b, 8);
}

static void hs(uint64_t *h, const pc_str *s)
{
    fnv(h, s->ptr, s->len);
    fnv_byte(h, 0);
}

static void opt_s(uint64_t *h, const pc_str *s)
{
    if (s != NULL) {
        fnv_byte(h, 1);
        hs(h, s);
    } else {
        fnv_byte(h, 0);
    }
}

typedef struct rng {
    uint64_t s;
} rng;

static uint64_t next(rng *r)
{
    uint64_t x = r->s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->s = x;
    return x;
}

static uint64_t below(rng *r, uint64_t n)
{
    return next(r) % n;
}

/* target(k): active k unless k % 3 == 0; selected k*10 + j for j < k % 3. */
typedef struct tgt {
    pc_layer_id ids[2];
    pc_layer_target t;
} tgt;

static void make_target(tgt *g, uint64_t k)
{
    g->t.has_active = k % 3 != 0;
    g->t.active = g->t.has_active ? k : 0;
    g->t.nselected = (size_t)(k % 3);
    for (size_t j = 0; j < g->t.nselected; j++) {
        g->ids[j] = k * 10 + j;
    }
    g->t.selected = g->t.nselected > 0 ? g->ids : NULL;
}

static void hash_target(uint64_t *h, const pc_layer_target *t)
{
    if (t->has_active) {
        fnv_byte(h, 1);
        u64le(h, t->active);
    } else {
        fnv_byte(h, 0);
    }
    u64le(h, t->nselected);
    for (size_t i = 0; i < t->nselected; i++) {
        u64le(h, t->selected[i]);
    }
}

typedef struct pt {
    int32_t x, y;
} pt;

static void hash_doc(uint64_t *h, const pc_document *d, const pt *points, size_t npoints)
{
    const pc_surface *surfaces[3];
    size_t count = 0;
    float px[8];
    hs(h, &d->name);
    u64le(h, d->nlayers);
    CHECK_EQ_INT(pc_document_layer_count(d, &count, NULL), PC_OK);
    u64le(h, count);
    u64le(h, d->nchannels);
    fnv_byte(h, d->has_quick_mask ? 1 : 0);
    surfaces[0] = d->nlayers > 0 ? pc_layer_surface(&d->layers[0]) : NULL;
    surfaces[1] = d->nlayers > 0 ? pc_layer_surface(&d->layers[d->nlayers - 1]) : NULL;
    surfaces[2] = d->nchannels > 0 ? &d->channels[0].surface : NULL;
    for (int k = 0; k < 3; k++) {
        if (surfaces[k] == NULL) {
            fnv_byte(h, 0xFF);
            continue;
        }
        for (size_t i = 0; i < npoints; i++) {
            size_t n = pc_surface_channels(surfaces[k]);
            pc_surface_read_pixel(surfaces[k], points[i].x, points[i].y, px);
            for (size_t c = 0; c < n; c++) {
                uint32_t bits;
                uint8_t b[4];
                memcpy(&bits, &px[c], 4);
                for (int j = 0; j < 4; j++) {
                    b[j] = (uint8_t)(bits >> (8 * j));
                }
                fnv(h, b, 4);
            }
        }
    }
}

static void hash_history(uint64_t *h, const pc_history *hist, const pc_document *cur, size_t *unique,
                         size_t *pixel)
{
    size_t past = pc_history_past_len(hist);
    u64le(h, past);
    u64le(h, pc_history_entry_count(hist));
    for (size_t i = 0; i < pc_history_entry_count(hist); i++) {
        hs(h, pc_history_entry(hist, i));
    }
    u64le(h, pc_history_redo_count(hist));
    for (size_t i = 0; i < pc_history_redo_count(hist); i++) {
        hs(h, pc_history_redo_label_at(hist, i));
    }
    opt_s(h, pc_history_undo_label(hist));
    opt_s(h, pc_history_redo_label(hist));
    fnv_byte(h, pc_history_can_undo(hist) ? 1 : 0);
    fnv_byte(h, pc_history_can_redo(hist) ? 1 : 0);
    CHECK_EQ_INT(pc_history_unique_bytes(hist, cur, unique, NULL), PC_OK);
    CHECK_EQ_INT(pc_history_pixel_bytes(hist, cur, pixel, NULL), PC_OK);
    u64le(h, *unique);
    u64le(h, *pixel);
    for (size_t i = 0; i <= past; i++) {
        pc_doc_arc *d = pc_history_state(hist, i);
        if (d != NULL) {
            hs(h, &pc_doc_arc_get(d)->name);
        } else {
            fnv_byte(h, 0xFE);
        }
    }
}

static const size_t BUDGETS[8] = {0, 1, 65536, 262144, 524288, 1048576, 4194304, SIZE_MAX};

static void paint_layer(pc_document *d, size_t idx, int32_t x, int32_t y, float v)
{
    const float px[5] = {v, 0.5f, 0.25f, 1.0f, 0.75f};
    pc_surface *sf = idx < d->nlayers ? pc_layer_surface_mut(&d->layers[idx]) : NULL;
    if (sf != NULL) {
        size_t n = pc_surface_channels(sf);
        CHECK_EQ_INT(pc_surface_write_pixel(sf, x, y, px, n < 5 ? n : 5, NULL), PC_OK);
    }
}

static bool push_channel(pc_document *d, const char *name, pc_surface *s)
{
    pc_alpha_channel *arr = realloc(d->channels, (d->nchannels + 1) * sizeof *arr);
    if (arr == NULL) {
        return false;
    }
    d->channels = arr;
    CHECK_EQ_INT(pc_alpha_channel_init(&arr[d->nchannels], name, s, NULL), PC_OK);
    d->nchannels++;
    return true;
}

typedef struct edit {
    uint64_t op, k;
    int32_t x, y;
    size_t idx;
    float v;
} edit;

static void apply(pc_document *d, const edit *e)
{
    switch (e->op) {
    case 0:
    case 1:
    case 2:
    case 3:
        paint_layer(d, e->idx, e->x, e->y, e->v);
        break;
    case 4: {
        char name[32];
        (void)snprintf(name, sizeof name, "n%llu", (unsigned long long)e->k);
        CHECK_EQ_INT(pc_str_set_c(&d->name, name, NULL), PC_OK);
        break;
    }
    case 5: {
        pc_layer l;
        const float px[4] = {e->v, e->v, e->v, 1.0f};
        CHECK_EQ_INT(pc_layer_raster(&l, "L", pc_document_pixel_format(d), NULL), PC_OK);
        CHECK_EQ_INT(pc_surface_write_pixel(pc_layer_surface_mut(&l), e->x, e->y, px, 4, NULL), PC_OK);
        CHECK_EQ_INT(pc_document_insert_above(d, NULL, &l, NULL, NULL), PC_OK);
        break;
    }
    case 6: {
        pc_layer child, inner, outer, *kids;
        const float px[4] = {0.0f, e->v, 0.0f, 1.0f};
        CHECK_EQ_INT(pc_layer_raster(&child, "C", pc_document_pixel_format(d), NULL), PC_OK);
        CHECK_EQ_INT(pc_surface_write_pixel(pc_layer_surface_mut(&child), e->x, e->y, px, 4, NULL), PC_OK);
        pc_layer_mask_fini(&child.mask);
        pc_layer_mask_reveal_all(&child.mask);
        child.has_mask = true;
        CHECK_EQ_INT(pc_surface_write_pixel(&child.mask.surface, e->x, e->y, &e->v, 1, NULL), PC_OK);
        kids = malloc(sizeof *kids);
        CHECK(kids != NULL);
        if (kids == NULL) {
            pc_layer_fini(&child);
            break;
        }
        kids[0] = child;
        CHECK_EQ_INT(pc_layer_group(&inner, "Inner", kids, 1, NULL), PC_OK);
        kids = malloc(sizeof *kids);
        CHECK(kids != NULL);
        if (kids == NULL) {
            pc_layer_fini(&inner);
            break;
        }
        kids[0] = inner;
        CHECK_EQ_INT(pc_layer_group(&outer, "G", kids, 1, NULL), PC_OK);
        CHECK_EQ_INT(pc_document_insert_above(d, NULL, &outer, NULL, NULL), PC_OK);
        break;
    }
    case 7: {
        pc_surface g;
        pc_surface_init(&g, PC_PIXEL_FORMAT_GRAY8);
        CHECK_EQ_INT(pc_surface_write_pixel(&g, e->x, e->y, &e->v, 1, NULL), PC_OK);
        if (!push_channel(d, "Alpha", &g)) {
            pc_surface_fini(&g);
        }
        break;
    }
    case 8:
        if (d->has_quick_mask) {
            pc_alpha_channel_fini(&d->quick_mask);
            d->has_quick_mask = false;
        } else {
            pc_surface g;
            if (d->nchannels > 0) {
                CHECK_EQ_INT(pc_surface_clone(&d->channels[0].surface, &g, NULL), PC_OK);
            } else {
                pc_surface_init(&g, PC_PIXEL_FORMAT_GRAY8);
            }
            CHECK_EQ_INT(pc_alpha_channel_init(&d->quick_mask, "Quick Mask", &g, NULL), PC_OK);
            d->has_quick_mask = true;
        }
        break;
    default:
        break;
    }
}

static const char *label_of(const edit *e, char *buf, size_t n)
{
    switch (e->op) {
    case 4:
        return "Rename";
    case 5:
        return "New Layer";
    case 6:
        return "New Group";
    case 7:
        return "New Channel";
    case 8:
        return "Quick Mask";
    default:
        (void)snprintf(buf, n, "Paint %llu", (unsigned long long)e->k);
        return buf;
    }
}

/* Snapshot, mutate a clone, record (the reference's `record` closure). */
static void record_edit(pc_history *h, pc_doc_arc **cur, const edit *e)
{
    pc_document d;
    pc_doc_arc *next_doc = NULL;
    tgt g;
    char buf[32];
    const char *label = label_of(e, buf, sizeof buf);
    make_target(&g, e->k);
    CHECK_EQ_INT(pc_document_clone(pc_doc_arc_get(*cur), &d, NULL), PC_OK);
    apply(&d, e);
    CHECK_EQ_INT(pc_doc_arc_new(&d, &next_doc, NULL), PC_OK);
    CHECK_EQ_INT(pc_history_record(h, label, strlen(label), *cur, &g.t, NULL), PC_OK);
    pc_doc_arc_release(*cur);
    *cur = next_doc;
}

/* ---- golden ---- */

static char **gold;
static size_t ngold, gi;

static void load_golden(void)
{
    FILE *f = fopen(GOLDEN, "r");
    char line[512];
    size_t cap = 0;
    if (f == NULL) {
        PC_T_FAIL("cannot open " GOLDEN);
        return;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strcspn(line, "\n");
        line[n] = '\0';
        if (ngold == cap) {
            char **g = realloc(gold, (cap = cap ? cap * 2 : 256) * sizeof *g);
            if (g == NULL) {
                break;
            }
            gold = g;
        }
        gold[ngold] = malloc(n + 1);
        if (gold[ngold] == NULL) {
            break;
        }
        memcpy(gold[ngold++], line, n + 1);
    }
    (void)fclose(f);
}

static void expect(const char *got)
{
    pc_t_checks++;
    if (gi >= ngold) {
        PC_T_FAIL("golden ran out at: %s", got);
        return;
    }
    if (strcmp(gold[gi], got) != 0) {
        PC_T_FAIL("line %zu:\n  want %s\n  got  %s", gi + 1, gold[gi], got);
    }
    gi++;
}

static void scenario(uint64_t seed, size_t steps, bool verbose)
{
    static const char *const DEPTH_NAMES[3] = {"U8", "U16", "F32"};
    static const pc_sample_type DEPTHS[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    size_t di = (size_t)(seed % 3);
    uint32_t w = 64 + (uint32_t)(seed * 37 % 600);
    uint32_t hgt = 64 + (uint32_t)(seed * 53 % 400);
    rng r = {(seed * 0x9E3779B97F4A7C15ull) | 1};
    pc_document d0;
    pc_doc_arc *cur = NULL;
    size_t max_states = seed % 5 == 0 ? 50 : 1 + (size_t)(seed % 7);
    pc_history *hist = NULL;
    pt *points = malloc((steps + 1) * sizeof *points);
    size_t npoints = 1, unique = 0, pixel = 0;
    uint64_t h = H0;
    char line[256];
    if (points == NULL) {
        PC_T_FAIL("out of memory");
        return;
    }
    points[0] = (pt){1, 1};
    CHECK_EQ_INT(pc_document_with_background(&d0, "h", pc_size_make(w, hgt), PC_COLOR_MODE_RGB, DEPTHS[di],
                                             PC_COLOR_WHITE, NULL),
                 PC_OK);
    CHECK_EQ_INT(pc_doc_arc_new(&d0, &cur, NULL), PC_OK);
    CHECK_EQ_INT(pc_history_new(max_states, &hist, NULL), PC_OK);
    if (cur == NULL || hist == NULL) {
        free(points);
        return;
    }
    for (size_t i = 0; i < steps; i++) {
        edit e;
        uint64_t sh = H0;
        tgt g;
        e.op = below(&r, 20);
        e.k = (uint64_t)i + 1;
        e.x = (int32_t)below(&r, (uint64_t)w + 40) - 20;
        e.y = (int32_t)below(&r, (uint64_t)hgt + 40) - 20;
        e.v = (float)(e.k % 11) / 10.0f;
        e.idx = 0;
        u64le(&sh, e.op);
        make_target(&g, e.k);
        switch (e.op) {
        case 0:
        case 1:
        case 2:
        case 3: {
            size_t n = pc_doc_arc_get(cur)->nlayers;
            points[npoints++] = (pt){e.x, e.y};
            e.idx = (size_t)below(&r, n > 1 ? n : 1);
            record_edit(hist, &cur, &e);
            break;
        }
        case 4:
        case 8:
            record_edit(hist, &cur, &e);
            break;
        case 5:
        case 6:
        case 7:
            points[npoints++] = (pt){e.x, e.y};
            record_edit(hist, &cur, &e);
            break;
        case 9:
        case 10:
        case 11: {
            pc_doc_arc *d = NULL;
            pc_layer_target layers = {0};
            pc_status st = e.op == 11 ? pc_history_redo(hist, cur, &d, &layers, NULL)
                                      : pc_history_undo(hist, cur, &d, &layers, NULL);
            CHECK_EQ_INT(st, PC_OK);
            if (d != NULL) {
                fnv_byte(&sh, 1);
                hash_target(&sh, &layers);
                pc_doc_arc_release(cur);
                cur = d;
            } else {
                fnv_byte(&sh, 0);
            }
            pc_layer_target_fini(&layers);
            break;
        }
        case 12: {
            size_t dropped = 0;
            CHECK_EQ_INT(pc_history_trim(hist, pc_doc_arc_get(cur), &dropped, NULL), PC_OK);
            u64le(&sh, dropped);
            break;
        }
        case 13:
            pc_history_set_max_bytes(hist, BUDGETS[below(&r, 8)]);
            break;
        case 14:
            fnv_byte(&sh, pc_history_purge_last(hist) ? 1 : 0);
            break;
        case 15:
            pc_history_clear_redo(hist);
            break;
        case 16:
            CHECK_EQ_INT(pc_history_record(hist, "Same", 4, cur, &g.t, NULL), PC_OK);
            break;
        case 17: {
            pc_document *m = NULL;
            points[npoints++] = (pt){e.x, e.y};
            CHECK_EQ_INT(pc_doc_arc_make_mut(&cur, &m, NULL), PC_OK);
            if (m == NULL) {
                break;
            }
            if (e.k % 2 == 0) {
                paint_layer(m, 0, e.x, e.y, e.v);
            } else if (m->nlayers > 0 && pc_layer_surface_mut(&m->layers[0]) != NULL) {
                pc_surface large;
                pc_tile_entry *entries = NULL;
                size_t n = 0;
                const float px[5] = {e.v, 0.0f, 0.0f, 0.0f, 1.0f};
                pc_surface_init(&large, (pc_pixel_format){PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_F32, true});
                CHECK_EQ_INT(pc_surface_write_pixel(&large, e.x, e.y, px, 5, NULL), PC_OK);
                CHECK_EQ_INT(
                    pc_surface_take_tiles(&large, pc_rect_from_xywh(e.x, e.y, 1, 1), &entries, &n, NULL),
                    PC_OK);
                CHECK_EQ_INT(pc_surface_put_tiles(pc_layer_surface_mut(&m->layers[0]), entries, n, NULL),
                             PC_OK);
                free(entries);
                pc_surface_fini(&large);
            }
            break;
        }
        case 18:
            if (e.k % 2 == 0) {
                char label[32];
                (void)snprintf(label, sizeof label, "Fold %llu", (unsigned long long)e.k);
                CHECK_EQ_INT(pc_history_set_current_label(hist, label, strlen(label), NULL), PC_OK);
            } else {
                CHECK_EQ_INT(pc_history_set_current_layers(hist, &g.t, NULL), PC_OK);
            }
            break;
        default:
            if (below(&r, 4) == 0) {
                pc_history_clear(hist);
            } else {
                pc_history *c = NULL;
                CHECK_EQ_INT(pc_history_clone(hist, &c, NULL), PC_OK);
                if (c != NULL) {
                    pc_history_free(hist);
                    hist = c;
                }
            }
            break;
        }
        hash_history(&sh, hist, pc_doc_arc_get(cur), &unique, &pixel);
        hash_doc(&sh, pc_doc_arc_get(cur), points, npoints);
        u64le(&h, sh);
        if (verbose) {
            (void)snprintf(line, sizeof line, "step\t%llu\t%zu\t%llu\t%zu\t%zu\t%zu\t%zu\t%016llx",
                           (unsigned long long)seed, i, (unsigned long long)e.op, pc_history_past_len(hist),
                           pc_history_redo_count(hist), unique, pixel, (unsigned long long)sh);
            expect(line);
        }
    }
    CHECK_EQ_INT(pc_history_pixel_bytes(hist, pc_doc_arc_get(cur), &pixel, NULL), PC_OK);
    (void)snprintf(line, sizeof line, "scenario\t%llu\t%s\t%zu\t%016llx\t%zu\t%zu\t%zu",
                   (unsigned long long)seed, DEPTH_NAMES[di], steps, (unsigned long long)h,
                   pc_history_past_len(hist), pc_history_redo_count(hist), pixel);
    expect(line);
    pc_history_free(hist);
    pc_doc_arc_release(cur);
    free(points);
}

static void test_ops_vectors(void)
{
    load_golden();
    for (uint64_t seed = 1; seed <= 120; seed++) {
        scenario(seed, 80, seed <= 4);
    }
    if (gi != ngold) {
        PC_T_FAIL("%zu golden lines left unchecked", ngold - gi);
    }
    for (size_t i = 0; i < ngold; i++) {
        free(gold[i]);
    }
    free(gold);
}

int main(int argc, char **argv)
{
    RUN(test_ops_vectors);
    return pc_t_finish();
}
