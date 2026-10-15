/* Preset-panel data model: the generic group machinery, PresetState lifecycle, and the pattern /
 * gradient accessors other chunks consume (port of crates/engine/src/presets/mod.rs, E2b).
 *
 * The per-kind data (gradients, swatches, styles, shapes, tool presets, clone sources) lives in
 * presets_*.c; this file holds what they share. Never crashes (REWRITE_PLAN.md rule 9). */
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "engine_internal.h"
#include "paint/tile.h"
#include "presets.h"

#define PC_PRESET_MAX_FILE_BYTES ((uint64_t)512 << 20)

const char *const PC_PRESET_GROUP_EDIT_PARAMS =
    "{\"action\":\"rename|delete|move|newGroup|renameGroup|deleteGroup\",\"preset\":name|[names] "
    "(rename/delete/move),\"group\":name? (narrows the lookup; the group for "
    "renameGroup/deleteGroup),\"name\":str (rename: new name; newGroup/renameGroup: group "
    "name),\"to\":group (move),\"index\":n? (move)}";

/* ---- small helpers ------------------------------------------------------------------------ */

static char *dup_str(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

static void *item_at(const pc_preset_group *g, const pc_preset_item_ops *ops, size_t i)
{
    return (char *)g->items + i * ops->size;
}

/* ---- group vector --------------------------------------------------------------------------- */

void pc_preset_groups_init(pc_preset_groups *g)
{
    g->v = NULL;
    g->n = 0;
    g->cap = 0;
}

void pc_preset_groups_fini(pc_preset_groups *g, const pc_preset_item_ops *ops)
{
    if (g == NULL) {
        return;
    }
    for (size_t i = 0; i < g->n; i++) {
        pc_preset_group *gr = &g->v[i];
        for (size_t j = 0; j < gr->n; j++) {
            ops->fini(item_at(gr, ops, j));
        }
        free(gr->items);
        free(gr->name);
    }
    free(g->v);
    pc_preset_groups_init(g);
}

static pc_status groups_reserve(pc_preset_groups *g, size_t extra, pc_error *err)
{
    if (g->n + extra <= g->cap) {
        return PC_OK;
    }
    size_t cap = g->cap == 0 ? 4 : g->cap * 2;
    while (cap < g->n + extra) {
        cap *= 2;
    }
    pc_preset_group *v = realloc(g->v, cap * sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "preset groups");
    }
    g->v = v;
    g->cap = cap;
    return PC_OK;
}

pc_preset_group *pc_preset_groups_push(pc_preset_groups *g, const char *name, pc_error *err)
{
    if (groups_reserve(g, 1, err) != PC_OK) {
        return NULL;
    }
    char *nm = dup_str(name == NULL ? "" : name);
    if (nm == NULL) {
        (void)pc_fail(err, PC_ERR_NOMEM, "group name");
        return NULL;
    }
    pc_preset_group *gr = &g->v[g->n++];
    gr->name = nm;
    gr->items = NULL;
    gr->n = 0;
    gr->cap = 0;
    return gr;
}

static pc_status group_reserve(pc_preset_group *gr, const pc_preset_item_ops *ops, size_t extra,
                               pc_error *err)
{
    if (gr->n + extra <= gr->cap) {
        return PC_OK;
    }
    size_t cap = gr->cap == 0 ? 4 : gr->cap * 2;
    while (cap < gr->n + extra) {
        cap *= 2;
    }
    void *items = realloc(gr->items, cap * ops->size);
    if (items == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "preset items");
    }
    gr->items = items;
    gr->cap = cap;
    return PC_OK;
}

pc_status pc_preset_group_push(pc_preset_groups *g, size_t gi, const void *item,
                               const pc_preset_item_ops *ops, pc_error *err)
{
    if (gi >= g->n) {
        return pc_fail(err, PC_ERR_INVALID, "group index");
    }
    pc_preset_group *gr = &g->v[gi];
    if (group_reserve(gr, ops, 1, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    memcpy(item_at(gr, ops, gr->n), item, ops->size);
    gr->n++;
    return PC_OK;
}

pc_status pc_preset_groups_clone(const pc_preset_groups *src, pc_preset_groups *out,
                                 const pc_preset_item_ops *ops, pc_error *err)
{
    pc_preset_groups_init(out);
    for (size_t i = 0; i < src->n; i++) {
        const pc_preset_group *sg = &src->v[i];
        pc_preset_group *dg = pc_preset_groups_push(out, sg->name, err);
        if (dg == NULL) {
            goto fail;
        }
        if (group_reserve(dg, ops, sg->n, err) != PC_OK) {
            goto fail;
        }
        for (size_t j = 0; j < sg->n; j++) {
            if (ops->clone(item_at(sg, ops, j), item_at(dg, ops, dg->n), err) != PC_OK) {
                goto fail;
            }
            dg->n++;
        }
    }
    return PC_OK;
fail:
    pc_preset_groups_fini(out, ops);
    return PC_ERR_NOMEM;
}

/* ---- lookup helpers ----------------------------------------------------------------------- */

bool pc_preset_find(const pc_preset_groups *g, const char *name, const char *group,
                    const pc_preset_item_ops *ops, size_t *gi, size_t *ii)
{
    for (size_t i = 0; i < g->n; i++) {
        if (group != NULL && strcmp(g->v[i].name, group) != 0) {
            continue;
        }
        const pc_preset_group *gr = &g->v[i];
        for (size_t j = 0; j < gr->n; j++) {
            if (strcmp(ops->get_name(item_at(gr, ops, j)), name) == 0) {
                *gi = i;
                *ii = j;
                return true;
            }
        }
    }
    return false;
}

static bool name_taken(const pc_preset_groups *g, const char *name, const pc_preset_item_ops *ops)
{
    size_t gi, ii;
    return pc_preset_find(g, name, NULL, ops, &gi, &ii);
}

char *pc_preset_unique_name(const pc_preset_groups *g, const char *base, const pc_preset_item_ops *ops)
{
    if (!name_taken(g, base, ops)) {
        return dup_str(base);
    }
    for (unsigned i = 2; i < 1000000u; i++) {
        char buf[320];
        (void)snprintf(buf, sizeof buf, "%s %u", base, i);
        if (!name_taken(g, buf, ops)) {
            return dup_str(buf);
        }
    }
    return dup_str(base);
}

static bool group_name_taken(const pc_preset_groups *g, const char *name)
{
    for (size_t i = 0; i < g->n; i++) {
        if (strcmp(g->v[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

char *pc_preset_unique_group(const pc_preset_groups *g, const char *base)
{
    if (!group_name_taken(g, base)) {
        return dup_str(base);
    }
    for (unsigned i = 1; i < 1000000u; i++) {
        char buf[320];
        (void)snprintf(buf, sizeof buf, "%s %u", base, i);
        if (!group_name_taken(g, buf)) {
            return dup_str(buf);
        }
    }
    return dup_str("");
}

size_t pc_preset_group_index(pc_preset_groups *g, const char *group, pc_error *err)
{
    if (group != NULL) {
        for (size_t i = 0; i < g->n; i++) {
            if (strcmp(g->v[i].name, group) == 0) {
                return i;
            }
        }
        if (pc_preset_groups_push(g, group, err) == NULL) {
            return SIZE_MAX;
        }
        return g->n - 1;
    }
    if (g->n == 0) {
        if (pc_preset_groups_push(g, "Presets", err) == NULL) {
            return SIZE_MAX;
        }
    }
    return 0;
}

/* Remove item ii from group gi: run fini, then shift. */
static void group_remove(pc_preset_groups *g, const pc_preset_item_ops *ops, size_t gi, size_t ii)
{
    pc_preset_group *gr = &g->v[gi];
    if (ii >= gr->n) {
        return;
    }
    ops->fini(item_at(gr, ops, ii));
    size_t tail = gr->n - ii - 1;
    if (tail > 0) {
        memmove(item_at(gr, ops, ii), item_at(gr, ops, ii + 1), tail * ops->size);
    }
    gr->n--;
}

/* Remove item ii WITHOUT fini, moving its bytes into `scratch` (ops.size). */
static void group_take(pc_preset_groups *g, const pc_preset_item_ops *ops, size_t gi, size_t ii,
                       void *scratch)
{
    pc_preset_group *gr = &g->v[gi];
    memcpy(scratch, item_at(gr, ops, ii), ops->size);
    size_t tail = gr->n - ii - 1;
    if (tail > 0) {
        memmove(item_at(gr, ops, ii), item_at(gr, ops, ii + 1), tail * ops->size);
    }
    gr->n--;
}

static pc_status group_insert(pc_preset_groups *g, const pc_preset_item_ops *ops, size_t gi, size_t at,
                              const void *item, pc_error *err)
{
    pc_preset_group *gr = &g->v[gi];
    if (group_reserve(gr, ops, 1, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (at > gr->n) {
        at = gr->n;
    }
    size_t tail = gr->n - at;
    if (tail > 0) {
        memmove(item_at(gr, ops, at + 1), item_at(gr, ops, at), tail * ops->size);
    }
    memcpy(item_at(gr, ops, at), item, ops->size);
    gr->n++;
    return PC_OK;
}

/* A trimmed non-empty string param, or NULL. */
static const char *str_param(yyjson_val *p, const char *key)
{
    yyjson_val *v = pc_json_get(p, key);
    size_t len = 0;
    const char *s = pc_json_as_str(v, &len);
    if (s == NULL) {
        return NULL;
    }
    while (len > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r')) {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) {
        len--;
    }
    return len == 0 ? NULL : s; /* borrowed; NUL-terminated in the parsed doc but trailing ws kept */
}

/* A req trimmed string copied into out[cap]; PC_ERR_INVALID with the reference message otherwise. */
static pc_status req_str_copy(yyjson_val *p, const char *key, const char *cmd, char *out, size_t cap,
                              pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    size_t len = 0;
    const char *s = pc_json_as_str(v, &len);
    if (s != NULL) {
        size_t a = 0, b = len;
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) {
            a++;
        }
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) {
            b--;
        }
        if (b > a) {
            size_t n = b - a;
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, s + a, n);
            out[n] = '\0';
            return PC_OK;
        }
    }
    return pc_engine_err_bad_params(err, cmd, "missing `%s`", key);
}

/* ---- edit_groups -------------------------------------------------------------------------- */

static pc_status result_obj_str(pc_json_value *out, const char *key, const char *val, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, key, val);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status result_obj_uint(pc_json_value *out, const char *key, uint64_t val, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, key, val);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status edit_rename(pc_preset_groups *g, yyjson_val *p, const char *cmd,
                             const pc_preset_item_ops *ops, pc_json_value *out, pc_error *err)
{
    char name[512], to[512];
    if (req_str_copy(p, "preset", cmd, name, sizeof name, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (req_str_copy(p, "name", cmd, to, sizeof to, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    const char *group = str_param(p, "group");
    char grp[512];
    if (group != NULL) {
        size_t gl = strlen(group);
        if (gl >= sizeof grp) {
            gl = sizeof grp - 1;
        }
        memcpy(grp, group, gl);
        grp[gl] = '\0';
    }
    size_t gi, ii, dgi, dii;
    if (!pc_preset_find(g, name, group != NULL ? grp : NULL, ops, &gi, &ii)) {
        return pc_engine_err_bad_params(err, cmd, "no preset \"%s\"", name);
    }
    if (strcmp(name, to) != 0 && pc_preset_find(g, to, NULL, ops, &dgi, &dii)) {
        return pc_engine_err_bad_params(err, cmd, "a preset named \"%s\" already exists", to);
    }
    if (!ops->set_name(item_at(&g->v[gi], ops, ii), to)) {
        return pc_fail(err, PC_ERR_NOMEM, "rename");
    }
    /* Echo the stored (owned) name, not the stack copy of the JSON string. */
    return result_obj_str(out, "name", ops->get_name(item_at(&g->v[gi], ops, ii)), err);
}

static pc_status edit_delete(pc_preset_groups *g, yyjson_val *p, const char *cmd,
                             const pc_preset_item_ops *ops, pc_json_value *out, pc_error *err)
{
    const char *group = str_param(p, "group");
    char grp[512];
    if (group != NULL) {
        size_t gl = strlen(group);
        if (gl >= sizeof grp) {
            gl = sizeof grp - 1;
        }
        memcpy(grp, group, gl);
        grp[gl] = '\0';
        group = grp;
    }
    /* Collect the names (one or a list). All-or-nothing: a missing name deletes none. */
    char **names = NULL;
    size_t nn = 0, cap = 0;
    yyjson_val *pre = pc_json_get(p, "preset");
    pc_status rc = PC_OK;
    if (pre != NULL && yyjson_is_arr(pre)) {
        size_t idx, max;
        yyjson_val *el;
        yyjson_arr_foreach(pre, idx, max, el) {
            size_t len = 0;
            const char *s = pc_json_as_str(el, &len);
            if (s == NULL) {
                rc = pc_engine_err_bad_params(err, cmd, "missing `preset`");
                goto done;
            }
            if (nn == cap) {
                size_t nc = cap == 0 ? 4 : cap * 2;
                char **gr2 = realloc(names, nc * sizeof *gr2);
                if (gr2 == NULL) {
                    rc = pc_fail(err, PC_ERR_NOMEM, "delete");
                    goto done;
                }
                names = gr2;
                cap = nc;
            }
            names[nn] = malloc(len + 1);
            if (names[nn] == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "delete");
                goto done;
            }
            memcpy(names[nn], s, len);
            names[nn][len] = '\0';
            nn++;
        }
    } else {
        char one[512];
        if (req_str_copy(p, "preset", cmd, one, sizeof one, err) != PC_OK) {
            rc = PC_ERR_INVALID;
            goto done;
        }
        names = malloc(sizeof *names);
        if (names == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "delete");
            goto done;
        }
        names[0] = dup_str(one);
        if (names[0] == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "delete");
            goto done;
        }
        nn = 1;
    }
    /* Verify every name exists before touching anything. */
    for (size_t i = 0; i < nn; i++) {
        size_t gi, ii;
        if (!pc_preset_find(g, names[i], group, ops, &gi, &ii)) {
            rc = pc_engine_err_bad_params(err, cmd, "no preset \"%s\"", names[i]);
            goto done;
        }
    }
    /* Dedup (sort by name, drop equal neighbours). */
    for (size_t i = 1; i < nn; i++) {
        char *key = names[i];
        size_t j = i;
        while (j > 0 && strcmp(names[j - 1], key) > 0) {
            names[j] = names[j - 1];
            j--;
        }
        names[j] = key;
    }
    size_t uniq = 0;
    for (size_t i = 0; i < nn; i++) {
        if (i == 0 || strcmp(names[i], names[uniq - 1]) != 0) {
            names[uniq++] = names[i];
        } else {
            free(names[i]);
        }
    }
    nn = uniq;
    for (size_t i = 0; i < nn; i++) {
        size_t gi, ii;
        if (pc_preset_find(g, names[i], group, ops, &gi, &ii)) {
            group_remove(g, ops, gi, ii);
        }
    }
    rc = result_obj_uint(out, "deleted", nn, err);
done:
    for (size_t i = 0; i < nn && names != NULL; i++) {
        free(names[i]);
    }
    free(names);
    return rc;
}

static pc_status edit_move(pc_preset_groups *g, yyjson_val *p, const char *cmd,
                           const pc_preset_item_ops *ops, pc_json_value *out, pc_error *err)
{
    char name[512], to[512];
    if (req_str_copy(p, "preset", cmd, name, sizeof name, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (req_str_copy(p, "to", cmd, to, sizeof to, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    const char *group = str_param(p, "group");
    char grp[512];
    if (group != NULL) {
        size_t gl = strlen(group);
        if (gl >= sizeof grp) {
            gl = sizeof grp - 1;
        }
        memcpy(grp, group, gl);
        grp[gl] = '\0';
        group = grp;
    }
    size_t gi, ii;
    if (!pc_preset_find(g, name, group, ops, &gi, &ii)) {
        return pc_engine_err_bad_params(err, cmd, "no preset \"%s\"", name);
    }
    void *scratch = malloc(ops->size);
    if (scratch == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "move");
    }
    group_take(g, ops, gi, ii, scratch);
    size_t ti = pc_preset_group_index(g, to, err);
    if (ti == SIZE_MAX) {
        ops->fini(scratch);
        free(scratch);
        return PC_ERR_NOMEM;
    }
    uint64_t at;
    size_t pos = g->v[ti].n;
    if (pc_json_as_u64(pc_json_get(p, "index"), &at)) {
        pos = at < (uint64_t)g->v[ti].n ? (size_t)at : g->v[ti].n;
    }
    if (group_insert(g, ops, ti, pos, scratch, err) != PC_OK) {
        ops->fini(scratch);
        free(scratch);
        return PC_ERR_NOMEM;
    }
    free(scratch); /* bytes moved into the array; do not fini */
    return result_obj_str(out, "group", g->v[ti].name, err);
}

static pc_status edit_new_group(pc_preset_groups *g, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *base = str_param(p, "name");
    if (base == NULL) {
        base = "Group";
    }
    /* Trim into a stack copy since str_param returns a borrowed, still-ws-tail pointer. */
    char baseb[512];
    size_t bl = strlen(base);
    size_t a = 0, b = bl;
    while (a < b && (base[a] == ' ' || base[a] == '\t')) {
        a++;
    }
    while (b > a && (base[b - 1] == ' ' || base[b - 1] == '\t')) {
        b--;
    }
    size_t n = b - a;
    if (n >= sizeof baseb) {
        n = sizeof baseb - 1;
    }
    memcpy(baseb, base + a, n);
    baseb[n] = '\0';
    /* newGroup uses a start of 1 (base, base 1, base 2...). */
    char *name;
    if (!group_name_taken(g, baseb)) {
        name = dup_str(baseb);
    } else {
        name = NULL;
        for (unsigned i = 1; i < 1000000u; i++) {
            char buf[576];
            (void)snprintf(buf, sizeof buf, "%s %u", baseb, i);
            if (!group_name_taken(g, buf)) {
                name = dup_str(buf);
                break;
            }
        }
        if (name == NULL) {
            name = dup_str("");
        }
    }
    if (name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "newGroup");
    }
    if (pc_preset_groups_push(g, name, err) == NULL) {
        free(name);
        return PC_ERR_NOMEM;
    }
    pc_status rc = result_obj_str(out, "group", name, err);
    free(name);
    return rc;
}

static pc_status edit_rename_group(pc_preset_groups *g, yyjson_val *p, const char *cmd,
                                   pc_json_value *out, pc_error *err)
{
    char gname[512], to[512];
    if (req_str_copy(p, "group", cmd, gname, sizeof gname, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (req_str_copy(p, "name", cmd, to, sizeof to, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    for (size_t i = 0; i < g->n; i++) {
        if (strcmp(g->v[i].name, gname) == 0) {
            char *nm = dup_str(to);
            if (nm == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "renameGroup");
            }
            free(g->v[i].name);
            g->v[i].name = nm;
            return result_obj_str(out, "group", g->v[i].name, err);
        }
    }
    return pc_engine_err_bad_params(err, cmd, "no group \"%s\"", gname);
}

static pc_status edit_delete_group(pc_preset_groups *g, yyjson_val *p, const char *cmd,
                                   const pc_preset_item_ops *ops, pc_json_value *out, pc_error *err)
{
    char gname[512];
    if (req_str_copy(p, "group", cmd, gname, sizeof gname, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    for (size_t i = 0; i < g->n; i++) {
        if (strcmp(g->v[i].name, gname) == 0) {
            pc_preset_group *gr = &g->v[i];
            size_t removed = gr->n;
            char *defname = gr->name; /* owned, defined; freed after the result is built */
            for (size_t j = 0; j < gr->n; j++) {
                ops->fini(item_at(gr, ops, j));
            }
            free(gr->items);
            yyjson_mut_doc *d = NULL;
            if (pc_engine_result_doc(out, &d, err) != PC_OK) {
                free(defname);
                size_t tail0 = g->n - i - 1;
                if (tail0 > 0) {
                    memmove(&g->v[i], &g->v[i + 1], tail0 * sizeof *g->v);
                }
                g->n--;
                return PC_ERR_NOMEM;
            }
            yyjson_mut_val *root = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, root, "group", defname);
            yyjson_mut_obj_add_uint(d, root, "deleted", removed);
            yyjson_mut_doc_set_root(d, root);
            free(defname);
            size_t tail = g->n - i - 1;
            if (tail > 0) {
                memmove(&g->v[i], &g->v[i + 1], tail * sizeof *g->v);
            }
            g->n--;
            return PC_OK;
        }
    }
    return pc_engine_err_bad_params(err, cmd, "no group \"%s\"", gname);
}

pc_status pc_preset_edit_groups(pc_preset_groups *g, const char *action, yyjson_val *p,
                                const char *cmd, const pc_preset_item_ops *ops, pc_json_value *out,
                                pc_error *err)
{
    if (strcmp(action, "rename") == 0) {
        return edit_rename(g, p, cmd, ops, out, err);
    }
    if (strcmp(action, "delete") == 0) {
        return edit_delete(g, p, cmd, ops, out, err);
    }
    if (strcmp(action, "move") == 0) {
        return edit_move(g, p, cmd, ops, out, err);
    }
    if (strcmp(action, "newGroup") == 0) {
        return edit_new_group(g, p, out, err);
    }
    if (strcmp(action, "renameGroup") == 0) {
        return edit_rename_group(g, p, cmd, out, err);
    }
    if (strcmp(action, "deleteGroup") == 0) {
        return edit_delete_group(g, p, cmd, ops, out, err);
    }
    return pc_engine_err_bad_params(err, cmd, "unknown action %s", action);
}

/* ---- String item ops (pattern group ids) -------------------------------------------------- */

static const char *str_get_name(const void *item)
{
    return *(char *const *)item;
}
static bool str_set_name(void *item, const char *name)
{
    char *nm = dup_str(name);
    if (nm == NULL) {
        return false;
    }
    free(*(char **)item);
    *(char **)item = nm;
    return true;
}
static void str_fini(void *item)
{
    free(*(char **)item);
    *(char **)item = NULL;
}
static pc_status str_clone(const void *src, void *dst, pc_error *err)
{
    char *nm = dup_str(*(char *const *)src);
    if (nm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "string");
    }
    *(char **)dst = nm;
    return PC_OK;
}

const pc_preset_item_ops *pc_preset_string_ops(void)
{
    static const pc_preset_item_ops ops = {sizeof(char *), str_get_name, str_set_name, str_fini, str_clone};
    return &ops;
}

/* ---- PresetState -------------------------------------------------------------------------- */

void pc_preset_state_init(pc_preset_state *st)
{
    memset(st, 0, sizeof *st);
    pc_preset_groups_init(&st->gradients);
    pc_preset_groups_init(&st->styles);
    pc_preset_groups_init(&st->shapes);
    pc_preset_groups_init(&st->pattern_groups);
    pc_preset_groups_init(&st->swatches);
    pc_json_value_fini(&st->layer_defaults);

    pc_error err = {0};
    (void)pc_gradients_builtin(&st->gradients, &err);
    (void)pc_gradient_preset_fg_to_bg(&st->gradient, &err);
    (void)pc_styles_builtin(&st->styles, &err);
    (void)pc_shapes_builtin(&st->shapes, &err);
    /* pattern_groups builtin ids come from the E10 pattern library (deferred); start with the
     * named folders empty so pc_engine_presets_patterns has them for E10 to populate. */
    (void)pc_preset_groups_push(&st->pattern_groups, "Geometric", &err);
    (void)pc_preset_groups_push(&st->pattern_groups, "Textures", &err);
    st->has_pattern = false;
    st->pattern = NULL;
    (void)pc_tool_presets_builtin(&st->tool_presets, &st->ntool_presets, &err);
    st->cap_tool_presets = st->ntool_presets;
    pc_clone_sources_default(&st->clone);
    (void)pc_swatches_builtin(&st->swatches, &err);
    st->rev = 0;
}

void pc_preset_state_fini(pc_preset_state *st)
{
    if (st == NULL) {
        return;
    }
    pc_preset_groups_fini(&st->gradients, pc_gradient_preset_ops());
    pc_gradient_preset_fini(&st->gradient);
    pc_preset_groups_fini(&st->styles, pc_style_preset_ops());
    pc_preset_groups_fini(&st->shapes, pc_shape_preset_ops());
    pc_preset_groups_fini(&st->pattern_groups, pc_preset_string_ops());
    free(st->pattern);
    for (size_t i = 0; i < st->ntool_presets; i++) {
        pc_tool_preset_fini(&st->tool_presets[i]);
    }
    free(st->tool_presets);
    pc_json_value_fini(&st->layer_defaults);
    pc_preset_groups_fini(&st->swatches, pc_swatch_ops());
    pc_engine_document_presets_fini(st); /* E3: File > New presets / recent list */
    memset(st, 0, sizeof *st);
}

/* ---- exported accessors (engine_internal.h) ----------------------------------------------- */

pc_preset_groups *pc_engine_presets_patterns(pc_engine_session *s)
{
    return &s->presets.pattern_groups;
}

pc_preset_groups *pc_engine_presets_gradients(pc_engine_session *s)
{
    return &s->presets.gradients;
}

void pc_engine_presets_changed(pc_engine_session *s)
{
    s->presets.rev++;
    s->prefs.rev++; /* mark the preferences document dirty (Rust prefs.edit(|_| ())) */
}

pc_status pc_engine_preset_file_bytes(yyjson_val *p, const char *cmd, const char *what, pc_buf *out,
                                      char *stem, size_t stem_cap, pc_error *err)
{
    pc_buf_init(out);
    if (stem_cap > 0) {
        stem[0] = '\0';
    }
    yyjson_val *data = pc_json_get(p, "data");
    if (data != NULL) {
        size_t slen = 0;
        const char *s = pc_json_as_str(data, &slen);
        if (s == NULL) {
            return pc_engine_err_bad_params(err, cmd, "`data` must be a base64 string");
        }
        if ((uint64_t)slen > PC_PRESET_MAX_FILE_BYTES / 3 * 4 + 4) {
            return pc_engine_err_bad_params(err, cmd, "`data` is too large");
        }
        if (pc_paint_b64_decode(s, slen, out, err) != PC_OK) {
            pc_buf_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`data` is not valid base64");
        }
        return PC_OK;
    }
    const char *path = str_param(p, "path");
    if (path == NULL) {
        return pc_engine_err_bad_params(err, cmd, "give `path` (%s) or `data` (base64)", what);
    }
    /* str_param returns a borrowed pointer that may carry trailing whitespace; copy + trim. */
    char pbuf[4096];
    {
        size_t n = strlen(path);
        while (n > 0 && (path[n - 1] == ' ' || path[n - 1] == '\t' || path[n - 1] == '\n' || path[n - 1] == '\r')) {
            n--;
        }
        if (n == 0 || n >= sizeof pbuf) {
            return pc_engine_err_bad_params(err, cmd, "give `path` (%s) or `data` (base64)", what);
        }
        memcpy(pbuf, path, n);
        pbuf[n] = '\0';
    }
    if (pc_file_read(pbuf, (size_t)PC_PRESET_MAX_FILE_BYTES, out, err) != PC_OK) {
        pc_buf_fini(out);
        return err != NULL && err->code != PC_OK ? err->code
                                                 : pc_engine_err_other(err, "%s", pbuf);
    }
    /* file stem: base name without extension */
    if (stem_cap > 0) {
        const char *base = pbuf;
        for (const char *q = pbuf; *q; q++) {
            if (*q == '/') {
                base = q + 1;
            }
        }
        const char *dot = NULL;
        for (const char *q = base; *q; q++) {
            if (*q == '.') {
                dot = q;
            }
        }
        size_t sn = dot != NULL && dot != base ? (size_t)(dot - base) : strlen(base);
        if (sn >= stem_cap) {
            sn = stem_cap - 1;
        }
        memcpy(stem, base, sn);
        stem[sn] = '\0';
    }
    return PC_OK;
}
