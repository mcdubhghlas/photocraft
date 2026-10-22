/* Edit > Presets > Preset Manager and Export/Import Presets (port of the preset half of
 * crates/engine/src/edit_menu_cmds.rs: PRESET_KINDS, preset_names/preset_index, preset_manager,
 * PresetFile/export_import).
 *
 * The Preset Manager lists, renames, deletes and reorders the three preset libraries the C engine
 * keeps as flat vectors: brushes (ToolState::presets), custom shapes (EditState::custom_shapes) and
 * the pattern library (Session::patterns). Export/Import Presets moves brush and custom-shape sets
 * in and out through a portable JSON file ("photocraft-presets"), with every brush bitmap embedded
 * (PC_PAINT_JSON_PRESET) so a file carries its tips.
 *
 * Never crashes on any params or session state (REWRITE_PLAN.md rule 9): indices and names from the
 * params are hostile, bounds are checked before every access, and nothing panics. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "base/str.h"
#include "doc/pattern.h"
#include "doc/vector.h"
#include "paint/json.h"
#include "paint/presets.h"
#include "psd/abr.h" /* PC_PSD_ABR_MAX_FOLDER_DEPTH */

#include "brush_cmds.h" /* pc_engine_brush_presets_changed */
#include "engine_internal.h"
#include "preset_import_cmds.h"
#include "preset_manager_cmds.h"
#include "preset_store.h" /* pc_engine_session_load_brush_tips */
#include "vector_cmds.h"  /* pc_engine_parse_path, pc_engine_path_to_json */

#define CMD_MANAGER "edit.presets.presetManager"
#define CMD_EXPORT  "edit.presets.exportImportPresets"

/* ---- the three libraries ------------------------------------------------------------------ */

typedef enum pm_kind { PM_BRUSHES = 0, PM_CUSTOM_SHAPES, PM_PATTERNS, PM_KIND_COUNT } pm_kind;

static const char *const PM_KIND_ID[PM_KIND_COUNT] = {"brushes", "customShapes", "patterns"};

static bool pm_kind_parse(const char *s, pm_kind *out)
{
    if (s == NULL) {
        return false;
    }
    for (int k = 0; k < PM_KIND_COUNT; k++) {
        if (strcmp(s, PM_KIND_ID[k]) == 0) {
            *out = (pm_kind)k;
            return true;
        }
    }
    return false;
}

static size_t pm_count(const pc_engine_session *s, pm_kind kind)
{
    switch (kind) {
    case PM_BRUSHES:
        return s->tools.npresets;
    case PM_CUSTOM_SHAPES:
        return s->ncustom_shapes;
    case PM_PATTERNS:
        return s->npatterns;
    default:
        return 0;
    }
}

/* The display name of preset `i` of a kind: a borrowed pointer and its length (never NULL). */
static const char *pm_name(const pc_engine_session *s, pm_kind kind, size_t i, size_t *len)
{
    const char *n = "";
    *len = 0;
    switch (kind) {
    case PM_BRUSHES:
        if (i < s->tools.npresets) {
            n = pc_str_c(&s->tools.presets[i].name);
            *len = strlen(n);
        }
        break;
    case PM_CUSTOM_SHAPES:
        if (i < s->ncustom_shapes && s->custom_shapes[i].name != NULL) {
            n = s->custom_shapes[i].name;
            *len = strlen(n);
        }
        break;
    case PM_PATTERNS:
        if (i < s->npatterns) {
            const char *raw = pc_str_c(&s->patterns[i].name);
            n = pc_pattern_display_name(raw, strlen(raw), len);
        }
        break;
    default:
        break;
    }
    return n;
}

/* Build the JSON array of a kind's display names (owned by `d`). */
static yyjson_mut_val *pm_names_json(const pc_engine_session *s, pm_kind kind, yyjson_mut_doc *d)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    if (arr == NULL) {
        return NULL;
    }
    size_t n = pm_count(s, kind);
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        const char *name = pm_name(s, kind, i, &len);
        yyjson_mut_arr_append(arr, yyjson_mut_strncpy(d, name, len));
    }
    return arr;
}

/* Resolve the `index` or `name` param to a library index (reference preset_index). */
static pc_status pm_resolve(const pc_engine_session *s, pm_kind kind, yyjson_val *p, size_t *out,
                            pc_error *err)
{
    size_t n = pm_count(s, kind);
    uint64_t idx = 0;
    yyjson_val *iv = p != NULL ? pc_json_get(p, "index") : NULL;
    if (iv != NULL && pc_json_as_u64(iv, &idx)) {
        if (idx >= n) {
            return pc_engine_err_bad_params(err, CMD_MANAGER, "no preset %llu",
                                            (unsigned long long)idx);
        }
        *out = (size_t)idx;
        return PC_OK;
    }
    size_t want = 0;
    yyjson_val *nv = p != NULL ? pc_json_get(p, "name") : NULL;
    const char *name = nv != NULL ? pc_json_as_str(nv, &want) : NULL;
    if (name == NULL) {
        return pc_engine_err_bad_params(err, CMD_MANAGER, "give `index` or `name`");
    }
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        const char *have = pm_name(s, kind, i, &len);
        if (len == want && memcmp(have, name, want) == 0) {
            *out = i;
            return PC_OK;
        }
    }
    return pc_engine_err_bad_params(err, CMD_MANAGER, "no preset named `%.*s`", (int)want, name);
}

/* ---- per-kind mutation -------------------------------------------------------------------- */

static pc_status pm_rename(pc_engine_session *s, pm_kind kind, size_t i, const char *newname,
                           pc_error *err)
{
    switch (kind) {
    case PM_BRUSHES: {
        /* A renamed built-in becomes the user's own preset (reference edit_menu_cmds). */
        pc_paint_preset *b = &s->tools.presets[i];
        if (pc_str_set_c(&b->name, newname, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        b->builtin = false;
        pc_engine_brush_presets_changed(s);
        return PC_OK;
    }
    case PM_CUSTOM_SHAPES: {
        char *dup = pc_strndup(newname, strlen(newname));
        if (dup == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, CMD_MANAGER);
        }
        free(s->custom_shapes[i].name);
        s->custom_shapes[i].name = dup;
        return PC_OK;
    }
    case PM_PATTERNS:
        if (pc_str_set_c(&s->patterns[i].name, newname, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        return PC_OK;
    default:
        return pc_fail(err, PC_ERR_INVALID, CMD_MANAGER);
    }
}

static void pm_delete(pc_engine_session *s, pm_kind kind, size_t i)
{
    switch (kind) {
    case PM_BRUSHES: {
        pc_paint_preset_fini(&s->tools.presets[i]);
        size_t tail = s->tools.npresets - i - 1;
        if (tail > 0) {
            memmove(&s->tools.presets[i], &s->tools.presets[i + 1],
                    tail * sizeof *s->tools.presets);
        }
        s->tools.npresets--;
        pc_engine_brush_presets_changed(s);
        break;
    }
    case PM_CUSTOM_SHAPES: {
        free(s->custom_shapes[i].name);
        pc_path_fini(&s->custom_shapes[i].path);
        size_t tail = s->ncustom_shapes - i - 1;
        if (tail > 0) {
            memmove(&s->custom_shapes[i], &s->custom_shapes[i + 1],
                    tail * sizeof *s->custom_shapes);
        }
        s->ncustom_shapes--;
        break;
    }
    case PM_PATTERNS: {
        pc_pattern_fini(&s->patterns[i]);
        size_t tail = s->npatterns - i - 1;
        if (tail > 0) {
            memmove(&s->patterns[i], &s->patterns[i + 1], tail * sizeof *s->patterns);
        }
        s->npatterns--;
        break;
    }
    default:
        break;
    }
}

/* Move element `from` to `to` within a flat array (remove then insert; reference move_item). */
#define PM_MAX2(a, b) ((a) > (b) ? (a) : (b))
#define PM_ELEM_MAX                                                                                 \
    PM_MAX2(PM_MAX2(sizeof(pc_paint_preset), sizeof(pc_pattern)), sizeof(pc_engine_custom_shape))

static void pm_array_move(void *base, size_t n, size_t elem, size_t from, size_t to)
{
    if (from >= n || from == to || elem == 0 || elem > PM_ELEM_MAX) {
        return;
    }
    if (to >= n) {
        to = n - 1;
    }
    uint8_t *v = base;
    uint8_t tmp[PM_ELEM_MAX]; /* fits every library element (brush, pattern, custom shape) */
    memcpy(tmp, v + from * elem, elem);
    if (to > from) {
        memmove(v + from * elem, v + (from + 1) * elem, (to - from) * elem);
    } else {
        memmove(v + (to + 1) * elem, v + to * elem, (from - to) * elem);
    }
    memcpy(v + to * elem, tmp, elem);
}

static pc_status pm_move(pc_engine_session *s, pm_kind kind, size_t i, size_t to, pc_error *err)
{
    (void)err;
    switch (kind) {
    case PM_BRUSHES:
        pm_array_move(s->tools.presets, s->tools.npresets, sizeof *s->tools.presets, i, to);
        pc_engine_brush_presets_changed(s);
        break;
    case PM_CUSTOM_SHAPES:
        pm_array_move(s->custom_shapes, s->ncustom_shapes, sizeof *s->custom_shapes, i, to);
        break;
    case PM_PATTERNS:
        pm_array_move(s->patterns, s->npatterns, sizeof *s->patterns, i, to);
        break;
    default:
        break;
    }
    return PC_OK;
}

/* ---- edit.presets.presetManager ----------------------------------------------------------- */

static pc_status run_preset_manager(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                    pc_error *err)
{
    yyjson_val *av = p != NULL ? pc_json_get(p, "action") : NULL;
    size_t alen = 0;
    const char *action = av != NULL ? pc_json_as_str(av, &alen) : NULL;
    if (action == NULL) {
        action = "list";
    }

    if (strcmp(action, "list") == 0) {
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_val *kv = p != NULL ? pc_json_get(p, "kind") : NULL;
        const char *kstr = kv != NULL ? yyjson_get_str(kv) : NULL;
        if (kstr != NULL) {
            pm_kind kind;
            if (!pm_kind_parse(kstr, &kind)) {
                pc_json_value_fini(out);
                return pc_engine_err_bad_params(err, CMD_MANAGER,
                                                "unknown preset kind `%s` (brushes|customShapes|patterns)",
                                                kstr);
            }
            /* Use the canonical static id, not the caller's `kstr` (which the params doc frees):
             * yyjson_mut_obj_add_val stores the key pointer without copying. */
            yyjson_mut_obj_add_val(d, root, PM_KIND_ID[kind], pm_names_json(s, kind, d));
        } else {
            for (int k = 0; k < PM_KIND_COUNT; k++) {
                yyjson_mut_obj_add_val(d, root, PM_KIND_ID[k], pm_names_json(s, (pm_kind)k, d));
            }
        }
        yyjson_mut_doc_set_root(d, root);
        return PC_OK;
    }

    /* rename / delete / move need a kind and a target. */
    yyjson_val *kv = p != NULL ? pc_json_get(p, "kind") : NULL;
    const char *kstr = kv != NULL ? yyjson_get_str(kv) : NULL;
    pm_kind kind;
    if (kstr == NULL) {
        return pc_engine_err_bad_params(err, CMD_MANAGER, "missing `kind`");
    }
    if (!pm_kind_parse(kstr, &kind)) {
        return pc_engine_err_bad_params(err, CMD_MANAGER,
                                        "unknown preset kind `%s` (brushes|customShapes|patterns)",
                                        kstr);
    }
    size_t i = 0;
    pc_status rc = pm_resolve(s, kind, p, &i, err);
    if (rc != PC_OK) {
        return rc;
    }

    if (strcmp(action, "rename") == 0) {
        yyjson_val *nv = p != NULL ? pc_json_get(p, "newName") : NULL;
        size_t nlen = 0;
        const char *newname = nv != NULL ? pc_json_as_str(nv, &nlen) : NULL;
        /* Trim and reject empty, as the reference does. */
        char buf[512];
        if (newname != NULL) {
            while (nlen > 0 && (newname[0] == ' ' || newname[0] == '\t')) {
                newname++;
                nlen--;
            }
            while (nlen > 0 && (newname[nlen - 1] == ' ' || newname[nlen - 1] == '\t')) {
                nlen--;
            }
        }
        if (newname == NULL || nlen == 0 || nlen >= sizeof buf) {
            return pc_engine_err_bad_params(err, CMD_MANAGER, "missing `newName`");
        }
        memcpy(buf, newname, nlen);
        buf[nlen] = '\0';
        rc = pm_rename(s, kind, i, buf, err);
    } else if (strcmp(action, "delete") == 0) {
        pm_delete(s, kind, i);
    } else if (strcmp(action, "move") == 0) {
        yyjson_val *tv = p != NULL ? pc_json_get(p, "to") : NULL;
        uint64_t to = 0;
        if (tv == NULL || !pc_json_as_u64(tv, &to)) {
            return pc_engine_err_bad_params(err, CMD_MANAGER, "missing `to`");
        }
        rc = pm_move(s, kind, i, (size_t)to, err);
    } else {
        return pc_engine_err_bad_params(err, CMD_MANAGER,
                                        "unknown action `%s` (list|rename|delete|move)", action);
    }
    if (rc != PC_OK) {
        return rc;
    }

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, root, PM_KIND_ID[kind], pm_names_json(s, kind, d));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- edit.presets.exportImportPresets ----------------------------------------------------- */

static bool want_kind(yyjson_val *kinds, const char *id, bool dflt)
{
    if (kinds == NULL || !yyjson_is_arr(kinds)) {
        return dflt;
    }
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach(kinds, idx, max, v) {
        const char *k = yyjson_get_str(v);
        if (k != NULL && strcmp(k, id) == 0) {
            return true;
        }
    }
    return false;
}

static pc_status export_presets(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                pc_error *err)
{
    yyjson_val *kinds = p != NULL ? pc_json_get(p, "kinds") : NULL;
    bool want_brushes = want_kind(kinds, "brushes", kinds == NULL);
    bool want_shapes = want_kind(kinds, "customShapes", kinds == NULL);
    bool include_builtins = false;
    yyjson_val *ib = p != NULL ? pc_json_get(p, "includeBuiltins") : NULL;
    if (ib != NULL) {
        (void)pc_json_as_bool(ib, &include_builtins);
    }

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *data = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, data, "format", PC_PRESET_FILE_FORMAT);
    yyjson_mut_obj_add_uint(d, data, "version", 1);

    yyjson_mut_val *barr = yyjson_mut_arr(d);
    size_t nb = 0;
    if (want_brushes) {
        for (size_t i = 0; i < s->tools.npresets; i++) {
            if (s->tools.presets[i].builtin && !include_builtins) {
                continue;
            }
            /* The file embeds every bitmap: write a copy whose tips are loaded. */
            pc_paint_preset copy;
            if (pc_paint_preset_clone(&s->tools.presets[i], &copy, err) != PC_OK) {
                pc_json_value_fini(out);
                return PC_ERR_NOMEM;
            }
            pc_status lr = pc_engine_session_load_brush_tips(s, &copy.brush, err);
            if (lr != PC_OK) {
                pc_paint_preset_fini(&copy);
                pc_json_value_fini(out);
                return lr;
            }
            yyjson_mut_val *pv = pc_paint_json_write(PC_PAINT_JSON_PRESET, d, &copy, 0);
            pc_paint_preset_fini(&copy);
            if (pv == NULL) {
                pc_json_value_fini(out);
                return pc_fail(err, PC_ERR_NOMEM, CMD_EXPORT);
            }
            yyjson_mut_arr_append(barr, pv);
            nb++;
        }
    }
    yyjson_mut_obj_add_val(d, data, "brushes", barr);

    yyjson_mut_val *sarr = yyjson_mut_arr(d);
    size_t ns = 0;
    if (want_shapes) {
        for (size_t i = 0; i < s->ncustom_shapes; i++) {
            yyjson_mut_val *o = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, o, "name",
                                      s->custom_shapes[i].name != NULL ? s->custom_shapes[i].name
                                                                       : "");
            yyjson_mut_val *pathj = pc_engine_path_to_json(d, &s->custom_shapes[i].path);
            yyjson_mut_obj_add_val(d, o, "path", pathj != NULL ? pathj : yyjson_mut_obj(d));
            yyjson_mut_arr_append(sarr, o);
            ns++;
        }
    }
    yyjson_mut_obj_add_val(d, data, "customShapes", sarr);

    yyjson_mut_obj_add_val(d, root, "data", data);
    yyjson_mut_obj_add_uint(d, root, "brushes", nb);
    yyjson_mut_obj_add_uint(d, root, "customShapes", ns);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* Replace the brush preset of this name (ASCII case-insensitive), else append. Takes over *pr. */
static pc_status brush_upsert(pc_engine_session *s, pc_paint_preset *pr, pc_error *err)
{
    const char *name = pc_str_c(&pr->name);
    for (size_t i = 0; i < s->tools.npresets; i++) {
        if (pc_str_eq_c(&s->tools.presets[i].name, name)) {
            pc_paint_preset_fini(&s->tools.presets[i]);
            s->tools.presets[i] = *pr;
            memset(pr, 0, sizeof *pr);
            return PC_OK;
        }
    }
    size_t cap = s->tools.npresets;
    pc_paint_preset *grown =
        pc_grow_array(s->tools.presets, &cap, s->tools.npresets + 1, sizeof *s->tools.presets);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD_EXPORT);
    }
    s->tools.presets = grown;
    s->tools.presets[s->tools.npresets++] = *pr;
    memset(pr, 0, sizeof *pr);
    return PC_OK;
}

static pc_status shape_upsert(pc_engine_session *s, const char *name, pc_path *path, pc_error *err)
{
    for (size_t i = 0; i < s->ncustom_shapes; i++) {
        if (s->custom_shapes[i].name != NULL && strcmp(s->custom_shapes[i].name, name) == 0) {
            pc_path_fini(&s->custom_shapes[i].path);
            s->custom_shapes[i].path = *path; /* move */
            memset(path, 0, sizeof *path);
            return PC_OK;
        }
    }
    size_t cap = s->ncustom_shapes;
    pc_engine_custom_shape *grown =
        pc_grow_array(s->custom_shapes, &cap, s->ncustom_shapes + 1, sizeof *s->custom_shapes);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD_EXPORT);
    }
    s->custom_shapes = grown;
    s->cap_custom_shapes = cap;
    char *dup = pc_strndup(name, strlen(name));
    if (dup == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD_EXPORT);
    }
    s->custom_shapes[s->ncustom_shapes].name = dup;
    s->custom_shapes[s->ncustom_shapes].path = *path; /* move */
    memset(path, 0, sizeof *path);
    s->ncustom_shapes++;
    return PC_OK;
}

static pc_status import_presets(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                pc_error *err)
{
    yyjson_val *kinds = p != NULL ? pc_json_get(p, "kinds") : NULL;
    bool want_brushes = want_kind(kinds, "brushes", kinds == NULL);
    bool want_shapes = want_kind(kinds, "customShapes", kinds == NULL);

    yyjson_val *data = p != NULL ? pc_json_get(p, "data") : NULL;
    if (data == NULL) {
        return pc_engine_err_bad_params(err, CMD_EXPORT, "missing `data` (an exported preset file)");
    }
    /* `data` may be the parsed object or a JSON string holding one. */
    yyjson_doc *parsed = NULL;
    if (yyjson_is_str(data)) {
        size_t tlen = 0;
        const char *text = pc_json_as_str(data, &tlen);
        if (text == NULL || pc_json_parse(text, tlen, &parsed, err) != PC_OK) {
            return pc_engine_err_bad_params(err, CMD_EXPORT, "not a preset file");
        }
        data = yyjson_doc_get_root(parsed);
    }
    if (data == NULL || !yyjson_is_obj(data)) {
        yyjson_doc_free(parsed);
        return pc_engine_err_bad_params(err, CMD_EXPORT, "not a preset file");
    }
    const char *fmt = yyjson_get_str(pc_json_get(data, "format"));
    if (fmt == NULL || strcmp(fmt, PC_PRESET_FILE_FORMAT) != 0) {
        yyjson_doc_free(parsed);
        return pc_engine_err_bad_params(err, CMD_EXPORT, "not a preset file (format `%s`)",
                                        fmt != NULL ? fmt : "");
    }

    size_t nb = 0, ns = 0;
    pc_status rc = PC_OK;
    if (want_brushes) {
        yyjson_val *arr = pc_json_get(data, "brushes");
        size_t idx, max;
        yyjson_val *bv;
        if (arr != NULL && yyjson_is_arr(arr)) {
            yyjson_arr_foreach(arr, idx, max, bv) {
                pc_paint_preset pr;
                if (pc_paint_json_read(PC_PAINT_JSON_PRESET, bv, &pr, err) != PC_OK) {
                    rc = pc_engine_err_bad_params(err, CMD_EXPORT, "not a preset file");
                    break;
                }
                /* Nested folders only as deep as the brush commands bind them. */
                while (pr.folder.len > 0 &&
                       pc_str_c(&pr.folder.v[pr.folder.len - 1])[0] == '\0') {
                    pr.folder.len--;
                    pc_str_fini(&pr.folder.v[pr.folder.len]);
                }
                if (pr.folder.len > PC_PSD_ABR_MAX_FOLDER_DEPTH) {
                    for (size_t k = PC_PSD_ABR_MAX_FOLDER_DEPTH; k < pr.folder.len; k++) {
                        pc_str_fini(&pr.folder.v[k]);
                    }
                    pr.folder.len = PC_PSD_ABR_MAX_FOLDER_DEPTH;
                }
                rc = brush_upsert(s, &pr, err);
                pc_paint_json_fini(PC_PAINT_JSON_PRESET, &pr);
                if (rc != PC_OK) {
                    break;
                }
                nb++;
            }
        }
        if (nb > 0) {
            pc_engine_brush_presets_changed(s);
        }
    }
    if (rc == PC_OK && want_shapes) {
        yyjson_val *arr = pc_json_get(data, "customShapes");
        size_t idx, max;
        yyjson_val *cv;
        if (arr != NULL && yyjson_is_arr(arr)) {
            yyjson_arr_foreach(arr, idx, max, cv) {
                const char *name = yyjson_get_str(pc_json_get(cv, "name"));
                yyjson_val *pathv = pc_json_get(cv, "path");
                pc_path path;
                char emsg[256];
                if (name == NULL || pathv == NULL ||
                    !pc_engine_parse_path(pathv, &path, emsg, sizeof emsg)) {
                    rc = pc_engine_err_bad_params(err, CMD_EXPORT, "not a preset file");
                    break;
                }
                rc = shape_upsert(s, name, &path, err);
                pc_path_fini(&path);
                if (rc != PC_OK) {
                    break;
                }
                ns++;
            }
        }
    }
    yyjson_doc_free(parsed);
    if (rc != PC_OK) {
        return rc;
    }

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "brushes", nb);
    yyjson_mut_obj_add_uint(d, root, "customShapes", ns);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_export_import(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                   pc_error *err)
{
    yyjson_val *av = p != NULL ? pc_json_get(p, "action") : NULL;
    const char *action = av != NULL ? yyjson_get_str(av) : NULL;
    if (action == NULL) {
        action = "export";
    }
    if (strcmp(action, "export") == 0) {
        return export_presets(s, p, out, err);
    }
    if (strcmp(action, "import") == 0) {
        return import_presets(s, p, out, err);
    }
    return pc_engine_err_bad_params(err, CMD_EXPORT, "unknown action `%s` (export|import)", action);
}

/* ---- specs -------------------------------------------------------------------------------- */

static pc_status always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

static const char *const MENU_EDIT_PRESETS[] = {"Edit", "Presets"};

static const pc_engine_command SPECS[] = {
    {"edit.presets.presetManager", "Preset Manager\xE2\x80\xA6", MENU_EDIT_PRESETS, 2, NULL,
     "{\"action\":\"list|rename|delete|move\"=\"list\",\"kind\":\"brushes|customShapes|patterns\","
     "\"index\":n?,\"name\":str?,\"newName\":str? (rename),\"to\":n? (move)} \xE2\x86\x92 {<kind>:[names]}",
     always, run_preset_manager, true},
    {"edit.presets.exportImportPresets", "Export/Import Presets\xE2\x80\xA6", MENU_EDIT_PRESETS, 2,
     NULL,
     "{\"action\":\"export|import\"=\"export\",\"kinds\":[\"brushes\",\"customShapes\"]?,"
     "\"includeBuiltins\":bool=false (export),\"data\":object|string (import)} \xE2\x86\x92 export: "
     "{data, brushes, customShapes}; import: {brushes, customShapes}",
     always, run_export_import, true},
};

const pc_engine_command *pc_engine_preset_manager_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
