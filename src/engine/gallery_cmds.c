/* Filter > Filter Gallery (filter.filterGallery) and one command per gallery filter
 * (filter.gallery.<key>, not in menus) -- port of crates/engine/src/gallery_cmds.rs, E9.
 *
 * filter.filterGallery {effects:[{filter, params, visible}]} applies a stack of effect layers in
 * order. Every id goes through the shared filter runner (filters_cmds.c): selection, history,
 * Last Filter and -- once E6 lands -- smart objects all work. The foreground/background colours
 * the Sketch filters read are injected from the session, so the stored params are self-contained.
 * The algorithm is photocraft_algo's gallery (pc_algo_def_filter_gallery); never crashes. */
#include <stdlib.h>
#include <string.h>

#include "algo/artistic.h"
#include "algo/filter.h"
#include "base/mem.h"
#include "engine_internal.h"
#include "gallery_catalog.h"

/* ---- colour parsing ----------------------------------------------------------------------- */

/* Straight RGBA from [r,g,b(,a)] (0..1) or "#rrggbb"; false when v is neither. */
static bool colour_of(yyjson_val *v, float out[4])
{
    if (v == NULL) {
        return false;
    }
    if (yyjson_is_arr(v) && yyjson_arr_size(v) >= 3) {
        double def[4] = {0.0, 0.0, 0.0, 1.0};
        for (size_t j = 0; j < 4; j++) {
            yyjson_val *e = yyjson_arr_get(v, j);
            double n;
            out[j] = (e != NULL && pc_json_as_f64(e, &n)) ? (float)n : (float)def[j];
        }
        return true;
    }
    if (yyjson_is_str(v) && yyjson_get_len(v) == 7) {
        const char *h = yyjson_get_str(v);
        if (h == NULL || h[0] != '#') {
            return false;
        }
        for (int c = 0; c < 3; c++) {
            char a = h[1 + c * 2], b = h[2 + c * 2];
            int hi, lo;
            if (a >= '0' && a <= '9') {
                hi = a - '0';
            } else if (a >= 'a' && a <= 'f') {
                hi = a - 'a' + 10;
            } else if (a >= 'A' && a <= 'F') {
                hi = a - 'A' + 10;
            } else {
                hi = 0;
            }
            if (b >= '0' && b <= '9') {
                lo = b - '0';
            } else if (b >= 'a' && b <= 'f') {
                lo = b - 'a' + 10;
            } else if (b >= 'A' && b <= 'F') {
                lo = b - 'A' + 10;
            } else {
                lo = 0;
            }
            out[c] = (float)(hi * 16 + lo) / 255.0f;
        }
        out[3] = 1.0f;
        return true;
    }
    return false;
}

static bool pick_colour(yyjson_val *p, yyjson_val *fallback, const char *key, float out[4])
{
    yyjson_val *v = (p != NULL && yyjson_is_obj(p)) ? yyjson_obj_get(p, key) : NULL;
    if (colour_of(v, out)) {
        return true;
    }
    v = (fallback != NULL && yyjson_is_obj(fallback)) ? yyjson_obj_get(fallback, key) : NULL;
    return colour_of(v, out);
}

/* ---- one effect from its params object ---------------------------------------------------- */

static pc_status effect_from_json(pc_algo_gallery_filter f, yyjson_val *p, yyjson_val *fallback,
                                  pc_algo_gallery_effect *e, pc_error *err)
{
    pc_status rc = pc_algo_gallery_effect_new(f, e, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_algo_gallery_param params[PC_ALGO_GALLERY_MAX_PARAMS];
    size_t np = pc_algo_gallery_params(f, params, PC_ALGO_GALLERY_MAX_PARAMS);
    for (size_t i = 0; i < np; i++) {
        yyjson_val *v = (p != NULL && yyjson_is_obj(p)) ? yyjson_obj_get(p, params[i].key) : NULL;
        if (v == NULL) {
            continue;
        }
        if (params[i].kind == PC_ALGO_GALLERY_CHOICE && yyjson_is_str(v)) {
            bool ok = false;
            (void)pc_algo_gallery_effect_set_choice(e, params[i].key, yyjson_get_str(v), &ok, err);
        } else if (yyjson_is_bool(v)) {
            (void)pc_algo_gallery_effect_set(e, params[i].key, yyjson_get_bool(v) ? 1.0f : 0.0f, err);
        } else {
            double n;
            if (pc_json_as_f64(v, &n)) {
                (void)pc_algo_gallery_effect_set(e, params[i].key, (float)n, err);
            }
        }
    }
    float c[4];
    if (pick_colour(p, fallback, "foreground", c)) {
        memcpy(e->foreground, c, sizeof c);
    }
    if (pick_colour(p, fallback, "background", c)) {
        memcpy(e->background, c, sizeof c);
    }
    if (pick_colour(p, fallback, "glowColor", c)) {
        memcpy(e->color, c, sizeof c);
    }
    return PC_OK;
}

/* The effect stack of a filterGallery params object (NULL `effects` -> empty, not an error). */
static pc_status effects_from_json(yyjson_val *p, pc_algo_gallery_effect **out, size_t *count, pc_error *err)
{
    *out = NULL;
    *count = 0;
    yyjson_val *list = (p != NULL && yyjson_is_obj(p)) ? yyjson_obj_get(p, "effects") : NULL;
    if (list == NULL || !yyjson_is_arr(list)) {
        return PC_OK;
    }
    size_t n = yyjson_arr_size(list);
    if (n > 32) {
        return pc_fail(err, PC_ERR_INVALID, "too many effects in filterGallery stack (max 32)");
    }
    pc_algo_gallery_effect *eff = n > 0 ? pc_malloc_array(n, sizeof *eff) : NULL;
    if (n > 0 && eff == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "effects");
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        yyjson_val *item = yyjson_arr_get(list, i);
        yyjson_val *vis = (item != NULL && yyjson_is_obj(item)) ? yyjson_obj_get(item, "visible") : NULL;
        bool b;
        if (vis != NULL && pc_json_as_bool(vis, &b) && !b) {
            continue;
        }
        yyjson_val *fv = (item != NULL && yyjson_is_obj(item)) ? yyjson_obj_get(item, "filter") : NULL;
        if (fv == NULL || !yyjson_is_str(fv)) {
            for (size_t j = 0; j < k; j++) {
                pc_algo_gallery_effect_fini(&eff[j]);
            }
            free(eff);
            return pc_fail(err, PC_ERR_INVALID, "effect %zu: missing `filter`", i);
        }
        pc_algo_gallery_filter f;
        if (!pc_algo_gallery_filter_from_key(yyjson_get_str(fv), yyjson_get_len(fv), &f)) {
            for (size_t j = 0; j < k; j++) {
                pc_algo_gallery_effect_fini(&eff[j]);
            }
            free(eff);
            return pc_fail(err, PC_ERR_INVALID, "effect %zu: unknown gallery filter `%s`", i,
                           yyjson_get_str(fv));
        }
        yyjson_val *prm = yyjson_obj_get(item, "params");
        if (prm == NULL || !yyjson_is_obj(prm)) {
            prm = item;
        }
        if (effect_from_json(f, prm, p, &eff[k], err) != PC_OK) {
            for (size_t j = 0; j < k; j++) {
                pc_algo_gallery_effect_fini(&eff[j]);
            }
            free(eff);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        k++;
    }
    *out = eff;
    *count = k;
    return PC_OK;
}

/* ---- params_for (chained from filters_cmds.c) --------------------------------------------- */

static pc_status make_gallery(pc_algo_gallery_effect *eff, size_t n, pc_algo_filter *out, pc_error *err)
{
    pc_algo_filter_gallery_params gp;
    gp.effects.v = eff;
    gp.effects.n = n;
    return pc_algo_filter_make(&pc_algo_def_filter_gallery, &gp, out, err);
}

pc_status pc_engine_gallery_params_for(const char *id, yyjson_val *p, pc_algo_filter *out, bool *found,
                                       pc_error *err)
{
    *found = false;
    if (strcmp(id, "filter.filterGallery") == 0) {
        pc_algo_gallery_effect *eff = NULL;
        size_t n = 0;
        pc_status rc = effects_from_json(p, &eff, &n, err);
        if (rc != PC_OK) {
            return rc;
        }
        *found = true;
        rc = make_gallery(eff, n, out, err);
        for (size_t j = 0; j < n; j++) {
            pc_algo_gallery_effect_fini(&eff[j]);
        }
        free(eff);
        return rc;
    }
    if (strncmp(id, "filter.gallery.", 15) == 0) {
        pc_algo_gallery_filter f;
        if (!pc_algo_gallery_filter_from_key(id + 15, strlen(id + 15), &f)) {
            return PC_OK; /* unknown key: not a filter we own */
        }
        *found = true;
        pc_algo_gallery_effect e;
        pc_status rc = effect_from_json(f, p, NULL, &e, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = make_gallery(&e, 1, out, err);
        pc_algo_gallery_effect_fini(&e);
        return rc;
    }
    return PC_OK; /* not a gallery id */
}

/* ---- validation --------------------------------------------------------------------------- */

static bool ascii_ieq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static pc_status check_choices(pc_algo_gallery_filter f, yyjson_val *p, const char *cmd, pc_error *err)
{
    if (p == NULL || !yyjson_is_obj(p)) {
        return PC_OK;
    }
    pc_algo_gallery_param params[PC_ALGO_GALLERY_MAX_PARAMS];
    size_t np = pc_algo_gallery_params(f, params, PC_ALGO_GALLERY_MAX_PARAMS);
    for (size_t i = 0; i < np; i++) {
        if (params[i].kind != PC_ALGO_GALLERY_CHOICE) {
            continue;
        }
        yyjson_val *v = yyjson_obj_get(p, params[i].key);
        if (v == NULL) {
            continue;
        }
        bool ok;
        if (yyjson_is_str(v)) {
            ok = false;
            const char *name = yyjson_get_str(v);
            for (size_t c = 0; c < params[i].nchoices; c++) {
                if (ascii_ieq(params[i].choices[c], name)) {
                    ok = true;
                    break;
                }
            }
        } else {
            ok = yyjson_is_num(v); /* a choice index, clamped as the dialog stores it */
        }
        if (!ok) {
            char choices[PC_ALGO_GALLERY_MAX_CHOICES * (PC_ALGO_GALLERY_NAME_MAX + 1)];
            size_t off = 0;
            for (size_t c = 0; c < params[i].nchoices; c++) {
                int w =
                    snprintf(choices + off, sizeof choices - off, "%s%s", c ? "|" : "", params[i].choices[c]);
                if (w < 0 || (size_t)w >= sizeof choices - off) {
                    break;
                }
                off += (size_t)w;
            }
            return pc_engine_err_bad_params(err, cmd, "`%s` must be one of %s", params[i].key, choices);
        }
    }
    return PC_OK;
}

pc_status pc_engine_gallery_validate(const char *id, yyjson_val *p, pc_error *err)
{
    if (strcmp(id, "filter.filterGallery") == 0) {
        yyjson_val *list = (p != NULL && yyjson_is_obj(p)) ? yyjson_obj_get(p, "effects") : NULL;
        if (list == NULL || !yyjson_is_arr(list)) {
            return PC_OK;
        }
        size_t n = yyjson_arr_size(list);
        for (size_t i = 0; i < n; i++) {
            yyjson_val *item = yyjson_arr_get(list, i);
            yyjson_val *fv = (item != NULL && yyjson_is_obj(item)) ? yyjson_obj_get(item, "filter") : NULL;
            pc_algo_gallery_filter f;
            if (fv == NULL || !yyjson_is_str(fv) ||
                !pc_algo_gallery_filter_from_key(yyjson_get_str(fv), yyjson_get_len(fv), &f)) {
                continue;
            }
            yyjson_val *prm = yyjson_obj_get(item, "params");
            if (prm == NULL || !yyjson_is_obj(prm)) {
                prm = item;
            }
            pc_error e;
            pc_error_clear(&e);
            if (check_choices(f, prm, id, &e) != PC_OK) {
                return pc_engine_err_bad_params(err, id, "effect %zu: %s", i, e.msg);
            }
        }
        return PC_OK;
    }
    pc_algo_gallery_filter f;
    if (strncmp(id, "filter.gallery.", 15) == 0 &&
        pc_algo_gallery_filter_from_key(id + 15, strlen(id + 15), &f)) {
        return check_choices(f, p, id, err);
    }
    return PC_OK;
}

/* ---- the catalogue ------------------------------------------------------------------------ */

pc_status pc_engine_gallery_catalogue(pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *cats = yyjson_mut_arr(rd);
    for (size_t i = 0; i < 6; i++) {
        yyjson_mut_arr_add_str(rd, cats, pc_algo_gallery_categories[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "categories", cats);
    yyjson_mut_val *filters = yyjson_mut_arr(rd);
    for (int i = 0; i < PC_ALGO_GALLERY_FILTERS; i++) {
        pc_algo_gallery_filter f = (pc_algo_gallery_filter)i;
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_str(rd, o, "key", pc_algo_gallery_filter_key(f));
        yyjson_mut_obj_add_str(rd, o, "name", pc_algo_gallery_filter_name(f));
        yyjson_mut_obj_add_str(rd, o, "category", pc_algo_gallery_filter_category(f));
        yyjson_mut_obj_add_str(rd, o, "command", pc_algo_gallery_filter_command_id(f));
        yyjson_mut_obj_add_str(rd, o, "params", pc_algo_gallery_filter_params_doc(f));
        yyjson_mut_arr_add_val(filters, o);
    }
    yyjson_mut_obj_add_val(rd, root, "filters", filters);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- running ------------------------------------------------------------------------------ */

/* Inject the session foreground / background (as [r,g,b,a]) into a mutable params copy when the
 * gallery filter reads colours and the caller did not pass them (filters_ext::prepare). */
static void inject_colour(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, const float c[4])
{
    if (yyjson_mut_obj_get(obj, key) != NULL) {
        return;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (int i = 0; i < 4; i++) {
        yyjson_mut_arr_add_real(d, arr, (double)c[i]);
    }
    yyjson_mut_obj_add_val(d, obj, key, arr);
}

static bool id_uses_colours(const char *id)
{
    if (strcmp(id, "filter.filterGallery") == 0) {
        return true;
    }
    pc_algo_gallery_filter f;
    if (strncmp(id, "filter.gallery.", 15) == 0 &&
        pc_algo_gallery_filter_from_key(id + 15, strlen(id + 15), &f)) {
        return pc_algo_gallery_filter_uses_colours(f);
    }
    return false;
}

/* Normalise params (drop "list", inject colours) and run through the shared filter runner. */
static pc_status run_gallery_id(pc_engine_session *s, const char *id, yyjson_val *p, pc_json_value *out,
                                pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "filterGallery");
    }
    yyjson_mut_val *obj;
    if (p != NULL && yyjson_is_obj(p)) {
        obj = yyjson_val_mut_copy(d, p);
        if (obj == NULL || !yyjson_mut_is_obj(obj)) {
            obj = yyjson_mut_obj(d);
        }
    } else {
        obj = yyjson_mut_obj(d);
    }
    yyjson_mut_obj_remove_key(obj, "list");
    if (id_uses_colours(id)) {
        inject_colour(d, obj, "foreground", s->tools.foreground);
        inject_colour(d, obj, "background", s->tools.background);
    }
    yyjson_mut_doc_set_root(d, obj);
    yyjson_doc *imm = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    if (imm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "filterGallery");
    }
    pc_status rc = pc_engine_run_filter(s, id, yyjson_doc_get_root(imm), out, err);
    yyjson_doc_free(imm);
    return rc;
}

static pc_status run_filter_gallery(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_val *lv = (p != NULL && yyjson_is_obj(p)) ? yyjson_obj_get(p, "list") : NULL;
    bool list;
    if (lv != NULL && pc_json_as_bool(lv, &list) && list) {
        return pc_engine_gallery_catalogue(out, err);
    }
    pc_algo_gallery_effect *eff = NULL;
    size_t n = 0;
    pc_status rc = effects_from_json(p, &eff, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t j = 0; j < n; j++) {
        pc_algo_gallery_effect_fini(&eff[j]);
    }
    free(eff);
    if (n == 0) {
        return pc_engine_err_other(
            err, "filter.filterGallery needs `effects`: [{\"filter\": \"<key>\", \"params\": {...}}] "
                 "(pass {\"list\": true} for the catalogue)");
    }
    return run_gallery_id(s, "filter.filterGallery", p, out, err);
}

/* ---- specs -------------------------------------------------------------------------------- *
 *
 * run() receives only (s, p, out, err), so each filter.gallery.<key> command needs its own run
 * thunk that knows its id (like filters_cmds.c's FILTER_RUN). One thunk per gallery filter,
 * indexed by the enum value, keeps the ids in lockstep with photocraft_algo. */
static const char *const MENU_FILTER[] = {"Filter"};

#define GAL_THUNK(i)                                                                                         \
    static pc_status gal_run_##i(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)     \
    {                                                                                                        \
        return run_gallery_id(s, pc_algo_gallery_filter_command_id((pc_algo_gallery_filter)(i)), p, out,     \
                              err);                                                                          \
    }

GAL_THUNK(0)
GAL_THUNK(1) GAL_THUNK(2) GAL_THUNK(3) GAL_THUNK(4) GAL_THUNK(5) GAL_THUNK(6) GAL_THUNK(7) GAL_THUNK(8)
    GAL_THUNK(9) GAL_THUNK(10) GAL_THUNK(11) GAL_THUNK(12) GAL_THUNK(13) GAL_THUNK(14) GAL_THUNK(15)
        GAL_THUNK(16) GAL_THUNK(17) GAL_THUNK(18) GAL_THUNK(19) GAL_THUNK(20) GAL_THUNK(21) GAL_THUNK(22)
            GAL_THUNK(23) GAL_THUNK(24) GAL_THUNK(25) GAL_THUNK(26) GAL_THUNK(27) GAL_THUNK(28) GAL_THUNK(29)
                GAL_THUNK(30) GAL_THUNK(31) GAL_THUNK(32) GAL_THUNK(33) GAL_THUNK(34) GAL_THUNK(35)
                    GAL_THUNK(36) GAL_THUNK(37) GAL_THUNK(38) GAL_THUNK(39) GAL_THUNK(40) GAL_THUNK(41)
                        GAL_THUNK(42) GAL_THUNK(43) GAL_THUNK(44) GAL_THUNK(45) GAL_THUNK(46)

                            static const pc_engine_run_fn GAL_THUNKS[PC_ALGO_GALLERY_FILTERS] = {
                                gal_run_0,  gal_run_1,  gal_run_2,  gal_run_3,  gal_run_4,  gal_run_5,
                                gal_run_6,  gal_run_7,  gal_run_8,  gal_run_9,  gal_run_10, gal_run_11,
                                gal_run_12, gal_run_13, gal_run_14, gal_run_15, gal_run_16, gal_run_17,
                                gal_run_18, gal_run_19, gal_run_20, gal_run_21, gal_run_22, gal_run_23,
                                gal_run_24, gal_run_25, gal_run_26, gal_run_27, gal_run_28, gal_run_29,
                                gal_run_30, gal_run_31, gal_run_32, gal_run_33, gal_run_34, gal_run_35,
                                gal_run_36, gal_run_37, gal_run_38, gal_run_39, gal_run_40, gal_run_41,
                                gal_run_42, gal_run_43, gal_run_44, gal_run_45, gal_run_46};

#define GALLERY_SPEC_CAP (PC_ALGO_GALLERY_FILTERS + 1)
static pc_engine_command g_specs[GALLERY_SPEC_CAP];
static bool g_built;

const pc_engine_command *pc_engine_gallery_specs(size_t *n)
{
    if (!g_built) {
        g_specs[0] =
            (pc_engine_command){"filter.filterGallery",
                                "Filter Gallery\xE2\x80\xA6",
                                MENU_FILTER,
                                1,
                                NULL,
                                "{\"effects\":json,\"foreground\":json,\"background\":json,\"list\":bool}",
                                pc_engine_filterable,
                                run_filter_gallery,
                                true};
        for (int i = 0; i < PC_ALGO_GALLERY_FILTERS; i++) {
            pc_algo_gallery_filter f = (pc_algo_gallery_filter)i;
            g_specs[i + 1] = (pc_engine_command){pc_algo_gallery_filter_command_id(f),
                                                 pc_algo_gallery_filter_name(f),
                                                 NULL,
                                                 0,
                                                 NULL,
                                                 pc_algo_gallery_filter_params_doc(f),
                                                 pc_engine_filterable,
                                                 GAL_THUNKS[i],
                                                 true};
        }
        g_built = true;
    }
    *n = GALLERY_SPEC_CAP;
    return g_specs;
}
