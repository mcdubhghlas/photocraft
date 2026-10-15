/* Window > Layer Comps: capture, apply and manage layer comps (layerComp.*). Port of
 * crates/engine/src/comps_cmds.rs (E12).
 *
 * DEFERRED (owner: file_cmds/E3): file.export.layerCompsToFiles needs the static file-name helpers
 * (sanitize/join/stem) and native_doc of file_cmds.c, which that chunk did not export. The 14 core
 * comp commands are fully ported (REWRITE_PLAN.md §7). */
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "base/str.h"
#include "doc/comps.h"
#include "doc/doc.h"
#include "engine_internal.h"
#include "vector_cmds.h" /* pc_engine_translate_vectors */

static pc_status comps_has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return PC_OK;
}

static pc_status comps_has_comps(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (pc_doc_arc_get(d->doc)->nlayer_comps == 0) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no layer comps");
    }
    return PC_OK;
}

static pc_status comps_has_last_state(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!pc_doc_arc_get(d->doc)->has_last_document_state) {
        return pc_fail(reason, PC_ERR_INVALID, "no comp has been applied over the document state");
    }
    return PC_OK;
}

static size_t index_of(const pc_document *doc, uint32_t id)
{
    for (size_t i = 0; i < doc->nlayer_comps; i++) {
        if (doc->layer_comps[i].id == id) {
            return i;
        }
    }
    return 0;
}

/* The comp a command acts on: "comp" as id or name, else last applied, else first. */
static pc_status comp_param(const pc_document *doc, yyjson_val *p, const char *cmd, uint32_t *out,
                           pc_error *err)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, "comp") : NULL;
    if (v != NULL && yyjson_is_num(v)) {
        uint32_t id;
        if (pc_engine_param_u32_id(cmd, "comp", v, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        if (pc_document_comp(doc, id) == NULL) {
            return pc_engine_err_bad_params(err, cmd, "no layer comp with id %u", id);
        }
        *out = id;
        return PC_OK;
    }
    if (v != NULL && yyjson_is_str(v)) {
        size_t nl = 0;
        const char *name = pc_json_as_str(v, &nl);
        for (size_t i = 0; i < doc->nlayer_comps; i++) {
            if (pc_str_eq_mem(&doc->layer_comps[i].name, name, nl)) {
                *out = doc->layer_comps[i].id;
                return PC_OK;
            }
        }
        return pc_engine_err_bad_params(err, cmd, "no layer comp named \"%.*s\"", (int)nl, name);
    }
    if (v != NULL) {
        return pc_engine_err_bad_params(err, cmd, "\"comp\" must be a comp id or name");
    }
    /* default: last applied (if live), else first */
    if (doc->has_last_applied_comp && pc_document_comp(doc, doc->last_applied_comp) != NULL) {
        *out = doc->last_applied_comp;
        return PC_OK;
    }
    if (doc->nlayer_comps > 0) {
        *out = doc->layer_comps[0].id;
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, cmd, "the document has no layer comps");
}

static bool flag(yyjson_val *p, const char *key, bool def)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, key) : NULL;
    bool b;
    if (v != NULL && pc_json_as_bool(v, &b)) {
        return b;
    }
    return def;
}

/* Replace a comp's states with an owned array (freeing the old). */
static void comp_set_states(pc_layer_comp *c, pc_comp_layer_state *states, size_t n)
{
    pc_comp_layer_states_free(c->states, c->nstates);
    c->states = states;
    c->nstates = n;
}

/* apply_comp: restore comp's recorded state (only its options; all forces everything). */
static void apply_comp_impl(pc_document *doc, const pc_layer_comp *comp, bool all);
static void apply_comp_impl(pc_document *doc, const pc_layer_comp *comp, bool all)
{
    bool vis = all || comp->apply_visibility;
    bool pos = all || comp->apply_position;
    bool app = all || comp->apply_appearance;
    pc_walk walk;
    if (pc_document_walk(doc, &walk, NULL) != PC_OK) {
        return;
    }
    /* collect ids first (walk pointers are invalidated by mutation) */
    size_t nids = walk.len;
    pc_layer_id *ids = pc_malloc_array(nids > 0 ? nids : 1, sizeof *ids);
    if (ids == NULL) {
        pc_walk_fini(&walk);
        return;
    }
    for (size_t i = 0; i < nids; i++) {
        ids[i] = walk.items[i].layer->id;
    }
    pc_walk_fini(&walk);
    for (size_t i = 0; i < nids; i++) {
        pc_layer_id id = ids[i];
        const pc_comp_layer_state *st = pc_layer_comp_state(comp, id);
        if (st == NULL) {
            continue;
        }
        if (pos && st->has_position) {
            int32_t cur[2];
            const pc_layer *cl = pc_document_layer(doc, id);
            if (cl != NULL && pc_comp_layer_position(cl, cur)) {
                int64_t dx = (int64_t)st->position[0] - cur[0];
                int64_t dy = (int64_t)st->position[1] - cur[1];
                if ((dx != 0 || dy != 0) && dx >= INT32_MIN && dx <= INT32_MAX && dy >= INT32_MIN &&
                    dy <= INT32_MAX) {
                    pc_document snapshot;
                    if (pc_document_clone(doc, &snapshot, NULL) == PC_OK) {
                        pc_layer *l = pc_document_layer_mut(doc, id);
                        if (l != NULL) {
                            pc_engine_translate_layer(&snapshot, l, (int32_t)dx, (int32_t)dy);
                            pc_engine_translate_vectors(&snapshot, l, (double)dx, (double)dy);
                        }
                        pc_document_fini(&snapshot);
                    }
                }
            }
        }
        pc_layer *l = pc_document_layer_mut(doc, id);
        if (l == NULL) {
            continue;
        }
        if (vis && st->has_visible) {
            l->visible = st->visible;
        }
        if (app && st->has_appearance) {
            l->blend = st->appearance.blend;
            l->opacity = st->appearance.opacity;
            l->fill_opacity = st->appearance.fill_opacity;
            pc_effects eff;
            if (pc_effects_clone(&st->appearance.effects, &eff, NULL) == PC_OK) {
                pc_effects_fini(&l->effects);
                l->effects = eff;
            }
        }
    }
    free(ids);
}

static bool matches_last_comp(const pc_document *doc, const pc_comp_layer_state *now, size_t nnow)
{
    if (!doc->has_last_applied_comp) {
        return false;
    }
    const pc_layer_comp *comp = pc_document_comp(doc, doc->last_applied_comp);
    if (comp == NULL) {
        return false;
    }
    if (nnow != comp->nstates) {
        return false;
    }
    for (size_t i = 0; i < nnow; i++) {
        const pc_comp_layer_state *live = &now[i];
        const pc_comp_layer_state *saved = pc_layer_comp_state(comp, live->layer);
        if (saved == NULL) {
            return false;
        }
        if (comp->apply_visibility && saved->has_visible &&
            (!live->has_visible || saved->visible != live->visible)) {
            return false;
        }
        if (comp->apply_position && saved->has_position &&
            (!live->has_position || saved->position[0] != live->position[0] ||
             saved->position[1] != live->position[1])) {
            return false;
        }
        if (comp->apply_appearance && saved->has_appearance) {
            if (!live->has_appearance) {
                return false;
            }
            pc_comp_layer_state a = *saved, b = *live;
            /* compare appearance via state eq on singletons sharing layer id */
            pc_comp_layer_state sa, sb;
            memset(&sa, 0, sizeof sa);
            memset(&sb, 0, sizeof sb);
            sa.layer = a.layer;
            sa.has_appearance = true;
            sa.appearance = a.appearance;
            sb.layer = b.layer;
            sb.has_appearance = true;
            sb.appearance = b.appearance;
            if (!pc_comp_layer_state_eq(&sa, &sb)) {
                return false;
            }
        }
    }
    return true;
}

/* Append a comp to the doc's layer_comps (takes over *c on success). */
static pc_status doc_push_comp(pc_document *doc, pc_layer_comp *c, pc_error *err)
{
    size_t need = doc->nlayer_comps + 1;
    if (need == 0 || need > SIZE_MAX / sizeof *doc->layer_comps) {
        return pc_fail(err, PC_ERR_LIMIT, "layerComp");
    }
    pc_layer_comp *grown = realloc(doc->layer_comps, need * sizeof *doc->layer_comps);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "layerComp");
    }
    doc->layer_comps = grown;
    doc->layer_comps[doc->nlayer_comps] = *c;
    doc->nlayer_comps++;
    *c = (pc_layer_comp){0};
    return PC_OK;
}

/* ---- layerComp.new ------------------------------------------------------------------------ */

typedef struct {
    char name[256];
    bool has_name;
    char comment[1024];
    bool v, pos, a;
    uint32_t id_out;
} new_ctx;

static pc_status edit_new(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    new_ctx *c = ctx;
    uint32_t id = pc_next_comp_id(doc);
    pc_layer_comp comp;
    memset(&comp, 0, sizeof comp);
    comp.id = id;
    comp.apply_visibility = c->v;
    comp.apply_position = c->pos;
    comp.apply_appearance = c->a;
    pc_status rc = PC_OK;
    if (c->has_name) {
        rc = pc_str_set_c(&comp.name, c->name, err);
    } else {
        /* "Layer Comp N" for the first free N */
        char cand[64];
        for (size_t n = 1; n <= doc->nlayer_comps + 1; n++) {
            snprintf(cand, sizeof cand, "Layer Comp %zu", n);
            bool taken = false;
            for (size_t i = 0; i < doc->nlayer_comps; i++) {
                if (pc_str_eq_c(&doc->layer_comps[i].name, cand)) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                break;
            }
        }
        rc = pc_str_set_c(&comp.name, cand, err);
    }
    if (rc == PC_OK) {
        rc = pc_str_set_c(&comp.comment, c->comment, err);
    }
    if (rc == PC_OK) {
        rc = pc_comp_capture_states(doc, &comp.states, &comp.nstates, err);
    }
    if (rc != PC_OK) {
        pc_layer_comp_fini(&comp);
        return rc;
    }
    rc = doc_push_comp(doc, &comp, err);
    if (rc != PC_OK) {
        pc_layer_comp_fini(&comp);
        return rc;
    }
    doc->has_last_applied_comp = true;
    doc->last_applied_comp = id;
    c->id_out = id;
    return PC_OK;
}

static pc_status result_comp(pc_json_value *out, uint32_t id, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "comp", id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_new(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    new_ctx c;
    memset(&c, 0, sizeof c);
    c.v = flag(p, "visibility", true);
    c.pos = flag(p, "position", true);
    c.a = flag(p, "appearance", true);
    size_t nl = 0;
    const char *nm = NULL;
    yyjson_val *nv = pc_json_get(p, "name");
    if (nv != NULL && (nm = pc_json_as_str(nv, &nl)) != NULL && nl < sizeof c.name) {
        memcpy(c.name, nm, nl);
        c.name[nl] = '\0';
        c.has_name = true;
    }
    yyjson_val *cv = pc_json_get(p, "comment");
    size_t cl = 0;
    const char *cm = cv != NULL ? pc_json_as_str(cv, &cl) : NULL;
    if (cm != NULL && cl < sizeof c.comment) {
        memcpy(c.comment, cm, cl);
        c.comment[cl] = '\0';
    }
    pc_status rc = pc_engine_session_edit(s, "New Layer Comp", edit_new, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return result_comp(out, c.id_out, err);
}

/* ---- layerComp.update --------------------------------------------------------------------- */

typedef struct {
    uint32_t *ids;
    size_t nids;
    int what; /* 0 all, 1 visibility, 2 position, 3 appearance */
} update_ctx;

static pc_status edit_update(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    update_ctx *u = ctx;
    pc_comp_layer_state *now = NULL;
    size_t nnow = 0;
    pc_status rc = pc_comp_capture_states(doc, &now, &nnow, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t k = 0; k < u->nids; k++) {
        pc_layer_comp *c = NULL;
        for (size_t i = 0; i < doc->nlayer_comps; i++) {
            if (doc->layer_comps[i].id == u->ids[k]) {
                c = &doc->layer_comps[i];
                break;
            }
        }
        if (c == NULL) {
            continue;
        }
        if (u->what == 0) {
            pc_comp_layer_state *copy = NULL;
            size_t ncopy = 0;
            if (nnow > 0) {
                copy = pc_malloc_array(nnow, sizeof *copy);
                if (copy == NULL) {
                    pc_comp_layer_states_free(now, nnow);
                    return pc_fail(err, PC_ERR_NOMEM, "layerComp.update");
                }
                for (size_t i = 0; i < nnow; i++) {
                    if (pc_comp_layer_state_clone(&now[i], &copy[i], err) != PC_OK) {
                        pc_comp_layer_states_free(copy, i);
                        pc_comp_layer_states_free(now, nnow);
                        return PC_ERR_NOMEM;
                    }
                }
                ncopy = nnow;
            }
            comp_set_states(c, copy, ncopy);
            continue;
        }
        /* one property: refresh it on every live layer, keep the others as recorded */
        pc_comp_layer_state *states = nnow > 0 ? pc_malloc_array(nnow, sizeof *states) : NULL;
        if (nnow > 0 && states == NULL) {
            pc_comp_layer_states_free(now, nnow);
            return pc_fail(err, PC_ERR_NOMEM, "layerComp.update");
        }
        for (size_t i = 0; i < nnow; i++) {
            const pc_comp_layer_state *prev = pc_layer_comp_state(c, now[i].layer);
            pc_comp_layer_state st;
            memset(&st, 0, sizeof st);
            st.layer = now[i].layer;
            if (prev != NULL) {
                if (pc_comp_layer_state_clone(prev, &st, err) != PC_OK) {
                    pc_comp_layer_states_free(states, i);
                    pc_comp_layer_states_free(now, nnow);
                    return PC_ERR_NOMEM;
                }
            }
            if (u->what == 1) {
                st.has_visible = now[i].has_visible;
                st.visible = now[i].visible;
            } else if (u->what == 2) {
                st.has_position = now[i].has_position;
                st.position[0] = now[i].position[0];
                st.position[1] = now[i].position[1];
            } else {
                pc_comp_layer_state tmp;
                memset(&tmp, 0, sizeof tmp);
                tmp.layer = now[i].layer;
                tmp.has_appearance = now[i].has_appearance;
                tmp.appearance = now[i].appearance;
                /* copy appearance via clone of now[i] (appearance only) */
                if (st.has_appearance) {
                    pc_effects_fini(&st.appearance.effects);
                    st.has_appearance = false;
                }
                if (now[i].has_appearance) {
                    pc_comp_layer_state ap;
                    if (pc_comp_layer_state_clone(&now[i], &ap, err) == PC_OK) {
                        st.has_appearance = true;
                        st.appearance = ap.appearance;
                        ap.has_appearance = false; /* moved */
                        pc_comp_layer_state_fini(&ap);
                    }
                }
            }
            states[i] = st;
        }
        comp_set_states(c, states, nnow);
    }
    pc_comp_layer_states_free(now, nnow);
    return PC_OK;
}

static pc_status run_update(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layerComp.update";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    int what = 0;
    yyjson_val *wv = pc_json_get(p, "what");
    if (wv != NULL) {
        size_t wl = 0;
        const char *ws = pc_json_as_str(wv, &wl);
        if (ws == NULL) {
            return pc_engine_err_bad_params(err, CMD, "unknown \"what\"");
        }
        if (wl == 3 && memcmp(ws, "all", 3) == 0) {
            what = 0;
        } else if (wl == 10 && memcmp(ws, "visibility", 10) == 0) {
            what = 1;
        } else if (wl == 8 && memcmp(ws, "position", 8) == 0) {
            what = 2;
        } else if (wl == 10 && memcmp(ws, "appearance", 10) == 0) {
            what = 3;
        } else {
            return pc_engine_err_bad_params(err, CMD,
                                            "unknown \"what\" `%.*s` (all|visibility|position|appearance)",
                                            (int)wl, ws);
        }
    }
    uint32_t *ids = NULL;
    size_t nids = 0;
    yyjson_val *cv = pc_json_get(p, "comp");
    bool every = false;
    if (cv != NULL && yyjson_is_str(cv)) {
        size_t sl = 0;
        const char *ss = pc_json_as_str(cv, &sl);
        every = sl == 1 && ss[0] == '*';
    }
    if (every) {
        nids = doc->nlayer_comps;
        ids = pc_malloc_array(nids > 0 ? nids : 1, sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, CMD);
        }
        for (size_t i = 0; i < nids; i++) {
            ids[i] = doc->layer_comps[i].id;
        }
    } else {
        uint32_t id;
        pc_status rc = comp_param(doc, p, CMD, &id, err);
        if (rc != PC_OK) {
            return rc;
        }
        ids = pc_malloc_array(1, sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, CMD);
        }
        ids[0] = id;
        nids = 1;
    }
    const char *label = what == 1   ? "Update Layer Comp Visibility"
                        : what == 2 ? "Update Layer Comp Position"
                        : what == 3 ? "Update Layer Comp Appearance"
                                    : "Update Layer Comp";
    update_ctx u = {.ids = ids, .nids = nids, .what = what};
    pc_status rc = pc_engine_session_edit(s, label, edit_update, &u, err);
    if (rc != PC_OK) {
        free(ids);
        return rc;
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
    yyjson_mut_obj_add_val(rd, root, "updated", arr);
    yyjson_mut_doc_set_root(rd, root);
    free(ids);
    return PC_OK;
}

/* ---- apply / step ------------------------------------------------------------------------- */

static pc_status edit_apply(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    uint32_t id = *(const uint32_t *)ctx;
    const pc_layer_comp *comp0 = pc_document_comp(doc, id);
    if (comp0 == NULL) {
        return pc_engine_err_other(err, "no layer comp with id %u", id);
    }
    pc_layer_comp comp;
    if (pc_layer_comp_clone(comp0, &comp, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_comp_layer_state *states = NULL;
    size_t nstates = 0;
    pc_status rc = pc_comp_capture_states(doc, &states, &nstates, err);
    if (rc != PC_OK) {
        pc_layer_comp_fini(&comp);
        return rc;
    }
    if (!matches_last_comp(doc, states, nstates)) {
        pc_layer_comp lds;
        memset(&lds, 0, sizeof lds);
        lds.id = 0;
        lds.apply_visibility = true;
        lds.apply_position = true;
        lds.apply_appearance = true;
        lds.states = states;
        lds.nstates = nstates;
        states = NULL; /* moved */
        (void)pc_str_set_c(&lds.name, "Last Document State", NULL);
        if (doc->has_last_document_state) {
            pc_layer_comp_fini(&doc->last_document_state);
        }
        doc->last_document_state = lds;
        doc->has_last_document_state = true;
    }
    if (states != NULL) {
        pc_comp_layer_states_free(states, nstates);
    }
    apply_comp_impl(doc, &comp, false);
    pc_layer_comp_fini(&comp);
    doc->has_last_applied_comp = true;
    doc->last_applied_comp = id;
    return PC_OK;
}

static pc_status apply_comp_cmd(pc_engine_session *s, uint32_t id, const char *label,
                                pc_json_value *out, pc_error *err)
{
    pc_status rc = pc_engine_session_edit(s, label, edit_apply, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    size_t missing = 0;
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer_comp *c = pc_document_comp(doc, id);
    if (c != NULL) {
        pc_layer_id *ml = NULL;
        if (pc_layer_comp_missing_layers(c, doc, &ml, &missing, NULL) == PC_OK) {
            free(ml);
        } else {
            missing = 0;
        }
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "comp", id);
    yyjson_mut_obj_add_uint(rd, root, "missingLayers", missing);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_apply(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    uint32_t id;
    pc_status rc = comp_param(pc_doc_arc_get(d->doc), p, "layerComp.apply", &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    return apply_comp_cmd(s, id, "Apply Layer Comp", out, err);
}

static pc_status run_step(pc_engine_session *s, int64_t delta, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    int64_t n = (int64_t)doc->nlayer_comps;
    if (n == 0) {
        return pc_engine_err_bad_params(err, "layerComp.apply", "the document has no layer comps");
    }
    int64_t cur = -1;
    if (doc->has_last_applied_comp) {
        for (size_t i = 0; i < doc->nlayer_comps; i++) {
            if (doc->layer_comps[i].id == doc->last_applied_comp) {
                cur = (int64_t)i;
                break;
            }
        }
    }
    int64_t next;
    if (cur >= 0) {
        next = ((cur + delta) % n + n) % n;
    } else {
        next = delta > 0 ? 0 : n - 1;
    }
    uint32_t id = doc->layer_comps[next].id;
    return apply_comp_cmd(s, id, "Apply Layer Comp", out, err);
}

static pc_status run_prev(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_step(s, -1, out, err);
}
static pc_status run_next(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_step(s, 1, out, err);
}

/* ---- delete / duplicate / rename / comment / options -------------------------------------- */

static pc_status edit_delete(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    (void)err;
    uint32_t id = *(const uint32_t *)ctx;
    for (size_t i = 0; i < doc->nlayer_comps; i++) {
        if (doc->layer_comps[i].id == id) {
            pc_layer_comp_fini(&doc->layer_comps[i]);
            memmove(&doc->layer_comps[i], &doc->layer_comps[i + 1],
                    (doc->nlayer_comps - i - 1) * sizeof *doc->layer_comps);
            doc->nlayer_comps--;
            break;
        }
    }
    if (doc->has_last_applied_comp && doc->last_applied_comp == id) {
        doc->has_last_applied_comp = false;
    }
    return PC_OK;
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    uint32_t id;
    pc_status rc = comp_param(pc_doc_arc_get(d->doc), p, "layerComp.delete", &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_session_edit(s, "Delete Layer Comp", edit_delete, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "deleted", id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

typedef struct {
    uint32_t id;
    uint32_t new_id;
} dup_ctx;

static pc_status edit_duplicate(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    dup_ctx *dc = ctx;
    size_t i = index_of(doc, dc->id);
    if (i >= doc->nlayer_comps) {
        return pc_engine_err_other(err, "no layer comp");
    }
    pc_layer_comp c;
    if (pc_layer_comp_clone(&doc->layer_comps[i], &c, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    c.id = pc_next_comp_id(doc);
    /* name = "<name> copy" */
    pc_str newname = PC_STR_INIT;
    if (pc_str_clone(&c.name, &newname, err) == PC_OK) {
        (void)pc_str_append(&newname, " copy", 5, NULL);
        pc_str_fini(&c.name);
        c.name = newname;
    }
    uint32_t nid = c.id;
    size_t need = doc->nlayer_comps + 1;
    if (need == 0 || need > SIZE_MAX / sizeof *doc->layer_comps) {
        pc_layer_comp_fini(&c);
        return pc_fail(err, PC_ERR_LIMIT, "layerComp");
    }
    pc_layer_comp *grown = realloc(doc->layer_comps, need * sizeof *doc->layer_comps);
    if (grown == NULL) {
        pc_layer_comp_fini(&c);
        return pc_fail(err, PC_ERR_NOMEM, "layerComp");
    }
    doc->layer_comps = grown;
    memmove(&grown[i + 2], &grown[i + 1], (doc->nlayer_comps - i - 1) * sizeof *grown);
    grown[i + 1] = c;
    doc->nlayer_comps++;
    dc->new_id = nid;
    return PC_OK;
}

static pc_status run_duplicate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    dup_ctx dc = {0, 0};
    pc_status rc = comp_param(pc_doc_arc_get(d->doc), p, "layerComp.duplicate", &dc.id, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_session_edit(s, "Duplicate Layer Comp", edit_duplicate, &dc, err);
    if (rc != PC_OK) {
        return rc;
    }
    return result_comp(out, dc.new_id, err);
}

typedef struct {
    uint32_t id;
    const char *str;
    size_t len;
    int field; /* 0 name, 1 comment */
} setstr_ctx;

static pc_status edit_setstr(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    setstr_ctx *c = ctx;
    size_t i = index_of(doc, c->id);
    if (i >= doc->nlayer_comps) {
        return pc_engine_err_other(err, "no layer comp");
    }
    pc_str *dst = c->field == 0 ? &doc->layer_comps[i].name : &doc->layer_comps[i].comment;
    return pc_str_set(dst, c->str, c->len, err);
}

static pc_status run_rename(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layerComp.rename";
    size_t nl = 0;
    const char *nm = NULL;
    yyjson_val *nv = pc_json_get(p, "name");
    if (nv == NULL || (nm = pc_json_as_str(nv, &nl)) == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing \"name\"");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    uint32_t id;
    pc_status rc = comp_param(pc_doc_arc_get(d->doc), p, CMD, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    setstr_ctx c = {.id = id, .str = nm, .len = nl, .field = 0};
    rc = pc_engine_session_edit(s, "Rename Layer Comp", edit_setstr, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return result_comp(out, id, err);
}

static pc_status run_set_comment(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layerComp.setComment";
    size_t nl = 0;
    const char *nm = NULL;
    yyjson_val *nv = pc_json_get(p, "comment");
    if (nv == NULL || (nm = pc_json_as_str(nv, &nl)) == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing \"comment\"");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    uint32_t id;
    pc_status rc = comp_param(pc_doc_arc_get(d->doc), p, CMD, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    setstr_ctx c = {.id = id, .str = nm, .len = nl, .field = 1};
    rc = pc_engine_session_edit(s, "Layer Comp Options", edit_setstr, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return result_comp(out, id, err);
}

typedef struct {
    uint32_t id;
    int v, pos, a; /* -1 unset, 0 false, 1 true */
    const char *name;
    size_t name_len;
    const char *comment;
    size_t comment_len;
} opts_ctx;

static pc_status edit_set_options(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    opts_ctx *o = ctx;
    size_t i = index_of(doc, o->id);
    if (i >= doc->nlayer_comps) {
        return pc_engine_err_other(err, "no layer comp");
    }
    pc_layer_comp *c = &doc->layer_comps[i];
    if (o->v >= 0) {
        c->apply_visibility = o->v != 0;
    }
    if (o->pos >= 0) {
        c->apply_position = o->pos != 0;
    }
    if (o->a >= 0) {
        c->apply_appearance = o->a != 0;
    }
    if (o->name != NULL) {
        if (pc_str_set(&c->name, o->name, o->name_len, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    if (o->comment != NULL) {
        if (pc_str_set(&c->comment, o->comment, o->comment_len, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    return PC_OK;
}

static int tribool(yyjson_val *p, const char *k)
{
    yyjson_val *v = pc_json_get(p, k);
    bool b;
    if (v != NULL && pc_json_as_bool(v, &b)) {
        return b ? 1 : 0;
    }
    return -1;
}

static pc_status run_set_options(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layerComp.setOptions";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    uint32_t id;
    pc_status rc = comp_param(pc_doc_arc_get(d->doc), p, CMD, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    opts_ctx o;
    memset(&o, 0, sizeof o);
    o.id = id;
    o.v = tribool(p, "visibility");
    o.pos = tribool(p, "position");
    o.a = tribool(p, "appearance");
    yyjson_val *nv = pc_json_get(p, "name");
    if (nv != NULL) {
        o.name = pc_json_as_str(nv, &o.name_len);
    }
    yyjson_val *cv = pc_json_get(p, "comment");
    if (cv != NULL) {
        o.comment = pc_json_as_str(cv, &o.comment_len);
    }
    if (o.v < 0 && o.pos < 0 && o.a < 0 && o.name == NULL && cv == NULL) {
        return pc_engine_err_bad_params(
            err, CMD, "give \"visibility\", \"position\", \"appearance\", \"name\" or \"comment\"");
    }
    rc = pc_engine_session_edit(s, "Layer Comp Options", edit_set_options, &o, err);
    if (rc != PC_OK) {
        return rc;
    }
    return result_comp(out, id, err);
}

/* ---- restore last / warnings / list ------------------------------------------------------- */

static pc_status edit_restore_last(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    (void)ctx;
    if (!doc->has_last_document_state) {
        return pc_engine_err_other(err, "no Last Document State");
    }
    pc_layer_comp last;
    if (pc_layer_comp_clone(&doc->last_document_state, &last, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    apply_comp_impl(doc, &last, true);
    pc_layer_comp_fini(&last);
    doc->has_last_applied_comp = false;
    return PC_OK;
}

static pc_status run_restore_last(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_status rc = pc_engine_session_edit(s, "Restore Last Document State", edit_restore_last, NULL, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

typedef struct {
    bool has_only;
    uint32_t only;
} warn_ctx;

static pc_status edit_clear_warnings(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    warn_ctx *w = ctx;
    pc_walk walk;
    if (pc_document_walk(doc, &walk, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < doc->nlayer_comps; i++) {
        pc_layer_comp *c = &doc->layer_comps[i];
        if (w->has_only && c->id != w->only) {
            continue;
        }
        size_t keep = 0;
        for (size_t k = 0; k < c->nstates; k++) {
            bool live = false;
            for (size_t j = 0; j < walk.len; j++) {
                if (walk.items[j].layer->id == c->states[k].layer) {
                    live = true;
                    break;
                }
            }
            if (live) {
                c->states[keep++] = c->states[k];
            } else {
                pc_comp_layer_state_fini(&c->states[k]);
            }
        }
        c->nstates = keep;
    }
    pc_walk_fini(&walk);
    return PC_OK;
}

static pc_status run_update_warnings(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                     pc_error *err)
{
    const char *CMD = "layerComp.updateWarnings";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    warn_ctx w = {.has_only = false, .only = 0};
    yyjson_val *cv = pc_json_get(p, "comp");
    if (cv != NULL && !yyjson_is_null(cv)) {
        pc_status rc = comp_param(doc, p, CMD, &w.only, err);
        if (rc != PC_OK) {
            return rc;
        }
        w.has_only = true;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    size_t count = 0;
    for (size_t i = 0; i < doc->nlayer_comps; i++) {
        const pc_layer_comp *c = &doc->layer_comps[i];
        if (w.has_only && c->id != w.only) {
            continue;
        }
        pc_layer_id *ml = NULL;
        size_t nml = 0;
        if (pc_layer_comp_missing_layers(c, doc, &ml, &nml, NULL) != PC_OK) {
            continue;
        }
        if (nml > 0) {
            yyjson_mut_val *o = yyjson_mut_obj(rd);
            yyjson_mut_obj_add_uint(rd, o, "comp", c->id);
            yyjson_mut_obj_add_strcpy(rd, o, "name", pc_str_c(&c->name));
            yyjson_mut_val *mla = yyjson_mut_arr(rd);
            for (size_t k = 0; k < nml; k++) {
                yyjson_mut_arr_add_uint(rd, mla, ml[k]);
            }
            yyjson_mut_obj_add_val(rd, o, "missingLayers", mla);
            yyjson_mut_arr_append(arr, o);
            count++;
        }
        free(ml);
    }
    yyjson_mut_obj_add_val(rd, root, "warnings", arr);
    yyjson_mut_obj_add_uint(rd, root, "count", count);
    yyjson_mut_doc_set_root(rd, root);

    if (flag(p, "clear", false) && count > 0) {
        const char *label = w.has_only ? "Clear Layer Comp Warning" : "Clear All Layer Comp Warnings";
        pc_status rc = pc_engine_session_edit(s, label, edit_clear_warnings, &w, err);
        if (rc != PC_OK) {
            pc_json_value_fini(out);
            return rc;
        }
    }
    return PC_OK;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < doc->nlayer_comps; i++) {
        const pc_layer_comp *c = &doc->layer_comps[i];
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, o, "id", c->id);
        yyjson_mut_obj_add_strcpy(rd, o, "name", pc_str_c(&c->name));
        yyjson_mut_obj_add_strcpy(rd, o, "comment", pc_str_c(&c->comment));
        yyjson_mut_obj_add_bool(rd, o, "visibility", c->apply_visibility);
        yyjson_mut_obj_add_bool(rd, o, "position", c->apply_position);
        yyjson_mut_obj_add_bool(rd, o, "appearance", c->apply_appearance);
        yyjson_mut_obj_add_uint(rd, o, "layers", c->nstates);
        pc_layer_id *ml = NULL;
        size_t nml = 0;
        if (pc_layer_comp_missing_layers(c, doc, &ml, &nml, NULL) == PC_OK) {
            free(ml);
        } else {
            nml = 0;
        }
        yyjson_mut_obj_add_uint(rd, o, "missingLayers", nml);
        yyjson_mut_arr_append(arr, o);
    }
    yyjson_mut_obj_add_val(rd, root, "comps", arr);
    if (doc->has_last_applied_comp) {
        yyjson_mut_obj_add_uint(rd, root, "lastApplied", doc->last_applied_comp);
    } else {
        yyjson_mut_obj_add_null(rd, root, "lastApplied");
    }
    yyjson_mut_obj_add_bool(rd, root, "hasLastDocumentState", doc->has_last_document_state);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"layerComp.new", "New Layer Comp\xE2\x80\xA6", NULL, 0, NULL,
     "{\"name\":str=\"Layer Comp N\",\"comment\":str=\"\",\"visibility\":bool=true,\"position\":bool="
     "true,\"appearance\":bool=true} \xE2\x86\x92 {comp}",
     comps_has_doc, run_new, true},
    {"layerComp.update", "Update Layer Comp", NULL, 0, NULL,
     "{\"comp\":id|name|\"*\"? (default: last applied; \"*\" = all),\"what\":\"all|visibility|position|"
     "appearance\"=\"all\"}",
     comps_has_comps, run_update, true},
    {"layerComp.apply", "Apply Layer Comp", NULL, 0, NULL,
     "{\"comp\":id|name? (default: last applied)} \xE2\x86\x92 {comp, missingLayers}", comps_has_comps,
     run_apply, true},
    {"layerComp.previous", "Apply Previous Layer Comp", NULL, 0, NULL, "{}", comps_has_comps, run_prev,
     true},
    {"layerComp.next", "Apply Next Layer Comp", NULL, 0, NULL, "{}", comps_has_comps, run_next, true},
    {"layerComp.delete", "Delete Layer Comp", NULL, 0, NULL, "{\"comp\":id|name?}", comps_has_comps,
     run_delete, true},
    {"layerComp.duplicate", "Duplicate Layer Comp", NULL, 0, NULL, "{\"comp\":id|name?} \xE2\x86\x92 {comp}",
     comps_has_comps, run_duplicate, true},
    {"layerComp.rename", "Rename Layer Comp", NULL, 0, NULL, "{\"comp\":id|name?,\"name\":str}",
     comps_has_comps, run_rename, true},
    {"layerComp.setComment", "Layer Comp Comment", NULL, 0, NULL,
     "{\"comp\":id|name?,\"comment\":str}", comps_has_comps, run_set_comment, true},
    {"layerComp.setOptions", "Layer Comp Options\xE2\x80\xA6", NULL, 0, NULL,
     "{\"comp\":id|name?,\"visibility\":bool?,\"position\":bool?,\"appearance\":bool?,\"name\":str?,"
     "\"comment\":str?}",
     comps_has_comps, run_set_options, true},
    {"layerComp.restoreLastDocumentState", "Restore Last Document State", NULL, 0, NULL, "{}",
     comps_has_last_state, run_restore_last, true},
    {"layerComp.updateWarnings", "Layer Comp Warnings", NULL, 0, NULL,
     "{\"comp\":id|name? (default: every comp; limits the report and the clear to that comp),\"clear\":"
     "bool=false (drop states of deleted layers)} \xE2\x86\x92 {warnings:[{comp,name,missingLayers}],count}",
     comps_has_doc, run_update_warnings, true},
    {"layerComp.list", "List Layer Comps", NULL, 0, NULL,
     "{} \xE2\x86\x92 {comps:[{id,name,comment,visibility,position,appearance,layers,missingLayers}],"
     "lastApplied,hasLastDocumentState}",
     comps_has_doc, run_list, false},
};

const pc_engine_command *pc_engine_comps_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
