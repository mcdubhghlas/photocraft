/* Paint Bucket (paint.bucket) and the Gradient tool (paint.gradient) (port of
 * crates/engine/src/paint_cmds.rs, E10).
 *
 * The Paint Bucket floods the region similar to the seed pixel (or a whole-image wand region when
 * Sample All Layers is on) with the foreground colour or a pattern. The Gradient tool paints a
 * gradient from a preset, explicit stops, the Gradients-panel selection, or a legacy evenly-spaced
 * colour list, optionally through Photoshop's interpolation (method + smoothness) baked into dense
 * stops. Both honour the selection, the Channels-panel target, opacity and blend mode, and work at
 * every bit depth and colour model. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo_paint.h"
#include "algo/selection.h"
#include "base/json.h"
#include "base/mem.h"
#include "color/blend.h"
#include "color/color.h"
#include "compose/compose.h"
#include "compose/pattern.h"
#include "doc/doc.h"
#include "doc/pattern.h"
#include "geom/geom.h"
#include "io/gradient_bake.h"
#include "presets.h"

#include "brush_cmds.h"
#include "channel_cmds.h"
#include "engine_internal.h"
#include "pattern_cmds.h"

/* ========================================================================================== */
/* param helpers                                                                               */
/* ========================================================================================== */

static float fnum(yyjson_val *p, const char *k, float d)
{
    double v;
    yyjson_val *x = pc_json_get(p, k);
    return (x != NULL && pc_json_as_f64(x, &v)) ? (float)v : d;
}
static bool bflag(yyjson_val *p, const char *k, bool d)
{
    bool v;
    yyjson_val *x = pc_json_get(p, k);
    return (x != NULL && pc_json_as_bool(x, &v)) ? v : d;
}

/* eraser_cmds::number: absent/null -> default; a finite number clamped to [lo,hi]; anything else
 * is BadParams. */
static pc_status number(yyjson_val *p, const char *cmd, const char *k, double def, double lo, double hi,
                        double *out, pc_error *err)
{
    yyjson_val *x = pc_json_get(p, k);
    if (x == NULL || yyjson_is_null(x)) {
        *out = def;
        return PC_OK;
    }
    double v;
    if (!pc_json_as_f64(x, &v) || !isfinite(v)) {
        return pc_engine_err_bad_params(err, cmd, "`%s` must be a number", k);
    }
    *out = v < lo ? lo : (v > hi ? hi : v);
    return PC_OK;
}

static pc_status opacity_param(yyjson_val *p, const char *cmd, float *out, pc_error *err)
{
    double v = 100.0;
    pc_status rc = number(p, cmd, "opacity", 100.0, 1.0, 100.0, &v, err);
    if (rc != PC_OK) {
        return rc;
    }
    *out = (float)(v / 100.0);
    return PC_OK;
}

/* "color": [r,g,b,a?] or "#rrggbb[aa]"; `def` otherwise. */
static void color_param(yyjson_val *v, const float def[4], float out[4])
{
    if (v != NULL && yyjson_is_arr(v) && yyjson_arr_size(v) >= 3) {
        double c[4] = {0, 0, 0, 1.0};
        for (size_t i = 0; i < 4; i++) {
            double d;
            yyjson_val *e = yyjson_arr_get(v, i);
            if (e != NULL && pc_json_as_f64(e, &d)) {
                c[i] = d;
            }
        }
        out[0] = (float)c[0];
        out[1] = (float)c[1];
        out[2] = (float)c[2];
        out[3] = (float)c[3];
        return;
    }
    size_t len = 0;
    const char *h = v != NULL ? pc_json_as_str(v, &len) : NULL;
    if (h != NULL) {
        if (len > 0 && h[0] == '#') {
            h++;
            len--;
        }
        if (len == 6 || len == 8) {
            char buf[10];
            buf[0] = '#';
            memcpy(buf + 1, h, len);
            buf[len + 1] = '\0';
            if (pc_engine_parse_hex(buf, len + 1, out)) {
                return;
            }
        }
    }
    memcpy(out, def, 4 * sizeof *out);
}

static bool point_param(yyjson_val *p, const char *k, float out[2])
{
    yyjson_val *a = pc_json_get(p, k);
    if (a == NULL || !yyjson_is_arr(a) || yyjson_arr_size(a) < 2) {
        return false;
    }
    double x, y;
    if (!pc_json_as_f64(yyjson_arr_get(a, 0), &x) || !pc_json_as_f64(yyjson_arr_get(a, 1), &y)) {
        return false;
    }
    out[0] = (float)x;
    out[1] = (float)y;
    return true;
}

/* ========================================================================================== */
/* Paint Bucket                                                                                */
/* ========================================================================================== */

struct const_src {
    float color[4];
};
static void const_src_fn(void *ctx, int32_t x, int32_t y, float out[4])
{
    (void)x;
    (void)y;
    memcpy(out, ((struct const_src *)ctx)->color, 4 * sizeof(float));
}

struct pat_src {
    const pc_compose_buffer *buf;
    pc_rect area;
    int32_t w;
};
static void pat_src_fn(void *ctx, int32_t x, int32_t y, float out[4])
{
    struct pat_src *c = ctx;
    int64_t ix = (int64_t)(x - c->area.x0);
    int64_t iy = (int64_t)(y - c->area.y0);
    int64_t idx = iy * (int64_t)c->w + ix;
    if (idx < 0 || (size_t)idx >= c->buf->len) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    memcpy(out, c->buf->px[idx], 4 * sizeof(float));
}

struct bucket_ctx {
    float color[4];
    int32_t sx, sy;
    float tol, opacity;
    bool contiguous, anti_alias;
    bool all;
    const pc_algo_region *region; /* NULL when none */
    pc_compose_pattern_tile *tile; /* NULL when no pattern */
    float scale, angle, phase[2];
    yyjson_val *p;
    bool filled;
};

static pc_status bucket_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    struct bucket_ctx *c = vctx;
    pc_rect area = pc_document_bounds(doc);
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_surface *surf = NULL;
    bool lock = false;
    pc_status rc = pc_engine_target_surface(doc, *ha, *active, c->p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    c->filled = false;
    if (c->all && c->region == NULL) {
        return PC_OK; /* nothing to fill (off-canvas sample) */
    }
    if (c->tile != NULL) {
        pc_compose_placement place = pc_compose_placement_new(PC_RECT_EMPTY, false, c->phase, c->scale, c->angle);
        pc_compose_buffer buf = {0};
        rc = pc_compose_pattern_render(c->tile, &place, area, &buf, err);
        if (rc != PC_OK) {
            return rc;
        }
        struct pat_src ps = {&buf, area, (int32_t)pc_rect_width(area)};
        if (c->region != NULL) {
            rc = pc_algo_paint_bucket_fill_region_src(surf, c->region, c->opacity, sel, lock, pat_src_fn,
                                                      &ps, err);
            c->filled = (rc == PC_OK);
        } else {
            rc = pc_algo_paint_bucket_fill_src(surf, area, c->sx, c->sy, c->tol, c->contiguous,
                                               c->anti_alias, c->opacity, sel, lock, pat_src_fn, &ps,
                                               &c->filled, err);
        }
        pc_compose_buffer_fini(&buf);
    } else if (c->region != NULL) {
        struct const_src cs;
        memcpy(cs.color, c->color, sizeof cs.color);
        rc = pc_algo_paint_bucket_fill_region_src(surf, c->region, c->opacity, sel, lock, const_src_fn,
                                                  &cs, err);
        c->filled = (rc == PC_OK);
    } else {
        rc = pc_algo_paint_bucket_fill(surf, area, c->sx, c->sy, c->tol, c->contiguous, c->anti_alias,
                                       c->color, c->opacity, sel, lock, &c->filled, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_prune(surf);
    return PC_OK;
}

static pc_status run_bucket(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "paint.bucket";
    float fg[4];
    memcpy(fg, s->tools.foreground, sizeof fg);
    float color[4];
    color_param(pc_json_get(p, "color"), fg, color);
    int32_t sx = (int32_t)floor(fnum(p, "x", 0.0f));
    int32_t sy = (int32_t)floor(fnum(p, "y", 0.0f));
    float tol = fnum(p, "tolerance", 32.0f);
    bool contiguous = bflag(p, "contiguous", true);
    bool aa = bflag(p, "antiAlias", true);
    float opacity;
    pc_status rc = opacity_param(p, cmd, &opacity, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool all = bflag(p, "sampleAllLayers", false) && pc_engine_target_of(p, NULL) == PC_ENGINE_PIXTGT_PIXELS;

    /* Fill source: foreground colour, or a pattern (the "Fill" dropdown). */
    size_t srclen = 0;
    yyjson_val *sv = pc_json_get(p, "contents");
    const char *source = sv != NULL ? pc_json_as_str(sv, &srclen) : NULL;
    if (source == NULL) {
        yyjson_val *sv2 = pc_json_get(p, "source");
        source = sv2 != NULL ? pc_json_as_str(sv2, &srclen) : NULL;
    }
    bool is_pattern = source != NULL && srclen == 7 && memcmp(source, "pattern", 7) == 0;

    /* Wand region from the visible composite (Sample All Layers). */
    pc_algo_region region = {0};
    bool have_region = false;
    if (all) {
        pc_rect sarea;
        uint8_t (*img)[4] = NULL;
        size_t ilen = 0;
        rc = pc_engine_sample_rgba8(s, true, &sarea, &img, &ilen, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = pc_algo_selection_wand_region((const uint8_t (*)[4])img, ilen, sarea, sx, sy, tol, contiguous, aa, &region, err);
        free(img);
        if (rc != PC_OK) {
            pc_algo_region_fini(&region);
            return rc;
        }
        have_region = !pc_algo_region_is_none(&region);
        if (!have_region) {
            pc_algo_region_fini(&region);
        }
    }

    pc_compose_pattern_tile *tile = NULL;
    float scale = 1.0f, angle = 0.0f, phase[2] = {0, 0};
    pc_pattern pat = {0};
    bool have_pat = false;
    if (is_pattern) {
        rc = pc_engine_pattern_resolve_param(s, cmd, p, &pat, err);
        if (rc != PC_OK) {
            if (have_region) {
                pc_algo_region_fini(&region);
            }
            return rc;
        }
        have_pat = true;
        rc = pc_compose_pattern_tile_new(&pat, &tile, err);
        if (rc != PC_OK || tile == NULL) {
            pc_pattern_fini(&pat);
            if (have_region) {
                pc_algo_region_fini(&region);
            }
            if (rc == PC_OK) {
                return pc_engine_err_bad_params(err, cmd, "the pattern is empty or too large to paint");
            }
            return rc;
        }
        bool link;
        pc_engine_pattern_placement(p, &scale, &angle, &link, phase);
    }

    struct bucket_ctx ctx = {0};
    memcpy(ctx.color, color, sizeof color);
    ctx.sx = sx;
    ctx.sy = sy;
    ctx.tol = tol;
    ctx.opacity = opacity;
    ctx.contiguous = contiguous;
    ctx.anti_alias = aa;
    ctx.all = all;
    ctx.region = have_region ? &region : NULL;
    ctx.tile = tile;
    ctx.scale = scale;
    ctx.angle = angle;
    ctx.phase[0] = phase[0];
    ctx.phase[1] = phase[1];
    ctx.p = p;
    ctx.filled = false;

    rc = pc_engine_session_edit(s, "Paint Bucket", bucket_edit, &ctx, err);
    pc_compose_pattern_tile_release(tile);
    if (have_pat) {
        pc_pattern_fini(&pat);
    }
    if (have_region) {
        pc_algo_region_fini(&region);
    }
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, root, "filled", ctx.filled);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ========================================================================================== */
/* Gradient tool                                                                               */
/* ========================================================================================== */

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Piecewise-linear RGBA of sorted (loc,rgba) stops at t. */
static void sample_cstops(const float *locs, const float (*cols)[4], size_t n, float t, float out[4])
{
    if (n == 0) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    if (t <= locs[0]) {
        memcpy(out, cols[0], 4 * sizeof(float));
        return;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (t <= locs[i + 1]) {
            float u = locs[i + 1] > locs[i] ? (t - locs[i]) / (locs[i + 1] - locs[i]) : 1.0f;
            for (int k = 0; k < 4; k++) {
                out[k] = cols[i][k] + (cols[i + 1][k] - cols[i][k]) * u;
            }
            return;
        }
    }
    memcpy(out, cols[n - 1], 4 * sizeof(float));
}

/* Piecewise-linear opacity of sorted (loc,opacity) stops at t (1.0 when none). */
static float sample_ostops(const pc_gradient_ostop *os, size_t n, float t)
{
    if (n == 0) {
        return 1.0f;
    }
    if (t <= os[0].loc) {
        return os[0].opacity;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (t <= os[i + 1].loc) {
            float u = os[i + 1].loc > os[i].loc ? (t - os[i].loc) / (os[i + 1].loc - os[i].loc) : 1.0f;
            return os[i].opacity + (os[i + 1].opacity - os[i].opacity) * u;
        }
    }
    return os[n - 1].opacity;
}

/* gradients::apply_opacity: colour stops with transparency stops multiplied into alpha, with extra
 * samples where both vary. *out owned (free()). */
static pc_status apply_opacity(const float *clocs, const float (*ccols)[4], size_t cn,
                               const pc_gradient_ostop *os_in, size_t non, pc_algo_gradient_stop **out,
                               size_t *nout, pc_error *err)
{
    /* os sorted + clamped. */
    pc_gradient_ostop *os = non > 0 ? pc_malloc_array(non, sizeof *os) : NULL;
    if (non > 0 && os == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    for (size_t i = 0; i < non; i++) {
        os[i].loc = clamp01(os_in[i].loc);
        os[i].opacity = clamp01(os_in[i].opacity);
    }
    for (size_t i = 1; i < non; i++) { /* insertion sort by loc */
        pc_gradient_ostop k = os[i];
        size_t j = i;
        while (j > 0 && os[j - 1].loc > k.loc) {
            os[j] = os[j - 1];
            j--;
        }
        os[j] = k;
    }
    bool all_one = true;
    for (size_t i = 0; i < non; i++) {
        if (fabsf(os[i].opacity - 1.0f) >= 1e-6f) {
            all_one = false;
            break;
        }
    }
    if (non == 0 || all_one) {
        /* return cs unchanged */
        pc_algo_gradient_stop *r = pc_malloc_array(cn > 0 ? cn : 1, sizeof *r);
        if (r == NULL) {
            free(os);
            return pc_fail(err, PC_ERR_NOMEM, "gradient");
        }
        for (size_t i = 0; i < cn; i++) {
            r[i].pos = clocs[i];
            memcpy(r[i].color, ccols[i], 4 * sizeof(float));
        }
        free(os);
        *out = r;
        *nout = cn;
        return PC_OK;
    }
    /* ts = color locs + opacity locs + {0,1}; sort; dedup within 1e-6. */
    size_t ncap = cn + non + 2;
    float *ts = pc_malloc_array(ncap, sizeof *ts);
    if (ts == NULL) {
        free(os);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    size_t tn = 0;
    for (size_t i = 0; i < cn; i++) {
        ts[tn++] = clocs[i];
    }
    for (size_t i = 0; i < non; i++) {
        ts[tn++] = os[i].loc;
    }
    ts[tn++] = 0.0f;
    ts[tn++] = 1.0f;
    for (size_t i = 1; i < tn; i++) { /* insertion sort */
        float k = ts[i];
        size_t j = i;
        while (j > 0 && ts[j - 1] > k) {
            ts[j] = ts[j - 1];
            j--;
        }
        ts[j] = k;
    }
    size_t dn = 0;
    for (size_t i = 0; i < tn; i++) {
        if (dn == 0 || fabsf(ts[i] - ts[dn - 1]) >= 1e-6f) {
            ts[dn++] = ts[i];
        }
    }
    /* all = per window, 4 subdivisions; then 1.0. */
    size_t allcap = (dn > 0 ? (dn - 1) : 0) * 4 + 1;
    float *allt = pc_malloc_array(allcap > 0 ? allcap : 1, sizeof *allt);
    if (allt == NULL) {
        free(os);
        free(ts);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    size_t an = 0;
    for (size_t i = 0; i + 1 < dn; i++) {
        for (int k = 0; k < 4; k++) {
            allt[an++] = ts[i] + (ts[i + 1] - ts[i]) * (float)k / 4.0f;
        }
    }
    allt[an++] = 1.0f;
    pc_algo_gradient_stop *r = pc_malloc_array(an > 0 ? an : 1, sizeof *r);
    if (r == NULL) {
        free(os);
        free(ts);
        free(allt);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    for (size_t i = 0; i < an; i++) {
        float t = allt[i];
        float c[4];
        sample_cstops(clocs, ccols, cn, t, c);
        c[3] *= sample_ostops(os, non, t);
        r[i].pos = t;
        memcpy(r[i].color, c, 4 * sizeof(float));
    }
    free(os);
    free(ts);
    free(allt);
    *out = r;
    *nout = an;
    return PC_OK;
}

/* Resolve a preset's colour stops to sorted (loc,rgba) with fg/bg filled in (GradientPreset's
 * colour ramp, before opacity). *out_locs / *out_cols owned (free()). */
static pc_status resolve_ramp(const pc_gradient_preset *g, const float fg[4], const float bg[4],
                              float **out_locs, float (**out_cols)[4], size_t *out_n, pc_error *err)
{
    size_t nc = g->nstops;
    size_t cap = nc < 2 ? 2 : nc;
    float *locs = pc_malloc_array(cap, sizeof *locs);
    float (*cols)[4] = pc_malloc_array(cap, sizeof *cols);
    if (locs == NULL || cols == NULL) {
        free(locs);
        free(cols);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    size_t cn = 0;
    for (size_t i = 0; i < nc; i++) {
        locs[cn] = clamp01(g->stops[i].loc);
        switch (g->stops[i].color.kind) {
        case PC_STOP_FOREGROUND:
            cols[cn][0] = fg[0];
            cols[cn][1] = fg[1];
            cols[cn][2] = fg[2];
            cols[cn][3] = 1.0f;
            break;
        case PC_STOP_BACKGROUND:
            cols[cn][0] = bg[0];
            cols[cn][1] = bg[1];
            cols[cn][2] = bg[2];
            cols[cn][3] = 1.0f;
            break;
        default:
            cols[cn][0] = g->stops[i].color.rgb[0];
            cols[cn][1] = g->stops[i].color.rgb[1];
            cols[cn][2] = g->stops[i].color.rgb[2];
            cols[cn][3] = 1.0f;
            break;
        }
        cn++;
    }
    for (size_t i = 1; i < cn; i++) { /* stable insertion sort by loc */
        float kl = locs[i];
        float kc[4];
        memcpy(kc, cols[i], sizeof kc);
        size_t j = i;
        while (j > 0 && locs[j - 1] > kl) {
            locs[j] = locs[j - 1];
            memcpy(cols[j], cols[j - 1], sizeof kc);
            j--;
        }
        locs[j] = kl;
        memcpy(cols[j], kc, sizeof kc);
    }
    if (cn == 0) {
        locs[0] = 0.0f;
        cols[0][0] = fg[0];
        cols[0][1] = fg[1];
        cols[0][2] = fg[2];
        cols[0][3] = 1.0f;
        locs[1] = 1.0f;
        cols[1][0] = bg[0];
        cols[1][1] = bg[1];
        cols[1][2] = bg[2];
        cols[1][3] = 1.0f;
        cn = 2;
    }
    *out_locs = locs;
    *out_cols = cols;
    *out_n = cn;
    return PC_OK;
}

/* Resolve a preset (colours + opacity) to final RGBA stops for the algo. */
static pc_status preset_to_stops(const pc_gradient_preset *g, const float fg[4], const float bg[4],
                                 pc_algo_gradient_stop **out, size_t *nout, pc_error *err)
{
    float *locs = NULL;
    float (*cols)[4] = NULL;
    size_t cn = 0;
    pc_status rc = resolve_ramp(g, fg, bg, &locs, &cols, &cn, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = apply_opacity(locs, (const float (*)[4])cols, cn, g->opacity, g->nopacity, out, nout, err);
    free(locs);
    free(cols);
    return rc;
}

/* The gradient tool's stops: a preset ("gradient"), explicit "stops", or the current gradient;
 * explicit "transparency" replaces its own. *have=false (caller uses the legacy "colors" list)
 * when "colors" is a non-empty array. */
static pc_status tool_stops(pc_engine_session *s, yyjson_val *p, pc_algo_gradient_stop **out,
                            size_t *nout, bool *have, pc_error *err)
{
    const char *cmd = "paint.gradient";
    *have = false;
    yyjson_val *colors = pc_json_get(p, "colors");
    if (colors != NULL && yyjson_is_arr(colors) && yyjson_arr_size(colors) > 0) {
        return PC_OK; /* legacy path */
    }
    float fg[4], bg[4];
    memcpy(fg, s->tools.foreground, sizeof fg);
    memcpy(bg, s->tools.background, sizeof bg);
    pc_gradient_preset g = {0};
    bool owned = false;
    size_t glen = 0;
    const char *name = NULL;
    yyjson_val *gv = pc_json_get(p, "gradient");
    if (gv != NULL) {
        name = pc_json_as_str(gv, &glen);
    }
    if (name != NULL) {
        size_t gi, ii;
        if (!pc_preset_find(&s->presets.gradients, name, NULL, pc_gradient_preset_ops(), &gi, &ii)) {
            return pc_engine_err_bad_params(err, cmd, "no gradient preset \"%.*s\"", (int)glen, name);
        }
        const pc_gradient_preset *src = &((const pc_gradient_preset *)s->presets.gradients.v[gi].items)[ii];
        if (pc_gradient_preset_clone(src, &g, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        owned = true;
        if (pc_gradient_with_transparency(&g, p, cmd, err) != PC_OK) {
            pc_gradient_preset_fini(&g);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    } else if (pc_json_get(p, "stops") != NULL) {
        if (pc_gradient_from_params("Custom", p, cmd, &g, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        owned = true;
    } else {
        if (pc_gradient_preset_clone(&s->presets.gradient, &g, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        owned = true;
        if (pc_gradient_with_transparency(&g, p, cmd, err) != PC_OK) {
            pc_gradient_preset_fini(&g);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    }
    pc_status rc = preset_to_stops(&g, fg, bg, out, nout, err);
    if (owned) {
        pc_gradient_preset_fini(&g);
    }
    if (rc == PC_OK) {
        *have = true;
    }
    return rc;
}

/* Legacy "colors" list -> evenly-spaced RGBA stops, with explicit "transparency" applied. */
static pc_status legacy_stops(pc_engine_session *s, yyjson_val *p, pc_algo_gradient_stop **out,
                              size_t *nout, pc_error *err)
{
    const char *cmd = "paint.gradient";
    float fg[4], bg[4];
    memcpy(fg, s->tools.foreground, sizeof fg);
    memcpy(bg, s->tools.background, sizeof bg);
    yyjson_val *colors = pc_json_get(p, "colors");
    size_t n = (colors != NULL && yyjson_is_arr(colors)) ? yyjson_arr_size(colors) : 0;
    float *locs = NULL;
    float (*cols)[4] = NULL;
    size_t cn;
    if (n > 0) {
        locs = pc_malloc_array(n, sizeof *locs);
        cols = pc_malloc_array(n, sizeof *cols);
        if (locs == NULL || cols == NULL) {
            free(locs);
            free(cols);
            return pc_fail(err, PC_ERR_NOMEM, "gradient");
        }
        for (size_t i = 0; i < n; i++) {
            color_param(yyjson_arr_get(colors, i), fg, cols[i]);
            locs[i] = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
        }
        cn = n;
    } else {
        cn = 2;
        locs = pc_malloc_array(2, sizeof *locs);
        cols = pc_malloc_array(2, sizeof *cols);
        if (locs == NULL || cols == NULL) {
            free(locs);
            free(cols);
            return pc_fail(err, PC_ERR_NOMEM, "gradient");
        }
        locs[0] = 0.0f;
        memcpy(cols[0], fg, sizeof fg);
        locs[1] = 1.0f;
        memcpy(cols[1], bg, sizeof bg);
    }
    /* transparency (default empty). */
    pc_gradient_preset tmp = {0};
    if (pc_gradient_with_transparency(&tmp, p, cmd, err) != PC_OK) {
        free(locs);
        free(cols);
        pc_gradient_preset_fini(&tmp);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_status rc = apply_opacity(locs, (const float (*)[4])cols, cn, tmp.opacity, tmp.nopacity, out, nout, err);
    pc_gradient_preset_fini(&tmp);
    free(locs);
    free(cols);
    return rc;
}

static bool method_from_name(const char *s, size_t len, pc_io_bake_method *out)
{
    char buf[16];
    if (len >= sizeof buf) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    buf[len] = '\0';
    if (strcmp(buf, "classic") == 0) {
        *out = PC_IO_BAKE_CLASSIC;
    } else if (strcmp(buf, "perceptual") == 0) {
        *out = PC_IO_BAKE_PERCEPTUAL;
    } else if (strcmp(buf, "linear") == 0) {
        *out = PC_IO_BAKE_LINEAR;
    } else if (strcmp(buf, "smooth") == 0) {
        *out = PC_IO_BAKE_SMOOTH;
    } else {
        return false;
    }
    return true;
}

/* Apply Photoshop's interpolation (method + smoothness) to the stops, baking dense stops. */
static pc_status bake_method(yyjson_val *p, pc_algo_gradient_stop *stops, size_t n,
                             pc_algo_gradient_stop **out, size_t *nout, pc_error *err)
{
    size_t mlen = 0;
    yyjson_val *mv = pc_json_get(p, "method");
    const char *mname = mv != NULL ? pc_json_as_str(mv, &mlen) : NULL;
    pc_io_bake_method method;
    if (mname == NULL || !method_from_name(mname, mlen, &method) || n < 2) {
        *out = NULL;
        *nout = 0;
        return PC_OK; /* keep straight stops */
    }
    pc_io_color_stop *in = pc_malloc_array(n, sizeof *in);
    float *mids = n > 1 ? pc_malloc_array(n - 1, sizeof *mids) : NULL;
    if (in == NULL || (n > 1 && mids == NULL)) {
        free(in);
        free(mids);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    for (size_t i = 0; i < n; i++) {
        in[i].at = stops[i].pos;
        in[i].color = pc_color_rgba(stops[i].color[0], stops[i].color[1], stops[i].color[2],
                                    stops[i].color[3]);
    }
    for (size_t i = 0; i + 1 < n; i++) {
        mids[i] = 0.5f;
    }
    float smoothness = clamp01(fnum(p, "smoothness", 100.0f) / 100.0f);
    pc_io_color_stop *baked = NULL;
    size_t nbaked = 0;
    pc_status rc = pc_io_gradient_bake(in, n, mids, n - 1, smoothness, method, &baked, &nbaked, err);
    free(in);
    free(mids);
    if (rc != PC_OK) {
        return rc;
    }
    pc_algo_gradient_stop *r = pc_malloc_array(nbaked > 0 ? nbaked : 1, sizeof *r);
    if (r == NULL) {
        free(baked);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    for (size_t i = 0; i < nbaked; i++) {
        float rgb[3];
        pc_color_to_rgb(&baked[i].color, rgb);
        r[i].pos = baked[i].at;
        r[i].color[0] = rgb[0];
        r[i].color[1] = rgb[1];
        r[i].color[2] = rgb[2];
        r[i].color[3] = baked[i].color.alpha;
    }
    free(baked);
    *out = r;
    *nout = nbaked;
    return PC_OK;
}

struct gradient_ctx {
    float from[2], to[2];
    pc_algo_gradient_shape shape;
    const pc_algo_gradient_stop *stops;
    size_t nstops;
    bool reverse;
    float opacity;
    pc_blend_mode blend;
    bool dither;
    yyjson_val *p;
};

static pc_status gradient_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    struct gradient_ctx *c = vctx;
    pc_rect bounds = pc_document_bounds(doc);
    pc_rect area = bounds;
    if (doc->has_selection) {
        pc_rect cb = pc_surface_content_bounds(&doc->selection);
        if (!pc_rect_is_empty(cb)) {
            area = pc_rect_intersect(cb, bounds);
        }
    }
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_surface *surf = NULL;
    bool lock = false;
    pc_status rc = pc_engine_target_surface(doc, *ha, *active, c->p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_algo_paint_gradient(surf, area, c->from, c->to, c->shape, c->stops, c->nstops, c->reverse,
                                  c->opacity, c->blend, c->dither, sel, lock, err);
}

static pc_status run_gradient(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "paint.gradient";
    float from[2], to[2];
    if (!point_param(p, "from", from)) {
        return pc_engine_err_bad_params(err, cmd, "missing from");
    }
    if (!point_param(p, "to", to)) {
        return pc_engine_err_bad_params(err, cmd, "missing to");
    }
    pc_algo_gradient_shape shape = PC_ALGO_GRADIENT_LINEAR;
    size_t stlen = 0;
    yyjson_val *stv = pc_json_get(p, "style");
    const char *style = stv != NULL ? pc_json_as_str(stv, &stlen) : NULL;
    if (style != NULL) {
        (void)pc_algo_gradient_shape_from_name(style, stlen, &shape);
    }

    pc_algo_gradient_stop *stops = NULL;
    size_t nstops = 0;
    bool have = false;
    pc_status rc = tool_stops(s, p, &stops, &nstops, &have, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!have) {
        rc = legacy_stops(s, p, &stops, &nstops, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    /* method bake (optional). */
    pc_algo_gradient_stop *baked = NULL;
    size_t nbaked = 0;
    rc = bake_method(p, stops, nstops, &baked, &nbaked, err);
    if (rc != PC_OK) {
        free(stops);
        return rc;
    }
    if (baked != NULL) {
        free(stops);
        stops = baked;
        nstops = nbaked;
    }

    bool reverse = bflag(p, "reverse", false);
    float opacity;
    rc = opacity_param(p, cmd, &opacity, err);
    if (rc != PC_OK) {
        free(stops);
        return rc;
    }
    pc_blend_mode blend = PC_BLEND_NORMAL;
    size_t mlen = 0;
    yyjson_val *mv = pc_json_get(p, "mode");
    const char *mode = mv != NULL ? pc_json_as_str(mv, &mlen) : NULL;
    if (mode != NULL) {
        (void)pc_engine_blend_from_str(mode, mlen, &blend);
    }
    bool dither = bflag(p, "dither", true);

    struct gradient_ctx ctx = {0};
    ctx.from[0] = from[0];
    ctx.from[1] = from[1];
    ctx.to[0] = to[0];
    ctx.to[1] = to[1];
    ctx.shape = shape;
    ctx.stops = stops;
    ctx.nstops = nstops;
    ctx.reverse = reverse;
    ctx.opacity = opacity;
    ctx.blend = blend;
    ctx.dither = dither;
    ctx.p = p;
    rc = pc_engine_session_edit(s, "Gradient", gradient_edit, &ctx, err);
    free(stops);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ========================================================================================== */
/* specs                                                                                       */
/* ========================================================================================== */

static const pc_engine_command SPECS[] = {
    {"paint.bucket", "Paint Bucket", NULL, 0, NULL,
     "{\"x\":px,\"y\":px,\"tolerance\":0..255=32,\"contiguous\":bool=true,\"antiAlias\":bool=true,"
     "\"sampleAllLayers\":bool=false,\"contents\":\"foreground|pattern\"=\"foreground\",\"color\":"
     "\"#rrggbb\"=foreground,\"pattern\":id|name (contents=pattern),\"scale\":%=100,\"angle\":deg,"
     "\"opacity\":1..100=100,\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}=Channels "
     "panel target}",
     pc_engine_has_paintable, run_bucket, true},
    {"paint.gradient", "Gradient", NULL, 0, NULL,
     "{\"from\":[x,y],\"to\":[x,y],\"style\":\"linear|radial|angle|reflected|diamond\"=\"linear\","
     "\"colors\":[\"#rrggbb\",...]? (evenly spaced),\"gradient\":preset name?,\"stops\":"
     "[[t,\"#rrggbb\"|\"foreground\"|\"background\"],...]?,\"transparency\":[[t,0..100],...]? (default: "
     "the current gradient, see gradient.presets.select),\"method\":\"classic|perceptual|linear|"
     "smooth\"? (Photoshop's interpolation; omitted: straight stops),\"smoothness\":0..100=100,"
     "\"reverse\":bool=false,\"dither\":bool=true,\"opacity\":1..100=100,\"mode\":\"normal|multiply|"
     "...\"=\"normal\",\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}=Channels panel "
     "target}",
     pc_engine_has_paintable, run_gradient, true},
};

const pc_engine_command *pc_engine_paint_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
