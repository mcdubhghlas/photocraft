/* The user contour library of the Layer Style dialog (layer.layerStyle.contours.*; port of
 * crates/engine/src/contour_lib_cmds.rs).
 *
 * Photoshop's Contour Editor "New…" saves the edited curve under a name; the picker lists those
 * user contours after its standard ones (Load, Save, Replace Contours). The library lives in the
 * preferences document (pc_preset_state.contours) and changes the preferences, not a document's
 * history. A user contour is applied to an effect as a `contour` curve object; .shc files go
 * through src/psd shc. Never crashes (REWRITE_PLAN.md rule 9): params are validated, the library is
 * size-capped and names are clipped. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "base/base64.h"
#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "base/mem.h"
#include "doc/adjust.h"
#include "doc/effects.h"
#include "psd/shc.h"
#include "psd/types.h"

#include "contour_lib_cmds.h"
#include "engine_internal.h"
#include "layer_style.h" /* builtin_contour / contour_param / contour_from_param / contours_from_shc */
#include "presets.h"

/* Most user contours kept; longest name kept, in bytes (the reference caps characters). */
#define MAX_USER_CONTOURS 1000u
#define MAX_NAME 255u

static const char *contour_name(const pc_contour *c)
{
    return c->custom ? pc_str_c(&c->name) : "Linear";
}

/* Clip `name` to MAX_NAME bytes, not splitting a UTF-8 code point, into `out` (cap bytes). */
static void clip_name(const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    if (n > MAX_NAME) {
        n = MAX_NAME;
        while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80) {
            n--; /* back off a continuation byte */
        }
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, name, n);
    out[n] = '\0';
}

static bool name_taken(const pc_preset_state *st, const char *n)
{
    pc_contour b;
    if (pc_engine_builtin_contour(n, &b)) {
        pc_contour_fini(&b);
        return true;
    }
    for (size_t i = 0; i < st->ncontours; i++) {
        if (strcmp(contour_name(&st->contours[i]), n) == 0) {
            return true;
        }
    }
    return false;
}

/* `base`, or `base 2`, `base 3`… so a name shadows neither a standard nor a user contour. */
static void unique_name(const pc_preset_state *st, const char *base, char *out, size_t cap)
{
    char clipped[MAX_NAME + 1];
    clip_name(base, clipped, sizeof clipped);
    if (!name_taken(st, clipped)) {
        (void)snprintf(out, cap, "%s", clipped);
        return;
    }
    for (unsigned i = 2; i < 100000u; i++) {
        (void)snprintf(out, cap, "%s %u", clipped, i);
        if (!name_taken(st, out)) {
            return;
        }
    }
    (void)snprintf(out, cap, "%s", clipped);
}

/* A storable named custom contour from `src` (Linear becomes the identity curve). */
static pc_status make_named(const pc_contour *src, const char *name, pc_contour *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->custom = true;
    if (pc_str_set_c(&out->name, name, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (src->custom) {
        if (pc_curve_clone(&src->points, &out->points, err) != PC_OK) {
            pc_contour_fini(out);
            return PC_ERR_NOMEM;
        }
        if (src->ncorners > 0) {
            out->corners = pc_malloc_array(src->ncorners, sizeof *out->corners);
            if (out->corners == NULL) {
                pc_contour_fini(out);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            memcpy(out->corners, src->corners, src->ncorners * sizeof *out->corners);
            out->ncorners = src->ncorners;
        }
    } else {
        if (pc_curve_identity(&out->points, err) != PC_OK) {
            pc_contour_fini(out);
            return PC_ERR_NOMEM;
        }
    }
    return PC_OK;
}

/* Add `src` under `name` (made unique); the stored name is written to out_name (cap bytes). */
static pc_status lib_add(pc_preset_state *st, const pc_contour *src, const char *name, char *out_name,
                         size_t out_cap, pc_error *err)
{
    char unique[MAX_NAME + 16];
    unique_name(st, name, unique, sizeof unique);
    pc_contour stored;
    pc_status rc = make_named(src, unique, &stored, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_contour *grown = pc_grow_array(st->contours, &st->cap_contours, st->ncontours + 1, sizeof *grown);
    if (grown == NULL) {
        pc_contour_fini(&stored);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    st->contours = grown;
    st->contours[st->ncontours++] = stored;
    if (out_name != NULL) {
        (void)snprintf(out_name, out_cap, "%s", unique);
    }
    return PC_OK;
}

/* ---- persistence (presets.h hooks) --------------------------------------------------------- */

void pc_engine_contour_lib_fini(pc_preset_state *st)
{
    if (st == NULL) {
        return;
    }
    for (size_t i = 0; i < st->ncontours; i++) {
        pc_contour_fini(&st->contours[i]);
    }
    free(st->contours);
    st->contours = NULL;
    st->ncontours = 0;
    st->cap_contours = 0;
}

void pc_engine_contour_lib_to_json(const pc_preset_state *st, yyjson_mut_doc *d, yyjson_mut_val *root)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < st->ncontours; i++) {
        yyjson_mut_val *v = NULL;
        if (pc_engine_contour_param(&st->contours[i], d, &v, NULL) == PC_OK && v != NULL) {
            yyjson_mut_arr_append(arr, v);
        }
    }
    yyjson_mut_obj_add_val(d, root, "contours", arr);
}

void pc_engine_contour_lib_load(pc_preset_state *st, yyjson_val *v)
{
    yyjson_val *arr = v != NULL ? pc_json_get(v, "contours") : NULL;
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return;
    }
    pc_engine_contour_lib_fini(st);
    size_t idx, max;
    yyjson_val *e;
    yyjson_arr_foreach(arr, idx, max, e) {
        if (st->ncontours >= MAX_USER_CONTOURS) {
            break;
        }
        /* Entries are curve objects; a bare name would only name a standard contour. */
        if (!yyjson_is_obj(e)) {
            continue;
        }
        pc_contour c;
        if (!pc_engine_contour_from_param(e, &c)) {
            continue;
        }
        (void)lib_add(st, &c, contour_name(&c), NULL, 0, NULL);
        pc_contour_fini(&c);
    }
}

/* ---- commands ------------------------------------------------------------------------------ */

static pc_preset_state *presets_of(pc_engine_session *s)
{
    return &s->presets;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_preset_state *st = presets_of(s);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "count", st->ncontours);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < st->ncontours; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, o, "name", contour_name(&st->contours[i]));
        yyjson_mut_val *cp = NULL;
        if (pc_engine_contour_param(&st->contours[i], rd, &cp, err) == PC_OK && cp != NULL) {
            yyjson_mut_obj_add_val(rd, o, "contour", cp);
        }
        yyjson_mut_arr_append(arr, o);
    }
    yyjson_mut_obj_add_val(rd, root, "contours", arr);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_new(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.layerStyle.contours.new";
    yyjson_val *jc = p != NULL ? pc_json_get(p, "contour") : NULL;
    if (jc == NULL) {
        return pc_engine_err_bad_params(err, CMD,
                                        "missing `contour` (a standard contour's name or a curve object)");
    }
    pc_contour c;
    if (!pc_engine_contour_from_param(jc, &c)) {
        return pc_engine_err_bad_params(err, CMD,
                                        "`contour` is not a usable contour (unknown name, or a bad curve)");
    }
    pc_preset_state *st = presets_of(s);
    if (st->ncontours >= MAX_USER_CONTOURS) {
        pc_contour_fini(&c);
        return pc_engine_err_bad_params(err, CMD, "the contour library is full (%u contours)",
                                        (unsigned)MAX_USER_CONTOURS);
    }
    char name[MAX_NAME + 16];
    yyjson_val *jn = p != NULL ? pc_json_get(p, "name") : NULL;
    if (jn == NULL || yyjson_is_null(jn)) {
        (void)snprintf(name, sizeof name, "Contour %zu", st->ncontours + 1);
    } else {
        size_t nl = 0;
        const char *nm = pc_json_as_str(jn, &nl);
        /* trim */
        while (nm != NULL && nl > 0 && (nm[0] == ' ' || nm[0] == '\t')) {
            nm++;
            nl--;
        }
        while (nm != NULL && nl > 0 && (nm[nl - 1] == ' ' || nm[nl - 1] == '\t')) {
            nl--;
        }
        if (nm == NULL || nl == 0) {
            pc_contour_fini(&c);
            return pc_engine_err_bad_params(err, CMD, "`name` must be a non-empty string");
        }
        size_t copy = nl < sizeof name ? nl : sizeof name - 1;
        memcpy(name, nm, copy);
        name[copy] = '\0';
    }
    char stored[MAX_NAME + 16];
    pc_status rc = lib_add(st, &c, name, stored, sizeof stored, err);
    pc_contour_fini(&c);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_presets_changed(s);

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "name", stored);
    yyjson_mut_val *cp = NULL;
    if (st->ncontours > 0 &&
        pc_engine_contour_param(&st->contours[st->ncontours - 1], rd, &cp, err) == PC_OK && cp != NULL) {
        yyjson_mut_obj_add_val(rd, root, "contour", cp);
    } else {
        yyjson_mut_obj_add_null(rd, root, "contour");
    }
    yyjson_mut_obj_add_uint(rd, root, "count", st->ncontours);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.layerStyle.contours.delete";
    size_t nl = 0;
    const char *name = pc_json_as_str(p != NULL ? pc_json_get(p, "name") : NULL, &nl);
    if (name == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing `name`");
    }
    pc_preset_state *st = presets_of(s);
    size_t at = st->ncontours;
    for (size_t i = 0; i < st->ncontours; i++) {
        if (strcmp(contour_name(&st->contours[i]), name) == 0) {
            at = i;
            break;
        }
    }
    if (at == st->ncontours) {
        return pc_engine_err_bad_params(err, CMD, "no user contour \"%s\"", name);
    }
    pc_contour_fini(&st->contours[at]);
    memmove(&st->contours[at], &st->contours[at + 1], (st->ncontours - at - 1) * sizeof *st->contours);
    st->ncontours--;
    pc_engine_presets_changed(s);

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strncpy(rd, root, "deleted", name, nl);
    yyjson_mut_obj_add_uint(rd, root, "count", st->ncontours);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_load(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.layerStyle.contours.load";
    bool replace = false;
    size_t ml = 0;
    const char *mode = pc_json_as_str(p != NULL ? pc_json_get(p, "mode") : NULL, &ml);
    if (mode != NULL) {
        if (ml == 7 && memcmp(mode, "replace", 7) == 0) {
            replace = true;
        } else if (!(ml == 6 && memcmp(mode, "append", 6) == 0)) {
            return pc_engine_err_bad_params(err, CMD, "unknown mode (append|replace)");
        }
    }
    pc_buf bytes = PC_BUF_INIT;
    pc_status rc = pc_engine_preset_file_bytes(p, CMD, ".shc file", &bytes, NULL, 0, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Parse the .shc into contour param values, then into contours. */
    yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
    if (tmp == NULL) {
        pc_buf_fini(&bytes);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *arr = yyjson_mut_arr(tmp);
    rc = pc_engine_contours_from_shc(bytes.data, bytes.len, tmp, arr, err);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        yyjson_mut_doc_free(tmp);
        return rc;
    }
    size_t found = yyjson_mut_arr_size(arr);
    if (found == 0) {
        yyjson_mut_doc_free(tmp);
        return pc_engine_err_bad_params(err, CMD, "the file holds no usable contours");
    }
    /* contour_from_param wants immutable values: serialise the mutable array and re-parse it. */
    yyjson_mut_doc_set_root(tmp, arr);
    pc_buf text = PC_BUF_INIT;
    rc = pc_json_write(yyjson_mut_doc_get_root(tmp), 0, &text, err);
    yyjson_mut_doc_free(tmp);
    if (rc != PC_OK) {
        pc_buf_fini(&text);
        return rc;
    }
    yyjson_doc *idoc = NULL;
    rc = pc_json_parse(pc_buf_cstr(&text), text.len, &idoc, err);
    pc_buf_fini(&text);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_val *iarr = yyjson_doc_get_root(idoc);

    pc_preset_state *st = presets_of(s);
    /* Build a fresh library (replace) or extend the current one (append). */
    if (replace) {
        pc_engine_contour_lib_fini(st);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        yyjson_doc_free(idoc);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *names = yyjson_mut_arr(rd);
    size_t loaded = 0;
    size_t idx, max;
    yyjson_val *el;
    yyjson_arr_foreach(iarr, idx, max, el) {
        if (st->ncontours >= MAX_USER_CONTOURS) {
            break;
        }
        pc_contour c;
        if (pc_engine_contour_from_param(el, &c)) {
            char stored[MAX_NAME + 16];
            if (lib_add(st, &c, contour_name(&c), stored, sizeof stored, NULL) == PC_OK) {
                yyjson_mut_arr_add_strcpy(rd, names, stored);
                loaded++;
            }
            pc_contour_fini(&c);
        }
    }
    yyjson_doc_free(idoc);
    pc_engine_presets_changed(s);

    yyjson_mut_obj_add_val(rd, root, "loaded", names);
    yyjson_mut_obj_add_uint(rd, root, "count", st->ncontours);
    yyjson_mut_obj_add_uint(rd, root, "skipped", found - loaded);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* Write the selected contours as one .shc (a local multi-contour writer, mirroring the single
 * pc_engine_contour_to_shc but through psd shc's n-contour pc_psd_shc_write). */
static pc_status contours_to_shc(const pc_contour *const *lib, size_t n, pc_buf *out, pc_error *err)
{
    pc_psd_shc_contour *arr = pc_calloc_array(n, sizeof *arr);
    if (arr == NULL && n > 0) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_status rc = PC_OK;
    size_t built = 0;
    for (; built < n; built++) {
        const pc_contour *c = lib[built];
        const char *name = c->custom ? pc_str_c(&c->name) : "Linear";
        if (pc_psd_str_set(&arr[built].name, name, strlen(name), err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            break;
        }
        size_t np = c->custom ? c->points.len : 2;
        arr[built].points = pc_calloc_array(np, sizeof *arr[built].points);
        if (arr[built].points == NULL && np > 0) {
            rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            break;
        }
        arr[built].npoints = np;
        if (c->custom) {
            for (size_t i = 0; i < np; i++) {
                arr[built].points[i].input = c->points.pts[i].input * 255.0f;
                arr[built].points[i].output = c->points.pts[i].output * 255.0f;
                arr[built].points[i].continuous = !pc_contour_is_corner(c, i);
            }
        } else {
            arr[built].points[0] = (pc_psd_shc_point){0.0f, 0.0f, true};
            arr[built].points[1] = (pc_psd_shc_point){255.0f, 255.0f, true};
        }
    }
    if (rc == PC_OK) {
        pc_psd_bytes bytes = {0};
        rc = pc_psd_shc_write(arr, n, &bytes, err);
        if (rc == PC_OK) {
            pc_buf_init(out);
            rc = pc_buf_append(out, bytes.data, bytes.len, err);
        }
        pc_psd_bytes_fini(&bytes);
    }
    for (size_t i = 0; i <= built && i < n; i++) {
        pc_psd_shc_contour_fini(&arr[i]);
    }
    free(arr);
    return rc;
}

static pc_status run_save(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.layerStyle.contours.save";
    pc_preset_state *st = presets_of(s);
    const pc_contour **lib = NULL;
    size_t n = 0;
    yyjson_val *jn = p != NULL ? pc_json_get(p, "names") : NULL;
    if (jn == NULL || yyjson_is_null(jn)) {
        n = st->ncontours;
        if (n > 0) {
            lib = pc_malloc_array(n, sizeof *lib);
            if (lib == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            for (size_t i = 0; i < n; i++) {
                lib[i] = &st->contours[i];
            }
        }
    } else if (yyjson_is_arr(jn)) {
        n = yyjson_arr_size(jn);
        lib = n > 0 ? pc_malloc_array(n, sizeof *lib) : NULL;
        if (n > 0 && lib == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        size_t idx, max, k = 0;
        yyjson_val *e;
        yyjson_arr_foreach(jn, idx, max, e) {
            size_t nl = 0;
            const char *nm = pc_json_as_str(e, &nl);
            if (nm == NULL) {
                free(lib);
                return pc_engine_err_bad_params(err, CMD, "`names` must be strings");
            }
            const pc_contour *found = NULL;
            for (size_t i = 0; i < st->ncontours; i++) {
                if (strcmp(contour_name(&st->contours[i]), nm) == 0) {
                    found = &st->contours[i];
                    break;
                }
            }
            if (found == NULL) {
                free(lib);
                return pc_engine_err_bad_params(err, CMD, "no user contour \"%.*s\"", (int)nl, nm);
            }
            lib[k++] = found;
        }
    } else {
        return pc_engine_err_bad_params(err, CMD, "`names` must be a list of contour names");
    }
    if (n == 0) {
        free(lib);
        return pc_engine_err_bad_params(err, CMD, "there are no user contours to save");
    }

    pc_buf bytes = PC_BUF_INIT;
    pc_status rc = contours_to_shc(lib, n, &bytes, err);
    free(lib);
    if (rc != PC_OK) {
        pc_buf_fini(&bytes);
        return rc;
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_buf_fini(&bytes);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "count", n);
    yyjson_mut_obj_add_uint(rd, root, "bytes", bytes.len);
    size_t pl = 0;
    const char *path = pc_json_as_str(p != NULL ? pc_json_get(p, "path") : NULL, &pl);
    if (path != NULL) {
        char *pathz = pc_strndup(path, pl);
        rc = pathz != NULL ? pc_file_write_atomic(pathz, bytes.data, bytes.len, err)
                           : pc_fail(err, PC_ERR_NOMEM, "out of memory");
        free(pathz);
        if (rc != PC_OK) {
            pc_buf_fini(&bytes);
            pc_json_value_fini(out);
            return rc;
        }
        yyjson_mut_obj_add_strncpy(rd, root, "path", path, pl);
    } else {
        pc_buf b64 = PC_BUF_INIT;
        if (pc_base64_encode(bytes.data, bytes.len, &b64, err) == PC_OK) {
            yyjson_mut_obj_add_strcpy(rd, root, "data", pc_buf_cstr(&b64));
        }
        pc_buf_fini(&b64);
    }
    pc_buf_fini(&bytes);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"layer.layerStyle.contours.list", "List Contours", NULL, 0, NULL,
     "{} -> {count,contours:[{name,contour}]}", pc_engine_always, run_list, false},
    {"layer.layerStyle.contours.new", "New Contour", NULL, 0, NULL,
     "{\"contour\":name|curve,\"name\":str?} -> {name,contour,count}", pc_engine_always, run_new, false},
    {"layer.layerStyle.contours.delete", "Delete Contour", NULL, 0, NULL,
     "{\"name\":str} -> {deleted,count}", pc_engine_always, run_delete, false},
    {"layer.layerStyle.contours.load", "Load Contours", NULL, 0, NULL,
     "{\"path\":str|\"data\":b64,\"mode\":\"append|replace\"=append} -> {loaded,count,skipped}",
     pc_engine_always, run_load, false},
    {"layer.layerStyle.contours.save", "Save Contours", NULL, 0, NULL,
     "{\"names\":[str]?,\"path\":str?} -> {count,bytes,path|data} (.shc; all contours by default)",
     pc_engine_always, run_save, false},
};

const pc_engine_command *pc_engine_contour_lib_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
