#define _XOPEN_SOURCE 700 /* realpath / lstat / S_ISLNK (XSI), for the slice-export path confinement */
/* File > Export > Save for Web (Legacy), Export Preferences and the Quick Export pipeline that
 * honours them. Port of crates/engine/src/web_cmds.rs (E-tail: GIF/PNG-8 colour quantization and
 * slice-based export).
 *
 * Save for Web optimises the flattened (optionally resized, sRGB-converted) image as GIF, PNG-8,
 * PNG-24, JPEG or WBMP, per slice, into a folder with an optional HTML table; without an output it
 * only reports the optimised size (the dialog's 2-Up/4-Up annotations). Colour reduction reuses
 * algo::quantize's palette builder and Bayer table through a per-image inverse colour map;
 * GIF/WBMP/progressive JPEG writers live in codecs::web.
 *
 * STUB (REWRITE_PLAN.md §7): File > Generate > Image Assets (Adobe's Generator) stays unsupported;
 * it needs layer_menu_cmds::layer_document (a single-layer document), which is not ported yet.
 * Nothing here crashes on any params (Rule 9). */
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "algo/quantize.h"
#include "base/blob.h"
#include "base/buf.h"
#include "base/str.h"
#include "codecs/codecs.h"
#include "codecs/image.h"
#include "codecs/web.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "doc/slices.h"
#include "engine/engine_internal.h"
#include "engine/prefs.h"
#include "engine/web_cmds.h"
#include "ops/ops.h"

/* ---- small helpers ------------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static uint8_t to8(float v) { return (uint8_t)roundf(clampf(v, 0.0f, 1.0f) * 255.0f); }

/* "#rrggbb" -> three 0..1 floats; false when malformed. */
static bool hex3(const char *s, size_t len, float out[3])
{
    if (s == NULL) {
        return false;
    }
    if (len >= 1 && s[0] == '#') {
        s++;
        len--;
    }
    if (len != 6) {
        return false;
    }
    for (size_t i = 0; i < 6; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    int v[6];
    for (size_t i = 0; i < 6; i++) {
        char c = s[i];
        v[i] = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
    }
    out[0] = (float)(v[0] * 16 + v[1]) / 255.0f;
    out[1] = (float)(v[2] * 16 + v[3]) / 255.0f;
    out[2] = (float)(v[4] * 16 + v[5]) / 255.0f;
    return true;
}

static const char *jstr(yyjson_val *p, const char *k, size_t *len)
{
    return pc_json_as_str(pc_json_get(p, k), len);
}

static bool jstr_eq(const char *s, size_t len, const char *lit)
{
    return s != NULL && strlen(lit) == len && memcmp(s, lit, len) == 0;
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* ASCII lowercase into a stack buffer (for case-insensitive id matching); truncates at cap. */
static void ascii_lower(const char *s, size_t len, char *out, size_t cap)
{
    size_t n = len < cap - 1 ? len : cap - 1;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[n] = '\0';
}

/* ---- settings ----------------------------------------------------------------------------- */

bool pc_web_format_from_id(const char *s, pc_web_format *out)
{
    if (s == NULL) {
        return false;
    }
    char b[16];
    ascii_lower(s, strlen(s), b, sizeof b);
    if (strcmp(b, "gif") == 0) {
        *out = PC_WEB_GIF;
    } else if (strcmp(b, "png8") == 0 || strcmp(b, "png-8") == 0) {
        *out = PC_WEB_PNG8;
    } else if (strcmp(b, "png24") == 0 || strcmp(b, "png-24") == 0 || strcmp(b, "png") == 0 ||
               strcmp(b, "png32") == 0) {
        *out = PC_WEB_PNG24;
    } else if (strcmp(b, "jpeg") == 0 || strcmp(b, "jpg") == 0) {
        *out = PC_WEB_JPEG;
    } else if (strcmp(b, "wbmp") == 0) {
        *out = PC_WEB_WBMP;
    } else {
        return false;
    }
    return true;
}

const char *pc_web_format_ext(pc_web_format f)
{
    switch (f) {
    case PC_WEB_GIF:
        return "gif";
    case PC_WEB_PNG8:
    case PC_WEB_PNG24:
        return "png";
    case PC_WEB_JPEG:
        return "jpg";
    case PC_WEB_WBMP:
        return "wbmp";
    }
    return "png";
}

void pc_web_settings_default(pc_web_settings *st)
{
    memset(st, 0, sizeof *st);
    st->format = PC_WEB_PNG24;
    st->palette = PC_ALGO_PALETTE_SELECTIVE;
    st->colors = 256;
    st->dither = PC_ALGO_DITHER_DIFFUSION;
    st->dither_amount = 0.88f;
    st->transparency = true;
    st->has_matte = true;
    st->matte[0] = st->matte[1] = st->matte[2] = 1.0f;
    st->interlaced = false;
    st->web_snap = 0.0f;
    st->quality = 60;
    st->progressive = false;
    st->optimized = true;
    st->embed_icc = false;
    st->metadata = PC_WEB_META_COPYRIGHT;
    st->convert_to_srgb = true;
}

void pc_web_settings_fini(pc_web_settings *st)
{
    if (st == NULL) {
        return;
    }
    free(st->color_table);
    free(st->edits);
    st->color_table = NULL;
    st->ncolor_table = 0;
    st->edits = NULL;
    st->nedits = 0;
}

bool pc_web_preset_settings(const char *name, pc_web_settings *out)
{
    pc_web_settings st;
    pc_web_settings_default(&st);
    size_t gif_colors = 256;
    bool gif_dither = true;
    if (strcmp(name, "GIF 128 Dithered") == 0) {
        gif_colors = 128;
        gif_dither = true;
    } else if (strcmp(name, "GIF 128 No Dither") == 0) {
        gif_colors = 128;
        gif_dither = false;
    } else if (strcmp(name, "GIF 32 Dithered") == 0) {
        gif_colors = 32;
        gif_dither = true;
    } else if (strcmp(name, "GIF 32 No Dither") == 0) {
        gif_colors = 32;
        gif_dither = false;
    } else if (strcmp(name, "GIF 64 Dithered") == 0) {
        gif_colors = 64;
        gif_dither = true;
    } else if (strcmp(name, "GIF 64 No Dither") == 0) {
        gif_colors = 64;
        gif_dither = false;
    } else if (strcmp(name, "GIF Restrictive") == 0) {
        gif_colors = 256;
        gif_dither = true;
        st.palette = PC_ALGO_PALETTE_WEB;
    } else if (strcmp(name, "JPEG High") == 0) {
        st.format = PC_WEB_JPEG;
        st.quality = 60;
        *out = st;
        return true;
    } else if (strcmp(name, "JPEG Low") == 0) {
        st.format = PC_WEB_JPEG;
        st.quality = 10;
        *out = st;
        return true;
    } else if (strcmp(name, "JPEG Medium") == 0) {
        st.format = PC_WEB_JPEG;
        st.quality = 30;
        *out = st;
        return true;
    } else if (strcmp(name, "JPEG Maximum") == 0) {
        st.format = PC_WEB_JPEG;
        st.quality = 100;
        *out = st;
        return true;
    } else if (strcmp(name, "JPEG Very High") == 0) {
        st.format = PC_WEB_JPEG;
        st.quality = 80;
        *out = st;
        return true;
    } else if (strcmp(name, "PNG-24") == 0) {
        st.format = PC_WEB_PNG24;
        *out = st;
        return true;
    } else if (strcmp(name, "PNG-8 128 Dithered") == 0) {
        st.format = PC_WEB_PNG8;
        st.colors = 128;
        *out = st;
        return true;
    } else {
        return false;
    }
    /* gif(colors, dither) */
    st.format = PC_WEB_GIF;
    st.colors = gif_colors;
    st.dither = gif_dither ? PC_ALGO_DITHER_DIFFUSION : PC_ALGO_DITHER_NONE;
    *out = st;
    return true;
}

pc_status pc_web_settings_from_params(yyjson_val *p, const char *cmd, pc_web_settings *out, pc_error *err)
{
    pc_web_settings st;
    pc_web_settings_default(&st);
    st.has_matte = false; /* WebSettings { matte: None, ..default() } */
    size_t len = 0;
    const char *preset = jstr(p, "preset", &len);
    if (preset != NULL) {
        char nm[48];
        ascii_lower(preset, len, nm, sizeof nm); /* keep exact case for preset lookup */
        /* preset names are case-sensitive in the reference; use the original bytes */
        char exact[48];
        size_t pn = len < sizeof exact - 1 ? len : sizeof exact - 1;
        memcpy(exact, preset, pn);
        exact[pn] = '\0';
        (void)nm;
        pc_web_settings ps;
        if (!pc_web_preset_settings(exact, &ps)) {
            pc_web_settings_fini(&st);
            return pc_engine_err_bad_params(err, cmd, "unknown preset `%s`", exact);
        }
        pc_web_settings_fini(&st);
        st = ps;
    }
    const char *f = jstr(p, "format", &len);
    if (f != NULL) {
        char tmp[16];
        ascii_lower(f, len, tmp, sizeof tmp);
        if (!pc_web_format_from_id(tmp, &st.format)) {
            pc_web_settings_fini(&st);
            return pc_engine_err_bad_params(err, cmd, "unknown format `%.*s` (gif|png8|png24|jpeg|wbmp)",
                                            (int)len, f);
        }
    }
    const char *k = jstr(p, "palette", &len);
    if (k != NULL) {
        char tmp[32];
        ascii_lower(k, len, tmp, sizeof tmp);
        if (strcmp(tmp, "restrictive") == 0 || strcmp(tmp, "web") == 0) {
            st.palette = PC_ALGO_PALETTE_WEB;
        } else if (!pc_algo_palette_kind_from_id(tmp, &st.palette)) {
            pc_web_settings_fini(&st);
            return pc_engine_err_bad_params(err, cmd,
                                            "unknown palette `%.*s` (perceptual|selective|adaptive|"
                                            "restrictive|exact|systemMac|systemWindows|uniform)",
                                            (int)len, k);
        }
    }
    int64_t colors = 0;
    if (pc_engine_param_int(p, "colors", &colors) && colors > 0) {
        if (colors < 2) {
            colors = 2;
        }
        if (colors > 256) {
            colors = 256;
        }
        st.colors = (size_t)colors;
    }
    const char *d = jstr(p, "dither", &len);
    if (d != NULL) {
        char tmp[16];
        ascii_lower(d, len, tmp, sizeof tmp);
        if (strcmp(tmp, "none") == 0 || strcmp(tmp, "nodither") == 0) {
            st.dither = PC_ALGO_DITHER_NONE;
        } else if (strcmp(tmp, "diffusion") == 0) {
            st.dither = PC_ALGO_DITHER_DIFFUSION;
        } else if (strcmp(tmp, "pattern") == 0) {
            st.dither = PC_ALGO_DITHER_PATTERN;
        } else if (strcmp(tmp, "noise") == 0) {
            st.dither = PC_ALGO_DITHER_NOISE;
        } else {
            pc_web_settings_fini(&st);
            return pc_engine_err_bad_params(err, cmd, "unknown dither `%.*s` (none|diffusion|pattern|noise)",
                                            (int)len, d);
        }
    }
    double a = 0.0;
    if (pc_json_as_f64(pc_json_get(p, "ditherAmount"), &a)) {
        st.dither_amount = clampf((float)(a / 100.0), 0.0f, 1.0f);
    }
    bool b = false;
    if (pc_json_as_bool(pc_json_get(p, "transparency"), &b)) {
        st.transparency = b;
    }
    const char *m = jstr(p, "matte", &len);
    if (m != NULL) {
        if (jstr_eq(m, len, "none")) {
            st.has_matte = false;
        } else {
            float c[3];
            if (!hex3(m, len, c)) {
                pc_web_settings_fini(&st);
                return pc_engine_err_bad_params(err, cmd, "matte is \"none\" or \"#rrggbb\"");
            }
            st.has_matte = true;
            st.matte[0] = c[0];
            st.matte[1] = c[1];
            st.matte[2] = c[2];
        }
    }
    struct {
        const char *key;
        bool *slot;
    } flags[] = {{"interlaced", &st.interlaced},  {"progressive", &st.progressive},
                 {"optimized", &st.optimized},    {"embedIcc", &st.embed_icc},
                 {"convertToSrgb", &st.convert_to_srgb}};
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        if (pc_json_as_bool(pc_json_get(p, flags[i].key), &b)) {
            *flags[i].slot = b;
        }
    }
    double w = 0.0;
    if (pc_json_as_f64(pc_json_get(p, "webSnap"), &w)) {
        st.web_snap = clampf((float)(w / 100.0), 0.0f, 1.0f);
    }
    double q = 0.0;
    if (pc_json_as_f64(pc_json_get(p, "quality"), &q)) {
        double cq = q < 0.0 ? 0.0 : (q > 100.0 ? 100.0 : q);
        st.quality = (uint8_t)round(cq);
    }
    const char *md = jstr(p, "metadata", &len);
    if (md != NULL) {
        char tmp[24];
        ascii_lower(md, len, tmp, sizeof tmp);
        if (strcmp(tmp, "none") == 0) {
            st.metadata = PC_WEB_META_NONE;
        } else if (strcmp(tmp, "copyright") == 0) {
            st.metadata = PC_WEB_META_COPYRIGHT;
        } else if (strcmp(tmp, "copyrightandcontact") == 0 || strcmp(tmp, "contact") == 0) {
            st.metadata = PC_WEB_META_CONTACT;
        } else if (strcmp(tmp, "all") == 0 || strcmp(tmp, "allexceptcamerainfo") == 0) {
            st.metadata = PC_WEB_META_ALL;
        } else {
            pc_web_settings_fini(&st);
            return pc_engine_err_bad_params(err, cmd,
                                            "unknown metadata `%.*s` (none|copyright|copyrightAndContact|all)",
                                            (int)len, md);
        }
    }
    yyjson_val *table = pc_json_get(p, "colorTable");
    if (table != NULL) {
        if (!yyjson_is_arr(table) || yyjson_arr_size(table) < 1 || yyjson_arr_size(table) > 256) {
            pc_web_settings_fini(&st);
            return pc_engine_err_bad_params(err, cmd, "colorTable must contain 1-256 #rrggbb colours");
        }
        size_t n = yyjson_arr_size(table);
        uint8_t(*ct)[3] = malloc(n * sizeof *ct);
        if (ct == NULL) {
            pc_web_settings_fini(&st);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        size_t i = 0;
        yyjson_val *v;
        yyjson_arr_iter it;
        yyjson_arr_iter_init(table, &it);
        while ((v = yyjson_arr_iter_next(&it)) != NULL) {
            size_t sl = 0;
            const char *cs = pc_json_as_str(v, &sl);
            float c[3];
            if (!hex3(cs, sl, c)) {
                free(ct);
                pc_web_settings_fini(&st);
                return pc_engine_err_bad_params(err, cmd, "colorTable entries must be #rrggbb colours");
            }
            ct[i][0] = to8(c[0]);
            ct[i][1] = to8(c[1]);
            ct[i][2] = to8(c[2]);
            i++;
        }
        st.has_color_table = true;
        st.color_table = ct;
        st.ncolor_table = n;
    }
    yyjson_val *edits = pc_json_get(p, "colorTableEdits");
    if (edits != NULL) {
        if (!yyjson_is_obj(edits) || yyjson_obj_size(edits) > 256) {
            pc_web_settings_fini(&st);
            return pc_engine_err_bad_params(
                err, cmd, "colorTableEdits must be an object of palette indices to #rrggbb colours");
        }
        if (!st.has_color_table) {
            pc_web_settings_fini(&st);
            return pc_engine_err_bad_params(err, cmd, "colorTableEdits requires colorTable");
        }
        size_t cap = yyjson_obj_size(edits);
        pc_web_edit *ed = cap > 0 ? malloc(cap * sizeof *ed) : NULL;
        if (cap > 0 && ed == NULL) {
            pc_web_settings_fini(&st);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        size_t ne = 0;
        yyjson_obj_iter it;
        yyjson_obj_iter_init(edits, &it);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&it)) != NULL) {
            yyjson_val *val = yyjson_obj_iter_get_val(key);
            const char *ks = yyjson_get_str(key);
            char *end = NULL;
            unsigned long idx = ks != NULL ? strtoul(ks, &end, 10) : 0;
            if (ks == NULL || end == ks || *end != '\0' || idx >= st.ncolor_table) {
                free(ed);
                pc_web_settings_fini(&st);
                return pc_engine_err_bad_params(err, cmd, "colorTableEdits index is outside colorTable");
            }
            size_t vl = 0;
            const char *vs = pc_json_as_str(val, &vl);
            float c[3];
            if (!hex3(vs, vl, c)) {
                free(ed);
                pc_web_settings_fini(&st);
                return pc_engine_err_bad_params(err, cmd, "colorTableEdits values must be #rrggbb colours");
            }
            ed[ne].index = (size_t)idx;
            ed[ne].color[0] = to8(c[0]);
            ed[ne].color[1] = to8(c[1]);
            ed[ne].color[2] = to8(c[2]);
            ne++;
        }
        st.edits = ed;
        st.nedits = ne;
    }
    *out = st;
    return PC_OK;
}

/* ---- the optimiser's inverse colour map + quantizer --------------------------------------- */

typedef struct {
    const uint8_t (*pal)[3];
    size_t npal;
    bool has_t;
    size_t t;
    uint16_t *lut; /* 1 << 18 entries; 0xFFFF = unresolved */
} inverse_map;

static size_t inverse_map_get(inverse_map *m, const float c[3])
{
    size_t q0 = (size_t)roundf(clampf(c[0], 0.0f, 1.0f) * 255.0f) >> 2;
    size_t q1 = (size_t)roundf(clampf(c[1], 0.0f, 1.0f) * 255.0f) >> 2;
    size_t q2 = (size_t)roundf(clampf(c[2], 0.0f, 1.0f) * 255.0f) >> 2;
    size_t key = q0 << 12 | q1 << 6 | q2;
    uint16_t v = m->lut[key];
    if (v != 0xFFFF) {
        return (size_t)v;
    }
    float center[3] = {(float)(q0 * 4 + 2), (float)(q1 * 4 + 2), (float)(q2 * 4 + 2)};
    float best_d = FLT_MAX;
    size_t best_i = 0;
    for (size_t i = 0; i < m->npal; i++) {
        if (m->has_t && i == m->t) {
            continue;
        }
        float dx = center[0] - (float)m->pal[i][0];
        float dy = center[1] - (float)m->pal[i][1];
        float dz = center[2] - (float)m->pal[i][2];
        float dd = dx * dx + dy * dy + dz * dz;
        if (dd < best_d) {
            best_d = dd;
            best_i = i;
        }
    }
    m->lut[key] = (uint16_t)best_i;
    return best_i;
}

static float hash01(size_t x, size_t y)
{
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ 0x2545f491u;
    h ^= h >> 13;
    h = h * 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xffff) / 65535.0f;
}

/* quantize_band: maps px[0..n] (n = h*w rows w wide) to palette indices into idx (n bytes), with
 * dithering (y0 keeps patterns continuous across bands). Pixels with alpha < 1/2 take index t.
 * pal is npal RGB triples (the transparent entry pushed last when has_t). Returns PC_OK; idx must
 * hold n bytes. */
static pc_status quantize_band(const float (*px)[4], size_t n, size_t w, size_t y0, const uint8_t (*pal)[3],
                               size_t npal, pc_algo_dither dither, float amount, bool has_t, size_t t,
                               uint8_t *idx, pc_error *err)
{
    size_t wmax = w == 0 ? 1 : w;
    size_t h = n / wmax;
    inverse_map map = {.pal = pal, .npal = npal, .has_t = has_t, .t = t, .lut = NULL};
    map.lut = malloc((size_t)(1u << 18) * sizeof *map.lut);
    if (map.lut == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < (size_t)(1u << 18); i++) {
        map.lut[i] = 0xFFFF;
    }
    size_t opaque_n = npal - (has_t ? 1u : 0u);
    float cb = cbrtf((float)opaque_n);
    if (cb < 1.0f) {
        cb = 1.0f;
    }
    float spread = 1.0f / cb;
    if (spread > 0.5f) {
        spread = 0.5f;
    }
    float a = clampf(amount, 0.0f, 1.0f);
    spread *= a;
    float(*cur)[3] = NULL;
    float(*nxt)[3] = NULL;
    if (dither == PC_ALGO_DITHER_DIFFUSION) {
        cur = calloc(w + 2, sizeof *cur);
        nxt = calloc(w + 2, sizeof *nxt);
        if (cur == NULL || nxt == NULL) {
            free(cur);
            free(nxt);
            free(map.lut);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    for (size_t y = 0; y < h; y++) {
        if (dither == PC_ALGO_DITHER_DIFFUSION) {
            float(*tmp)[3] = cur;
            cur = nxt;
            nxt = tmp;
            for (size_t i = 0; i < w + 2; i++) {
                nxt[i][0] = nxt[i][1] = nxt[i][2] = 0.0f;
            }
        }
        for (size_t x = 0; x < w; x++) {
            size_t i = y * w + x;
            const float *p = px[i];
            if (p[3] < 0.5f && has_t) {
                idx[i] = (uint8_t)t;
                continue;
            }
            float c[3] = {p[0], p[1], p[2]};
            switch (dither) {
            case PC_ALGO_DITHER_NONE:
                break;
            case PC_ALGO_DITHER_DIFFUSION:
                c[0] += cur[x + 1][0];
                c[1] += cur[x + 1][1];
                c[2] += cur[x + 1][2];
                break;
            case PC_ALGO_DITHER_PATTERN: {
                float dd = pc_algo_quantize_bayer8(x, y0 + y) * spread;
                c[0] += dd;
                c[1] += dd;
                c[2] += dd;
                break;
            }
            case PC_ALGO_DITHER_NOISE: {
                float dd = (hash01(x, y0 + y) - 0.5f) * spread;
                c[0] += dd;
                c[1] += dd;
                c[2] += dd;
                break;
            }
            }
            c[0] = clampf(c[0], 0.0f, 1.0f);
            c[1] = clampf(c[1], 0.0f, 1.0f);
            c[2] = clampf(c[2], 0.0f, 1.0f);
            size_t kk = inverse_map_get(&map, c);
            idx[i] = (uint8_t)kk;
            if (dither == PC_ALGO_DITHER_DIFFUSION) {
                float e[3] = {(float)pal[kk][0] / 255.0f, (float)pal[kk][1] / 255.0f,
                              (float)pal[kk][2] / 255.0f};
                float dq[3] = {(c[0] - e[0]) * a, (c[1] - e[1]) * a, (c[2] - e[2]) * a};
                for (size_t qi = 0; qi < 3; qi++) {
                    cur[x + 2][qi] += dq[qi] * 7.0f / 16.0f;
                    nxt[x][qi] += dq[qi] * 3.0f / 16.0f;
                    nxt[x + 1][qi] += dq[qi] * 5.0f / 16.0f;
                    nxt[x + 2][qi] += dq[qi] / 16.0f;
                }
            }
        }
    }
    free(cur);
    free(nxt);
    free(map.lut);
    return PC_OK;
}

/* quantize_rows: quantize_band over the whole image, in bands of 64 rows when the dither is
 * position-independent (output identical to a single band; the reference parallelises this). */
static pc_status quantize_rows(const float (*px)[4], size_t n, size_t w, const uint8_t (*pal)[3],
                               size_t npal, pc_algo_dither dither, float amount, bool has_t, size_t t,
                               uint8_t *idx, pc_error *err)
{
    size_t wmax = w == 0 ? 1 : w;
    size_t h = n / wmax;
    if (dither == PC_ALGO_DITHER_DIFFUSION || h < 128) {
        return quantize_band(px, n, w, 0, pal, npal, dither, amount, has_t, t, idx, err);
    }
    for (size_t band = 0; band * 64 < h; band++) {
        size_t y0 = band * 64;
        size_t rows = h - y0 < 64 ? h - y0 : 64;
        size_t off = y0 * w;
        pc_status rc = quantize_band(px + off, rows * w, w, y0, pal, npal, dither, amount, has_t, t,
                                     idx + off, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return PC_OK;
}

/* ---- optimize ----------------------------------------------------------------------------- */

void pc_web_optimized_fini(pc_web_optimized *o)
{
    if (o == NULL) {
        return;
    }
    pc_buf_fini(&o->bytes);
    free(o->color_table);
    free(o->indices);
    free(o->preview);
    memset(o, 0, sizeof *o);
}

/* over(): straight-alpha pixel composited over the matte. */
static void composite_over(const float p[4], const float matte[3], float out[4])
{
    float ia = 1.0f - p[3];
    out[0] = p[0] * p[3] + matte[0] * ia;
    out[1] = p[1] * p[3] + matte[1] * ia;
    out[2] = p[2] * p[3] + matte[2] * ia;
    out[3] = 1.0f;
}

pc_status pc_web_optimize(const float (*px)[4], size_t bw, pc_rect rect, const pc_web_settings *st,
                          const uint8_t *icc, size_t icc_len, const char *xmp, size_t xmp_len, float dpi,
                          bool preview, pc_web_optimized *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    size_t px_len = (size_t)bw * (rect.y1 > 0 ? (size_t)rect.y1 : 0); /* lower bound only */
    (void)px_len;
    if (bw == 0 || rect.x0 < 0 || rect.y0 < 0 || pc_rect_is_empty(rect) || (size_t)rect.x1 > bw) {
        return pc_engine_err_other(err, "slice is outside the pixel buffer");
    }
    size_t w = pc_rect_width(rect);
    size_t h = pc_rect_height(rect);
    out->ext = pc_web_format_ext(st->format);
    out->width = (uint32_t)w;
    out->height = (uint32_t)h;
    float matte[3] = {st->has_matte ? st->matte[0] : 1.0f, st->has_matte ? st->matte[1] : 1.0f,
                      st->has_matte ? st->matte[2] : 1.0f};

#define REGION_PIXEL(yy, xx) (px[(size_t)(yy) * bw + (size_t)(xx)])

    pc_status rc = PC_OK;
    switch (st->format) {
    case PC_WEB_PNG24: {
        bool alpha = false;
        if (st->transparency) {
            for (int32_t y = rect.y0; y < rect.y1 && !alpha; y++) {
                for (int32_t x = rect.x0; x < rect.x1; x++) {
                    if (REGION_PIXEL(y, x)[3] < 1.0f) {
                        alpha = true;
                        break;
                    }
                }
            }
        }
        size_t kch = alpha ? 4 : 3;
        uint8_t *data = calloc(w * h * kch, 1);
        if (data == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        size_t o = 0;
        for (int32_t y = rect.y0; y < rect.y1; y++) {
            for (int32_t x = rect.x0; x < rect.x1; x++) {
                const float *p = REGION_PIXEL(y, x);
                float q[4];
                if (alpha) {
                    q[0] = p[0];
                    q[1] = p[1];
                    q[2] = p[2];
                    q[3] = p[3];
                } else {
                    composite_over(p, matte, q);
                }
                data[o++] = to8(q[0]);
                data[o++] = to8(q[1]);
                data[o++] = to8(q[2]);
                if (alpha) {
                    data[o++] = to8(q[3]);
                }
            }
        }
        pc_cimage img;
        pc_cimage_init(&img);
        rc = pc_cimage_from_copy((uint32_t)w, (uint32_t)h, alpha ? PC_CL_RGBA : PC_CL_RGB, PC_CS_U8, data,
                                 w * h * kch, &img, err);
        if (rc == PC_OK && icc != NULL) {
            rc = pc_meta_blob_set(&img.icc, icc, icc_len, err);
        }
        if (rc == PC_OK) {
            img.meta.has_dpi = true;
            img.meta.dpi_x = dpi;
            img.meta.dpi_y = dpi;
            if (xmp != NULL) {
                rc = pc_meta_blob_set(&img.meta.xmp, xmp, xmp_len, err);
            }
        }
        if (rc == PC_OK) {
            pc_cencode_opts opts = pc_cencode_opts_default();
            opts.png_interlaced = st->interlaced;
            opts.embed_icc = icc != NULL;
            opts.embed_metadata = xmp != NULL;
            rc = pc_cencode(&img, PC_CF_PNG, &opts, &out->bytes, err);
        }
        if (rc == PC_OK && preview) {
            out->preview = malloc(w * h * 4);
            if (out->preview == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            } else {
                out->npreview = w * h * 4;
                for (size_t i = 0; i < w * h; i++) {
                    out->preview[i * 4 + 0] = data[i * kch + 0];
                    out->preview[i * 4 + 1] = data[i * kch + 1];
                    out->preview[i * 4 + 2] = data[i * kch + 2];
                    out->preview[i * 4 + 3] = alpha ? data[i * kch + 3] : 255;
                }
            }
        }
        pc_cimage_fini(&img);
        free(data);
        break;
    }
    case PC_WEB_JPEG: {
        uint8_t *rgb = calloc(w * h * 3, 1);
        if (rgb == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        size_t o = 0;
        for (int32_t y = rect.y0; y < rect.y1; y++) {
            for (int32_t x = rect.x0; x < rect.x1; x++) {
                float q[4];
                composite_over(REGION_PIXEL(y, x), matte, q);
                rgb[o++] = to8(q[0]);
                rgb[o++] = to8(q[1]);
                rgb[o++] = to8(q[2]);
            }
        }
        uint8_t quality = st->quality > 1 ? st->quality : 1;
        const uint8_t *eicc = st->embed_icc ? icc : NULL;
        size_t eicc_len = st->embed_icc ? icc_len : 0;
        rc = pc_web_encode_jpeg_rgb8((uint32_t)w, (uint32_t)h, rgb, w * h * 3, quality, st->progressive,
                                     st->optimized, eicc, eicc_len, true, dpi, xmp, xmp != NULL ? xmp_len : 0,
                                     &out->bytes, err);
        if (rc == PC_OK && preview) {
            out->preview = malloc(w * h * 4);
            if (out->preview == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            } else {
                out->npreview = w * h * 4;
                pc_cimage dec;
                pc_cimage_init(&dec);
                uint8_t *rgba = NULL;
                size_t rgba_n = 0;
                bool ok = pc_cdecode(out->bytes.data, out->bytes.len, &dec, NULL) == PC_OK &&
                          pc_cimage_to_rgba8(&dec, &rgba, &rgba_n, NULL) == PC_OK && rgba_n == w * h * 4;
                if (ok) {
                    memcpy(out->preview, rgba, w * h * 4);
                } else {
                    for (size_t i = 0; i < w * h; i++) {
                        out->preview[i * 4 + 0] = rgb[i * 3 + 0];
                        out->preview[i * 4 + 1] = rgb[i * 3 + 1];
                        out->preview[i * 4 + 2] = rgb[i * 3 + 2];
                        out->preview[i * 4 + 3] = 255;
                    }
                }
                free(rgba);
                pc_cimage_fini(&dec);
            }
        }
        free(rgb);
        break;
    }
    case PC_WEB_WBMP: {
        float *gray = calloc(w * h, sizeof *gray);
        if (gray == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        size_t o = 0;
        for (int32_t y = rect.y0; y < rect.y1; y++) {
            for (int32_t x = rect.x0; x < rect.x1; x++) {
                float q[4];
                composite_over(REGION_PIXEL(y, x), matte, q);
                gray[o++] = 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
            }
        }
        pc_algo_bitmap_method method = {.kind = PC_ALGO_BITMAP_DIFFUSION, .cell = 0, .angle = 0,
                                        .shape = PC_ALGO_HALFTONE_ROUND};
        if (st->dither == PC_ALGO_DITHER_NONE) {
            method.kind = PC_ALGO_BITMAP_THRESHOLD;
        } else if (st->dither == PC_ALGO_DITHER_PATTERN) {
            method.kind = PC_ALGO_BITMAP_PATTERN;
        }
        rc = pc_algo_quantize_to_bitmap(gray, w * h, w, method, err);
        bool *white = NULL;
        if (rc == PC_OK) {
            white = malloc(w * h * sizeof *white);
            if (white == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
        }
        if (rc == PC_OK) {
            for (size_t i = 0; i < w * h; i++) {
                white[i] = gray[i] > 0.5f;
            }
            rc = pc_web_encode_wbmp((uint32_t)w, (uint32_t)h, white, w * h, &out->bytes, err);
        }
        if (rc == PC_OK) {
            out->has_colors = true;
            out->colors = 2;
            if (preview) {
                out->preview = malloc(w * h * 4);
                if (out->preview == NULL) {
                    rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                } else {
                    out->npreview = w * h * 4;
                    for (size_t i = 0; i < w * h; i++) {
                        uint8_t v = white[i] ? 255 : 0;
                        out->preview[i * 4 + 0] = v;
                        out->preview[i * 4 + 1] = v;
                        out->preview[i * 4 + 2] = v;
                        out->preview[i * 4 + 3] = 255;
                    }
                }
            }
        }
        free(white);
        free(gray);
        break;
    }
    case PC_WEB_GIF:
    case PC_WEB_PNG8: {
        bool transparent = false;
        if (st->transparency) {
            for (int32_t y = rect.y0; y < rect.y1 && !transparent; y++) {
                for (int32_t x = rect.x0; x < rect.x1; x++) {
                    if (REGION_PIXEL(y, x)[3] < 0.5f) {
                        transparent = true;
                        break;
                    }
                }
            }
        }
        float(*work)[4] = calloc(w * h, sizeof *work);
        if (work == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        size_t o = 0;
        for (int32_t y = rect.y0; y < rect.y1; y++) {
            for (int32_t x = rect.x0; x < rect.x1; x++) {
                const float *p = REGION_PIXEL(y, x);
                if (st->transparency && p[3] < 0.5f) {
                    work[o][0] = work[o][1] = work[o][2] = work[o][3] = 0.0f;
                } else if (st->transparency && !st->has_matte) {
                    work[o][0] = p[0];
                    work[o][1] = p[1];
                    work[o][2] = p[2];
                    work[o][3] = 1.0f;
                } else {
                    composite_over(p, matte, work[o]);
                }
                o++;
            }
        }
        size_t room;
        if (transparent) {
            room = st->colors > 1 ? st->colors - 1 : 0;
            if (room < 2) {
                room = 2;
            }
        } else {
            room = st->colors;
        }
        /* palette (dynamic; transparent entry pushed at the end). */
        uint8_t(*pal)[3] = NULL;
        size_t npal = 0, palcap = 0;
        if (st->has_color_table) {
            size_t lim = transparent ? 255 : 256;
            if (st->ncolor_table == 0 || st->ncolor_table > lim) {
                free(work);
                return pc_engine_err_other(err, "colorTable has too many entries for this image's transparency");
            }
            palcap = st->ncolor_table + 1;
            pal = malloc(palcap * sizeof *pal);
            if (pal == NULL) {
                free(work);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            memcpy(pal, st->color_table, st->ncolor_table * sizeof *pal);
            npal = st->ncolor_table;
        } else {
            /* Build an adaptive palette from an even ~1 MP sample of the opaque pixels. */
            size_t n_opaque = 0;
            for (size_t i = 0; i < w * h; i++) {
                if (work[i][3] >= 0.5f) {
                    n_opaque++;
                }
            }
            size_t step = (n_opaque > (1u << 20) && st->palette != PC_ALGO_PALETTE_EXACT)
                              ? n_opaque / (1u << 20) + 1
                              : 1;
            size_t cap_src = n_opaque > 0 ? (n_opaque + step - 1) / step : w * h;
            float(*src)[4] = malloc((cap_src > 0 ? cap_src : 1) * sizeof *src);
            if (src == NULL) {
                free(work);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            size_t ns = 0, seen = 0;
            for (size_t i = 0; i < w * h; i++) {
                if (work[i][3] < 0.5f) {
                    continue;
                }
                if (seen % step == 0) {
                    memcpy(src[ns++], work[i], sizeof src[0]);
                }
                seen++;
            }
            if (ns == 0) {
                free(src);
                src = malloc(w * h * sizeof *src);
                if (src == NULL) {
                    free(work);
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory");
                }
                memcpy(src, work, w * h * sizeof *src);
                ns = w * h;
            }
            pc_algo_palette built;
            rc = pc_algo_quantize_build_palette((const float(*)[4])src, ns, st->palette, room,
                                                PC_ALGO_FORCED_NONE, &built, err);
            free(src);
            if (rc != PC_OK) {
                free(work);
                return rc;
            }
            palcap = built.n + 2;
            pal = malloc(palcap * sizeof *pal);
            if (pal == NULL) {
                free(work);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            memcpy(pal, built.c, built.n * sizeof *pal);
            npal = built.n;
        }
        /* Web Snap. */
        if (st->web_snap > 0.0f && !st->has_color_table) {
            uint8_t tol = (uint8_t)(st->web_snap * 255.0f);
            for (size_t i = 0; i < npal; i++) {
                uint8_t snap[3];
                bool all = true;
                for (int cch = 0; cch < 3; cch++) {
                    snap[cch] = (uint8_t)(roundf((float)pal[i][cch] / 51.0f) * 51.0f);
                    uint8_t diff = pal[i][cch] > snap[cch] ? pal[i][cch] - snap[cch] : snap[cch] - pal[i][cch];
                    if (diff > tol) {
                        all = false;
                    }
                }
                if (all) {
                    pal[i][0] = snap[0];
                    pal[i][1] = snap[1];
                    pal[i][2] = snap[2];
                }
            }
        }
        if (transparent && npal > 255) {
            npal = 255;
        }
        if (npal == 0) {
            free(pal);
            free(work);
            return pc_engine_err_other(err, "empty colour table");
        }
        uint8_t first[3] = {pal[0][0], pal[0][1], pal[0][2]};
        /* opaque source palette before replacements (the result's color_table). */
        out->color_table = malloc(npal * sizeof *out->color_table);
        if (out->color_table == NULL) {
            free(pal);
            free(work);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memcpy(out->color_table, pal, npal * sizeof *pal);
        out->ncolor_table = npal;
        bool has_t = false;
        size_t t = 0;
        if (transparent) {
            pal[npal][0] = first[0];
            pal[npal][1] = first[1];
            pal[npal][2] = first[2];
            t = npal;
            npal++;
            has_t = true;
        }
        out->has_transparent = has_t;
        out->transparent_index = t;
        uint8_t *idx = malloc(w * h);
        if (idx == NULL) {
            free(pal);
            free(work);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        rc = quantize_rows((const float(*)[4])work, w * h, w, (const uint8_t(*)[3])pal, npal, st->dither,
                           st->dither_amount, has_t, t, idx, err);
        free(work);
        if (rc != PC_OK) {
            free(pal);
            free(idx);
            return rc;
        }
        /* Replacements applied after indexing. */
        for (size_t e = 0; e < st->nedits; e++) {
            size_t index = st->edits[e].index;
            if (index >= npal || (has_t && index == t)) {
                free(pal);
                free(idx);
                return pc_engine_err_other(err, "colour edit is outside the opaque palette");
            }
            pal[index][0] = st->edits[e].color[0];
            pal[index][1] = st->edits[e].color[1];
            pal[index][2] = st->edits[e].color[2];
        }
        if (preview) {
            out->preview = malloc(w * h * 4);
            if (out->preview == NULL) {
                free(pal);
                free(idx);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            out->npreview = w * h * 4;
            for (size_t i = 0; i < w * h; i++) {
                size_t kk = idx[i];
                const uint8_t *c = kk < npal ? pal[kk] : first;
                out->preview[i * 4 + 0] = c[0];
                out->preview[i * 4 + 1] = c[1];
                out->preview[i * 4 + 2] = c[2];
                out->preview[i * 4 + 3] = (has_t && kk == t) ? 0 : 255;
            }
        }
        int tt = has_t ? (int)t : -1;
        if (st->format == PC_WEB_GIF) {
            rc = pc_web_encode_gif_indexed((uint32_t)w, (uint32_t)h, idx, w * h, (const uint8_t(*)[3])pal,
                                           npal, tt, st->interlaced, &out->bytes, err);
        } else {
            rc = pc_cencode_png_indexed((uint32_t)w, (uint32_t)h, idx, w * h, (const uint8_t(*)[3])pal, npal,
                                        tt, &out->bytes, err);
        }
        if (rc == PC_OK) {
            out->has_colors = true;
            out->colors = npal;
            if (preview) {
                out->indices = idx;
                out->nindices = w * h;
                idx = NULL;
            }
        }
        free(idx);
        free(pal);
        break;
    }
    }
#undef REGION_PIXEL
    if (rc != PC_OK) {
        pc_web_optimized_fini(out);
    }
    return rc;
}

/* ---- the document as Save for Web sees it ------------------------------------------------- */

/* RGB (converted to sRGB unless told otherwise), resized by width/height/percent, flattened.
 * Returns a new document in *out (caller fini) and the x/y scale factors. */
static pc_status web_document(const pc_document *doc, yyjson_val *p, const pc_web_settings *st,
                              pc_document *out, double *sx, double *sy, pc_error *err)
{
    pc_engine_session *tmp = NULL;
    pc_status rc = pc_engine_session_new(&tmp, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_document clone;
    rc = pc_document_clone(doc, &clone, err);
    if (rc != PC_OK) {
        pc_engine_session_free(tmp);
        return rc;
    }
    size_t idx = 0;
    rc = pc_engine_session_add_document(tmp, &clone, NULL, &idx, err);
    if (rc != PC_OK) {
        pc_document_fini(&clone);
        pc_engine_session_free(tmp);
        return rc;
    }
    pc_json_value ro = PC_JSON_VALUE_INIT;
    if (doc->mode != PC_COLOR_MODE_RGB) {
        rc = pc_engine_session_execute_json(tmp, "image.mode.rgb", "{}", &ro, err);
        pc_json_value_fini(&ro);
    }
    if (rc == PC_OK && st->convert_to_srgb && doc->icc_profile != NULL) {
        rc = pc_engine_session_execute_json(tmp, "edit.convertToProfile",
                                            "{\"profile\":\"srgb\",\"intent\":\"perceptual\"}", &ro, err);
        pc_json_value_fini(&ro);
    }
    double w0 = (double)doc->size.width;
    double h0 = (double)doc->size.height;
    double pct = 0.0, pw = 0.0, ph = 0.0;
    bool has_pct = pc_json_as_f64(pc_json_get(p, "percent"), &pct) && pct > 0.0;
    bool has_w = pc_json_as_f64(pc_json_get(p, "width"), &pw) && pw > 0.0;
    bool has_h = pc_json_as_f64(pc_json_get(p, "height"), &ph) && ph > 0.0;
    double nw, nh;
    if (has_pct) {
        nw = w0 * pct / 100.0;
        nh = h0 * pct / 100.0;
    } else if (has_w && has_h) {
        nw = pw;
        nh = ph;
    } else if (has_w) {
        nw = pw;
        nh = h0 * pw / w0;
    } else if (has_h) {
        nw = w0 * ph / h0;
        nh = ph;
    } else {
        nw = w0;
        nh = h0;
    }
    nw = round(nw);
    nh = round(nh);
    if (nw < 1.0) {
        nw = 1.0;
    }
    if (nh < 1.0) {
        nh = 1.0;
    }
    if (rc == PC_OK && (fabs(nw - w0) >= 1.0 || fabs(nh - h0) >= 1.0)) {
        size_t rl = 0;
        const char *resample = jstr(p, "resample", &rl);
        char rs[32];
        if (resample != NULL) {
            ascii_lower(resample, rl, rs, sizeof rs);
        } else {
            strcpy(rs, "bicubic");
        }
        char cmd[128];
        snprintf(cmd, sizeof cmd, "{\"width\":%d,\"height\":%d,\"resample\":\"%s\"}", (int)nw, (int)nh, rs);
        rc = pc_engine_session_execute_json(tmp, "image.imageSize", cmd, &ro, err);
        pc_json_value_fini(&ro);
    }
    if (rc == PC_OK) {
        const pc_engine_doc_state *d = pc_engine_session_active(tmp);
        if (d == NULL) {
            rc = pc_engine_err_no_document(err);
        } else {
            rc = pc_document_clone(pc_doc_arc_get(d->doc), out, err);
        }
    }
    *sx = nw / w0;
    *sy = nh / h0;
    pc_engine_session_free(tmp);
    return rc;
}

/* ---- slice / path safety + HTML ----------------------------------------------------------- */

static bool is_windows_device_asset_name(const char *comp, size_t len)
{
    /* stem = up to first '.', trailing spaces trimmed, uppercased */
    size_t end = len;
    for (size_t i = 0; i < len; i++) {
        if (comp[i] == '.') {
            end = i;
            break;
        }
    }
    while (end > 0 && comp[end - 1] == ' ') {
        end--;
    }
    char s[16];
    size_t n = end < sizeof s - 1 ? end : sizeof s - 1;
    for (size_t i = 0; i < n; i++) {
        char c = comp[i];
        s[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    s[n] = '\0';
    static const char *const dev[] = {"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"};
    for (size_t i = 0; i < sizeof dev / sizeof dev[0]; i++) {
        if (strcmp(s, dev[i]) == 0) {
            return true;
        }
    }
    if ((strncmp(s, "COM", 3) == 0 || strncmp(s, "LPT", 3) == 0) && s[3] >= '1' && s[3] <= '9' && s[4] == '\0') {
        return true;
    }
    return false;
}

/* portable_generated_stem: append '_' to a reserved device name. out must hold len+2 bytes. */
static char *portable_generated_stem(const char *stem)
{
    size_t len = strlen(stem);
    bool dev = is_windows_device_asset_name(stem, len);
    char *r = malloc(len + 2);
    if (r == NULL) {
        return NULL;
    }
    memcpy(r, stem, len);
    if (dev) {
        r[len] = '_';
        r[len + 1] = '\0';
    } else {
        r[len] = '\0';
    }
    return r;
}

/* slice_file: file-name-safe slice name (alphanumeric/-/_ kept, else '_'; empty -> "slice"). */
static char *slice_file(const char *name)
{
    size_t len = strlen(name);
    /* Unicode alphanumeric: keep ASCII alnum + '-' '_'; non-ASCII bytes (>=0x80) treated as
     * alphanumeric to match char::is_alphanumeric for the common case, else '_'. */
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    c == '-' || c == '_' || c >= 0x80;
        buf[o++] = keep ? (char)c : '_';
    }
    buf[o] = '\0';
    const char *base = o == 0 ? "slice" : buf;
    char *r = portable_generated_stem(base);
    free(buf);
    return r;
}

static pc_status html_escape_append(pc_buf *b, const char *s, pc_error *err)
{
    pc_status rc = PC_OK;
    for (const char *p = s; *p != '\0' && rc == PC_OK; p++) {
        switch (*p) {
        case '&':
            rc = pc_buf_append_str(b, "&amp;", err);
            break;
        case '<':
            rc = pc_buf_append_str(b, "&lt;", err);
            break;
        case '>':
            rc = pc_buf_append_str(b, "&gt;", err);
            break;
        case '"':
            rc = pc_buf_append_str(b, "&quot;", err);
            break;
        default:
            rc = pc_buf_append_byte(b, (uint8_t)*p, err);
        }
    }
    return rc;
}

/* safe_slice_url: ordinary web/contact or relative links become anchors; active schemes do not. */
static bool safe_slice_url(const char *url)
{
    size_t len = strlen(url);
    if (len == 0) {
        return false;
    }
    /* url.trim() != url : reject leading/trailing whitespace */
    if (url[0] == ' ' || url[0] == '\t' || url[0] == '\n' || url[0] == '\r' || url[0] == '\f' ||
        url[len - 1] == ' ' || url[len - 1] == '\t' || url[len - 1] == '\n' || url[len - 1] == '\r' ||
        url[len - 1] == '\f') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c < 0x20 || c == 0x7f || c == '\\') {
            return false;
        }
    }
    size_t delim = len;
    for (size_t i = 0; i < len; i++) {
        if (url[i] == '/' || url[i] == '?' || url[i] == '#') {
            delim = i;
            break;
        }
    }
    size_t colon = len;
    for (size_t i = 0; i < len; i++) {
        if (url[i] == ':') {
            colon = i;
            break;
        }
    }
    if (colon >= delim) {
        return true; /* no scheme before the first path delimiter: relative */
    }
    char scheme[16];
    ascii_lower(url, colon, scheme, sizeof scheme);
    const char *rest = url + colon + 1;
    if (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0 || strcmp(scheme, "ftp") == 0 ||
        strcmp(scheme, "ftps") == 0) {
        if (strncmp(rest, "//", 2) != 0) {
            return false;
        }
        const char *host = rest + 2;
        return host[0] != '\0' && host[0] != '/';
    }
    if (strcmp(scheme, "mailto") == 0 || strcmp(scheme, "tel") == 0) {
        return rest[0] != '\0';
    }
    return false;
}

/* asset_path_components: split both separators, rejecting unsafe components. Writes nothing; just
 * validates. On error fills err and returns non-OK with the reference's message. */
static pc_status asset_path_components(const char *relative, pc_error *err)
{
    size_t len = strlen(relative);
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || relative[i] == '/' || relative[i] == '\\') {
            size_t plen = i - start;
            const char *part = relative + start;
            bool bad = plen == 0 || (plen == 1 && part[0] == '.') ||
                       (plen == 2 && part[0] == '.' && part[1] == '.') || part[plen - 1 + 0] == '\0';
            if (!bad) {
                if (part[plen - 1] == '.' || part[plen - 1] == ' ') {
                    bad = true;
                }
                for (size_t j = 0; j < plen && !bad; j++) {
                    if (part[j] == ':' || part[j] == '\0') {
                        bad = true;
                    }
                }
                if (!bad && is_windows_device_asset_name(part, plen)) {
                    bad = true;
                }
            }
            if (bad) {
                return pc_engine_err_other(err, "asset path \"%s\" has an unsafe component \"%.*s\"", relative,
                                           (int)plen, part);
            }
            start = i + 1;
        }
    }
    return PC_OK;
}

/* web_images_folder: keep the image folder a relative folder name below the export directory. */
static pc_status web_images_folder(const char *images, const char *cmd, char **out, pc_error *err)
{
    *out = NULL;
    if (images == NULL || images[0] == '\0') {
        *out = calloc(1, 1);
        return *out != NULL ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    size_t len = strlen(images);
    while (len > 0 && (images[len - 1] == '/' || images[len - 1] == '\\')) {
        len--;
    }
    if (len == 0) {
        return pc_engine_err_bad_params(err, cmd, "imagesFolder must be a relative folder name");
    }
    char *trimmed = malloc(len + 1);
    if (trimmed == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    memcpy(trimmed, images, len);
    trimmed[len] = '\0';
    pc_error inner = {0};
    if (asset_path_components(trimmed, &inner) != PC_OK) {
        pc_status rc = pc_engine_err_bad_params(err, cmd, "imagesFolder: %s", inner.msg);
        free(trimmed);
        return rc;
    }
    /* components already '/'-joined logical form; normalise '\\' to '/'. */
    for (size_t i = 0; i < len; i++) {
        if (trimmed[i] == '\\') {
            trimmed[i] = '/';
        }
    }
    *out = trimmed;
    return PC_OK;
}

static pc_rect scale_rect(pc_rect r, double sx, double sy)
{
    return pc_rect_make((int32_t)round((double)r.x0 * sx), (int32_t)round((double)r.y0 * sy),
                        (int32_t)round((double)r.x1 * sx), (int32_t)round((double)r.y1 * sy));
}

/* ---- the Save for Web impl ---------------------------------------------------------------- */

/* A platform writer: native files or browser downloads. */
typedef pc_status (*web_writer)(void *ctx, const char *path, const uint8_t *data, size_t len, pc_error *err);

static pc_status native_writer(void *ctx, const char *path, const uint8_t *data, size_t len, pc_error *err)
{
    (void)ctx;
    return pc_engine_write_file(path, data, len, err);
}

/* web_output_path (native): resolve a generated file below `dir`, symlink-hardened. Returns an
 * owned actual path. */
static pc_status web_output_path(const char *dir, const char *relative, char **out, pc_error *err);

/* web_derived_path: the path handed to the writer (native filesystem path, or logical download
 * name). Returns owned string. */
static pc_status web_derived_path(const char *dir, const char *relative, bool native, char **out,
                                  pc_error *err)
{
    if (native) {
        return web_output_path(dir, relative, out, err);
    }
    pc_status rc = asset_path_components(relative, err);
    if (rc != PC_OK) {
        return rc;
    }
    *out = pc_engine_file_join(dir, relative);
    return *out != NULL ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "out of memory");
}

/* A case-insensitive used-name set (small; linear). */
typedef struct {
    char **names;
    size_t n, cap;
} name_set;

static bool name_set_insert(name_set *s, const char *lower)
{
    for (size_t i = 0; i < s->n; i++) {
        if (strcmp(s->names[i], lower) == 0) {
            return false;
        }
    }
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 8;
        char **g = realloc(s->names, nc * sizeof *g);
        if (g == NULL) {
            return false; /* treat OOM as "already used" -> caller bumps suffix; bounded */
        }
        s->names = g;
        s->cap = nc;
    }
    s->names[s->n] = strdup(lower);
    if (s->names[s->n] == NULL) {
        return false;
    }
    s->n++;
    return true;
}

static void name_set_fini(name_set *s)
{
    for (size_t i = 0; i < s->n; i++) {
        free(s->names[i]);
    }
    free(s->names);
}

/* unique(stem, ext): "<stem>.<ext>", disambiguated case-insensitively. Returns owned name. */
static char *unique_name(name_set *used, const char *stem, const char *ext)
{
    size_t cap = strlen(stem) + strlen(ext) + 24;
    char *name = malloc(cap);
    char *low = malloc(cap);
    if (name == NULL || low == NULL) {
        free(name);
        free(low);
        return NULL;
    }
    snprintf(name, cap, "%s.%s", stem, ext);
    uint32_t k = 2;
    for (;;) {
        for (size_t i = 0; name[i] != '\0'; i++) {
            char c = name[i];
            low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            low[i + 1] = '\0';
        }
        if (name_set_insert(used, low)) {
            break;
        }
        snprintf(name, cap, "%s_%u.%s", stem, k, ext);
        if (k != UINT32_MAX) {
            k++;
        }
    }
    free(low);
    return name;
}

/* z-order of a resolved slice: position in the stored list, or -1 (auto, lowest). */
static long slice_z(const pc_document *doc, const pc_resolved_slice *r)
{
    if (!r->has_id) {
        return -1;
    }
    for (size_t i = 0; i < doc->slices.len; i++) {
        if (doc->slices.list[i].id == r->id) {
            return (long)i;
        }
    }
    return -1;
}

/* A collected HTML table cell. */
typedef struct {
    pc_rect rect;
    char *attrs; /* owned */
    char *body;  /* owned */
} html_cell;

/* html_table: the HTML table for cells over a w*h image (ported from web_cmds::html_table). */
static pc_status html_table(const char *title, uint32_t w, uint32_t h, const html_cell *cells, size_t ncells,
                            const char *spacer, pc_buf *out, pc_error *err)
{
    /* distinct x / y edges */
    int32_t *xs = malloc((ncells * 2 + 1) * sizeof *xs);
    int32_t *ys = malloc((ncells * 2 + 1) * sizeof *ys);
    if ((ncells > 0 && (xs == NULL || ys == NULL))) {
        free(xs);
        free(ys);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    size_t nx = 0, ny = 0;
    for (size_t i = 0; i < ncells; i++) {
        xs[nx++] = cells[i].rect.x0;
        xs[nx++] = cells[i].rect.x1;
        ys[ny++] = cells[i].rect.y0;
        ys[ny++] = cells[i].rect.y1;
    }
    /* sort + dedup */
    qsort(xs, nx, sizeof *xs, cmp_i32);
    qsort(ys, ny, sizeof *ys, cmp_i32);
    size_t ux = 0;
    for (size_t i = 0; i < nx; i++) {
        if (ux == 0 || xs[i] != xs[ux - 1]) {
            xs[ux++] = xs[i];
        }
    }
    size_t uy = 0;
    for (size_t i = 0; i < ny; i++) {
        if (uy == 0 || ys[i] != ys[uy - 1]) {
            ys[uy++] = ys[i];
        }
    }
    nx = ux;
    ny = uy;
    size_t nc = nx > 0 ? nx - 1 : 0;
    size_t nr = ny > 0 ? ny - 1 : 0;
    bool *taken = nc * nr > 0 ? calloc(nc * nr, sizeof *taken) : NULL;
    /* by_row: a list of (c0, td-string) per row */
    pc_buf *by_row = nr > 0 ? calloc(nr, sizeof *by_row) : NULL;
    size_t *rowcols_cap = nr > 0 ? calloc(nr, sizeof *rowcols_cap) : NULL;
    /* We append tds in c0 order directly by first collecting then sorting; simpler: store each
     * cell's (row, c0, string) and emit sorted per row. */
    (void)rowcols_cap;
    pc_status rc = PC_OK;
    if ((nc * nr > 0 && taken == NULL) || (nr > 0 && by_row == NULL)) {
        rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto done;
    }

    /* Collect td strings with their (row, col) for ordering. */
    struct td_item {
        size_t row;
        size_t col;
        char *td;
    } *items = ncells > 0 ? calloc(ncells, sizeof *items) : NULL;
    size_t nitems = 0;
    if (ncells > 0 && items == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto done;
    }
#define EDGE_INDEX(arr, len, val, dst)                                                                      \
    do {                                                                                                    \
        (dst) = 0;                                                                                          \
        for (size_t _i = 0; _i < (len); _i++) {                                                             \
            if ((arr)[_i] == (val)) {                                                                       \
                (dst) = _i;                                                                                 \
                break;                                                                                      \
            }                                                                                               \
        }                                                                                                   \
    } while (0)
    for (size_t i = 0; i < ncells; i++) {
        size_t c0, c1, r0, r1;
        EDGE_INDEX(xs, nx, cells[i].rect.x0, c0);
        EDGE_INDEX(xs, nx, cells[i].rect.x1, c1);
        EDGE_INDEX(ys, ny, cells[i].rect.y0, r0);
        EDGE_INDEX(ys, ny, cells[i].rect.y1, r1);
        bool overlap = false;
        for (size_t j = r0; j < r1 && !overlap; j++) {
            for (size_t ii = c0; ii < c1; ii++) {
                if (taken[j * nc + ii]) {
                    overlap = true;
                    break;
                }
            }
        }
        if (overlap) {
            continue;
        }
        for (size_t j = r0; j < r1; j++) {
            for (size_t ii = c0; ii < c1; ii++) {
                taken[j * nc + ii] = true;
            }
        }
        pc_buf td = PC_BUF_INIT;
        rc = pc_buf_append_str(&td, "\t\t<td", err);
        if (rc == PC_OK && c1 - c0 > 1) {
            rc = pc_buf_appendf(&td, err, " colspan=\"%zu\"", c1 - c0);
        }
        if (rc == PC_OK && r1 - r0 > 1) {
            rc = pc_buf_appendf(&td, err, " rowspan=\"%zu\"", r1 - r0);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append_str(&td, cells[i].attrs != NULL ? cells[i].attrs : "", err);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append_byte(&td, '>', err);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append_str(&td, cells[i].body != NULL ? cells[i].body : "", err);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append_str(&td, "</td>\n", err);
        }
        if (rc != PC_OK) {
            pc_buf_fini(&td);
            goto done_items;
        }
        items[nitems].row = r0;
        items[nitems].col = c0;
        items[nitems].td = strdup(pc_buf_cstr(&td));
        pc_buf_fini(&td);
        if (items[nitems].td == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto done_items;
        }
        nitems++;
    }
#undef EDGE_INDEX

    rc = pc_buf_appendf(out, err,
                        "<html>\n<head>\n<title>");
    if (rc == PC_OK) {
        rc = html_escape_append(out, title, err);
    }
    if (rc == PC_OK) {
        rc = pc_buf_append_str(
            out,
            "</title>\n<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n</head>\n<body "
            "bgcolor=\"#FFFFFF\" leftmargin=\"0\" topmargin=\"0\" marginwidth=\"0\" marginheight=\"0\">\n<!-- "
            "Save for Web Slices (",
            err);
    }
    if (rc == PC_OK) {
        rc = html_escape_append(out, title, err);
    }
    if (rc == PC_OK) {
        rc = pc_buf_appendf(out, err,
                            ") -->\n<table id=\"Table_01\" width=\"%u\" height=\"%u\" border=\"0\" "
                            "cellpadding=\"0\" cellspacing=\"0\">\n",
                            w, h);
    }
    for (size_t j = 0; j < nr && rc == PC_OK; j++) {
        rc = pc_buf_append_str(out, "\t<tr>\n", err);
        /* emit items of row j sorted by col */
        for (;;) {
            size_t best = SIZE_MAX;
            for (size_t i = 0; i < nitems; i++) {
                if (items[i].td != NULL && items[i].row == j) {
                    if (best == SIZE_MAX || items[i].col < items[best].col) {
                        best = i;
                    }
                }
            }
            if (best == SIZE_MAX) {
                break;
            }
            if (rc == PC_OK) {
                rc = pc_buf_append_str(out, items[best].td, err);
            }
            free(items[best].td);
            items[best].td = NULL;
        }
        if (rc == PC_OK && spacer != NULL) {
            rc = pc_buf_append_str(out, "\t\t<td>\n\t\t\t<img src=\"", err);
            if (rc == PC_OK) {
                rc = html_escape_append(out, spacer, err);
            }
            if (rc == PC_OK) {
                rc = pc_buf_appendf(out, err, "\" width=\"1\" height=\"%d\" alt=\"\"></td>\n",
                                    ys[j + 1] - ys[j]);
            }
        }
        if (rc == PC_OK) {
            rc = pc_buf_append_str(out, "\t</tr>\n", err);
        }
    }
    if (rc == PC_OK && spacer != NULL) {
        rc = pc_buf_append_str(out, "\t<tr>\n", err);
        for (size_t i = 0; i < nc && rc == PC_OK; i++) {
            rc = pc_buf_append_str(out, "\t\t<td>\n\t\t\t<img src=\"", err);
            if (rc == PC_OK) {
                rc = html_escape_append(out, spacer, err);
            }
            if (rc == PC_OK) {
                rc = pc_buf_appendf(out, err, "\" width=\"%d\" height=\"1\" alt=\"\"></td>\n",
                                    xs[i + 1] - xs[i]);
            }
        }
        if (rc == PC_OK) {
            rc = pc_buf_append_str(out, "\t\t<td></td>\n\t</tr>\n", err);
        }
    }
    if (rc == PC_OK) {
        rc = pc_buf_append_str(out, "</table>\n<!-- End Save for Web Slices -->\n</body>\n</html>\n", err);
    }

done_items:
    for (size_t i = 0; i < nitems; i++) {
        free(items[i].td);
    }
    free(items);
done:
    free(taken);
    free(by_row);
    free(rowcols_cap);
    free(xs);
    free(ys);
    return rc;
}

/* The document's base file name (stem) for generated output; owned. */
static char *web_base_name(const pc_document *doc)
{
    pc_str bn = {0};
    if (pc_slices_base_name(doc, &bn, NULL) != PC_OK) {
        return portable_generated_stem("Untitled");
    }
    char *r = portable_generated_stem(pc_str_c(&bn));
    pc_str_fini(&bn);
    return r;
}

/* Create `path` and its ancestors (mkdir -p). */
static pc_status mkdir_p(const char *path, pc_error *err)
{
    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (tmp == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    memcpy(tmp, path, len + 1);
    for (size_t i = 1; i <= len; i++) {
        if (i == len || tmp[i] == '/') {
            char save = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0' && mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                pc_status rc = pc_engine_err_other(err, "asset output directory \"%s\": %s", path,
                                                   strerror(errno));
                free(tmp);
                return rc;
            }
            tmp[i] = save;
        }
    }
    free(tmp);
    return PC_OK;
}

/* asset_output_path: resolve a generated file below `dir` (symlink-hardened). *out_actual is the
 * canonical write path (owned). Port of web_cmds::asset_output_path, confinement only. */
static pc_status asset_output_path(const char *dir, const char *relative, char **out_actual, pc_error *err)
{
    *out_actual = NULL;
    if (dir == NULL || dir[0] == '\0') {
        return pc_engine_err_other(err, "image assets need an output directory");
    }
    pc_status rc = asset_path_components(relative, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = mkdir_p(dir, err);
    if (rc != PC_OK) {
        return rc;
    }
    char *root = realpath(dir, NULL);
    if (root == NULL) {
        return pc_engine_err_other(err, "asset output directory \"%s\": %s", dir, strerror(errno));
    }
    size_t rootlen = strlen(root);
    /* Split relative into parts (normalising both separators). */
    char *rel = strdup(relative);
    if (rel == NULL) {
        free(root);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (char *q = rel; *q != '\0'; q++) {
        if (*q == '\\') {
            *q = '/';
        }
    }
    char *parent = strdup(root);
    if (parent == NULL) {
        free(root);
        free(rel);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    char *save = NULL;
    char *part = strtok_r(rel, "/", &save);
    char *filepart = NULL;
    while (part != NULL) {
        char *next = strtok_r(NULL, "/", &save);
        if (next == NULL) {
            filepart = part; /* the final component is the file */
            break;
        }
        /* a directory component */
        char *child = pc_engine_file_join(parent, part);
        if (child == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto fail;
        }
        struct stat sb;
        if (lstat(child, &sb) == 0) {
            if (S_ISLNK(sb.st_mode)) {
                rc = pc_engine_err_other(err, "asset path \"%s\" crosses a symlink at %s", relative, child);
                free(child);
                goto fail;
            }
            if (!S_ISDIR(sb.st_mode)) {
                rc = pc_engine_err_other(err, "asset path \"%s\" has a non-directory at %s", relative, child);
                free(child);
                goto fail;
            }
        } else if (errno == ENOENT) {
            if (mkdir(child, 0777) != 0 && errno != EEXIST) {
                rc = pc_engine_err_other(err, "asset folder %s: %s", child, strerror(errno));
                free(child);
                goto fail;
            }
        } else {
            rc = pc_engine_err_other(err, "asset folder %s: %s", child, strerror(errno));
            free(child);
            goto fail;
        }
        char *canon = realpath(child, NULL);
        free(child);
        if (canon == NULL) {
            rc = pc_engine_err_other(err, "asset folder: %s", strerror(errno));
            goto fail;
        }
        if (strncmp(canon, root, rootlen) != 0 || (canon[rootlen] != '\0' && canon[rootlen] != '/')) {
            free(canon);
            rc = pc_engine_err_other(err, "asset path \"%s\" escapes output directory \"%s\"", relative, dir);
            goto fail;
        }
        free(parent);
        parent = canon;
        part = next;
    }
    if (filepart == NULL) {
        rc = pc_engine_err_other(err, "asset path \"%s\" is empty", relative);
        goto fail;
    }
    char *output = pc_engine_file_join(parent, filepart);
    if (output == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto fail;
    }
    struct stat sb;
    if (lstat(output, &sb) == 0) {
        if (S_ISLNK(sb.st_mode)) {
            rc = pc_engine_err_other(err, "asset path \"%s\" points to a symlink", relative);
            free(output);
            goto fail;
        }
        if (!S_ISREG(sb.st_mode)) {
            rc = pc_engine_err_other(err, "asset path \"%s\" is not a file", relative);
            free(output);
            goto fail;
        }
    } else if (errno != ENOENT) {
        rc = pc_engine_err_other(err, "asset path \"%s\": %s", relative, strerror(errno));
        free(output);
        goto fail;
    }
    *out_actual = output;
    rc = PC_OK;
fail:
    free(parent);
    free(root);
    free(rel);
    return rc;
}

static pc_status web_output_path(const char *dir, const char *relative, char **out, pc_error *err)
{
    return asset_output_path((dir == NULL || dir[0] == '\0') ? "." : dir, relative, out, err);
}

/* remember_web_settings: seed next launch's dialog from the preferences (strip the per-export
 * destination keys). Port of web_cmds::remember_web_settings. */
static void web_remember(pc_engine_session *s, const char *cmd, yyjson_val *p)
{
    /* last_web keeps the full params (minus trustedSliceHtml). */
    pc_json_value lw = PC_JSON_VALUE_INIT;
    if (pc_json_value_from_val(p, &lw, NULL) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&lw);
        if (root != NULL && yyjson_mut_is_obj(root)) {
            (void)yyjson_mut_obj_remove_str(root, "trustedSliceHtml");
        }
        pc_json_value_fini(&s->last_web);
        s->last_web = lw;
    }
    /* prefs.dialogs[cmd] = params without path/dir/trustedSliceHtml. */
    if (s->prefs.values == NULL) {
        return;
    }
    yyjson_mut_doc *pd = s->prefs.values;
    yyjson_mut_val *proot = yyjson_mut_doc_get_root(pd);
    if (proot == NULL || !yyjson_mut_is_obj(proot)) {
        return;
    }
    yyjson_mut_val *dialogs = yyjson_mut_obj_get(proot, "dialogs");
    if (dialogs == NULL || !yyjson_mut_is_obj(dialogs)) {
        dialogs = yyjson_mut_obj(pd);
        if (dialogs == NULL) {
            return;
        }
        yyjson_mut_obj_add(proot, yyjson_mut_strcpy(pd, "dialogs"), dialogs);
    }
    yyjson_mut_val *m = yyjson_val_mut_copy(pd, p);
    if (m == NULL || !yyjson_mut_is_obj(m)) {
        return;
    }
    (void)yyjson_mut_obj_remove_str(m, "path");
    (void)yyjson_mut_obj_remove_str(m, "dir");
    (void)yyjson_mut_obj_remove_str(m, "trustedSliceHtml");
    (void)yyjson_mut_obj_remove_str(dialogs, cmd);
    yyjson_mut_obj_add(dialogs, yyjson_mut_strcpy(pd, cmd), m);
}

/* Directory + slices export (one file per chosen slice, optional HTML table). */
static pc_status save_for_web_dir(pc_engine_session *s, yyjson_val *p, const char *cmd, web_writer write,
                                  void *wctx, bool native_paths, const pc_document *doc,
                                  const pc_document *wdoc, const pc_compose_buffer *buf,
                                  const pc_web_settings *st, const uint8_t *icc, size_t icc_len,
                                  const char *xmp, size_t xmp_len, float dpi, double sx, double sy,
                                  const pc_resolved_slice *all, size_t nall, const char *single,
                                  size_t single_len, const char *dir_in, size_t dir_len, pc_json_value *out,
                                  pc_error *err);

/* The core: estimate, single-file, or dir+slices export. */
static pc_status save_for_web_impl(pc_engine_session *s, yyjson_val *p, web_writer write, void *wctx,
                                   bool native_paths, pc_json_value *out, pc_error *err)
{
    const char *cmd = "file.export.saveForWebLegacy";
    if (s->authorize != NULL) {
        pc_status arc = s->authorize(cmd, p, err);
        if (arc != PC_OK) {
            return arc;
        }
    }
    bool handled = false;
    pc_json_value fo = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_hook_float_before_command(s, cmd, &handled, &fo, err);
    pc_json_value_fini(&fo);
    if (rc != PC_OK) {
        return rc;
    }

    pc_web_settings st;
    rc = pc_web_settings_from_params(p, cmd, &st, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        pc_web_settings_fini(&st);
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);

    pc_document wdoc;
    memset(&wdoc, 0, sizeof wdoc);
    double sx = 1.0, sy = 1.0;
    rc = web_document(doc, p, &st, &wdoc, &sx, &sy, err);
    if (rc != PC_OK) {
        pc_web_settings_fini(&st);
        return rc;
    }
    pc_compose_buffer buf = {0};
    rc = pc_compose_flatten(&wdoc, &buf, err);
    if (rc != PC_OK) {
        pc_document_fini(&wdoc);
        pc_web_settings_fini(&st);
        return rc;
    }
    size_t bw = wdoc.size.width;
    pc_rect wbounds = pc_document_bounds(&wdoc);
    const uint8_t *icc = wdoc.icc_profile != NULL ? pc_blob_data(wdoc.icc_profile) : NULL;
    size_t icc_len = wdoc.icc_profile != NULL ? pc_blob_len(wdoc.icc_profile) : 0;
    float dpi = wdoc.resolution_dpi;
    char *xmp = NULL;
    const char *dxmp = doc->metadata.has_xmp ? pc_str_c(&doc->metadata.xmp) : NULL;
    size_t dxl = doc->metadata.has_xmp ? doc->metadata.xmp.len : 0;
    int meta_mode = st.metadata == PC_WEB_META_NONE
                        ? 0
                        : (st.metadata == PC_WEB_META_COPYRIGHT ? 1 : (st.metadata == PC_WEB_META_CONTACT ? 2 : 3));
    (void)pc_engine_web_xmp(dxmp, dxl, meta_mode, &xmp, NULL);
    size_t xmp_len = xmp != NULL ? strlen(xmp) : 0;

    /* Note: buffer must cover the document from the origin (flatten does). */
    if (buf.rect.x0 != 0 || buf.rect.y0 != 0) {
        rc = pc_engine_err_other(err, "slice is outside the pixel buffer");
    }

    /* The chosen slices. */
    pc_resolved_slice *all = NULL;
    size_t nall = 0;
    if (rc == PC_OK) {
        rc = pc_slices_resolve(doc, &all, &nall, err);
    }

    size_t plen = 0;
    const char *single = jstr(p, "path", &plen);
    if (single != NULL && plen == 0) {
        single = NULL;
    }
    size_t dlen = 0;
    const char *dir_in = jstr(p, "dir", &dlen);
    if (dir_in != NULL && dlen == 0) {
        dir_in = NULL;
    }
    bool has_slices_key = pc_json_get(p, "slices") != NULL;

    if (rc != PC_OK) {
        goto cleanup;
    }

    /* Estimate only. */
    if (single == NULL && dir_in == NULL) {
        pc_web_optimized o;
        rc = pc_web_optimize((const float (*)[4])buf.px, bw, wbounds, &st, icc, icc_len, xmp, xmp_len, dpi, false, &o, err);
        if (rc == PC_OK) {
            yyjson_mut_doc *rd = NULL;
            if (pc_engine_result_doc(out, &rd, err) == PC_OK) {
                yyjson_mut_val *root = yyjson_mut_obj(rd);
                yyjson_mut_obj_add_str(rd, root, "format", o.ext);
                yyjson_mut_obj_add_uint(rd, root, "bytes", o.bytes.len);
                yyjson_mut_obj_add_uint(rd, root, "width", o.width);
                yyjson_mut_obj_add_uint(rd, root, "height", o.height);
                if (o.has_colors) {
                    yyjson_mut_obj_add_uint(rd, root, "colors", o.colors);
                } else {
                    yyjson_mut_obj_add_null(rd, root, "colors");
                }
                yyjson_mut_val *ct = yyjson_mut_arr(rd);
                for (size_t i = 0; i < o.ncolor_table; i++) {
                    char hx[8];
                    snprintf(hx, sizeof hx, "#%02x%02x%02x", o.color_table[i][0], o.color_table[i][1],
                             o.color_table[i][2]);
                    yyjson_mut_arr_add_strcpy(rd, ct, hx);
                }
                yyjson_mut_obj_add_val(rd, root, "colorTable", ct);
                if (o.has_transparent) {
                    yyjson_mut_obj_add_uint(rd, root, "transparentIndex", o.transparent_index);
                } else {
                    yyjson_mut_obj_add_null(rd, root, "transparentIndex");
                }
                yyjson_mut_doc_set_root(rd, root);
            } else {
                rc = PC_ERR_NOMEM;
            }
            pc_web_optimized_fini(&o);
        }
        goto cleanup;
    }

    /* Single-file export (whole image). */
    if (single != NULL && dir_in == NULL && (nall == 1 || !has_slices_key)) {
        char *path = strndup(single, plen);
        pc_web_optimized o;
        rc = path != NULL
                 ? pc_web_optimize((const float (*)[4])buf.px, bw, wbounds, &st, icc, icc_len, xmp, xmp_len, dpi, false, &o, err)
                 : pc_fail(err, PC_ERR_NOMEM, "out of memory");
        if (rc == PC_OK) {
            rc = write(wctx, path, o.bytes.data, o.bytes.len, err);
            if (rc == PC_OK) {
                /* remember + last_web + fire export */
                web_remember(s, cmd, p);
                (void)pc_engine_automate_fire_event(s, "export");
                yyjson_mut_doc *rd = NULL;
                if (pc_engine_result_doc(out, &rd, err) == PC_OK) {
                    yyjson_mut_val *root = yyjson_mut_obj(rd);
                    yyjson_mut_val *files = yyjson_mut_arr(rd);
                    yyjson_mut_arr_add_strcpy(rd, files, path);
                    yyjson_mut_obj_add_val(rd, root, "files", files);
                    yyjson_mut_obj_add_uint(rd, root, "bytes", o.bytes.len);
                    yyjson_mut_obj_add_uint(rd, root, "width", o.width);
                    yyjson_mut_obj_add_uint(rd, root, "height", o.height);
                    if (o.has_colors) {
                        yyjson_mut_obj_add_uint(rd, root, "colors", o.colors);
                    } else {
                        yyjson_mut_obj_add_null(rd, root, "colors");
                    }
                    yyjson_mut_doc_set_root(rd, root);
                } else {
                    rc = PC_ERR_NOMEM;
                }
            }
            pc_web_optimized_fini(&o);
        }
        free(path);
        goto cleanup;
    }

    /* Directory + slices export. */
    rc = save_for_web_dir(s, p, cmd, write, wctx, native_paths, doc, &wdoc, &buf, &st, icc, icc_len, xmp,
                          xmp_len, dpi, sx, sy, all, nall, single, plen, dir_in, dlen, out, err);

cleanup:
    pc_resolved_slices_free(all, nall);
    free(xmp);
    pc_compose_buffer_fini(&buf);
    pc_document_fini(&wdoc);
    pc_web_settings_fini(&st);
    return rc;
}

/* ---- directory + slices export ------------------------------------------------------------ */

static bool slice_chosen(yyjson_val *p, const char *pick, const pc_resolved_slice *r)
{
    yyjson_val *numbers = pc_json_get(p, "numbers");
    if (numbers != NULL && yyjson_is_arr(numbers)) {
        yyjson_val *v;
        yyjson_arr_iter it;
        yyjson_arr_iter_init(numbers, &it);
        while ((v = yyjson_arr_iter_next(&it)) != NULL) {
            uint64_t n = 0;
            if (pc_json_as_u64(v, &n) && n == (uint64_t)r->number) {
                return true;
            }
        }
        return false;
    }
    if (strcmp(pick, "user") == 0) {
        return r->origin != PC_SLICE_AUTO;
    }
    return true;
}

static pc_status save_for_web_dir(pc_engine_session *s, yyjson_val *p, const char *cmd, web_writer write,
                                  void *wctx, bool native_paths, const pc_document *doc,
                                  const pc_document *wdoc, const pc_compose_buffer *buf,
                                  const pc_web_settings *st, const uint8_t *icc, size_t icc_len,
                                  const char *xmp, size_t xmp_len, float dpi, double sx, double sy,
                                  const pc_resolved_slice *all, size_t nall, const char *single,
                                  size_t single_len, const char *dir_in, size_t dir_len, pc_json_value *out,
                                  pc_error *err)
{
    size_t bw = wdoc->size.width;
    pc_rect wbounds = pc_document_bounds(wdoc);
    pc_status rc = PC_OK;

    /* dir: explicit, else the single path's parent, else "". */
    char *dir = NULL;
    if (dir_in != NULL) {
        dir = strndup(dir_in, dir_len);
    } else if (single != NULL) {
        const char *slash = NULL;
        for (size_t i = 0; i < single_len; i++) {
            if (single[i] == '/') {
                slash = single + i;
            }
        }
        dir = slash != NULL ? strndup(single, (size_t)(slash - single)) : strdup("");
    } else {
        dir = strdup("");
    }
    if (dir == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }

    bool html = false;
    (void)pc_json_as_bool(pc_json_get(p, "html"), &html);
    size_t ifl = 0;
    const char *images_in = jstr(p, "imagesFolder", &ifl);
    char *images = NULL;
    rc = web_images_folder(images_in != NULL ? images_in : "images", cmd, &images, err);
    if (rc != PC_OK) {
        free(dir);
        return rc;
    }
    bool trusted = false;
    (void)pc_json_as_bool(pc_json_get(p, "trustedSliceHtml"), &trusted);
    char *base = NULL;
    if (single != NULL) {
        char stem[512];
        char *sp = strndup(single, single_len);
        pc_engine_file_stem(sp != NULL ? sp : "", stem, sizeof stem);
        free(sp);
        base = portable_generated_stem(stem);
    } else {
        base = web_base_name(doc);
    }
    const char *pick = "all";
    size_t pl = 0;
    const char *ps = jstr(p, "slices", &pl);
    char pickbuf[16];
    if (ps != NULL) {
        ascii_lower(ps, pl, pickbuf, sizeof pickbuf);
        pick = pickbuf;
    }

    /* chosen = filtered resolved slices (pointers into all). */
    const pc_resolved_slice **chosen = malloc((nall > 0 ? nall : 1) * sizeof *chosen);
    if (images == NULL || base == NULL || chosen == NULL) {
        free(dir);
        free(images);
        free(base);
        free(chosen);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    size_t nchosen = 0;
    for (size_t i = 0; i < nall; i++) {
        if (slice_chosen(p, pick, &all[i])) {
            chosen[nchosen++] = &all[i];
        }
    }

    html_cell *cells = NULL;
    size_t ncells = 0, cells_cap = 0;
    yyjson_mut_doc *rd = NULL;
    yyjson_mut_val *files = NULL;
    char *html_logical = NULL;
    size_t total = 0;
    name_set used = {0};
    if (html) {
        (void)name_set_insert(&used, "spacer.gif");
    }
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
        goto cleanup;
    }
    files = yyjson_mut_arr(rd);

    for (size_t ci = 0; ci < nchosen && rc == PC_OK; ci++) {
        const pc_resolved_slice *r = chosen[ci];
        pc_rect rect = pc_rect_intersect(scale_rect(r->rect, sx, sy), wbounds);
        if (pc_rect_is_empty(rect)) {
            continue;
        }
        pc_rect *pieces = NULL;
        size_t npieces = 0;
        if (html) {
            long zr = slice_z(doc, r);
            pc_rect *above = malloc((nchosen > 0 ? nchosen : 1) * sizeof *above);
            size_t nabove = 0;
            if (above == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                break;
            }
            for (size_t oi = 0; oi < nchosen; oi++) {
                if (slice_z(doc, chosen[oi]) > zr) {
                    pc_rect ar = pc_rect_intersect(scale_rect(chosen[oi]->rect, sx, sy), wbounds);
                    above[nabove++] = ar;
                }
            }
            rc = pc_auto_slices(rect, above, nabove, &pieces, &npieces, err);
            free(above);
            if (rc != PC_OK) {
                break;
            }
        } else {
            pieces = malloc(sizeof *pieces);
            if (pieces == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                break;
            }
            pieces[0] = rect;
            npieces = 1;
        }
        bool split = npieces > 1;
        const pc_slice *stored = r->has_id ? pc_slices_get(&doc->slices, r->id) : NULL;
        for (size_t k = 0; k < npieces && rc == PC_OK; k++) {
            pc_rect piece = pieces[k];
            if (r->kind == PC_SLICE_NO_IMAGE) {
                /* a no-image cell: text + background colour. */
                pc_buf text = PC_BUF_INIT;
                if (k == 0 && stored != NULL) {
                    const char *ct = pc_str_c(&stored->cell_text);
                    if (trusted && stored->cell_text_is_html) {
                        (void)pc_buf_append_str(&text, ct, err);
                    } else {
                        (void)html_escape_append(&text, ct, err);
                    }
                }
                char *attrs = NULL;
                if (stored != NULL && stored->has_background) {
                    char bg[40];
                    snprintf(bg, sizeof bg, " bgcolor=\"#%02X%02X%02X\"", stored->background[1],
                             stored->background[2], stored->background[3]);
                    attrs = strdup(bg);
                }
                if (ncells == cells_cap) {
                    size_t nc = cells_cap ? cells_cap * 2 : 8;
                    html_cell *g = realloc(cells, nc * sizeof *g);
                    if (g == NULL) {
                        pc_buf_fini(&text);
                        free(attrs);
                        rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                        break;
                    }
                    cells = g;
                    cells_cap = nc;
                }
                cells[ncells].rect = piece;
                cells[ncells].attrs = attrs;
                cells[ncells].body = strdup(pc_buf_cstr(&text));
                pc_buf_fini(&text);
                if (cells[ncells].body == NULL) {
                    free(attrs);
                    rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                    break;
                }
                ncells++;
                continue;
            }
            pc_web_optimized o;
            rc = pc_web_optimize((const float (*)[4])buf->px, bw, piece, st, icc, icc_len, xmp, xmp_len, dpi, false, &o, err);
            if (rc != PC_OK) {
                break;
            }
            char *sf = slice_file(pc_str_c(&r->name));
            char *stem = NULL;
            if (sf != NULL) {
                if (split) {
                    size_t cap = strlen(sf) + 8;
                    stem = malloc(cap);
                    if (stem != NULL) {
                        snprintf(stem, cap, "%s_%02zu", sf, k + 1);
                    }
                } else {
                    stem = strdup(sf);
                }
            }
            char *name = stem != NULL ? unique_name(&used, stem, o.ext) : NULL;
            char *rel = NULL;
            if (name != NULL) {
                rel = images[0] != '\0' ? pc_engine_file_join(images, name) : strdup(name);
            }
            char *out_logical = rel != NULL ? pc_engine_file_join(dir, rel) : NULL;
            char *derived = NULL;
            if (out_logical != NULL) {
                rc = web_derived_path(dir, rel, native_paths, &derived, err);
            } else {
                rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            if (rc == PC_OK) {
                rc = write(wctx, derived, o.bytes.data, o.bytes.len, err);
            }
            if (rc == PC_OK) {
                total += o.bytes.len;
                yyjson_mut_arr_add_strcpy(rd, files, out_logical);
                /* the HTML cell for this image. */
                pc_buf body = PC_BUF_INIT;
                const char *url = stored != NULL ? pc_str_c(&stored->url) : "";
                bool linked = stored != NULL && safe_slice_url(url);
                if (linked) {
                    (void)pc_buf_append_str(&body, "\n\t\t\t<a href=\"", err);
                    (void)html_escape_append(&body, url, err);
                    (void)pc_buf_append_byte(&body, '"', err);
                    const char *target = pc_str_c(&stored->target);
                    if (target[0] != '\0') {
                        (void)pc_buf_append_str(&body, " target=\"", err);
                        (void)html_escape_append(&body, target, err);
                        (void)pc_buf_append_byte(&body, '"', err);
                    }
                    (void)pc_buf_append_byte(&body, '>', err);
                }
                (void)pc_buf_append_str(&body, "\n\t\t\t<img src=\"", err);
                (void)html_escape_append(&body, rel, err);
                (void)pc_buf_appendf(&body, err, "\" width=\"%u\" height=\"%u\" alt=\"",
                                     pc_rect_width(piece), pc_rect_height(piece));
                (void)html_escape_append(&body, stored != NULL ? pc_str_c(&stored->alt) : "", err);
                (void)pc_buf_append_str(&body, "\">", err);
                if (linked) {
                    (void)pc_buf_append_str(&body, "</a>", err);
                }
                if (ncells == cells_cap) {
                    size_t nc = cells_cap ? cells_cap * 2 : 8;
                    html_cell *g = realloc(cells, nc * sizeof *g);
                    if (g == NULL) {
                        pc_buf_fini(&body);
                        rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                    } else {
                        cells = g;
                        cells_cap = nc;
                    }
                }
                if (rc == PC_OK) {
                    cells[ncells].rect = piece;
                    cells[ncells].attrs = NULL;
                    cells[ncells].body = strdup(pc_buf_cstr(&body));
                    if (cells[ncells].body == NULL) {
                        rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                    } else {
                        ncells++;
                    }
                }
                pc_buf_fini(&body);
            }
            free(derived);
            free(out_logical);
            free(rel);
            free(name);
            free(stem);
            free(sf);
            pc_web_optimized_fini(&o);
        }
        free(pieces);
    }

    if (rc == PC_OK && html) {
        bool grid = ncells > 1;
        char *spacer = NULL;
        if (grid) {
            spacer = images[0] != '\0' ? pc_engine_file_join(images, "spacer.gif") : strdup("spacer.gif");
            if (spacer == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
        }
        if (rc == PC_OK && spacer != NULL) {
            uint8_t sidx = 0;
            uint8_t spal[1][3] = {{0, 0, 0}};
            pc_buf gif = PC_BUF_INIT;
            rc = pc_web_encode_gif_indexed(1, 1, &sidx, 1, (const uint8_t(*)[3])spal, 1, 0, false, &gif, err);
            char *derived = NULL;
            if (rc == PC_OK) {
                rc = web_derived_path(dir, spacer, native_paths, &derived, err);
            }
            if (rc == PC_OK) {
                rc = write(wctx, derived, gif.data, gif.len, err);
            }
            free(derived);
            pc_buf_fini(&gif);
        }
        if (rc == PC_OK) {
            pc_buf page = PC_BUF_INIT;
            rc = html_table(base, wdoc->size.width, wdoc->size.height, cells, ncells, spacer, &page, err);
            char *relhtml = NULL;
            if (rc == PC_OK) {
                size_t cap = strlen(base) + 6;
                relhtml = malloc(cap);
                if (relhtml != NULL) {
                    snprintf(relhtml, cap, "%s.html", base);
                }
            }
            char *derived = NULL;
            if (rc == PC_OK && relhtml != NULL) {
                html_logical = pc_engine_file_join(dir, relhtml);
                rc = web_derived_path(dir, relhtml, native_paths, &derived, err);
            } else if (rc == PC_OK) {
                rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            if (rc == PC_OK) {
                rc = write(wctx, derived, page.data, page.len, err);
            }
            free(derived);
            free(relhtml);
            pc_buf_fini(&page);
        }
        free(spacer);
    }

    if (rc == PC_OK) {
        web_remember(s, cmd, p);
        (void)pc_engine_automate_fire_event(s, "export");
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_val(rd, root, "files", files);
        if (html_logical != NULL) {
            yyjson_mut_obj_add_strcpy(rd, root, "html", html_logical);
        } else {
            yyjson_mut_obj_add_null(rd, root, "html");
        }
        yyjson_mut_obj_add_uint(rd, root, "bytes", total);
        yyjson_mut_obj_add_uint(rd, root, "slices", nchosen);
        yyjson_mut_obj_add_uint(rd, root, "width", wdoc->size.width);
        yyjson_mut_obj_add_uint(rd, root, "height", wdoc->size.height);
        yyjson_mut_doc_set_root(rd, root);
    }

cleanup:
    free(html_logical);
    for (size_t i = 0; i < ncells; i++) {
        free(cells[i].attrs);
        free(cells[i].body);
    }
    free(cells);
    name_set_fini(&used);
    free((void *)chosen);
    free(base);
    free(images);
    free(dir);
    return rc;
}

/* ---- Export Preferences / Quick Export ---------------------------------------------------- */

static pc_status run_export_prefs(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "file.export.exportPreferences";
    static const struct {
        const char *key;
        const char *pref;
    } MAP[] = {{"quickExportFormat", "export.quickExportFormat"},
               {"quickExportLocation", "export.quickExportLocation"},
               {"jpegQuality", "export.jpegQuality"},
               {"webpLossless", "export.webpLossless"},
               {"webpQuality", "export.webpQuality"},
               {"metadata", "export.metadata"},
               {"convertToSrgb", "export.convertToSrgb"}};
    yyjson_mut_doc *vd = yyjson_mut_doc_new(NULL);
    if (vd == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *values = yyjson_mut_obj(vd);
    bool any = false;
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
        yyjson_val *v = pc_json_get(p, MAP[i].key);
        if (v != NULL) {
            yyjson_mut_val *cp = yyjson_val_mut_copy(vd, v);
            if (cp != NULL) {
                yyjson_mut_obj_add(values, yyjson_mut_strcpy(vd, MAP[i].pref), cp);
                any = true;
            }
        }
    }
    pc_status rc = PC_OK;
    if (any) {
        yyjson_mut_val *wrap = yyjson_mut_obj(vd);
        yyjson_mut_obj_add_val(vd, wrap, "values", values);
        yyjson_mut_doc_set_root(vd, wrap);
        pc_buf txt = PC_BUF_INIT;
        if (pc_json_write(yyjson_mut_doc_get_root(vd), 0, &txt, err) == PC_OK) {
            pc_json_value so = PC_JSON_VALUE_INIT;
            pc_status src = pc_engine_session_execute_json(s, "prefs.set", pc_buf_cstr(&txt), &so, err);
            pc_json_value_fini(&so);
            if (src != PC_OK) {
                rc = pc_engine_err_bad_params(err, cmd, "%s", err != NULL ? err->msg : "prefs.set failed");
            }
        }
        pc_buf_fini(&txt);
    }
    yyjson_mut_doc_free(vd);
    if (rc != PC_OK) {
        return rc;
    }
    /* result: {section:"export", values: prefs.get("export")} */
    pc_json_value ev = PC_JSON_VALUE_INIT;
    (void)pc_engine_prefs_get(s, "export", &ev, NULL);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_json_value_fini(&ev);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_str(rd, root, "section", "export");
    yyjson_mut_val *evroot = pc_json_value_root(&ev);
    yyjson_mut_obj_add_val(rd, root, "values",
                           evroot != NULL ? yyjson_mut_val_mut_copy(rd, evroot) : yyjson_mut_null(rd));
    yyjson_mut_doc_set_root(rd, root);
    pc_json_value_fini(&ev);
    return PC_OK;
}

/* Read a string field of the export prefs section into buf (NUL-terminated). */
static void export_pref_str(pc_engine_session *s, const char *key, char *buf, size_t cap, const char *def)
{
    strncpy(buf, def, cap - 1);
    buf[cap - 1] = '\0';
    pc_json_value ev = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(s, "export", &ev, NULL) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&ev);
        yyjson_mut_val *v = root != NULL ? yyjson_mut_obj_get(root, key) : NULL;
        const char *sv = v != NULL ? yyjson_mut_get_str(v) : NULL;
        if (sv != NULL) {
            strncpy(buf, sv, cap - 1);
            buf[cap - 1] = '\0';
        }
    }
    pc_json_value_fini(&ev);
}

static double export_pref_num(pc_engine_session *s, const char *key, double def)
{
    double r = def;
    pc_json_value ev = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(s, "export", &ev, NULL) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&ev);
        yyjson_mut_val *v = root != NULL ? yyjson_mut_obj_get(root, key) : NULL;
        if (v != NULL && yyjson_mut_is_num(v)) {
            r = yyjson_mut_get_num(v);
        }
    }
    pc_json_value_fini(&ev);
    return r;
}

static bool export_pref_bool(pc_engine_session *s, const char *key, bool def)
{
    bool r = def;
    pc_json_value ev = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(s, "export", &ev, NULL) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&ev);
        yyjson_mut_val *v = root != NULL ? yyjson_mut_obj_get(root, key) : NULL;
        if (v != NULL && yyjson_mut_is_bool(v)) {
            r = yyjson_mut_get_bool(v);
        }
    }
    pc_json_value_fini(&ev);
    return r;
}

static pc_status run_quick_export(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "file.export.quickExport";
    char fmt[16];
    export_pref_str(s, "quickExportFormat", fmt, sizeof fmt, "png");
    pc_engine_doc_state *d = pc_engine_session_active_mut(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);

    /* path: param, else sameFolder + saved document. */
    size_t plen = 0;
    const char *pp = jstr(p, "path", &plen);
    char *path = NULL;
    if (pp != NULL && plen > 0) {
        path = strndup(pp, plen);
    } else {
        char loc[16];
        export_pref_str(s, "quickExportLocation", loc, sizeof loc, "ask");
        if (strcmp(loc, "sameFolder") == 0 && d->path != NULL) {
            char stem[512];
            pc_engine_file_stem(d->path, stem, sizeof stem);
            const char *slash = strrchr(d->path, '/');
            char *dir = slash != NULL ? strndup(d->path, (size_t)(slash - d->path)) : strdup("");
            if (dir != NULL) {
                char rel[600];
                snprintf(rel, sizeof rel, "%s.%s", stem, fmt);
                path = pc_engine_file_join(dir, rel);
                free(dir);
            }
        }
        if (path == NULL) {
            return pc_engine_err_bad_params(
                err, cmd, "missing `path` (Export Preferences > Location is \"ask\", or the document was "
                          "never saved)");
        }
    }
    if (path == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }

    char metapref[16];
    export_pref_str(s, "metadata", metapref, sizeof metapref, "copyright");
    bool to_srgb = export_pref_bool(s, "convertToSrgb", true);
    const char *meta = strcmp(metapref, "all") == 0 ? "all" : (strcmp(metapref, "none") == 0 ? "none"
                                                                                              : "copyright");

    pc_status rc = PC_OK;
    pc_document clone;
    bool have_clone = false;

    if (strcmp(fmt, "webp") == 0) {
        /* WebP uses the regular exporter with the Photoshop 0..12 quality scale. */
        pc_web_settings wst;
        pc_web_settings_default(&wst);
        wst.convert_to_srgb = to_srgb;
        pc_document wdoc;
        memset(&wdoc, 0, sizeof wdoc);
        double sx, sy;
        rc = web_document(doc, NULL, &wst, &wdoc, &sx, &sy, err);
        pc_web_settings_fini(&wst);
        if (rc != PC_OK) {
            free(path);
            return rc;
        }
        bool lossless = export_pref_bool(s, "webpLossless", true);
        double wq = export_pref_num(s, "webpQuality", 85.0);
        pc_engine_save_opts so = pc_engine_save_opts_default();
        if (!lossless) {
            double c = wq < 1.0 ? 1.0 : (wq > 100.0 ? 100.0 : wq);
            so.has_quality = true;
            so.quality = (c - 1.0) * 12.0 / 99.0;
        }
        rc = pc_engine_save_doc(&wdoc, path, &so, NULL, err);
        pc_document_fini(&wdoc);
        if (rc == PC_OK) {
            (void)pc_engine_automate_fire_event(s, "export");
            yyjson_mut_doc *rd = NULL;
            if (pc_engine_result_doc(out, &rd, err) == PC_OK) {
                yyjson_mut_val *root = yyjson_mut_obj(rd);
                yyjson_mut_obj_add_strcpy(rd, root, "path", path);
                yyjson_mut_obj_add_strcpy(rd, root, "format", fmt);
                yyjson_mut_obj_add_val(rd, root, "warnings", yyjson_mut_arr(rd));
                yyjson_mut_doc_set_root(rd, root);
            } else {
                rc = PC_ERR_NOMEM;
            }
        }
        free(path);
        return rc;
    }

    /* Build web settings from an options object (format + quality from prefs). */
    yyjson_mut_doc *od = yyjson_mut_doc_new(NULL);
    if (od == NULL) {
        free(path);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *o = yyjson_mut_obj(od);
    yyjson_mut_obj_add_strcpy(od, o, "metadata", meta);
    yyjson_mut_obj_add_bool(od, o, "convertToSrgb", to_srgb);
    yyjson_mut_obj_add_bool(od, o, "transparency", true);
    yyjson_mut_obj_add_strcpy(od, o, "matte", "#ffffff");
    if (strcmp(fmt, "jpg") == 0) {
        yyjson_mut_obj_add_strcpy(od, o, "format", "jpeg");
        double jq = export_pref_num(s, "jpegQuality", 85.0);
        if (jq > 100.0) {
            jq = 100.0;
        }
        yyjson_mut_obj_add_real(od, o, "quality", jq);
    } else if (strcmp(fmt, "gif") == 0) {
        yyjson_mut_obj_add_strcpy(od, o, "format", "gif");
    } else {
        yyjson_mut_obj_add_strcpy(od, o, "format", "png24");
    }
    yyjson_mut_doc_set_root(od, o);
    pc_buf qtxt = PC_BUF_INIT;
    (void)pc_json_write(o, 0, &qtxt, NULL);
    yyjson_doc *qdoc = NULL;
    (void)pc_json_parse(pc_buf_cstr(&qtxt), qtxt.len, &qdoc, NULL);
    pc_buf_fini(&qtxt);
    yyjson_mut_doc_free(od);
    if (qdoc == NULL) {
        free(path);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_web_settings wst;
    rc = pc_web_settings_from_params(yyjson_doc_get_root(qdoc), cmd, &wst, err);
    yyjson_doc_free(qdoc);
    if (rc != PC_OK) {
        free(path);
        return rc;
    }
    pc_document wdoc;
    memset(&wdoc, 0, sizeof wdoc);
    double sx, sy;
    rc = web_document(doc, NULL, &wst, &wdoc, &sx, &sy, err);
    if (rc != PC_OK) {
        pc_web_settings_fini(&wst);
        free(path);
        return rc;
    }
    (void)have_clone;
    (void)clone;
    pc_compose_buffer buf = {0};
    rc = pc_compose_flatten(&wdoc, &buf, err);
    char *xmp = NULL;
    if (rc == PC_OK) {
        const char *dxmp = doc->metadata.has_xmp ? pc_str_c(&doc->metadata.xmp) : NULL;
        size_t dxl = doc->metadata.has_xmp ? doc->metadata.xmp.len : 0;
        int mode = wst.metadata == PC_WEB_META_NONE
                       ? 0
                       : (wst.metadata == PC_WEB_META_COPYRIGHT ? 1
                                                                : (wst.metadata == PC_WEB_META_CONTACT ? 2 : 3));
        (void)pc_engine_web_xmp(dxmp, dxl, mode, &xmp, NULL);
        pc_web_optimized opt;
        const uint8_t *icc = wdoc.icc_profile != NULL ? pc_blob_data(wdoc.icc_profile) : NULL;
        size_t icc_len = wdoc.icc_profile != NULL ? pc_blob_len(wdoc.icc_profile) : 0;
        rc = pc_web_optimize((const float (*)[4])buf.px, wdoc.size.width, pc_document_bounds(&wdoc), &wst, icc, icc_len, xmp,
                             xmp != NULL ? strlen(xmp) : 0, wdoc.resolution_dpi, false, &opt, err);
        if (rc == PC_OK) {
            rc = pc_engine_write_file(path, opt.bytes.data, opt.bytes.len, err);
            if (rc == PC_OK) {
                (void)pc_engine_automate_fire_event(s, "export");
                yyjson_mut_doc *rd = NULL;
                if (pc_engine_result_doc(out, &rd, err) == PC_OK) {
                    yyjson_mut_val *root = yyjson_mut_obj(rd);
                    yyjson_mut_obj_add_strcpy(rd, root, "path", path);
                    yyjson_mut_obj_add_strcpy(rd, root, "format", fmt);
                    yyjson_mut_obj_add_uint(rd, root, "bytes", opt.bytes.len);
                    yyjson_mut_doc_set_root(rd, root);
                } else {
                    rc = PC_ERR_NOMEM;
                }
            }
            pc_web_optimized_fini(&opt);
        }
    }
    free(xmp);
    pc_compose_buffer_fini(&buf);
    pc_document_fini(&wdoc);
    pc_web_settings_fini(&wst);
    free(path);
    return rc;
}

/* ---- command run functions ---------------------------------------------------------------- */

static pc_status web_has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return PC_OK;
}

static pc_status web_always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

static pc_status run_save_for_web(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return save_for_web_impl(s, p, native_writer, NULL, true, out, err);
}

static pc_status run_image_assets(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    (void)out;
    return pc_fail(err, PC_ERR_UNSUPPORTED,
                   "Image Assets is not available yet in this build (Generator pipeline pending)");
}

static const char *const MENU_FILE_EXPORT[] = {"File", "Export"};
static const char *const MENU_FILE_GENERATE[] = {"File", "Generate"};

static const pc_engine_command SPECS[] = {
    {"file.export.saveForWebLegacy", "Save for Web (Legacy)\xE2\x80\xA6", MENU_FILE_EXPORT, 2,
     "Cmd+Alt+Shift+S",
     "{\"preset\":\"GIF 128 Dithered|JPEG High|PNG-24|\xE2\x80\xA6\"?,\"format\":\"gif|png8|png24|jpeg|"
     "wbmp\"=\"png24\",\"palette\":\"perceptual|selective|adaptive|restrictive|exact|systemMac|"
     "systemWindows|uniform\"=\"selective\",\"colors\":2..256=256,\"colorTable\":[\"#rrggbb\",\xE2\x80\xA6]?,"
     "\"colorTableEdits\":{\"0\":\"#rrggbb\",\xE2\x80\xA6}?,\"dither\":\"none|diffusion|pattern|noise\"="
     "\"diffusion\",\"ditherAmount\":0..100=88,\"transparency\":bool=true,\"matte\":\"#rrggbb|none\"="
     "\"none\",\"interlaced\":bool=false,\"webSnap\":0..100=0,\"quality\":0..100=60,\"progressive\":bool="
     "false,\"optimized\":bool=true,\"embedIcc\":bool=false,\"metadata\":\"none|copyright|"
     "copyrightAndContact|all\"=\"copyright\",\"convertToSrgb\":bool=true,\"width\"|\"height\"|\"percent\"?,"
     "\"resample\":\"bicubic|bilinear|nearest\"?,\"path\":file?,\"dir\":folder?,\"imagesFolder\":relative-"
     "folder?=\"images\",\"html\":bool=false,\"trustedSliceHtml\":bool=false,\"slices\":\"all|user\"=\"all\","
     "\"numbers\":[n]?} \xE2\x86\x92 no path/dir: {bytes,width,height,colors,colorTable,transparentIndex} "
     "estimate; else {files, html, bytes}",
     web_has_doc, run_save_for_web, true},
    {"file.export.exportPreferences", "Export Preferences\xE2\x80\xA6", MENU_FILE_EXPORT, 2, NULL,
     "{\"quickExportFormat\":\"png|jpg|gif|webp\"?,\"quickExportLocation\":\"ask|sameFolder\"?,"
     "\"jpegQuality\":1..100?,\"webpLossless\":bool?,\"webpQuality\":1..100?,\"metadata\":\"none|"
     "copyright|all\"?,\"convertToSrgb\":bool?} \xE2\x86\x92 {values}",
     web_always, run_export_prefs, true},
    {"file.export.quickExport", "Quick Export", NULL, 0, NULL,
     "{\"path\":str? (required unless Export Preferences \xE2\x80\xBA Location is \"sameFolder\" and the "
     "document is saved)} \xE2\x86\x92 {path, format, bytes}",
     web_has_doc, run_quick_export, true},
    {"file.generate.imageAssets", "Image Assets", MENU_FILE_GENERATE, 2, NULL,
     "{\"on\":bool? (default: toggle),\"dir\":folder? (default <document>-assets next to the file)} "
     "\xE2\x86\x92 {enabled, files, errors}",
     web_has_doc, run_image_assets, true},
};

const pc_engine_command *pc_engine_web_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
