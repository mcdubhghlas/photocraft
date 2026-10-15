/* Patterns: Edit > Define Pattern, the pattern library (pattern.*), the Patterns panel
 * (pattern.presets.*), Layer > New Fill Layer > Pattern, Edit > Fill with a pattern and brush
 * textures from library patterns (port of crates/engine/src/pattern_cmds.rs and
 * crates/engine/src/presets/patterns.rs, E10).
 *
 * The app-level pattern library (pc_engine_session.patterns) holds built-in procedural patterns
 * plus defined/imported ones; each document also owns the patterns its layers use. A pattern is
 * referenced by "pattern": id | name (document first, then the library). Commands that apply a
 * pattern copy it into the document in the same history step so the document renders on its own. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/file.h"
#include "base/json.h"
#include "base/mem.h"
#include "color/color.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "doc/fill.h"
#include "doc/pattern.h"
#include "geom/geom.h"
#include "io/pattern_map.h"
#include "paint/brush.h"
#include "paint/tile.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "engine_internal.h"
#include "pattern_cmds.h"
#include "presets.h"

/* ========================================================================================== */
/* built-in procedural patterns (pattern_cmds::builtin; our own, CC0; deterministic ids)       */
/* ========================================================================================== */

static float hash01(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h = h * 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xffffu) / 65535.0f;
}

typedef void (*gen_fn)(uint32_t x, uint32_t y, float out[3]);

static void gen_checkerboard(uint32_t x, uint32_t y, float o[3])
{
    float v = ((x / 8 + y / 8) % 2 == 0) ? 0.8f : 1.0f;
    o[0] = o[1] = o[2] = v;
}
static void gen_diagonal(uint32_t x, uint32_t y, float o[3])
{
    float v = ((x + y) % 8 < 2) ? 0.2f : 1.0f;
    o[0] = o[1] = o[2] = v;
}
static void gen_dots(uint32_t x, uint32_t y, float o[3])
{
    float dx = (float)x + 0.5f - 6.0f, dy = (float)y + 0.5f - 6.0f;
    float d = sqrtf(dx * dx + dy * dy) - 3.0f;
    d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
    o[0] = o[1] = o[2] = d;
}
static void gen_grid(uint32_t x, uint32_t y, float o[3])
{
    float v = (x == 0 || y == 0) ? 0.55f : 1.0f;
    o[0] = o[1] = o[2] = v;
}
static void gen_bricks(uint32_t x, uint32_t y, float o[3])
{
    uint32_t row = y / 8;
    uint32_t xo = (x + (row % 2 == 1 ? 8u : 0u)) % 16;
    bool mortar = (y % 8 == 7) || (xo == 15);
    float n = hash01(x, y, 7) * 0.08f;
    if (mortar) {
        o[0] = 0.78f + n;
        o[1] = 0.76f + n;
        o[2] = 0.72f + n;
    } else {
        o[0] = 0.62f + n;
        o[1] = 0.27f + n * 0.5f;
        o[2] = 0.2f;
    }
}
static void gen_weave(uint32_t x, uint32_t y, float o[3])
{
    bool over = ((x / 4) + (y / 4)) % 2 == 0;
    float t = over ? (float)(y % 4) / 3.0f : (float)(x % 4) / 3.0f;
    float v = 0.55f + 0.35f * (1.0f - fabsf(2.0f * t - 1.0f));
    o[0] = v;
    o[1] = v * 0.92f;
    o[2] = v * 0.8f;
}
static float paper_oct(uint32_t x, uint32_t y, uint32_t cell, uint32_t seed)
{
    uint32_t cx = x / cell, cy = y / cell;
    float fx = (float)(x % cell) / (float)cell, fy = (float)(y % cell) / (float)cell;
    uint32_t n = 64 / cell;
    float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    float v00 = hash01(cx % n, cy % n, seed);
    float v10 = hash01((cx + 1) % n, cy % n, seed);
    float v01 = hash01(cx % n, (cy + 1) % n, seed);
    float v11 = hash01((cx + 1) % n, (cy + 1) % n, seed);
    float a = v00 + (v10 - v00) * sx;
    float b = v01 + (v11 - v01) * sx;
    return a + (b - a) * sy;
}
static void gen_paper(uint32_t x, uint32_t y, float o[3])
{
    float v = 0.78f + 0.14f * paper_oct(x, y, 8, 1) + 0.08f * paper_oct(x, y, 2, 2);
    o[0] = o[1] = o[2] = v;
}

static pc_status make_procedural(const char *name, uint32_t w, uint32_t h, gen_fn f, pc_pattern *out,
                                 pc_error *err)
{
    size_t n = (size_t)w * (size_t)h * 3;
    float *px = pc_malloc_array(n, sizeof *px);
    if (px == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "pattern");
    }
    size_t i = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            float c[3];
            f(x, y, c);
            px[i++] = c[0];
            px[i++] = c[1];
            px[i++] = c[2];
        }
    }
    pc_surface surf;
    pc_surface_init(&surf, (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false});
    pc_status rc = pc_surface_write_region(&surf, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), px, n, err);
    free(px);
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
        return rc;
    }
    rc = pc_pattern_new(name, strlen(name), &surf, w, h, out, err); /* takes over surf on success */
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
    }
    return rc;
}

pc_status pc_engine_patterns_builtin(pc_pattern **out, size_t *n, pc_error *err)
{
    static const struct {
        const char *name;
        uint32_t w, h;
        gen_fn f;
    } defs[] = {
        {"Checkerboard", 16, 16, gen_checkerboard}, {"Diagonal Lines", 8, 8, gen_diagonal},
        {"Dots", 12, 12, gen_dots},                 {"Grid", 16, 16, gen_grid},
        {"Bricks", 32, 16, gen_bricks},             {"Weave", 16, 16, gen_weave},
        {"Paper Noise", 64, 64, gen_paper},
    };
    size_t count = sizeof defs / sizeof defs[0];
    pc_pattern *v = pc_calloc_array(count, sizeof *v);
    if (v == NULL) {
        *out = NULL;
        *n = 0;
        return pc_fail(err, PC_ERR_NOMEM, "pattern library");
    }
    size_t built = 0;
    for (size_t i = 0; i < count; i++) {
        if (make_procedural(defs[i].name, defs[i].w, defs[i].h, defs[i].f, &v[built], err) != PC_OK) {
            break; /* leave a short-but-valid library (never crashes) */
        }
        built++;
    }
    *out = v;
    *n = built;
    return PC_OK;
}

/* ========================================================================================== */
/* session pattern library                                                                     */
/* ========================================================================================== */

void pc_engine_pattern_library_fini(pc_engine_session *s)
{
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->npatterns; i++) {
        pc_pattern_fini(&s->patterns[i]);
    }
    free(s->patterns);
    s->patterns = NULL;
    s->npatterns = 0;
    s->cap_patterns = 0;
}

static void group_push_id(pc_preset_groups *groups, const char *group_name, const char *id)
{
    size_t gi = 0;
    bool found = false;
    for (size_t i = 0; i < groups->n; i++) {
        if (groups->v[i].name != NULL && strcmp(groups->v[i].name, group_name) == 0) {
            gi = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    char *copy = NULL;
    size_t len = strlen(id);
    copy = pc_malloc_array(len + 1, 1);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, id, len + 1);
    pc_error err = {0};
    if (pc_preset_group_push(groups, gi, &copy, pc_preset_string_ops(), &err) != PC_OK) {
        free(copy);
    }
}

void pc_engine_pattern_library_init(pc_engine_session *s)
{
    pc_error err = {0};
    pc_pattern *v = NULL;
    size_t n = 0;
    (void)pc_engine_patterns_builtin(&v, &n, &err);
    s->patterns = v;
    s->npatterns = n;
    s->cap_patterns = n;
    /* Fill the Patterns-panel default folders with the matching built-in ids
     * (presets::patterns::builtin_groups). */
    static const struct {
        const char *group;
        const char *names[4];
    } folders[] = {
        {"Geometric", {"Checkerboard", "Diagonal Lines", "Dots", "Grid"}},
        {"Textures", {"Bricks", "Weave", "Paper Noise"}},
    };
    for (size_t fi = 0; fi < sizeof folders / sizeof folders[0]; fi++) {
        for (size_t ni = 0; ni < 4; ni++) {
            const char *name = folders[fi].names[ni];
            if (name == NULL) {
                continue;
            }
            for (size_t pi = 0; pi < s->npatterns; pi++) {
                if (strcmp(pc_str_c(&s->patterns[pi].name), name) == 0) {
                    group_push_id(&s->presets.pattern_groups, folders[fi].group,
                                  pc_str_c(&s->patterns[pi].id));
                    break;
                }
            }
        }
    }
}

/* Grow the library and append pat (moved; caller must not fini it afterwards). */
static pc_status library_push(pc_engine_session *s, pc_pattern *pat, pc_error *err)
{
    pc_pattern *grown = pc_grow_array(s->patterns, &s->cap_patterns, s->npatterns + 1, sizeof *s->patterns);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "pattern library");
    }
    s->patterns = grown;
    s->patterns[s->npatterns++] = *pat;
    return PC_OK;
}

/* ========================================================================================== */
/* resolution helpers                                                                          */
/* ========================================================================================== */

static const char *str_param(yyjson_val *p, const char *k, size_t *len)
{
    yyjson_val *v = pc_json_get(p, k);
    return v != NULL ? pc_json_as_str(v, len) : NULL;
}

/* The Patterns-panel selection, if it is still in the library. */
static const pc_pattern *panel_current(const pc_engine_session *s)
{
    if (!s->presets.has_pattern || s->presets.pattern == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < s->npatterns; i++) {
        if (strcmp(pc_str_c(&s->patterns[i].id), s->presets.pattern) == 0) {
            return &s->patterns[i];
        }
    }
    return NULL;
}

/* Borrowed pattern by id or name: active document first, then library. An empty key picks the
 * panel selection, else the first library pattern. NULL when none. */
static const pc_pattern *resolve_borrow(const pc_engine_session *s, const char *key, size_t keylen)
{
    if (keylen == 0) {
        const pc_pattern *cur = panel_current(s);
        if (cur != NULL) {
            return cur;
        }
        return s->npatterns > 0 ? &s->patterns[0] : NULL;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL) {
        const pc_document *doc = pc_doc_arc_get(d->doc);
        const pc_pattern *p = pc_pattern_find(doc->patterns, doc->npatterns, key, keylen, key, keylen);
        if (p != NULL) {
            return p;
        }
    }
    return pc_pattern_find(s->patterns, s->npatterns, key, keylen, key, keylen);
}

pc_status pc_engine_pattern_resolve(const pc_engine_session *s, const char *key, size_t keylen,
                                    pc_pattern *out, bool *found, pc_error *err)
{
    const pc_pattern *p = resolve_borrow(s, key, keylen);
    if (p == NULL) {
        *found = false;
        return PC_OK;
    }
    pc_status rc = pc_pattern_clone(p, out, err);
    if (rc != PC_OK) {
        *found = false;
        return rc;
    }
    *found = true;
    return PC_OK;
}

pc_status pc_engine_pattern_resolve_param(const pc_engine_session *s, const char *cmd, yyjson_val *p,
                                          pc_pattern *out, pc_error *err)
{
    size_t klen = 0;
    const char *key = str_param(p, "pattern", &klen);
    if (key == NULL) {
        key = "";
        klen = 0;
    }
    bool found = false;
    pc_status rc = pc_engine_pattern_resolve(s, key, klen, out, &found, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!found) {
        return pc_engine_err_bad_params(err, cmd, "no pattern \"%.*s\" (see pattern.list)", (int)klen, key);
    }
    return PC_OK;
}

void pc_engine_pattern_placement(yyjson_val *p, float *scale, float *angle, bool *link, float phase[2])
{
    double v;
    yyjson_val *sv = pc_json_get(p, "scale");
    double sc = (sv != NULL && pc_json_as_f64(sv, &v)) ? v : 100.0;
    float s = (float)(sc / 100.0);
    if (!(s >= 0.01f)) {
        s = 0.01f;
    } else if (s > 10.0f) {
        s = 10.0f;
    }
    *scale = s;
    yyjson_val *av = pc_json_get(p, "angle");
    *angle = (av != NULL && pc_json_as_f64(av, &v)) ? (float)v : 0.0f;
    bool lk;
    yyjson_val *lv = pc_json_get(p, "link");
    *link = (lv != NULL && pc_json_as_bool(lv, &lk)) ? lk : true;
    phase[0] = 0.0f;
    phase[1] = 0.0f;
    yyjson_val *ph = pc_json_get(p, "phase");
    if (ph != NULL && yyjson_is_arr(ph)) {
        yyjson_val *e0 = yyjson_arr_get(ph, 0), *e1 = yyjson_arr_get(ph, 1);
        if (e0 != NULL && pc_json_as_f64(e0, &v)) {
            phase[0] = (float)v;
        }
        if (e1 != NULL && pc_json_as_f64(e1, &v)) {
            phase[1] = (float)v;
        }
    }
}

pc_status pc_engine_pattern_ensure_in_doc(pc_document *doc, const pc_pattern *pat, pc_error *err)
{
    for (size_t i = 0; i < doc->npatterns; i++) {
        if (pc_str_eq(&doc->patterns[i].id, &pat->id)) {
            return PC_OK;
        }
    }
    pc_pattern copy;
    pc_status rc = pc_pattern_clone(pat, &copy, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t cap = doc->npatterns;
    pc_pattern *grown = pc_grow_array(doc->patterns, &cap, doc->npatterns + 1, sizeof *doc->patterns);
    if (grown == NULL) {
        pc_pattern_fini(&copy);
        return pc_fail(err, PC_ERR_NOMEM, "document patterns");
    }
    doc->patterns = grown;
    doc->patterns[doc->npatterns++] = copy;
    return PC_OK;
}

/* ========================================================================================== */
/* enabled predicates                                                                          */
/* ========================================================================================== */

static pc_status always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

static pc_status has_pattern(const pc_engine_session *s, pc_error *reason)
{
    if (s->npatterns > 0) {
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && pc_doc_arc_get(d->doc)->npatterns > 0) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "no patterns");
}

/* ========================================================================================== */
/* JSON helpers                                                                                */
/* ========================================================================================== */

static pc_status pattern_info(yyjson_mut_doc *d, yyjson_mut_val *root, const pc_pattern *p, bool in_doc,
                              bool in_library)
{
    pc_pixel_format f = pc_surface_format(&p->surface);
    const char *idp = pc_str_c(&p->id);
    yyjson_mut_obj_add_strcpy(d, root, "id", idp);
    size_t dlen = 0;
    const char *disp = pc_pattern_display_name(pc_str_c(&p->name), p->name.len, &dlen);
    yyjson_mut_obj_add_strncpy(d, root, "name", disp, dlen);
    yyjson_mut_obj_add_strcpy(d, root, "storedName", pc_str_c(&p->name));
    yyjson_mut_obj_add_uint(d, root, "width", p->width);
    yyjson_mut_obj_add_uint(d, root, "height", p->height);
    yyjson_mut_obj_add_str(d, root, "mode", pc_color_mode_name(f.mode));
    yyjson_mut_obj_add_str(d, root, "depth", pc_sample_type_name(f.sample));
    yyjson_mut_obj_add_bool(d, root, "alpha", f.alpha);
    yyjson_mut_obj_add_bool(d, root, "inDocument", in_doc);
    yyjson_mut_obj_add_bool(d, root, "inLibrary", in_library);
    return PC_OK;
}

static bool doc_has_pattern_id(const pc_document *doc, const pc_str *id)
{
    for (size_t i = 0; i < doc->npatterns; i++) {
        if (pc_str_eq(&doc->patterns[i].id, id)) {
            return true;
        }
    }
    return false;
}
static bool lib_has_pattern_id(const pc_engine_session *s, const pc_str *id)
{
    for (size_t i = 0; i < s->npatterns; i++) {
        if (pc_str_eq(&s->patterns[i].id, id)) {
            return true;
        }
    }
    return false;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    const pc_engine_doc_state *ds = pc_engine_session_active(s);
    const pc_document *doc = ds != NULL ? pc_doc_arc_get(ds->doc) : NULL;
    for (size_t i = 0; i < s->npatterns; i++) {
        bool in_doc = doc != NULL && doc_has_pattern_id(doc, &s->patterns[i].id);
        yyjson_mut_val *row = yyjson_mut_obj(d);
        (void)pattern_info(d, row, &s->patterns[i], in_doc, true);
        yyjson_mut_arr_append(arr, row);
    }
    if (doc != NULL) {
        for (size_t i = 0; i < doc->npatterns; i++) {
            if (lib_has_pattern_id(s, &doc->patterns[i].id)) {
                continue;
            }
            yyjson_mut_val *row = yyjson_mut_obj(d);
            (void)pattern_info(d, row, &doc->patterns[i], true, false);
            yyjson_mut_arr_append(arr, row);
        }
    }
    yyjson_mut_doc_set_root(d, arr);
    return PC_OK;
}

/* ========================================================================================== */
/* Define Pattern                                                                              */
/* ========================================================================================== */

/* The visible composite over r as a surface in the document's pixel format (with alpha). Writes
 * *out (fini it) on success. */
static pc_status composite_surface(const pc_document *doc, pc_rect r, pc_surface *out, pc_error *err)
{
    pc_compose_buffer buf = {0};
    pc_status rc = pc_compose_render(doc, r, &buf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_pixel_format df = pc_document_pixel_format(doc);
    pc_pixel_format fmt = {df.mode, df.sample, true};
    size_t nch = pc_pixel_format_channels(fmt);
    size_t total = buf.len * nch;
    float *px = total > 0 ? pc_malloc_array(total, sizeof *px) : NULL;
    if (total > 0 && px == NULL) {
        pc_compose_buffer_fini(&buf);
        return pc_fail(err, PC_ERR_NOMEM, "pattern");
    }
    for (size_t i = 0; px != NULL && i < buf.len; i++) {
        float tmp[PC_RASTER_MAX_CHANNELS];
        size_t w = pc_from_rgba(fmt, buf.px[i], tmp);
        if (w > nch) {
            w = nch;
        }
        memcpy(&px[i * nch], tmp, w * sizeof *tmp);
    }
    pc_compose_buffer_fini(&buf);
    pc_surface_init(out, fmt);
    rc = pc_surface_write_region(out, pc_rect_make(0, 0, (int32_t)pc_rect_width(r), (int32_t)pc_rect_height(r)),
                                 px, total, err);
    free(px);
    if (rc != PC_OK) {
        pc_surface_fini(out);
    }
    return rc;
}

/* The default Define Pattern name: the document's file stem ("Pattern" with none), numbered past
 * the library's existing names. *out is owned (free()). */
static char *default_name(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const char *base = "Pattern";
    char stem[256];
    if (d != NULL) {
        const char *name = pc_str_c(&pc_doc_arc_get(d->doc)->name);
        const char *slash = name;
        for (const char *c = name; *c != '\0'; c++) {
            if (*c == '/' || *c == '\\') {
                slash = c + 1;
            }
        }
        const char *dot = NULL;
        for (const char *c = slash; *c != '\0'; c++) {
            if (*c == '.') {
                dot = c;
            }
        }
        size_t len = dot != NULL ? (size_t)(dot - slash) : strlen(slash);
        if (len > 0 && len < sizeof stem) {
            memcpy(stem, slash, len);
            stem[len] = '\0';
            base = stem;
        }
    }
    bool taken = false;
    for (size_t i = 0; i < s->npatterns; i++) {
        if (strcmp(pc_str_c(&s->patterns[i].name), base) == 0) {
            taken = true;
            break;
        }
    }
    if (!taken) {
        char *r = pc_malloc_array(strlen(base) + 1, 1);
        if (r != NULL) {
            memcpy(r, base, strlen(base) + 1);
        }
        return r;
    }
    for (int i = 2; i < 100000; i++) {
        char cand[300];
        snprintf(cand, sizeof cand, "%s %d", base, i);
        bool t = false;
        for (size_t k = 0; k < s->npatterns; k++) {
            if (strcmp(pc_str_c(&s->patterns[k].name), cand) == 0) {
                t = true;
                break;
            }
        }
        if (!t) {
            char *r = pc_malloc_array(strlen(cand) + 1, 1);
            if (r != NULL) {
                memcpy(r, cand, strlen(cand) + 1);
            }
            return r;
        }
    }
    char *r = pc_malloc_array(strlen(base) + 1, 1);
    if (r != NULL) {
        memcpy(r, base, strlen(base) + 1);
    }
    return r;
}

static pc_status run_define(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "edit.definePattern";
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_rect bounds = pc_document_bounds(doc);
    pc_rect r;
    yyjson_val *rect = pc_json_get(p, "rect");
    if (rect != NULL && yyjson_is_arr(rect) && yyjson_arr_size(rect) == 4) {
        int32_t v[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < 4; i++) {
            double f;
            yyjson_val *e = yyjson_arr_get(rect, i);
            if (e != NULL && pc_json_as_f64(e, &f) && isfinite(f)) {
                double rr = round(f);
                if (rr < (double)INT32_MIN) {
                    rr = (double)INT32_MIN;
                } else if (rr > (double)INT32_MAX) {
                    rr = (double)INT32_MAX;
                }
                v[i] = (int32_t)rr;
            }
        }
        r = pc_rect_make(v[0], v[1], v[2], v[3]);
    } else if (doc->has_selection) {
        r = pc_surface_content_bounds(&doc->selection);
    } else {
        r = bounds;
    }
    r = pc_rect_intersect(r, bounds);
    if (pc_rect_is_empty(r)) {
        return pc_engine_err_bad_params(err, cmd, "the area is empty");
    }
    if (pc_rect_width(r) > 4000 || pc_rect_height(r) > 4000) {
        return pc_engine_err_bad_params(err, cmd, "patterns are limited to 4000 x 4000 px");
    }
    pc_surface surf;
    pc_status rc = composite_surface(doc, r, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Opaque patterns drop their alpha channel (Photoshop). */
    pc_pixel_format fmt = pc_surface_format(&surf);
    size_t nch = pc_pixel_format_channels(fmt);
    float *rd = NULL;
    size_t rdlen = 0;
    if (pc_surface_read_region(&surf, pc_rect_make(0, 0, (int32_t)pc_rect_width(r), (int32_t)pc_rect_height(r)),
                               &rd, &rdlen, err) != PC_OK) {
        pc_surface_fini(&surf);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    bool opaque = fmt.alpha && nch > 0;
    if (opaque) {
        for (size_t i = nch - 1; i < rdlen; i += nch) {
            if (rd[i] < 1.0f) {
                opaque = false;
                break;
            }
        }
    }
    free(rd);
    if (opaque) {
        pc_pixel_format noalpha = {fmt.mode, fmt.sample, false};
        pc_surface conv;
        if (pc_surface_convert(&surf, noalpha, &conv, err) != PC_OK) {
            pc_surface_fini(&surf);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_surface_fini(&surf);
        surf = conv;
    }
    size_t nmlen = 0;
    const char *nm = str_param(p, "name", &nmlen);
    char *owned = NULL;
    const char *name = NULL;
    if (nm != NULL) {
        /* Trim to check for an all-whitespace name. */
        bool blank = true;
        for (size_t i = 0; i < nmlen; i++) {
            if (nm[i] != ' ' && nm[i] != '\t' && nm[i] != '\n' && nm[i] != '\r') {
                blank = false;
                break;
            }
        }
        if (!blank) {
            owned = pc_malloc_array(nmlen + 1, 1);
            if (owned == NULL) {
                pc_surface_fini(&surf);
                return pc_fail(err, PC_ERR_NOMEM, "pattern");
            }
            memcpy(owned, nm, nmlen);
            owned[nmlen] = '\0';
            name = owned;
        }
    }
    if (name == NULL) {
        owned = default_name(s);
        if (owned == NULL) {
            pc_surface_fini(&surf);
            return pc_fail(err, PC_ERR_NOMEM, "pattern");
        }
        name = owned;
    }
    pc_pattern pat = {0};
    rc = pc_pattern_new(name, strlen(name), &surf, pc_rect_width(r), pc_rect_height(r), &pat, err);
    free(owned);
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
        return rc;
    }
    /* Same content -> same id; don't duplicate. */
    bool exists = lib_has_pattern_id(s, &pat.id);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_pattern_fini(&pat);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "pattern", pc_str_c(&pat.id));
    yyjson_mut_val *info = yyjson_mut_obj(d);
    (void)pattern_info(d, info, &pat, false, true);
    yyjson_mut_obj_add_val(d, root, "info", info);
    yyjson_mut_doc_set_root(d, root);
    if (!exists) {
        rc = library_push(s, &pat, err);
        if (rc != PC_OK) {
            pc_pattern_fini(&pat);
            pc_json_value_fini(out);
            return rc;
        }
    } else {
        pc_pattern_fini(&pat);
    }
    return PC_OK;
}

/* ========================================================================================== */
/* library edits                                                                               */
/* ========================================================================================== */

/* Index of the library pattern named by "pattern" (id or name). */
static pc_status library_index(pc_engine_session *s, const char *cmd, yyjson_val *p, size_t *out,
                               pc_error *err)
{
    size_t klen = 0;
    const char *key = str_param(p, "pattern", &klen);
    if (key == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `pattern` (id or name)");
    }
    const pc_pattern *f = pc_pattern_find(s->patterns, s->npatterns, key, klen, key, klen);
    if (f == NULL) {
        return pc_engine_err_bad_params(err, cmd, "no library pattern \"%.*s\"", (int)klen, key);
    }
    for (size_t i = 0; i < s->npatterns; i++) {
        if (&s->patterns[i] == f) {
            *out = i;
            return PC_OK;
        }
    }
    *out = 0;
    return PC_OK;
}

struct rename_ctx {
    pc_str id;
    const char *name;
};
static pc_status edit_rename(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct rename_ctx *c = vctx;
    for (size_t i = 0; i < doc->npatterns; i++) {
        if (pc_str_eq(&doc->patterns[i].id, &c->id)) {
            if (pc_str_set_c(&doc->patterns[i].name, c->name, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        }
    }
    return PC_OK;
}

static pc_status run_rename(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "pattern.rename";
    size_t nlen = 0;
    const char *name = str_param(p, "name", &nlen);
    bool blank = name == NULL;
    for (size_t i = 0; name != NULL && i < nlen; i++) {
        if (name[i] != ' ' && name[i] != '\t' && name[i] != '\n' && name[i] != '\r') {
            blank = false;
            break;
        }
    }
    if (name == NULL || blank || nlen == 0) {
        return pc_engine_err_bad_params(err, cmd, "missing `name`");
    }
    size_t i = 0;
    pc_status rc = library_index(s, cmd, p, &i, err);
    if (rc != PC_OK) {
        return rc;
    }
    char *nm = pc_malloc_array(nlen + 1, 1);
    if (nm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "pattern");
    }
    memcpy(nm, name, nlen);
    nm[nlen] = '\0';
    pc_str idcopy = {0};
    if (pc_str_set(&idcopy, pc_str_c(&s->patterns[i].id), s->patterns[i].id.len, err) != PC_OK) {
        free(nm);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (pc_str_set_c(&s->patterns[i].name, nm, err) != PC_OK) {
        pc_str_fini(&idcopy);
        free(nm);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    const pc_engine_doc_state *ds = pc_engine_session_active(s);
    if (ds != NULL && doc_has_pattern_id(pc_doc_arc_get(ds->doc), &idcopy)) {
        struct rename_ctx ctx = {idcopy, nm};
        rc = pc_engine_session_edit(s, "Rename Pattern", edit_rename, &ctx, err);
        if (rc != PC_OK) {
            pc_str_fini(&idcopy);
            free(nm);
            return rc;
        }
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_str_fini(&idcopy);
        free(nm);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "pattern", pc_str_c(&idcopy));
    yyjson_mut_obj_add_strcpy(d, root, "name", nm);
    yyjson_mut_doc_set_root(d, root);
    pc_str_fini(&idcopy);
    free(nm);
    return PC_OK;
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t i = 0;
    pc_status rc = library_index(s, "pattern.delete", p, &i, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_str idcopy = {0};
    if (pc_str_set(&idcopy, pc_str_c(&s->patterns[i].id), s->patterns[i].id.len, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_pattern_fini(&s->patterns[i]);
    memmove(&s->patterns[i], &s->patterns[i + 1], (s->npatterns - i - 1) * sizeof *s->patterns);
    s->npatterns--;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_str_fini(&idcopy);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "deleted", pc_str_c(&idcopy));
    yyjson_mut_doc_set_root(d, root);
    pc_str_fini(&idcopy);
    return PC_OK;
}

static pc_status run_import(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "pattern.import";
    size_t plen = 0, dlen = 0;
    const char *path = str_param(p, "path", &plen);
    const char *data = str_param(p, "data", &dlen);
    pc_buf bytes = PC_BUF_INIT;
    if (path != NULL) {
        char *pc = pc_malloc_array(plen + 1, 1);
        if (pc == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "pattern");
        }
        memcpy(pc, path, plen);
        pc[plen] = '\0';
        pc_status rc = pc_file_read(pc, (size_t)1 << 30, &bytes, err);
        free(pc);
        if (rc != PC_OK) {
            pc_buf_fini(&bytes);
            return rc;
        }
    } else if (data != NULL) {
        if (pc_paint_b64_decode(data, dlen, &bytes, err) != PC_OK) {
            pc_buf_fini(&bytes);
            return pc_engine_err_bad_params(err, cmd, "`data` is not base64");
        }
    } else {
        return pc_engine_err_bad_params(err, cmd, "missing `path` (.pat file) or `data`");
    }
    pc_pattern *pats = NULL;
    size_t n = 0;
    pc_status rc = pc_io_read_pat(bytes.data, bytes.len, &pats, &n, err);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_io_patterns_free(pats, n);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *ids = yyjson_mut_arr(d);
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_arr_add_strcpy(d, ids, pc_str_c(&pats[i].id));
        count++;
        bool replaced = false;
        for (size_t k = 0; k < s->npatterns; k++) {
            if (pc_str_eq(&s->patterns[k].id, &pats[i].id)) {
                pc_pattern_fini(&s->patterns[k]);
                s->patterns[k] = pats[i]; /* moved */
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            if (library_push(s, &pats[i], err) != PC_OK) {
                pc_pattern_fini(&pats[i]);
            }
        }
    }
    free(pats); /* items were moved into the library (or finalised) */
    yyjson_mut_obj_add_val(d, root, "imported", ids);
    yyjson_mut_obj_add_uint(d, root, "count", count);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_export(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "pattern.export";
    size_t plen = 0;
    const char *path = str_param(p, "path", &plen);
    if (path == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `path`");
    }
    char *pc = pc_malloc_array(plen + 1, 1);
    if (pc == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "pattern");
    }
    memcpy(pc, path, plen);
    pc[plen] = '\0';

    pc_pattern *sel = NULL;
    size_t nsel = 0, capsel = 0;
    yyjson_val *keys = pc_json_get(p, "patterns");
    pc_status rc = PC_OK;
    if (keys != NULL && yyjson_is_arr(keys)) {
        size_t idx, max;
        yyjson_val *kv;
        yyjson_arr_foreach (keys, idx, max, kv) {
            size_t klen = 0;
            const char *k = pc_json_as_str(kv, &klen);
            if (k == NULL) {
                continue;
            }
            pc_pattern copy;
            bool found = false;
            rc = pc_engine_pattern_resolve(s, k, klen, &copy, &found, err);
            if (rc != PC_OK) {
                break;
            }
            if (!found) {
                rc = pc_engine_err_bad_params(err, cmd, "no pattern \"%.*s\"", (int)klen, k);
                break;
            }
            pc_pattern *grown = pc_grow_array(sel, &capsel, nsel + 1, sizeof *sel);
            if (grown == NULL) {
                pc_pattern_fini(&copy);
                rc = pc_fail(err, PC_ERR_NOMEM, "pattern");
                break;
            }
            sel = grown;
            sel[nsel++] = copy;
        }
    }
    const pc_pattern *list = (keys != NULL && yyjson_is_arr(keys)) ? sel : s->patterns;
    size_t nlist = (keys != NULL && yyjson_is_arr(keys)) ? nsel : s->npatterns;
    pc_buf bytes = PC_BUF_INIT;
    if (rc == PC_OK) {
        rc = pc_io_write_pat(list, nlist, &bytes, err);
    }
    if (rc == PC_OK) {
        rc = pc_file_write_atomic(pc, bytes.data, bytes.len, err);
    }
    size_t nbytes = bytes.len;
    pc_buf_fini(&bytes);
    for (size_t i = 0; i < nsel; i++) {
        pc_pattern_fini(&sel[i]);
    }
    free(sel);
    if (rc != PC_OK) {
        free(pc);
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(pc);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "path", pc);
    yyjson_mut_obj_add_uint(d, root, "count", nlist);
    yyjson_mut_obj_add_uint(d, root, "bytes", nbytes);
    yyjson_mut_doc_set_root(d, root);
    free(pc);
    return PC_OK;
}

/* ========================================================================================== */
/* Pattern Fill layer                                                                          */
/* ========================================================================================== */

/* commands::mask_new_layer (selection only; a path mask is deferred). */
static pc_status mask_new_layer(pc_document *doc, pc_layer *l, yyjson_val *p, const char *cmd, pc_error *err)
{
    yyjson_val *path = (p != NULL && yyjson_is_obj(p)) ? pc_json_get(p, "path") : NULL;
    if (path != NULL && !yyjson_is_null(path)) {
        return pc_engine_err_other(
            err, "a path (vector) mask for a new fill/adjustment layer is not yet supported in this build");
    }
    (void)cmd;
    pc_layer_mask m;
    pc_layer_mask_reveal_all(&m);
    if (doc->has_selection) {
        pc_surface sel;
        if (pc_surface_clone(&doc->selection, &sel, err) != PC_OK) {
            pc_layer_mask_fini(&m);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_surface_fini(&m.surface);
        m.surface = sel;
    }
    pc_pixel_format cur = pc_surface_format(&m.surface);
    pc_pixel_format want = cur;
    want.sample = doc->depth;
    if (!pc_pixel_format_eq(cur, want)) {
        pc_surface conv;
        if (pc_surface_convert(&m.surface, want, &conv, err) != PC_OK) {
            pc_layer_mask_fini(&m);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_surface_fini(&m.surface);
        m.surface = conv;
    }
    if (l->has_mask) {
        pc_layer_mask_fini(&l->mask);
    }
    l->mask = m;
    l->has_mask = true;
    return PC_OK;
}

struct fill_ctx {
    const pc_pattern *pat; /* borrowed */
    yyjson_val *p;
    pc_layer_id id;
};

static pc_status edit_new_fill(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    struct fill_ctx *c = vctx;
    pc_status rc = pc_engine_pattern_ensure_in_doc(doc, c->pat, err);
    if (rc != PC_OK) {
        return rc;
    }
    float scale, angle;
    bool link;
    float phase[2];
    pc_engine_pattern_placement(c->p, &scale, &angle, &link, phase);
    pc_fill fill;
    memset(&fill, 0, sizeof fill);
    fill.kind = PC_FILL_PATTERN;
    fill.u.pattern.scale = scale;
    fill.u.pattern.angle = angle;
    fill.u.pattern.link = link;
    fill.u.pattern.phase[0] = phase[0];
    fill.u.pattern.phase[1] = phase[1];
    if (pc_str_set(&fill.u.pattern.name, pc_str_c(&c->pat->name), c->pat->name.len, err) != PC_OK ||
        pc_str_set(&fill.u.pattern.id, pc_str_c(&c->pat->id), c->pat->id.len, err) != PC_OK) {
        pc_fill_fini(&fill);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_str nm = {0};
    if (pc_document_next_layer_name(doc, "Pattern Fill", &nm, err) != PC_OK) {
        pc_fill_fini(&fill);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_content content;
    memset(&content, 0, sizeof content);
    content.kind = PC_LAYER_FILL;
    content.u.fill = fill; /* moved */
    pc_layer l;
    rc = pc_layer_init(&l, pc_str_c(&nm), &content, err);
    pc_str_fini(&nm);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        return rc;
    }
    rc = mask_new_layer(doc, &l, c->p, "layer.newFillLayer.pattern", err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    pc_layer_id id;
    const pc_layer_id *above = *ha ? active : NULL;
    rc = pc_document_insert_above(doc, above, &l, &id, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    c->id = id;
    *active = id;
    *ha = true;
    return PC_OK;
}

static pc_status run_new_fill_layer(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_pattern pat = {0};
    pc_status rc = pc_engine_pattern_resolve_param(s, "layer.newFillLayer.pattern", p, &pat, err);
    if (rc != PC_OK) {
        return rc;
    }
    struct fill_ctx ctx = {&pat, p, 0};
    rc = pc_engine_session_edit(s, "New Pattern Fill Layer", edit_new_fill, &ctx, err);
    if (rc != PC_OK) {
        pc_pattern_fini(&pat);
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_pattern_fini(&pat);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", ctx.id);
    yyjson_mut_obj_add_strcpy(d, root, "pattern", pc_str_c(&pat.id));
    yyjson_mut_doc_set_root(d, root);
    pc_pattern_fini(&pat);
    return PC_OK;
}

/* ========================================================================================== */
/* brush texture                                                                               */
/* ========================================================================================== */

static pc_status run_brush_texture(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "brush.texturePattern";
    pc_pattern pat = {0};
    pc_status rc = pc_engine_pattern_resolve_param(s, cmd, p, &pat, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!pc_pattern_is_paintable(&pat)) {
        pc_pattern_fini(&pat);
        return pc_engine_err_bad_params(err, cmd, "the pattern is empty or too large to paint");
    }
    uint64_t count = pc_pattern_pixel_count(&pat);
    if (count == 0 || count > (uint64_t)SIZE_MAX / sizeof(float)) {
        pc_pattern_fini(&pat);
        return pc_engine_err_bad_params(err, cmd, "the pattern is empty or too large to paint");
    }
    size_t n = (size_t)count;
    float (*rgba)[4] = pc_malloc_array(n, sizeof *rgba);
    float *lum = pc_malloc_array(n, sizeof *lum);
    if (rgba == NULL || lum == NULL) {
        free(rgba);
        free(lum);
        pc_pattern_fini(&pat);
        return pc_fail(err, PC_ERR_NOMEM, "pattern");
    }
    if (pc_surface_read_rgba_into(&pat.surface, pc_pattern_rect(&pat), rgba, n, err) != PC_OK) {
        free(rgba);
        free(lum);
        pc_pattern_fini(&pat);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        float l = 0.299f * rgba[i][0] + 0.587f * rgba[i][1] + 0.114f * rgba[i][2];
        lum[i] = l * rgba[i][3] + (1.0f - rgba[i][3]);
    }
    pc_paint_gray_tile tile = {0};
    rc = pc_paint_gray_tile_from_f32(pat.width, pat.height, lum, n, &tile, err);
    free(rgba);
    free(lum);
    if (rc != PC_OK) {
        pc_pattern_fini(&pat);
        return rc;
    }
    pc_paint_texture *t = &s->tools.brush.texture;
    pc_paint_pattern_fini(&t->pattern);
    memset(&t->pattern, 0, sizeof t->pattern);
    t->pattern.kind = PC_PAINT_PATTERN_TILE;
    t->pattern.tile = tile; /* moved */
    bool en;
    yyjson_val *ev = pc_json_get(p, "enabled");
    t->enabled = (ev != NULL && pc_json_as_bool(ev, &en)) ? en : true;
    double sc;
    yyjson_val *sv = pc_json_get(p, "scale");
    if (sv != NULL && pc_json_as_f64(sv, &sc)) {
        float f = (float)(sc / 100.0);
        if (!(f >= 0.01f)) {
            f = 0.01f;
        } else if (f > 10.0f) {
            f = 10.0f;
        }
        t->scale = f;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_pattern_fini(&pat);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "pattern", pc_str_c(&pat.id));
    yyjson_mut_doc_set_root(d, root);
    pc_pattern_fini(&pat);
    return PC_OK;
}

/* ========================================================================================== */
/* Patterns panel (pattern.presets.*)                                                          */
/* ========================================================================================== */

/* Bring the panel groups in line with the library: drop ids whose pattern is gone, and gather
 * loose library ids into a trailing "Patterns" group (presets::patterns::sync). */
static void panel_sync(pc_engine_session *s)
{
    pc_preset_groups *groups = &s->presets.pattern_groups;
    for (size_t gi = 0; gi < groups->n; gi++) {
        pc_preset_group *g = &groups->v[gi];
        char **items = g->items;
        size_t w = 0;
        for (size_t i = 0; i < g->n; i++) {
            bool keep = false;
            for (size_t k = 0; k < s->npatterns; k++) {
                if (strcmp(pc_str_c(&s->patterns[k].id), items[i]) == 0) {
                    keep = true;
                    break;
                }
            }
            if (keep) {
                items[w++] = items[i];
            } else {
                free(items[i]);
            }
        }
        g->n = w;
    }
    for (size_t k = 0; k < s->npatterns; k++) {
        const char *id = pc_str_c(&s->patterns[k].id);
        bool grouped = false;
        for (size_t gi = 0; gi < groups->n && !grouped; gi++) {
            for (size_t i = 0; i < groups->v[gi].n; i++) {
                if (strcmp(((char **)groups->v[gi].items)[i], id) == 0) {
                    grouped = true;
                    break;
                }
            }
        }
        if (!grouped) {
            group_push_id(groups, "Patterns", id);
            /* "Patterns" may not exist; create and retry once. */
            bool present = false;
            for (size_t gi = 0; gi < groups->n; gi++) {
                if (groups->v[gi].name != NULL && strcmp(groups->v[gi].name, "Patterns") == 0) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                pc_error e = {0};
                (void)pc_preset_groups_push(groups, "Patterns", &e);
                group_push_id(groups, "Patterns", id);
            }
        }
    }
}

static pc_status panel_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    panel_sync(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *groups = yyjson_mut_arr(d);
    pc_preset_groups *g = &s->presets.pattern_groups;
    for (size_t gi = 0; gi < g->n; gi++) {
        yyjson_mut_val *grp = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, grp, "name", g->v[gi].name != NULL ? g->v[gi].name : "");
        yyjson_mut_val *pats = yyjson_mut_arr(d);
        char **items = g->v[gi].items;
        for (size_t i = 0; i < g->v[gi].n; i++) {
            const pc_pattern *pat = NULL;
            for (size_t k = 0; k < s->npatterns; k++) {
                if (strcmp(pc_str_c(&s->patterns[k].id), items[i]) == 0) {
                    pat = &s->patterns[k];
                    break;
                }
            }
            if (pat == NULL) {
                continue;
            }
            yyjson_mut_val *row = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, row, "id", pc_str_c(&pat->id));
            size_t dl = 0;
            const char *disp = pc_pattern_display_name(pc_str_c(&pat->name), pat->name.len, &dl);
            yyjson_mut_obj_add_strncpy(d, row, "name", disp, dl);
            yyjson_mut_obj_add_uint(d, row, "width", pat->width);
            yyjson_mut_obj_add_uint(d, row, "height", pat->height);
            yyjson_mut_arr_append(pats, row);
        }
        yyjson_mut_obj_add_val(d, grp, "patterns", pats);
        yyjson_mut_arr_append(groups, grp);
    }
    yyjson_mut_obj_add_val(d, root, "groups", groups);
    const pc_pattern *cur = panel_current(s);
    if (cur != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, "current", pc_str_c(&cur->id));
    } else {
        yyjson_mut_obj_add_null(d, root, "current");
    }
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static bool set_selected(pc_engine_session *s, const pc_pattern *pat, pc_error *err)
{
    char *copy = pc_malloc_array(pat->id.len + 1, 1);
    if (copy == NULL) {
        (void)pc_fail(err, PC_ERR_NOMEM, "pattern");
        return false;
    }
    memcpy(copy, pc_str_c(&pat->id), pat->id.len + 1);
    free(s->presets.pattern);
    s->presets.pattern = copy;
    s->presets.has_pattern = true;
    return true;
}

struct change_fill_ctx {
    const pc_pattern *pat;
    pc_layer_id id;
};
static pc_status edit_change_fill(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct change_fill_ctx *c = vctx;
    pc_status rc = pc_engine_pattern_ensure_in_doc(doc, c->pat, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l != NULL && l->content.kind == PC_LAYER_FILL && l->content.u.fill.kind == PC_FILL_PATTERN) {
        if (pc_str_set(&l->content.u.fill.u.pattern.name, pc_str_c(&c->pat->name), c->pat->name.len, err) !=
                PC_OK ||
            pc_str_set(&l->content.u.fill.u.pattern.id, pc_str_c(&c->pat->id), c->pat->id.len, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    return PC_OK;
}

static bool active_pattern_fill(const pc_engine_session *s, pc_layer_id *out)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        return false;
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    if (l != NULL && l->content.kind == PC_LAYER_FILL && l->content.u.fill.kind == PC_FILL_PATTERN) {
        *out = d->active_layer;
        return true;
    }
    return false;
}

static pc_status panel_select(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "pattern.presets.select";
    pc_pattern pat = {0};
    pc_status rc = pc_engine_pattern_resolve_param(s, cmd, p, &pat, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!set_selected(s, &pat, err)) {
        pc_pattern_fini(&pat);
        return PC_ERR_NOMEM;
    }
    s->presets.rev++;
    bool apply_layer = true;
    bool b;
    yyjson_val *av = pc_json_get(p, "applyToLayer");
    if (av != NULL && pc_json_as_bool(av, &b)) {
        apply_layer = b;
    }
    pc_layer_id lid = 0;
    bool did_layer = false;
    if (apply_layer && active_pattern_fill(s, &lid)) {
        struct change_fill_ctx ctx = {&pat, lid};
        rc = pc_engine_session_edit(s, "Change Pattern Fill", edit_change_fill, &ctx, err);
        if (rc != PC_OK) {
            pc_pattern_fini(&pat);
            return rc;
        }
        did_layer = true;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_pattern_fini(&pat);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "pattern", pc_str_c(&pat.id));
    size_t dl = 0;
    const char *disp = pc_pattern_display_name(pc_str_c(&pat.name), pat.name.len, &dl);
    yyjson_mut_obj_add_strncpy(d, root, "name", disp, dl);
    if (did_layer) {
        yyjson_mut_obj_add_uint(d, root, "layer", lid);
    }
    yyjson_mut_doc_set_root(d, root);
    pc_pattern_fini(&pat);
    return PC_OK;
}

static pc_status panel_apply(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    /* Resolve the pattern, then run the New Pattern Fill Layer edit with its id. */
    pc_pattern pat = {0};
    pc_status rc = pc_engine_pattern_resolve_param(s, "pattern.presets.apply", p, &pat, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_pattern_fini(&pat);
    return run_new_fill_layer(s, p, out, err);
}

static pc_status panel_new(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_status rc = run_define(s, p, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* The new pattern's id is out["pattern"]; copy it out, then rebuild the result as {pattern}. */
    char idbuf[64];
    idbuf[0] = '\0';
    yyjson_mut_val *root0 = pc_json_value_root(out);
    if (root0 != NULL) {
        yyjson_mut_val *pv = yyjson_mut_obj_get(root0, "pattern");
        const char *s0 = pv != NULL ? yyjson_mut_get_str(pv) : NULL;
        if (s0 != NULL) {
            size_t l = strlen(s0);
            if (l < sizeof idbuf) {
                memcpy(idbuf, s0, l + 1);
            }
        }
    }
    const char *id = idbuf[0] != '\0' ? idbuf : NULL;
    panel_sync(s);
    size_t glen = 0;
    const char *group = str_param(p, "group", &glen);
    if (id != NULL && group != NULL && glen > 0) {
        /* Drop the id from every group, then push it into `group`. */
        pc_preset_groups *groups = &s->presets.pattern_groups;
        for (size_t gi = 0; gi < groups->n; gi++) {
            pc_preset_group *g = &groups->v[gi];
            char **items = g->items;
            size_t w = 0;
            for (size_t i = 0; i < g->n; i++) {
                if (strcmp(items[i], id) == 0) {
                    free(items[i]);
                } else {
                    items[w++] = items[i];
                }
            }
            g->n = w;
        }
        char gname[256];
        size_t gl = glen < sizeof gname - 1 ? glen : sizeof gname - 1;
        memcpy(gname, group, gl);
        gname[gl] = '\0';
        bool present = false;
        for (size_t gi = 0; gi < groups->n; gi++) {
            if (groups->v[gi].name != NULL && strcmp(groups->v[gi].name, gname) == 0) {
                present = true;
                break;
            }
        }
        if (!present) {
            pc_error e = {0};
            (void)pc_preset_groups_push(groups, gname, &e);
        }
        group_push_id(groups, gname, id);
    }
    pc_engine_presets_changed(s);
    /* Rebuild the result as {"pattern": id} (presets::patterns::new_preset). */
    pc_json_value_fini(out);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    if (id != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, "pattern", id);
    } else {
        yyjson_mut_obj_add_null(d, root, "pattern");
    }
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status panel_edit(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "pattern.presets.edit";
    panel_sync(s);
    size_t alen = 0;
    const char *action = str_param(p, "action", &alen);
    if (action == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `action`");
    }
    pc_status rc;
    if (alen == 6 && strncmp(action, "rename", 6) == 0) {
        rc = run_rename(s, p, out, err);
    } else if (alen == 6 && strncmp(action, "delete", 6) == 0) {
        /* Library-pattern delete (one or an array); resolve all first (#4103). */
        yyjson_val *pref = pc_json_get(p, "preset");
        size_t n = 0;
        pc_str *ids = NULL;
        size_t capids = 0;
        if (pref != NULL && yyjson_is_arr(pref)) {
            size_t idx, max;
            yyjson_val *kv;
            yyjson_arr_foreach (pref, idx, max, kv) {
                size_t klen = 0;
                const char *k = pc_json_as_str(kv, &klen);
                if (k == NULL) {
                    for (size_t i = 0; i < n; i++) {
                        pc_str_fini(&ids[i]);
                    }
                    free(ids);
                    return pc_engine_err_bad_params(err, cmd,
                                                    "`preset` must contain only pattern ids or names");
                }
                const pc_pattern *f = pc_pattern_find(s->patterns, s->npatterns, k, klen, k, klen);
                if (f == NULL) {
                    for (size_t i = 0; i < n; i++) {
                        pc_str_fini(&ids[i]);
                    }
                    free(ids);
                    return pc_engine_err_bad_params(err, cmd, "no library pattern \"%.*s\"", (int)klen, k);
                }
                pc_str *grown = pc_grow_array(ids, &capids, n + 1, sizeof *ids);
                if (grown == NULL) {
                    for (size_t i = 0; i < n; i++) {
                        pc_str_fini(&ids[i]);
                    }
                    free(ids);
                    return pc_fail(err, PC_ERR_NOMEM, "pattern");
                }
                ids = grown;
                ids[n] = (pc_str){0};
                (void)pc_str_set(&ids[n], pc_str_c(&f->id), f->id.len, err);
                n++;
            }
        } else {
            size_t klen = 0;
            const char *k = str_param(p, "preset", &klen);
            if (k == NULL) {
                return pc_engine_err_bad_params(err, cmd, "missing `preset`");
            }
            const pc_pattern *f = pc_pattern_find(s->patterns, s->npatterns, k, klen, k, klen);
            if (f == NULL) {
                return pc_engine_err_bad_params(err, cmd, "no library pattern \"%.*s\"", (int)klen, k);
            }
            ids = pc_calloc_array(1, sizeof *ids);
            if (ids == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "pattern");
            }
            (void)pc_str_set(&ids[0], pc_str_c(&f->id), f->id.len, err);
            n = 1;
        }
        for (size_t i = 0; i < n; i++) {
            for (size_t k = 0; k < s->npatterns; k++) {
                if (pc_str_eq(&s->patterns[k].id, &ids[i])) {
                    pc_pattern_fini(&s->patterns[k]);
                    memmove(&s->patterns[k], &s->patterns[k + 1],
                            (s->npatterns - k - 1) * sizeof *s->patterns);
                    s->npatterns--;
                    break;
                }
            }
        }
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            for (size_t i = 0; i < n; i++) {
                pc_str_fini(&ids[i]);
            }
            free(ids);
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, root, "deleted", n);
        yyjson_mut_doc_set_root(d, root);
        for (size_t i = 0; i < n; i++) {
            pc_str_fini(&ids[i]);
        }
        free(ids);
        rc = PC_OK;
    } else {
        /* newGroup / renameGroup / move / deleteGroup via the generic group machinery. deleteGroup
         * also removes the (now orphaned) patterns from the library. */
        bool delete_group = (alen == 11 && strncmp(action, "deleteGroup", 11) == 0);
        char acopy[64];
        size_t al = alen < sizeof acopy - 1 ? alen : sizeof acopy - 1;
        memcpy(acopy, action, al);
        acopy[al] = '\0';
        /* A "move" references the pattern by id after resolving its key. */
        rc = pc_preset_edit_groups(&s->presets.pattern_groups, acopy, p, cmd, pc_preset_string_ops(), out, err);
        if (rc == PC_OK && delete_group) {
            /* Remove from the library any id no longer present in any group. */
            for (size_t k = s->npatterns; k-- > 0;) {
                const char *id = pc_str_c(&s->patterns[k].id);
                bool grouped = false;
                for (size_t gi = 0; gi < s->presets.pattern_groups.n && !grouped; gi++) {
                    for (size_t i = 0; i < s->presets.pattern_groups.v[gi].n; i++) {
                        if (strcmp(((char **)s->presets.pattern_groups.v[gi].items)[i], id) == 0) {
                            grouped = true;
                            break;
                        }
                    }
                }
                if (!grouped) {
                    pc_pattern_fini(&s->patterns[k]);
                    memmove(&s->patterns[k], &s->patterns[k + 1],
                            (s->npatterns - k - 1) * sizeof *s->patterns);
                    s->npatterns--;
                }
            }
        }
    }
    if (rc != PC_OK) {
        return rc;
    }
    panel_sync(s);
    pc_engine_presets_changed(s);
    return PC_OK;
}

/* ========================================================================================== */
/* specs                                                                                       */
/* ========================================================================================== */

static const char *const MENU_EDIT[] = {"Edit"};
static const char *const MENU_LAYER_NEWFILL[] = {"Layer", "New Fill Layer"};

static const pc_engine_command SPECS[] = {
    {"edit.definePattern", "Define Pattern...", MENU_EDIT, 1, NULL,
     "{\"name\":text,\"rect\":[x0,y0,x1,y1]?} -> {\"pattern\":id} (name: default the document's name "
     "numbered past the library's names; rect: default the selection bounds or else the canvas; added "
     "to the library from the visible composite)",
     pc_engine_has_doc, run_define, true},
    {"pattern.list", "Patterns", NULL, 0, NULL,
     "{} -> [{id,name,width,height,mode,depth,inDocument,inLibrary}]", always, run_list, false},
    {"pattern.rename", "Rename Pattern", NULL, 0, NULL, "{\"pattern\":id|name,\"name\":str}", has_pattern,
     run_rename, true},
    {"pattern.delete", "Delete Pattern", NULL, 0, NULL,
     "{\"pattern\":id|name} (library only; documents keep their copy)", has_pattern, run_delete, true},
    {"pattern.import", "Import Patterns...", NULL, 0, NULL,
     "{\"path\":\".pat file\"?,\"data\":base64 bytes? (instead of a path)} -> {imported:[ids],count}",
     always, run_import, true},
    {"pattern.export", "Export Patterns...", NULL, 0, NULL,
     "{\"path\":\".pat file\",\"patterns\":[id|name]? (default: the whole library)}", has_pattern,
     run_export, true},
    {"layer.newFillLayer.pattern", "Pattern...", MENU_LAYER_NEWFILL, 2, NULL,
     "{\"pattern\":id|name?=first library pattern,\"scale\":1..1000=100,\"angle\":deg=0,\"link\":bool=true,"
     "\"phase\":[x,y]?,\"path\":\"work\"|saved path name|{...path}? (the active path: becomes the vector "
     "mask; else the selection is the layer mask)}",
     pc_engine_has_doc, run_new_fill_layer, true},
    {"brush.texturePattern", "Brush Texture Pattern", NULL, 0, NULL,
     "{\"pattern\":id|name,\"scale\":1..1000?,\"enabled\":bool=true} (sets the session brush's Texture to "
     "that pattern)",
     has_pattern, run_brush_texture, true},
    {"pattern.presets.list", "Patterns Panel", NULL, 0, NULL,
     "{} -> {groups:[{name,patterns:[{id,name,width,height}]}],current:id?}", always, panel_list, false},
    {"pattern.presets.select", "Select Pattern", NULL, 0, NULL,
     "{\"pattern\":id|name,\"applyToLayer\":bool=true} (the Patterns-panel selection; changes an active "
     "Pattern Fill layer)",
     has_pattern, panel_select, true},
    {"pattern.presets.apply", "Apply Pattern", NULL, 0, NULL,
     "{\"pattern\":id|name,...layer.newFillLayer.pattern params} (a new Pattern Fill layer)", pc_engine_has_doc,
     panel_apply, true},
    {"pattern.presets.new", "New Pattern", NULL, 0, NULL,
     "{\"name\":text?,\"rect\":[x0,y0,x1,y1]?,\"group\":folder?} (Define Pattern, filed into a folder)",
     pc_engine_has_doc, panel_new, true},
    {"pattern.presets.edit", "Edit Patterns", NULL, 0, NULL,
     "{\"action\":\"rename|delete|move|newGroup|renameGroup|deleteGroup\",\"preset\":id|name|[id|name],"
     "\"name\":str?,\"group\":folder?,\"index\":n?}",
     always, panel_edit, true},
};

const pc_engine_command *pc_engine_pattern_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
