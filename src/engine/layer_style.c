/* Layer styles: Layer > Layer Style commands and effect-building primitives (port of
 * crates/engine/src/layer_style.rs, E6).
 *
 * Never crashes on any params (REWRITE_PLAN.md rule 9): absent/null/malformed keys leave the
 * effect unchanged; unknown kinds are graceful errors.
 *
 * Pattern resources resolve in the same undoable edit. Per-kind user defaults are exposed
 * by style_editor.c and persisted with the session's preset store. */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "base/buf.h"
#include "base/str.h"
#include "color/blend.h"
#include "color/color.h"
#include "doc/adjust.h"
#include "doc/doc.h"
#include "doc/effects.h"
#include "doc/json.h"
#include "io/gradient_bake.h"
#include "psd/shc.h"

#include "brush_cmds.h" /* pc_engine_blend_from_str */
#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param, pc_engine_selected */
#include "layer_style.h"
#include "pattern_cmds.h"
#include "style_editor.h"

/* ============================================================== param readers (p may be NULL) */

static yyjson_val *obj_get(yyjson_val *p, const char *k)
{
    return (p != NULL && yyjson_is_obj(p)) ? yyjson_obj_get(p, k) : NULL;
}

/* f(p,k,d): number or default. */
static float jf(yyjson_val *p, const char *k, float d)
{
    yyjson_val *v = obj_get(p, k);
    return (v != NULL && yyjson_is_num(v)) ? (float)yyjson_get_num(v) : d;
}

/* f_of(p,k): Option<f32>. */
static bool jf_of(yyjson_val *p, const char *k, float *out)
{
    yyjson_val *v = obj_get(p, k);
    if (v != NULL && yyjson_is_num(v)) {
        *out = (float)yyjson_get_num(v);
        return true;
    }
    return false;
}

static bool jb(yyjson_val *p, const char *k, bool d)
{
    yyjson_val *v = obj_get(p, k);
    return (v != NULL && yyjson_is_bool(v)) ? yyjson_get_bool(v) : d;
}

static bool jb_of(yyjson_val *p, const char *k, bool *out)
{
    yyjson_val *v = obj_get(p, k);
    if (v != NULL && yyjson_is_bool(v)) {
        *out = yyjson_get_bool(v);
        return true;
    }
    return false;
}

static const char *jstr(yyjson_val *p, const char *k)
{
    yyjson_val *v = obj_get(p, k);
    return (v != NULL && yyjson_is_str(v)) ? yyjson_get_str(v) : NULL;
}

/* One hex digit pair at s[i..i+2] as 0..1, or 0.0 (Rust map_or(0.0)). */
static float hex_pair(const char *s, size_t len, size_t i)
{
    if (i + 2 > len) {
        return 0.0f;
    }
    int hi = -1, lo = -1;
    char a = s[i], b = s[i + 1];
    if (a >= '0' && a <= '9')
        hi = a - '0';
    else if (a >= 'a' && a <= 'f')
        hi = a - 'a' + 10;
    else if (a >= 'A' && a <= 'F')
        hi = a - 'A' + 10;
    if (b >= '0' && b <= '9')
        lo = b - '0';
    else if (b >= 'a' && b <= 'f')
        lo = b - 'a' + 10;
    else if (b >= 'A' && b <= 'F')
        lo = b - 'A' + 10;
    if (hi < 0 || lo < 0) {
        return 0.0f;
    }
    return (float)(hi * 16 + lo) / 255.0f;
}

/* color_of(p,k): [r,g,b,..] array (>=3) or "#rrggbb" string -> an rgb pc_color; false otherwise. */
static bool color_of(yyjson_val *p, const char *k, pc_color *out)
{
    yyjson_val *v = obj_get(p, k);
    if (v == NULL) {
        return false;
    }
    if (yyjson_is_arr(v) && yyjson_arr_size(v) >= 3) {
        yyjson_val *r = yyjson_arr_get(v, 0), *g = yyjson_arr_get(v, 1), *b = yyjson_arr_get(v, 2);
        float rf = (r != NULL && yyjson_is_num(r)) ? (float)yyjson_get_num(r) : 0.0f;
        float gf = (g != NULL && yyjson_is_num(g)) ? (float)yyjson_get_num(g) : 0.0f;
        float bf = (b != NULL && yyjson_is_num(b)) ? (float)yyjson_get_num(b) : 0.0f;
        *out = pc_color_rgb(rf, gf, bf);
        return true;
    }
    if (yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        size_t len = yyjson_get_len(v);
        while (len > 0 && *s == '#') {
            s++;
            len--;
        }
        if (len < 6) {
            return false;
        }
        *out = pc_color_rgb(hex_pair(s, len, 0), hex_pair(s, len, 2), hex_pair(s, len, 4));
        return true;
    }
    return false;
}

/* color(p,k,d): color_of as rgb, else default rgb. */
static pc_color color_or(yyjson_val *p, const char *k, float r, float g, float b)
{
    pc_color c;
    if (color_of(p, k, &c)) {
        float rgb[3];
        pc_color_to_rgb(&c, rgb);
        return pc_color_rgb(rgb[0], rgb[1], rgb[2]);
    }
    return pc_color_rgb(r, g, b);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ============================================================== contours */

typedef struct contour_pt {
    float input;
    float output;
    bool corner;
} contour_pt;

typedef struct contour_preset {
    const char *name;
    const contour_pt *pts;
    size_t npts;
} contour_preset;

#define CP(name_, ...)                                                                                       \
    {name_, (const contour_pt[])__VA_ARGS__, sizeof((const contour_pt[])__VA_ARGS__) / sizeof(contour_pt)}

/* Built-in contour presets, Photoshop's picker order (measured curves; see layer_style.rs). */
static const contour_preset CONTOURS[] = {
    CP("Linear", {{0.0f, 0.0f, false}, {1.0f, 1.0f, false}}),
    CP("Cone", {{0.0f, 0.0f, false},
                {0.172f, 0.477f, true},
                {0.195f, 0.555f, true},
                {0.266f, 0.711f, true},
                {0.383f, 0.922f, true},
                {0.43f, 0.969f, true},
                {0.477f, 0.992f, true},
                {0.523f, 0.992f, true},
                {0.57f, 0.969f, true},
                {0.664f, 0.859f, true},
                {0.703f, 0.789f, true},
                {0.742f, 0.711f, true},
                {0.836f, 0.477f, true},
                {0.859f, 0.398f, true},
                {0.914f, 0.266f, true},
                {0.945f, 0.156f, true},
                {0.977f, 0.078f, true},
                {1.0f, 0.0f, false}}),
    CP("Cone - Inverted",
       {{0.0f, 1.0f, false},    {0.031f, 0.922f, true}, {0.07f, 0.789f, true},  {0.172f, 0.523f, true},
        {0.195f, 0.445f, true}, {0.266f, 0.289f, true}, {0.383f, 0.078f, true}, {0.43f, 0.031f, true},
        {0.477f, 0.008f, true}, {0.523f, 0.008f, true}, {0.57f, 0.031f, true},  {0.664f, 0.141f, true},
        {0.703f, 0.211f, true}, {0.742f, 0.289f, true}, {0.836f, 0.523f, true}, {0.859f, 0.602f, true},
        {0.914f, 0.734f, true}, {0.945f, 0.844f, true}, {0.977f, 0.922f, true}, {1.0f, 1.0f, false}}),
    CP("Cove - Deep", {{0.0f, 0.258f, false},
                       {0.344f, 0.289f, true},
                       {0.383f, 0.297f, true},
                       {0.477f, 0.352f, true},
                       {0.57f, 0.43f, true},
                       {0.617f, 0.484f, true},
                       {0.664f, 0.57f, true},
                       {0.703f, 0.711f, true},
                       {0.742f, 0.906f, true},
                       {0.773f, 1.0f, true},
                       {1.0f, 1.0f, false}}),
    CP("Cove - Shallow", {{0.0f, 0.0f, false},
                          {0.008f, 0.164f, true},
                          {0.016f, 0.477f, true},
                          {0.039f, 0.508f, true},
                          {0.172f, 0.57f, true},
                          {0.305f, 0.688f, true},
                          {0.344f, 0.734f, true},
                          {0.477f, 0.945f, true},
                          {0.523f, 1.0f, true},
                          {1.0f, 1.0f, false}}),
    CP("Gaussian", {{0.0f, 0.0f, false},
                    {0.117f, 0.023f, true},
                    {0.172f, 0.055f, true},
                    {0.234f, 0.117f, true},
                    {0.305f, 0.234f, true},
                    {0.383f, 0.414f, true},
                    {0.523f, 0.688f, true},
                    {0.617f, 0.812f, true},
                    {0.703f, 0.891f, true},
                    {0.805f, 0.953f, true},
                    {1.0f, 1.0f, false}}),
    CP("Half Round", {{0.0f, 0.0f, false},
                      {0.07f, 0.18f, true},
                      {0.195f, 0.445f, true},
                      {0.266f, 0.555f, true},
                      {0.383f, 0.703f, true},
                      {0.523f, 0.828f, true},
                      {0.617f, 0.883f, true},
                      {0.773f, 0.945f, true},
                      {1.0f, 1.0f, false}}),
    CP("Ring",
       {{0.0f, 0.0f, false},    {0.031f, 0.078f, true}, {0.039f, 0.117f, true}, {0.117f, 0.383f, true},
        {0.172f, 0.609f, true}, {0.195f, 0.734f, true}, {0.266f, 0.953f, true}, {0.305f, 1.0f, true},
        {0.344f, 0.992f, true}, {0.383f, 0.922f, true}, {0.43f, 0.805f, true},  {0.477f, 0.641f, true},
        {0.57f, 0.281f, true},  {0.617f, 0.141f, true}, {0.664f, 0.055f, true}, {0.703f, 0.016f, true},
        {0.742f, 0.016f, true}, {0.773f, 0.062f, true}, {0.836f, 0.234f, true}, {0.859f, 0.344f, true},
        {0.891f, 0.445f, true}, {0.914f, 0.547f, true}, {0.945f, 0.734f, true}, {0.977f, 0.875f, true},
        {1.0f, 1.0f, false}}),
    CP("Ring - Double",
       {{0.0f, 0.0f, false},    {0.055f, 0.086f, true}, {0.094f, 0.172f, true}, {0.117f, 0.242f, true},
        {0.172f, 0.477f, true}, {0.195f, 0.625f, true}, {0.266f, 0.891f, true}, {0.305f, 0.945f, true},
        {0.344f, 0.891f, true}, {0.383f, 0.695f, true}, {0.43f, 0.367f, true},  {0.477f, 0.07f, true},
        {0.523f, 0.047f, true}, {0.57f, 0.32f, true},   {0.617f, 0.648f, true}, {0.664f, 0.867f, true},
        {0.703f, 0.93f, true},  {0.742f, 0.852f, true}, {0.773f, 0.641f, true}, {0.805f, 0.375f, true},
        {0.836f, 0.156f, true}, {0.859f, 0.031f, true}, {0.891f, 0.023f, true}, {0.914f, 0.109f, true},
        {0.977f, 0.703f, true}, {0.984f, 0.82f, true},  {1.0f, 1.0f, false}}),
    CP("Rolling Slope - Descending",
       {{0.0f, 1.0f, false},    {0.008f, 1.0f, true},   {0.031f, 0.93f, true},  {0.039f, 0.883f, true},
        {0.07f, 0.758f, true},  {0.141f, 0.539f, true}, {0.195f, 0.406f, true}, {0.266f, 0.352f, true},
        {0.305f, 0.352f, true}, {0.523f, 0.57f, true},  {0.57f, 0.602f, true},  {0.617f, 0.617f, true},
        {0.703f, 0.602f, true}, {0.742f, 0.562f, true}, {0.836f, 0.391f, true}, {0.914f, 0.211f, true},
        {0.945f, 0.109f, true}, {0.977f, 0.039f, true}, {0.984f, 0.008f, true}, {1.0f, 0.0f, false}}),
    CP("Rounded Steps",
       {{0.0f, 0.234f, false},  {0.031f, 0.305f, true}, {0.039f, 0.336f, true}, {0.094f, 0.43f, true},
        {0.141f, 0.477f, true}, {0.234f, 0.492f, true}, {0.266f, 0.516f, true}, {0.305f, 0.602f, true},
        {0.344f, 0.664f, true}, {0.383f, 0.711f, true}, {0.477f, 0.734f, true}, {0.523f, 0.797f, true},
        {0.57f, 0.898f, true},  {0.617f, 0.953f, true}, {0.664f, 0.984f, true}, {0.773f, 0.984f, true},
        {0.836f, 0.906f, true}, {0.891f, 0.891f, true}, {0.914f, 0.898f, true}, {1.0f, 1.0f, false}}),
    CP("Sawtooth 1", {{0.0f, 0.0f, false},
                      {0.031f, 0.047f, true},
                      {0.195f, 0.375f, true},
                      {0.266f, 0.492f, true},
                      {0.477f, 0.047f, true},
                      {0.523f, 0.055f, true},
                      {0.742f, 0.469f, true},
                      {0.773f, 0.445f, true},
                      {0.914f, 0.18f, true},
                      {0.945f, 0.102f, true},
                      {1.0f, 0.0f, false}}),
};
#define NCONTOURS (sizeof CONTOURS / sizeof CONTOURS[0])
const char *pc_engine_style_contour_name(size_t index)
{
    return index < NCONTOURS ? CONTOURS[index].name : NULL;
}

static const contour_preset LEGACY_CONTOURS[] = {
    CP("Cone (Inverted)", {{0.0f, 1.0f, false}, {0.5f, 0.0f, false}, {1.0f, 0.0f, false}}),
    CP("Domed", {{0.0f, 0.0f, false}, {0.25f, 1.0f, false}, {0.75f, 1.0f, false}, {1.0f, 0.0f, false}}),
    CP("Domed (Inverted)",
       {{0.0f, 1.0f, false}, {0.25f, 0.0f, false}, {0.75f, 0.0f, false}, {1.0f, 1.0f, false}}),
    CP("Diagonal (Descending)", {{0.0f, 1.0f, false}, {1.0f, 0.0f, false}}),
};
#define NLEGACY (sizeof LEGACY_CONTOURS / sizeof LEGACY_CONTOURS[0])

#define MAX_CONTOUR_POINTS 64u

static void contour_linear(pc_contour *out)
{
    out->custom = false;
    out->name = (pc_str)PC_STR_INIT;
    out->points = (pc_curve){NULL, 0};
    out->corners = NULL;
    out->ncorners = 0;
}

/* Build a Custom contour from input/output/corner arrays; corners empty unless any is a corner. */
static bool build_custom_contour(const char *name, const contour_pt *pts, size_t n, pc_contour *out)
{
    pc_contour c;
    c.custom = true;
    c.name = (pc_str)PC_STR_INIT;
    c.points = (pc_curve){NULL, 0};
    c.corners = NULL;
    c.ncorners = 0;
    if (pc_str_set_c(&c.name, name, NULL) != PC_OK) {
        return false;
    }
    c.points.pts = calloc(n, sizeof *c.points.pts);
    if (c.points.pts == NULL && n > 0) {
        pc_str_fini(&c.name);
        return false;
    }
    c.points.len = n;
    bool any_corner = false;
    for (size_t i = 0; i < n; i++) {
        c.points.pts[i].input = pts[i].input;
        c.points.pts[i].output = pts[i].output;
        if (pts[i].corner) {
            any_corner = true;
        }
    }
    if (any_corner) {
        c.corners = calloc(n, sizeof *c.corners);
        if (c.corners == NULL && n > 0) {
            pc_str_fini(&c.name);
            free(c.points.pts);
            return false;
        }
        c.ncorners = n;
        for (size_t i = 0; i < n; i++) {
            c.corners[i] = pts[i].corner;
        }
    }
    *out = c;
    return true;
}

bool pc_engine_builtin_contour(const char *name, pc_contour *out)
{
    if (name == NULL) {
        return false;
    }
    if (strcasecmp(name, "Linear") == 0) {
        contour_linear(out);
        return true;
    }
    for (size_t i = 1; i < NCONTOURS; i++) { /* skip Linear */
        if (strcmp(CONTOURS[i].name, name) == 0) {
            return build_custom_contour(CONTOURS[i].name, CONTOURS[i].pts, CONTOURS[i].npts, out);
        }
    }
    for (size_t i = 0; i < NLEGACY; i++) {
        if (strcmp(LEGACY_CONTOURS[i].name, name) == 0) {
            return build_custom_contour(LEGACY_CONTOURS[i].name, LEGACY_CONTOURS[i].pts,
                                        LEGACY_CONTOURS[i].npts, out);
        }
    }
    return false;
}

/* x as the shortest decimal that reads back as the same f32 (short()); here: a JSON real. */
static yyjson_mut_val *short_real(yyjson_mut_doc *d, float x)
{
    /* yyjson prints the shortest round-tripping decimal for a double; feeding the f32 value
     * (widened) matches Rust's f32::to_string().parse::<f64>() closely enough for params. */
    return yyjson_mut_real(d, (double)x);
}

pc_status pc_engine_contour_param(const pc_contour *c, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                  pc_error *err)
{
    if (!c->custom) {
        *out = yyjson_mut_str(doc, "Linear");
        return *out != NULL ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    /* A built-in preset the contour matches by name -> its name. */
    pc_contour builtin;
    if (pc_engine_builtin_contour(pc_str_c(&c->name), &builtin)) {
        bool same = pc_contour_eq(&builtin, c);
        pc_contour_fini(&builtin);
        if (same) {
            *out = yyjson_mut_str(doc, pc_str_c(&c->name));
            return *out != NULL ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_val *pts = yyjson_mut_arr(doc);
    yyjson_mut_val *corners = yyjson_mut_arr(doc);
    if (o == NULL || pts == NULL || corners == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < c->points.len; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(doc);
        if (pair == NULL || !yyjson_mut_arr_append(pair, short_real(doc, c->points.pts[i].input)) ||
            !yyjson_mut_arr_append(pair, short_real(doc, c->points.pts[i].output)) ||
            !yyjson_mut_arr_append(pts, pair)) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        bool corner = pc_contour_is_corner(c, i);
        if (!yyjson_mut_arr_add_bool(doc, corners, corner)) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    if (!yyjson_mut_obj_add_strcpy(doc, o, "name", pc_str_c(&c->name)) ||
        !yyjson_mut_obj_add_val(doc, o, "points", pts) ||
        !yyjson_mut_obj_add_val(doc, o, "corners", corners)) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *out = o;
    return PC_OK;
}

static int cmp_pt(const void *a, const void *b)
{
    float x = ((const contour_pt *)a)->input, y = ((const contour_pt *)b)->input;
    return (x < y) ? -1 : (x > y ? 1 : 0); /* total_cmp order matches for finite values */
}

bool pc_engine_contour_from_param(yyjson_val *v, pc_contour *out)
{
    if (v == NULL) {
        return false;
    }
    if (yyjson_is_str(v)) {
        return pc_engine_builtin_contour(yyjson_get_str(v), out);
    }
    if (!yyjson_is_obj(v)) {
        return false;
    }
    yyjson_val *raw = yyjson_obj_get(v, "points");
    if (raw == NULL || !yyjson_is_arr(raw)) {
        return false;
    }
    size_t n = yyjson_arr_size(raw);
    if (n < 2 || n > MAX_CONTOUR_POINTS) {
        return false;
    }
    yyjson_val *flags = yyjson_obj_get(v, "corners");
    if (flags != NULL && !yyjson_is_arr(flags)) {
        flags = NULL;
    }
    contour_pt *pts = calloc(n, sizeof *pts);
    if (pts == NULL) {
        return false;
    }
    size_t i = 0;
    yyjson_val *q;
    yyjson_arr_iter it = yyjson_arr_iter_with(raw);
    while ((q = yyjson_arr_iter_next(&it)) != NULL) {
        if (!yyjson_is_arr(q) || yyjson_arr_size(q) < 2) {
            free(pts);
            return false;
        }
        yyjson_val *qx = yyjson_arr_get(q, 0), *qy = yyjson_arr_get(q, 1);
        if (qx == NULL || qy == NULL || !yyjson_is_num(qx) || !yyjson_is_num(qy)) {
            free(pts);
            return false;
        }
        float x = (float)yyjson_get_num(qx), y = (float)yyjson_get_num(qy);
        if (!isfinite(x) || !isfinite(y)) {
            free(pts);
            return false;
        }
        bool corner = false;
        if (flags != NULL) {
            yyjson_val *fv = yyjson_arr_get(flags, i);
            corner = (fv != NULL && yyjson_is_bool(fv)) ? yyjson_get_bool(fv) : false;
        }
        pts[i].input = clampf(x, 0.0f, 1.0f);
        pts[i].output = clampf(y, 0.0f, 1.0f);
        pts[i].corner = corner;
        i++;
    }
    qsort(pts, n, sizeof *pts, cmp_pt);
    /* dedup by input within 1e-6 (keep first). */
    size_t m = 0;
    for (size_t j = 0; j < n; j++) {
        if (m > 0 && fabsf(pts[j].input - pts[m - 1].input) < 1e-6f) {
            continue;
        }
        pts[m++] = pts[j];
    }
    if (m < 2) {
        free(pts);
        return false;
    }
    const char *name = jstr(v, "name");
    bool ok = build_custom_contour(name != NULL ? name : "Custom", pts, m, out);
    free(pts);
    return ok;
}

pc_status pc_engine_contours_from_shc(const uint8_t *bytes, size_t len, yyjson_mut_doc *doc,
                                      yyjson_mut_val *arr, pc_error *err)
{
    pc_psd_shc_contour *file = NULL;
    size_t n = 0;
    pc_status rc = pc_psd_shc_parse(bytes, len, &file, &n, err);
    if (rc != PC_OK) {
        /* Reference wraps as "not a contour file: {e}" (EngineError::Other). */
        return pc_fail(err, PC_ERR_INVALID, "not a contour file");
    }
    for (size_t c = 0; c < n; c++) {
        /* Build the candidate param object, parse it (validates/clamps), re-emit canonically. */
        yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
        if (tmp == NULL) {
            pc_psd_shc_free(file, n);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_val *o = yyjson_mut_obj(tmp);
        yyjson_mut_val *pts = yyjson_mut_arr(tmp);
        yyjson_mut_val *corners = yyjson_mut_arr(tmp);
        bool oom = (o == NULL || pts == NULL || corners == NULL);
        for (size_t i = 0; !oom && i < file[c].npoints; i++) {
            yyjson_mut_val *pair = yyjson_mut_arr(tmp);
            if (pair == NULL ||
                !yyjson_mut_arr_add_real(tmp, pair, (double)file[c].points[i].input / 255.0) ||
                !yyjson_mut_arr_add_real(tmp, pair, (double)file[c].points[i].output / 255.0) ||
                !yyjson_mut_arr_append(pts, pair) ||
                !yyjson_mut_arr_add_bool(tmp, corners, !file[c].points[i].continuous)) {
                oom = true;
            }
        }
        if (!oom) {
            oom = !yyjson_mut_obj_add_strcpy(tmp, o, "name", pc_psd_str_c(&file[c].name)) ||
                  !yyjson_mut_obj_add_val(tmp, o, "points", pts) ||
                  !yyjson_mut_obj_add_val(tmp, o, "corners", corners);
        }
        if (oom) {
            yyjson_mut_doc_free(tmp);
            pc_psd_shc_free(file, n);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        /* Parse the mutable value: copy it to an immutable doc first. */
        yyjson_mut_doc_set_root(tmp, o);
        char *text = yyjson_mut_write(tmp, 0, NULL);
        yyjson_mut_doc_free(tmp);
        if (text == NULL) {
            pc_psd_shc_free(file, n);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_doc *idoc = yyjson_read(text, strlen(text), 0);
        free(text);
        pc_contour parsed;
        bool ok = idoc != NULL && pc_engine_contour_from_param(yyjson_doc_get_root(idoc), &parsed);
        if (idoc != NULL) {
            yyjson_doc_free(idoc);
        }
        if (!ok) {
            continue; /* unusable curve: skip */
        }
        yyjson_mut_val *param = NULL;
        rc = pc_engine_contour_param(&parsed, doc, &param, err);
        pc_contour_fini(&parsed);
        if (rc != PC_OK || !yyjson_mut_arr_append(arr, param)) {
            pc_psd_shc_free(file, n);
            return rc != PC_OK ? rc : pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    pc_psd_shc_free(file, n);
    return PC_OK;
}

pc_status pc_engine_contour_to_shc(const pc_contour *contour, pc_buf *out, pc_error *err)
{
    pc_psd_shc_contour c;
    c.name = (pc_psd_str)PC_STR_INIT;
    c.points = NULL;
    c.npoints = 0;
    const char *name = contour->custom ? pc_str_c(&contour->name) : "Linear";
    if (pc_psd_str_set(&c.name, name, strlen(name), err) != PC_OK) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    size_t n = contour->custom ? contour->points.len : 2;
    c.points = calloc(n, sizeof *c.points);
    if (c.points == NULL && n > 0) {
        pc_psd_str_fini(&c.name);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    c.npoints = n;
    if (contour->custom) {
        for (size_t i = 0; i < n; i++) {
            c.points[i].input = contour->points.pts[i].input * 255.0f;
            c.points[i].output = contour->points.pts[i].output * 255.0f;
            c.points[i].continuous = !pc_contour_is_corner(contour, i);
        }
    } else {
        c.points[0] = (pc_psd_shc_point){0.0f, 0.0f, true};
        c.points[1] = (pc_psd_shc_point){255.0f, 255.0f, true};
    }
    pc_psd_bytes bytes = {0};
    pc_status rc = pc_psd_shc_write(&c, 1, &bytes, err);
    pc_psd_str_fini(&c.name);
    free(c.points);
    if (rc != PC_OK) {
        return pc_fail(err, PC_ERR_INVALID, "can't write the contour");
    }
    pc_buf_init(out);
    rc = pc_buf_append(out, bytes.data, bytes.len, err);
    pc_psd_bytes_fini(&bytes);
    return rc;
}

/* ============================================================== gradients */

pc_gradient_style pc_engine_gradient_style(const char *name)
{
    if (name == NULL) {
        return PC_GRADIENT_LINEAR;
    }
    if (strcasecmp(name, "radial") == 0)
        return PC_GRADIENT_RADIAL;
    if (strcasecmp(name, "angle") == 0)
        return PC_GRADIENT_ANGLE;
    if (strcasecmp(name, "reflected") == 0)
        return PC_GRADIENT_REFLECTED;
    if (strcasecmp(name, "diamond") == 0)
        return PC_GRADIENT_DIAMOND;
    return PC_GRADIENT_LINEAR;
}

static bool bake_method_from_name(const char *name, pc_io_bake_method *out)
{
    if (name == NULL) {
        return false;
    }
    char low[16];
    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof low - 1; i++) {
        low[i] = (char)tolower((unsigned char)name[i]);
    }
    low[i] = '\0';
    if (strcmp(low, "classic") == 0) {
        *out = PC_IO_BAKE_CLASSIC;
        return true;
    }
    if (strcmp(low, "perceptual") == 0) {
        *out = PC_IO_BAKE_PERCEPTUAL;
        return true;
    }
    if (strcmp(low, "linear") == 0) {
        *out = PC_IO_BAKE_LINEAR;
        return true;
    }
    if (strcmp(low, "smooth") == 0) {
        *out = PC_IO_BAKE_SMOOTH;
        return true;
    }
    return false;
}

/* Two-colour stops, baked with Photoshop's interpolation when `method` names one. *out owned. */
static bool gradient_stops(yyjson_val *p, pc_color from, pc_color to, pc_color_stop **out, size_t *nout)
{
    pc_io_bake_method method;
    if (bake_method_from_name(jstr(p, "method"), &method)) {
        pc_io_color_stop in[2] = {{0.0f, from}, {1.0f, to}};
        float mid = 0.5f;
        float smooth = clampf(jf(p, "smoothness", 100.0f) / 100.0f, 0.0f, 1.0f);
        pc_io_color_stop *baked = NULL;
        size_t nb = 0;
        if (pc_io_gradient_bake(in, 2, &mid, 1, smooth, method, &baked, &nb, NULL) != PC_OK) {
            return false;
        }
        pc_color_stop *s = calloc(nb, sizeof *s);
        if (s == NULL && nb > 0) {
            free(baked);
            return false;
        }
        for (size_t i = 0; i < nb; i++) {
            s[i].pos = baked[i].at;
            s[i].color = baked[i].color;
        }
        free(baked);
        *out = s;
        *nout = nb;
        return true;
    }
    pc_color_stop *s = calloc(2, sizeof *s);
    if (s == NULL) {
        return false;
    }
    s[0] = (pc_color_stop){0.0f, from};
    s[1] = (pc_color_stop){1.0f, to};
    *out = s;
    *nout = 2;
    return true;
}

/* A fresh two-colour gradient from params; false on OOM. On success caller owns *g. */
static bool gradient_from_params(yyjson_val *p, pc_gradient *g)
{
    if (pc_gradient_default(g, NULL) != PC_OK) {
        return false;
    }
    pc_color_stop *stops = NULL;
    size_t n = 0;
    if (!gradient_stops(p, color_or(p, "from", 0, 0, 0), color_or(p, "to", 1, 1, 1), &stops, &n)) {
        pc_gradient_fini(g);
        return false;
    }
    free(g->stops);
    g->stops = stops;
    g->nstops = n;
    g->style = pc_engine_gradient_style(jstr(p, "style"));
    g->angle = jf(p, "angle", 90.0f);
    g->scale = jf(p, "scale", 100.0f) / 100.0f;
    g->reverse = jb(p, "reverse", false);
    return true;
}

/* ============================================================== fresh effects (factory) */

static void fresh_shadow(pc_shadow *s, bool inner)
{
    s->common = pc_fx_common_new(PC_BLEND_MULTIPLY, 0.75f);
    s->color = PC_COLOR_BLACK;
    s->angle = 120.0f;
    s->use_global_light = true;
    s->distance = 5.0f;
    s->spread = 0.0f;
    s->size = 5.0f;
    contour_linear(&s->contour);
    s->anti_alias = false;
    s->noise = 0.0f;
    s->knocks_out = !inner;
}

static void fresh_glow(pc_glow *g)
{
    g->common = pc_fx_common_new(PC_BLEND_SCREEN, 0.75f);
    g->paint.kind = PC_FX_PAINT_COLOR;
    g->paint.u.color = pc_color_rgb(1.0f, 1.0f, 190.0f / 255.0f);
    g->technique = PC_GLOW_SOFTER;
    g->spread = 0.0f;
    g->size = 5.0f;
    contour_linear(&g->contour);
    g->anti_alias = false;
    g->range = 0.5f;
    g->jitter = 0.0f;
    g->noise = 0.0f;
    g->source = PC_GLOW_SOURCE_EDGE;
}

/* An effect of `kind` with its factory settings; false for an unknown kind. */
static bool fresh_effect(const char *kind, pc_effect *out)
{
    memset(out, 0, sizeof *out);
    if (strcmp(kind, "dropShadow") == 0) {
        out->kind = PC_FX_DROP_SHADOW;
        fresh_shadow(&out->u.shadow, false);
    } else if (strcmp(kind, "innerShadow") == 0) {
        out->kind = PC_FX_INNER_SHADOW;
        fresh_shadow(&out->u.shadow, true);
    } else if (strcmp(kind, "outerGlow") == 0) {
        out->kind = PC_FX_OUTER_GLOW;
        fresh_glow(&out->u.glow);
    } else if (strcmp(kind, "innerGlow") == 0) {
        out->kind = PC_FX_INNER_GLOW;
        fresh_glow(&out->u.glow);
    } else if (strcmp(kind, "stroke") == 0) {
        out->kind = PC_FX_STROKE;
        out->u.stroke.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
        out->u.stroke.size = 3.0f;
        out->u.stroke.position = PC_STROKE_OUTSIDE;
        out->u.stroke.paint.kind = PC_FX_PAINT_COLOR;
        out->u.stroke.paint.u.color = pc_color_rgb(1.0f, 0.0f, 0.0f);
    } else if (strcmp(kind, "colorOverlay") == 0) {
        out->kind = PC_FX_COLOR_OVERLAY;
        out->u.color_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
        out->u.color_overlay.color = pc_color_rgb(1.0f, 0.0f, 0.0f);
    } else if (strcmp(kind, "gradientOverlay") == 0) {
        out->kind = PC_FX_GRADIENT_OVERLAY;
        out->u.gradient_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
        if (pc_gradient_default(&out->u.gradient_overlay.gradient, NULL) != PC_OK) {
            return false;
        }
        out->u.gradient_overlay.dither = false;
    } else if (strcmp(kind, "patternOverlay") == 0) {
        out->kind = PC_FX_PATTERN_OVERLAY;
        out->u.pattern_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
        out->u.pattern_overlay.scale = 1.0f;
        out->u.pattern_overlay.angle = 0.0f;
        out->u.pattern_overlay.link = true;
    } else if (strcmp(kind, "satin") == 0) {
        out->kind = PC_FX_SATIN;
        out->u.satin.common = pc_fx_common_new(PC_BLEND_MULTIPLY, 0.5f);
        out->u.satin.color = PC_COLOR_BLACK;
        out->u.satin.angle = 19.0f;
        out->u.satin.distance = 11.0f;
        out->u.satin.size = 14.0f;
        contour_linear(&out->u.satin.contour);
        out->u.satin.anti_alias = true;
        out->u.satin.invert = true;
    } else if (strcmp(kind, "bevelEmboss") == 0) {
        out->kind = PC_FX_BEVEL_EMBOSS;
        pc_bevel *b = &out->u.bevel;
        b->enabled = true;
        b->style = PC_BEVEL_INNER;
        b->technique = PC_BEVEL_SMOOTH;
        b->depth = 1.0f;
        b->up = true;
        b->size = 5.0f;
        b->soften = 0.0f;
        b->angle = 120.0f;
        b->altitude = 30.0f;
        b->use_global_light = true;
        contour_linear(&b->gloss_contour);
        b->highlight = pc_fx_common_new(PC_BLEND_SCREEN, 0.75f);
        b->highlight_color = PC_COLOR_WHITE;
        b->shadow = pc_fx_common_new(PC_BLEND_MULTIPLY, 0.75f);
        b->shadow_color = PC_COLOR_BLACK;
        b->has_contour = false;
        b->has_texture = false;
    } else {
        return false;
    }
    return true;
}

/* ============================================================== overlay params onto an effect */

static void set_color(yyjson_val *p, const char *k, pc_color *c)
{
    pc_color x;
    if (color_of(p, k, &x)) {
        *c = x;
    }
}

static void set_contour_field(yyjson_val *p, const char *key, pc_contour *c)
{
    yyjson_val *v = obj_get(p, key);
    pc_contour x;
    if (v != NULL && pc_engine_contour_from_param(v, &x)) {
        pc_contour_fini(c);
        *c = x;
    }
}

static void set_bool_field(yyjson_val *p, const char *key, bool *b)
{
    bool x;
    if (jb_of(p, key, &x)) {
        *b = x;
    }
}

static void overlay_common(pc_fx_common *c, yyjson_val *p)
{
    bool bx;
    if (jb_of(p, "enabled", &bx)) {
        c->enabled = bx;
    }
    const char *blend = jstr(p, "blend");
    pc_blend_mode m;
    if (blend != NULL && pc_engine_blend_from_str(blend, strlen(blend), &m)) {
        c->blend = m;
    }
    float x;
    if (jf_of(p, "opacity", &x)) {
        c->opacity = clampf(x / 100.0f, 0.0f, 1.0f);
    }
}

/* Edits a gradient in place: only end colours and geometry (overlay_gradient). */
static void overlay_gradient(pc_gradient *g, yyjson_val *p)
{
    pc_io_bake_method method;
    if (bake_method_from_name(jstr(p, "method"), &method)) {
        pc_color from = (g->nstops > 0) ? g->stops[0].color : PC_COLOR_BLACK;
        pc_color to = (g->nstops > 0) ? g->stops[g->nstops - 1].color : PC_COLOR_WHITE;
        pc_color cf, ct;
        if (color_of(p, "from", &cf))
            from = cf;
        if (color_of(p, "to", &ct))
            to = ct;
        pc_color_stop *stops = NULL;
        size_t n = 0;
        if (gradient_stops(p, from, to, &stops, &n)) {
            free(g->stops);
            g->stops = stops;
            g->nstops = n;
        }
    }
    pc_color c;
    if (color_of(p, "from", &c) && g->nstops > 0) {
        g->stops[0].color = c;
    }
    if (color_of(p, "to", &c) && g->nstops > 0) {
        g->stops[g->nstops - 1].color = c;
    }
    const char *style = jstr(p, "style");
    if (style != NULL) {
        g->style = pc_engine_gradient_style(style);
    }
    float x;
    if (jf_of(p, "angle", &x))
        g->angle = x;
    if (jf_of(p, "scale", &x))
        g->scale = clampf(x / 100.0f, 0.01f, 10.0f);
    bool bx;
    if (jb_of(p, "reverse", &bx))
        g->reverse = bx;
}

void pc_engine_overlay_effect(pc_effect *fx, yyjson_val *p)
{
    float x;
    const char *s;
    switch (fx->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW: {
        pc_shadow *sh = &fx->u.shadow;
        bool inner = (fx->kind == PC_FX_INNER_SHADOW);
        overlay_common(&sh->common, p);
        set_color(p, "color", &sh->color);
        if (jf_of(p, "angle", &x))
            sh->angle = x;
        set_bool_field(p, "useGlobalLight", &sh->use_global_light);
        if (jf_of(p, "distance", &x))
            sh->distance = x;
        if (jf_of(p, inner ? "choke" : "spread", &x))
            sh->spread = clampf(x / 100.0f, 0.0f, 1.0f);
        if (jf_of(p, "size", &x))
            sh->size = x;
        set_contour_field(p, "contour", &sh->contour);
        set_bool_field(p, "antiAlias", &sh->anti_alias);
        if (jf_of(p, "noise", &x))
            sh->noise = clampf(x / 100.0f, 0.0f, 1.0f);
        if (!inner) {
            set_bool_field(p, "knocksOut", &sh->knocks_out);
        }
        break;
    }
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW: {
        pc_glow *g = &fx->u.glow;
        bool inner = (fx->kind == PC_FX_INNER_GLOW);
        overlay_common(&g->common, p);
        pc_color c;
        if (color_of(p, "color", &c)) {
            pc_fx_paint_fini(&g->paint);
            g->paint.kind = PC_FX_PAINT_COLOR;
            g->paint.u.color = c;
        }
        s = jstr(p, "technique");
        if (s != NULL) {
            g->technique = (strcmp(s, "precise") == 0) ? PC_GLOW_PRECISE : PC_GLOW_SOFTER;
        }
        if (jf_of(p, inner ? "choke" : "spread", &x))
            g->spread = clampf(x / 100.0f, 0.0f, 1.0f);
        if (jf_of(p, "size", &x))
            g->size = x;
        set_contour_field(p, "contour", &g->contour);
        set_bool_field(p, "antiAlias", &g->anti_alias);
        if (jf_of(p, "range", &x))
            g->range = clampf(x / 100.0f, 0.01f, 1.0f);
        if (jf_of(p, "noise", &x))
            g->noise = clampf(x / 100.0f, 0.0f, 1.0f);
        if (inner) {
            s = jstr(p, "source");
            if (s != NULL) {
                g->source = (strcmp(s, "center") == 0) ? PC_GLOW_SOURCE_CENTER : PC_GLOW_SOURCE_EDGE;
            }
        }
        break;
    }
    case PC_FX_STROKE: {
        pc_stroke_fx *st = &fx->u.stroke;
        overlay_common(&st->common, p);
        if (jf_of(p, "size", &x))
            st->size = x;
        s = jstr(p, "position");
        if (s != NULL) {
            st->position = (strcmp(s, "inside") == 0)   ? PC_STROKE_INSIDE
                           : (strcmp(s, "center") == 0) ? PC_STROKE_CENTER
                                                        : PC_STROKE_OUTSIDE;
        }
        if (obj_get(p, "from") != NULL) {
            pc_gradient g;
            if (gradient_from_params(p, &g)) {
                pc_fx_paint_fini(&st->paint);
                st->paint.kind = PC_FX_PAINT_GRADIENT;
                st->paint.u.gradient = g;
            }
        } else {
            pc_color c;
            if (color_of(p, "color", &c)) {
                pc_fx_paint_fini(&st->paint);
                st->paint.kind = PC_FX_PAINT_COLOR;
                st->paint.u.color = c;
            }
        }
        break;
    }
    case PC_FX_COLOR_OVERLAY:
        overlay_common(&fx->u.color_overlay.common, p);
        set_color(p, "color", &fx->u.color_overlay.color);
        break;
    case PC_FX_GRADIENT_OVERLAY:
        overlay_common(&fx->u.gradient_overlay.common, p);
        overlay_gradient(&fx->u.gradient_overlay.gradient, p);
        set_bool_field(p, "dither", &fx->u.gradient_overlay.dither);
        break;
    case PC_FX_PATTERN_OVERLAY: {
        overlay_common(&fx->u.pattern_overlay.common, p);
        s = jstr(p, "pattern");
        if (s != NULL) {
            (void)pc_str_set_c(&fx->u.pattern_overlay.name, s, NULL);
            pc_str_fini(&fx->u.pattern_overlay.id);
            fx->u.pattern_overlay.id = (pc_str)PC_STR_INIT;
        }
        if (jf_of(p, "scale", &x))
            fx->u.pattern_overlay.scale = clampf(x / 100.0f, 0.01f, 10.0f);
        if (jf_of(p, "angle", &x))
            fx->u.pattern_overlay.angle = x;
        set_bool_field(p, "link", &fx->u.pattern_overlay.link);
        if (jf_of(p, "phaseX", &x))
            fx->u.pattern_overlay.phase[0] = x;
        if (jf_of(p, "phaseY", &x))
            fx->u.pattern_overlay.phase[1] = x;
        break;
    }
    case PC_FX_SATIN: {
        pc_satin *sa = &fx->u.satin;
        overlay_common(&sa->common, p);
        set_color(p, "color", &sa->color);
        if (jf_of(p, "angle", &x))
            sa->angle = x;
        if (jf_of(p, "distance", &x))
            sa->distance = x;
        if (jf_of(p, "size", &x))
            sa->size = x;
        set_bool_field(p, "invert", &sa->invert);
        set_contour_field(p, "contour", &sa->contour);
        set_bool_field(p, "antiAlias", &sa->anti_alias);
        break;
    }
    case PC_FX_BEVEL_EMBOSS: {
        pc_bevel *b = &fx->u.bevel;
        set_bool_field(p, "enabled", &b->enabled);
        s = jstr(p, "style");
        if (s != NULL) {
            b->style = (strcmp(s, "outer") == 0)    ? PC_BEVEL_OUTER
                       : (strcmp(s, "emboss") == 0) ? PC_BEVEL_EMBOSS
                       : (strcmp(s, "pillow") == 0) ? PC_BEVEL_PILLOW_EMBOSS
                       : (strcmp(s, "stroke") == 0) ? PC_BEVEL_STROKE_EMBOSS
                                                    : PC_BEVEL_INNER;
        }
        s = jstr(p, "technique");
        if (s != NULL) {
            b->technique = (strcmp(s, "chiselHard") == 0)   ? PC_BEVEL_CHISEL_HARD
                           : (strcmp(s, "chiselSoft") == 0) ? PC_BEVEL_CHISEL_SOFT
                                                            : PC_BEVEL_SMOOTH;
        }
        if (jf_of(p, "depth", &x))
            b->depth = x / 100.0f;
        s = jstr(p, "direction");
        if (s != NULL) {
            b->up = (strcmp(s, "down") != 0);
        }
        if (jf_of(p, "size", &x))
            b->size = x;
        if (jf_of(p, "soften", &x))
            b->soften = x;
        if (jf_of(p, "angle", &x))
            b->angle = x;
        if (jf_of(p, "altitude", &x))
            b->altitude = x;
        set_bool_field(p, "useGlobalLight", &b->use_global_light);
        set_contour_field(p, "glossContour", &b->gloss_contour);
        bool cv;
        if (jb_of(p, "contour", &cv)) {
            if (!cv) {
                if (b->has_contour) {
                    pc_contour_fini(&b->contour.contour);
                    b->has_contour = false;
                }
            } else if (!b->has_contour) {
                b->contour = pc_bevel_contour_default();
                b->has_contour = true;
            }
        }
        if (b->has_contour) {
            set_contour_field(p, "contourName", &b->contour.contour);
            if (jf_of(p, "contourRange", &x))
                b->contour.range = clampf(x / 100.0f, 0.01f, 1.0f);
            set_bool_field(p, "contourAntiAlias", &b->contour.anti_alias);
        }
        s = jstr(p, "texture");
        if (s != NULL && s[0] != '\0') {
            if (b->has_texture) {
                pc_str_fini(&b->texture.name);
                pc_str_fini(&b->texture.id);
            }
            memset(&b->texture, 0, sizeof b->texture);
            (void)pc_str_set_c(&b->texture.name, s, NULL);
            (void)pc_str_set_c(&b->texture.id, s, NULL);
            b->texture.scale = clampf(jf(p, "textureScale", 100.0f) / 100.0f, 0.01f, 10.0f);
            b->texture.depth = clampf(jf(p, "textureDepth", 100.0f) / 100.0f, -10.0f, 10.0f);
            b->texture.invert = jb(p, "textureInvert", false);
            b->texture.link = jb(p, "textureLink", true);
            b->texture.phase[0] = 0.0f;
            b->texture.phase[1] = 0.0f;
            b->has_texture = true;
        }
        break;
    }
    default:
        break;
    }
}

bool pc_engine_effect_from_params(const char *kind, yyjson_val *p, pc_effect *out)
{
    if (kind == NULL || !fresh_effect(kind, out)) {
        return false;
    }
    pc_engine_overlay_effect(out, p);
    return true;
}

const char *pc_engine_effect_kind_of(const pc_effect *e)
{
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
        return "dropShadow";
    case PC_FX_INNER_SHADOW:
        return "innerShadow";
    case PC_FX_OUTER_GLOW:
        return "outerGlow";
    case PC_FX_INNER_GLOW:
        return "innerGlow";
    case PC_FX_STROKE:
        return "stroke";
    case PC_FX_COLOR_OVERLAY:
        return "colorOverlay";
    case PC_FX_GRADIENT_OVERLAY:
        return "gradientOverlay";
    case PC_FX_PATTERN_OVERLAY:
        return "patternOverlay";
    case PC_FX_SATIN:
        return "satin";
    case PC_FX_BEVEL_EMBOSS:
        return "bevelEmboss";
    }
    return "dropShadow";
}

/* ============================================================== factory-default params json */

static bool add_s(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, const char *v)
{
    return yyjson_mut_obj_add_strcpy(d, o, k, v);
}
static bool add_i(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, int64_t v)
{
    return yyjson_mut_obj_add_int(d, o, k, v);
}
static bool add_b(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, bool v)
{
    return yyjson_mut_obj_add_bool(d, o, k, v);
}

bool pc_engine_effect_defaults(const char *kind, yyjson_mut_doc *d, yyjson_mut_val **out)
{
    if (kind == NULL) {
        return false;
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (o == NULL) {
        return false;
    }
    bool ok = true;
    if (strcmp(kind, "dropShadow") == 0) {
        ok = add_s(d, o, "blend", "Multiply") && add_s(d, o, "color", "#000000") &&
             add_i(d, o, "opacity", 75) && add_i(d, o, "angle", 120) && add_b(d, o, "useGlobalLight", true) &&
             add_i(d, o, "distance", 5) && add_i(d, o, "spread", 0) && add_i(d, o, "size", 5) &&
             add_s(d, o, "contour", "Linear") && add_b(d, o, "antiAlias", false) && add_i(d, o, "noise", 0) &&
             add_b(d, o, "knocksOut", true);
    } else if (strcmp(kind, "innerShadow") == 0) {
        ok = add_s(d, o, "blend", "Multiply") && add_s(d, o, "color", "#000000") &&
             add_i(d, o, "opacity", 75) && add_i(d, o, "angle", 120) && add_b(d, o, "useGlobalLight", true) &&
             add_i(d, o, "distance", 5) && add_i(d, o, "choke", 0) && add_i(d, o, "size", 5) &&
             add_s(d, o, "contour", "Linear") && add_b(d, o, "antiAlias", false) && add_i(d, o, "noise", 0);
    } else if (strcmp(kind, "outerGlow") == 0) {
        ok = add_s(d, o, "blend", "Screen") && add_i(d, o, "opacity", 75) &&
             add_s(d, o, "color", "#ffffbe") && add_i(d, o, "spread", 0) && add_i(d, o, "size", 5) &&
             add_i(d, o, "range", 50) && add_s(d, o, "contour", "Linear") &&
             add_b(d, o, "antiAlias", false) && add_i(d, o, "noise", 0);
    } else if (strcmp(kind, "innerGlow") == 0) {
        ok = add_s(d, o, "blend", "Screen") && add_i(d, o, "opacity", 75) &&
             add_s(d, o, "color", "#ffffbe") && add_s(d, o, "source", "edge") && add_i(d, o, "choke", 0) &&
             add_i(d, o, "size", 5) && add_s(d, o, "contour", "Linear") && add_b(d, o, "antiAlias", false) &&
             add_i(d, o, "noise", 0);
    } else if (strcmp(kind, "stroke") == 0) {
        ok = add_i(d, o, "size", 3) && add_s(d, o, "position", "outside") && add_s(d, o, "blend", "Normal") &&
             add_i(d, o, "opacity", 100) && add_s(d, o, "color", "#000000");
    } else if (strcmp(kind, "colorOverlay") == 0) {
        ok = add_s(d, o, "blend", "Normal") && add_s(d, o, "color", "#ff0000") && add_i(d, o, "opacity", 100);
    } else if (strcmp(kind, "gradientOverlay") == 0) {
        ok = add_s(d, o, "blend", "Normal") && add_i(d, o, "opacity", 100) &&
             add_s(d, o, "from", "#000000") && add_s(d, o, "to", "#ffffff") &&
             add_b(d, o, "reverse", false) && add_s(d, o, "style", "linear") && add_i(d, o, "angle", 90) &&
             add_i(d, o, "scale", 100);
    } else if (strcmp(kind, "patternOverlay") == 0) {
        ok = add_s(d, o, "blend", "Normal") && add_i(d, o, "opacity", 100) && add_s(d, o, "pattern", "") &&
             add_i(d, o, "angle", 0) && add_i(d, o, "scale", 100) && add_b(d, o, "link", true);
    } else if (strcmp(kind, "bevelEmboss") == 0) {
        ok = add_s(d, o, "style", "inner") && add_i(d, o, "depth", 100) && add_s(d, o, "direction", "up") &&
             add_i(d, o, "size", 5) && add_i(d, o, "soften", 0) && add_i(d, o, "angle", 120) &&
             add_b(d, o, "useGlobalLight", true) && add_i(d, o, "altitude", 30) &&
             add_s(d, o, "glossContour", "Linear") && add_b(d, o, "contour", false) &&
             add_s(d, o, "contourName", "Linear") && add_i(d, o, "contourRange", 50) &&
             add_b(d, o, "contourAntiAlias", false);
    } else if (strcmp(kind, "satin") == 0) {
        ok = add_s(d, o, "blend", "Multiply") && add_s(d, o, "color", "#000000") &&
             add_i(d, o, "opacity", 50) && add_i(d, o, "angle", 19) && add_i(d, o, "distance", 11) &&
             add_i(d, o, "size", 14) && add_s(d, o, "contour", "Linear") && add_b(d, o, "antiAlias", true) &&
             add_b(d, o, "invert", true);
    } else {
        return false;
    }
    if (!ok) {
        return false;
    }
    *out = o;
    return true;
}

/* ============================================================== commands */

static pc_status has_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!d->has_active_layer || pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    return PC_OK;
}

static bool same_kind(const pc_effect *a, const pc_effect *b)
{
    return a->kind == b->kind;
}

/* set_effect state passed through session_edit. */
typedef struct set_fx_ctx {
    pc_layer_id id;
    pc_effect fx; /* already resolved + overlaid; converted in_mode inside the edit */
    bool add;
    bool has_pattern;
    pc_pattern pattern;
} set_fx_ctx;

static pc_status set_fx_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                             pc_error *err)
{
    (void)has_active;
    (void)active;
    set_fx_ctx *c = vctx;
    if (c->has_pattern) {
        pc_status status = pc_engine_pattern_ensure_in_doc(doc, &c->pattern, err);
        if (status != PC_OK)
            return status;
    }
    pc_effect fx;
    if (pc_effect_clone(&c->fx, &fx, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_effect_in_mode(&fx, doc->mode);
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        pc_effect_fini(&fx);
        return pc_engine_err_no_layer(err, c->id);
    }
    l->effects.enabled = true;
    pc_effect *slot = NULL;
    if (!c->add) {
        for (size_t i = 0; i < l->effects.nitems; i++) {
            if (same_kind(&l->effects.items[i], &fx)) {
                slot = &l->effects.items[i];
                break;
            }
        }
    }
    if (slot != NULL) {
        pc_effect_fini(slot);
        *slot = fx;
        return PC_OK;
    }
    pc_effect *grown = realloc(l->effects.items, (l->effects.nitems + 1) * sizeof *grown);
    if (grown == NULL) {
        pc_effect_fini(&fx);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    l->effects.items = grown;
    l->effects.items[l->effects.nitems++] = fx;
    return PC_OK;
}

/* Resolve the live pattern library before editing, then copy its resource in the same undo step.
 * An unchanged imported snapshot may retain a missing pattern for later relinking. */
static pc_status resolve_effect(const pc_engine_session *session, const char *cmd, pc_effect *fx,
                                bool preserve_missing, pc_pattern *pattern, bool *has_pattern, pc_error *err)
{
    *has_pattern = false;
    if (fx->kind != PC_FX_PATTERN_OVERLAY || preserve_missing)
        return PC_OK;
    const pc_str *key =
        fx->u.pattern_overlay.id.len ? &fx->u.pattern_overlay.id : &fx->u.pattern_overlay.name;
    bool found = false;
    pc_status status = pc_engine_pattern_resolve(session, pc_str_c(key), key->len, pattern, &found, err);
    if (status != PC_OK)
        return status;
    if (!found)
        return preserve_missing
                   ? PC_OK
                   : pc_engine_err_bad_params(err, cmd, "no matching pattern (see pattern.list)");
    pc_str name = PC_STR_INIT, id = PC_STR_INIT;
    status = pc_str_clone(&pattern->name, &name, err);
    if (status == PC_OK)
        status = pc_str_clone(&pattern->id, &id, err);
    if (status != PC_OK) {
        pc_str_fini(&name);
        pc_str_fini(&id);
        pc_pattern_fini(pattern);
        return status;
    }
    pc_str_fini(&fx->u.pattern_overlay.name);
    pc_str_fini(&fx->u.pattern_overlay.id);
    fx->u.pattern_overlay.name = name;
    fx->u.pattern_overlay.id = id;
    *has_pattern = true;
    return PC_OK;
}

static pc_status run_set_effect(pc_engine_session *s, const char *kind, yyjson_val *p, pc_json_value *out,
                                pc_error *err)
{
    set_fx_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    if (!pc_engine_effect_from_params(kind, p, &ctx.fx)) {
        return pc_fail(err, PC_ERR_INVALID, "unknown effect %s", kind);
    }
    pc_status rc = resolve_effect(s, "layer.layerStyle", &ctx.fx, false, &ctx.pattern, &ctx.has_pattern, err);
    if (rc != PC_OK) {
        pc_pattern_fini(&ctx.pattern);
        pc_effect_fini(&ctx.fx);
        return rc;
    }
    char label[64];
    (void)snprintf(label, sizeof label, "Layer Style: %s", pc_effect_label(&ctx.fx));
    ctx.add = jb(p, "add", false);
    rc = pc_engine_layer_param(s, p, &ctx.id, err);
    if (rc != PC_OK) {
        pc_pattern_fini(&ctx.pattern);
        pc_effect_fini(&ctx.fx);
        return rc;
    }
    pc_cmyk_space *space = pc_engine_doc_cmyk_space(s), *prior = space ? pc_cmyk_space_enter(space) : NULL;
    rc = pc_engine_session_edit(s, label, set_fx_edit, &ctx, err);
    if (space) {
        pc_cmyk_space_leave(prior);
        pc_cmyk_space_release(space);
    }
    pc_effect_fini(&ctx.fx);
    pc_pattern_fini(&ctx.pattern);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    (void)yyjson_mut_obj_add_uint(d, o, "layer", ctx.id);
    return PC_OK;
}

/* Each style command is a tiny wrapper naming its kind. */
#define STYLE_RUN(fn, kind)                                                                                  \
    static pc_status fn(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)              \
    {                                                                                                        \
        return run_set_effect(s, kind, p, out, err);                                                         \
    }
STYLE_RUN(run_drop_shadow, "dropShadow")
STYLE_RUN(run_inner_shadow, "innerShadow")
STYLE_RUN(run_outer_glow, "outerGlow")
STYLE_RUN(run_inner_glow, "innerGlow")
STYLE_RUN(run_stroke, "stroke")
STYLE_RUN(run_color_overlay, "colorOverlay")
STYLE_RUN(run_gradient_overlay, "gradientOverlay")
STYLE_RUN(run_pattern_overlay, "patternOverlay")
STYLE_RUN(run_satin, "satin")
STYLE_RUN(run_bevel, "bevelEmboss")

/* ---- replace ---- */
#define MAX_EFFECTS 256u

typedef struct replace_ctx {
    pc_layer_id id;
    pc_effect *list; /* resolved, in_mode applied inside the edit */
    size_t n;
    pc_pattern *patterns;
    size_t npatterns;
} replace_ctx;

static pc_status replace_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    replace_ctx *c = vctx;
    for (size_t i = 0; i < c->npatterns; i++) {
        pc_status status = pc_engine_pattern_ensure_in_doc(doc, &c->patterns[i], err);
        if (status != PC_OK)
            return status;
    }
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_effect *items = (c->n > 0) ? calloc(c->n, sizeof *items) : NULL;
    if (c->n > 0 && items == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < c->n; i++) {
        if (pc_effect_clone(&c->list[i], &items[i], err) != PC_OK) {
            for (size_t j = 0; j < i; j++) {
                pc_effect_fini(&items[j]);
            }
            free(items);
            return PC_ERR_NOMEM;
        }
        pc_effect_in_mode(&items[i], doc->mode);
    }
    for (size_t i = 0; i < l->effects.nitems; i++)
        pc_effect_fini(&l->effects.items[i]);
    free(l->effects.items);
    l->effects.enabled = true;
    l->effects.items = items;
    l->effects.nitems = c->n;
    return PC_OK;
}

static pc_status run_replace(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.layerStyle.replace";
    replace_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    pc_status rc = pc_engine_layer_param(s, p, &ctx.id, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_val *entries = obj_get(p, "effects");
    if (entries == NULL || !yyjson_is_arr(entries)) {
        return pc_engine_err_bad_params(err, CMD, "need `effects`: [{kind, params, fx?}]");
    }
    size_t count = yyjson_arr_size(entries);
    if (count > MAX_EFFECTS) {
        return pc_engine_err_bad_params(err, CMD, "at most %u effects", MAX_EFFECTS);
    }
    ctx.list = (count > 0) ? calloc(count, sizeof *ctx.list) : NULL;
    if (count > 0 && ctx.list == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    ctx.patterns = count ? calloc(count, sizeof *ctx.patterns) : NULL;
    if (count && !ctx.patterns) {
        free(ctx.list);
        return pc_fail(err, PC_ERR_NOMEM, "style patterns");
    }
    yyjson_val *e;
    yyjson_arr_iter it = yyjson_arr_iter_with(entries);
    while ((e = yyjson_arr_iter_next(&it)) != NULL) {
        const char *kind = yyjson_is_obj(e) ? jstr(e, "kind") : NULL;
        if (kind == NULL) {
            rc = pc_engine_err_bad_params(err, CMD, "each effect needs a `kind` string");
            goto fail;
        }
        yyjson_val *params = yyjson_obj_get(e, "params");
        if (params != NULL && !yyjson_is_null(params) && !yyjson_is_obj(params)) {
            rc = pc_engine_err_bad_params(err, CMD, "`params` must be an object");
            goto fail;
        }
        yyjson_val *fxj = yyjson_obj_get(e, "fx");
        pc_effect fx;
        if (fxj != NULL) {
            char serde[PC_ERROR_MSG_MAX];
            pc_error snap_err;
            if (pc_effect_from_json(fxj, &fx, &snap_err) != PC_OK) {
                (void)snprintf(serde, sizeof serde, "%s", snap_err.msg);
                rc = pc_engine_err_bad_params(err, CMD, "bad effect snapshot: %s", serde);
                goto fail;
            }
            if (strcmp(pc_engine_effect_kind_of(&fx), kind) != 0) {
                rc = pc_engine_err_bad_params(err, CMD, "effect snapshot is a %s, not `%s`",
                                              pc_engine_effect_kind_of(&fx), kind);
                pc_effect_fini(&fx);
                goto fail;
            }
            pc_engine_overlay_effect(&fx, params);
        } else if (!pc_engine_effect_from_params(kind, params, &fx)) {
            rc = pc_engine_err_bad_params(err, CMD, "unknown effect %s", kind);
            goto fail;
        }
        pc_pattern pattern = {0};
        bool has_pattern = false;
        bool preserve = fxj && !obj_get(params, "pattern");
        rc = resolve_effect(s, CMD, &fx, preserve, &pattern, &has_pattern, err);
        if (rc != PC_OK) {
            pc_effect_fini(&fx);
            goto fail;
        }
        if (has_pattern)
            ctx.patterns[ctx.npatterns++] = pattern;
        ctx.list[ctx.n++] = fx;
    }
    pc_cmyk_space *space = pc_engine_doc_cmyk_space(s), *prior = space ? pc_cmyk_space_enter(space) : NULL;
    rc = pc_engine_session_edit(s, ctx.n == 0 ? "Clear Layer Style" : "Edit Layer Style", replace_edit, &ctx,
                                err);
    if (space) {
        pc_cmyk_space_leave(prior);
        pc_cmyk_space_release(space);
    }
    if (rc != PC_OK) {
        goto fail;
    }
    for (size_t i = 0; i < ctx.n; i++) {
        pc_effect_fini(&ctx.list[i]);
    }
    for (size_t i = 0; i < ctx.npatterns; i++)
        pc_pattern_fini(&ctx.patterns[i]);
    free(ctx.patterns);
    free(ctx.list);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    (void)yyjson_mut_obj_add_uint(d, o, "layer", ctx.id);
    (void)yyjson_mut_obj_add_uint(d, o, "effects", ctx.n);
    return PC_OK;
fail:
    for (size_t i = 0; i < ctx.n; i++) {
        pc_effect_fini(&ctx.list[i]);
    }
    for (size_t i = 0; i < ctx.npatterns; i++)
        pc_pattern_fini(&ctx.patterns[i]);
    free(ctx.patterns);
    free(ctx.list);
    return rc;
}

/* ---- clear ---- */
typedef struct clear_ctx {
    pc_layer_id *ids;
    size_t n;
} clear_ctx;

static pc_status clear_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    clear_ctx *c = vctx;
    for (size_t i = 0; i < c->n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, c->ids[i]);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, c->ids[i]);
        }
        pc_effects_fini(&l->effects);
        memset(&l->effects, 0, sizeof l->effects);
        l->effects.enabled = true;
    }
    return PC_OK;
}

typedef struct delete_fx_ctx {
    pc_layer_id id;
    size_t index;
} delete_fx_ctx;

static pc_status delete_fx_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    delete_fx_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    if (c->index < l->effects.nitems) {
        pc_effect_fini(&l->effects.items[c->index]);
        memmove(&l->effects.items[c->index], &l->effects.items[c->index + 1],
                (l->effects.nitems - c->index - 1) * sizeof *l->effects.items);
        l->effects.nitems--;
    }
    return PC_OK;
}

static pc_status run_delete_effect(pc_engine_session *s, yyjson_val *p, yyjson_val *effect,
                                   pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.layerStyle.clear";
    if (!yyjson_is_int(effect) || yyjson_get_sint(effect) < 0) {
        return pc_engine_err_bad_params(err, CMD, "`effect` must be an effect index");
    }
    delete_fx_ctx ctx;
    ctx.index = (size_t)yyjson_get_sint(effect);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    yyjson_val *lv = obj_get(p, "layer");
    if (lv != NULL && !yyjson_is_null(lv)) {
        if (!yyjson_is_int(lv)) {
            return pc_engine_err_bad_params(err, CMD, "`layer` must be a layer id");
        }
        ctx.id = (pc_layer_id)yyjson_get_uint(lv);
    } else if (d->has_active_layer) {
        ctx.id = d->active_layer;
    } else {
        return pc_engine_err_bad_params(err, CMD, "no active layer");
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), ctx.id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, ctx.id);
    }
    size_t cnt = l->effects.nitems;
    if (ctx.index >= cnt) {
        return pc_engine_err_bad_params(err, CMD, "the layer has %zu effect(s), no effect %zu", cnt,
                                        ctx.index);
    }
    pc_status rc = pc_engine_session_edit(s, "Delete Effect", delete_fx_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* style_targets inlined: the `layer` param, else the selected layers; keep only non-background
 * layers with effects. */
static pc_status run_clear(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_val *effect = obj_get(p, "effect");
    if (effect != NULL && !yyjson_is_null(effect)) {
        return run_delete_effect(s, p, effect, out, err);
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    clear_ctx ctx;
    ctx.ids = NULL;
    ctx.n = 0;
    bool present = false;
    pc_layer_id one;
    pc_status rc = pc_engine_explicit_layer(s, p, &present, &one, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id *targets = NULL;
    size_t ntargets = 0;
    if (present) {
        targets = malloc(sizeof *targets);
        if (targets == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        targets[0] = one;
        ntargets = 1;
    } else {
        if (pc_engine_selected(s, &targets, &ntargets, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    /* style_targets: with 2+ targets, filter to non-background layers that have a style. */
    if (ntargets >= 2) {
        pc_layer_id *filtered = malloc(ntargets * sizeof *filtered);
        if (filtered == NULL) {
            free(targets);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        size_t m = 0;
        for (size_t i = 0; i < ntargets; i++) {
            const pc_layer *l = pc_document_layer(doc, targets[i]);
            if (l != NULL && !pc_document_is_background(doc, l->id) && l->effects.nitems > 0) {
                filtered[m++] = targets[i];
            }
        }
        free(targets);
        targets = filtered;
        ntargets = m;
        if (ntargets == 0) {
            free(targets);
            return pc_fail(err, PC_ERR_INVALID,
                           "none of the selected layers can take this layer style change");
        }
    }
    ctx.ids = targets;
    ctx.n = ntargets;
    rc = pc_engine_session_edit(s, "Clear Layer Style", clear_edit, &ctx, err);
    free(targets);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ============================================================== registry */

static const char *const MENU_STYLE[] = {"Layer", "Layer Style"};

const pc_engine_command *pc_engine_layer_style_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"layer.layerStyle.dropShadow", "Drop Shadow\xE2\x80\xA6", MENU_STYLE, 2, NULL,
         "{\"color\":\"#rrggbb\",\"opacity\":0..100=75,\"blend\":str=\"multiply\",\"angle\":deg=120,"
         "\"useGlobalLight\":bool,\"distance\":px=5,\"spread\":0..100,\"size\":px=5,\"contour\":name|curve="
         "\"Linear\","
         "\"antiAlias\":bool,\"noise\":0..100,\"knocksOut\":bool,\"add\":bool,\"layer\":id}",
         has_layer, run_drop_shadow, true},
        {"layer.layerStyle.innerShadow", "Inner Shadow\xE2\x80\xA6", MENU_STYLE, 2, NULL,
         "{\"color\":\"#rrggbb\",\"opacity\":0..100=75,\"blend\":str,\"angle\":deg,\"distance\":px,\"choke\":"
         "0..100,"
         "\"size\":px,\"contour\":name|curve,\"antiAlias\":bool,\"noise\":0..100,\"add\":bool}",
         has_layer, run_inner_shadow, true},
        {"layer.layerStyle.outerGlow", "Outer Glow\xE2\x80\xA6", MENU_STYLE, 2, NULL,
         "{\"color\":\"#rrggbb\",\"opacity\":0..100=75,\"blend\":str=\"screen\",\"technique\":\"softer|"
         "precise\","
         "\"spread\":0..100,\"size\":px,\"range\":0..100,\"contour\":name|curve,\"antiAlias\":bool,\"noise\":"
         "0..100,"
         "\"add\":bool}",
         has_layer, run_outer_glow, true},
        {"layer.layerStyle.innerGlow", "Inner Glow\xE2\x80\xA6", MENU_STYLE, 2, NULL,
         "{\"color\":\"#rrggbb\",\"opacity\":0..100=75,\"blend\":str=\"screen\",\"technique\":\"softer|"
         "precise\","
         "\"source\":\"edge|center\",\"choke\":0..100,\"size\":px,\"contour\":name|curve,\"antiAlias\":bool,"
         "\"noise\":0..100,\"add\":bool}",
         has_layer, run_inner_glow, true},
        {"layer.layerStyle.stroke", "Stroke\xE2\x80\xA6", MENU_STYLE, 2, NULL,
         "{\"size\":px=3,\"position\":\"outside|inside|center\",\"color\":\"#rrggbb\",\"from\":\"#rrggbb\","
         "\"to\":\"#rrggbb\",\"method\":\"classic|perceptual|linear|smooth\"?,\"smoothness\":0..100=100,"
         "\"style\":str,\"angle\":deg,\"opacity\":0..100,\"blend\":str,\"add\":bool}",
         has_layer, run_stroke, true},
        {"layer.layerStyle.colorOverlay", "Color Overlay\xE2\x80\xA6", MENU_STYLE, 2, NULL,
         "{\"color\":\"#rrggbb\",\"opacity\":0..100=100,\"blend\":str,\"add\":bool}", has_layer,
         run_color_overlay, true},
        {"layer.layerStyle.gradientOverlay", "Gradient Overlay\xE2\x80\xA6", MENU_STYLE, 2, NULL,
         "{\"from\":\"#rrggbb\",\"to\":\"#rrggbb\",\"method\":\"classic|perceptual|linear|smooth\"?,"
         "\"smoothness\":0..100=100,\"style\":\"linear|radial|angle|reflected|diamond\",\"angle\":deg=90,"
         "\"scale\":10..150=100,\"reverse\":bool,\"opacity\":0..100,\"blend\":str,\"add\":bool}",
         has_layer, run_gradient_overlay, true},
        {"layer.layerStyle.patternOverlay", "Pattern Overlay…", MENU_STYLE, 2, NULL,
         "{\"pattern\":text,\"scale\":1..1000=100,\"angle\":-180..180=0,\"link\":bool=true,\"phaseX\":px=0,"
         "\"phaseY\":px=0,\"opacity\":0..100=100,\"blend\":str,\"add\":bool}",
         has_layer, run_pattern_overlay, true},
        {"layer.layerStyle.bevelEmboss", "Bevel & Emboss\xE2\x80\xA6", MENU_STYLE, 2, NULL,
         "{\"style\":\"inner|outer|emboss|pillow|stroke\",\"technique\":\"smooth|chiselHard|chiselSoft\","
         "\"contour\":bool,\"contourName\":name|curve,\"contourRange\":1..100=50,\"contourAntiAlias\":bool,"
         "\"glossContour\":name|curve,\"texture\":pattern id|name?,\"textureScale\":1..1000=100,"
         "\"textureDepth\":-1000..1000=100,\"textureInvert\":bool,\"textureLink\":bool=true,\"depth\":1.."
         "1000=100,"
         "\"direction\":\"up|down\",\"size\":px=5,\"soften\":px,\"angle\":deg,\"altitude\":deg,\"add\":bool}",
         has_layer, run_bevel, true},
        {"layer.layerStyle.satin", "Satin\xE2\x80\xA6", MENU_STYLE, 2, NULL,
         "{\"color\":\"#rrggbb\",\"opacity\":0..100=50,\"blend\":str,\"angle\":deg,\"distance\":px,\"size\":"
         "px,"
         "\"contour\":name|curve,\"antiAlias\":bool=true,\"invert\":bool,\"add\":bool}",
         has_layer, run_satin, true},
        {"layer.layerStyle.replace", "Edit Layer Style", NULL, 0, NULL,
         "{\"layer\":id,\"effects\":[{\"kind\":\"dropShadow|innerShadow|outerGlow|innerGlow|stroke|"
         "colorOverlay|"
         "gradientOverlay|patternOverlay|satin|bevelEmboss\",\"params\":{\xE2\x80\xA6},\"fx\":<effect "
         "snapshot>}]}",
         has_layer, run_replace, true},
        {"layer.layerStyle.clear", "Clear Layer Style", MENU_STYLE, 2, NULL,
         "{\"layer\":id? (no layer: every selected layer that has a style),\"effect\":index? (only that "
         "effect)}",
         has_layer, run_clear, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
