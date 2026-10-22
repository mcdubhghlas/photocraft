/* Organising brush presets: the Brushes panel's drag-and-drop and context menus (port of
 * crates/engine/src/brush_preset_cmds.rs):
 *
 *   - brush.presets.rename        rename a preset
 *   - brush.presets.move          move a preset within/between groups and nested folders
 *   - brush.presets.moveGroup     move a group or folder (optionally `into` another folder)
 *   - brush.presets.renameGroup   rename a group or nested folder
 *   - brush.presets.deleteGroup   delete a group or nested folder
 *
 * Groups are the panel's top-level folders (pc_paint_preset.group); a group can hold nested folders
 * (pc_paint_preset.folder, a path inside the group). The library order is the panel order. Every
 * change bumps presets_rev (pc_engine_brush_presets_changed) so the store syncs. A built-in that is
 * renamed or moved becomes the user's preset. Never crashes on hostile params (rule 9). */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "base/str.h"
#include "paint/presets.h"
#include "psd/abr.h" /* PC_PSD_ABR_MAX_FOLDER_DEPTH */

#include "brush_cmds.h" /* pc_engine_brush_presets_changed */
#include "brush_organize_cmds.h"
#include "engine_internal.h"

#define BO_MAX_NAME 255u
#define BO_MAX_FOLDER_DEPTH PC_PSD_ABR_MAX_FOLDER_DEPTH

/* ---- a parsed folder path param (Vec<String>) --------------------------------------------- */

typedef struct strvec {
    char **v; /* each owned, NUL-terminated */
    size_t n;
} strvec;

static void strvec_fini(strvec *s)
{
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->n; i++) {
        free(s->v[i]);
    }
    free(s->v);
    s->v = NULL;
    s->n = 0;
}

static int bo_casecmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* UTF-8 code-point count (Rust chars().count()). */
static size_t char_count(const char *s, size_t bytes)
{
    size_t n = 0;
    for (size_t i = 0; i < bytes; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

/* Trim ASCII whitespace; returns pointer and length into the original buffer. */
static const char *trim(const char *s, size_t len, size_t *out_len)
{
    while (len > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r')) {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) {
        len--;
    }
    *out_len = len;
    return s;
}

static char *dup_n(const char *s, size_t len)
{
    char *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/* A required, trimmed, non-empty, length-bounded string param. */
static pc_status bo_text(yyjson_val *p, const char *k, const char *cmd, char **out, pc_error *err)
{
    *out = NULL;
    yyjson_val *v = pc_json_get(p, k);
    size_t len = 0;
    const char *s = v != NULL ? pc_json_as_str(v, &len) : NULL;
    if (s == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `%s`", k);
    }
    size_t tlen = 0;
    const char *t = trim(s, len, &tlen);
    if (tlen == 0) {
        return pc_engine_err_bad_params(err, cmd, "missing `%s`", k);
    }
    if (char_count(t, tlen) > BO_MAX_NAME) {
        return pc_engine_err_bad_params(err, cmd, "`%s` is longer than %u characters", k, BO_MAX_NAME);
    }
    char *d = dup_n(t, tlen);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "name");
    }
    *out = d;
    return PC_OK;
}

/* A group-name param (may be empty = the ungrouped presets); *present false when absent/null. */
static pc_status bo_group_param(yyjson_val *p, const char *k, const char *cmd, bool *present, char **out,
                                pc_error *err)
{
    *present = false;
    *out = NULL;
    yyjson_val *v = pc_json_get(p, k);
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    size_t len = 0;
    const char *s = pc_json_as_str(v, &len);
    if (s == NULL || char_count(s, len) > BO_MAX_NAME) {
        return pc_engine_err_bad_params(err, cmd,
                                        "`%s` must be a group name (string, at most %u characters)", k,
                                        BO_MAX_NAME);
    }
    size_t tlen = 0;
    const char *t = trim(s, len, &tlen);
    char *d = dup_n(t, tlen);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "group");
    }
    *present = true;
    *out = d;
    return PC_OK;
}

/* A folder-path param ["Outer","Inner"]; *present false when absent/null. */
static pc_status bo_folder_param(yyjson_val *obj, const char *cmd, bool *present, strvec *out, pc_error *err)
{
    *present = false;
    out->v = NULL;
    out->n = 0;
    yyjson_val *v = pc_json_get(obj, "folder");
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    const char *msg = "`folder` must be a list of folder names (non-empty strings)";
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) > BO_MAX_FOLDER_DEPTH) {
        return pc_engine_err_bad_params(err, cmd, "%s", msg);
    }
    size_t count = yyjson_arr_size(v);
    out->v = calloc(count ? count : 1, sizeof(char *));
    if (out->v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "folder");
    }
    size_t i, max;
    yyjson_val *e;
    yyjson_arr_foreach(v, i, max, e) {
        size_t len = 0;
        const char *s = e != NULL ? pc_json_as_str(e, &len) : NULL;
        if (s == NULL) {
            strvec_fini(out);
            return pc_engine_err_bad_params(err, cmd, "%s", msg);
        }
        size_t tlen = 0;
        const char *t = trim(s, len, &tlen);
        if (tlen == 0 || char_count(t, tlen) > BO_MAX_NAME) {
            strvec_fini(out);
            return pc_engine_err_bad_params(err, cmd, "%s", msg);
        }
        char *d = dup_n(t, tlen);
        if (d == NULL) {
            strvec_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "folder");
        }
        out->v[out->n++] = d;
    }
    *present = true;
    return PC_OK;
}

/* ---- library predicates ------------------------------------------------------------------- */

static bool folder_starts_with(const pc_paint_str_list *f, const strvec *prefix)
{
    if (f->len < prefix->n) {
        return false;
    }
    for (size_t i = 0; i < prefix->n; i++) {
        if (!pc_str_eq_c(&f->v[i], prefix->v[i])) {
            return false;
        }
    }
    return true;
}

static bool in_folder(const pc_paint_preset *x, const char *g, const strvec *f)
{
    return pc_str_eq_c(&x->group, g) && folder_starts_with(&x->folder, f);
}

static size_t index_of(const pc_paint_preset *v, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++) {
        if (bo_casecmp(pc_str_c(&v[i].name), name) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static bool group_exists(const pc_paint_preset *v, size_t n, const char *g)
{
    for (size_t i = 0; i < n; i++) {
        if (pc_str_eq_c(&v[i].group, g)) {
            return true;
        }
    }
    return false;
}

/* The subfolders directly inside folder `parent` of group `g`, in panel order (out: owned strvec). */
static pc_status subfolder_order(const pc_paint_preset *v, size_t n, const char *g, const strvec *parent,
                                 strvec *out, pc_error *err)
{
    out->v = NULL;
    out->n = 0;
    size_t cap = 0;
    for (size_t i = 0; i < n; i++) {
        if (!in_folder(&v[i], g, parent)) {
            continue;
        }
        if (v[i].folder.len <= parent->n) {
            continue;
        }
        const pc_str *nm = &v[i].folder.v[parent->n];
        bool have = false;
        for (size_t k = 0; k < out->n; k++) {
            if (pc_str_eq_c(nm, out->v[k])) {
                have = true;
                break;
            }
        }
        if (have) {
            continue;
        }
        if (out->n == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            char **grown = realloc(out->v, ncap * sizeof(char *));
            if (grown == NULL) {
                strvec_fini(out);
                return pc_fail(err, PC_ERR_NOMEM, "subfolders");
            }
            out->v = grown;
            cap = ncap;
        }
        char *d = dup_n(pc_str_c(nm), nm->len);
        if (d == NULL) {
            strvec_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "subfolders");
        }
        out->v[out->n++] = d;
    }
    return PC_OK;
}

static size_t strvec_index(const strvec *s, const char *name)
{
    for (size_t i = 0; i < s->n; i++) {
        if (strcmp(s->v[i], name) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Groups in panel order (first appearance) into *out (owned). */
static pc_status group_order(const pc_paint_preset *v, size_t n, strvec *out, pc_error *err)
{
    out->v = NULL;
    out->n = 0;
    size_t cap = 0;
    for (size_t i = 0; i < n; i++) {
        const char *g = pc_str_c(&v[i].group);
        if (strvec_index(out, g) != SIZE_MAX) {
            continue;
        }
        if (out->n == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            char **grown = realloc(out->v, ncap * sizeof(char *));
            if (grown == NULL) {
                strvec_fini(out);
                return pc_fail(err, PC_ERR_NOMEM, "groups");
            }
            out->v = grown;
            cap = ncap;
        }
        char *d = dup_n(g, v[i].group.len);
        if (d == NULL) {
            strvec_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "groups");
        }
        out->v[out->n++] = d;
    }
    return PC_OK;
}

static size_t position_in_group(const pc_paint_preset *v, size_t n, size_t i)
{
    if (i >= n) {
        return 0;
    }
    size_t count = 0;
    for (size_t k = 0; k < i; k++) {
        if (pc_str_eq(&v[k].group, &v[i].group)) {
            count++;
        }
    }
    return count;
}

/* ---- mutate a preset's placement (the moved ones become the user's) ----------------------- */

static pc_status set_group(pc_paint_preset *x, const char *g, pc_error *err)
{
    return pc_str_set_c(&x->group, g, err);
}

/* Replace x->folder with the strvec `f`. */
static pc_status set_folder(pc_paint_preset *x, const strvec *f, pc_error *err)
{
    pc_str *nv = NULL;
    if (f->n > 0) {
        nv = calloc(f->n, sizeof(pc_str));
        if (nv == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "folder");
        }
        for (size_t i = 0; i < f->n; i++) {
            if (pc_str_set_c(&nv[i], f->v[i], err) != PC_OK) {
                for (size_t k = 0; k <= i; k++) {
                    pc_str_fini(&nv[k]);
                }
                free(nv);
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        }
    }
    pc_paint_str_list_fini(&x->folder);
    x->folder.v = nv;
    x->folder.len = f->n;
    return PC_OK;
}

/* ---- in-place remove + insert (count unchanged) ------------------------------------------- */

static void remove_at(pc_paint_preset *v, size_t *n, size_t i, pc_paint_preset *out)
{
    *out = v[i];
    memmove(&v[i], &v[i + 1], (*n - i - 1) * sizeof(pc_paint_preset));
    (*n)--;
}

static void insert_at(pc_paint_preset *v, size_t *n, size_t pos, pc_paint_preset preset)
{
    if (pos > *n) {
        pos = *n;
    }
    memmove(&v[pos + 1], &v[pos], (*n - pos) * sizeof(pc_paint_preset));
    v[pos] = preset;
    (*n)++;
}

/* Reorder: move the block (indices where pred(x,ctx)) to position `pos` among the rest, both in
 * original order. Count unchanged. */
typedef bool (*bo_pred)(const pc_paint_preset *x, void *ctx);

static pc_status reorder_block(pc_paint_preset *v, size_t n, bo_pred pred, void *ctx, size_t pos,
                               pc_error *err)
{
    pc_paint_preset *scratch = malloc((n ? n : 1) * sizeof(pc_paint_preset));
    if (scratch == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "reorder");
    }
    size_t nrest = 0;
    for (size_t i = 0; i < n; i++) {
        if (!pred(&v[i], ctx)) {
            nrest++;
        }
    }
    if (pos > nrest) {
        pos = nrest;
    }
    size_t w = 0, seen_rest = 0;
    /* rest[0..pos] */
    for (size_t i = 0; i < n && seen_rest < pos; i++) {
        if (!pred(&v[i], ctx)) {
            scratch[w++] = v[i];
            seen_rest++;
        }
    }
    /* block */
    for (size_t i = 0; i < n; i++) {
        if (pred(&v[i], ctx)) {
            scratch[w++] = v[i];
        }
    }
    /* rest[pos..] */
    seen_rest = 0;
    for (size_t i = 0; i < n; i++) {
        if (!pred(&v[i], ctx)) {
            if (seen_rest >= pos) {
                scratch[w++] = v[i];
            }
            seen_rest++;
        }
    }
    memcpy(v, scratch, n * sizeof(pc_paint_preset));
    free(scratch);
    return PC_OK;
}

/* ---- result helpers ----------------------------------------------------------------------- */

static yyjson_mut_val *strvec_json(yyjson_mut_doc *d, const pc_paint_str_list *f)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < f->len; i++) {
        yyjson_mut_arr_add_strcpy(d, arr, pc_str_c(&f->v[i]));
    }
    return arr;
}

static pc_status always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

/* ---- rename ------------------------------------------------------------------------------- */

static pc_status run_rename(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "brush.presets.rename";
    pc_paint_preset *v = s->tools.presets;
    size_t n = s->tools.npresets;
    char *name = NULL, *newname = NULL;
    pc_status rc = bo_text(p, "name", cmd, &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t i = index_of(v, n, name);
    if (i == SIZE_MAX) {
        rc = pc_engine_err_bad_params(err, cmd, "no brush preset named `%s`", name);
        free(name);
        return rc;
    }
    free(name);
    if ((rc = bo_text(p, "newName", cmd, &newname, err)) != PC_OK) {
        return rc;
    }
    size_t j = index_of(v, n, newname);
    if (j != SIZE_MAX && j != i) {
        rc = pc_engine_err_bad_params(err, cmd, "a brush preset named `%s` already exists", newname);
        free(newname);
        return rc;
    }
    if (!pc_str_eq_c(&v[i].name, newname)) {
        bool was_current = s->tools.current_preset != NULL &&
                           bo_casecmp(s->tools.current_preset, pc_str_c(&v[i].name)) == 0;
        if ((rc = pc_str_set_c(&v[i].name, newname, err)) != PC_OK) {
            free(newname);
            return rc;
        }
        v[i].builtin = false;
        if (was_current) {
            free(s->tools.current_preset);
            s->tools.current_preset = dup_n(newname, strlen(newname));
        }
        pc_engine_brush_presets_changed(s);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(newname);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "name", newname);
    yyjson_mut_doc_set_root(rd, root);
    free(newname);
    return PC_OK;
}

/* ---- move a preset ------------------------------------------------------------------------ */

static pc_status opt_index(yyjson_val *p, const char *cmd, bool *present, uint64_t *out, pc_error *err)
{
    *present = false;
    yyjson_val *v = pc_json_get(p, "index");
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    if (!pc_json_as_u64(v, out)) {
        return pc_engine_err_bad_params(err, cmd, "`index` must be a non-negative integer");
    }
    *present = true;
    return PC_OK;
}

static pc_status run_move(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "brush.presets.move";
    pc_paint_preset *v = s->tools.presets;
    size_t n = s->tools.npresets;
    char *name = NULL, *group = NULL, *before_name = NULL;
    strvec folder = {0};
    bool have_group = false, have_folder = false, have_index = false;
    uint64_t index = 0;
    pc_status rc = bo_text(p, "name", cmd, &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t i = index_of(v, n, name);
    free(name);
    if (i == SIZE_MAX) {
        return pc_engine_err_bad_params(err, cmd, "no brush preset named the given `name`");
    }
    /* before (optional). */
    size_t before_i = SIZE_MAX;
    yyjson_val *bv = pc_json_get(p, "before");
    if (bv != NULL && !yyjson_is_null(bv)) {
        char *bn = NULL;
        if ((rc = bo_text(p, "before", cmd, &bn, err)) != PC_OK) {
            return rc;
        }
        before_i = index_of(v, n, bn);
        free(bn);
        if (before_i == SIZE_MAX) {
            return pc_engine_err_bad_params(err, cmd, "no brush preset named `before`");
        }
        if (before_i == i) {
            return pc_engine_err_bad_params(err, cmd, "`before` names the preset being moved");
        }
    }
    if ((rc = opt_index(p, cmd, &have_index, &index, err)) != PC_OK) {
        return rc;
    }
    if ((rc = bo_folder_param(p, cmd, &have_folder, &folder, err)) != PC_OK) {
        return rc;
    }
    if ((rc = bo_group_param(p, "group", cmd, &have_group, &group, err)) != PC_OK) {
        strvec_fini(&folder);
        return rc;
    }

    /* target group. */
    char *target = NULL;
    if (have_group) {
        target = dup_n(group, strlen(group));
    } else if (before_i != SIZE_MAX) {
        target = dup_n(pc_str_c(&v[before_i].group), v[before_i].group.len);
    } else {
        target = dup_n(pc_str_c(&v[i].group), v[i].group.len);
    }
    free(group);
    if (target == NULL) {
        strvec_fini(&folder);
        return pc_fail(err, PC_ERR_NOMEM, "move");
    }

    /* target folder. */
    strvec tfolder = {0};
    bool tfolder_from_param = false;
    if (have_folder) {
        tfolder = folder; /* take ownership */
        folder.v = NULL;
        folder.n = 0;
        tfolder_from_param = true;
    } else if (before_i != SIZE_MAX) {
        /* before's folder */
        const pc_paint_str_list *bf = &v[before_i].folder;
        if (bf->len > 0) {
            tfolder.v = calloc(bf->len, sizeof(char *));
            if (tfolder.v == NULL) {
                free(target);
                return pc_fail(err, PC_ERR_NOMEM, "move");
            }
            for (size_t k = 0; k < bf->len; k++) {
                tfolder.v[tfolder.n++] = dup_n(pc_str_c(&bf->v[k]), bf->v[k].len);
            }
        }
    } else if (pc_str_eq_c(&v[i].group, target)) {
        const pc_paint_str_list *mf = &v[i].folder;
        if (mf->len > 0) {
            tfolder.v = calloc(mf->len, sizeof(char *));
            if (tfolder.v == NULL) {
                free(target);
                return pc_fail(err, PC_ERR_NOMEM, "move");
            }
            for (size_t k = 0; k < mf->len; k++) {
                tfolder.v[tfolder.n++] = dup_n(pc_str_c(&mf->v[k]), mf->v[k].len);
            }
        }
    }
    (void)tfolder_from_param;
    strvec_fini(&folder);

    /* before must be in the target group and folder. */
    if (before_i != SIZE_MAX) {
        if (!pc_str_eq_c(&v[before_i].group, target)) {
            rc = pc_engine_err_bad_params(err, cmd, "`before` is not in the target group");
            goto done_move;
        }
        if (!(v[before_i].folder.len == tfolder.n && folder_starts_with(&v[before_i].folder, &tfolder))) {
            rc = pc_engine_err_bad_params(err, cmd, "`before` is not in that folder");
            goto done_move;
        }
    }

    /* Capture before's name (the array shifts after remove). */
    if (before_i != SIZE_MAX) {
        before_name = dup_n(pc_str_c(&v[before_i].name), v[before_i].name.len);
    }

    /* Remove the moving preset. */
    pc_paint_preset pr;
    remove_at(v, &n, i, &pr);
    s->tools.npresets = n;

    /* Re-place it. */
    bool same = pc_str_eq_c(&pr.group, target) && pr.folder.len == tfolder.n &&
                folder_starts_with(&pr.folder, &tfolder);
    if (!same) {
        if ((rc = set_group(&pr, target, err)) != PC_OK) {
            insert_at(v, &n, i, pr); /* restore */
            s->tools.npresets = n;
            goto done_move;
        }
        if ((rc = set_folder(&pr, &tfolder, err)) != PC_OK) {
            insert_at(v, &n, i, pr);
            s->tools.npresets = n;
            goto done_move;
        }
        pr.builtin = false;
    }

    /* Compute position in the post-remove lib (= v[0..n]). */
    /* members: indices whose group == target. */
    size_t last_in_folder = SIZE_MAX;
    size_t last_member = SIZE_MAX, nmembers = 0;
    for (size_t k = 0; k < n; k++) {
        if (pc_str_eq_c(&v[k].group, target)) {
            last_member = k;
            nmembers++;
            if (in_folder(&v[k], target, &tfolder)) {
                last_in_folder = k;
            }
        }
    }
    size_t end;
    if (last_in_folder != SIZE_MAX) {
        end = last_in_folder + 1;
    } else if (last_member != SIZE_MAX) {
        end = last_member + 1;
    } else {
        end = n;
    }
    size_t pos;
    if (before_name != NULL) {
        size_t bi = index_of(v, n, before_name);
        pos = bi == SIZE_MAX ? n : bi;
    } else if (have_index) {
        /* the index-th member of the target group. */
        size_t want = index > n ? n : (size_t)index;
        size_t seen = 0, found = SIZE_MAX;
        for (size_t k = 0; k < n; k++) {
            if (pc_str_eq_c(&v[k].group, target)) {
                if (seen == want) {
                    found = k;
                    break;
                }
                seen++;
            }
        }
        if (found != SIZE_MAX) {
            pos = found;
        } else {
            pos = last_member != SIZE_MAX ? last_member + 1 : n;
        }
    } else {
        pos = end;
    }
    if (pos > n) {
        pos = n;
    }
    (void)nmembers;
    insert_at(v, &n, pos, pr);
    s->tools.npresets = n;
    size_t at = position_in_group(v, n, pos);
    pc_engine_brush_presets_changed(s);

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
        goto done_move;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "name", pc_str_c(&v[pos].name));
    yyjson_mut_obj_add_strcpy(rd, root, "group", target);
    yyjson_mut_obj_add_val(rd, root, "folder", strvec_json(rd, &v[pos].folder));
    yyjson_mut_obj_add_uint(rd, root, "index", at);
    yyjson_mut_doc_set_root(rd, root);
    rc = PC_OK;
done_move:
    free(target);
    free(before_name);
    strvec_fini(&tfolder);
    return rc;
}

/* ---- group / folder existence ------------------------------------------------------------- */

static pc_status group_block(const pc_paint_preset *v, size_t n, const char *g, const char *cmd,
                             pc_error *err)
{
    if (group_exists(v, n, g)) {
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, cmd, "no brush preset group named `%s`", g);
}

/* The group and (possibly empty) folder a group command acts on; both must exist. Owns *g, *f. */
static pc_status target_folder(const pc_paint_preset *v, size_t n, yyjson_val *p, const char *cmd,
                               char **g_out, strvec *f_out, pc_error *err)
{
    *g_out = NULL;
    f_out->v = NULL;
    f_out->n = 0;
    bool have_g = false;
    char *g = NULL;
    pc_status rc = bo_group_param(p, "group", cmd, &have_g, &g, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!have_g) {
        return pc_engine_err_bad_params(err, cmd, "missing `group`");
    }
    if ((rc = group_block(v, n, g, cmd, err)) != PC_OK) {
        free(g);
        return rc;
    }
    bool have_f = false;
    strvec f = {0};
    if ((rc = bo_folder_param(p, cmd, &have_f, &f, err)) != PC_OK) {
        free(g);
        return rc;
    }
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (in_folder(&v[i], g, &f)) {
            any = true;
            break;
        }
    }
    if (!any) {
        free(g);
        strvec_fini(&f);
        return pc_engine_err_bad_params(err, cmd, "no such folder in the brush preset group");
    }
    *g_out = g;
    *f_out = f;
    return PC_OK;
}

/* ---- move a whole group (top level) ------------------------------------------------------- */

struct group_pred {
    const char *group;
};
static bool pred_group(const pc_paint_preset *x, void *ctx)
{
    struct group_pred *c = ctx;
    return pc_str_eq_c(&x->group, c->group);
}

struct folder_pred {
    const char *group;
    const strvec *folder;
};
static bool pred_in_folder(const pc_paint_preset *x, void *ctx)
{
    struct folder_pred *c = ctx;
    return in_folder(x, c->group, c->folder);
}

/* move a nested folder among its siblings (move_folder). */
static pc_status move_folder_op(pc_engine_session *s, yyjson_val *p, const char *g, const strvec *f,
                                const char *cmd, pc_json_value *out, pc_error *err)
{
    pc_paint_preset *v = s->tools.presets;
    size_t n = s->tools.npresets;
    if (f->n == 0) {
        return pc_engine_err_bad_params(err, cmd, "missing `folder`");
    }
    const char *leaf = f->v[f->n - 1];
    strvec parent = {f->v, f->n - 1}; /* borrowed view */
    strvec siblings = {0};
    pc_status rc = subfolder_order(v, n, g, &parent, &siblings, err);
    if (rc != PC_OK) {
        return rc;
    }
    char *before = NULL;
    yyjson_val *bv = pc_json_get(p, "before");
    if (bv != NULL && !yyjson_is_null(bv)) {
        size_t bl = 0;
        const char *bs = pc_json_as_str(bv, &bl);
        if (bs == NULL) {
            strvec_fini(&siblings);
            return pc_engine_err_bad_params(err, cmd, "`before` must name a folder next to the one being moved");
        }
        char *bd = dup_n(bs, bl);
        if (bd == NULL) {
            strvec_fini(&siblings);
            return pc_fail(err, PC_ERR_NOMEM, "before");
        }
        if (strcmp(bd, leaf) == 0) {
            free(bd);
            strvec_fini(&siblings);
            return pc_engine_err_bad_params(err, cmd, "`before` names the folder being moved");
        }
        if (strvec_index(&siblings, bd) == SIZE_MAX) {
            free(bd);
            strvec_fini(&siblings);
            return pc_engine_err_bad_params(err, cmd, "`before` must name a folder next to the one being moved");
        }
        before = bd;
    }
    bool have_index = false;
    uint64_t index = 0;
    if ((rc = opt_index(p, cmd, &have_index, &index, err)) != PC_OK) {
        free(before);
        strvec_fini(&siblings);
        return rc;
    }
    /* Compute pos among the rest (presets not in the folder). */
    size_t nrest = 0;
    for (size_t i = 0; i < n; i++) {
        if (!in_folder(&v[i], g, f)) {
            nrest++;
        }
    }
    /* first(name): position in rest of the first preset in sibling folder `name`. */
    /* Build the rest array index mapping lazily by scanning. */
    size_t pos;
    /* Helper: position-in-rest of first preset in folder sibling(name). */
#define FIRST_IN_SIBLING(NAME, OUTPOS)                                                                 \
    do {                                                                                               \
        strvec sib_ = {0};                                                                             \
        sib_.v = calloc(parent.n + 1, sizeof(char *));                                                 \
        if (sib_.v == NULL) {                                                                          \
            free(before);                                                                              \
            strvec_fini(&siblings);                                                                    \
            return pc_fail(err, PC_ERR_NOMEM, "sibling");                                              \
        }                                                                                              \
        for (size_t k_ = 0; k_ < parent.n; k_++) {                                                     \
            sib_.v[sib_.n++] = parent.v[k_];                                                           \
        }                                                                                              \
        sib_.v[sib_.n++] = (char *)(NAME);                                                             \
        size_t seen_ = 0, found_ = nrest;                                                              \
        for (size_t i_ = 0; i_ < n; i_++) {                                                            \
            if (in_folder(&v[i_], g, f)) {                                                             \
                continue;                                                                              \
            }                                                                                          \
            if (in_folder(&v[i_], g, &sib_)) {                                                         \
                found_ = seen_;                                                                        \
                break;                                                                                 \
            }                                                                                          \
            seen_++;                                                                                   \
        }                                                                                              \
        free(sib_.v);                                                                                  \
        (OUTPOS) = found_;                                                                             \
    } while (0)

    if (before != NULL) {
        FIRST_IN_SIBLING(before, pos);
    } else if (have_index) {
        /* others: siblings excluding leaf, in order. */
        size_t want = index;
        char *name_at = NULL;
        size_t seen = 0;
        for (size_t k = 0; k < siblings.n; k++) {
            if (strcmp(siblings.v[k], leaf) == 0) {
                continue;
            }
            if (seen == want) {
                name_at = siblings.v[k];
                break;
            }
            seen++;
        }
        if (name_at != NULL) {
            FIRST_IN_SIBLING(name_at, pos);
        } else {
            /* parent_end: position-in-rest after the parent's last preset, else start. */
            size_t seen2 = 0, last = SIZE_MAX;
            for (size_t i = 0; i < n; i++) {
                if (in_folder(&v[i], g, f)) {
                    continue;
                }
                if (in_folder(&v[i], g, &parent)) {
                    last = seen2;
                }
                seen2++;
            }
            pos = last == SIZE_MAX ? nrest : last + 1;
        }
    } else {
        size_t seen2 = 0, last = SIZE_MAX;
        for (size_t i = 0; i < n; i++) {
            if (in_folder(&v[i], g, f)) {
                continue;
            }
            if (in_folder(&v[i], g, &parent)) {
                last = seen2;
            }
            seen2++;
        }
        pos = last == SIZE_MAX ? nrest : last + 1;
    }
#undef FIRST_IN_SIBLING

    struct folder_pred fp = {g, f};
    rc = reorder_block(v, n, pred_in_folder, &fp, pos, err);
    free(before);
    if (rc != PC_OK) {
        strvec_fini(&siblings);
        return rc;
    }
    pc_engine_brush_presets_changed(s);
    /* new sibling index of leaf. */
    strvec sib2 = {0};
    if ((rc = subfolder_order(v, n, g, &parent, &sib2, err)) != PC_OK) {
        strvec_fini(&siblings);
        return rc;
    }
    size_t at = strvec_index(&sib2, leaf);
    if (at == SIZE_MAX) {
        at = 0;
    }
    strvec_fini(&sib2);
    strvec_fini(&siblings);

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "group", g);
    yyjson_mut_val *fa = yyjson_mut_arr(rd);
    for (size_t k = 0; k < f->n; k++) {
        yyjson_mut_arr_add_strcpy(rd, fa, f->v[k]);
    }
    yyjson_mut_obj_add_val(rd, root, "folder", fa);
    yyjson_mut_obj_add_uint(rd, root, "index", at);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* move a whole group at the top level (the f-empty, no-into case). */
static pc_status move_group_top(pc_engine_session *s, yyjson_val *p, const char *g, const char *cmd,
                                pc_json_value *out, pc_error *err)
{
    pc_paint_preset *v = s->tools.presets;
    size_t n = s->tools.npresets;
    bool have_before = false;
    char *before = NULL;
    pc_status rc = bo_group_param(p, "before", cmd, &have_before, &before, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (have_before) {
        if ((rc = group_block(v, n, before, cmd, err)) != PC_OK) {
            free(before);
            return rc;
        }
        if (strcmp(before, g) == 0) {
            free(before);
            return pc_engine_err_bad_params(err, cmd, "`before` names the group being moved");
        }
    }
    bool have_index = false;
    uint64_t index = 0;
    if ((rc = opt_index(p, cmd, &have_index, &index, err)) != PC_OK) {
        free(before);
        return rc;
    }
    /* rest = presets whose group != g; its group order. */
    size_t nrest = 0;
    for (size_t i = 0; i < n; i++) {
        if (!pc_str_eq_c(&v[i].group, g)) {
            nrest++;
        }
    }
    /* position-in-rest of the first preset of group `name`. */
    size_t pos = nrest;
    if (have_before) {
        size_t seen = 0;
        pos = nrest;
        for (size_t i = 0; i < n; i++) {
            if (pc_str_eq_c(&v[i].group, g)) {
                continue;
            }
            if (pc_str_eq_c(&v[i].group, before)) {
                pos = seen;
                break;
            }
            seen++;
        }
    } else if (have_index) {
        strvec order = {0};
        /* order of groups in rest. */
        for (size_t i = 0; i < n; i++) {
            if (pc_str_eq_c(&v[i].group, g)) {
                continue;
            }
            const char *gg = pc_str_c(&v[i].group);
            if (strvec_index(&order, gg) != SIZE_MAX) {
                continue;
            }
            char **grown = realloc(order.v, (order.n + 1) * sizeof(char *));
            if (grown == NULL) {
                strvec_fini(&order);
                free(before);
                return pc_fail(err, PC_ERR_NOMEM, "groups");
            }
            order.v = grown;
            order.v[order.n] = dup_n(gg, v[i].group.len);
            if (order.v[order.n] == NULL) {
                strvec_fini(&order);
                free(before);
                return pc_fail(err, PC_ERR_NOMEM, "groups");
            }
            order.n++;
        }
        if (index < order.n) {
            const char *name = order.v[index];
            size_t seen = 0;
            pos = nrest;
            for (size_t i = 0; i < n; i++) {
                if (pc_str_eq_c(&v[i].group, g)) {
                    continue;
                }
                if (pc_str_eq_c(&v[i].group, name)) {
                    pos = seen;
                    break;
                }
                seen++;
            }
        } else {
            pos = nrest;
        }
        strvec_fini(&order);
    }
    struct group_pred gp = {g};
    rc = reorder_block(v, n, pred_group, &gp, pos, err);
    free(before);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_brush_presets_changed(s);
    /* new group index. */
    strvec order = {0};
    if ((rc = group_order(v, n, &order, err)) != PC_OK) {
        return rc;
    }
    size_t at = strvec_index(&order, g);
    if (at == SIZE_MAX) {
        at = 0;
    }
    strvec_fini(&order);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "group", g);
    yyjson_mut_obj_add_uint(rd, root, "index", at);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_move_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "brush.presets.moveGroup";
    pc_paint_preset *v = s->tools.presets;
    size_t n = s->tools.npresets;
    char *g = NULL;
    strvec f = {0};
    pc_status rc = target_folder(v, n, p, cmd, &g, &f, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* `into` (move a folder/group inside another, or out to the top level) is not supported here;
     * report it clearly rather than silently ignoring (§8). */
    yyjson_val *into = pc_json_get(p, "into");
    if (into != NULL && !yyjson_is_null(into)) {
        free(g);
        strvec_fini(&f);
        return pc_engine_err_bad_params(err, cmd, "`into` (moving a folder inside another) is not supported in this build");
    }
    if (f.n > 0) {
        rc = move_folder_op(s, p, g, &f, cmd, out, err);
    } else {
        rc = move_group_top(s, p, g, cmd, out, err);
    }
    free(g);
    strvec_fini(&f);
    return rc;
}

/* ---- rename a group or folder ------------------------------------------------------------- */

static pc_status run_rename_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "brush.presets.renameGroup";
    pc_paint_preset *v = s->tools.presets;
    size_t n = s->tools.npresets;
    char *g = NULL;
    strvec f = {0};
    pc_status rc = target_folder(v, n, p, cmd, &g, &f, err);
    if (rc != PC_OK) {
        return rc;
    }
    char *newname = NULL;
    if ((rc = bo_text(p, "newName", cmd, &newname, err)) != PC_OK) {
        free(g);
        strvec_fini(&f);
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (f.n > 0) {
        const char *leaf = f.v[f.n - 1];
        strvec parent = {f.v, f.n - 1};
        if (strcmp(newname, leaf) != 0) {
            strvec sib = {0};
            if ((rc = subfolder_order(v, n, g, &parent, &sib, err)) != PC_OK) {
                goto done_rg;
            }
            if (strvec_index(&sib, newname) != SIZE_MAX) {
                strvec_fini(&sib);
                rc = pc_engine_err_bad_params(err, cmd, "a folder named `%s` already exists there", newname);
                goto done_rg;
            }
            strvec_fini(&sib);
            size_t depth = parent.n;
            for (size_t i = 0; i < n; i++) {
                if (!in_folder(&v[i], g, &f)) {
                    continue;
                }
                if (depth < v[i].folder.len) {
                    if ((rc = pc_str_set_c(&v[i].folder.v[depth], newname, err)) != PC_OK) {
                        goto done_rg;
                    }
                }
                v[i].builtin = false;
            }
            pc_engine_brush_presets_changed(s);
        }
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            goto done_rg;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, root, "group", g);
        yyjson_mut_val *fa = yyjson_mut_arr(rd);
        for (size_t k = 0; k < parent.n; k++) {
            yyjson_mut_arr_add_strcpy(rd, fa, parent.v[k]);
        }
        yyjson_mut_arr_add_strcpy(rd, fa, newname);
        yyjson_mut_obj_add_val(rd, root, "folder", fa);
        yyjson_mut_doc_set_root(rd, root);
        rc = PC_OK;
        goto done_rg;
    }
    /* whole group. */
    if (strcmp(newname, g) != 0 && group_exists(v, n, newname)) {
        rc = pc_engine_err_bad_params(err, cmd, "a group named `%s` already exists", newname);
        goto done_rg;
    }
    if (strcmp(newname, g) != 0) {
        for (size_t i = 0; i < n; i++) {
            if (pc_str_eq_c(&v[i].group, g)) {
                if ((rc = pc_str_set_c(&v[i].group, newname, err)) != PC_OK) {
                    goto done_rg;
                }
                v[i].builtin = false;
            }
        }
        pc_engine_brush_presets_changed(s);
    }
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
        goto done_rg;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "group", newname);
    yyjson_mut_doc_set_root(rd, root);
    rc = PC_OK;
done_rg:
    free(g);
    free(newname);
    strvec_fini(&f);
    return rc;
}

/* ---- delete a group or folder ------------------------------------------------------------- */

static pc_status run_delete_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "brush.presets.deleteGroup";
    pc_paint_preset *v = s->tools.presets;
    size_t n = s->tools.npresets;
    char *g = NULL;
    strvec f = {0};
    pc_status rc = target_folder(v, n, p, cmd, &g, &f, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t before = n;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (in_folder(&v[i], g, &f)) {
            pc_paint_preset_fini(&v[i]);
        } else {
            if (w != i) {
                v[w] = v[i];
            }
            w++;
        }
    }
    n = w;
    s->tools.npresets = n;
    /* Drop the current preset if it is gone. */
    if (s->tools.current_preset != NULL) {
        bool still = false;
        for (size_t i = 0; i < n; i++) {
            if (bo_casecmp(pc_str_c(&v[i].name), s->tools.current_preset) == 0) {
                still = true;
                break;
            }
        }
        if (!still) {
            free(s->tools.current_preset);
            s->tools.current_preset = NULL;
        }
    }
    pc_engine_brush_presets_changed(s);
    free(g);
    strvec_fini(&f);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "deleted", before - n);
    yyjson_mut_obj_add_uint(rd, root, "count", n);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- registry ---------------------------------------------------------------------------- */

static const pc_engine_command SPECS[] = {
    {"brush.presets.rename", "Rename Brush", NULL, 0, NULL,
     "{\"name\":string,\"newName\":string} -> {name}", always, run_rename, true},
    {"brush.presets.move", "Move Brush", NULL, 0, NULL,
     "{\"name\":string,\"group\":string?,\"folder\":[name]?,\"before\":name? | \"index\":n?} -> "
     "{name, group, folder, index}",
     always, run_move, true},
    {"brush.presets.moveGroup", "Move Brush Group", NULL, 0, NULL,
     "{\"group\":string,\"folder\":[name]?,\"before\":group|folder? | \"index\":n?} -> {group, folder?, "
     "index} (`into` nesting is not supported in this build)",
     always, run_move_group, true},
    {"brush.presets.renameGroup", "Rename Brush Group", NULL, 0, NULL,
     "{\"group\":string,\"folder\":[name]?,\"newName\":string} -> {group, folder?}", always,
     run_rename_group, true},
    {"brush.presets.deleteGroup", "Delete Brush Group", NULL, 0, NULL,
     "{\"group\":string,\"folder\":[name]?} -> {deleted, count}", always, run_delete_group, true},
};

const pc_engine_command *pc_engine_brush_organize_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
