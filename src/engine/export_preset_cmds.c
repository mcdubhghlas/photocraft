/* Named Export As configurations (file.export.presets.*; port of
 * crates/engine/src/export_preset_cmds.rs). They persist with the other presets in the preferences
 * document; applying one only returns settings, and the export itself stays in Export As.
 *
 * Never crashes (REWRITE_PLAN.md rule 9): the name and every settings field are validated and
 * coerced before a preset is stored, and a damaged saved entry is dropped on load. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "base/json.h"
#include "base/mem.h"

#include "engine_internal.h"
#include "export_preset_cmds.h"
#include "presets.h"

#define MAX_PRESETS 256u
#define MAX_PX 300000u
#define MAX_NAME 255u

/* One Export As preset: a trimmed name and a validated, normalised settings object. */
struct pc_export_preset {
    char *name;             /* owned, trimmed */
    pc_json_value settings; /* owned normalised object */
};

static const char *const FORMATS[] = {"png", "jpg", "webp", "tif", "tga", "ico"};
static const char *const SIZINGS[] = {"scale", "pixels"};
static const char *const METADATA[] = {"none", "exif", "all"};
static const char *const PROFILES[] = {"document", "srgb"};
static const char *const TGA_BITS[] = {"auto", "24", "32"};
static const char *const RESAMPLE[] = {"bicubicAutomatic", "bicubic",  "bicubicSmoother",
                                       "bicubicSharper",   "bilinear", "nearest"};

static bool in_set(const char *v, const char *const *set, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(v, set[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* A string field of `in` or `def`. */
static const char *field_str(yyjson_val *in, const char *key, const char *def)
{
    const char *v = pc_json_as_str(pc_json_get(in, key), NULL);
    return v != NULL ? v : def;
}

/* Validate and normalise an incoming settings object into *out. On failure returns false and
 * *msg names the problem (a static string). */
static bool build_settings(yyjson_val *in, pc_json_value *out, const char **msg)
{
    if (in == NULL || !yyjson_is_obj(in)) {
        *msg = "`settings` must be an object";
        return false;
    }
    const char *format = field_str(in, "format", "png");
    if (!in_set(format, FORMATS, 6)) {
        *msg = "format must be one of png, jpg, webp, tif, tga, ico";
        return false;
    }
    /* quality: round from a number, 1..100. */
    double q = 85.0;
    yyjson_val *jq = pc_json_get(in, "quality");
    if (jq != NULL && !yyjson_is_null(jq)) {
        if (!pc_json_as_f64(jq, &q)) {
            *msg = "`quality` must be a number";
            return false;
        }
    }
    long ql = (long)(q + (q >= 0 ? 0.5 : -0.5));
    if (!(ql >= 1 && ql <= 100)) {
        *msg = "quality must be within 1..=100";
        return false;
    }
    double scale = 100.0;
    yyjson_val *js = pc_json_get(in, "scale");
    if (js != NULL && !yyjson_is_null(js)) {
        if (!pc_json_as_f64(js, &scale)) {
            *msg = "`scale` must be a number";
            return false;
        }
    }
    if (!(scale >= 1.0 && scale <= 1000.0)) {
        *msg = "scale must be within 1..=1000 percent";
        return false;
    }
    /* width/height: round from a number, 0..MAX_PX. */
    long dim[2] = {0, 0};
    const char *dimkey[2] = {"width", "height"};
    for (int i = 0; i < 2; i++) {
        double d = 0.0;
        yyjson_val *jd = pc_json_get(in, dimkey[i]);
        if (jd != NULL && !yyjson_is_null(jd)) {
            if (!pc_json_as_f64(jd, &d)) {
                *msg = "`width`/`height` must be numbers";
                return false;
            }
        }
        if (!(d >= 0.0 && d <= (double)MAX_PX)) {
            *msg = "width and height must be within 0..=300000 pixels";
            return false;
        }
        dim[i] = (long)(d + 0.5);
    }
    const char *sizing = field_str(in, "sizing", "scale");
    if (!in_set(sizing, SIZINGS, 2)) {
        *msg = "sizing must be scale or pixels";
        return false;
    }
    if (strcmp(sizing, "pixels") == 0 && !(dim[0] >= 1 && dim[1] >= 1)) {
        *msg = "width and height must be within 1..=300000 pixels";
        return false;
    }
    const char *metadata = field_str(in, "metadata", "none");
    if (!in_set(metadata, METADATA, 3)) {
        *msg = "metadata must be none, exif or all";
        return false;
    }
    const char *profile = field_str(in, "profile", "document");
    if (!in_set(profile, PROFILES, 2)) {
        *msg = "profile must be document or srgb";
        return false;
    }
    const char *tga = field_str(in, "tgaBits", "auto");
    if (!in_set(tga, TGA_BITS, 3)) {
        *msg = "tgaBits must be auto, 24 or 32";
        return false;
    }
    /* resample: the field must be a string naming one of the choices (no silent fallback). */
    yyjson_val *jr = pc_json_get(in, "resample");
    const char *resample = "bicubicAutomatic";
    if (jr != NULL && !yyjson_is_null(jr)) {
        resample = pc_json_as_str(jr, NULL);
        if (resample == NULL) {
            *msg = "resample must be one of bicubicAutomatic, bicubic, bicubicSmoother, "
                   "bicubicSharper, bilinear, nearest";
            return false;
        }
    }
    if (!in_set(resample, RESAMPLE, 6)) {
        *msg = "resample must be one of bicubicAutomatic, bicubic, bicubicSmoother, bicubicSharper, "
               "bilinear, nearest";
        return false;
    }
    bool lossless = yyjson_get_bool(pc_json_get(in, "lossless"));
    yyjson_val *jt = pc_json_get(in, "transparency");
    bool transparency = jt != NULL ? yyjson_get_bool(jt) : true;
    bool lock_aspect = true;
    yyjson_val *jla = pc_json_get(in, "lockAspect");
    if (jla != NULL) {
        lock_aspect = yyjson_get_bool(jla);
    }
    bool embed = true;
    yyjson_val *je = pc_json_get(in, "embedProfile");
    if (je != NULL) {
        embed = yyjson_get_bool(je);
    }

    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        *msg = "out of memory";
        return false;
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "format", format);
    yyjson_mut_obj_add_uint(d, o, "quality", (uint64_t)ql);
    yyjson_mut_obj_add_bool(d, o, "lossless", lossless);
    yyjson_mut_obj_add_bool(d, o, "transparency", transparency);
    yyjson_mut_obj_add_real(d, o, "scale", scale);
    yyjson_mut_obj_add_uint(d, o, "width", (uint64_t)dim[0]);
    yyjson_mut_obj_add_uint(d, o, "height", (uint64_t)dim[1]);
    yyjson_mut_obj_add_bool(d, o, "lockAspect", lock_aspect);
    yyjson_mut_obj_add_strcpy(d, o, "sizing", sizing);
    yyjson_mut_obj_add_strcpy(d, o, "metadata", metadata);
    yyjson_mut_obj_add_strcpy(d, o, "profile", profile);
    yyjson_mut_obj_add_bool(d, o, "embedProfile", embed);
    yyjson_mut_obj_add_strcpy(d, o, "tgaBits", tga);
    yyjson_mut_obj_add_strcpy(d, o, "resample", resample);
    yyjson_mut_doc_set_root(d, o);
    pc_status rc = pc_json_value_from_mut(o, out, NULL);
    yyjson_mut_doc_free(d);
    if (rc != PC_OK) {
        *msg = "out of memory";
        return false;
    }
    return true;
}

/* A trimmed copy of `name` (1..255 chars), or NULL if empty/too long. Caller frees. */
static char *checked_name(const char *name, size_t len)
{
    while (len > 0 && (name[0] == ' ' || name[0] == '\t' || name[0] == '\n' || name[0] == '\r')) {
        name++;
        len--;
    }
    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t' || name[len - 1] == '\n' ||
                       name[len - 1] == '\r')) {
        len--;
    }
    if (len == 0 || len > MAX_NAME) {
        return NULL;
    }
    return pc_strndup(name, len);
}

static void preset_fini(struct pc_export_preset *pr)
{
    free(pr->name);
    pc_json_value_fini(&pr->settings);
    memset(pr, 0, sizeof *pr);
}

static struct pc_export_preset *find_ci(pc_preset_state *st, const char *name)
{
    for (size_t i = 0; i < st->nexport_presets; i++) {
        if (strcasecmp(st->export_presets[i].name, name) == 0) {
            return &st->export_presets[i];
        }
    }
    return NULL;
}

/* {name, settings} into a new object in `d`. */
static yyjson_mut_val *preset_json(yyjson_mut_doc *d, const struct pc_export_preset *pr)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "name", pr->name);
    yyjson_mut_val *sr = pc_json_value_root(&pr->settings);
    yyjson_mut_obj_add_val(d, o, "settings", sr != NULL ? yyjson_mut_val_mut_copy(d, sr) : yyjson_mut_obj(d));
    return o;
}

/* ---- persistence (presets.h hooks) --------------------------------------------------------- */

void pc_engine_export_presets_fini(pc_preset_state *st)
{
    if (st == NULL) {
        return;
    }
    for (size_t i = 0; i < st->nexport_presets; i++) {
        preset_fini(&st->export_presets[i]);
    }
    free(st->export_presets);
    st->export_presets = NULL;
    st->nexport_presets = 0;
    st->cap_export_presets = 0;
}

void pc_engine_export_presets_to_json(const pc_preset_state *st, yyjson_mut_doc *d, yyjson_mut_val *root)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < st->nexport_presets; i++) {
        yyjson_mut_arr_append(arr, preset_json(d, &st->export_presets[i]));
    }
    yyjson_mut_obj_add_val(d, root, "export_presets", arr);
}

static pc_status push_preset(pc_preset_state *st, char *name, pc_json_value *settings, pc_error *err)
{
    struct pc_export_preset *grown =
        pc_grow_array(st->export_presets, &st->cap_export_presets, st->nexport_presets + 1,
                      sizeof *grown);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    st->export_presets = grown;
    st->export_presets[st->nexport_presets].name = name;
    st->export_presets[st->nexport_presets].settings = *settings;
    st->nexport_presets++;
    return PC_OK;
}

void pc_engine_export_presets_load(pc_preset_state *st, yyjson_val *v)
{
    yyjson_val *arr = v != NULL ? pc_json_get(v, "export_presets") : NULL;
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return;
    }
    pc_engine_export_presets_fini(st);
    size_t idx, max;
    yyjson_val *e;
    yyjson_arr_foreach(arr, idx, max, e) {
        if (st->nexport_presets >= MAX_PRESETS) {
            break;
        }
        if (!yyjson_is_obj(e)) {
            continue;
        }
        size_t nl = 0;
        const char *nm = pc_json_as_str(pc_json_get(e, "name"), &nl);
        char *name = nm != NULL ? checked_name(nm, nl) : NULL;
        if (name == NULL) {
            continue;
        }
        if (find_ci(st, name) != NULL) {
            free(name);
            continue;
        }
        pc_json_value settings = {0};
        const char *msg = NULL;
        if (!build_settings(pc_json_get(e, "settings"), &settings, &msg)) {
            free(name);
            continue;
        }
        if (push_preset(st, name, &settings, NULL) != PC_OK) {
            free(name);
            pc_json_value_fini(&settings);
        }
    }
}

/* ---- commands ------------------------------------------------------------------------------ */

static pc_status result_preset(pc_json_value *out, const struct pc_export_preset *pr, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_val(rd, root, "preset", preset_json(rd, pr));
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_preset_state *st = &s->presets;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < st->nexport_presets; i++) {
        yyjson_mut_arr_append(arr, preset_json(rd, &st->export_presets[i]));
    }
    yyjson_mut_obj_add_val(rd, root, "presets", arr);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const char *req_name(yyjson_val *p, size_t *len)
{
    return pc_json_as_str(p != NULL ? pc_json_get(p, "name") : NULL, len);
}

static pc_status run_get(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "file.export.presets.get";
    size_t nl = 0;
    const char *nm = req_name(p, &nl);
    if (nm == NULL) {
        return pc_engine_err_bad_params(err, CMD, "`name` is required");
    }
    char *name = pc_strndup(nm, nl);
    if (name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    struct pc_export_preset *pr = find_ci(&s->presets, name);
    free(name);
    if (pr == NULL) {
        return pc_engine_err_bad_params(err, CMD, "No such export preset");
    }
    return result_preset(out, pr, err);
}

static pc_status run_save(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "file.export.presets.save";
    size_t nl = 0;
    const char *nm = req_name(p, &nl);
    if (nm == NULL) {
        return pc_engine_err_bad_params(err, CMD, "`name` is required");
    }
    char *name = checked_name(nm, nl);
    if (name == NULL) {
        return pc_engine_err_bad_params(err, CMD,
                                        "Preset names must contain 1-255 characters after trimming");
    }
    pc_preset_state *st = &s->presets;
    struct pc_export_preset *existing = find_ci(st, name);
    if (existing == NULL && st->nexport_presets >= MAX_PRESETS) {
        free(name);
        return pc_engine_err_bad_params(err, CMD, "At most %u export presets can be saved",
                                        (unsigned)MAX_PRESETS);
    }
    pc_json_value settings = {0};
    const char *msg = NULL;
    if (!build_settings(p != NULL ? pc_json_get(p, "settings") : NULL, &settings, &msg)) {
        free(name);
        return pc_engine_err_bad_params(err, CMD, "%s", msg != NULL ? msg : "invalid settings");
    }
    if (existing != NULL) {
        free(existing->name); /* replace keeps the new casing, as the reference does */
        existing->name = name;
        pc_json_value_fini(&existing->settings);
        existing->settings = settings;
    } else {
        if (push_preset(st, name, &settings, err) != PC_OK) {
            free(name);
            pc_json_value_fini(&settings);
            return PC_ERR_NOMEM;
        }
        existing = &st->export_presets[st->nexport_presets - 1];
    }
    pc_engine_presets_changed(s);
    return result_preset(out, existing, err);
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "file.export.presets.delete";
    size_t nl = 0;
    const char *nm = req_name(p, &nl);
    if (nm == NULL) {
        return pc_engine_err_bad_params(err, CMD, "`name` is required");
    }
    char *name = pc_strndup(nm, nl);
    if (name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_preset_state *st = &s->presets;
    size_t at = st->nexport_presets;
    for (size_t i = 0; i < st->nexport_presets; i++) {
        if (strcasecmp(st->export_presets[i].name, name) == 0) {
            at = i;
            break;
        }
    }
    if (at == st->nexport_presets) {
        free(name);
        return pc_engine_err_bad_params(err, CMD, "No such export preset");
    }
    preset_fini(&st->export_presets[at]);
    memmove(&st->export_presets[at], &st->export_presets[at + 1],
            (st->nexport_presets - at - 1) * sizeof *st->export_presets);
    st->nexport_presets--;
    pc_engine_presets_changed(s);

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(name);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "deleted", name);
    free(name);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"file.export.presets.list", "List Export Presets", NULL, 0, NULL, "{} -> {presets}",
     pc_engine_always, run_list, false},
    {"file.export.presets.get", "Get Export Preset", NULL, 0, NULL,
     "{\"name\":str} -> {preset} (settings for Export As; nothing is exported)", pc_engine_always,
     run_get, false},
    {"file.export.presets.save", "Save Export Preset", NULL, 0, NULL,
     "{\"name\":str,\"settings\":{format,quality:1..100,lossless,transparency,scale:1..1000,width,"
     "height,lockAspect,sizing:scale|pixels,metadata:none|exif|all,profile:document|srgb,"
     "embedProfile,tgaBits:auto|24|32,resample}} (a saved name is replaced)",
     pc_engine_always, run_save, false},
    {"file.export.presets.delete", "Delete Export Preset", NULL, 0, NULL, "{\"name\":str}",
     pc_engine_always, run_delete, false},
};

const pc_engine_command *pc_engine_export_preset_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
