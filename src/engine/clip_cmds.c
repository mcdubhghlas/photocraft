/* Clip-group tracking (port of crates/engine/src/clip_group.rs) and the single-layer Layer >
 * Create/Release Clipping Mask and Layer > Arrange commands (crates/engine/src/commands.rs
 * set_clipped / arrange / reverse). E5 deferred these to the chunk that provides clip_group and
 * the Background helpers (see layer_multi_cmds.c's scope note). */
#include <stdlib.h>

#include "doc/tree.h"
#include "layer_base_cmds.h"

/* ---- Background helpers (port of background_cmds::background_id / keep_background_at_bottom) --- */

static bool background_id(const pc_document *doc, pc_layer_id *out)
{
    const pc_layer *bg = pc_document_background(doc);
    if (bg == NULL) {
        return false;
    }
    *out = bg->id;
    return true;
}

/* The Background must stay at the bottom of the root stack. */
static pc_status keep_background_at_bottom(bool has_bg, pc_layer_id bg, const pc_document *doc,
                                           pc_error *err)
{
    if (has_bg && !(doc->nlayers > 0 && doc->layers[0].id == bg)) {
        return pc_engine_err_other(
            err, "the Background layer stays at the bottom of the stack (Layer \xe2\x80\xba New \xe2\x80\xba "
                 "Layer from Background makes it a normal layer)");
    }
    return PC_OK;
}

/* ---- clip-group helpers (operate on a document; read side is const) ----------------------- */

static bool clip_in(const pc_layer_id *arr, size_t n, pc_layer_id id)
{
    for (size_t i = 0; i < n; i++) {
        if (arr[i] == id) {
            return true;
        }
    }
    return false;
}

/* Borrow the sibling array holding `id` and id's index in it. false when id is absent. */
static bool clip_siblings(const pc_document *doc, pc_layer_id id, const pc_layer **list, size_t *nlist,
                          size_t *idx)
{
    pc_layer_path path;
    if (pc_document_path_of(doc, id, &path, NULL) != PC_OK) {
        return false;
    }
    bool ok = false;
    if (path.len >= 1) {
        size_t k = path.idx[path.len - 1];
        const pc_layer *arr = NULL;
        size_t n = 0;
        if (path.len == 1) {
            arr = doc->layers;
            n = doc->nlayers;
        } else {
            const pc_layer *parent = pc_document_layer_at(doc, path.idx, path.len - 1);
            if (parent != NULL) {
                arr = pc_layer_children(parent, &n);
            }
        }
        if (arr != NULL && k < n) {
            *list = arr;
            *nlist = n;
            *idx = k;
            ok = true;
        }
    }
    pc_layer_path_fini(&path);
    return ok;
}

/* The layer `delta` slots above (+) or below (-) `id`, or NULL. */
static const pc_layer *clip_neighbour(const pc_document *doc, pc_layer_id id, int delta)
{
    const pc_layer *list = NULL;
    size_t n = 0, idx = 0;
    if (!clip_siblings(doc, id, &list, &n, &idx)) {
        return NULL;
    }
    size_t j;
    if (delta < 0) {
        size_t down = (size_t)(-(int64_t)delta);
        if (down > idx) {
            return NULL;
        }
        j = idx - down;
    } else {
        j = idx + (size_t)delta;
    }
    if (j >= n) {
        return NULL;
    }
    return &list[j];
}

/* The clip group `id` belongs to, base first, when it has one (len > 1); else empty. On OOM
 * fills err and leaves an empty result. */
static pc_status clip_group_of(const pc_document *doc, pc_layer_id id, pc_layer_id **out, size_t *nout,
                               pc_error *err)
{
    *out = NULL;
    *nout = 0;
    const pc_layer *list = NULL;
    size_t n = 0, idx = 0;
    if (!clip_siblings(doc, id, &list, &n, &idx)) {
        return PC_OK;
    }
    /* base: the last index at or below idx that is not clipped. */
    size_t base = 0;
    bool found = false;
    for (size_t i = idx + 1; i-- > 0;) {
        if (!list[i].clipped) {
            base = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return PC_OK;
    }
    pc_layer_id *g = pc_malloc_array(n - base, sizeof *g);
    if (g == NULL) { /* base <= idx < n, so n - base >= 1: the allocation is always needed */
        return pc_fail(err, PC_ERR_NOMEM, "clip group");
    }
    size_t cnt = 0;
    for (size_t i = base; i < n; i++) {
        if (i == base || list[i].clipped) {
            g[cnt++] = list[i].id;
        } else {
            break;
        }
    }
    if (cnt > 1) {
        *out = g;
        *nout = cnt;
    } else {
        free(g);
    }
    return PC_OK;
}

static void set_clipped_flag(pc_document *doc, pc_layer_id id, bool clipped)
{
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l != NULL) {
        l->clipped = clipped;
    }
}

/* Rule 4: release the clipped run at the bottom of every sibling list (nothing below to clip to).
 * One explicit stack, each list visited once, so a deep document can't recurse without bound. */
static void clip_release_orphans(pc_document *doc)
{
    typedef struct {
        pc_layer *arr;
        size_t n;
    } frame;
    frame inlinebuf[PC_MAX_GROUP_DEPTH + 2];
    frame *stack = inlinebuf;
    size_t cap = PC_MAX_GROUP_DEPTH + 2;
    size_t top = 0;
    stack[top++] = (frame){doc->layers, doc->nlayers};
    while (top > 0) {
        frame f = stack[--top];
        for (size_t i = 0; i < f.n && f.arr[i].clipped; i++) {
            f.arr[i].clipped = false;
        }
        for (size_t i = 0; i < f.n; i++) {
            if (pc_layer_is_group(&f.arr[i])) {
                size_t cn = f.arr[i].content.u.group.nchildren;
                pc_layer *ch = f.arr[i].content.u.group.children;
                if (cn == 0 || ch == NULL) {
                    continue;
                }
                if (top == cap) {
                    size_t ncap = cap * 2;
                    frame *grown = pc_malloc_array(ncap, sizeof *grown);
                    if (grown == NULL) {
                        /* Out of memory: stop early. The remaining lists keep their clipped
                         * flags (the compositor still draws an orphan run unclipped). */
                        if (stack != inlinebuf) {
                            free(stack);
                        }
                        return;
                    }
                    for (size_t k = 0; k < top; k++) {
                        grown[k] = stack[k];
                    }
                    if (stack != inlinebuf) {
                        free(stack);
                    }
                    stack = grown;
                    cap = ncap;
                }
                stack[top++] = (frame){ch, cn};
            }
        }
    }
    if (stack != inlinebuf) {
        free(stack);
    }
}

/* ---- the Before snapshot ------------------------------------------------------------------ */

void pc_engine_clip_before_fini(pc_engine_clip_before *b)
{
    if (b == NULL) {
        return;
    }
    free(b->moving);
    free(b->clipped);
    for (size_t i = 0; i < b->ngroups; i++) {
        free(b->groups[i].ids);
    }
    free(b->groups);
    free(b->left);
    *b = (pc_engine_clip_before){0};
}

pc_status pc_engine_clip_before_read(const pc_document *doc, const pc_layer_id *moving, size_t nmoving,
                                     pc_engine_clip_before *out, pc_error *err)
{
    *out = (pc_engine_clip_before){0};
    if (nmoving == 0) {
        return PC_OK;
    }
    out->moving = pc_malloc_array(nmoving, sizeof *out->moving);
    out->clipped = pc_malloc_array(nmoving, sizeof *out->clipped);
    out->groups = pc_calloc_array(nmoving, sizeof *out->groups);
    /* `left`: at most one clipped-to entry per moving layer's group member. Bound generously by
     * the total layer count. */
    size_t total = 0;
    (void)pc_document_layer_count(doc, &total, NULL);
    out->left = total > 0 ? pc_malloc_array(total, sizeof *out->left) : NULL;
    if (out->moving == NULL || out->clipped == NULL || out->groups == NULL ||
        (total > 0 && out->left == NULL)) {
        pc_engine_clip_before_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "clip before");
    }
    out->nmoving = nmoving;
    for (size_t i = 0; i < nmoving; i++) {
        out->moving[i] = moving[i];
    }
    for (size_t i = 0; i < nmoving; i++) {
        const pc_layer *l = pc_document_layer(doc, moving[i]);
        if (l != NULL && l->clipped) {
            out->clipped[out->nclipped++] = moving[i];
        }
    }
    for (size_t i = 0; i < nmoving; i++) {
        pc_layer_id *g = NULL;
        size_t ng = 0;
        if (clip_group_of(doc, moving[i], &g, &ng, err) != PC_OK) {
            pc_engine_clip_before_fini(out);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (ng > 0) {
            out->groups[out->ngroups].id = moving[i];
            out->groups[out->ngroups].ids = g;
            out->groups[out->ngroups].n = ng;
            out->ngroups++;
        }
    }
    /* left: layers clipped to a moving base, except the moving ones; deduplicated. */
    for (size_t i = 0; i < nmoving; i++) {
        pc_layer_id *g = NULL;
        size_t ng = 0;
        if (clip_group_of(doc, moving[i], &g, &ng, err) != PC_OK) {
            pc_engine_clip_before_fini(out);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        /* clipped_to(id): the group's rest when id is its base. */
        if (ng > 1 && g[0] == moving[i]) {
            for (size_t k = 1; k < ng && out->left != NULL; k++) {
                if (!clip_in(moving, nmoving, g[k]) && !clip_in(out->left, out->nleft, g[k])) {
                    out->left[out->nleft++] = g[k];
                }
            }
        }
        free(g);
    }
    return PC_OK;
}

/* Group ids for a moving layer, or NULL/0. */
static const pc_layer_id *before_group(const pc_engine_clip_before *b, pc_layer_id id, size_t *n)
{
    *n = 0;
    if (b == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < b->ngroups; i++) {
        if (b->groups[i].id == id) {
            *n = b->groups[i].n;
            return b->groups[i].ids;
        }
    }
    return NULL;
}

static void clip_settle_one(pc_document *doc, pc_layer_id id, const pc_engine_clip_before *before,
                            bool force)
{
    size_t gn = 0;
    const pc_layer_id *group = before_group(before, id, &gn);
    const pc_layer *a = clip_neighbour(doc, id, 1);
    bool joins = false;
    if (a != NULL && a->clipped) {
        bool travelling = before != NULL && clip_in(before->moving, before->nmoving, a->id) &&
                          clip_in(group, gn, a->id);
        joins = !travelling;
    }
    bool stays = false;
    if (before != NULL && clip_in(before->clipped, before->nclipped, id)) {
        const pc_layer *b = clip_neighbour(doc, id, -1);
        if (b != NULL && (b->clipped || clip_in(group, gn, b->id))) {
            stays = true;
        }
    }
    set_clipped_flag(doc, id, force || joins || stays);
}

/* Order `placed` top-to-bottom (paths descending). Only ever 0 or 1 for this chunk's callers;
 * general for future ones (moveTo). Returns a malloc'd array of ids in that order, or NULL on
 * OOM or when nplaced is 0. */
static pc_status order_placed_top_first(const pc_document *doc, const pc_layer_id *placed,
                                        size_t nplaced, pc_layer_id **out, pc_error *err)
{
    *out = NULL;
    if (nplaced == 0) {
        return PC_OK;
    }
    pc_layer_id *ids = pc_malloc_array(nplaced, sizeof *ids);
    if (ids == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "settle order");
    }
    for (size_t i = 0; i < nplaced; i++) {
        ids[i] = placed[i];
    }
    if (nplaced > 1) {
        /* Insertion sort by path, descending (top first). A missing layer sorts last. */
        for (size_t i = 1; i < nplaced; i++) {
            pc_layer_id key = ids[i];
            pc_layer_path pk;
            bool pk_ok = pc_document_path_of(doc, key, &pk, NULL) == PC_OK;
            size_t j = i;
            while (j > 0) {
                pc_layer_path pj;
                bool pj_ok = pc_document_path_of(doc, ids[j - 1], &pj, NULL) == PC_OK;
                /* "ids[j-1] < key" (ascending) means ids[j-1] should stay before key in a
                 * descending-by-path order? We want descending: larger path first. Compare. */
                int cmp = 0; /* cmp>0 : key's path > ids[j-1]'s path */
                if (pk_ok && pj_ok) {
                    size_t m = pk.len < pj.len ? pk.len : pj.len;
                    for (size_t t = 0; t < m; t++) {
                        if (pk.idx[t] != pj.idx[t]) {
                            cmp = pk.idx[t] > pj.idx[t] ? 1 : -1;
                            break;
                        }
                    }
                    if (cmp == 0) {
                        cmp = pk.len > pj.len ? 1 : (pk.len < pj.len ? -1 : 0);
                    }
                }
                if (pj_ok) {
                    pc_layer_path_fini(&pj);
                }
                if (cmp > 0) { /* key is higher: it belongs before ids[j-1] */
                    ids[j] = ids[j - 1];
                    j--;
                } else {
                    break;
                }
            }
            if (pk_ok) {
                pc_layer_path_fini(&pk);
            }
            ids[j] = key;
        }
    }
    *out = ids;
    return PC_OK;
}

pc_status pc_engine_clip_settle(pc_document *doc, const pc_layer_id *placed, size_t nplaced,
                                const pc_engine_clip_before *before, bool force, pc_error *err)
{
    pc_layer_id *order = NULL;
    if (order_placed_top_first(doc, placed, nplaced, &order, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < nplaced; i++) {
        clip_settle_one(doc, order[i], before, force);
    }
    free(order);
    if (before != NULL) {
        for (size_t i = 0; i < before->nleft; i++) {
            if (!clip_in(placed, nplaced, before->left[i])) {
                set_clipped_flag(doc, before->left[i], false);
            }
        }
    }
    clip_release_orphans(doc);
    return PC_OK;
}

/* ---- commands ----------------------------------------------------------------------------- */

/* Create (clip) / Release Clipping Mask. */
typedef struct {
    pc_layer_id *ids;
    size_t n;
    bool clip;
} clip_ctx;

static pc_status edit_set_clipped(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    clip_ctx *ctx = vctx;
    for (size_t i = 0; i < ctx->n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, ctx->ids[i]);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, ctx->ids[i]);
        }
        l->clipped = ctx->clip;
    }
    return PC_OK;
}

static pc_status run_set_clipped(pc_engine_session *s, yyjson_val *p, pc_json_value *out, bool clip,
                                 pc_error *err)
{
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    yyjson_val *lv = (p != NULL) ? pc_json_get(p, "layer") : NULL;
    if (lv != NULL && !yyjson_is_null(lv)) {
        pc_layer_id id;
        if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        ids = pc_malloc_array(1, sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "clipping mask");
        }
        ids[0] = id;
        nids = 1;
    } else {
        pc_layer_id *sel = NULL;
        size_t nsel = 0;
        if (pc_engine_selected(s, &sel, &nsel, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (nsel == 0) {
            free(sel);
            pc_layer_id id;
            if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_INVALID;
            }
            ids = pc_malloc_array(1, sizeof *ids);
            if (ids == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "clipping mask");
            }
            ids[0] = id;
            nids = 1;
        } else if (nsel == 1) {
            ids = sel;
            nids = 1;
        } else if (clip) {
            /* Create: every selected layer but the lowest (the base). */
            nids = nsel - 1;
            ids = pc_malloc_array(nids, sizeof *ids);
            if (ids == NULL) {
                free(sel);
                return pc_fail(err, PC_ERR_NOMEM, "clipping mask");
            }
            for (size_t i = 0; i < nids; i++) {
                ids[i] = sel[i + 1];
            }
            free(sel);
        } else {
            ids = sel;
            nids = nsel;
        }
    }
    clip_ctx ctx = {ids, nids, clip};
    const char *label = clip ? "Create Clipping Mask" : "Release Clipping Mask";
    if (pc_engine_session_edit(s, label, edit_set_clipped, &ctx, err) != PC_OK) {
        free(ids);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(ids);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < nids; i++) {
        yyjson_mut_arr_add_uint(rd, arr, ids[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "layers", arr);
    yyjson_mut_doc_set_root(rd, root);
    free(ids);
    return PC_OK;
}

static pc_status run_create_clip(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_set_clipped(s, p, out, true, err);
}
static pc_status run_release_clip(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_set_clipped(s, p, out, false, err);
}

/* Arrange (Bring Forward / Send Backward / to Front / to Back). */
typedef struct {
    pc_layer_id id;
    int32_t delta;
} arrange_ctx;

static pc_status edit_arrange(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    arrange_ctx *ctx = vctx;
    pc_layer_id bg = 0;
    bool has_bg = background_id(doc, &bg);
    if (has_bg && bg == ctx->id) {
        return pc_engine_err_other(err, "the Background layer can't be moved");
    }
    int32_t steps = ctx->delta;
    if (steps == INT32_MAX) {
        steps = 10000;
    } else if (steps == INT32_MIN) {
        steps = -10000;
    }
    int32_t dir = steps > 0 ? 1 : -1;
    int32_t count = steps > 0 ? steps : -steps;
    pc_engine_clip_before before;
    if (pc_engine_clip_before_read(doc, &ctx->id, 1, &before, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    bool moved = false;
    for (int32_t i = 0; i < count; i++) {
        if (!pc_document_shift(doc, ctx->id, dir)) {
            break;
        }
        if (keep_background_at_bottom(has_bg, bg, doc, NULL) != PC_OK) {
            (void)pc_document_shift(doc, ctx->id, -dir);
            break;
        }
        moved = true;
    }
    if (!moved) {
        pc_engine_clip_before_fini(&before);
        return pc_engine_err_other(err, "layer can't move further");
    }
    pc_status rc = pc_engine_clip_settle(doc, &ctx->id, 1, &before, false, err);
    pc_engine_clip_before_fini(&before);
    return rc;
}

static pc_status run_arrange(pc_engine_session *s, yyjson_val *p, pc_json_value *out, int32_t delta,
                             pc_error *err)
{
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    arrange_ctx ctx = {id, delta};
    if (pc_engine_session_edit(s, "Arrange", edit_arrange, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_bring_forward(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_arrange(s, p, out, 1, err);
}
static pc_status run_send_backward(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_arrange(s, p, out, -1, err);
}
static pc_status run_bring_to_front(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_arrange(s, p, out, INT32_MAX, err);
}
static pc_status run_send_to_back(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_arrange(s, p, out, INT32_MIN, err);
}

/* Arrange > Reverse: reverse the stacking of two or more selected top-level layers. */
typedef struct {
    pc_layer_id *sel;
    size_t n;
} reverse_ctx;

static pc_status edit_reverse(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    reverse_ctx *ctx = vctx;
    pc_layer_id bg = 0;
    bool has_bg = background_id(doc, &bg);
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (pc_engine_top_level(doc, ctx->sel, ctx->n, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (nids < 2) {
        free(ids);
        return pc_engine_err_other(err, "select two or more layers to reverse");
    }
    /* Paths of each target (disjoint slots), and a clone of each. */
    pc_layer_path *paths = pc_calloc_array(nids, sizeof *paths);
    pc_layer *clones = pc_calloc_array(nids, sizeof *clones);
    if (paths == NULL || clones == NULL) {
        free(paths);
        free(clones);
        free(ids);
        return pc_fail(err, PC_ERR_NOMEM, "reverse");
    }
    pc_status rc = PC_OK;
    size_t got = 0;
    for (; got < nids; got++) {
        if (pc_document_path_of(doc, ids[got], &paths[got], err) != PC_OK) {
            rc = pc_engine_err_no_layer(err, ids[got]);
            break;
        }
        const pc_layer *src = pc_document_layer(doc, ids[got]);
        if (src == NULL || pc_layer_clone(src, &clones[got], err) != PC_OK) {
            pc_layer_path_fini(&paths[got]);
            rc = (src == NULL) ? pc_engine_err_no_layer(err, ids[got]) : (err != NULL ? err->code : PC_ERR_NOMEM);
            break;
        }
    }
    if (rc == PC_OK) {
        for (size_t k = 0; k < nids; k++) {
            pc_layer *dst = pc_document_layer_at_mut(doc, paths[k].idx, paths[k].len);
            if (dst == NULL) {
                rc = pc_engine_err_other(err, "layer tree changed");
                break;
            }
            pc_layer_fini(dst);
            *dst = clones[nids - 1 - k]; /* move the clone in */
            clones[nids - 1 - k] = (pc_layer){0};
        }
        if (rc == PC_OK) {
            rc = keep_background_at_bottom(has_bg, bg, doc, err);
        }
    }
    for (size_t k = 0; k < got; k++) {
        pc_layer_path_fini(&paths[k]);
    }
    for (size_t k = 0; k < nids; k++) {
        pc_layer_fini(&clones[k]); /* moved-out clones are zeroed, fini is a noop */
    }
    free(paths);
    free(clones);
    free(ids);
    return rc;
}

static pc_status run_reverse(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_layer_id *sel = NULL;
    size_t n = 0;
    if (pc_engine_selected(s, &sel, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    reverse_ctx ctx = {sel, n};
    if (pc_engine_session_edit(s, "Reverse", edit_reverse, &ctx, err) != PC_OK) {
        free(sel);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    free(sel);
    return pc_engine_result_null(out, err);
}

/* ---- enabled predicates ------------------------------------------------------------------- */

static pc_status enabled_two_plus(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    pc_layer_id *sel = NULL;
    size_t n = 0;
    (void)pc_engine_doc_state_selected_layers(d, &sel, &n, NULL);
    free(sel);
    if (n < 2) {
        return pc_fail(reason, PC_ERR_INVALID, "select two or more layers");
    }
    return PC_OK;
}

static const char *const MENU_LAYER[] = {"Layer"};
static const char *const MENU_ARRANGE[] = {"Layer", "Arrange"};

static const pc_engine_command SPECS[] = {
    {"layer.createClippingMask", "Create Clipping Mask", MENU_LAYER, 1, "Cmd+Alt+G",
     "{\"layer\":id?} (no layer: every selected layer but the lowest clips to the layer below it)",
     pc_engine_has_layer, run_create_clip, true},
    {"layer.releaseClippingMask", "Release Clipping Mask", MENU_LAYER, 1, NULL,
     "{\"layer\":id?} (no layer: every selected layer)", pc_engine_has_layer, run_release_clip, true},
    {"layer.arrange.bringForward", "Bring Forward", MENU_ARRANGE, 2, "Cmd+]", "{\"layer\":id?}",
     pc_engine_has_layer, run_bring_forward, true},
    {"layer.arrange.sendBackward", "Send Backward", MENU_ARRANGE, 2, "Cmd+[", "{\"layer\":id?}",
     pc_engine_has_layer, run_send_backward, true},
    {"layer.arrange.bringToFront", "Bring to Front", MENU_ARRANGE, 2, "Cmd+Shift+]", "{\"layer\":id?}",
     pc_engine_has_layer, run_bring_to_front, true},
    {"layer.arrange.sendToBack", "Send to Back", MENU_ARRANGE, 2, "Cmd+Shift+[", "{\"layer\":id?}",
     pc_engine_has_layer, run_send_to_back, true},
    {"layer.arrange.reverse", "Reverse", MENU_ARRANGE, 2, NULL, "{}", enabled_two_plus, run_reverse, true},
};

const pc_engine_command *pc_engine_clip_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
