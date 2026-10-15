/* Swatches panel commands (port of crates/engine/src/swatch_cmds.rs, E2b): add, delete, rename,
 * reorder and group swatches, set the foreground/background from one, restore defaults, and import
 * or export .aco / .ase files. The library model lives in presets_swatches.c. Never crashes on a
 * corrupt .aco / .ase file. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "base/buf.h"
#include "base/file.h"
#include "engine_internal.h"
#include "paint/tile.h"
#include "presets.h"
#include "psd/aco.h"
#include "psd/ase.h"

static char *dup_str(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

static const pc_preset_item_ops *OPS(void)
{
    return pc_swatch_ops();
}

static size_t total(const pc_preset_groups *g)
{
    size_t t = 0;
    for (size_t i = 0; i < g->n; i++) {
        t += g->v[i].n;
    }
    return t;
}

static pc_status enabled_has_swatches(const pc_engine_session *s, pc_error *r)
{
    return total(&s->presets.swatches) > 0 ? PC_OK : pc_fail(r, PC_ERR_INVALID, "the Swatches panel is empty");
}
static pc_status enabled_has_groups(const pc_engine_session *s, pc_error *r)
{
    return s->presets.swatches.n > 0 ? PC_OK : pc_fail(r, PC_ERR_INVALID, "there are no swatch groups");
}

/* clip to 255 codepoints (returns owned). */
static char *clip(const char *s)
{
    size_t chars = 0, i = 0;
    while (s[i] != '\0' && chars < PC_SWATCH_MAX_NAME) {
        unsigned char c = (unsigned char)s[i];
        i += c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        chars++;
    }
    char *p = malloc(i + 1);
    if (p != NULL) {
        memcpy(p, s, i);
        p[i] = '\0';
    }
    return p;
}

/* An optional trimmed name param: *present=false when absent/null; error on empty / non-string.
 * On success *out is an owned clipped string (free). */
static pc_status name_param(yyjson_val *p, const char *k, const char *cmd, bool *present, char **out,
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
    if (s == NULL) {
        return pc_engine_err_bad_params(err, cmd, "`%s` must be a non-empty string", k);
    }
    size_t a = 0, b = len;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) {
        a++;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) {
        b--;
    }
    if (b == a) {
        return pc_engine_err_bad_params(err, cmd, "`%s` must be a non-empty string", k);
    }
    char tmp[1024];
    size_t n = b - a;
    if (n >= sizeof tmp) {
        n = sizeof tmp - 1;
    }
    memcpy(tmp, s + a, n);
    tmp[n] = '\0';
    *out = clip(tmp);
    if (*out == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "name");
    }
    *present = true;
    return PC_OK;
}

/* borrowed trimmed string (may keep trailing ws trimmed via copy in callers). */
static const char *str_param(yyjson_val *p, const char *k, char *buf, size_t cap)
{
    size_t len = 0;
    const char *s = pc_json_as_str(pc_json_get(p, k), &len);
    if (s == NULL) {
        return NULL;
    }
    size_t a = 0, b = len;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) {
        a++;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) {
        b--;
    }
    if (b == a) {
        return NULL;
    }
    size_t n = b - a;
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(buf, s + a, n);
    buf[n] = '\0';
    return buf;
}

static bool find_swatch(const pc_preset_groups *g, const char *name, const char *group, size_t *gi,
                        size_t *ii)
{
    return pc_preset_find(g, name, group, pc_swatch_ops(), gi, ii);
}

/* Resolve the swatch a command names: "swatch" (+ "group") or "index" across all groups. */
static pc_status locate(pc_engine_session *s, yyjson_val *p, const char *cmd, size_t *gi, size_t *ii,
                        pc_error *err)
{
    char nb[1024], gb[1024];
    const char *name = str_param(p, "swatch", nb, sizeof nb);
    if (name != NULL) {
        const char *grp = str_param(p, "group", gb, sizeof gb);
        if (!find_swatch(&s->presets.swatches, name, grp, gi, ii)) {
            return pc_engine_err_bad_params(err, cmd, "no swatch \"%s\" (see swatches.list)", name);
        }
        return PC_OK;
    }
    yyjson_val *iv = pc_json_get(p, "index");
    if (iv != NULL) {
        uint64_t idx;
        if (!pc_json_as_u64(iv, &idx)) {
            return pc_engine_err_bad_params(err, cmd, "`index` must be a non-negative integer");
        }
        size_t left = idx > SIZE_MAX ? SIZE_MAX : (size_t)idx;
        for (size_t i = 0; i < s->presets.swatches.n; i++) {
            if (left < s->presets.swatches.v[i].n) {
                *gi = i;
                *ii = left;
                return PC_OK;
            }
            left -= s->presets.swatches.v[i].n;
        }
        return pc_engine_err_bad_params(err, cmd, "no swatch at index %llu (the panel has %zu)",
                                        (unsigned long long)idx, total(&s->presets.swatches));
    }
    return pc_engine_err_bad_params(err, cmd, "give `swatch` (a name) or `index`");
}

static pc_status group_of(pc_engine_session *s, const char *name, const char *cmd, size_t *out, pc_error *err)
{
    for (size_t i = 0; i < s->presets.swatches.n; i++) {
        if (strcmp(s->presets.swatches.v[i].name, name) == 0) {
            *out = i;
            return PC_OK;
        }
    }
    return pc_engine_err_bad_params(err, cmd, "no swatch group \"%s\"", name);
}

static yyjson_mut_val *swatch_json(yyjson_mut_doc *d, const pc_swatch_conv *conv, const pc_swatch *sw)
{
    yyjson_mut_val *v = pc_swatch_color_to_json(d, &sw->color);
    yyjson_mut_obj_add_strcpy(d, v, "name", sw->name);
    float rgb[3];
    pc_swatch_conv_rgb(conv, &sw->color, rgb);
    char hex[8];
    pc_swatch_hex(rgb, hex);
    yyjson_mut_obj_add_strcpy(d, v, "hex", hex);
    if (sw->kind != PC_SWATCH_PROCESS) {
        yyjson_mut_obj_add_strcpy(d, v, "kind", pc_swatch_kind_id(sw->kind));
    }
    return v;
}

/* ---- swatch array helpers ----------------------------------------------------------------- */

static pc_status group_grow(pc_preset_group *gr, size_t extra, pc_error *err)
{
    if (gr->n + extra <= gr->cap) {
        return PC_OK;
    }
    size_t cap = gr->cap == 0 ? 4 : gr->cap * 2;
    while (cap < gr->n + extra) {
        cap *= 2;
    }
    pc_swatch *v = realloc(gr->items, cap * sizeof(pc_swatch));
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "swatches");
    }
    gr->items = v;
    gr->cap = cap;
    return PC_OK;
}

static pc_status group_insert_sw(pc_preset_group *gr, size_t at, pc_swatch sw, pc_error *err)
{
    if (group_grow(gr, 1, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_swatch *items = gr->items;
    if (at > gr->n) {
        at = gr->n;
    }
    memmove(&items[at + 1], &items[at], (gr->n - at) * sizeof(pc_swatch));
    items[at] = sw;
    gr->n++;
    return PC_OK;
}

static pc_swatch group_take_sw(pc_preset_group *gr, size_t at)
{
    pc_swatch *items = gr->items;
    pc_swatch sw = items[at];
    memmove(&items[at], &items[at + 1], (gr->n - at - 1) * sizeof(pc_swatch));
    gr->n--;
    return sw;
}

/* ---- commands ----------------------------------------------------------------------------- */

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_swatch_conv *conv = pc_swatch_conv_new(s);
    if (conv == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "swatches");
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_swatch_conv_free(conv);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *groups = yyjson_mut_arr(d);
    for (size_t i = 0; i < s->presets.swatches.n; i++) {
        const pc_preset_group *gr = &s->presets.swatches.v[i];
        yyjson_mut_val *go = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, go, "name", gr->name);
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        const pc_swatch *items = gr->items;
        for (size_t j = 0; j < gr->n; j++) {
            yyjson_mut_arr_append(arr, swatch_json(d, conv, &items[j]));
        }
        yyjson_mut_obj_add_val(d, go, "swatches", arr);
        yyjson_mut_arr_append(groups, go);
    }
    yyjson_mut_obj_add_val(d, root, "groups", groups);
    yyjson_mut_obj_add_uint(d, root, "count", total(&s->presets.swatches));
    yyjson_mut_doc_set_root(d, root);
    pc_swatch_conv_free(conv);
    return PC_OK;
}

static pc_status run_add(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.add";
    pc_swatch_color color;
    yyjson_val *cv = pc_json_get(p, "color");
    if (cv == NULL || yyjson_is_null(cv)) {
        color.model = PC_SWATCH_RGB;
        for (int i = 0; i < 3; i++) {
            float v = s->tools.foreground[i];
            color.v[i] = !isfinite(v) ? 0.0f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
        }
        color.v[3] = 0.0f;
    } else {
        char msg[256];
        if (!pc_swatch_color_from_param(cv, &color, msg)) {
            return pc_engine_err_bad_params(err, cmd, "%s", msg);
        }
    }
    char kb[64];
    pc_swatch_kind kind = PC_SWATCH_PROCESS;
    const char *ks = str_param(p, "kind", kb, sizeof kb);
    if (ks != NULL && !pc_swatch_kind_parse(ks, &kind)) {
        return pc_engine_err_bad_params(err, cmd, "unknown kind `%s` (process|global|spot)", ks);
    }
    if (total(&s->presets.swatches) >= PC_SWATCH_MAX_SWATCHES) {
        return pc_engine_err_bad_params(err, cmd, "the Swatches panel is full (%d swatches)",
                                        PC_SWATCH_MAX_SWATCHES);
    }
    bool have_group = false, have_name = false;
    char *group = NULL, *explicit = NULL;
    if (name_param(p, "group", cmd, &have_group, &group, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (name_param(p, "name", cmd, &have_name, &explicit, err) != PC_OK) {
        free(group);
        return PC_ERR_INVALID;
    }
    char *name = NULL;
    if (have_name) {
        name = pc_preset_unique_name(&s->presets.swatches, explicit, OPS());
    } else {
        /* "Color Swatch N" */
        for (unsigned i = 1; i < 10000000u; i++) {
            char buf[32];
            (void)snprintf(buf, sizeof buf, "Color Swatch %u", i);
            size_t gi = 0, ii = 0;
            if (!find_swatch(&s->presets.swatches, buf, NULL, &gi, &ii)) {
                name = dup_str(buf);
                break;
            }
        }
    }
    free(explicit);
    if (name == NULL) {
        free(group);
        return pc_fail(err, PC_ERR_NOMEM, "name");
    }
    /* resolve group index */
    size_t gi;
    if (have_group) {
        bool found = false;
        for (size_t i = 0; i < s->presets.swatches.n; i++) {
            if (strcmp(s->presets.swatches.v[i].name, group) == 0) {
                gi = i;
                found = true;
                break;
            }
        }
        if (!found) {
            if (s->presets.swatches.n >= PC_SWATCH_MAX_GROUPS) {
                free(group);
                free(name);
                return pc_engine_err_bad_params(err, cmd, "too many swatch groups");
            }
            if (pc_preset_groups_push(&s->presets.swatches, group, err) == NULL) {
                free(group);
                free(name);
                return PC_ERR_NOMEM;
            }
            gi = s->presets.swatches.n - 1;
        }
    } else {
        if (s->presets.swatches.n == 0) {
            if (pc_preset_groups_push(&s->presets.swatches, "Swatches", err) == NULL) {
                free(name);
                return PC_ERR_NOMEM;
            }
        }
        gi = 0;
    }
    free(group);
    pc_preset_group *gr = &s->presets.swatches.v[gi];
    size_t at = gr->n;
    yyjson_val *iv = pc_json_get(p, "index");
    if (iv != NULL && !yyjson_is_null(iv)) {
        uint64_t idx;
        if (!pc_json_as_u64(iv, &idx)) {
            free(name);
            return pc_engine_err_bad_params(err, cmd, "`index` must be a non-negative integer");
        }
        at = idx < gr->n ? (size_t)idx : gr->n;
    }
    pc_swatch sw = {name, color, kind};
    if (group_insert_sw(gr, at, sw, err) != PC_OK) {
        free(name);
        return PC_ERR_NOMEM;
    }
    char *gname = dup_str(gr->name);
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(gname);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", name);
    yyjson_mut_obj_add_strcpy(d, root, "group", gname != NULL ? gname : "");
    yyjson_mut_obj_add_uint(d, root, "index", at);
    yyjson_mut_doc_set_root(d, root);
    free(gname);
    return PC_OK;
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.delete";
    /* collect (gi,ii) targets */
    size_t(*tg)[2] = NULL;
    size_t ntg = 0, cap = 0;
    yyjson_val *sw = pc_json_get(p, "swatch");
    pc_status rc = PC_OK;
    char gb[1024];
    const char *group = str_param(p, "group", gb, sizeof gb);
    if (sw != NULL && yyjson_is_arr(sw)) {
        size_t idx, max;
        yyjson_val *el;
        yyjson_arr_foreach(sw, idx, max, el) {
            size_t nl = 0;
            const char *nm = pc_json_as_str(el, &nl);
            if (nm == NULL) {
                rc = pc_engine_err_bad_params(err, cmd, "`swatch` names must be strings");
                goto done;
            }
            char nb[1024];
            size_t n = nl < sizeof nb - 1 ? nl : sizeof nb - 1;
            memcpy(nb, nm, n);
            nb[n] = '\0';
            size_t gi = 0, ii = 0;
            if (!find_swatch(&s->presets.swatches, nb, group, &gi, &ii)) {
                rc = pc_engine_err_bad_params(err, cmd, "no swatch \"%s\" (see swatches.list)", nb);
                goto done;
            }
            if (ntg == cap) {
                size_t nc = cap == 0 ? 8 : cap * 2;
                size_t(*t2)[2] = realloc(tg, nc * sizeof *t2);
                if (t2 == NULL) {
                    rc = pc_fail(err, PC_ERR_NOMEM, "delete");
                    goto done;
                }
                tg = t2;
                cap = nc;
            }
            tg[ntg][0] = gi;
            tg[ntg][1] = ii;
            ntg++;
        }
    } else {
        size_t gi = 0, ii = 0;
        if (locate(s, p, cmd, &gi, &ii, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        tg = malloc(sizeof *tg);
        if (tg == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "delete");
        }
        tg[0][0] = gi;
        tg[0][1] = ii;
        ntg = 1;
    }
    /* sort desc + dedup so earlier positions stay valid */
    for (size_t i = 1; i < ntg; i++) {
        size_t a0 = tg[i][0], a1 = tg[i][1];
        size_t j = i;
        while (j > 0 && (tg[j - 1][0] > a0 || (tg[j - 1][0] == a0 && tg[j - 1][1] > a1))) {
            tg[j][0] = tg[j - 1][0];
            tg[j][1] = tg[j - 1][1];
            j--;
        }
        tg[j][0] = a0;
        tg[j][1] = a1;
    }
    size_t uniq = 0;
    for (size_t i = 0; i < ntg; i++) {
        if (i == 0 || tg[i][0] != tg[uniq - 1][0] || tg[i][1] != tg[uniq - 1][1]) {
            tg[uniq][0] = tg[i][0];
            tg[uniq][1] = tg[i][1];
            uniq++;
        }
    }
    ntg = uniq;
    /* collect names (in original ascending order) before removing */
    char **names = ntg > 0 ? calloc(ntg, sizeof *names) : NULL;
    if (ntg > 0 && names == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "delete");
        goto done;
    }
    /* remove from the back */
    for (size_t k = ntg; k > 0; k--) {
        size_t gi = tg[k - 1][0], ii = tg[k - 1][1];
        if (gi < s->presets.swatches.n && ii < s->presets.swatches.v[gi].n) {
            pc_swatch rem = group_take_sw(&s->presets.swatches.v[gi], ii);
            names[k - 1] = rem.name; /* take ownership of the name */
        }
    }
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        for (size_t i = 0; i < ntg; i++) {
            free(names[i]);
        }
        free(names);
        rc = PC_ERR_NOMEM;
        goto done;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < ntg; i++) {
        if (names[i] != NULL) {
            yyjson_mut_arr_add_strcpy(d, arr, names[i]);
            free(names[i]);
        }
    }
    free(names);
    yyjson_mut_obj_add_val(d, root, "deleted", arr);
    yyjson_mut_doc_set_root(d, root);
    rc = PC_OK;
done:
    free(tg);
    return rc;
}

static pc_status run_rename(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.rename";
    size_t gi = 0, ii = 0;
    if (locate(s, p, cmd, &gi, &ii, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bool have;
    char *to = NULL;
    if (name_param(p, "name", cmd, &have, &to, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (!have) {
        return pc_engine_err_bad_params(err, cmd, "missing `name`");
    }
    pc_swatch *sw = &((pc_swatch *)s->presets.swatches.v[gi].items)[ii];
    size_t dgi, dii;
    if (strcmp(sw->name, to) != 0 && find_swatch(&s->presets.swatches, to, NULL, &dgi, &dii)) {
        pc_status r = pc_engine_err_bad_params(err, cmd, "a swatch named \"%s\" already exists", to);
        free(to);
        return r;
    }
    free(sw->name);
    sw->name = to;
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", sw->name);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_move(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.move";
    size_t gi = 0, ii = 0;
    if (locate(s, p, cmd, &gi, &ii, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bool have_to;
    char *to = NULL;
    if (name_param(p, "to", cmd, &have_to, &to, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    size_t ti = gi;
    if (have_to) {
        pc_status rc = group_of(s, to, cmd, &ti, err);
        free(to);
        if (rc != PC_OK) {
            return rc;
        }
    }
    size_t at = SIZE_MAX;
    bool have_at = false;
    yyjson_val *pv = pc_json_get(p, "position");
    if (pv != NULL && !yyjson_is_null(pv)) {
        uint64_t v;
        if (!pc_json_as_u64(pv, &v)) {
            return pc_engine_err_bad_params(err, cmd, "`position` must be a non-negative integer");
        }
        at = v > SIZE_MAX ? SIZE_MAX : (size_t)v;
        have_at = true;
    }
    pc_swatch sw = group_take_sw(&s->presets.swatches.v[gi], ii);
    pc_preset_group *tg = &s->presets.swatches.v[ti];
    size_t pos = have_at ? (at < tg->n ? at : tg->n) : tg->n;
    if (group_insert_sw(tg, pos, sw, err) != PC_OK) {
        free(sw.name); /* best-effort: swatch lost on OOM */
        return PC_ERR_NOMEM;
    }
    char *gname = dup_str(tg->name);
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(gname);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "group", gname != NULL ? gname : "");
    yyjson_mut_obj_add_uint(d, root, "position", pos);
    yyjson_mut_doc_set_root(d, root);
    free(gname);
    return PC_OK;
}

static pc_status run_new_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.newGroup";
    if (s->presets.swatches.n >= PC_SWATCH_MAX_GROUPS) {
        return pc_engine_err_bad_params(err, cmd, "too many swatch groups");
    }
    bool have;
    char *base = NULL;
    if (name_param(p, "name", cmd, &have, &base, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    char *uniq = pc_preset_unique_group(&s->presets.swatches, have ? base : "Group");
    free(base);
    if (uniq == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "group");
    }
    /* move selected swatches into the new group */
    size_t(*tg)[2] = NULL;
    size_t ntg = 0, cap = 0;
    yyjson_val *swa = pc_json_get(p, "swatches");
    pc_status rc = PC_OK;
    if (swa != NULL) {
        if (!yyjson_is_arr(swa)) {
            free(uniq);
            return pc_engine_err_bad_params(err, cmd, "`swatches` must be a list of names");
        }
        size_t idx, max;
        yyjson_val *el;
        yyjson_arr_foreach(swa, idx, max, el) {
            size_t nl = 0;
            const char *nm = pc_json_as_str(el, &nl);
            if (nm == NULL) {
                rc = pc_engine_err_bad_params(err, cmd, "`swatches` names must be strings");
                goto cleanup;
            }
            char nb[1024];
            size_t n = nl < sizeof nb - 1 ? nl : sizeof nb - 1;
            memcpy(nb, nm, n);
            nb[n] = '\0';
            size_t gi = 0, ii = 0;
            if (!find_swatch(&s->presets.swatches, nb, NULL, &gi, &ii)) {
                rc = pc_engine_err_bad_params(err, cmd, "no swatch \"%s\"", nb);
                goto cleanup;
            }
            if (ntg == cap) {
                size_t nc = cap == 0 ? 8 : cap * 2;
                size_t(*t2)[2] = realloc(tg, nc * sizeof *t2);
                if (t2 == NULL) {
                    rc = pc_fail(err, PC_ERR_NOMEM, "newGroup");
                    goto cleanup;
                }
                tg = t2;
                cap = nc;
            }
            tg[ntg][0] = gi;
            tg[ntg][1] = ii;
            ntg++;
        }
    }
    /* sort desc + dedup */
    for (size_t i = 1; i < ntg; i++) {
        size_t a0 = tg[i][0], a1 = tg[i][1], j = i;
        while (j > 0 && (tg[j - 1][0] > a0 || (tg[j - 1][0] == a0 && tg[j - 1][1] > a1))) {
            tg[j][0] = tg[j - 1][0];
            tg[j][1] = tg[j - 1][1];
            j--;
        }
        tg[j][0] = a0;
        tg[j][1] = a1;
    }
    size_t uniqn = 0;
    for (size_t i = 0; i < ntg; i++) {
        if (i == 0 || tg[i][0] != tg[uniqn - 1][0] || tg[i][1] != tg[uniqn - 1][1]) {
            tg[uniqn][0] = tg[i][0];
            tg[uniqn][1] = tg[i][1];
            uniqn++;
        }
    }
    ntg = uniqn;
    /* take the swatches (back to front keeps indices valid), collect ascending */
    pc_swatch *moved = ntg > 0 ? calloc(ntg, sizeof *moved) : NULL;
    if (ntg > 0 && moved == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "newGroup");
        goto cleanup;
    }
    for (size_t k = ntg; k > 0; k--) {
        moved[k - 1] = group_take_sw(&s->presets.swatches.v[tg[k - 1][0]], tg[k - 1][1]);
    }
    pc_preset_group *gr = pc_preset_groups_push(&s->presets.swatches, uniq, err);
    if (gr == NULL) {
        for (size_t i = 0; i < ntg; i++) {
            free(moved[i].name);
        }
        free(moved);
        rc = PC_ERR_NOMEM;
        goto cleanup;
    }
    for (size_t i = 0; i < ntg; i++) {
        if (group_insert_sw(gr, gr->n, moved[i], err) != PC_OK) {
            for (size_t j = i; j < ntg; j++) {
                free(moved[j].name);
            }
            free(moved);
            rc = PC_ERR_NOMEM;
            goto cleanup;
        }
    }
    free(moved);
    pc_engine_presets_changed(s);
    {
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            goto cleanup;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, root, "group", uniq);
        yyjson_mut_obj_add_uint(d, root, "moved", ntg);
        yyjson_mut_doc_set_root(d, root);
    }
cleanup:
    free(uniq);
    free(tg);
    return rc;
}

static pc_status run_rename_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.renameGroup";
    bool hg, hn;
    char *g = NULL, *to = NULL;
    if (name_param(p, "group", cmd, &hg, &g, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (!hg) {
        return pc_engine_err_bad_params(err, cmd, "missing `group`");
    }
    if (name_param(p, "name", cmd, &hn, &to, err) != PC_OK) {
        free(g);
        return PC_ERR_INVALID;
    }
    if (!hn) {
        free(g);
        return pc_engine_err_bad_params(err, cmd, "missing `name`");
    }
    size_t i = 0;
    pc_status rc = group_of(s, g, cmd, &i, err);
    if (rc != PC_OK) {
        free(g);
        free(to);
        return rc;
    }
    if (strcmp(g, to) != 0) {
        for (size_t k = 0; k < s->presets.swatches.n; k++) {
            if (strcmp(s->presets.swatches.v[k].name, to) == 0) {
                free(g);
                char *msg = to;
                pc_status r = pc_engine_err_bad_params(err, cmd, "a group named \"%s\" already exists", msg);
                free(to);
                return r;
            }
        }
    }
    free(s->presets.swatches.v[i].name);
    s->presets.swatches.v[i].name = to;
    free(g);
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "group", to);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_delete_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.deleteGroup";
    bool hg;
    char *g = NULL;
    if (name_param(p, "group", cmd, &hg, &g, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (!hg) {
        return pc_engine_err_bad_params(err, cmd, "missing `group`");
    }
    bool keep = false;
    yyjson_val *kv = pc_json_get(p, "keepSwatches");
    if (kv != NULL && !yyjson_is_null(kv)) {
        if (!pc_json_as_bool(kv, &keep)) {
            free(g);
            return pc_engine_err_bad_params(err, cmd, "`keepSwatches` must be true or false");
        }
    }
    size_t i = 0;
    pc_status rc = group_of(s, g, cmd, &i, err);
    if (rc != PC_OK) {
        free(g);
        return rc;
    }
    if (keep && s->presets.swatches.n < 2) {
        free(g);
        return pc_engine_err_bad_params(err, cmd, "there is no other group to keep the swatches in");
    }
    pc_preset_group removed = s->presets.swatches.v[i];
    size_t tail = s->presets.swatches.n - i - 1;
    if (tail > 0) {
        memmove(&s->presets.swatches.v[i], &s->presets.swatches.v[i + 1], tail * sizeof(pc_preset_group));
    }
    s->presets.swatches.n--;
    size_t n = removed.n;
    if (keep) {
        size_t into = i == 0 ? 0 : i - 1;
        if (into >= s->presets.swatches.n && s->presets.swatches.n > 0) {
            into = s->presets.swatches.n - 1;
        }
        if (s->presets.swatches.n > 0) {
            pc_preset_group *t = &s->presets.swatches.v[into];
            pc_swatch *src = removed.items;
            for (size_t k = 0; k < removed.n; k++) {
                if (group_insert_sw(t, t->n, src[k], err) != PC_OK) {
                    /* drop the rest on OOM */
                    for (size_t j = k; j < removed.n; j++) {
                        free(src[j].name);
                    }
                    break;
                }
            }
        } else {
            for (size_t k = 0; k < removed.n; k++) {
                free(((pc_swatch *)removed.items)[k].name);
            }
        }
        free(removed.items);
        free(removed.name);
    } else {
        for (size_t k = 0; k < removed.n; k++) {
            free(((pc_swatch *)removed.items)[k].name);
        }
        free(removed.items);
        free(removed.name);
    }
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(g);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "group", g);
    yyjson_mut_obj_add_uint(d, root, keep ? "kept" : "deleted", n);
    yyjson_mut_doc_set_root(d, root);
    free(g);
    return PC_OK;
}

static pc_status run_use(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.use";
    size_t gi = 0, ii = 0;
    if (locate(s, p, cmd, &gi, &ii, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    char tb[32];
    const char *target = str_param(p, "target", tb, sizeof tb);
    if (target == NULL) {
        target = "foreground";
    }
    const pc_swatch *sw = &((const pc_swatch *)s->presets.swatches.v[gi].items)[ii];
    pc_swatch_conv *conv = pc_swatch_conv_new(s);
    if (conv == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "swatches");
    }
    float rgb[3];
    pc_swatch_conv_rgb(conv, &sw->color, rgb);
    pc_swatch_conv_free(conv);
    float c[4] = {rgb[0], rgb[1], rgb[2], 1.0f};
    if (strcmp(target, "foreground") == 0) {
        memcpy(s->tools.foreground, c, sizeof c);
    } else if (strcmp(target, "background") == 0) {
        memcpy(s->tools.background, c, sizeof c);
    } else {
        return pc_engine_err_bad_params(err, cmd, "unknown target `%s` (foreground|background)", target);
    }
    char hex[8];
    pc_swatch_hex(rgb, hex);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "swatch", sw->name);
    yyjson_mut_obj_add_strcpy(d, root, "target", target);
    yyjson_mut_obj_add_strcpy(d, root, "hex", hex);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_reset(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.reset";
    bool append = false;
    yyjson_val *av = pc_json_get(p, "append");
    if (av != NULL && !yyjson_is_null(av) && !pc_json_as_bool(av, &append)) {
        return pc_engine_err_bad_params(err, cmd, "`append` must be true or false");
    }
    pc_preset_groups b;
    if (pc_swatches_builtin(&b, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (append) {
        if (total(&s->presets.swatches) + total(&b) > PC_SWATCH_MAX_SWATCHES ||
            s->presets.swatches.n + b.n > PC_SWATCH_MAX_GROUPS) {
            pc_preset_groups_fini(&b, OPS());
            return pc_engine_err_bad_params(err, cmd, "the Swatches panel is full");
        }
        for (size_t i = 0; i < b.n; i++) {
            char *gname = pc_preset_unique_group(&s->presets.swatches, b.v[i].name);
            pc_preset_group *gr = pc_preset_groups_push(&s->presets.swatches, gname != NULL ? gname : b.v[i].name, err);
            free(gname);
            if (gr == NULL) {
                pc_preset_groups_fini(&b, OPS());
                return PC_ERR_NOMEM;
            }
            pc_swatch *src = b.v[i].items;
            for (size_t j = 0; j < b.v[i].n; j++) {
                char *un = pc_preset_unique_name(&s->presets.swatches, src[j].name, OPS());
                if (un != NULL) {
                    free(src[j].name);
                    src[j].name = un;
                }
                if (group_insert_sw(gr, gr->n, src[j], err) != PC_OK) {
                    for (size_t k = j + 1; k < b.v[i].n; k++) {
                        free(src[k].name);
                    }
                    free(b.v[i].items);
                    b.v[i].items = NULL;
                    b.v[i].n = 0;
                    pc_preset_groups_fini(&b, OPS());
                    return PC_ERR_NOMEM;
                }
            }
            free(b.v[i].items);
            b.v[i].items = NULL;
            b.v[i].n = 0;
        }
        pc_preset_groups_fini(&b, OPS());
    } else {
        pc_preset_groups_fini(&s->presets.swatches, OPS());
        s->presets.swatches = b;
    }
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "groups", s->presets.swatches.n);
    yyjson_mut_obj_add_uint(d, root, "count", total(&s->presets.swatches));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- .aco / .ase conversions -------------------------------------------------------------- */

static float unit(float v)
{
    return !isfinite(v) ? 0.0f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
}
static uint16_t q16(float v)
{
    return (uint16_t)lroundf(unit(v) * 65535.0f);
}

static void from_aco(const pc_psd_aco_color *c, pc_swatch_color *out)
{
    memset(out, 0, sizeof *out);
    switch (c->kind) {
    case PC_PSD_ACO_RGB:
        out->model = PC_SWATCH_RGB;
        for (int i = 0; i < 3; i++) {
            out->v[i] = (float)c->v[i] / 65535.0f;
        }
        break;
    case PC_PSD_ACO_HSB:
        out->model = PC_SWATCH_HSB;
        out->v[0] = (float)c->v[0] / 65535.0f * 360.0f;
        out->v[1] = (float)c->v[1] / 65535.0f;
        out->v[2] = (float)c->v[2] / 65535.0f;
        break;
    case PC_PSD_ACO_CMYK:
        out->model = PC_SWATCH_CMYK;
        for (int i = 0; i < 4; i++) {
            out->v[i] = (float)c->v[i] / 65535.0f;
        }
        break;
    case PC_PSD_ACO_LAB:
        out->model = PC_SWATCH_LAB;
        out->v[0] = (float)(c->v[0] > 10000 ? 10000 : c->v[0]) / 100.0f;
        out->v[1] = (float)(int16_t)c->v[1] / 100.0f;
        out->v[2] = (float)(int16_t)c->v[2] / 100.0f;
        break;
    case PC_PSD_ACO_GRAY:
        out->model = PC_SWATCH_GRAY;
        out->v[0] = 1.0f - (float)(c->v[0] > 10000 ? 10000 : c->v[0]) / 10000.0f;
        break;
    default:
        out->model = PC_SWATCH_BOOK;
        out->book_space = c->space;
        for (int i = 0; i < 4; i++) {
            out->book_values[i] = c->v[i];
        }
        break;
    }
}

static pc_psd_aco_color to_aco(const pc_swatch_color *c)
{
    pc_psd_aco_color o;
    memset(&o, 0, sizeof o);
    switch (c->model) {
    case PC_SWATCH_RGB:
        o.kind = PC_PSD_ACO_RGB;
        for (int i = 0; i < 3; i++) {
            o.v[i] = q16(c->v[i]);
        }
        break;
    case PC_SWATCH_HSB: {
        o.kind = PC_PSD_ACO_HSB;
        float h = isfinite(c->v[0]) ? c->v[0] - 360.0f * floorf(c->v[0] / 360.0f) : 0.0f;
        double hv = round((double)(h / 360.0f) * 65535.0);
        o.v[0] = (uint16_t)(hv > 65535.0 ? 65535.0 : hv);
        o.v[1] = q16(c->v[1]);
        o.v[2] = q16(c->v[2]);
        break;
    }
    case PC_SWATCH_CMYK:
        o.kind = PC_PSD_ACO_CMYK;
        for (int i = 0; i < 4; i++) {
            o.v[i] = q16(c->v[i]);
        }
        break;
    case PC_SWATCH_LAB: {
        o.kind = PC_PSD_ACO_LAB;
        double l = round((double)(isfinite(c->v[0]) ? c->v[0] : 0.0f) * 100.0);
        l = l < 0 ? 0 : (l > 10000 ? 10000 : l);
        o.v[0] = (uint16_t)l;
        for (int k = 1; k <= 2; k++) {
            double ab = isfinite(c->v[k]) ? round((double)c->v[k] * 100.0) : 0.0;
            ab = ab < -12800 ? -12800 : (ab > 12700 ? 12700 : ab);
            o.v[k] = (uint16_t)(int16_t)ab;
        }
        break;
    }
    case PC_SWATCH_GRAY:
        o.kind = PC_PSD_ACO_GRAY;
        o.v[0] = (uint16_t)lround((double)(1.0f - unit(c->v[0])) * 10000.0);
        break;
    default:
        o.kind = PC_PSD_ACO_OTHER;
        o.space = c->book_space;
        for (int i = 0; i < 4; i++) {
            o.v[i] = c->book_values[i];
        }
        break;
    }
    return o;
}

static bool from_ase(const pc_psd_ase_color *c, pc_swatch_color *out)
{
    memset(out, 0, sizeof *out);
    switch (c->model) {
    case PC_PSD_ASE_RGB:
        out->model = PC_SWATCH_RGB;
        for (int i = 0; i < 3; i++) {
            out->v[i] = c->c[i];
        }
        return true;
    case PC_PSD_ASE_CMYK:
        out->model = PC_SWATCH_CMYK;
        for (int i = 0; i < 4; i++) {
            out->v[i] = c->c[i];
        }
        return true;
    case PC_PSD_ASE_LAB:
        out->model = PC_SWATCH_LAB;
        out->v[0] = c->c[0] > 1.0f ? c->c[0] : c->c[0] * 100.0f;
        out->v[1] = c->c[1];
        out->v[2] = c->c[2];
        return true;
    case PC_PSD_ASE_GRAY:
        out->model = PC_SWATCH_GRAY;
        out->v[0] = 1.0f - c->c[0];
        return true;
    default:
        return false;
    }
}

static bool to_ase(const pc_swatch_color *c, pc_psd_ase_color *out)
{
    memset(out, 0, sizeof *out);
    switch (c->model) {
    case PC_SWATCH_RGB:
        out->model = PC_PSD_ASE_RGB;
        for (int i = 0; i < 3; i++) {
            out->c[i] = c->v[i];
        }
        return true;
    case PC_SWATCH_HSB: {
        out->model = PC_PSD_ASE_RGB;
        float rgb[3];
        pc_swatch_hsb_to_rgb(c->v, rgb);
        for (int i = 0; i < 3; i++) {
            out->c[i] = rgb[i];
        }
        return true;
    }
    case PC_SWATCH_CMYK:
        out->model = PC_PSD_ASE_CMYK;
        for (int i = 0; i < 4; i++) {
            out->c[i] = c->v[i];
        }
        return true;
    case PC_SWATCH_LAB:
        out->model = PC_PSD_ASE_LAB;
        out->c[0] = c->v[0] / 100.0f;
        out->c[1] = c->v[1];
        out->c[2] = c->v[2];
        return true;
    case PC_SWATCH_GRAY:
        out->model = PC_PSD_ASE_GRAY;
        out->c[0] = 1.0f - c->v[0];
        return true;
    default:
        return false;
    }
}

static pc_swatch_kind kind_from_ase(pc_psd_ase_kind k)
{
    return k == PC_PSD_ASE_GLOBAL ? PC_SWATCH_GLOBAL : (k == PC_PSD_ASE_SPOT ? PC_SWATCH_SPOT : PC_SWATCH_PROCESS);
}
static pc_psd_ase_kind kind_to_ase(pc_swatch_kind k)
{
    return k == PC_SWATCH_GLOBAL ? PC_PSD_ASE_GLOBAL : (k == PC_SWATCH_SPOT ? PC_PSD_ASE_SPOT : PC_PSD_ASE_PROCESS);
}

typedef enum { FMT_ACO, FMT_ASE } sw_format;

static bool format_parse(const char *s, sw_format *out)
{
    const char *p = s;
    while (*p == '.') {
        p++;
    }
    if (strcasecmp(p, "aco") == 0) {
        *out = FMT_ACO;
        return true;
    }
    if (strcasecmp(p, "ase") == 0) {
        *out = FMT_ASE;
        return true;
    }
    return false;
}

static const char *fallback_name(size_t i, const char *name, char *buf, size_t cap)
{
    const char *s = name == NULL ? "" : name;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        (void)snprintf(buf, cap, "Swatch %zu", i + 1);
        return buf;
    }
    return name; /* trimmed+clipped by the caller via clip() */
}

/* Parse a swatch file into groups; *warn is an owned array of owned strings. */
static pc_status read_file(const uint8_t *bytes, size_t len, sw_format fmt, const char *group,
                           pc_preset_groups *out, char ***warn, size_t *nwarn, char errbuf[256])
{
    pc_preset_groups_init(out);
    *warn = NULL;
    *nwarn = 0;
    pc_error e = {0};
    if (fmt == FMT_ACO) {
        pc_psd_aco f;
        if (pc_psd_aco_parse(bytes, len, &f, &e) != PC_OK) {
            (void)snprintf(errbuf, 256, "not a readable .aco swatch file: %.180s", e.msg[0] ? e.msg : "parse error");
            return PC_ERR_FORMAT;
        }
        size_t books = 0;
        for (size_t i = 0; i < f.n; i++) {
            if (f.v[i].color.kind == PC_PSD_ACO_OTHER) {
                books++;
            }
        }
        pc_preset_group *gr = pc_preset_groups_push(out, group, &e);
        if (gr == NULL) {
            pc_psd_aco_fini(&f);
            (void)snprintf(errbuf, 256, "out of memory");
            return PC_ERR_NOMEM;
        }
        for (size_t i = 0; i < f.n; i++) {
            char fb[64];
            const char *nm = fallback_name(i, pc_psd_str_c(&f.v[i].name), fb, sizeof fb);
            char *cn = clip(nm);
            pc_swatch sw = {cn, {0}, PC_SWATCH_PROCESS};
            from_aco(&f.v[i].color, &sw.color);
            if (cn == NULL || group_insert_sw(gr, gr->n, sw, &e) != PC_OK) {
                free(cn);
                pc_psd_aco_fini(&f);
                pc_preset_groups_fini(out, pc_swatch_ops());
                pc_preset_groups_init(out);
                (void)snprintf(errbuf, 256, "out of memory");
                return PC_ERR_NOMEM;
            }
        }
        pc_psd_aco_fini(&f);
        if (books > 0) {
            char w[256];
            (void)snprintf(w, sizeof w,
                           "%zu swatches use a colour book or colour space PhotoCraft can't show; they are "
                           "kept (shown gray) and saved back unchanged",
                           books);
            char **wv = malloc(sizeof *wv);
            if (wv != NULL && (wv[0] = dup_str(w)) != NULL) {
                *warn = wv;
                *nwarn = 1;
            } else {
                free(wv);
            }
        }
    } else {
        pc_psd_ase f;
        if (pc_psd_ase_parse(bytes, len, &f, &e) != PC_OK) {
            (void)snprintf(errbuf, 256, "not a readable .ase swatch file: %.180s", e.msg[0] ? e.msg : "parse error");
            return PC_ERR_FORMAT;
        }
        size_t skipped = 0, n = 0;
        /* loose colours go into group 0 */
        pc_preset_group *loose = pc_preset_groups_push(out, group, &e);
        bool used_loose = false;
        if (loose == NULL) {
            pc_psd_ase_fini(&f);
            (void)snprintf(errbuf, 256, "out of memory");
            return PC_ERR_NOMEM;
        }
        for (size_t i = 0; i < f.n; i++) {
            if (!f.entries[i].group) {
                pc_swatch_color col;
                if (from_ase(&f.entries[i].swatch.color, &col)) {
                    char fb[64];
                    const char *nm = fallback_name(n, pc_psd_str_c(&f.entries[i].swatch.name), fb, sizeof fb);
                    char *cn = clip(nm);
                    pc_swatch sw = {cn, col, kind_from_ase(f.entries[i].swatch.kind)};
                    if (cn == NULL || group_insert_sw(loose, loose->n, sw, &e) != PC_OK) {
                        free(cn);
                        goto ase_oom;
                    }
                    used_loose = true;
                } else {
                    skipped++;
                }
                n++;
            } else {
                const char *gn = pc_psd_str_c(&f.entries[i].name);
                const char *gp = gn;
                while (*gp == ' ' || *gp == '\t') {
                    gp++;
                }
                char *gname = clip(*gp == '\0' ? group : gn);
                pc_preset_group *gr = pc_preset_groups_push(out, gname != NULL ? gname : group, &e);
                free(gname);
                if (gr == NULL) {
                    goto ase_oom;
                }
                for (size_t j = 0; j < f.entries[i].n; j++) {
                    pc_swatch_color col;
                    if (from_ase(&f.entries[i].swatches[j].color, &col)) {
                        char fb[64];
                        const char *nm = fallback_name(n, pc_psd_str_c(&f.entries[i].swatches[j].name), fb, sizeof fb);
                        char *cn = clip(nm);
                        pc_swatch sw = {cn, col, kind_from_ase(f.entries[i].swatches[j].kind)};
                        if (cn == NULL || group_insert_sw(gr, gr->n, sw, &e) != PC_OK) {
                            free(cn);
                            goto ase_oom;
                        }
                    } else {
                        skipped++;
                    }
                    n++;
                }
            }
        }
        pc_psd_ase_fini(&f);
        /* The loose group is group 0; if it stayed empty and there are other groups, drop it,
         * unless there are no groups at all (keep one). */
        if (!used_loose && out->n > 1) {
            free(out->v[0].items);
            free(out->v[0].name);
            memmove(&out->v[0], &out->v[1], (out->n - 1) * sizeof(pc_preset_group));
            out->n--;
        }
        if (skipped > 0) {
            char w[128];
            (void)snprintf(w, sizeof w, "%zu swatches in an unknown colour model were skipped", skipped);
            char **wv = malloc(sizeof *wv);
            if (wv != NULL && (wv[0] = dup_str(w)) != NULL) {
                *warn = wv;
                *nwarn = 1;
            } else {
                free(wv);
            }
        }
        if (false) {
        ase_oom:
            pc_psd_ase_fini(&f);
            pc_preset_groups_fini(out, pc_swatch_ops());
            pc_preset_groups_init(out);
            (void)snprintf(errbuf, 256, "out of memory");
            return PC_ERR_NOMEM;
        }
    }
    if (total(out) == 0) {
        pc_preset_groups_fini(out, pc_swatch_ops());
        pc_preset_groups_init(out);
        (void)snprintf(errbuf, 256, "the file holds no swatches");
        return PC_ERR_FORMAT;
    }
    return PC_OK;
}

static void free_warn(char **w, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(w[i]);
    }
    free(w);
}

static pc_status run_import(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.import";
    pc_buf bytes;
    char stem[256];
    if (pc_engine_preset_file_bytes(p, cmd, ".aco or .ase file", &bytes, stem, sizeof stem, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    sw_format fmt;
    char fb[16];
    const char *fs = str_param(p, "format", fb, sizeof fb);
    if (fs != NULL) {
        if (!format_parse(fs, &fmt)) {
            pc_buf_fini(&bytes);
            return pc_engine_err_bad_params(err, cmd, "unknown format `%s` (aco|ase)", fs);
        }
    } else if (bytes.len >= 4 && memcmp(bytes.data, "ASEF", 4) == 0) {
        fmt = FMT_ASE;
    } else if (bytes.len >= 2 && bytes.data[0] == 0 && (bytes.data[1] == 1 || bytes.data[1] == 2)) {
        fmt = FMT_ACO;
    } else {
        /* fall back to the file extension of `path` */
        char pb[4096];
        const char *ps = str_param(p, "path", pb, sizeof pb);
        const char *dot = ps != NULL ? strrchr(ps, '.') : NULL;
        if (dot == NULL || !format_parse(dot + 1, &fmt)) {
            pc_buf_fini(&bytes);
            return pc_engine_err_bad_params(err, cmd, "not an .aco or .ase swatch file");
        }
    }
    char mb[16];
    const char *mode = str_param(p, "mode", mb, sizeof mb);
    if (mode == NULL) {
        mode = "append";
    }
    bool replace;
    if (strcmp(mode, "append") == 0) {
        replace = false;
    } else if (strcmp(mode, "replace") == 0) {
        replace = true;
    } else {
        pc_buf_fini(&bytes);
        return pc_engine_err_bad_params(err, cmd, "unknown mode `%s` (append|replace)", mode);
    }
    bool hg;
    char *gp = NULL;
    if (name_param(p, "group", cmd, &hg, &gp, err) != PC_OK) {
        pc_buf_fini(&bytes);
        return PC_ERR_INVALID;
    }
    char group[512];
    if (hg) {
        (void)snprintf(group, sizeof group, "%s", gp);
    } else if (stem[0] != '\0') {
        char *cs = clip(stem);
        (void)snprintf(group, sizeof group, "%s", cs != NULL ? cs : "Imported Swatches");
        free(cs);
    } else {
        (void)snprintf(group, sizeof group, "Imported Swatches");
    }
    free(gp);
    pc_preset_groups parsed;
    char **warn = NULL;
    size_t nwarn = 0;
    char errbuf[256];
    pc_status rc = read_file(bytes.data, bytes.len, fmt, group, &parsed, &warn, &nwarn, errbuf);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        return pc_engine_err_bad_params(err, cmd, "%s", errbuf);
    }
    size_t base_sw = replace ? 0 : total(&s->presets.swatches);
    size_t base_gr = replace ? 0 : s->presets.swatches.n;
    if (base_sw + total(&parsed) > PC_SWATCH_MAX_SWATCHES || base_gr + parsed.n > PC_SWATCH_MAX_GROUPS) {
        pc_preset_groups_fini(&parsed, OPS());
        free_warn(warn, nwarn);
        return pc_engine_err_bad_params(err, cmd, "too many swatches: the panel holds at most %d in %d groups",
                                        PC_SWATCH_MAX_SWATCHES, PC_SWATCH_MAX_GROUPS);
    }
    if (replace) {
        pc_preset_groups_fini(&s->presets.swatches, OPS());
        pc_preset_groups_init(&s->presets.swatches);
    }
    size_t count = total(&parsed);
    /* append each parsed group, making names unique across the library */
    char **gnames = parsed.n > 0 ? calloc(parsed.n, sizeof *gnames) : NULL;
    for (size_t i = 0; i < parsed.n; i++) {
        char *gname = pc_preset_unique_group(&s->presets.swatches, parsed.v[i].name);
        pc_preset_group *gr = pc_preset_groups_push(&s->presets.swatches, gname != NULL ? gname : parsed.v[i].name, err);
        if (gnames != NULL) {
            gnames[i] = dup_str(gname != NULL ? gname : parsed.v[i].name);
        }
        free(gname);
        if (gr == NULL) {
            free_warn(warn, nwarn);
            pc_preset_groups_fini(&parsed, OPS());
            if (gnames) {
                for (size_t k = 0; k < parsed.n; k++) {
                    free(gnames[k]);
                }
                free(gnames);
            }
            return PC_ERR_NOMEM;
        }
        pc_swatch *src = parsed.v[i].items;
        for (size_t j = 0; j < parsed.v[i].n; j++) {
            char *un = pc_preset_unique_name(&s->presets.swatches, src[j].name, OPS());
            if (un != NULL) {
                free(src[j].name);
                src[j].name = un;
            }
            if (group_insert_sw(gr, gr->n, src[j], err) != PC_OK) {
                for (size_t k = j + 1; k < parsed.v[i].n; k++) {
                    free(src[k].name);
                }
                free(parsed.v[i].items);
                parsed.v[i].items = NULL;
                parsed.v[i].n = 0;
                free_warn(warn, nwarn);
                pc_preset_groups_fini(&parsed, OPS());
                if (gnames) {
                    for (size_t k = 0; k < parsed.n; k++) {
                        free(gnames[k]);
                    }
                    free(gnames);
                }
                return PC_ERR_NOMEM;
            }
        }
        free(parsed.v[i].items);
        parsed.v[i].items = NULL;
        parsed.v[i].n = 0;
    }
    for (size_t i = 0; i < parsed.n; i++) {
        free(parsed.v[i].name);
    }
    free(parsed.v);
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free_warn(warn, nwarn);
        if (gnames) {
            for (size_t k = 0; k < parsed.n; k++) {
                free(gnames[k]);
            }
            free(gnames);
        }
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "format", fmt == FMT_ACO ? "aco" : "ase");
    yyjson_mut_obj_add_strcpy(d, root, "mode", mode);
    yyjson_mut_val *garr = yyjson_mut_arr(d);
    for (size_t i = 0; i < parsed.n; i++) {
        yyjson_mut_arr_add_strcpy(d, garr, gnames != NULL && gnames[i] != NULL ? gnames[i] : "");
    }
    yyjson_mut_obj_add_val(d, root, "groups", garr);
    yyjson_mut_obj_add_uint(d, root, "count", count);
    yyjson_mut_val *warr = yyjson_mut_arr(d);
    for (size_t i = 0; i < nwarn; i++) {
        yyjson_mut_arr_add_strcpy(d, warr, warn[i]);
    }
    yyjson_mut_obj_add_val(d, root, "warnings", warr);
    yyjson_mut_doc_set_root(d, root);
    if (gnames) {
        for (size_t k = 0; k < parsed.n; k++) {
            free(gnames[k]);
        }
        free(gnames);
    }
    free_warn(warn, nwarn);
    return PC_OK;
}

static pc_status run_export(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "swatches.export";
    char pathb[4096];
    const char *path = str_param(p, "path", pathb, sizeof pathb);
    sw_format fmt = FMT_ACO;
    char fb[16];
    const char *fs = str_param(p, "format", fb, sizeof fb);
    if (fs != NULL) {
        if (!format_parse(fs, &fmt)) {
            return pc_engine_err_bad_params(err, cmd, "unknown format `%s` (aco|ase)", fs);
        }
    } else if (path != NULL) {
        const char *dot = strrchr(path, '.');
        sw_format ef;
        if (dot != NULL && format_parse(dot + 1, &ef)) {
            fmt = ef;
        }
    }
    /* build the subset to export (a copy we can filter) */
    pc_preset_groups ex;
    pc_preset_groups_init(&ex);
    bool hg;
    char *gsel = NULL;
    if (name_param(p, "group", cmd, &hg, &gsel, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (hg) {
        size_t gi = 0;
        pc_status rc = group_of(s, gsel, cmd, &gi, err);
        free(gsel);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_preset_groups_clone(&(pc_preset_groups){&s->presets.swatches.v[gi], 1, 1}, &ex, OPS(), err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    } else {
        free(gsel);
        if (pc_preset_groups_clone(&s->presets.swatches, &ex, OPS(), err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    /* filter by `swatches` names */
    yyjson_val *want = pc_json_get(p, "swatches");
    if (want != NULL) {
        if (!yyjson_is_arr(want)) {
            pc_preset_groups_fini(&ex, OPS());
            return pc_engine_err_bad_params(err, cmd, "`swatches` must be a list of names");
        }
        /* verify all names exist in ex */
        size_t idx, max;
        yyjson_val *el;
        yyjson_arr_foreach(want, idx, max, el) {
            size_t nl = 0;
            const char *nm = pc_json_as_str(el, &nl);
            if (nm == NULL) {
                pc_preset_groups_fini(&ex, OPS());
                return pc_engine_err_bad_params(err, cmd, "`swatches` names must be strings");
            }
            char nb[1024];
            size_t n = nl < sizeof nb - 1 ? nl : sizeof nb - 1;
            memcpy(nb, nm, n);
            nb[n] = '\0';
            size_t gi = 0, ii = 0;
            if (!find_swatch(&ex, nb, NULL, &gi, &ii)) {
                pc_preset_groups_fini(&ex, OPS());
                return pc_engine_err_bad_params(err, cmd, "no swatch \"%s\"", nb);
            }
        }
        /* retain only wanted swatches */
        for (size_t i = 0; i < ex.n; i++) {
            pc_swatch *items = ex.v[i].items;
            size_t w = 0;
            for (size_t j = 0; j < ex.v[i].n; j++) {
                bool keep = false;
                size_t widx, wmax;
                yyjson_val *wel;
                yyjson_arr_foreach(want, widx, wmax, wel) {
                    size_t wl = 0;
                    const char *wn = pc_json_as_str(wel, &wl);
                    if (wn != NULL && strlen(items[j].name) == wl && memcmp(items[j].name, wn, wl) == 0) {
                        keep = true;
                        break;
                    }
                }
                if (keep) {
                    items[w++] = items[j];
                } else {
                    free(items[j].name);
                }
            }
            ex.v[i].n = w;
        }
        /* drop empty groups */
        size_t w = 0;
        for (size_t i = 0; i < ex.n; i++) {
            if (ex.v[i].n > 0) {
                ex.v[w++] = ex.v[i];
            } else {
                free(ex.v[i].items);
                free(ex.v[i].name);
            }
        }
        ex.n = w;
    }
    if (total(&ex) == 0) {
        pc_preset_groups_fini(&ex, OPS());
        return pc_engine_err_bad_params(err, cmd, "no swatches to export");
    }
    /* encode */
    pc_buf bytes;
    pc_buf_init(&bytes);
    size_t count = 0;
    char **warn = NULL;
    size_t nwarn = 0;
    pc_error e = {0};
    if (fmt == FMT_ACO) {
        size_t n = total(&ex);
        pc_psd_aco_swatch *arr = n > 0 ? calloc(n, sizeof *arr) : NULL;
        if (arr == NULL) { /* total(&ex) > 0 here, so n > 0 */
            pc_preset_groups_fini(&ex, OPS());
            return pc_fail(err, PC_ERR_NOMEM, "export");
        }
        size_t w = 0;
        bool nonprocess = false;
        for (size_t i = 0; i < ex.n; i++) {
            pc_swatch *items = ex.v[i].items;
            for (size_t j = 0; j < ex.v[i].n; j++) {
                (void)pc_psd_str_set(&arr[w].name, items[j].name, strlen(items[j].name), &e);
                arr[w].color = to_aco(&items[j].color);
                if (items[j].kind != PC_SWATCH_PROCESS) {
                    nonprocess = true;
                }
                w++;
            }
        }
        count = w;
        pc_psd_bytes ob;
        memset(&ob, 0, sizeof ob);
        pc_status wr = pc_psd_aco_write(arr, w, &ob, &e);
        for (size_t i = 0; i < w; i++) {
            pc_psd_str_fini(&arr[i].name);
        }
        free(arr);
        if (wr != PC_OK) {
            pc_preset_groups_fini(&ex, OPS());
            return pc_engine_err_bad_params(err, cmd, "%s", e.msg[0] ? e.msg : "write failed");
        }
        (void)pc_buf_append(&bytes, ob.data, ob.len, &e);
        pc_psd_bytes_fini(&ob);
        /* warnings */
        char *wv[2];
        size_t wn = 0;
        if (ex.n > 1) {
            wv[wn++] = dup_str(".aco files have no groups; the swatches were saved as one list");
        }
        if (nonprocess) {
            wv[wn++] = dup_str(".aco files don't mark spot or global colours; save as .ase to keep them");
        }
        if (wn > 0) {
            warn = malloc(wn * sizeof *warn);
            if (warn != NULL) {
                memcpy(warn, wv, wn * sizeof *warn);
                nwarn = wn;
            } else {
                for (size_t i = 0; i < wn; i++) {
                    free(wv[i]);
                }
            }
        }
    } else {
        pc_psd_ase f;
        memset(&f, 0, sizeof f);
        f.entries = ex.n > 0 ? calloc(ex.n, sizeof *f.entries) : NULL;
        if (f.entries == NULL) {
            pc_preset_groups_fini(&ex, OPS());
            return pc_fail(err, PC_ERR_NOMEM, "export");
        }
        size_t books = 0;
        for (size_t i = 0; i < ex.n; i++) {
            pc_psd_ase_entry *ent = &f.entries[f.n];
            memset(ent, 0, sizeof *ent);
            ent->group = true;
            (void)pc_psd_str_set(&ent->name, ex.v[i].name, strlen(ex.v[i].name), &e);
            pc_swatch *items = ex.v[i].items;
            ent->swatches = ex.v[i].n > 0 ? calloc(ex.v[i].n, sizeof *ent->swatches) : NULL;
            for (size_t j = 0; j < ex.v[i].n; j++) {
                pc_psd_ase_color col;
                if (!to_ase(&items[j].color, &col)) {
                    books++;
                    continue;
                }
                pc_psd_ase_swatch *os = &ent->swatches[ent->n];
                memset(os, 0, sizeof *os);
                (void)pc_psd_str_set(&os->name, items[j].name, strlen(items[j].name), &e);
                os->color = col;
                os->kind = kind_to_ase(items[j].kind);
                ent->n++;
                count++;
            }
            f.n++;
        }
        pc_psd_bytes ob;
        memset(&ob, 0, sizeof ob);
        pc_status wr = pc_psd_ase_write(&f, &ob, &e);
        pc_psd_ase_fini(&f);
        if (wr != PC_OK) {
            pc_preset_groups_fini(&ex, OPS());
            return pc_engine_err_bad_params(err, cmd, "%s", e.msg[0] ? e.msg : "write failed");
        }
        (void)pc_buf_append(&bytes, ob.data, ob.len, &e);
        pc_psd_bytes_fini(&ob);
        if (books > 0) {
            char w[160];
            (void)snprintf(w, sizeof w,
                           "%zu colour-book swatches can't be stored in an .ase file and were left out "
                           "(save as .aco to keep them)",
                           books);
            warn = malloc(sizeof *warn);
            if (warn != NULL && (warn[0] = dup_str(w)) != NULL) {
                nwarn = 1;
            } else {
                free(warn);
                warn = NULL;
            }
        }
    }
    pc_preset_groups_fini(&ex, OPS());

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_buf_fini(&bytes);
        free_warn(warn, nwarn);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "format", fmt == FMT_ACO ? "aco" : "ase");
    yyjson_mut_obj_add_uint(d, root, "count", count);
    yyjson_mut_obj_add_uint(d, root, "bytes", bytes.len);
    yyjson_mut_val *warr = yyjson_mut_arr(d);
    for (size_t i = 0; i < nwarn; i++) {
        yyjson_mut_arr_add_strcpy(d, warr, warn[i]);
    }
    yyjson_mut_obj_add_val(d, root, "warnings", warr);
    free_warn(warn, nwarn);
    pc_status rc = PC_OK;
    if (path != NULL) {
        if (pc_file_write_atomic(path, bytes.data, bytes.len, err) != PC_OK) {
            pc_buf_fini(&bytes);
            return err != NULL ? err->code : PC_ERR_IO;
        }
        yyjson_mut_obj_add_strcpy(d, root, "path", path);
    } else {
        pc_buf b64;
        pc_buf_init(&b64);
        pc_error e2 = {0};
        if (pc_paint_b64_encode(bytes.data, bytes.len, &b64, &e2) != PC_OK) {
            pc_buf_fini(&bytes);
            return pc_fail(err, PC_ERR_NOMEM, "export");
        }
        yyjson_mut_obj_add_strncpy(d, root, "data", (const char *)b64.data, b64.len);
        pc_buf_fini(&b64);
    }
    yyjson_mut_doc_set_root(d, root);
    pc_buf_fini(&bytes);
    return rc;
}

static const char TARGET_DOC[] = "\"swatch\":name (or \"index\":n across all groups),\"group\":name?";

static const pc_engine_command SPECS[] = {
    {"swatches.list", "Swatches", NULL, 0, NULL,
     "{} -> {groups:[{name,swatches:[{name,model,values,hex,kind?}]}],count}. values are in Color Picker "
     "units; hex is the colour in the RGB working space.",
     pc_engine_always, run_list, false},
    {"swatches.add", "New Swatch...", NULL, 0, NULL,
     "{\"color\":\"#rrggbb\"|{model,values}?=foreground,\"name\":str?=\"Color Swatch "
     "N\",\"group\":name?=first,\"index\":n?=end,\"kind\":\"process|global|spot\"=\"process\"} -> "
     "{name,group,index}",
     pc_engine_always, run_add, true},
    {"swatches.delete", "Delete Swatch", NULL, 0, NULL,
     "{\"swatch\":name|[names],\"group\":name?} -> {deleted:[names]}", enabled_has_swatches, run_delete, true},
    {"swatches.rename", "Rename Swatch...", NULL, 0, NULL, "{\"swatch\":name,\"group\":name?,\"name\":str} -> {name}",
     enabled_has_swatches, run_rename, true},
    {"swatches.move", "Move Swatch", NULL, 0, NULL,
     "{\"swatch\":name,\"group\":name?,\"to\":group?,\"position\":n?=end} -> {group,position}",
     enabled_has_swatches, run_move, true},
    {"swatches.newGroup", "New Swatch Group...", NULL, 0, NULL,
     "{\"name\":str=\"Group\",\"swatches\":[names]?} -> {group,moved}", pc_engine_always, run_new_group, true},
    {"swatches.renameGroup", "Rename Swatch Group...", NULL, 0, NULL, "{\"group\":name,\"name\":str} -> {group}",
     enabled_has_groups, run_rename_group, true},
    {"swatches.deleteGroup", "Delete Swatch Group", NULL, 0, NULL,
     "{\"group\":name,\"keepSwatches\":bool=false} -> {group,deleted|kept}", enabled_has_groups,
     run_delete_group, true},
    {"swatches.use", "Use Swatch", NULL, 0, NULL,
     "{\"swatch\":name,\"group\":name?,\"target\":\"foreground|background\"=\"foreground\"} -> "
     "{swatch,target,hex}. CMYK, Lab and gray swatches are converted to the RGB working space with colour "
     "management.",
     enabled_has_swatches, run_use, true},
    {"swatches.reset", "Reset Swatches", NULL, 0, NULL, "{\"append\":bool=false} -> {groups,count}",
     pc_engine_always, run_reset, true},
    {"swatches.import", "Import Swatches...", NULL, 0, NULL,
     "{\"path\":\".aco or .ase file\"?,\"data\":base64 bytes?,\"format\":\"aco|ase\"?,\"mode\":"
     "\"append|replace\"=\"append\",\"group\":str?=file name} -> {format,mode,groups,count,warnings}",
     pc_engine_always, run_import, true},
    {"swatches.export", "Export Swatches...", NULL, 0, NULL,
     "{\"format\":\"aco|ase\"?,\"path\":file?,\"group\":name?,\"swatches\":[names]?} -> "
     "{format,count,bytes,warnings,path|data}",
     enabled_has_swatches, run_export, true},
};

const pc_engine_command *pc_engine_swatch_specs(size_t *n)
{
    (void)TARGET_DOC;
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
