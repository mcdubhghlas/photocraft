/* ADJ-parameter machinery (port of crates/engine/src/adjust_params.rs + the Selective Color and
 * Color Lookup readers of adjust_cmds.rs). See adjust_params.h.
 *
 * The Levels/Curves eyedropper (Set Black/Neutral Gray/White Point) is NOT ported here: it needs
 * the merged-composite sampler (E10 sample_cmds); the commands report `eyedropper` as unsupported. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adjust_params.h"
#include "base/mem.h"
#include "cms/lutfile.h"
#include "compose/adjust.h"
#include "engine_internal.h" /* pc_engine_err_bad_params */

/* ---- small helpers ------------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* BadParams with the reference's "invalid parameters for `<cmd>`: <msg>" wording. */
static pc_status bad(pc_error *err, const char *cmd, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return pc_engine_err_bad_params(err, cmd, "%s", msg);
}

/* key's value; a null or a missing key counts as absent (as in the reference). */
static yyjson_val *jget(yyjson_val *p, const char *key)
{
    if (p == NULL || !yyjson_is_obj(p)) {
        return NULL;
    }
    yyjson_val *v = yyjson_obj_get(p, key);
    if (v == NULL || yyjson_is_null(v)) {
        return NULL;
    }
    return v;
}

static pc_status num_in(pc_error *err, const char *cmd, const char *key, yyjson_val *v, float lo, float hi,
                        float *out)
{
    *out = 0.0f; /* defined on every path (callers return on error; analyzer-clean) */
    if (!yyjson_is_num(v)) {
        return bad(err, cmd, "`%s` must be a number", key);
    }
    double x = yyjson_get_num(v);
    if (!isfinite(x)) {
        return bad(err, cmd, "`%s` must be a number", key);
    }
    *out = clampf((float)x, lo, hi);
    return PC_OK;
}

static pc_status jnum(pc_error *err, const char *cmd, yyjson_val *p, const char *key, float def, float lo,
                      float hi, float *out)
{
    yyjson_val *v = jget(p, key);
    if (v == NULL) {
        *out = clampf(def, lo, hi);
        return PC_OK;
    }
    return num_in(err, cmd, key, v, lo, hi, out);
}

static pc_status jbool(pc_error *err, const char *cmd, yyjson_val *p, const char *key, bool def, bool *out)
{
    *out = def;
    yyjson_val *v = jget(p, key);
    if (v == NULL) {
        *out = def;
        return PC_OK;
    }
    if (!yyjson_is_bool(v)) {
        return bad(err, cmd, "`%s` must be true or false", key);
    }
    *out = yyjson_get_bool(v);
    return PC_OK;
}

/* An array of exactly n numbers (each clamped to -1e6..1e6). *present false when the key is absent. */
static pc_status jnumbers(pc_error *err, const char *cmd, yyjson_val *p, const char *key, size_t n,
                          float *out, bool *present)
{
    *present = false;
    yyjson_val *v = jget(p, key);
    if (v == NULL) {
        return PC_OK;
    }
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) != n) {
        return bad(err, cmd, "`%s` must be an array of %zu numbers", key, n);
    }
    for (size_t i = 0; i < n; i++) {
        pc_status rc = num_in(err, cmd, key, yyjson_arr_get(v, i), -1e6f, 1e6f, &out[i]);
        if (rc != PC_OK) {
            return rc;
        }
    }
    *present = true;
    return PC_OK;
}

static int hex_byte(const char *h, size_t i)
{
    int hi = -1, lo = -1;
    char a = h[i], b = h[i + 1];
    if (a >= '0' && a <= '9') {
        hi = a - '0';
    } else if (a >= 'a' && a <= 'f') {
        hi = a - 'a' + 10;
    } else if (a >= 'A' && a <= 'F') {
        hi = a - 'A' + 10;
    }
    if (b >= '0' && b <= '9') {
        lo = b - '0';
    } else if (b >= 'a' && b <= 'f') {
        lo = b - 'a' + 10;
    } else if (b >= 'A' && b <= 'F') {
        lo = b - 'A' + 10;
    }
    if (hi < 0 || lo < 0) {
        return -1;
    }
    return hi * 16 + lo;
}

static pc_status color_of(pc_error *err, const char *cmd, const char *key, yyjson_val *v, float out[3])
{
    if (yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        size_t len = strlen(s);
        if (len > 0 && s[0] == '#') {
            s++;
            len--;
        }
        if ((len == 6 || len == 8)) {
            int r = hex_byte(s, 0), g = hex_byte(s, 2), b = hex_byte(s, 4);
            if (r >= 0 && g >= 0 && b >= 0) {
                out[0] = (float)r / 255.0f;
                out[1] = (float)g / 255.0f;
                out[2] = (float)b / 255.0f;
                return PC_OK;
            }
        }
        return bad(err, cmd, "`%s` must be \"#rrggbb\" or [r, g, b] in 0..1", key);
    }
    if (yyjson_is_arr(v) && yyjson_arr_size(v) >= 3) {
        for (int i = 0; i < 3; i++) {
            yyjson_val *c = yyjson_arr_get(v, (size_t)i);
            if (!yyjson_is_num(c) || !isfinite(yyjson_get_num(c))) {
                return bad(err, cmd, "`%s` must be \"#rrggbb\" or [r, g, b] in 0..1", key);
            }
            out[i] = clampf((float)yyjson_get_num(c), 0.0f, 1.0f);
        }
        return PC_OK;
    }
    return bad(err, cmd, "`%s` must be \"#rrggbb\" or [r, g, b] in 0..1", key);
}

static pc_status jcolor(pc_error *err, const char *cmd, yyjson_val *p, const char *key, const float def[3],
                        float out[3])
{
    yyjson_val *v = jget(p, key);
    if (v == NULL) {
        out[0] = def[0];
        out[1] = def[1];
        out[2] = def[2];
        return PC_OK;
    }
    return color_of(err, cmd, key, v, out);
}

/* A channel object (`{"inBlack": ...}`) or NULL (error if present and not an object). */
static pc_status jobject(pc_error *err, const char *cmd, yyjson_val *p, const char *key, yyjson_val **out)
{
    *out = NULL;
    yyjson_val *v = jget(p, key);
    if (v == NULL) {
        return PC_OK;
    }
    if (!yyjson_is_obj(v)) {
        return bad(err, cmd, "`%s` must be an object", key);
    }
    *out = v;
    return PC_OK;
}

/* ---- photo-filter presets ----------------------------------------------------------------- */

struct photo_filter {
    const char *id;
    uint8_t rgb[3];
};
static const struct photo_filter PHOTO_FILTERS[] = {
    {"warming85", {236, 138, 0}},   {"warmingLBA", {250, 150, 0}},  {"warming81", {235, 177, 19}},
    {"cooling80", {0, 109, 255}},   {"coolingLBB", {0, 93, 255}},   {"cooling82", {0, 181, 255}},
    {"red", {234, 26, 26}},         {"orange", {243, 132, 23}},     {"yellow", {249, 227, 28}},
    {"green", {25, 201, 25}},       {"cyan", {29, 203, 234}},       {"blue", {29, 53, 234}},
    {"violet", {155, 29, 234}},     {"magenta", {227, 24, 227}},    {"sepia", {172, 122, 51}},
    {"deepRed", {255, 0, 0}},       {"deepBlue", {0, 34, 205}},     {"deepEmerald", {0, 140, 0}},
    {"deepYellow", {255, 213, 0}},  {"underwater", {0, 194, 177}},
};

/* A Photo Filter preset colour (0..1) by id; false for an unknown id. */
static bool pc_engine_photo_filter_preset(const char *id, float out[3])
{
    for (size_t i = 0; i < sizeof PHOTO_FILTERS / sizeof PHOTO_FILTERS[0]; i++) {
        if (strcmp(PHOTO_FILTERS[i].id, id) == 0) {
            out[0] = (float)PHOTO_FILTERS[i].rgb[0] / 255.0f;
            out[1] = (float)PHOTO_FILTERS[i].rgb[1] / 255.0f;
            out[2] = (float)PHOTO_FILTERS[i].rgb[2] / 255.0f;
            return true;
        }
    }
    return false;
}

/* ---- tone-space + channel keys ------------------------------------------------------------ */

static const char *const RGB_KEYS[3] = {"red", "green", "blue"};
static const char *const CMYK_KEYS[4] = {"cyan", "magenta", "yellow", "black"};
static const char *const LAB_KEYS[3] = {"lightness", "a", "b"};
static const char *const HUE_RANGES[6] = {"reds", "yellows", "greens", "cyans", "blues", "magentas"};
static const float BW_DEFAULTS[6] = {40.0f, 60.0f, 40.0f, 60.0f, 20.0f, 80.0f};
/* Selective Color ranges in storage order. */
static const char *const SEL_RANGES[9] = {"reds", "yellows", "greens",  "cyans", "blues",
                                          "magentas", "whites", "neutrals", "blacks"};

static const char *const *channel_keys(pc_tone_space space, size_t *n)
{
    switch (space) {
    case PC_TONE_CMYK:
        *n = 4;
        return CMYK_KEYS;
    case PC_TONE_LAB:
        *n = 3;
        return LAB_KEYS;
    default:
        *n = 3;
        return RGB_KEYS;
    }
}

static pc_tone_space default_space(pc_color_mode mode)
{
    if (mode == PC_COLOR_MODE_CMYK) {
        return PC_TONE_CMYK;
    }
    if (mode == PC_COLOR_MODE_LAB) {
        return PC_TONE_LAB;
    }
    return PC_TONE_RGB;
}

/* Which tone space the params address: any channel key of a space picks it. */
static pc_tone_space space_of(yyjson_val *p, bool has_base, pc_tone_space base, pc_color_mode mode)
{
    bool cmyk = jget(p, "cyan") || jget(p, "magenta") || jget(p, "yellow") || jget(p, "black");
    bool rgb = jget(p, "red") || jget(p, "green") || jget(p, "blue");
    bool lab_ab = jget(p, "a") || jget(p, "b");
    if (cmyk) {
        return PC_TONE_CMYK;
    }
    if (lab_ab || (jget(p, "lightness") && !rgb)) {
        return PC_TONE_LAB;
    }
    if (rgb) {
        return PC_TONE_RGB;
    }
    return has_base ? base : default_space(mode);
}

/* ---- curves helpers ----------------------------------------------------------------------- */

#define MAX_CURVE_POINTS 19
#define MAX_FITTED_POINTS 16
#define CURVE_COORD_PREC 100.0f
#define CURVE_DUP_DIST (0.5f / 255.0f)

static pc_status curve_set(pc_curve *c, const pc_curve_point *pts, size_t n, pc_error *err)
{
    pc_curve_point *a = NULL;
    if (n > 0) {
        a = pc_malloc_array(n, sizeof *a);
        if (a == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "curve");
        }
        memcpy(a, pts, n * sizeof *a);
    }
    pc_curve_fini(c);
    c->pts = a;
    c->len = n;
    return PC_OK;
}

static pc_status identity_curve(pc_curve *out, pc_error *err)
{
    pc_curve_point pts[2] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    out->pts = NULL;
    out->len = 0;
    return curve_set(out, pts, 2, err);
}

/* `lut` (evenly spaced over 0..=1, len samples) at x, linearly interpolated. */
static float lut_at(const float *lut, size_t len, float x)
{
    if (len == 0) {
        return x;
    }
    size_t last = len - 1;
    float f = clampf(x, 0.0f, 1.0f) * (float)last;
    size_t i = (size_t)floorf(f);
    if (i > last) {
        i = last;
    }
    size_t j = (i + 1 < last) ? i + 1 : last;
    float a = lut[i], b = lut[j];
    return a + (b - a) * (f - (float)i);
}

static int cmp_curve_point(const void *a, const void *b)
{
    float x = ((const pc_curve_point *)a)->input, y = ((const pc_curve_point *)b)->input;
    return (x < y) ? -1 : (x > y ? 1 : 0);
}

/* `[[in, out], ...]` in 0..255: sorted by input, duplicates collapsed, 2..=19 points. */
static pc_status parse_curve(pc_error *err, const char *cmd, const char *key, yyjson_val *v,
                             pc_curve_point **out, size_t *out_n)
{
    const char *msg = "an array of 2..=19 [input, output] pairs in 0..255";
    if (!yyjson_is_arr(v)) {
        return bad(err, cmd, "`%s` must be %s", key, msg);
    }
    size_t a = yyjson_arr_size(v);
    if (a < 2 || a > 64) {
        return bad(err, cmd, "`%s` must be %s", key, msg);
    }
    pc_curve_point *pts = pc_malloc_array(a, sizeof *pts);
    if (pts == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "curve");
    }
    for (size_t i = 0; i < a; i++) {
        yyjson_val *q = yyjson_arr_get(v, i);
        if (!yyjson_is_arr(q) || yyjson_arr_size(q) != 2) {
            free(pts);
            return bad(err, cmd, "`%s` must be %s", key, msg);
        }
        yyjson_val *qx = yyjson_arr_get(q, 0), *qy = yyjson_arr_get(q, 1);
        if (!yyjson_is_num(qx) || !yyjson_is_num(qy) || !isfinite(yyjson_get_num(qx)) ||
            !isfinite(yyjson_get_num(qy))) {
            free(pts);
            return bad(err, cmd, "`%s` must be %s", key, msg);
        }
        pts[i].input = clampf((float)yyjson_get_num(qx), 0.0f, 255.0f) / 255.0f;
        pts[i].output = clampf((float)yyjson_get_num(qy), 0.0f, 255.0f) / 255.0f;
    }
    qsort(pts, a, sizeof *pts, cmp_curve_point);
    /* dedup adjacent within half a level (keep earlier). */
    size_t m = 0;
    for (size_t i = 0; i < a; i++) {
        if (m > 0 && fabsf(pts[i].input - pts[m - 1].input) < CURVE_DUP_DIST) {
            continue;
        }
        pts[m++] = pts[i];
    }
    if (m < 2 || m > MAX_CURVE_POINTS) {
        free(pts);
        return bad(err, cmd, "`%s` must be %s", key, msg);
    }
    *out = pts;
    *out_n = m;
    return PC_OK;
}

/* A pencil-mode map param: exactly PC_CURVE_MAP_LEN outputs in 0..255. */
static pc_status parse_map(pc_error *err, const char *cmd, const char *key, yyjson_val *v, pc_curve *out)
{
    const char *msg = "a pencil-drawn map of 256 output levels in 0..255 or [input, output] pairs";
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) != PC_CURVE_MAP_LEN) {
        return bad(err, cmd, "`%s` must be %s", key, msg);
    }
    float *outs = pc_malloc_array(PC_CURVE_MAP_LEN, sizeof *outs);
    if (outs == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "curve map");
    }
    for (size_t i = 0; i < PC_CURVE_MAP_LEN; i++) {
        yyjson_val *x = yyjson_arr_get(v, i);
        if (!yyjson_is_num(x) || !isfinite(yyjson_get_num(x))) {
            free(outs);
            return bad(err, cmd, "`%s` must be %s", key, msg);
        }
        outs[i] = clampf((float)yyjson_get_num(x), 0.0f, 255.0f) / 255.0f;
    }
    pc_status rc = pc_curve_map(outs, PC_CURVE_MAP_LEN, out, err);
    free(outs);
    return rc;
}

/* A point curve drawn into a pencil-mode map. */
static pc_status points_to_map(const pc_curve_point *pts, size_t n, pc_curve *out, pc_error *err)
{
    float *lut = pc_malloc_array(PC_COMPOSE_LUT_SIZE, sizeof *lut);
    if (lut == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "curve lut");
    }
    pc_status rc = pc_compose_curve_lut(pts, n, lut, err);
    if (rc != PC_OK) {
        free(lut);
        return rc;
    }
    float outs[PC_CURVE_MAP_LEN];
    float last = (float)(PC_CURVE_MAP_LEN - 1);
    for (size_t k = 0; k < PC_CURVE_MAP_LEN; k++) {
        outs[k] = lut_at(lut, PC_COMPOSE_LUT_SIZE, (float)k / last);
    }
    free(lut);
    return pc_curve_map(outs, PC_CURVE_MAP_LEN, out, err);
}

/* A pencil-mode map turned back into a point curve (Photoshop's "Edit points to modify"). */
static pc_status map_to_points(const pc_curve_point *mpts, size_t mn, pc_curve_point **out, size_t *out_n,
                               pc_error *err)
{
    float target[PC_CURVE_MAP_LEN];
    if (pc_curve_map_outputs(mpts, mn, target, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    size_t last = PC_CURVE_MAP_LEN - 1;
    pc_curve_point pts[MAX_FITTED_POINTS];
    size_t np = 0;
    pts[np].input = 0.0f;
    pts[np++].output = target[0];
    pts[np].input = 1.0f;
    pts[np++].output = target[last];
    float *lut = pc_malloc_array(PC_COMPOSE_LUT_SIZE, sizeof *lut);
    if (lut == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "curve lut");
    }
    while (np < MAX_FITTED_POINTS) {
        if (pc_compose_curve_lut(pts, np, lut, err) != PC_OK) {
            free(lut);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        size_t worst_k = 0;
        float worst = -1.0f;
        for (size_t k = 0; k <= last; k++) {
            float e = fabsf(lut_at(lut, PC_COMPOSE_LUT_SIZE, (float)k / (float)last) - target[k]);
            if (e > worst) {
                worst = e;
                worst_k = k;
            }
        }
        if (worst <= 1.0f / 255.0f) {
            break;
        }
        float in = (float)worst_k / (float)last;
        bool dup = false;
        for (size_t i = 0; i < np; i++) {
            if (fabsf(pts[i].input - in) < CURVE_DUP_DIST) {
                dup = true;
                break;
            }
        }
        if (dup) {
            break;
        }
        pts[np].input = in;
        pts[np].output = target[worst_k];
        np++;
        qsort(pts, np, sizeof *pts, cmp_curve_point);
    }
    free(lut);
    pc_curve_point *res = pc_malloc_array(np, sizeof *res);
    if (res == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "curve points");
    }
    memcpy(res, pts, np * sizeof *res);
    *out = res;
    *out_n = np;
    return PC_OK;
}

/* One Smooth pass over pencil-mode outputs: mean of a symmetric window of up to 3 neighbours. */
static void smooth_curve_map(float *outputs, size_t n)
{
    if (n == 0) {
        return;
    }
    float *src = pc_malloc_array(n, sizeof *src);
    if (src == NULL) {
        return;
    }
    memcpy(src, outputs, n * sizeof *src);
    for (size_t k = 0; k < n; k++) {
        size_t r = 3;
        if (k < r) {
            r = k;
        }
        size_t tail = n - k - 1;
        if (tail < r) {
            r = tail;
        }
        if (r == 0) {
            continue;
        }
        double sum = 0.0;
        size_t cnt = 0;
        for (size_t i = k - r; i <= k + r; i++) {
            sum += (double)src[i];
            cnt++;
        }
        outputs[k] = (float)(sum / (double)cnt);
    }
    free(src);
}

/* A Curves channel param: points (drawn into a map in pencil mode) or a map (fitted with points
 * outside pencil mode). Writes *out (a curve the caller frees). */
static pc_status parse_curve_in(pc_error *err, const char *cmd, const char *key, yyjson_val *v,
                                bool pencil, pc_curve *out)
{
    bool is_map = yyjson_is_arr(v) && yyjson_arr_size(v) > 0 && yyjson_is_num(yyjson_arr_get(v, 0));
    if (is_map) {
        pc_curve map;
        map.pts = NULL;
        map.len = 0;
        pc_status rc = parse_map(err, cmd, key, v, &map);
        if (rc != PC_OK) {
            return rc;
        }
        if (pencil) {
            pc_curve_fini(out);
            *out = map;
            return PC_OK;
        }
        pc_curve_point *pts = NULL;
        size_t n = 0;
        rc = map_to_points(map.pts, map.len, &pts, &n, err);
        pc_curve_fini(&map);
        if (rc != PC_OK) {
            return rc;
        }
        rc = curve_set(out, pts, n, err);
        free(pts);
        return rc;
    }
    pc_curve_point *pts = NULL;
    size_t n = 0;
    pc_status rc = parse_curve(err, cmd, key, v, &pts, &n);
    if (rc != PC_OK) {
        return rc;
    }
    if (pencil) {
        rc = points_to_map(pts, n, out, err);
        free(pts);
        return rc;
    }
    rc = curve_set(out, pts, n, err);
    free(pts);
    return rc;
}

/* ---- Selective Color / Color Lookup ------------------------------------------------------- */

/* A percentage in -100..100 (clamped); anything but a finite number is an error. */
static pc_status percent(pc_error *err, const char *cmd, const char *key, yyjson_val *v, float *out)
{
    if (!yyjson_is_num(v) || !isfinite(yyjson_get_num(v))) {
        return bad(err, cmd, "`%s` must be a number", key);
    }
    *out = clampf((float)yyjson_get_num(v), -100.0f, 100.0f);
    return PC_OK;
}

static pc_status opt_str(pc_error *err, const char *cmd, yyjson_val *p, const char *key, const char **out)
{
    *out = NULL;
    yyjson_val *v = jget(p, key);
    if (v == NULL) {
        return PC_OK;
    }
    if (!yyjson_is_str(v)) {
        return bad(err, cmd, "`%s` must be a string", key);
    }
    *out = yyjson_get_str(v);
    return PC_OK;
}

pc_status pc_engine_selective_from_params(yyjson_val *p, const pc_adjustment *base, pc_adjustment *out,
                                          pc_error *err)
{
    const char *cmd = "selectiveColor";
    bool relative = true;
    float adj[9][4] = {{0}};
    if (base != NULL && base->kind == PC_ADJ_SELECTIVE_COLOR) {
        relative = base->u.selective_color.relative;
        memcpy(adj, base->u.selective_color.adjustments, sizeof adj);
    }
    const char *method = NULL;
    pc_status rc = opt_str(err, cmd, p, "method", &method);
    if (rc != PC_OK) {
        return rc;
    }
    if (method != NULL) {
        if (strcmp(method, "relative") == 0) {
            relative = true;
        } else if (strcmp(method, "absolute") == 0) {
            relative = false;
        } else {
            return bad(err, cmd, "`method` must be \"relative\" or \"absolute\", not `%s`", method);
        }
    }
    yyjson_val *rv = jget(p, "relative");
    if (rv != NULL) {
        if (!yyjson_is_bool(rv)) {
            return bad(err, cmd, "`relative` must be true or false");
        }
        relative = yyjson_get_bool(rv);
    }
    for (size_t i = 0; i < 9; i++) {
        yyjson_val *v = jget(p, SEL_RANGES[i]);
        if (v == NULL) {
            continue;
        }
        if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 4) {
            return bad(err, cmd, "`%s` must be an array of 4 numbers [c, m, y, k]", SEL_RANGES[i]);
        }
        for (size_t j = 0; j < 4; j++) {
            rc = percent(err, cmd, SEL_RANGES[i], yyjson_arr_get(v, j), &adj[i][j]);
            if (rc != PC_OK) {
                return rc;
            }
        }
    }
    const char *colors = NULL;
    rc = opt_str(err, cmd, p, "colors", &colors);
    if (rc != PC_OK) {
        return rc;
    }
    if (colors != NULL) {
        size_t row = 9;
        for (size_t i = 0; i < 9; i++) {
            if (strcmp(colors, SEL_RANGES[i]) == 0) {
                row = i;
                break;
            }
        }
        if (row == 9) {
            return bad(err, cmd,
                       "unknown range `%s` (ranges: reds, yellows, greens, cyans, blues, magentas, "
                       "whites, neutrals, blacks)",
                       colors);
        }
        static const char *const cmyk[4] = {"cyan", "magenta", "yellow", "black"};
        for (size_t j = 0; j < 4; j++) {
            yyjson_val *v = jget(p, cmyk[j]);
            if (v != NULL) {
                rc = percent(err, cmd, cmyk[j], v, &adj[row][j]);
                if (rc != PC_OK) {
                    return rc;
                }
            }
        }
    }
    pc_adjustment a;
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_SELECTIVE_COLOR;
    a.u.selective_color.relative = relative;
    memcpy(a.u.selective_color.adjustments, adj, sizeof adj);
    *out = a;
    return PC_OK;
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *q = path; *q; q++) {
        if (*q == '/' || *q == '\\') {
            b = q + 1;
        }
    }
    return b;
}

pc_status pc_engine_lookup_from_params(yyjson_val *p, const pc_adjustment *base, pc_adjustment *out,
                                       pc_error *err)
{
    const char *cmd = "colorLookup";
    pc_str name;
    name.ptr = NULL;
    name.len = 0;
    pc_blob *lut = NULL;
    uint32_t size = 0;
    bool tetrahedral = false, dither = false;
    pc_status rc = PC_OK;
    if (base != NULL && base->kind == PC_ADJ_COLOR_LOOKUP) {
        if ((rc = pc_str_clone(&base->u.color_lookup.name, &name, err)) != PC_OK) {
            return rc;
        }
        lut = pc_blob_retain(base->u.color_lookup.lut);
        size = base->u.color_lookup.size;
        tetrahedral = base->u.color_lookup.tetrahedral;
        dither = base->u.color_lookup.dither;
    }
    bool have_loaded = false;
    pc_lutfile loaded;
    memset(&loaded, 0, sizeof loaded);
    char loaded_label[256] = {0};

    const char *id = NULL;
    if ((rc = opt_str(err, cmd, p, "lut", &id)) != PC_OK) {
        goto fail;
    }
    if (id != NULL) {
        if (id[0] == '\0' || strcmp(id, "none") == 0) {
            pc_str_set_c(&name, "", NULL);
            pc_blob_release(lut);
            lut = NULL;
            size = 0;
        } else {
            pc_lutfile f;
            memset(&f, 0, sizeof f);
            rc = pc_lutfile_builtin(id, &f, err);
            if (rc != PC_OK) {
                (void)bad(err, cmd, "unknown look `%s`", id);
                rc = PC_ERR_INVALID;
                goto fail;
            }
            if (have_loaded) {
                pc_lutfile_fini(&loaded);
            }
            loaded = f;
            have_loaded = true;
            snprintf(loaded_label, sizeof loaded_label, "%s", f.title != NULL ? f.title : "");
        }
    }
    const char *file_name = NULL;
    if ((rc = opt_str(err, cmd, p, "fileName", &file_name)) != PC_OK) {
        goto fail;
    }
    const char *data_text = NULL;
    if ((rc = opt_str(err, cmd, p, "data", &data_text)) != PC_OK) {
        goto fail;
    }
    if (data_text != NULL) {
        const char *fn = file_name != NULL ? file_name : "lut.cube";
        pc_lutfile f;
        memset(&f, 0, sizeof f);
        rc = pc_lutfile_parse(fn, (const uint8_t *)data_text, strlen(data_text), &f, err);
        if (rc != PC_OK) {
            goto fail; /* parse already set the message */
        }
        if (have_loaded) {
            pc_lutfile_fini(&loaded);
        }
        loaded = f;
        have_loaded = true;
        snprintf(loaded_label, sizeof loaded_label, "%s", base_name(fn));
    }
    /* `file` path reads: native-only; deferred (E3 file I/O). An empty/absent `file` is ignored. */
    const char *file_path = NULL;
    if ((rc = opt_str(err, cmd, p, "file", &file_path)) != PC_OK) {
        goto fail;
    }
    if (file_path != NULL && file_path[0] != '\0') {
        rc = bad(err, cmd, "reading a Color Lookup file is not yet supported in this build (E3 file I/O)");
        goto fail;
    }
    if (have_loaded) {
        if (!pc_lutfile_domain_is_default(&loaded)) {
            rc = bad(err, cmd,
                     "`%s` declares a custom DOMAIN: Color Lookup applies the plain 0..1 domain, so "
                     "the look would shift; honouring a custom domain is not supported yet",
                     loaded_label);
            goto fail;
        }
        pc_str_set_c(&name, loaded_label, NULL);
        size = (uint32_t)loaded.size;
        pc_blob_release(lut);
        lut = NULL;
        rc = pc_blob_new(loaded.data, loaded.ndata * sizeof(float), &lut, err);
        if (rc != PC_OK) {
            goto fail;
        }
    }
    const char *interp = NULL;
    if ((rc = opt_str(err, cmd, p, "interpolation", &interp)) != PC_OK) {
        goto fail;
    }
    if (interp != NULL) {
        if (strcmp(interp, "trilinear") == 0) {
            tetrahedral = false;
        } else if (strcmp(interp, "tetrahedral") == 0) {
            tetrahedral = true;
        } else {
            rc = bad(err, cmd, "`interpolation` must be \"trilinear\" or \"tetrahedral\", not `%s`", interp);
            goto fail;
        }
    }
    yyjson_val *tv = jget(p, "tetrahedral");
    if (tv != NULL) {
        if (!yyjson_is_bool(tv)) {
            rc = bad(err, cmd, "`tetrahedral` must be true or false");
            goto fail;
        }
        tetrahedral = yyjson_get_bool(tv);
    }
    yyjson_val *dv = jget(p, "dither");
    if (dv != NULL) {
        if (!yyjson_is_bool(dv)) {
            rc = bad(err, cmd, "`dither` must be true or false");
            goto fail;
        }
        dither = yyjson_get_bool(dv);
    }
    if (have_loaded) {
        pc_lutfile_fini(&loaded);
    }
    pc_adjustment a;
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_COLOR_LOOKUP;
    a.u.color_lookup.name = name;
    a.u.color_lookup.lut = lut;
    a.u.color_lookup.size = size;
    a.u.color_lookup.tetrahedral = tetrahedral;
    a.u.color_lookup.dither = dither;
    *out = a;
    return PC_OK;
fail:
    if (have_loaded) {
        pc_lutfile_fini(&loaded);
    }
    pc_str_fini(&name);
    pc_blob_release(lut);
    return rc != PC_OK ? rc : PC_ERR_INVALID;
}

/* ---- levels / curves / hue-saturation ----------------------------------------------------- */

static pc_status read_levels_channel(pc_error *err, const char *cmd, yyjson_val *o,
                                      const pc_levels_channel *c, pc_levels_channel *out)
{
    float in_black, in_white, gamma, out_black, out_white;
    pc_status rc;
    if ((rc = jnum(err, cmd, o, "inBlack", c->in_black * 255.0f, 0.0f, 253.0f, &in_black)) != PC_OK) {
        return rc;
    }
    if ((rc = jnum(err, cmd, o, "inWhite", c->in_white * 255.0f, 2.0f, 255.0f, &in_white)) != PC_OK) {
        return rc;
    }
    if (in_white < in_black + 2.0f) {
        in_white = in_black + 2.0f;
    }
    if ((rc = jnum(err, cmd, o, "gamma", c->gamma, 0.01f, 9.99f, &gamma)) != PC_OK) {
        return rc;
    }
    if ((rc = jnum(err, cmd, o, "outBlack", c->out_black * 255.0f, 0.0f, 255.0f, &out_black)) != PC_OK) {
        return rc;
    }
    if ((rc = jnum(err, cmd, o, "outWhite", c->out_white * 255.0f, 0.0f, 255.0f, &out_white)) != PC_OK) {
        return rc;
    }
    out->in_black = in_black / 255.0f;
    out->in_white = in_white / 255.0f;
    out->gamma = gamma;
    out->out_black = out_black / 255.0f;
    out->out_white = out_white / 255.0f;
    return PC_OK;
}

static pc_status levels(pc_error *err, const char *cmd, yyjson_val *p, const pc_adjustment *base,
                        pc_color_mode mode, pc_adjustment *out)
{
    pc_levels_channel master = pc_levels_channel_default();
    pc_levels_channel chans[4] = {pc_levels_channel_default(), pc_levels_channel_default(),
                                  pc_levels_channel_default(), pc_levels_channel_default()};
    bool has_base = false;
    pc_tone_space base_space = PC_TONE_RGB;
    if (base != NULL && base->kind == PC_ADJ_LEVELS) {
        master = base->u.levels.master;
        chans[0] = base->u.levels.per_channel[0];
        chans[1] = base->u.levels.per_channel[1];
        chans[2] = base->u.levels.per_channel[2];
        chans[3] = base->u.levels.black;
        base_space = base->u.levels.space;
        has_base = true;
    }
    pc_tone_space space = space_of(p, has_base, base_space, mode);
    if (has_base && base_space != space) {
        for (int i = 0; i < 4; i++) {
            chans[i] = pc_levels_channel_default();
        }
    }
    pc_status rc;
    if (space == PC_TONE_LAB) {
        if ((rc = read_levels_channel(err, cmd, p, &chans[0], &chans[0])) != PC_OK) {
            return rc;
        }
    } else {
        if ((rc = read_levels_channel(err, cmd, p, &master, &master)) != PC_OK) {
            return rc;
        }
        yyjson_val *o = NULL;
        if ((rc = jobject(err, cmd, p, "gray", &o)) != PC_OK) {
            return rc;
        }
        if (o != NULL && (rc = read_levels_channel(err, cmd, o, &master, &master)) != PC_OK) {
            return rc;
        }
    }
    size_t nk;
    const char *const *keys = channel_keys(space, &nk);
    for (size_t i = 0; i < nk; i++) {
        yyjson_val *o = NULL;
        if ((rc = jobject(err, cmd, p, keys[i], &o)) != PC_OK) {
            return rc;
        }
        if (o != NULL && (rc = read_levels_channel(err, cmd, o, &chans[i], &chans[i])) != PC_OK) {
            return rc;
        }
    }
    pc_adjustment a;
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_LEVELS;
    a.u.levels.master = master;
    a.u.levels.per_channel[0] = chans[0];
    a.u.levels.per_channel[1] = chans[1];
    a.u.levels.per_channel[2] = chans[2];
    a.u.levels.space = space;
    a.u.levels.black = (space == PC_TONE_CMYK) ? chans[3] : pc_levels_channel_default();
    *out = a;
    return PC_OK;
}

static pc_status curves(pc_error *err, const char *cmd, yyjson_val *p, const pc_adjustment *base,
                        pc_color_mode mode, pc_adjustment *out)
{
    pc_curve master, chans[4];
    memset(&master, 0, sizeof master);
    memset(chans, 0, sizeof chans);
    bool has_base = false, base_pencil = false;
    pc_tone_space base_space = PC_TONE_RGB;
    pc_status rc = PC_OK;
    /* Seed from base (or identities). */
    if (base != NULL && base->kind == PC_ADJ_CURVES) {
        has_base = true;
        base_space = base->u.curves.space;
        base_pencil = base->u.curves.pencil;
        if ((rc = pc_curve_clone(&base->u.curves.master, &master, err)) != PC_OK) {
            goto fail;
        }
        for (int i = 0; i < 3; i++) {
            if ((rc = pc_curve_clone(&base->u.curves.per_channel[i], &chans[i], err)) != PC_OK) {
                goto fail;
            }
        }
        if ((rc = pc_curve_clone(&base->u.curves.black, &chans[3], err)) != PC_OK) {
            goto fail;
        }
    } else {
        if ((rc = identity_curve(&master, err)) != PC_OK) {
            goto fail;
        }
        for (int i = 0; i < 4; i++) {
            if ((rc = identity_curve(&chans[i], err)) != PC_OK) {
                goto fail;
            }
        }
    }
    pc_tone_space space = space_of(p, has_base, base_space, mode);
    if (has_base && base_space != space) {
        for (int i = 0; i < 4; i++) {
            if ((rc = identity_curve(&chans[i], err)) != PC_OK) {
                goto fail;
            }
        }
    }
    bool pencil = base_pencil;
    if ((rc = jbool(err, cmd, p, "pencil", base_pencil, &pencil)) != PC_OK) {
        goto fail;
    }
    /* Convert each stored curve for the (possibly toggled) pencil mode. */
    pc_curve *all[5] = {&master, &chans[0], &chans[1], &chans[2], &chans[3]};
    for (int i = 0; i < 5; i++) {
        pc_curve conv;
        memset(&conv, 0, sizeof conv);
        bool did = false;
        if (!base_pencil && pencil) {
            rc = points_to_map(all[i]->pts, all[i]->len, &conv, err);
            did = true;
        } else if (base_pencil && !pencil) {
            pc_curve_point *pts = NULL;
            size_t n = 0;
            rc = map_to_points(all[i]->pts, all[i]->len, &pts, &n, err);
            if (rc == PC_OK) {
                rc = curve_set(&conv, pts, n, err);
            }
            free(pts);
            did = true;
        } else if (base_pencil && pencil && all[i]->len != PC_CURVE_MAP_LEN) {
            float outs[PC_CURVE_MAP_LEN];
            rc = pc_curve_map_outputs(all[i]->pts, all[i]->len, outs, err);
            if (rc == PC_OK) {
                rc = pc_curve_map(outs, PC_CURVE_MAP_LEN, &conv, err);
            }
            did = true;
        }
        if (did) {
            if (rc != PC_OK) {
                pc_curve_fini(&conv);
                goto fail;
            }
            pc_curve_fini(all[i]);
            *all[i] = conv;
        }
    }
    /* Composite (points or gray) and per-channel curves. */
    yyjson_val *composite = jget(p, "points");
    if (composite == NULL) {
        composite = jget(p, "gray");
    }
    if (composite != NULL) {
        pc_curve c;
        memset(&c, 0, sizeof c);
        if ((rc = parse_curve_in(err, cmd, "points", composite, pencil, &c)) != PC_OK) {
            goto fail;
        }
        if (space == PC_TONE_LAB) {
            pc_curve_fini(&chans[0]);
            chans[0] = c;
        } else {
            pc_curve_fini(&master);
            master = c;
        }
    }
    size_t nk;
    const char *const *keys = channel_keys(space, &nk);
    for (size_t i = 0; i < nk; i++) {
        yyjson_val *v = jget(p, keys[i]);
        if (v != NULL && i < 4) {
            pc_curve c;
            memset(&c, 0, sizeof c);
            if ((rc = parse_curve_in(err, cmd, keys[i], v, pencil, &c)) != PC_OK) {
                goto fail;
            }
            pc_curve_fini(&chans[i]);
            chans[i] = c;
        }
    }
    float passes_f;
    if ((rc = jnum(err, cmd, p, "smooth", 0.0f, 0.0f, 10.0f, &passes_f)) != PC_OK) {
        goto fail;
    }
    int passes = (int)(passes_f + 0.5f);
    if (pencil && passes > 0) {
        for (int i = 0; i < 5; i++) {
            float outs[PC_CURVE_MAP_LEN];
            if ((rc = pc_curve_map_outputs(all[i]->pts, all[i]->len, outs, err)) != PC_OK) {
                goto fail;
            }
            for (int k = 0; k < passes; k++) {
                smooth_curve_map(outs, PC_CURVE_MAP_LEN);
            }
            for (size_t k = 0; k < PC_CURVE_MAP_LEN; k++) {
                outs[k] = roundf(outs[k] * 255.0f * CURVE_COORD_PREC) / CURVE_COORD_PREC / 255.0f;
            }
            pc_curve nc;
            memset(&nc, 0, sizeof nc);
            if ((rc = pc_curve_map(outs, PC_CURVE_MAP_LEN, &nc, err)) != PC_OK) {
                goto fail;
            }
            pc_curve_fini(all[i]);
            *all[i] = nc;
        }
    }
    /* CMYK black channel kept only when non-identity. */
    bool identity = pencil ? pc_curve_is_identity(chans[3].pts, chans[3].len)
                           : pc_curve_is_identity(chans[3].pts, chans[3].len);
    pc_adjustment a;
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_CURVES;
    a.u.curves.master = master;
    a.u.curves.per_channel[0] = chans[0];
    a.u.curves.per_channel[1] = chans[1];
    a.u.curves.per_channel[2] = chans[2];
    a.u.curves.space = space;
    a.u.curves.pencil = pencil;
    if (space == PC_TONE_CMYK && !identity) {
        a.u.curves.black = chans[3];
    } else {
        pc_curve_fini(&chans[3]);
        a.u.curves.black.pts = NULL;
        a.u.curves.black.len = 0;
    }
    *out = a;
    return PC_OK;
fail:
    pc_curve_fini(&master);
    for (int i = 0; i < 4; i++) {
        pc_curve_fini(&chans[i]);
    }
    return rc != PC_OK ? rc : PC_ERR_INVALID;
}

static pc_status hue_saturation(pc_error *err, const char *cmd, yyjson_val *p, const pc_adjustment *base,
                                pc_adjustment *out)
{
    float h = 0.0f, s = 0.0f, l = 0.0f;
    bool c = false;
    pc_hue_range ranges[6];
    pc_hue_range_defaults(ranges);
    if (base != NULL && base->kind == PC_ADJ_HUE_SATURATION) {
        h = base->u.hue_saturation.hue;
        s = base->u.hue_saturation.saturation;
        l = base->u.hue_saturation.lightness;
        c = base->u.hue_saturation.colorize;
        memcpy(ranges, base->u.hue_saturation.ranges, sizeof ranges);
    }
    bool colorize = c;
    pc_status rc;
    if ((rc = jbool(err, cmd, p, "colorize", c, &colorize)) != PC_OK) {
        return rc;
    }
    float hue;
    if (colorize) {
        float h0 = fmodf(h, 360.0f);
        if (h0 < 0.0f) {
            h0 += 360.0f;
        }
        if ((rc = jnum(err, cmd, p, "hue", h0, 0.0f, 360.0f, &hue)) != PC_OK) {
            return rc;
        }
    } else {
        if ((rc = jnum(err, cmd, p, "hue", h, -180.0f, 180.0f, &hue)) != PC_OK) {
            return rc;
        }
    }
    float s_seed = s;
    if (colorize && !c && s_seed < 25.0f) {
        s_seed = 25.0f;
    }
    float saturation, lightness;
    if ((rc = jnum(err, cmd, p, "saturation", s_seed, colorize ? 0.0f : -100.0f, 100.0f, &saturation)) !=
        PC_OK) {
        return rc;
    }
    if ((rc = jnum(err, cmd, p, "lightness", l, -100.0f, 100.0f, &lightness)) != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < 6; i++) {
        yyjson_val *o = NULL;
        if ((rc = jobject(err, cmd, p, HUE_RANGES[i], &o)) != PC_OK) {
            return rc;
        }
        if (o == NULL) {
            continue;
        }
        if ((rc = jnum(err, cmd, o, "hue", ranges[i].hue, -180.0f, 180.0f, &ranges[i].hue)) != PC_OK) {
            return rc;
        }
        if ((rc = jnum(err, cmd, o, "saturation", ranges[i].saturation, -100.0f, 100.0f,
                       &ranges[i].saturation)) != PC_OK) {
            return rc;
        }
        if ((rc = jnum(err, cmd, o, "lightness", ranges[i].lightness, -100.0f, 100.0f,
                       &ranges[i].lightness)) != PC_OK) {
            return rc;
        }
        float b[4];
        bool present;
        if ((rc = jnumbers(err, cmd, o, "range", 4, b, &present)) != PC_OK) {
            return rc;
        }
        if (present) {
            pc_hue_range_canonical_bounds(b, ranges[i].bounds);
        }
    }
    pc_adjustment a;
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_HUE_SATURATION;
    a.u.hue_saturation.hue = hue;
    a.u.hue_saturation.saturation = saturation;
    a.u.hue_saturation.lightness = lightness;
    a.u.hue_saturation.colorize = colorize;
    memcpy(a.u.hue_saturation.ranges, ranges, sizeof ranges);
    *out = a;
    return PC_OK;
}

/* ---- from_params -------------------------------------------------------------------------- */

static pc_status gradient_stops(pc_error *err, const char *cmd, yyjson_val *v, pc_gradient_map_stop **out,
                                size_t *out_n);

pc_status pc_engine_adjustment_from_params(const char *kind, yyjson_val *p, const pc_adjustment *base,
                                           pc_color_mode mode, pc_adjustment *out, pc_error *err)
{
    char cmd[64];
    snprintf(cmd, sizeof cmd, "adjustment `%s`", kind);
    if (p != NULL && !yyjson_is_obj(p) && !yyjson_is_null(p)) {
        return bad(err, cmd, "params must be an object");
    }
    pc_adjustment a;
    memset(&a, 0, sizeof a);
    pc_status rc = PC_OK;
    if (strcmp(kind, "invert") == 0) {
        a.kind = PC_ADJ_INVERT;
    } else if (strcmp(kind, "brightnessContrast") == 0) {
        float b0 = 0.0f, c0 = 0.0f;
        bool l0 = false;
        if (base != NULL && base->kind == PC_ADJ_BRIGHTNESS_CONTRAST) {
            b0 = base->u.brightness_contrast.brightness;
            c0 = base->u.brightness_contrast.contrast;
            l0 = base->u.brightness_contrast.legacy;
        }
        bool legacy = l0;
        if ((rc = jbool(err, cmd, p, "legacy", l0, &legacy)) != PC_OK) {
            return rc;
        }
        float lo = legacy ? -100.0f : -50.0f, hi = 100.0f;
        float brightness, contrast;
        if ((rc = jnum(err, cmd, p, "brightness", b0, -150.0f, 150.0f, &brightness)) != PC_OK) {
            return rc;
        }
        if ((rc = jnum(err, cmd, p, "contrast", c0, lo, hi, &contrast)) != PC_OK) {
            return rc;
        }
        a.kind = PC_ADJ_BRIGHTNESS_CONTRAST;
        a.u.brightness_contrast.brightness = brightness;
        a.u.brightness_contrast.contrast = contrast;
        a.u.brightness_contrast.legacy = legacy;
    } else if (strcmp(kind, "threshold") == 0) {
        float l0 = 128.0f;
        if (base != NULL && base->kind == PC_ADJ_THRESHOLD) {
            l0 = base->u.threshold.level * 255.0f;
        }
        float level;
        if ((rc = jnum(err, cmd, p, "level", l0, 1.0f, 255.0f, &level)) != PC_OK) {
            return rc;
        }
        a.kind = PC_ADJ_THRESHOLD;
        a.u.threshold.level = roundf(level) / 255.0f;
    } else if (strcmp(kind, "posterize") == 0) {
        float l0 = 4.0f;
        if (base != NULL && base->kind == PC_ADJ_POSTERIZE) {
            l0 = (float)base->u.posterize.levels;
        }
        float levels;
        if ((rc = jnum(err, cmd, p, "levels", l0, 2.0f, 255.0f, &levels)) != PC_OK) {
            return rc;
        }
        a.kind = PC_ADJ_POSTERIZE;
        a.u.posterize.levels = (uint32_t)(roundf(levels));
    } else if (strcmp(kind, "exposure") == 0) {
        float e = 0.0f, o = 0.0f, g = 1.0f;
        if (base != NULL && base->kind == PC_ADJ_EXPOSURE) {
            e = base->u.exposure.exposure;
            o = base->u.exposure.offset;
            g = base->u.exposure.gamma;
        }
        float exposure, offset, gamma;
        if ((rc = jnum(err, cmd, p, "exposure", e, -20.0f, 20.0f, &exposure)) != PC_OK) {
            return rc;
        }
        if ((rc = jnum(err, cmd, p, "offset", o, -0.5f, 0.5f, &offset)) != PC_OK) {
            return rc;
        }
        if ((rc = jnum(err, cmd, p, "gamma", g, 0.01f, 9.99f, &gamma)) != PC_OK) {
            return rc;
        }
        a.kind = PC_ADJ_EXPOSURE;
        a.u.exposure.exposure = exposure;
        a.u.exposure.offset = offset;
        a.u.exposure.gamma = gamma;
    } else if (strcmp(kind, "vibrance") == 0) {
        float v = 0.0f, s = 0.0f;
        if (base != NULL && base->kind == PC_ADJ_VIBRANCE) {
            v = base->u.vibrance.vibrance;
            s = base->u.vibrance.saturation;
        }
        float vibrance, saturation;
        if ((rc = jnum(err, cmd, p, "vibrance", v, -100.0f, 100.0f, &vibrance)) != PC_OK) {
            return rc;
        }
        if ((rc = jnum(err, cmd, p, "saturation", s, -100.0f, 100.0f, &saturation)) != PC_OK) {
            return rc;
        }
        a.kind = PC_ADJ_VIBRANCE;
        a.u.vibrance.vibrance = vibrance;
        a.u.vibrance.saturation = saturation;
    } else if (strcmp(kind, "hueSaturation") == 0) {
        return hue_saturation(err, cmd, p, base, out);
    } else if (strcmp(kind, "levels") == 0) {
        return levels(err, cmd, p, base, mode, out);
    } else if (strcmp(kind, "curves") == 0) {
        return curves(err, cmd, p, base, mode, out);
    } else if (strcmp(kind, "colorBalance") == 0) {
        float tones[3][3] = {{0}};
        bool preserve = true;
        if (base != NULL && base->kind == PC_ADJ_COLOR_BALANCE) {
            memcpy(tones[0], base->u.color_balance.shadows, sizeof tones[0]);
            memcpy(tones[1], base->u.color_balance.midtones, sizeof tones[1]);
            memcpy(tones[2], base->u.color_balance.highlights, sizeof tones[2]);
            preserve = base->u.color_balance.preserve_luminosity;
        }
        static const char *const tk[3] = {"shadows", "midtones", "highlights"};
        for (int i = 0; i < 3; i++) {
            float v[3];
            bool present;
            if ((rc = jnumbers(err, cmd, p, tk[i], 3, v, &present)) != PC_OK) {
                return rc;
            }
            if (present) {
                for (int j = 0; j < 3; j++) {
                    tones[i][j] = clampf(v[j], -100.0f, 100.0f);
                }
            }
        }
        if ((rc = jbool(err, cmd, p, "preserveLuminosity", preserve, &preserve)) != PC_OK) {
            return rc;
        }
        a.kind = PC_ADJ_COLOR_BALANCE;
        memcpy(a.u.color_balance.shadows, tones[0], sizeof tones[0]);
        memcpy(a.u.color_balance.midtones, tones[1], sizeof tones[1]);
        memcpy(a.u.color_balance.highlights, tones[2], sizeof tones[2]);
        a.u.color_balance.preserve_luminosity = preserve;
    } else if (strcmp(kind, "blackWhite") == 0) {
        float weights[6];
        memcpy(weights, BW_DEFAULTS, sizeof weights);
        bool had_tint = false;
        float tint0[3] = {0.882f, 0.827f, 0.702f};
        if (base != NULL && base->kind == PC_ADJ_BLACK_WHITE) {
            memcpy(weights, base->u.black_white.weights, sizeof weights);
            had_tint = base->u.black_white.has_tint;
            if (had_tint) {
                memcpy(tint0, base->u.black_white.tint, sizeof tint0);
            }
        }
        for (int i = 0; i < 6; i++) {
            if ((rc = jnum(err, cmd, p, HUE_RANGES[i], weights[i], -200.0f, 300.0f, &weights[i])) != PC_OK) {
                return rc;
            }
        }
        float color[3] = {0, 0, 0};
        if ((rc = jcolor(err, cmd, p, "tintColor", tint0, color)) != PC_OK) {
            return rc;
        }
        bool tint;
        if ((rc = jbool(err, cmd, p, "tint", had_tint, &tint)) != PC_OK) {
            return rc;
        }
        a.kind = PC_ADJ_BLACK_WHITE;
        memcpy(a.u.black_white.weights, weights, sizeof weights);
        a.u.black_white.has_tint = tint;
        if (tint) {
            memcpy(a.u.black_white.tint, color, sizeof color);
        }
    } else if (strcmp(kind, "photoFilter") == 0) {
        float c0[3] = {236.0f / 255.0f, 138.0f / 255.0f, 0.0f};
        float d0 = 0.25f;
        bool p0 = true;
        if (base != NULL && base->kind == PC_ADJ_PHOTO_FILTER) {
            memcpy(c0, base->u.photo_filter.color, sizeof c0);
            d0 = base->u.photo_filter.density;
            p0 = base->u.photo_filter.preserve_luminosity;
        }
        float preset[3];
        bool has_preset = false;
        yyjson_val *fv = jget(p, "filter");
        if (fv != NULL) {
            if (!yyjson_is_str(fv)) {
                return bad(err, cmd, "`filter` must be a preset id");
            }
            const char *id = yyjson_get_str(fv);
            if (!pc_engine_photo_filter_preset(id, preset)) {
                return bad(err, cmd, "unknown filter `%s`", id);
            }
            has_preset = true;
        }
        float color[3] = {0, 0, 0};
        if ((rc = jcolor(err, cmd, p, "color", has_preset ? preset : c0, color)) != PC_OK) {
            return rc;
        }
        float density;
        if ((rc = jnum(err, cmd, p, "density", d0 * 100.0f, 0.0f, 100.0f, &density)) != PC_OK) {
            return rc;
        }
        bool preserve;
        if ((rc = jbool(err, cmd, p, "preserveLuminosity", p0, &preserve)) != PC_OK) {
            return rc;
        }
        a.kind = PC_ADJ_PHOTO_FILTER;
        memcpy(a.u.photo_filter.color, color, sizeof color);
        a.u.photo_filter.density = density / 100.0f;
        a.u.photo_filter.preserve_luminosity = preserve;
    } else if (strcmp(kind, "channelMixer") == 0) {
        float matrix[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        bool mono0 = false;
        if (base != NULL && base->kind == PC_ADJ_CHANNEL_MIXER) {
            memcpy(matrix, base->u.channel_mixer.matrix, sizeof matrix);
            mono0 = base->u.channel_mixer.monochrome;
        }
        bool monochrome;
        if ((rc = jbool(err, cmd, p, "monochrome", mono0, &monochrome)) != PC_OK) {
            return rc;
        }
        if (monochrome && !mono0 && jget(p, "gray") == NULL) {
            matrix[0][0] = 0.4f;
            matrix[0][1] = 0.4f;
            matrix[0][2] = 0.2f;
            matrix[0][3] = 0.0f;
        }
        static const char *const mk[3] = {"red", "green", "blue"};
        for (int i = 0; i < 3; i++) {
            float v[4];
            bool present;
            if ((rc = jnumbers(err, cmd, p, mk[i], 4, v, &present)) != PC_OK) {
                return rc;
            }
            if (present) {
                for (int j = 0; j < 4; j++) {
                    matrix[i][j] = clampf(v[j], -200.0f, 200.0f) / 100.0f;
                }
            }
        }
        float g[4];
        bool present;
        if ((rc = jnumbers(err, cmd, p, "gray", 4, g, &present)) != PC_OK) {
            return rc;
        }
        if (present) {
            for (int j = 0; j < 4; j++) {
                matrix[0][j] = clampf(g[j], -200.0f, 200.0f) / 100.0f;
            }
        }
        a.kind = PC_ADJ_CHANNEL_MIXER;
        memcpy(a.u.channel_mixer.matrix, matrix, sizeof matrix);
        a.u.channel_mixer.monochrome = monochrome;
    } else if (strcmp(kind, "gradientMap") == 0) {
        pc_gradient_map_stop *stops = NULL;
        size_t nstops = 0;
        bool rev0 = false, dither0 = false;
        if (base != NULL && base->kind == PC_ADJ_GRADIENT_MAP) {
            rev0 = base->u.gradient_map.reverse;
            dither0 = base->u.gradient_map.dither;
        }
        yyjson_val *sv = jget(p, "stops");
        if (sv != NULL) {
            if ((rc = gradient_stops(err, cmd, sv, &stops, &nstops)) != PC_OK) {
                return rc;
            }
        } else if (base != NULL && base->kind == PC_ADJ_GRADIENT_MAP && base->u.gradient_map.nstops > 0) {
            nstops = base->u.gradient_map.nstops;
            stops = pc_malloc_array(nstops, sizeof *stops);
            if (stops == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "gradient stops");
            }
            memcpy(stops, base->u.gradient_map.stops, nstops * sizeof *stops);
        } else {
            nstops = 2;
            stops = pc_malloc_array(2, sizeof *stops);
            if (stops == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "gradient stops");
            }
            stops[0] = (pc_gradient_map_stop){0.0f, {0.0f, 0.0f, 0.0f}};
            stops[1] = (pc_gradient_map_stop){1.0f, {1.0f, 1.0f, 1.0f}};
        }
        bool reverse, dither;
        if ((rc = jbool(err, cmd, p, "reverse", rev0, &reverse)) != PC_OK) {
            free(stops);
            return rc;
        }
        if ((rc = jbool(err, cmd, p, "dither", dither0, &dither)) != PC_OK) {
            free(stops);
            return rc;
        }
        a.kind = PC_ADJ_GRADIENT_MAP;
        a.u.gradient_map.stops = stops;
        a.u.gradient_map.nstops = nstops;
        a.u.gradient_map.reverse = reverse;
        a.u.gradient_map.dither = dither;
    } else if (strcmp(kind, "selectiveColor") == 0) {
        const pc_adjustment *b = (base != NULL && base->kind == PC_ADJ_SELECTIVE_COLOR) ? base : NULL;
        return pc_engine_selective_from_params(p, b, out, err);
    } else if (strcmp(kind, "colorLookup") == 0) {
        const pc_adjustment *b = (base != NULL && base->kind == PC_ADJ_COLOR_LOOKUP) ? base : NULL;
        return pc_engine_lookup_from_params(p, b, out, err);
    } else {
        return bad(err, "adjustment", "unknown adjustment kind `%s`", kind);
    }
    *out = a;
    return PC_OK;
}

static pc_status gradient_stops(pc_error *err, const char *cmd, yyjson_val *v, pc_gradient_map_stop **out,
                                size_t *out_n)
{
    const char *msg = "`stops` must be an array of 2..=64 [location 0..1, colour] pairs";
    if (!yyjson_is_arr(v)) {
        return bad(err, cmd, "%s", msg);
    }
    size_t n = yyjson_arr_size(v);
    if (n < 2 || n > 64) {
        return bad(err, cmd, "%s", msg);
    }
    pc_gradient_map_stop *s = pc_malloc_array(n, sizeof *s);
    if (s == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "gradient stops");
    }
    for (size_t i = 0; i < n; i++) {
        yyjson_val *item = yyjson_arr_get(v, i);
        yyjson_val *tv = NULL, *cv = NULL;
        if (yyjson_is_arr(item) && yyjson_arr_size(item) == 2) {
            tv = yyjson_arr_get(item, 0);
            cv = yyjson_arr_get(item, 1);
        } else if (yyjson_is_obj(item)) {
            tv = yyjson_obj_get(item, "location");
            cv = yyjson_obj_get(item, "color");
        } else {
            free(s);
            return bad(err, cmd, "%s", msg);
        }
        if (tv == NULL || !yyjson_is_num(tv) || !isfinite(yyjson_get_num(tv)) || cv == NULL) {
            free(s);
            return bad(err, cmd, "%s", msg);
        }
        s[i].pos = clampf((float)yyjson_get_num(tv), 0.0f, 1.0f);
        pc_status rc = color_of(err, cmd, "stops", cv, s[i].rgb);
        if (rc != PC_OK) {
            free(s);
            return rc;
        }
    }
    /* sort by position (stable insertion: n <= 64). */
    for (size_t i = 1; i < n; i++) {
        pc_gradient_map_stop key = s[i];
        size_t j = i;
        while (j > 0 && s[j - 1].pos > key.pos) {
            s[j] = s[j - 1];
            j--;
        }
        s[j] = key;
    }
    *out = s;
    *out_n = n;
    return PC_OK;
}

/* ---- to_params ---------------------------------------------------------------------------- */

static void hex_of(float c[3], char out[8])
{
    int r = (int)(clampf(c[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    int g = (int)(clampf(c[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    int b = (int)(clampf(c[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    snprintf(out, 8, "#%02x%02x%02x", r, g, b);
}

static double q255(float v)
{
    return (double)(roundf(v * 255.0f * 100.0f) / 100.0f);
}

static void levels_fields(yyjson_mut_doc *d, yyjson_mut_val *o, const pc_levels_channel *c)
{
    yyjson_mut_obj_add_real(d, o, "inBlack", q255(c->in_black));
    yyjson_mut_obj_add_real(d, o, "gamma", (double)(roundf(c->gamma * 1000.0f) / 1000.0f));
    yyjson_mut_obj_add_real(d, o, "inWhite", q255(c->in_white));
    yyjson_mut_obj_add_real(d, o, "outBlack", q255(c->out_black));
    yyjson_mut_obj_add_real(d, o, "outWhite", q255(c->out_white));
}

static yyjson_mut_val *levels_obj(yyjson_mut_doc *d, const pc_levels_channel *c)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    levels_fields(d, o, c);
    return o;
}

static double coord(float v)
{
    return (double)(roundf(v * 255.0f * CURVE_COORD_PREC) / CURVE_COORD_PREC);
}

/* A point curve as [[in, out], ...] in 0..255 (identity when fewer than two points). */
static yyjson_mut_val *curve_arr(yyjson_mut_doc *d, const pc_curve *c)
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    pc_curve_point id[2] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    const pc_curve_point *pts = (c->len >= 2) ? c->pts : id;
    size_t n = (c->len >= 2) ? c->len : 2;
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, pair, coord(pts[i].input));
        yyjson_mut_arr_add_real(d, pair, coord(pts[i].output));
        yyjson_mut_arr_add_val(a, pair);
    }
    return a;
}

/* A pencil-mode curve as 256 outputs in 0..255. */
static yyjson_mut_val *map_arr(yyjson_mut_doc *d, const pc_curve *c)
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    float outs[PC_CURVE_MAP_LEN];
    if (pc_curve_map_outputs(c->pts, c->len, outs, NULL) != PC_OK) {
        return a;
    }
    for (size_t i = 0; i < PC_CURVE_MAP_LEN; i++) {
        yyjson_mut_arr_add_real(d, a, coord(outs[i]));
    }
    return a;
}

static yyjson_mut_val *curve_param(yyjson_mut_doc *d, const pc_curve *c, bool pencil)
{
    return pencil ? map_arr(d, c) : curve_arr(d, c);
}

pc_status pc_engine_adjustment_to_params(const pc_adjustment *adj, yyjson_mut_doc *d,
                                         yyjson_mut_val **out, pc_error *err)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "to_params");
    }
    char hex[8];
    switch (adj->kind) {
    case PC_ADJ_INVERT:
    case PC_ADJ_UNSUPPORTED:
        break;
    case PC_ADJ_BRIGHTNESS_CONTRAST:
        yyjson_mut_obj_add_real(d, o, "brightness", adj->u.brightness_contrast.brightness);
        yyjson_mut_obj_add_real(d, o, "contrast", adj->u.brightness_contrast.contrast);
        yyjson_mut_obj_add_bool(d, o, "legacy", adj->u.brightness_contrast.legacy);
        break;
    case PC_ADJ_THRESHOLD:
        yyjson_mut_obj_add_real(d, o, "level", (double)roundf(adj->u.threshold.level * 255.0f));
        break;
    case PC_ADJ_POSTERIZE:
        yyjson_mut_obj_add_uint(d, o, "levels", adj->u.posterize.levels);
        break;
    case PC_ADJ_EXPOSURE:
        yyjson_mut_obj_add_real(d, o, "exposure", adj->u.exposure.exposure);
        yyjson_mut_obj_add_real(d, o, "offset", adj->u.exposure.offset);
        yyjson_mut_obj_add_real(d, o, "gamma", adj->u.exposure.gamma);
        break;
    case PC_ADJ_VIBRANCE:
        yyjson_mut_obj_add_real(d, o, "vibrance", adj->u.vibrance.vibrance);
        yyjson_mut_obj_add_real(d, o, "saturation", adj->u.vibrance.saturation);
        break;
    case PC_ADJ_HUE_SATURATION: {
        yyjson_mut_obj_add_real(d, o, "hue", adj->u.hue_saturation.hue);
        yyjson_mut_obj_add_real(d, o, "saturation", adj->u.hue_saturation.saturation);
        yyjson_mut_obj_add_real(d, o, "lightness", adj->u.hue_saturation.lightness);
        yyjson_mut_obj_add_bool(d, o, "colorize", adj->u.hue_saturation.colorize);
        for (size_t i = 0; i < 6; i++) {
            const pc_hue_range *r = &adj->u.hue_saturation.ranges[i];
            yyjson_mut_val *ro = yyjson_mut_obj(d);
            yyjson_mut_obj_add_real(d, ro, "hue", r->hue);
            yyjson_mut_obj_add_real(d, ro, "saturation", r->saturation);
            yyjson_mut_obj_add_real(d, ro, "lightness", r->lightness);
            yyjson_mut_val *b = yyjson_mut_arr(d);
            for (int k = 0; k < 4; k++) {
                yyjson_mut_arr_add_real(d, b, r->bounds[k]);
            }
            yyjson_mut_obj_add_val(d, ro, "range", b);
            yyjson_mut_obj_add_val(d, o, HUE_RANGES[i], ro);
        }
        break;
    }
    case PC_ADJ_LEVELS: {
        /* Top-level keys = the composite (master), or the lightness channel in Lab (no composite). */
        if (adj->u.levels.space == PC_TONE_LAB) {
            levels_fields(d, o, &adj->u.levels.per_channel[0]);
        } else {
            levels_fields(d, o, &adj->u.levels.master);
        }
        size_t nk;
        const char *const *keys = channel_keys(adj->u.levels.space, &nk);
        const pc_levels_channel *chans[4] = {&adj->u.levels.per_channel[0], &adj->u.levels.per_channel[1],
                                             &adj->u.levels.per_channel[2], &adj->u.levels.black};
        for (size_t i = 0; i < nk; i++) {
            if (adj->u.levels.space == PC_TONE_LAB && i == 0) {
                continue; /* lightness already written at top level */
            }
            yyjson_mut_obj_add_val(d, o, keys[i], levels_obj(d, chans[i]));
        }
        break;
    }
    case PC_ADJ_CURVES: {
        bool pencil = adj->u.curves.pencil;
        if (adj->u.curves.space == PC_TONE_LAB) {
            yyjson_mut_obj_add_val(d, o, "points", curve_param(d, &adj->u.curves.per_channel[0], pencil));
        } else {
            yyjson_mut_obj_add_val(d, o, "points", curve_param(d, &adj->u.curves.master, pencil));
        }
        size_t nk;
        const char *const *keys = channel_keys(adj->u.curves.space, &nk);
        pc_curve ink;
        memset(&ink, 0, sizeof ink);
        bool ink_identity = adj->u.curves.black.len < 2;
        const pc_curve *chans[4] = {&adj->u.curves.per_channel[0], &adj->u.curves.per_channel[1],
                                    &adj->u.curves.per_channel[2],
                                    ink_identity ? &ink : &adj->u.curves.black};
        if (ink_identity) {
            (void)identity_curve(&ink, NULL);
        }
        for (size_t i = 0; i < nk; i++) {
            if (adj->u.curves.space == PC_TONE_LAB && i == 0) {
                continue; /* points already carries the lightness channel */
            }
            yyjson_mut_obj_add_val(d, o, keys[i], curve_param(d, chans[i], pencil));
        }
        pc_curve_fini(&ink);
        if (pencil) {
            yyjson_mut_obj_add_bool(d, o, "pencil", true);
        }
        break;
    }
    case PC_ADJ_COLOR_BALANCE: {
        static const char *const tk[3] = {"shadows", "midtones", "highlights"};
        const float *tones[3] = {adj->u.color_balance.shadows, adj->u.color_balance.midtones,
                                 adj->u.color_balance.highlights};
        for (int i = 0; i < 3; i++) {
            yyjson_mut_val *arr = yyjson_mut_arr(d);
            for (int j = 0; j < 3; j++) {
                yyjson_mut_arr_add_real(d, arr, tones[i][j]);
            }
            yyjson_mut_obj_add_val(d, o, tk[i], arr);
        }
        yyjson_mut_obj_add_bool(d, o, "preserveLuminosity", adj->u.color_balance.preserve_luminosity);
        break;
    }
    case PC_ADJ_BLACK_WHITE: {
        float tint[3] = {0.882f, 0.827f, 0.702f};
        if (adj->u.black_white.has_tint) {
            memcpy(tint, adj->u.black_white.tint, sizeof tint);
        }
        hex_of(tint, hex);
        yyjson_mut_obj_add_bool(d, o, "tint", adj->u.black_white.has_tint);
        yyjson_mut_obj_add_strcpy(d, o, "tintColor", hex);
        for (int i = 0; i < 6; i++) {
            yyjson_mut_obj_add_real(d, o, HUE_RANGES[i], adj->u.black_white.weights[i]);
        }
        break;
    }
    case PC_ADJ_PHOTO_FILTER: {
        float col[3];
        memcpy(col, adj->u.photo_filter.color, sizeof col);
        hex_of(col, hex);
        yyjson_mut_obj_add_strcpy(d, o, "color", hex);
        yyjson_mut_obj_add_real(d, o, "density",
                                (double)(roundf(adj->u.photo_filter.density * 1000.0f) / 10.0f));
        yyjson_mut_obj_add_bool(d, o, "preserveLuminosity", adj->u.photo_filter.preserve_luminosity);
        break;
    }
    case PC_ADJ_CHANNEL_MIXER: {
        static const char *const mk[3] = {"red", "green", "blue"};
        for (int i = 0; i < 3; i++) {
            yyjson_mut_val *arr = yyjson_mut_arr(d);
            for (int j = 0; j < 4; j++) {
                yyjson_mut_arr_add_real(d, arr,
                                        (double)(roundf(adj->u.channel_mixer.matrix[i][j] * 1000.0f) / 10.0f));
            }
            yyjson_mut_obj_add_val(d, o, mk[i], arr);
        }
        yyjson_mut_obj_add_bool(d, o, "monochrome", adj->u.channel_mixer.monochrome);
        if (adj->u.channel_mixer.monochrome) {
            yyjson_mut_val *arr = yyjson_mut_arr(d);
            for (int j = 0; j < 4; j++) {
                yyjson_mut_arr_add_real(d, arr,
                                        (double)(roundf(adj->u.channel_mixer.matrix[0][j] * 1000.0f) / 10.0f));
            }
            yyjson_mut_obj_add_val(d, o, "gray", arr);
        }
        break;
    }
    case PC_ADJ_GRADIENT_MAP: {
        yyjson_mut_val *stops = yyjson_mut_arr(d);
        for (size_t i = 0; i < adj->u.gradient_map.nstops; i++) {
            float rgb[3];
            memcpy(rgb, adj->u.gradient_map.stops[i].rgb, sizeof rgb);
            hex_of(rgb, hex);
            yyjson_mut_val *pair = yyjson_mut_arr(d);
            yyjson_mut_arr_add_real(d, pair, adj->u.gradient_map.stops[i].pos);
            yyjson_mut_arr_add_strcpy(d, pair, hex);
            yyjson_mut_arr_add_val(stops, pair);
        }
        yyjson_mut_obj_add_val(d, o, "stops", stops);
        yyjson_mut_obj_add_bool(d, o, "reverse", adj->u.gradient_map.reverse);
        yyjson_mut_obj_add_bool(d, o, "dither", adj->u.gradient_map.dither);
        break;
    }
    case PC_ADJ_SELECTIVE_COLOR: {
        yyjson_mut_obj_add_strcpy(d, o, "method",
                                  adj->u.selective_color.relative ? "relative" : "absolute");
        for (int i = 0; i < 9; i++) {
            yyjson_mut_val *arr = yyjson_mut_arr(d);
            for (int j = 0; j < 4; j++) {
                yyjson_mut_arr_add_real(d, arr, adj->u.selective_color.adjustments[i][j]);
            }
            yyjson_mut_obj_add_val(d, o, SEL_RANGES[i], arr);
        }
        break;
    }
    case PC_ADJ_COLOR_LOOKUP:
        yyjson_mut_obj_add_strcpy(d, o, "interpolation",
                                  adj->u.color_lookup.tetrahedral ? "tetrahedral" : "trilinear");
        yyjson_mut_obj_add_bool(d, o, "dither", adj->u.color_lookup.dither);
        break;
    }
    *out = o;
    return PC_OK;
}

/* ---- default_for / kind / user_keys ------------------------------------------------------- */

pc_status pc_engine_adjustment_default_for(const char *kind, pc_color_mode mode, pc_adjustment *out,
                                           pc_error *err)
{
    return pc_engine_adjustment_from_params(kind, NULL, NULL, mode, out, err);
}

const char *pc_engine_adjustment_kind(const pc_adjustment *a)
{
    switch (a->kind) {
    case PC_ADJ_BRIGHTNESS_CONTRAST:
        return "brightnessContrast";
    case PC_ADJ_LEVELS:
        return "levels";
    case PC_ADJ_CURVES:
        return "curves";
    case PC_ADJ_EXPOSURE:
        return "exposure";
    case PC_ADJ_VIBRANCE:
        return "vibrance";
    case PC_ADJ_HUE_SATURATION:
        return "hueSaturation";
    case PC_ADJ_COLOR_BALANCE:
        return "colorBalance";
    case PC_ADJ_BLACK_WHITE:
        return "blackWhite";
    case PC_ADJ_PHOTO_FILTER:
        return "photoFilter";
    case PC_ADJ_CHANNEL_MIXER:
        return "channelMixer";
    case PC_ADJ_POSTERIZE:
        return "posterize";
    case PC_ADJ_THRESHOLD:
        return "threshold";
    case PC_ADJ_GRADIENT_MAP:
        return "gradientMap";
    case PC_ADJ_SELECTIVE_COLOR:
        return "selectiveColor";
    case PC_ADJ_COLOR_LOOKUP:
        return "colorLookup";
    default:
        return "invert";
    }
}

bool pc_engine_adjustment_user_keys_empty(yyjson_val *p)
{
    if (p == NULL || !yyjson_is_obj(p)) {
        return true;
    }
    size_t idx, max;
    yyjson_val *key, *val;
    yyjson_obj_foreach(p, idx, max, key, val) {
        const char *k = yyjson_get_str(key);
        if (k == NULL) {
            continue;
        }
        if (strcmp(k, "layer") == 0 || strcmp(k, "coalesce") == 0 || strcmp(k, "channel") == 0 ||
            strcmp(k, "targetChannel") == 0 || (k[0] == '_' && k[1] == '_')) {
            continue;
        }
        return false;
    }
    return true;
}
