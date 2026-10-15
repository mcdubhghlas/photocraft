/* Edit > Check Spelling and glyph insertion (port of crates/engine/src/type_spell_cmds.rs, E11).
 *
 * edit.checkSpelling checks type layers against the bundled English dictionary (src/text/spell)
 * plus the user dictionary in the preferences (userDictionary); it lists misspellings, changes
 * one or all, or adds/removes dictionary words. type.insertText inserts a glyph string. Offsets
 * are character indices. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "base/json.h"
#include "base/str.h"
#include "text/spell.h"

#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */
#include "type_cmds.h"  /* norm_text, replace_text, check_text_unlocked, byte/char helpers */

const pc_engine_command *pc_engine_type_spell_specs(size_t *n);

static pc_status bad(pc_error *err, const char *cmd, const char *msg)
{
    return pc_engine_err_bad_params(err, cmd, "%s", msg);
}

/* ---- user dictionary (preferences) -------------------------------------------------------- */

static yyjson_mut_val *user_dict_arr(pc_engine_session *s)
{
    if (s->prefs.values == NULL) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(s->prefs.values);
    return root != NULL ? yyjson_mut_obj_get(root, "userDictionary") : NULL;
}

/* Owned lowercased copies of the user-dictionary words, appended to `out`. */
static pc_status read_user_dict(pc_engine_session *s, pc_spell_list *out, pc_error *err)
{
    yyjson_mut_val *arr = user_dict_arr(s);
    if (arr == NULL) {
        return PC_OK;
    }
    size_t idx, max;
    yyjson_mut_val *v;
    yyjson_mut_arr_foreach(arr, idx, max, v) {
        const char *w = yyjson_mut_get_str(v);
        if (w == NULL) {
            continue;
        }
        char *lw = strdup(w);
        if (lw == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "spell");
        }
        for (char *c = lw; *c; c++) {
            *c = (char)tolower((unsigned char)*c);
        }
        char **nv = realloc(out->v, (out->n + 1) * sizeof *nv);
        if (nv == NULL) {
            free(lw);
            return pc_fail(err, PC_ERR_NOMEM, "spell");
        }
        out->v = nv;
        out->v[out->n++] = lw;
    }
    return PC_OK;
}

/* The accepted set: user dictionary + `ignore` words, all lowercased (owned). */
static pc_status accepted(pc_engine_session *s, yyjson_val *p, pc_spell_list *out, pc_error *err)
{
    if (read_user_dict(s, out, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    yyjson_val *ig = pc_json_get(p, "ignore");
    if (ig != NULL && yyjson_is_arr(ig)) {
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(ig, idx, max, v) {
            size_t wl = 0;
            const char *w = pc_json_as_str(v, &wl);
            if (w == NULL) {
                continue;
            }
            char *lw = malloc(wl + 1);
            if (lw == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "spell");
            }
            for (size_t i = 0; i < wl; i++) {
                lw[i] = (char)tolower((unsigned char)w[i]);
            }
            lw[wl] = '\0';
            char **nv = realloc(out->v, (out->n + 1) * sizeof *nv);
            if (nv == NULL) {
                free(lw);
                return pc_fail(err, PC_ERR_NOMEM, "spell");
            }
            out->v = nv;
            out->v[out->n++] = lw;
        }
    }
    return PC_OK;
}

static void spell_user_of(const pc_spell_list *l, pc_spell_user *u, const char ***scratch)
{
    const char **words = l->n > 0 ? malloc(l->n * sizeof *words) : NULL;
    for (size_t i = 0; i < l->n; i++) {
        words[i] = l->v[i];
    }
    u->words = words;
    u->n = words != NULL ? l->n : 0;
    *scratch = words;
}

/* ---- layer selection ---------------------------------------------------------------------- */

static bool is_text(const pc_document *doc, pc_layer_id id, const char **text, size_t *len)
{
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
        return false;
    }
    if (text != NULL) {
        *text = pc_str_c(&l->content.u.text.text);
        *len = l->content.u.text.text.len;
    }
    return true;
}

/* Type layers top to bottom (Layers-panel order), skipping fully locked ones. */
static pc_status type_layers(const pc_document *doc, pc_layer_id **out, size_t *n, pc_error *err)
{
    pc_walk w = {0};
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_id *ids = w.len > 0 ? malloc(w.len * sizeof *ids) : NULL;
    size_t m = 0;
    for (size_t ri = w.len; ri-- > 0;) {
        const pc_walk_item *it = &w.items[ri];
        if (it->layer->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        const size_t *path = &w.paths[it->path_start];
        if (pc_document_locks_at(doc, path, it->depth + 1).all) {
            continue;
        }
        ids[m++] = it->layer->id;
    }
    pc_walk_fini(&w);
    *out = ids;
    *n = m;
    return PC_OK;
}

static pc_status en_has_type(const pc_engine_session *s, pc_error *r)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_other(r, "no document open");
    }
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (type_layers(pc_doc_arc_get(st->doc), &ids, &n, NULL) != PC_OK) {
        return pc_engine_err_other(r, "the document has no type layers");
    }
    free(ids);
    return n > 0 ? PC_OK : pc_engine_err_other(r, "the document has no type layers");
}

static pc_status en_has_text_layer(const pc_engine_session *s, pc_error *r)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_other(r, "no document open");
    }
    if (st->has_active_layer && is_text(pc_doc_arc_get(st->doc), st->active_layer, NULL, NULL)) {
        return PC_OK;
    }
    return pc_engine_err_other(r, "the active layer is not a type layer");
}

/* The layers to check: active only (allLayers:false or a layer given), else all. *out owned. */
static pc_status layers_param(const pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *n, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        *out = NULL;
        *n = 0;
        return PC_OK;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_layer_id *all = NULL;
    size_t nall = 0;
    if (type_layers(doc, &all, &nall, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    bool all_layers = true;
    bool b;
    if (pc_json_as_bool(pc_json_get(p, "allLayers"), &b) && !b) {
        all_layers = false;
    }
    if (!all_layers || pc_json_get(p, "layer") != NULL) {
        pc_layer_id active;
        bool have = pc_engine_layer_param(s, p, &active, NULL) == PC_OK;
        pc_layer_id *filtered = nall > 0 ? malloc(nall * sizeof *filtered) : NULL;
        size_t m = 0;
        for (size_t i = 0; i < nall; i++) {
            if (have && all[i] == active) {
                filtered[m++] = all[i];
            }
        }
        free(all);
        *out = filtered;
        *n = m;
        return PC_OK;
    }
    *out = all;
    *n = nall;
    return PC_OK;
}

/* ---- actions ------------------------------------------------------------------------------ */

static pc_status spell_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_spell_dict *dict = pc_spell_english();
    pc_spell_list okwords = {0};
    if (accepted(s, p, &okwords, err) != PC_OK) {
        pc_spell_list_fini(&okwords);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_spell_user user;
    const char **scratch = NULL;
    spell_user_of(&okwords, &user, &scratch);
    uint64_t maxu;
    size_t maxs = pc_json_as_u64(pc_json_get(p, "suggestions"), &maxu) ? (size_t)maxu : 5;

    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (layers_param(s, p, &ids, &nids, err) != PC_OK) {
        free(scratch);
        pc_spell_list_fini(&okwords);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(ids);
        free(scratch);
        pc_spell_list_fini(&okwords);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    size_t count = 0;
    for (size_t i = 0; i < nids; i++) {
        const char *text = NULL;
        size_t tlen = 0;
        if (!is_text(doc, ids[i], &text, &tlen)) {
            continue;
        }
        pc_spell_span *spans = NULL;
        size_t ns = 0;
        if (pc_spell_misspellings(dict, text, tlen, &user, &spans, &ns, NULL) != PC_OK) {
            continue;
        }
        for (size_t k = 0; k < ns; k++) {
            yyjson_mut_val *o = yyjson_mut_obj(rd);
            yyjson_mut_obj_add_uint(rd, o, "layer", ids[i]);
            yyjson_mut_obj_add_uint(rd, o, "start", pc_engine_text_char_at(text, tlen, spans[k].start));
            yyjson_mut_obj_add_uint(rd, o, "end", pc_engine_text_char_at(text, tlen, spans[k].end));
            yyjson_mut_obj_add_strncpy(rd, o, "word", text + spans[k].start, spans[k].end - spans[k].start);
            pc_spell_list sug = {0};
            yyjson_mut_val *sa = yyjson_mut_arr(rd);
            if (pc_spell_suggest(dict, text + spans[k].start, spans[k].end - spans[k].start, maxs, &sug, NULL) ==
                PC_OK) {
                for (size_t j = 0; j < sug.n; j++) {
                    yyjson_mut_arr_add_strcpy(rd, sa, sug.v[j]);
                }
            }
            pc_spell_list_fini(&sug);
            yyjson_mut_obj_add_val(rd, o, "suggestions", sa);
            yyjson_mut_arr_append(arr, o);
            count++;
        }
        free(spans);
    }
    yyjson_mut_obj_add_val(rd, root, "misspellings", arr);
    yyjson_mut_obj_add_uint(rd, root, "count", count);
    yyjson_mut_doc_set_root(rd, root);
    free(ids);
    free(scratch);
    pc_spell_list_fini(&okwords);
    return PC_OK;
}

/* Replace one span of one layer. */
typedef struct {
    pc_layer_id id;
    size_t a, b;
    const char *to;
    size_t tolen;
} change_ctx;

static pc_status edit_change_one(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    change_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l != NULL && l->content.kind == PC_LAYER_TEXT) {
        pc_text_layer *t = &l->content.u.text;
        pc_engine_replace_text(t, c->a, c->b, c->to, c->tolen);
        pc_engine_type_refresh(doc, t);
    }
    return PC_OK;
}

static pc_status spell_change(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "edit.checkSpelling";
    uint64_t idn;
    if (!pc_json_as_u64(pc_json_get(p, "layer"), &idn)) {
        return bad(err, CMD, "`change` needs `layer`");
    }
    uint64_t start, end;
    if (!pc_json_as_u64(pc_json_get(p, "start"), &start)) {
        return bad(err, CMD, "`change` needs `start`");
    }
    if (!pc_json_as_u64(pc_json_get(p, "end"), &end)) {
        return bad(err, CMD, "`change` needs `end`");
    }
    size_t tolen = 0;
    const char *to = pc_json_as_str(pc_json_get(p, "replace"), &tolen);
    if (to == NULL) {
        return bad(err, CMD, "`change` needs `replace`");
    }
    pc_layer_id id = (pc_layer_id)idn;
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const char *text = NULL;
    size_t tlen = 0;
    if (!is_text(doc, id, &text, &tlen)) {
        return pc_engine_err_bad_params(err, CMD, "layer %llu is not a type layer", (unsigned long long)id);
    }
    if (pc_engine_check_text_unlocked(doc, id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    size_t a = pc_engine_text_byte_at(text, tlen, (size_t)start);
    size_t endc = end > start ? (size_t)end : (size_t)start;
    size_t b = pc_engine_text_byte_at(text, tlen, endc);
    size_t wl = 0;
    const char *word = pc_json_as_str(pc_json_get(p, "word"), &wl);
    if (word != NULL && (b - a != wl || memcmp(text + a, word, wl) != 0)) {
        return pc_engine_err_bad_params(err, CMD, "the text at %llu..%llu is \"%.*s\", not \"%.*s\"",
                                        (unsigned long long)start, (unsigned long long)end, (int)(b - a), text + a,
                                        (int)wl, word);
    }
    change_ctx c = {id, a, b, to, tolen};
    if (pc_engine_session_edit(s, "Check Spelling", edit_change_one, &c, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    size_t caret = (size_t)start + pc_engine_text_char_at(to, tolen, tolen);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "changed", 1);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_obj_add_uint(rd, root, "start", start);
    yyjson_mut_obj_add_uint(rd, root, "end", caret);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

typedef struct {
    pc_layer_id id;
    size_t *spans; /* 2*count: a,b pairs (byte), descending by start for safe reverse apply */
    size_t count;
} hits;

typedef struct {
    hits *plan;
    size_t nplan;
    const char *to;
    size_t tolen;
} change_all_ctx;

static pc_status edit_change_all(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    change_all_ctx *c = vctx;
    for (size_t i = 0; i < c->nplan; i++) {
        pc_layer *l = pc_document_layer_mut(doc, c->plan[i].id);
        if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        pc_text_layer *t = &l->content.u.text;
        for (size_t k = c->plan[i].count; k-- > 0;) {
            pc_engine_replace_text(t, c->plan[i].spans[2 * k], c->plan[i].spans[2 * k + 1], c->to, c->tolen);
        }
        pc_engine_type_refresh(doc, t);
    }
    return PC_OK;
}

static pc_status spell_change_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "edit.checkSpelling";
    size_t wl = 0;
    const char *word = pc_json_as_str(pc_json_get(p, "word"), &wl);
    if (word == NULL || wl == 0) {
        return bad(err, CMD, "`changeAll` needs `word`");
    }
    size_t tolen = 0;
    const char *to = pc_json_as_str(pc_json_get(p, "replace"), &tolen);
    if (to == NULL) {
        return bad(err, CMD, "`changeAll` needs `replace`");
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (layers_param(s, p, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    hits *plan = nids > 0 ? calloc(nids, sizeof *plan) : NULL;
    size_t nplan = 0, count = 0;
    for (size_t i = 0; i < nids; i++) {
        const char *text = NULL;
        size_t tlen = 0;
        if (!is_text(doc, ids[i], &text, &tlen)) {
            continue;
        }
        pc_spell_span *words = NULL;
        size_t nw = 0;
        if (pc_spell_words(text, tlen, &words, &nw, NULL) != PC_OK) {
            continue;
        }
        size_t *hb = NULL;
        size_t hc = 0;
        for (size_t k = 0; k < nw; k++) {
            size_t len = words[k].end - words[k].start;
            if (len == wl && memcmp(text + words[k].start, word, wl) == 0) {
                size_t *nh = realloc(hb, (hc + 1) * 2 * sizeof *nh);
                if (nh == NULL) {
                    break;
                }
                hb = nh;
                hb[2 * hc] = words[k].start;
                hb[2 * hc + 1] = words[k].end;
                hc++;
            }
        }
        free(words);
        if (hc > 0) {
            plan[nplan].id = ids[i];
            plan[nplan].spans = hb;
            plan[nplan].count = hc;
            nplan++;
            count += hc;
        } else {
            free(hb);
        }
    }
    free(ids);
    if (count > 0) {
        change_all_ctx c = {plan, nplan, to, tolen};
        pc_status rc = pc_engine_session_edit(s, "Check Spelling", edit_change_all, &c, err);
        if (rc != PC_OK) {
            for (size_t i = 0; i < nplan; i++) {
                free(plan[i].spans);
            }
            free(plan);
            return rc;
        }
    }
    for (size_t i = 0; i < nplan; i++) {
        free(plan[i].spans);
    }
    free(plan);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "changed", count);
    yyjson_mut_obj_add_uint(rd, root, "layers", nplan);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* Rebuilds the userDictionary array from a sorted (bytewise lowercase) owned list. */
static int cmp_lower(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    while (*x && *y) {
        int cx = tolower((unsigned char)*x), cy = tolower((unsigned char)*y);
        if (cx != cy) {
            return cx - cy;
        }
        x++;
        y++;
    }
    return (int)((unsigned char)tolower((unsigned char)*x)) - (int)((unsigned char)tolower((unsigned char)*y));
}

static pc_status dict_result(pc_engine_session *s, pc_json_value *out, const char *key, bool flag, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, key, flag);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    yyjson_mut_val *src = user_dict_arr(s);
    if (src != NULL) {
        size_t idx, max;
        yyjson_mut_val *v;
        yyjson_mut_arr_foreach(src, idx, max, v) {
            const char *w = yyjson_mut_get_str(v);
            if (w != NULL) {
                yyjson_mut_arr_add_strcpy(rd, arr, w);
            }
        }
    }
    yyjson_mut_obj_add_val(rd, root, "userDictionary", arr);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status spell_add(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t wl = 0;
    const char *w = pc_json_as_str(pc_json_get(p, "word"), &wl);
    /* trim + reject empty / containing whitespace */
    size_t a = 0, b = wl;
    while (a < b && isspace((unsigned char)w[a])) a++;
    while (b > a && isspace((unsigned char)w[b - 1])) b--;
    bool ok = w != NULL && b > a;
    for (size_t i = a; ok && i < b; i++) {
        if (isspace((unsigned char)w[i])) {
            ok = false;
        }
    }
    if (!ok) {
        return bad(err, "edit.checkSpelling", "`add` needs a single `word`");
    }
    char *word = malloc(b - a + 1);
    if (word == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "spell");
    }
    memcpy(word, w + a, b - a);
    word[b - a] = '\0';
    yyjson_mut_val *arr = user_dict_arr(s);
    bool added = false;
    if (arr != NULL) {
        bool dup = false;
        size_t idx, max;
        yyjson_mut_val *v;
        yyjson_mut_arr_foreach(arr, idx, max, v) {
            const char *e = yyjson_mut_get_str(v);
            if (e != NULL && strcasecmp(e, word) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            /* collect, add, sort, rewrite */
            size_t n = yyjson_mut_arr_size(arr);
            char **words = malloc((n + 1) * sizeof *words);
            size_t m = 0;
            if (words != NULL) {
                yyjson_mut_arr_foreach(arr, idx, max, v) {
                    const char *e = yyjson_mut_get_str(v);
                    words[m++] = e ? strdup(e) : strdup("");
                }
                words[m++] = strdup(word);
                qsort(words, m, sizeof *words, cmp_lower);
                yyjson_mut_arr_clear(arr);
                for (size_t i = 0; i < m; i++) {
                    if (words[i] != NULL) {
                        yyjson_mut_arr_add_strcpy(s->prefs.values, arr, words[i]);
                    }
                    free(words[i]);
                }
                free(words);
                s->prefs.rev++;
                added = true;
            }
        }
    }
    pc_status rc = dict_result(s, out, "added", added, err);
    free(word);
    return rc;
}

static pc_status spell_remove(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t wl = 0;
    const char *w = pc_json_as_str(pc_json_get(p, "word"), &wl);
    if (w == NULL) {
        w = "";
        wl = 0;
    }
    char *word = malloc(wl + 1);
    if (word == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "spell");
    }
    memcpy(word, w, wl);
    word[wl] = '\0';
    yyjson_mut_val *arr = user_dict_arr(s);
    bool removed = false;
    if (arr != NULL) {
        size_t n = yyjson_mut_arr_size(arr);
        char **keep = n > 0 ? malloc(n * sizeof *keep) : NULL;
        size_t m = 0;
        size_t idx, max;
        yyjson_mut_val *v;
        yyjson_mut_arr_foreach(arr, idx, max, v) {
            const char *e = yyjson_mut_get_str(v);
            if (e != NULL && strcasecmp(e, word) == 0) {
                removed = true;
            } else if (e != NULL) {
                keep[m++] = strdup(e);
            }
        }
        if (removed) {
            yyjson_mut_arr_clear(arr);
            for (size_t i = 0; i < m; i++) {
                if (keep[i] != NULL) {
                    yyjson_mut_arr_add_strcpy(s->prefs.values, arr, keep[i]);
                }
            }
            s->prefs.rev++;
        }
        for (size_t i = 0; i < m; i++) {
            free(keep[i]);
        }
        free(keep);
    }
    pc_status rc = dict_result(s, out, "removed", removed, err);
    free(word);
    return rc;
}

static pc_status spell_suggest(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t wl = 0;
    const char *w = pc_json_as_str(pc_json_get(p, "word"), &wl);
    if (w == NULL) {
        w = "";
        wl = 0;
    }
    pc_spell_list okwords = {0};
    if (accepted(s, p, &okwords, err) != PC_OK) {
        pc_spell_list_fini(&okwords);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_spell_user user;
    const char **scratch = NULL;
    spell_user_of(&okwords, &user, &scratch);
    const pc_spell_dict *d = pc_spell_english();
    bool correct = pc_spell_check(d, w, wl, &user);
    uint64_t maxu;
    size_t maxs = pc_json_as_u64(pc_json_get(p, "suggestions"), &maxu) ? (size_t)maxu : 5;
    pc_spell_list sug = {0};
    (void)pc_spell_suggest(d, w, wl, maxs, &sug, NULL);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_spell_list_fini(&sug);
        free(scratch);
        pc_spell_list_fini(&okwords);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strncpy(rd, root, "word", w, wl);
    yyjson_mut_obj_add_bool(rd, root, "correct", correct);
    yyjson_mut_val *sa = yyjson_mut_arr(rd);
    for (size_t i = 0; i < sug.n; i++) {
        yyjson_mut_arr_add_strcpy(rd, sa, sug.v[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "suggestions", sa);
    yyjson_mut_doc_set_root(rd, root);
    pc_spell_list_fini(&sug);
    free(scratch);
    pc_spell_list_fini(&okwords);
    return PC_OK;
}

static pc_status run_check_spelling(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t al = 0;
    const char *action = pc_json_as_str(pc_json_get(p, "action"), &al);
    if (action == NULL) {
        action = "list";
        al = 4;
    }
    if ((al == 4 && memcmp(action, "list", 4) == 0) || (al == 5 && memcmp(action, "check", 5) == 0)) {
        return spell_list(s, p, out, err);
    }
    if (al == 6 && memcmp(action, "change", 6) == 0) {
        return spell_change(s, p, out, err);
    }
    if (al == 9 && memcmp(action, "changeAll", 9) == 0) {
        return spell_change_all(s, p, out, err);
    }
    if ((al == 3 && memcmp(action, "add", 3) == 0) || (al == 15 && memcmp(action, "addToDictionary", 15) == 0)) {
        return spell_add(s, p, out, err);
    }
    if (al == 20 && memcmp(action, "removeFromDictionary", 20) == 0) {
        return spell_remove(s, p, out, err);
    }
    if (al == 7 && memcmp(action, "suggest", 7) == 0) {
        return spell_suggest(s, p, out, err);
    }
    return pc_engine_err_bad_params(err, "edit.checkSpelling", "unknown action \"%.*s\"", (int)al, action);
}

/* ---- type.insertText ---------------------------------------------------------------------- */

typedef struct {
    pc_layer_id id;
    size_t a, b;
    char *norm;
    size_t nlen;
} insert_ctx;

static pc_status edit_insert_text2(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    insert_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l != NULL && l->content.kind == PC_LAYER_TEXT) {
        pc_text_layer *t = &l->content.u.text;
        pc_engine_replace_text(t, c->a, c->b, c->norm, c->nlen);
        pc_engine_type_refresh(doc, t);
    }
    return PC_OK;
}

static pc_status run_insert_text(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "type.insertText";
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    size_t nl = 0;
    const char *newt = pc_json_as_str(pc_json_get(p, "text"), &nl);
    if (newt == NULL || nl == 0) {
        return bad(err, CMD, "missing `text`");
    }
    size_t normlen = 0;
    char *norm = pc_engine_norm_text(newt, nl, &normlen);
    if (norm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        free(norm);
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const char *text = NULL;
    size_t tlen = 0;
    if (!is_text(doc, id, &text, &tlen)) {
        free(norm);
        return pc_engine_err_bad_params(err, CMD, "layer %llu is not a type layer", (unsigned long long)id);
    }
    if (pc_engine_check_text_unlocked(doc, id, err) != PC_OK) {
        free(norm);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    size_t n = pc_engine_text_char_at(text, tlen, tlen);
    size_t ca, cb;
    yyjson_val *range = pc_json_get(p, "range");
    uint64_t at;
    if (range != NULL && yyjson_is_arr(range) && yyjson_arr_size(range) == 2) {
        uint64_t x = 0, y;
        (void)pc_json_as_u64(yyjson_arr_get(range, 0), &x);
        if (!pc_json_as_u64(yyjson_arr_get(range, 1), &y)) {
            y = x;
        }
        size_t lo = (size_t)(x < y ? x : y), hi = (size_t)(x < y ? y : x);
        ca = lo < n ? lo : n;
        cb = hi < n ? hi : n;
    } else if (pc_json_as_u64(pc_json_get(p, "at"), &at)) {
        ca = (size_t)at < n ? (size_t)at : n;
        cb = ca;
    } else {
        ca = n;
        cb = n;
    }
    size_t la = 0;
    const char *lbl = pc_json_as_str(pc_json_get(p, "label"), &la);
    char label[128];
    if (lbl != NULL) {
        size_t w = la < sizeof label - 1 ? la : sizeof label - 1;
        memcpy(label, lbl, w);
        label[w] = '\0';
    } else {
        memcpy(label, "Insert Glyph", 13);
    }
    insert_ctx c = {id, pc_engine_text_byte_at(text, tlen, ca), pc_engine_text_byte_at(text, tlen, cb), norm, normlen};
    pc_status rc = pc_engine_session_edit(s, label, edit_insert_text2, &c, err);
    size_t caret = ca + pc_engine_text_char_at(norm, normlen, normlen);
    free(norm);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_obj_add_uint(rd, root, "caret", caret);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const char *const MENU_EDIT[] = {"Edit"};

static const pc_engine_command SPECS[] = {
    {"edit.checkSpelling", "Check Spelling\xe2\x80\xa6", MENU_EDIT, 1, NULL,
     "{\"action\":\"list|change|changeAll|addToDictionary|removeFromDictionary|suggest\"=\"list\",\"allLayers\":"
     "bool=true,\"layer\":id? (check one layer),\"ignore\":[word]? (Ignore All for this "
     "check),\"suggestions\":n=5; change: \"layer\",\"start\",\"end\" (chars),\"word\"? "
     "(verified),\"replace\"; changeAll: \"word\",\"replace\"; "
     "addToDictionary/removeFromDictionary/suggest: \"word\"} \xe2\x86\x92 list: "
     "{\"misspellings\":[{\"layer\",\"start\",\"end\",\"word\",\"suggestions\"}],\"count\"}",
     en_has_type, run_check_spelling, true},
    {"type.insertText", "Insert Glyph", NULL, 0, NULL,
     "{\"layer\":id?,\"text\":str,\"at\":char? (default: end of text),\"range\":[startChar,endChar]? "
     "(replaced),\"label\":str?} \xe2\x86\x92 {\"layer\",\"caret\"}",
     en_has_text_layer, run_insert_text, true},
};

const pc_engine_command *pc_engine_type_spell_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
