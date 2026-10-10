/* Web slices (port of crates/doc/src/slices.rs). */
#include "doc/doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"

/* ---- codes and ids ---- */

uint32_t pc_slice_origin_code(pc_slice_origin o)
{
    switch (o) {
    case PC_SLICE_AUTO:
        return 0;
    case PC_SLICE_LAYER:
        return 1;
    case PC_SLICE_USER:
        return 2;
    }
    return 2;
}

pc_slice_origin pc_slice_origin_from_code(uint32_t c)
{
    switch (c) {
    case 0:
        return PC_SLICE_AUTO;
    case 1:
        return PC_SLICE_LAYER;
    default:
        return PC_SLICE_USER;
    }
}

const char *pc_slice_origin_id(pc_slice_origin o)
{
    switch (o) {
    case PC_SLICE_AUTO:
        return "auto";
    case PC_SLICE_LAYER:
        return "layer";
    case PC_SLICE_USER:
        return "user";
    }
    return "user";
}

uint32_t pc_slice_kind_code(pc_slice_kind k)
{
    switch (k) {
    case PC_SLICE_NO_IMAGE:
        return 0;
    case PC_SLICE_IMAGE:
        return 1;
    case PC_SLICE_TABLE:
        return 2;
    }
    return 1;
}

pc_slice_kind pc_slice_kind_from_code(uint32_t c)
{
    switch (c) {
    case 0:
        return PC_SLICE_NO_IMAGE;
    case 2:
        return PC_SLICE_TABLE;
    default:
        return PC_SLICE_IMAGE;
    }
}

const char *pc_slice_kind_id(pc_slice_kind k)
{
    switch (k) {
    case PC_SLICE_IMAGE:
        return "image";
    case PC_SLICE_NO_IMAGE:
        return "noImage";
    case PC_SLICE_TABLE:
        return "table";
    }
    return "image";
}

static bool mem_is(const char *s, size_t len, const char *w)
{
    return strlen(w) == len && (len == 0 || memcmp(s, w, len) == 0);
}

bool pc_slice_kind_from_id(const char *s, size_t len, pc_slice_kind *out)
{
    if (s == NULL) {
        return false;
    }
    if (mem_is(s, len, "image") || mem_is(s, len, "img")) {
        *out = PC_SLICE_IMAGE;
    } else if (mem_is(s, len, "noImage") || mem_is(s, len, "none") || mem_is(s, len, "no-image")) {
        *out = PC_SLICE_NO_IMAGE;
    } else if (mem_is(s, len, "table")) {
        *out = PC_SLICE_TABLE;
    } else {
        return false;
    }
    return true;
}

/* ---- slice ---- */

void pc_slice_default(pc_slice *out)
{
    memset(out, 0, sizeof *out);
    out->origin = PC_SLICE_USER;
    out->kind = PC_SLICE_IMAGE;
}

/* The owned strings of a slice, in one place so fini/clone/eq can't miss one. */
enum { SLICE_NSTR = 6 };

static pc_str *slice_strs(pc_slice *s, size_t k)
{
    pc_str *all[SLICE_NSTR];
    all[0] = &s->name;
    all[1] = &s->url;
    all[2] = &s->target;
    all[3] = &s->message;
    all[4] = &s->alt;
    all[5] = &s->cell_text;
    return all[k];
}

static const pc_str *slice_strs_c(const pc_slice *s, size_t k)
{
    const pc_str *all[SLICE_NSTR];
    all[0] = &s->name;
    all[1] = &s->url;
    all[2] = &s->target;
    all[3] = &s->message;
    all[4] = &s->alt;
    all[5] = &s->cell_text;
    return all[k];
}

void pc_slice_fini(pc_slice *s)
{
    size_t k;
    if (s == NULL) {
        return;
    }
    for (k = 0; k < SLICE_NSTR; k++) {
        pc_str_fini(slice_strs(s, k));
    }
    memset(s, 0, sizeof *s);
}

pc_status pc_slice_clone(const pc_slice *s, pc_slice *out, pc_error *err)
{
    pc_slice t = *s;
    size_t k;
    for (k = 0; k < SLICE_NSTR; k++) {
        *slice_strs(&t, k) = (pc_str)PC_STR_INIT;
    }
    for (k = 0; k < SLICE_NSTR; k++) {
        pc_status st = pc_str_clone(slice_strs_c(s, k), slice_strs(&t, k), err);
        if (st != PC_OK) {
            size_t j;
            for (j = 0; j < k; j++) {
                pc_str_fini(slice_strs(&t, j));
            }
            return st;
        }
    }
    *out = t;
    return PC_OK;
}

bool pc_slice_eq(const pc_slice *a, const pc_slice *b)
{
    size_t k;
    for (k = 0; k < SLICE_NSTR; k++) {
        if (!pc_str_eq(slice_strs_c(a, k), slice_strs_c(b, k))) {
            return false;
        }
    }
    if (a->has_layer != b->has_layer || (a->has_layer && a->layer != b->layer)) {
        return false;
    }
    if (a->has_background != b->has_background ||
        (a->has_background && memcmp(a->background, b->background, sizeof a->background) != 0)) {
        return false;
    }
    return a->id == b->id && a->group_id == b->group_id && a->origin == b->origin && pc_rect_eq(a->rect, b->rect) &&
           a->kind == b->kind && a->cell_text_is_html == b->cell_text_is_html &&
           a->horizontal_align == b->horizontal_align && a->vertical_align == b->vertical_align &&
           a->outsets[0] == b->outsets[0] && a->outsets[1] == b->outsets[1] && a->outsets[2] == b->outsets[2] &&
           a->outsets[3] == b->outsets[3];
}

/* ---- slices ---- */

void pc_slices_fini(pc_slices *s)
{
    size_t i;
    if (s == NULL) {
        return;
    }
    pc_str_fini(&s->group_name);
    for (i = 0; i < s->len; i++) {
        pc_slice_fini(&s->list[i]);
    }
    free(s->list);
    memset(s, 0, sizeof *s);
}

pc_status pc_slices_clone(const pc_slices *s, pc_slices *out, pc_error *err)
{
    pc_slices t;
    size_t i;
    pc_status st;
    memset(&t, 0, sizeof t);
    st = pc_str_clone(&s->group_name, &t.group_name, err);
    if (st != PC_OK) {
        return st;
    }
    if (s->len > 0) {
        t.list = pc_calloc_array(s->len, sizeof *t.list);
        if (t.list == NULL) {
            pc_str_fini(&t.group_name);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < s->len; i++) {
            st = pc_slice_clone(&s->list[i], &t.list[i], err);
            if (st != PC_OK) {
                pc_slices_fini(&t); /* len counts the clones made so far */
                return st;
            }
            t.len = i + 1;
        }
    }
    *out = t;
    return PC_OK;
}

bool pc_slices_eq(const pc_slices *a, const pc_slices *b)
{
    size_t i;
    if (!pc_str_eq(&a->group_name, &b->group_name) || a->len != b->len) {
        return false;
    }
    for (i = 0; i < a->len; i++) {
        if (!pc_slice_eq(&a->list[i], &b->list[i])) {
            return false;
        }
    }
    return true;
}

bool pc_slices_is_empty(const pc_slices *s)
{
    return s->len == 0;
}

bool pc_slices_next_id(const pc_slices *s, uint32_t *out)
{
    uint32_t max = 0;
    size_t i;
    for (i = 0; i < s->len; i++) {
        if (s->list[i].id > max) {
            max = s->list[i].id;
        }
    }
    if (max == UINT32_MAX) {
        return false;
    }
    *out = max + 1;
    return true;
}

const pc_slice *pc_slices_get(const pc_slices *s, uint32_t id)
{
    size_t i;
    for (i = 0; i < s->len; i++) {
        if (s->list[i].id == id) {
            return &s->list[i];
        }
    }
    return NULL;
}

pc_slice *pc_slices_get_mut(pc_slices *s, uint32_t id)
{
    size_t i;
    for (i = 0; i < s->len; i++) {
        if (s->list[i].id == id) {
            return &s->list[i];
        }
    }
    return NULL;
}

void pc_resolved_slices_free(pc_resolved_slice *s, size_t n)
{
    size_t i;
    if (s == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        pc_str_fini(&s[i].name);
    }
    free(s);
}

/* ---- auto slices ---- */

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a;
    int32_t y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

/* Sort and remove duplicates (sort_unstable + dedup); returns the new length. */
static size_t sort_dedup(int32_t *v, size_t n)
{
    size_t i;
    size_t o = 0;
    if (n == 0) {
        return 0;
    }
    qsort(v, n, sizeof *v, cmp_i32);
    for (i = 1; i < n; i++) {
        if (v[i] != v[o]) {
            v[++o] = v[i];
        }
    }
    return o + 1;
}

static bool covered(const pc_rect *taken, size_t n, int32_t x0, int32_t y0)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const pc_rect *r = &taken[i];
        if (x0 >= r->x0 && x0 < r->x1 && y0 >= r->y0 && y0 < r->y1) {
            return true;
        }
    }
    return false;
}

static pc_status push_rect(pc_rect **v, size_t *len, size_t *cap, pc_rect r, pc_error *err)
{
    if (*len == *cap) {
        pc_rect *g = pc_grow_array(*v, cap, *len + 1, sizeof **v);
        if (g == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        *v = g;
    }
    (*v)[(*len)++] = r;
    return PC_OK;
}

pc_status pc_auto_slices(pc_rect canvas, const pc_rect *taken, size_t ntaken, pc_rect **out, size_t *n,
                         pc_error *err)
{
    int32_t *xs = NULL;
    int32_t *ys = NULL;
    size_t nx = 2;
    size_t ny = 2;
    size_t cap;
    pc_rect *runs = NULL;
    size_t nruns = 0;
    size_t runs_cap = 0;
    pc_rect *res = NULL;
    size_t nres = 0;
    size_t i;
    size_t j;
    pc_status st = PC_OK;

    if (pc_rect_is_empty(canvas)) {
        *out = NULL;
        *n = 0;
        return PC_OK;
    }
    if (!pc_size_mul(ntaken, 2, &cap) || !pc_size_add(cap, 2, &cap)) {
        return pc_fail(err, PC_ERR_LIMIT, "too many slices");
    }
    xs = pc_malloc_array(cap, sizeof *xs);
    ys = pc_malloc_array(cap, sizeof *ys);
    if (xs == NULL || ys == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    xs[0] = canvas.x0;
    xs[1] = canvas.x1;
    ys[0] = canvas.y0;
    ys[1] = canvas.y1;
    for (i = 0; i < ntaken; i++) {
        pc_rect r = pc_rect_intersect(taken[i], canvas);
        if (pc_rect_is_empty(r)) {
            continue;
        }
        xs[nx++] = r.x0;
        xs[nx++] = r.x1;
        ys[ny++] = r.y0;
        ys[ny++] = r.y1;
    }
    nx = sort_dedup(xs, nx);
    ny = sort_dedup(ys, ny);

    /* Horizontal runs of free cells per row band. */
    for (j = 0; j + 1 < ny && st == PC_OK; j++) {
        bool open = false;
        int32_t start = 0;
        for (i = 0; i + 1 < nx && st == PC_OK; i++) {
            bool free_cell = !covered(taken, ntaken, xs[i], ys[j]);
            if (free_cell && !open) {
                open = true;
                start = xs[i];
            } else if (!free_cell && open) {
                st = push_rect(&runs, &nruns, &runs_cap, pc_rect_make(start, ys[j], xs[i], ys[j + 1]), err);
                open = false;
            }
        }
        if (st == PC_OK && open) {
            st = push_rect(&runs, &nruns, &runs_cap, pc_rect_make(start, ys[j], xs[nx - 1], ys[j + 1]), err);
        }
    }
    if (st != PC_OK) {
        goto cleanup;
    }

    /* Merge each run into the first earlier rectangle with the same columns ending where it
     * starts. */
    if (nruns > 0) {
        res = pc_malloc_array(nruns, sizeof *res);
        if (res == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto cleanup;
        }
    }
    for (i = 0; i < nruns; i++) {
        pc_rect r = runs[i];
        bool merged = false;
        for (j = 0; j < nres; j++) {
            if (res[j].x0 == r.x0 && res[j].x1 == r.x1 && res[j].y1 == r.y0) {
                res[j].y1 = r.y1;
                merged = true;
                break;
            }
        }
        if (!merged) {
            res[nres++] = r;
        }
    }
    *out = res;
    *n = nres;
    res = NULL;

cleanup:
    free(xs);
    free(ys);
    free(runs);
    free(res);
    return st;
}

/* ---- resolve and naming ---- */

typedef struct slice_item {
    bool has_id;
    uint32_t id;
    pc_rect rect;
    size_t order; /* position before sorting: makes qsort stable like sort_by_key */
} slice_item;

static int cmp_item(const void *pa, const void *pb)
{
    const slice_item *a = pa;
    const slice_item *b = pb;
    if (a->rect.y0 != b->rect.y0) {
        return a->rect.y0 < b->rect.y0 ? -1 : 1;
    }
    if (a->rect.x0 != b->rect.x0) {
        return a->rect.x0 < b->rect.x0 ? -1 : 1;
    }
    return (a->order > b->order) - (a->order < b->order);
}

pc_status pc_slices_base_name(const pc_document *doc, pc_str *out, pc_error *err)
{
    const uint8_t *s = (const uint8_t *)doc->name.ptr;
    size_t len = doc->name.len;
    size_t i = 0;
    size_t o = 0;
    char *buf;
    pc_str t = PC_STR_INIT;
    pc_status st;

    /* rsplit_once('.'): everything before the last dot. */
    for (i = len; i > 0; i--) {
        if (s[i - 1] == '.') {
            len = i - 1;
            break;
        }
    }
    /* Every character maps to itself or to a one-byte '_', so the result is never longer. */
    buf = pc_malloc(len + 1);
    if (buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    i = 0;
    while (i < len) {
        size_t at = i;
        uint32_t cp;
        if (!pc_utf8_next(s, len, &i, &cp)) {
            /* Not reachable from valid UTF-8 (a Rust String); one byte becomes one '_'. */
            buf[o++] = '_';
            i = at + 1;
            continue;
        }
        if (pc_unicode_is_alphanumeric(cp) || cp == '-' || cp == '_') {
            memcpy(buf + o, s + at, i - at);
            o += i - at;
        } else {
            buf[o++] = '_';
        }
    }
    if (o == 0) {
        free(buf);
        st = pc_str_set_c(&t, "Untitled", err);
        if (st != PC_OK) {
            return st;
        }
        *out = t;
        return PC_OK;
    }
    buf[o] = '\0';
    *out = pc_str_adopt(buf, o);
    return PC_OK;
}

pc_status pc_slices_resolve(const pc_document *doc, pc_resolved_slice **out, size_t *n, pc_error *err)
{
    pc_rect canvas = pc_document_bounds(doc);
    pc_str base = PC_STR_INIT;
    pc_rect *rects = NULL;
    uint32_t *ids = NULL;
    size_t nstored = 0;
    pc_rect *autos = NULL;
    size_t nauto = 0;
    slice_item *items = NULL;
    size_t nitems = 0;
    pc_resolved_slice *res = NULL;
    size_t i;
    pc_status st;

    st = pc_slices_base_name(doc, &base, err);
    if (st != PC_OK) {
        return st;
    }
    /* The visible part of every stored slice, clipped to the canvas. */
    if (doc->slices.len > 0) {
        rects = pc_malloc_array(doc->slices.len, sizeof *rects);
        ids = pc_malloc_array(doc->slices.len, sizeof *ids);
        if (rects == NULL || ids == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto cleanup;
        }
    }
    for (i = 0; i < doc->slices.len; i++) {
        pc_rect r = pc_rect_intersect(doc->slices.list[i].rect, canvas);
        if (!pc_rect_is_empty(r)) {
            ids[nstored] = doc->slices.list[i].id;
            rects[nstored++] = r;
        }
    }
    st = pc_auto_slices(canvas, rects, nstored, &autos, &nauto, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    if (!pc_size_add(nstored, nauto, &nitems)) {
        st = pc_fail(err, PC_ERR_LIMIT, "too many slices");
        goto cleanup;
    }
    if (nitems > 0) {
        items = pc_malloc_array(nitems, sizeof *items);
        res = pc_calloc_array(nitems, sizeof *res);
        if (items == NULL || res == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto cleanup;
        }
    }
    for (i = 0; i < nstored; i++) {
        items[i] = (slice_item){true, ids[i], rects[i], i};
    }
    for (i = 0; i < nauto; i++) {
        items[nstored + i] = (slice_item){false, 0, autos[i], nstored + i};
    }
    if (nitems > 1) {
        qsort(items, nitems, sizeof *items, cmp_item);
    }
    for (i = 0; i < nitems; i++) {
        const pc_slice *s = items[i].has_id ? pc_slices_get(&doc->slices, items[i].id) : NULL;
        pc_resolved_slice *r = &res[i];
        r->number = i + 1;
        r->has_id = items[i].has_id;
        r->id = items[i].id;
        r->origin = s != NULL ? s->origin : PC_SLICE_AUTO;
        r->rect = items[i].rect;
        r->kind = s != NULL ? s->kind : PC_SLICE_IMAGE;
        if (s != NULL && s->name.len > 0) {
            st = pc_str_clone(&s->name, &r->name, err);
        } else if (nitems == 1) {
            st = pc_str_clone(&base, &r->name, err);
        } else {
            char num[32];
            int len = snprintf(num, sizeof num, "_%02zu", r->number);
            st = pc_str_clone(&base, &r->name, err);
            if (st == PC_OK && len > 0) {
                st = pc_str_append(&r->name, num, (size_t)len, err);
            }
        }
        if (st != PC_OK) {
            goto cleanup;
        }
    }
    *out = res;
    *n = nitems;
    res = NULL;

cleanup:
    pc_resolved_slices_free(res, nitems);
    free(items);
    free(autos);
    free(rects);
    free(ids);
    pc_str_fini(&base);
    return st;
}
