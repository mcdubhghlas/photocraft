/* Linked smart objects follow their files while the document is open, as in Photoshop (port of
 * crates/engine/src/link_watch.rs, E6): a file another app saves updates its layers without Layer >
 * Smart Objects > Update Modified Content. The active document's linked files are compared by size
 * and modification time with what they were when last seen; the layers of a file whose stamp moved
 * are re-rendered from it, one Update Modified Content undo step per file.
 *
 * layer.smartObjects.updateChanged takes the stamps on the spot (CLI, agents). The reference's
 * desktop app also stamps every two seconds and on window focus, on a worker thread
 * (Session::start_link_scan / poll_link_scan); that background path belongs to the desktop shell
 * (Phase 8) and is deferred (REWRITE_PLAN.md §7). The synchronous apply here is the whole detection
 * path and is identical to the one the worker thread feeds into (link_watch::apply_link_stamps).
 *
 * Stamps are kept per document (Session::link_stamps): a document in the background is looked at
 * when it comes to the front. A file seen for the first time is only remembered; one that can't be
 * read (another app is still writing it) is tried again once it changes again, not on every look.
 * Only smart objects of the document itself follow their files, not ones nested in another smart
 * object's contents (pc_document_walk does not descend into smart contents). */
#include <sys/stat.h>

/* A linked file of the document being looked at: its path (borrowed from the document, valid for
 * the call), the layers showing it, and its stamp now (present = the file could be stat()ed). */
struct lw_group {
    const char *path;
    pc_layer_id *ids;
    size_t nids, cap;
    bool present;
    uint64_t size;
    uint64_t mtime_sec;
    uint32_t mtime_nsec;
    bool changed;
};

/* A file's size and modification time now, or present=false when it can't be stat()ed (missing, or
 * an app saving by delete-and-rename). The modification time keeps the reference's full resolution
 * as (seconds, nanoseconds); comparing the pair is equivalent to Rust's u128 nanoseconds. */
static void lw_stamp_now(struct lw_group *g)
{
    struct stat st;
    if (stat(g->path, &st) != 0 || !S_ISREG(st.st_mode)) {
        g->present = false;
        return;
    }
    g->present = true;
    g->size = (uint64_t)st.st_size;
    g->mtime_sec = (uint64_t)st.st_mtim.tv_sec;
    g->mtime_nsec = (uint32_t)st.st_mtim.tv_nsec;
}

static bool lw_stamp_eq(const struct lw_group *g, uint64_t size, uint64_t sec, uint32_t nsec)
{
    return g->size == size && g->mtime_sec == sec && g->mtime_nsec == nsec;
}

static void lw_groups_free(struct lw_group *g, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(g[i].ids);
    }
    free(g);
}

/* The files of `d`'s linked smart objects, with the layers showing each, sorted by path
 * (link_watch::linked_files, a BTreeMap). *out owned (lw_groups_free). */
static int lw_group_cmp(const void *a, const void *b)
{
    return strcmp(((const struct lw_group *)a)->path, ((const struct lw_group *)b)->path);
}

static pc_status lw_linked_files(const pc_document *d, struct lw_group **out, size_t *nout, pc_error *err)
{
    pc_walk w = {0};
    pc_status rc = pc_document_walk(d, &w, err);
    if (rc != PC_OK) {
        return rc;
    }
    struct lw_group *g = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < w.len; i++) {
        const pc_layer *l = w.items[i].layer;
        if (l->content.kind != PC_LAYER_SMART || !l->content.u.smart.source.linked) {
            continue;
        }
        const char *path = pc_str_c(&l->content.u.smart.source.path);
        /* find-or-add the group for this path */
        size_t gi = n;
        for (size_t k = 0; k < n; k++) {
            if (strcmp(g[k].path, path) == 0) {
                gi = k;
                break;
            }
        }
        if (gi == n) {
            if (n == cap) {
                size_t nc = cap ? cap * 2 : 8;
                struct lw_group *np = realloc(g, nc * sizeof *np);
                if (np == NULL) {
                    lw_groups_free(g, n);
                    pc_walk_fini(&w);
                    return pc_fail(err, PC_ERR_NOMEM, "linked files");
                }
                g = np;
                cap = nc;
            }
            g[n] = (struct lw_group){.path = path};
            n++;
        }
        struct lw_group *grp = &g[gi];
        if (grp->nids == grp->cap) {
            size_t nc = grp->cap ? grp->cap * 2 : 4;
            pc_layer_id *np = realloc(grp->ids, nc * sizeof *np);
            if (np == NULL) {
                lw_groups_free(g, n);
                pc_walk_fini(&w);
                return pc_fail(err, PC_ERR_NOMEM, "linked file layers");
            }
            grp->ids = np;
            grp->cap = nc;
        }
        grp->ids[grp->nids++] = l->id;
    }
    pc_walk_fini(&w);
    if (n > 1) {
        qsort(g, n, sizeof *g, lw_group_cmp);
    }
    *out = g;
    *nout = n;
    return PC_OK;
}

/* Remember the stamp of (doc, path) (Session::link_stamps insert). */
static pc_status lw_insert(pc_engine_session *s, pc_doc_id doc, const char *path, uint64_t size,
                           uint64_t sec, uint32_t nsec)
{
    if (s->nlink_stamps == s->cap_link_stamps) {
        size_t nc = s->cap_link_stamps ? s->cap_link_stamps * 2 : 8;
        pc_engine_link_stamp *np = realloc(s->link_stamps, nc * sizeof *np);
        if (np == NULL) {
            return PC_ERR_NOMEM;
        }
        s->link_stamps = np;
        s->cap_link_stamps = nc;
    }
    size_t len = strlen(path);
    char *p = malloc(len + 1);
    if (p == NULL) {
        return PC_ERR_NOMEM;
    }
    memcpy(p, path, len + 1);
    s->link_stamps[s->nlink_stamps++] =
        (pc_engine_link_stamp){.doc = doc, .path = p, .size = size, .mtime_sec = sec, .mtime_nsec = nsec};
    return PC_OK;
}

/* Is document `id` still open? (link_watch's `open` set.) */
static bool lw_doc_open(pc_engine_session *s, pc_doc_id id)
{
    size_t ndocs = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(s, &ndocs);
    for (size_t i = 0; i < ndocs; i++) {
        if (pc_doc_arc_get(docs[i].doc)->id == id) {
            return true;
        }
    }
    return false;
}

/* Re-render a changed file's layers; fails (unchanged) if the file can't be read yet. */
struct lw_edit_ctx {
    const pc_layer_id *ids;
    size_t nids;
};

static pc_status lw_edit_cb(pc_document *doc, bool *has_active, pc_layer_id *active, void *v, pc_error *err)
{
    (void)has_active;
    (void)active;
    struct lw_edit_ctx *c = v;
    for (size_t i = 0; i < c->nids; i++) {
        bool updated = false;
        pc_status rc = pc_engine_smart_refresh(doc, c->ids[i], &updated, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (!updated) {
            return pc_engine_err_other(err, "the linked file can't be read");
        }
    }
    return PC_OK;
}

/* Compare `now` with the stamps last seen for `doc` and re-render the layers of the files that
 * changed (link_watch::apply_link_stamps). Builds {updated: [layer ids]}. */
static pc_status lw_apply(pc_engine_session *s, pc_doc_id doc, struct lw_group *now, size_t nnow,
                          pc_json_value *out, pc_error *err)
{
    pc_layer_id *updated = NULL;
    size_t nupd = 0, capupd = 0;

    /* The document went to the back meanwhile: it is looked at when it is in front again. */
    const pc_engine_doc_state *act = pc_engine_session_active(s);
    bool in_front = act != NULL && pc_doc_arc_get(act->doc)->id == doc;

    if (in_front) {
        /* This document's last stamps, moved out of link_stamps (which keeps only other open
         * documents); stamps of closed documents are dropped. `before` is sized once up front (at
         * most one entry per existing stamp) so the compaction below never allocates and so cannot
         * leave link_stamps half-rewritten. */
        struct before_stamp {
            char *path; /* owned (moved from link_stamps) */
            uint64_t size, sec;
            uint32_t nsec;
        } *before = NULL;
        size_t nbefore = 0;
        if (s->nlink_stamps > 0) {
            before = malloc(s->nlink_stamps * sizeof *before);
            if (before == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "link-watch stamps");
            }
        }
        size_t w = 0;
        for (size_t i = 0; i < s->nlink_stamps; i++) {
            pc_engine_link_stamp *e = &s->link_stamps[i];
            if (e->doc == doc) {
                before[nbefore].path = e->path;
                before[nbefore].size = e->size;
                before[nbefore].sec = e->mtime_sec;
                before[nbefore].nsec = e->mtime_nsec;
                nbefore++;
                e->path = NULL; /* dropped from link_stamps */
            } else if (lw_doc_open(s, e->doc)) {
                s->link_stamps[w++] = *e; /* keep; path retained */
            } else {
                free(e->path); /* drop a closed document's stamp */
            }
        }
        s->nlink_stamps = w;

        /* Update the stamps and mark the files that changed. */
        pc_status rc = PC_OK;
        for (size_t gi = 0; gi < nnow && rc == PC_OK; gi++) {
            struct lw_group *g = &now[gi];
            struct before_stamp *old = NULL;
            for (size_t j = 0; j < nbefore; j++) {
                if (strcmp(before[j].path, g->path) == 0) {
                    old = &before[j];
                    break;
                }
            }
            if (!g->present) {
                /* Not there right now (an app saving by delete and rename): keep the last stamp. */
                if (old != NULL) {
                    rc = lw_insert(s, doc, g->path, old->size, old->sec, old->nsec);
                }
            } else {
                if (old != NULL && !lw_stamp_eq(g, old->size, old->sec, old->nsec)) {
                    g->changed = true;
                }
                rc = lw_insert(s, doc, g->path, g->size, g->mtime_sec, g->mtime_nsec);
            }
        }
        for (size_t j = 0; j < nbefore; j++) {
            free(before[j].path);
        }
        free(before);
        if (rc != PC_OK) {
            return pc_fail(err, rc, "link-watch stamps");
        }

        /* Re-render the changed files, one Update Modified Content undo step per file. A file that
         * can't be read (still being written, or not a format smart objects read) leaves nothing
         * behind and is tried again once it changes again, not on every look. */
        for (size_t gi = 0; gi < nnow; gi++) {
            struct lw_group *g = &now[gi];
            if (!g->changed) {
                continue;
            }
            struct lw_edit_ctx ctx = {g->ids, g->nids};
            pc_error scratch;
            pc_error_clear(&scratch);
            if (pc_engine_session_edit(s, "Update Modified Content", lw_edit_cb, &ctx, &scratch) == PC_OK) {
                for (size_t j = 0; j < g->nids; j++) {
                    if (nupd == capupd) {
                        size_t nc = capupd ? capupd * 2 : 8;
                        pc_layer_id *np = realloc(updated, nc * sizeof *np);
                        if (np == NULL) {
                            free(updated);
                            return pc_fail(err, PC_ERR_NOMEM, "updated layers");
                        }
                        updated = np;
                        capupd = nc;
                    }
                    updated[nupd++] = g->ids[j];
                }
            }
        }
    }

    /* Build {updated: [layer ids]}. */
    yyjson_mut_doc *dj = NULL;
    if (pc_engine_result_doc(out, &dj, err) != PC_OK) {
        free(updated);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(dj);
    yyjson_mut_val *arr = yyjson_mut_arr(dj);
    for (size_t i = 0; i < nupd; i++) {
        yyjson_mut_arr_add_uint(dj, arr, (uint64_t)updated[i]);
    }
    yyjson_mut_obj_add_val(dj, root, "updated", arr);
    yyjson_mut_doc_set_root(dj, root);
    free(updated);
    return PC_OK;
}

/* layer.smartObjects.updateChanged: stamp the active document's linked files now and update the
 * layers of the ones that changed (link_watch::update_changed). */
static pc_status cn_update_changed(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_doc_id doc = pc_doc_arc_get(d->doc)->id;
    struct lw_group *now = NULL;
    size_t nnow = 0;
    pc_status rc = lw_linked_files(pc_doc_arc_get(d->doc), &now, &nnow, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < nnow; i++) {
        lw_stamp_now(&now[i]);
    }
    rc = lw_apply(s, doc, now, nnow, out, err);
    lw_groups_free(now, nnow);
    return rc;
}
