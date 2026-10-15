/* Image menu remnants (port of crates/engine/src/mode_cmds.rs, E3): the palette / ink modes —
 * Image > Mode > Indexed Color, Color Table, Bitmap and Duotone.
 *
 * Indexed, Bitmap and Duotone documents keep expanded pixels (RGB 8-bit / Gray 8-bit / Gray):
 * Indexed and Bitmap quantise the flattened image in place and record the palette or the 1-bit
 * look; Duotone keeps the gray pixels and the inks. Like Photoshop the conversions flatten
 * (Indexed, Bitmap) and require a grayscale source (Bitmap, Duotone).
 *
 * Image Rotation › Arbitrary (image.rotation.arbitrary, ET3) rotates the whole document through the
 * Free-Transform engine (transform_cmds: warp_gray / composite_over / transform_layer): the canvas
 * grows to fit, the Background fills what the rotation uncovers with the background colour, and
 * every other layer (even locked ones, and those inside groups) turns with it, keeping transparency
 * there. */
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/json.h"
#include "base/mem.h"
#include "color/color.h"
#include "color/convert.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "doc/mode.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "algo/quantize.h"
#include "algo/transform.h"

#include "engine_internal.h"
#include "transform_cmds.h"

/* ---- small param helpers ------------------------------------------------------------------ */

static pc_status bad(pc_error *err, const char *cmd, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return pc_engine_err_bad_params(err, cmd, "%s", msg);
}

static const char *str_or(yyjson_val *p, const char *key, const char *def)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return def;
    }
    const char *s = yyjson_get_str(v);
    return s != NULL ? s : def;
}

static float num(yyjson_val *p, const char *key, float def)
{
    yyjson_val *v = pc_json_get(p, key);
    double d;
    if (v != NULL && pc_json_as_f64(v, &d)) {
        return (float)d;
    }
    return def;
}

/* A finite number param, `default` when absent or null; anything else is a bad-params error. */
static pc_status finite_num(yyjson_val *p, const char *cmd, const char *key, double def, double *out,
                            pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL || yyjson_is_null(v)) {
        *out = def;
        return PC_OK;
    }
    double d;
    if (!pc_json_as_f64(v, &d) || !isfinite(d)) {
        return bad(err, cmd, "`%s` must be a number", key);
    }
    *out = d;
    return PC_OK;
}

/* Parses "#rrggbb" or [r,g,b] into an RGB byte triple; false otherwise. */
static bool parse_color(yyjson_val *v, uint8_t out[3])
{
    size_t slen = 0;
    const char *s = v != NULL ? pc_json_as_str(v, &slen) : NULL;
    if (s != NULL) {
        const char *h = (slen > 0 && s[0] == '#') ? s + 1 : s;
        size_t hn = (slen > 0 && s[0] == '#') ? slen - 1 : slen;
        if (hn < 6) {
            return false;
        }
        for (int i = 0; i < 3; i++) {
            int acc = 0;
            for (int k = 0; k < 2; k++) {
                int ch = h[i * 2 + k];
                int d;
                if (ch >= '0' && ch <= '9') {
                    d = ch - '0';
                } else if (ch >= 'a' && ch <= 'f') {
                    d = ch - 'a' + 10;
                } else if (ch >= 'A' && ch <= 'F') {
                    d = ch - 'A' + 10;
                } else {
                    return false;
                }
                acc = acc * 16 + d;
            }
            out[i] = (uint8_t)acc;
        }
        return true;
    }
    if (yyjson_is_arr(v) && yyjson_arr_size(v) >= 3) {
        for (int i = 0; i < 3; i++) {
            yyjson_val *e = yyjson_arr_get(v, (size_t)i);
            double d;
            if (e == NULL || !pc_json_as_f64(e, &d)) {
                return false;
            }
            d = round(d);
            if (d < 0.0) {
                d = 0.0;
            } else if (d > 255.0) {
                d = 255.0;
            }
            out[i] = (uint8_t)d;
        }
        return true;
    }
    return false;
}

/* ---- enabled predicates ------------------------------------------------------------------- */

static pc_status mode_is(const pc_engine_session *s, pc_error *reason, const pc_color_mode *modes,
                         size_t nmodes, const char *what)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    pc_color_mode m = pc_doc_arc_get(d->doc)->mode;
    for (size_t i = 0; i < nmodes; i++) {
        if (modes[i] == m) {
            return PC_OK;
        }
    }
    return pc_fail(reason, PC_ERR_INVALID, "%s (the document is %s)", what, pc_color_mode_name(m));
}

static pc_status en_indexed(const pc_engine_session *s, pc_error *reason)
{
    static const pc_color_mode modes[] = {PC_COLOR_MODE_RGB,     PC_COLOR_MODE_GRAYSCALE,
                                          PC_COLOR_MODE_INDEXED, PC_COLOR_MODE_DUOTONE,
                                          PC_COLOR_MODE_CMYK,    PC_COLOR_MODE_LAB};
    return mode_is(s, reason, modes, 6, "Indexed Color needs an RGB or Grayscale image");
}

static pc_status en_color_table(const pc_engine_session *s, pc_error *reason)
{
    static const pc_color_mode modes[] = {PC_COLOR_MODE_INDEXED};
    return mode_is(s, reason, modes, 1, "Color Table needs an Indexed Color image");
}

static pc_status en_bitmap(const pc_engine_session *s, pc_error *reason)
{
    static const pc_color_mode modes[] = {PC_COLOR_MODE_GRAYSCALE};
    return mode_is(s, reason, modes, 1, "Bitmap needs a Grayscale image (Image > Mode > Grayscale first)");
}

static pc_status en_duotone(const pc_engine_session *s, pc_error *reason)
{
    static const pc_color_mode modes[] = {PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_DUOTONE};
    return mode_is(s, reason, modes, 2, "Duotone needs a Grayscale image (Image > Mode > Grayscale first)");
}

/* ---- flatten helpers ---------------------------------------------------------------------- */

/* The flattened image as straight RGBA (over white when `opaque`). *out is `*n` [4] floats, owned
 * (free()); the width is the document's. */
static pc_status composite(const pc_document *doc, bool opaque, float (**out)[4], size_t *n, pc_error *err)
{
    pc_compose_buffer buf;
    pc_status rc = pc_compose_flatten(doc, &buf, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (opaque) {
        float white[3] = {1.0f, 1.0f, 1.0f};
        pc_compose_buffer_over_background(&buf, white);
    }
    /* Steal the buffer's pixel array. */
    *out = buf.px;
    *n = buf.len;
    buf.px = NULL;
    buf.len = 0;
    pc_compose_buffer_fini(&buf);
    return PC_OK;
}

/* Replaces the document's layers by one 8-bit layer holding `px` (n straight-RGBA pixels, row
 * order) in `mode`'s storage format; converts channels to Gray8; clears quick mask / table /
 * duotone. Writes *active. */
static pc_status single_layer(pc_document *doc, bool *has_active, pc_layer_id *active, pc_color_mode mode,
                              const float (*px)[4], size_t n, const char *name, bool background,
                              pc_error *err)
{
    doc->mode = mode;
    doc->depth = PC_SAMPLE_TYPE_U8;
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t nch = pc_pixel_format_channels(fmt);
    float *data = NULL;
    if (n > 0 && nch > 0) {
        data = pc_malloc_array(n * nch, sizeof(float));
        if (data == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory converting mode");
        }
    }
    for (size_t i = 0; i < n; i++) {
        float enc[PC_RASTER_MAX_CHANNELS];
        float q[4] = {px[i][0], px[i][1], px[i][2], px[i][3]};
        size_t m = pc_from_rgba(fmt, q, enc);
        memcpy(&data[i * nch], enc, m * sizeof(float));
    }
    pc_layer l;
    pc_status rc = pc_layer_raster(&l, name, fmt, err);
    if (rc != PC_OK) {
        free(data);
        return rc;
    }
    if (background) {
        l.locks.transparency = true;
        l.locks.position = true;
    }
    pc_surface *surf = pc_layer_surface_mut(&l);
    if (surf != NULL && data != NULL) {
        rc = pc_surface_write_region(surf, pc_document_bounds(doc), data, n * nch, err);
        if (rc != PC_OK) {
            free(data);
            pc_layer_fini(&l);
            return rc;
        }
        pc_surface_prune(surf);
    }
    free(data);
    *has_active = true;
    *active = l.id;
    pc_layer *one = pc_malloc_array(1, sizeof(pc_layer));
    if (one == NULL) {
        pc_layer_fini(&l);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory converting mode");
    }
    for (size_t i = 0; i < doc->nlayers; i++) {
        pc_layer_fini(&doc->layers[i]);
    }
    free(doc->layers);
    one[0] = l;
    doc->layers = one;
    doc->nlayers = 1;
    /* Channels become Gray8 (keeping alpha-ness). */
    for (size_t i = 0; i < doc->nchannels; i++) {
        pc_pixel_format cf = pc_surface_format(&doc->channels[i].surface);
        pc_pixel_format gf = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, cf.alpha};
        pc_surface cout;
        rc = pc_surface_convert(&doc->channels[i].surface, gf, &cout, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&doc->channels[i].surface);
        doc->channels[i].surface = cout;
    }
    if (doc->has_quick_mask) {
        pc_alpha_channel_fini(&doc->quick_mask);
        doc->has_quick_mask = false;
        doc->quick_mask_unselected = false;
    }
    if (doc->has_color_table) {
        pc_color_table_fini(&doc->color_table);
        doc->has_color_table = false;
    }
    if (doc->has_duotone) {
        pc_duotone_fini(&doc->duotone);
        doc->has_duotone = false;
    }
    return PC_OK;
}

/* ---- Indexed Color ------------------------------------------------------------------------ */

static bool parse_forced(const char *s, pc_algo_forced *out)
{
    if (strcmp(s, "none") == 0) {
        *out = PC_ALGO_FORCED_NONE;
    } else if (strcmp(s, "blackWhite") == 0 || strcmp(s, "blackAndWhite") == 0) {
        *out = PC_ALGO_FORCED_BLACK_WHITE;
    } else if (strcmp(s, "primaries") == 0) {
        *out = PC_ALGO_FORCED_PRIMARIES;
    } else if (strcmp(s, "web") == 0) {
        *out = PC_ALGO_FORCED_WEB;
    } else {
        return false;
    }
    return true;
}

static bool parse_dither(const char *s, pc_algo_dither *out)
{
    if (strcmp(s, "none") == 0) {
        *out = PC_ALGO_DITHER_NONE;
    } else if (strcmp(s, "diffusion") == 0) {
        *out = PC_ALGO_DITHER_DIFFUSION;
    } else if (strcmp(s, "pattern") == 0) {
        *out = PC_ALGO_DITHER_PATTERN;
    } else if (strcmp(s, "noise") == 0) {
        *out = PC_ALGO_DITHER_NOISE;
    } else {
        return false;
    }
    return true;
}

typedef struct {
    pc_algo_palette_kind kind;
    pc_algo_forced forced;
    pc_algo_dither dither;
    size_t colors;
    float amount;
    bool transparency;
    /* outputs */
    size_t pal_len;
    bool has_transparent;
    size_t transparent;
} indexed_ctx;

static pc_status indexed_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                              pc_error *err)
{
    indexed_ctx *c = ctx;
    float (*px)[4] = NULL;
    size_t n = 0;
    pc_status rc = composite(doc, !c->transparency, &px, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool has_clear = false;
    if (c->transparency) {
        for (size_t i = 0; i < n; i++) {
            if (px[i][3] < 0.5f) {
                has_clear = true;
                break;
            }
        }
    }
    size_t want = c->colors;
    if (has_clear) {
        want = c->colors > 1 ? c->colors - 1 : c->colors;
        if (want < 2) {
            want = 2;
        }
    }
    pc_algo_palette pal;
    rc = pc_algo_quantize_build_palette((const float (*)[4])px, n, c->kind, want, c->forced, &pal, err);
    if (rc != PC_OK) {
        free(px);
        return rc;
    }
    bool has_transparent = false;
    size_t transparent = 0;
    if (has_clear && pal.n < 256) {
        pal.c[pal.n][0] = 255;
        pal.c[pal.n][1] = 255;
        pal.c[pal.n][2] = 255;
        pal.n++;
        has_transparent = true;
        transparent = pal.n - 1;
    }
    size_t w = doc->size.width;
    uint8_t *idx = NULL;
    if (n > 0) {
        idx = pc_malloc_array(n, 1);
        if (idx == NULL) {
            free(px);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory quantising");
        }
    }
    rc = pc_algo_quantize(px, n, w, &pal.c[0][0], pal.n, c->dither, c->amount, has_transparent,
                          transparent, idx, err);
    free(idx);
    if (rc != PC_OK) {
        free(px);
        return rc;
    }
    rc = single_layer(doc, has_active, active, PC_COLOR_MODE_INDEXED, (const float (*)[4])px, n,
                      has_transparent ? "Index" : "Background", !has_transparent, err);
    free(px);
    if (rc != PC_OK) {
        return rc;
    }
    /* Record the colour table. */
    uint8_t (*colors)[3] = pc_malloc_array(pal.n > 0 ? pal.n : 1, 3);
    if (colors == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory recording the colour table");
    }
    for (size_t i = 0; i < pal.n; i++) {
        colors[i][0] = pal.c[i][0];
        colors[i][1] = pal.c[i][1];
        colors[i][2] = pal.c[i][2];
    }
    doc->color_table.colors = colors;
    doc->color_table.ncolors = pal.n;
    doc->color_table.has_transparent = has_transparent;
    doc->color_table.transparent = (uint8_t)transparent;
    doc->has_color_table = true;
    c->pal_len = pal.n;
    c->has_transparent = has_transparent;
    c->transparent = transparent;
    return PC_OK;
}

static pc_status run_indexed(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "image.mode.indexedColor";
    indexed_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    if (!pc_algo_palette_kind_from_id(str_or(p, "palette", "selective"), &ctx.kind)) {
        return bad(err, CMD,
                   "palette: exact|systemMac|systemWindows|web|uniform|perceptual|selective|adaptive");
    }
    if (!parse_forced(str_or(p, "forced", "blackWhite"), &ctx.forced)) {
        return bad(err, CMD, "forced: none|blackWhite|primaries|web");
    }
    if (!parse_dither(str_or(p, "dither", "diffusion"), &ctx.dither)) {
        return bad(err, CMD, "dither: none|diffusion|pattern|noise");
    }
    ctx.colors = 256;
    yyjson_val *cv = pc_json_get(p, "colors");
    if (cv != NULL) {
        double d;
        if (!pc_json_as_f64(cv, &d) || !isfinite(d) || floor(d) != d || d < 2.0 || d > 256.0) {
            return bad(err, CMD, "colors must be an integer from 2 to 256");
        }
        ctx.colors = (size_t)d;
    }
    float amount = num(p, "amount", 75.0f);
    amount = amount < 0.0f ? 0.0f : (amount > 100.0f ? 100.0f : amount);
    ctx.amount = amount / 100.0f;
    yyjson_val *tv = pc_json_get(p, "transparency");
    bool tr = true;
    if (tv != NULL) {
        (void)pc_json_as_bool(tv, &tr);
    }
    ctx.transparency = tr;
    pc_status rc = pc_engine_session_edit(s, "Indexed Color", indexed_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_uint(jd, root, "colors", ctx.pal_len);
    if (ctx.has_transparent) {
        yyjson_mut_obj_add_uint(jd, root, "transparentIndex", ctx.transparent);
    } else {
        yyjson_mut_obj_add_null(jd, root, "transparentIndex");
    }
    yyjson_mut_doc_set_root(jd, root);
    return PC_OK;
}

/* ---- Bitmap ------------------------------------------------------------------------------- */

typedef struct {
    pc_algo_bitmap_method method;
} bitmap_ctx;

static pc_status bitmap_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                             pc_error *err)
{
    bitmap_ctx *c = ctx;
    float (*px)[4] = NULL;
    size_t n = 0;
    pc_status rc = composite(doc, true, &px, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    float *gray = NULL;
    if (n > 0) {
        gray = pc_malloc_array(n, sizeof(float));
        if (gray == NULL) {
            free(px);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory converting to bitmap");
        }
    }
    for (size_t i = 0; i < n; i++) {
        float rgb[3] = {px[i][0], px[i][1], px[i][2]};
        gray[i] = pc_rgb_to_gray(rgb);
    }
    rc = pc_algo_quantize_to_bitmap(gray, n, doc->size.width, c->method, err);
    if (rc != PC_OK) {
        free(px);
        free(gray);
        return rc;
    }
    for (size_t i = 0; i < n; i++) {
        px[i][0] = gray[i];
        px[i][1] = gray[i];
        px[i][2] = gray[i];
        px[i][3] = 1.0f;
    }
    free(gray);
    rc = single_layer(doc, has_active, active, PC_COLOR_MODE_BITMAP, (const float (*)[4])px, n, "Background", true, err);
    free(px);
    return rc;
}

static pc_status run_bitmap(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "image.mode.bitmap";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    float dpi = pc_doc_arc_get(d->doc)->resolution_dpi;
    if (dpi < 1.0f) {
        dpi = 1.0f;
    }
    bitmap_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    const char *method = str_or(p, "method", "diffusion");
    if (strcmp(method, "threshold") == 0 || strcmp(method, "threshold50") == 0) {
        ctx.method.kind = PC_ALGO_BITMAP_THRESHOLD;
    } else if (strcmp(method, "pattern") == 0) {
        ctx.method.kind = PC_ALGO_BITMAP_PATTERN;
    } else if (strcmp(method, "diffusion") == 0) {
        ctx.method.kind = PC_ALGO_BITMAP_DIFFUSION;
    } else if (strcmp(method, "halftone") == 0) {
        double freq = 53.0, angle = 45.0;
        pc_status rc = finite_num(p, CMD, "frequency", 53.0, &freq, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (freq < 1.0) {
            freq = 1.0;
        } else if (freq > 999.0) {
            freq = 999.0;
        }
        rc = finite_num(p, CMD, "angle", 45.0, &angle, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (angle < -180.0 || angle > 180.0) {
            return bad(err, CMD, "angle %g is outside -180..180", angle);
        }
        pc_algo_halftone_shape shape;
        if (!pc_algo_halftone_shape_from_id(str_or(p, "shape", "round"), &shape)) {
            return bad(err, CMD, "shape: round|ellipse|line|square|diamond|cross");
        }
        float cell = (float)((double)dpi / freq);
        if (cell < 2.0f) {
            cell = 2.0f;
        }
        ctx.method.kind = PC_ALGO_BITMAP_HALFTONE;
        ctx.method.cell = cell;
        ctx.method.angle = (float)angle;
        ctx.method.shape = shape;
    } else {
        return bad(err, CMD, "method `%s` (threshold|pattern|diffusion|halftone)", method);
    }
    pc_status rc = pc_engine_session_edit(s, "Bitmap", bitmap_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ---- Color Table -------------------------------------------------------------------------- */

/* Built-in Color Table presets into *colors (malloc'd, 256 entries) / *ncolors; false unknown. */
static bool preset_table(const char *name, uint8_t (**colors)[3], size_t *ncolors, pc_error *err)
{
    pc_algo_palette pal;
    bool sys = false;
    if (strcmp(name, "systemMac") == 0 || strcmp(name, "system") == 0) {
        pc_algo_quantize_mac_palette(&pal);
        sys = true;
    } else if (strcmp(name, "systemWindows") == 0) {
        pc_algo_quantize_windows_palette(&pal);
        sys = true;
    } else if (strcmp(name, "web") == 0) {
        pc_algo_quantize_web_palette(&pal);
        sys = true;
    }
    uint8_t (*c)[3] = pc_malloc_array(256, 3);
    if (c == NULL) {
        (void)pc_fail(err, PC_ERR_NOMEM, "out of memory building the colour table");
        return false;
    }
    if (sys) {
        for (size_t i = 0; i < pal.n && i < 256; i++) {
            c[i][0] = pal.c[i][0];
            c[i][1] = pal.c[i][1];
            c[i][2] = pal.c[i][2];
        }
        *colors = c;
        *ncolors = pal.n; /* web is 216, mac/windows 256 */
        return true;
    }
    for (int i = 0; i < 256; i++) {
        float t = (float)i / 255.0f;
        float rgb[3];
        if (strcmp(name, "grayscale") == 0) {
            rgb[0] = rgb[1] = rgb[2] = t;
        } else if (strcmp(name, "blackBody") == 0) {
            rgb[0] = fminf(t * 3.0f, 1.0f);
            rgb[1] = fminf(fmaxf(t * 3.0f - 1.0f, 0.0f), 1.0f);
            rgb[2] = fminf(fmaxf(t * 3.0f - 2.0f, 0.0f), 1.0f);
        } else if (strcmp(name, "spectrum") == 0) {
            float hh = t * 300.0f / 60.0f;
            float x = 1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f);
            int seg = (int)hh;
            switch (seg) {
            case 0: rgb[0] = 1.0f; rgb[1] = x; rgb[2] = 0.0f; break;
            case 1: rgb[0] = x; rgb[1] = 1.0f; rgb[2] = 0.0f; break;
            case 2: rgb[0] = 0.0f; rgb[1] = 1.0f; rgb[2] = x; break;
            case 3: rgb[0] = 0.0f; rgb[1] = x; rgb[2] = 1.0f; break;
            default: rgb[0] = x; rgb[1] = 0.0f; rgb[2] = 1.0f; break;
            }
        } else {
            free(c);
            return false; /* unknown */
        }
        for (int k = 0; k < 3; k++) {
            float v = rgb[k] < 0.0f ? 0.0f : (rgb[k] > 1.0f ? 1.0f : rgb[k]);
            c[i][k] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }
    *colors = c;
    *ncolors = 256;
    return true;
}

typedef struct {
    pc_color_table table; /* owned; the desired new table */
} color_table_ctx;

static pc_status color_table_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    color_table_ctx *c = ctx;
    pc_color_table old = doc->color_table; /* borrow the current table (not owned here) */
    pc_color_table *table = &c->table;
    for (size_t li = 0; li < doc->nlayers; li++) {
        pc_surface *surf = pc_layer_surface_mut(&doc->layers[li]);
        if (surf == NULL) {
            continue;
        }
        pc_rect r = pc_surface_content_bounds(surf);
        if (pc_rect_is_empty(r)) {
            continue;
        }
        pc_pixel_format fmt = pc_surface_format(surf);
        size_t nch = pc_pixel_format_channels(fmt);
        float *data = NULL;
        size_t len = 0;
        pc_status rc = pc_surface_read_region(surf, r, &data, &len, err);
        if (rc != PC_OK) {
            return rc;
        }
        for (size_t off = 0; off + nch <= len; off += nch) {
            float cc[4];
            pc_to_rgba(fmt, &data[off], nch, cc);
            float rgb[3] = {cc[0], cc[1], cc[2]};
            size_t i;
            bool skip = false;
            if (cc[3] <= 0.0f) {
                if (!old.has_transparent) {
                    skip = true;
                } else {
                    i = old.transparent;
                    float orgb[3];
                    pc_color_table_rgb(&old, i, orgb);
                    if (i >= old.ncolors || orgb[0] != rgb[0] || orgb[1] != rgb[1] || orgb[2] != rgb[2]) {
                        skip = true;
                    }
                }
            } else {
                i = pc_color_table_nearest(&old, rgb);
            }
            if (skip) {
                continue;
            }
            float alpha;
            if (table->has_transparent && (size_t)table->transparent == i) {
                alpha = 0.0f;
            } else if (cc[3] <= 0.0f) {
                alpha = 1.0f;
            } else {
                alpha = cc[3];
            }
            float nc[3];
            if (i < table->ncolors) {
                pc_color_table_rgb(table, i, nc);
            } else {
                pc_color_table_rgb(table, pc_color_table_nearest(table, rgb), nc);
            }
            float enc[PC_RASTER_MAX_CHANNELS];
            float rgba[4] = {nc[0], nc[1], nc[2], alpha};
            size_t m = pc_from_rgba(fmt, rgba, enc);
            memcpy(&data[off], enc, m * sizeof(float));
        }
        rc = pc_surface_write_region(surf, r, data, len, err);
        free(data);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_prune(surf);
    }
    /* Install the new table (take over c->table; free the old one). */
    pc_color_table_fini(&doc->color_table);
    doc->color_table = c->table;
    doc->has_color_table = true;
    memset(&c->table, 0, sizeof c->table);
    return PC_OK;
}

static pc_status report_table(pc_json_value *out, const pc_color_table *t, pc_error *err)
{
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_val *arr = yyjson_mut_arr(jd);
    for (size_t i = 0; i < t->ncolors; i++) {
        char hex[8];
        (void)snprintf(hex, sizeof hex, "#%02x%02x%02x", t->colors[i][0], t->colors[i][1], t->colors[i][2]);
        yyjson_mut_arr_add_strcpy(jd, arr, hex);
    }
    yyjson_mut_obj_add_val(jd, root, "colors", arr);
    if (t->has_transparent) {
        yyjson_mut_obj_add_uint(jd, root, "transparent", t->transparent);
    } else {
        yyjson_mut_obj_add_null(jd, root, "transparent");
    }
    yyjson_mut_doc_set_root(jd, root);
    return PC_OK;
}

static pc_status run_color_table(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "image.mode.colorTable";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!doc->has_color_table) {
        return pc_engine_err_other(err, "the document has no color table");
    }
    pc_color_table cur;
    if (pc_color_table_clone(&doc->color_table, &cur, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_color_table table;
    if (pc_color_table_clone(&doc->color_table, &table, err) != PC_OK) {
        pc_color_table_fini(&cur);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }

    /* `table` preset replaces the colour list. */
    const char *tname = str_or(p, "table", "custom");
    if (strcmp(tname, "custom") != 0) {
        uint8_t (*pcols)[3] = NULL;
        size_t pn = 0;
        if (!preset_table(tname, &pcols, &pn, err)) {
            pc_color_table_fini(&cur);
            pc_color_table_fini(&table);
            if (err != NULL && err->code == PC_ERR_NOMEM) {
                return PC_ERR_NOMEM;
            }
            return bad(err, CMD, "unknown table `%s`", tname);
        }
        free(table.colors);
        table.colors = pcols;
        table.ncolors = pn;
    }
    /* `colors`: a full new list. */
    yyjson_val *colorsv = pc_json_get(p, "colors");
    if (colorsv != NULL && yyjson_is_arr(colorsv)) {
        size_t m = yyjson_arr_size(colorsv);
        if (m == 0 || m > 256) {
            pc_color_table_fini(&cur);
            pc_color_table_fini(&table);
            return bad(err, CMD, "a color table has 1 to 256 entries");
        }
        uint8_t (*nc)[3] = pc_malloc_array(m, 3);
        if (nc == NULL) {
            pc_color_table_fini(&cur);
            pc_color_table_fini(&table);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (size_t i = 0; i < m; i++) {
            if (!parse_color(yyjson_arr_get(colorsv, i), nc[i])) {
                free(nc);
                pc_color_table_fini(&cur);
                pc_color_table_fini(&table);
                return bad(err, CMD, "colors: [\"#rrggbb\" | [r, g, b], ...]");
            }
        }
        free(table.colors);
        table.colors = nc;
        table.ncolors = m;
    }
    /* `entries`: {index: colour}. */
    yyjson_val *entriesv = pc_json_get(p, "entries");
    if (entriesv != NULL && yyjson_is_obj(entriesv)) {
        yyjson_val *k, *v;
        yyjson_obj_iter it = yyjson_obj_iter_with(entriesv);
        while ((k = yyjson_obj_iter_next(&it)) != NULL) {
            v = yyjson_obj_iter_get_val(k);
            const char *ks = yyjson_get_str(k);
            char *endp = NULL;
            long idx = ks != NULL ? strtol(ks, &endp, 10) : -1;
            if (ks == NULL || endp == ks || *endp != '\0' || idx < 0) {
                pc_color_table_fini(&cur);
                pc_color_table_fini(&table);
                return bad(err, CMD, "entry index `%s`", ks != NULL ? ks : "");
            }
            uint8_t col[3];
            if (!parse_color(v, col)) {
                pc_color_table_fini(&cur);
                pc_color_table_fini(&table);
                return bad(err, CMD, "entry %ld: bad colour", idx);
            }
            if ((size_t)idx >= table.ncolors) {
                pc_color_table_fini(&cur);
                pc_color_table_fini(&table);
                return bad(err, CMD, "entry %ld is past the table (%zu entries)", idx, table.ncolors);
            }
            table.colors[idx][0] = col[0];
            table.colors[idx][1] = col[1];
            table.colors[idx][2] = col[2];
        }
    }
    /* `transparent`: index | null. */
    yyjson_val *trv = pc_json_get(p, "transparent");
    if (trv != NULL && yyjson_is_null(trv)) {
        table.has_transparent = false;
    } else if (trv != NULL) {
        uint64_t u;
        if (!pc_json_as_u64(trv, &u) || u >= table.ncolors) {
            size_t last = table.ncolors > 0 ? table.ncolors - 1 : 0;
            pc_color_table_fini(&cur);
            pc_color_table_fini(&table);
            return bad(err, CMD, "transparent: an index from 0 to %zu or null", last);
        }
        table.has_transparent = true;
        table.transparent = (uint8_t)u;
    } else {
        /* A shrunk table can't keep a transparent entry past its end (#4186). */
        if (table.has_transparent && (size_t)table.transparent >= table.ncolors) {
            table.has_transparent = false;
        }
    }

    bool same = pc_color_table_eq(&table, &cur);
    if (same) {
        pc_status rc = report_table(out, &cur, err);
        pc_color_table_fini(&cur);
        pc_color_table_fini(&table);
        return rc;
    }
    /* Build the report before the table is moved into the edit. */
    pc_json_value report;
    pc_status rc = report_table(&report, &table, err);
    if (rc != PC_OK) {
        pc_color_table_fini(&cur);
        pc_color_table_fini(&table);
        return rc;
    }
    color_table_ctx ctx;
    if (pc_color_table_clone(&table, &ctx.table, err) != PC_OK) {
        pc_json_value_fini(&report);
        pc_color_table_fini(&cur);
        pc_color_table_fini(&table);
        return PC_ERR_NOMEM;
    }
    pc_color_table_fini(&cur);
    pc_color_table_fini(&table);
    rc = pc_engine_session_edit(s, "Color Table", color_table_edit, &ctx, err);
    pc_color_table_fini(&ctx.table); /* NULL-safe; edit zeroed it on success */
    if (rc != PC_OK) {
        pc_json_value_fini(&report);
        return rc;
    }
    *out = report;
    return PC_OK;
}

/* ---- Duotone ------------------------------------------------------------------------------ */

static const struct {
    const char *name;
    float color[3];
} DEFAULT_INKS[4] = {
    {"Black", {0.0f, 0.0f, 0.0f}},
    {"Warm Brown", {0.62f, 0.38f, 0.18f}},
    {"Gold", {0.86f, 0.68f, 0.18f}},
    {"Slate Blue", {0.27f, 0.38f, 0.58f}},
};

static pc_status make_default_ink(size_t i, pc_duotone_ink *out, pc_error *err)
{
    if (i < 4) {
        return pc_duotone_ink_new(DEFAULT_INKS[i].name, DEFAULT_INKS[i].color, out, err);
    }
    char name[32];
    (void)snprintf(name, sizeof name, "Ink %zu", i + 1);
    float black[3] = {0.0f, 0.0f, 0.0f};
    return pc_duotone_ink_new(name, black, out, err);
}

typedef struct {
    pc_duotone duotone; /* owned */
} duotone_ctx;

static pc_status duotone_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    duotone_ctx *c = ctx;
    doc->mode = PC_COLOR_MODE_DUOTONE;
    if (doc->has_duotone) {
        pc_duotone_fini(&doc->duotone);
    }
    doc->duotone = c->duotone;
    doc->has_duotone = true;
    memset(&c->duotone, 0, sizeof c->duotone);
    if (doc->has_color_table) {
        pc_color_table_fini(&doc->color_table);
        doc->has_color_table = false;
    }
    return PC_OK;
}

static pc_status run_duotone(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "image.mode.duotone";
    size_t n;
    const char *type = str_or(p, "type", "duotone");
    if (strcmp(type, "monotone") == 0) {
        n = 1;
    } else if (strcmp(type, "duotone") == 0) {
        n = 2;
    } else if (strcmp(type, "tritone") == 0) {
        n = 3;
    } else if (strcmp(type, "quadtone") == 0) {
        n = 4;
    } else {
        return bad(err, CMD, "type `%s` (monotone|duotone|tritone|quadtone)", type);
    }
    pc_duotone_ink *inks = pc_malloc_array(n, sizeof(pc_duotone_ink));
    if (inks == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    size_t built = 0;
    pc_status rc = PC_OK;
    yyjson_val *inksv = pc_json_get(p, "inks");
    for (size_t i = 0; i < n; i++) {
        rc = make_default_ink(i, &inks[i], err);
        if (rc != PC_OK) {
            break;
        }
        built++;
        if (inksv != NULL && yyjson_is_arr(inksv) && i < yyjson_arr_size(inksv)) {
            yyjson_val *ink = yyjson_arr_get(inksv, i);
            const char *nm = str_or(ink, "name", NULL);
            if (nm != NULL) {
                if (pc_str_set_c(&inks[i].name, nm, err) != PC_OK) {
                    rc = PC_ERR_NOMEM;
                    break;
                }
            }
            yyjson_val *cv = pc_json_get(ink, "color");
            uint8_t col[3];
            if (cv != NULL && parse_color(cv, col)) {
                inks[i].color[0] = (float)col[0] / 255.0f;
                inks[i].color[1] = (float)col[1] / 255.0f;
                inks[i].color[2] = (float)col[2] / 255.0f;
            }
            yyjson_val *curvev = pc_json_get(ink, "curve");
            if (curvev != NULL && yyjson_is_arr(curvev)) {
                size_t m = yyjson_arr_size(curvev);
                pc_curve_point *pts = m > 0 ? pc_malloc_array(m, sizeof(pc_curve_point)) : NULL;
                size_t np = 0;
                for (size_t k = 0; pts != NULL && k < m; k++) {
                    yyjson_val *pt = yyjson_arr_get(curvev, k);
                    if (!yyjson_is_arr(pt) || yyjson_arr_size(pt) < 2) {
                        continue;
                    }
                    double in_v, out_v;
                    if (pc_json_as_f64(yyjson_arr_get(pt, 0), &in_v) &&
                        pc_json_as_f64(yyjson_arr_get(pt, 1), &out_v)) {
                        pts[np].input = (float)(in_v / 100.0);
                        pts[np].output = (float)(out_v / 100.0);
                        np++;
                    }
                }
                /* sort by input ascending */
                for (size_t x = 1; x < np; x++) {
                    pc_curve_point key = pts[x];
                    size_t y = x;
                    while (y > 0 && pts[y - 1].input > key.input) {
                        pts[y] = pts[y - 1];
                        y--;
                    }
                    pts[y] = key;
                }
                if (np >= 2) {
                    pc_curve_fini(&inks[i].curve);
                    inks[i].curve.pts = pts;
                    inks[i].curve.len = np;
                    pts = NULL;
                }
                free(pts);
            }
        }
    }
    if (rc != PC_OK) {
        for (size_t i = 0; i < built; i++) {
            pc_duotone_ink_fini(&inks[i]);
        }
        free(inks);
        return rc;
    }
    /* result: ink names */
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        for (size_t i = 0; i < n; i++) {
            pc_duotone_ink_fini(&inks[i]);
        }
        free(inks);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_val *names = yyjson_mut_arr(jd);
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_arr_add_strcpy(jd, names, pc_str_c(&inks[i].name));
    }
    yyjson_mut_obj_add_val(jd, root, "inks", names);
    yyjson_mut_doc_set_root(jd, root);

    duotone_ctx ctx;
    ctx.duotone.inks = inks;
    ctx.duotone.ninks = n;
    ctx.duotone.psd_raw = NULL;
    pc_status erc = pc_engine_session_edit(s, "Duotone", duotone_edit, &ctx, err);
    pc_duotone_fini(&ctx.duotone); /* NULL-safe; edit zeroed it on success */
    if (erc != PC_OK) {
        pc_json_value_fini(out);
        return erc;
    }
    return PC_OK;
}

/* ---------- Image Rotation › Arbitrary ---------- */

static const double ROT_PI = 3.14159265358979323846; /* M_PI is not ISO C */

/* Canvas size after rotating `size` by `deg` (the canvas grows to fit, as in Photoshop). */
pc_size pc_engine_rotated_size(pc_size size, double deg)
{
    double rad = deg * (ROT_PI / 180.0);
    double sn = fabs(sin(rad)), cs = fabs(cos(rad));
    double w = (double)size.width, h = (double)size.height;
    double nw = ceil(w * cs + h * sn - 1e-6);
    double nh = ceil(w * sn + h * cs - 1e-6);
    if (nw < 1.0) {
        nw = 1.0;
    }
    if (nh < 1.0) {
        nh = 1.0;
    }
    return pc_size_make((uint32_t)nw, (uint32_t)nh);
}

static size_t count_layers_rec(const pc_layer *l)
{
    size_t n = 1;
    if (pc_layer_is_group(l)) {
        size_t nk = 0;
        const pc_layer *kids = pc_layer_children(l, &nk);
        for (size_t i = 0; i < nk; i++) {
            n += count_layers_rec(&kids[i]);
        }
    }
    return n;
}

/* Temporarily release the movement locks (position, all) on `l` and every descendant so a
 * canvas-wide rotation moves even locked layers (mode_cmds::with_released_movement_locks). The
 * second pass restores them. `saved` must hold count_layers_rec(l) entries. */
static void movement_locks_save_clear(pc_layer *l, pc_locks *saved, size_t *idx)
{
    saved[*idx] = l->locks;
    (*idx)++;
    l->locks.position = false;
    l->locks.all = false;
    if (pc_layer_is_group(l)) {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            movement_locks_save_clear(&g->children[i], saved, idx);
        }
    }
}

static void movement_locks_restore(pc_layer *l, const pc_locks *saved, size_t *idx)
{
    l->locks = saved[*idx];
    (*idx)++;
    if (pc_layer_is_group(l)) {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            movement_locks_restore(&g->children[i], saved, idx);
        }
    }
}

/* Fill `base` (format `fmt`, with alpha) with the background colour over the whole canvas, so the
 * rotated Background composites onto an opaque field (as the reference's write_region + repeat). */
static pc_status fill_canvas_bg(pc_surface *base, pc_rect canvas, pc_pixel_format fmt,
                                const float bg[4], pc_error *err)
{
    size_t ch = pc_pixel_format_channels(fmt);
    if (ch == 0 || pc_rect_is_empty(canvas)) {
        return PC_OK;
    }
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t m = pc_from_rgba(fmt, bg, enc);
    uint64_t px = (uint64_t)pc_rect_width(canvas) * (uint64_t)pc_rect_height(canvas);
    uint64_t total = px * (uint64_t)ch;
    if (px != 0 && total / px != (uint64_t)ch) {
        return pc_fail(err, PC_ERR_NOMEM, "rotated canvas fill");
    }
    float *raw = pc_malloc_array((size_t)total, sizeof(float));
    if (raw == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "rotated canvas fill");
    }
    for (uint64_t i = 0; i < px; i++) {
        for (size_t c = 0; c < ch; c++) {
            raw[i * ch + c] = c < m ? enc[c] : 0.0f;
        }
    }
    pc_status rc = pc_surface_write_region(base, canvas, raw, (size_t)total, err);
    free(raw);
    return rc;
}

typedef struct {
    double deg;
    pc_algo_interp interp;
    float bg[4];
} rotate_ctx;

static pc_status rotate_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    rotate_ctx *c = vctx;
    pc_size old = doc->size;
    pc_size nw = pc_engine_rotated_size(old, c->deg);
    double rad = c->deg * (ROT_PI / 180.0);
    double sn = sin(rad), cs = cos(rad);
    double cx = (double)old.width / 2.0, cy = (double)old.height / 2.0;
    double nx = (double)nw.width / 2.0, ny = (double)nw.height / 2.0;
    /* Clockwise on screen (y down): x' = c·x − s·y, y' = s·x + c·y about the old and new centres. */
    pc_affine af = {{cs, sn, -sn, cs, nx - (cs * cx - sn * cy), ny - (sn * cx + cs * cy)}};
    pc_algo_homography h = {{af.m[0], af.m[2], af.m[4], af.m[1], af.m[3], af.m[5], 0.0, 0.0, 1.0}};

    doc->size = nw;
    pc_rect canvas = pc_document_bounds(doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    float dpi = doc->resolution_dpi;
    const pc_layer *bgl = pc_document_background(doc);
    bool has_bg = bgl != NULL;
    pc_layer_id bg_id = has_bg ? bgl->id : 0;

    for (size_t i = 0; i < doc->nlayers; i++) {
        pc_layer *l = &doc->layers[i];
        pc_surface *surf = NULL;
        bool is_bg = has_bg && l->id == bg_id;
        if (is_bg && (surf = pc_layer_surface_mut(l)) != NULL) {
            /* The Background stays a Background: rotated pixels over the background colour. */
            pc_rect src = pc_surface_content_bounds(surf);
            pc_surface rotated;
            pc_status rc = pc_algo_warp_surface(surf, src, &h, c->interp, &rotated, err);
            if (rc != PC_OK) {
                return rc;
            }
            if (!pc_pixel_format_eq(pc_surface_format(&rotated), fmt)) {
                pc_surface conv;
                rc = pc_surface_convert(&rotated, fmt, &conv, err);
                pc_surface_fini(&rotated);
                if (rc != PC_OK) {
                    return rc;
                }
                rotated = conv;
            }
            pc_surface base;
            pc_surface_init(&base, fmt);
            rc = fill_canvas_bg(&base, canvas, fmt, c->bg, err);
            if (rc != PC_OK) {
                pc_surface_fini(&base);
                pc_surface_fini(&rotated);
                return rc;
            }
            rc = pc_engine_transform_composite_over_native(&base, &rotated, err);
            pc_surface_fini(&rotated);
            if (rc != PC_OK) {
                pc_surface_fini(&base);
                return rc;
            }
            pc_surface_prune(&base);
            pc_surface_fini(surf);
            *surf = base;
            if (l->has_mask) {
                pc_surface m;
                rc = pc_engine_transform_warp_gray(&l->mask.surface, &h, c->interp, &m, err);
                if (rc != PC_OK) {
                    return rc;
                }
                pc_surface_fini(&l->mask.surface);
                l->mask.surface = m;
            }
        } else {
            /* Rotating the whole image moves locked layers too, including inside groups. */
            size_t n = count_layers_rec(l);
            pc_locks *saved = pc_malloc_array(n, sizeof(pc_locks));
            if (saved == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "image.rotation.arbitrary");
            }
            size_t idx = 0;
            movement_locks_save_clear(l, saved, &idx);
            pc_locks def = {false, false, false, false, false};
            pc_status rc = pc_engine_transform_layer(NULL, def, l, &h, &af, c->interp, err);
            idx = 0;
            movement_locks_restore(l, saved, &idx);
            free(saved);
            if (rc != PC_OK) {
                return rc;
            }
        }
        /* What Free Transform leaves alone: unlinked vector masks, gradient angles, artboards… */
        pc_engine_transform_layer_geometry(l, &af, false, dpi);
    }
    pc_engine_transform_doc_marks(doc, &af);
    for (size_t i = 0; i < doc->nchannels; i++) {
        pc_surface m;
        pc_status rc = pc_engine_transform_warp_gray(&doc->channels[i].surface, &h, c->interp, &m, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&doc->channels[i].surface);
        doc->channels[i].surface = m;
    }
    if (doc->has_quick_mask) {
        pc_surface m;
        pc_status rc = pc_engine_transform_warp_gray(&doc->quick_mask.surface, &h, c->interp, &m, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&doc->quick_mask.surface);
        doc->quick_mask.surface = m;
    }
    if (doc->has_selection) {
        pc_surface w;
        pc_status rc = pc_engine_transform_warp_gray(&doc->selection, &h, PC_ALGO_INTERP_BILINEAR, &w, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_rect_is_empty(pc_surface_content_bounds(&w))) {
            pc_surface_fini(&w);
            pc_surface_fini(&doc->selection);
            doc->has_selection = false;
        } else {
            pc_surface_fini(&doc->selection);
            doc->selection = w;
        }
    }
    /* Type, shapes and smart objects re-render from their new geometry. */
    pc_engine_canvas_refresh(doc, true);
    pc_guides_fini(&doc->guides);
    memset(&doc->guides, 0, sizeof doc->guides);
    return PC_OK;
}

static pc_status rotate_result(pc_json_value *out, uint32_t w, uint32_t h, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "width", w);
    yyjson_mut_obj_add_uint(d, root, "height", h);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_rotate_arbitrary(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                      pc_error *err)
{
    const char *CMD = "image.rotation.arbitrary";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    double angle = (double)num(p, "angle", 0.0f);
    if (!isfinite(angle) || fabs(angle) > 3600.0) {
        return bad(err, CMD, "angle out of range");
    }
    const char *dir = str_or(p, "direction", "cw");
    double deg;
    if (strcmp(dir, "cw") == 0) {
        deg = angle;
    } else if (strcmp(dir, "ccw") == 0) {
        deg = -angle;
    } else {
        return bad(err, CMD, "direction `%s` (cw|ccw)", dir);
    }
    double norm = fmod(deg, 360.0);
    if (norm < 0.0) {
        norm += 360.0;
    }
    if (fabs(norm) < 1e-9) {
        const pc_document *doc = pc_doc_arc_get(d->doc);
        return rotate_result(out, doc->size.width, doc->size.height, err);
    }
    rotate_ctx c;
    c.deg = deg;
    c.interp = pc_algo_interp_parse(str_or(p, "interpolation", "bicubic"));
    const pc_engine_tool_state *t = pc_engine_session_tools(s);
    memcpy(c.bg, t->background, sizeof(float) * 4);
    /* Color Sampler points turn with the canvas (the same map the edit applies to the geometry). */
    const pc_document *doc0 = pc_doc_arc_get(d->doc);
    pc_size old = doc0->size;
    pc_size nw = pc_engine_rotated_size(old, deg);
    double rad = deg * (ROT_PI / 180.0);
    double sn = sin(rad), cs = cos(rad);
    double cx = (double)old.width / 2.0, cy = (double)old.height / 2.0;
    double nx = (double)nw.width / 2.0, ny = (double)nw.height / 2.0;
    pc_affine map = {{cs, sn, -sn, cs, nx - (cs * cx - sn * cy), ny - (sn * cx + cs * cy)}};
    s->has_sampler_map = true;
    s->sampler_map = map;
    pc_status rc = pc_engine_session_edit(s, "Rotate Canvas", rotate_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d2 = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d2->doc);
    return rotate_result(out, doc->size.width, doc->size.height, err);
}

/* ---- specs -------------------------------------------------------------------------------- */

static const char *const MENU_MODE[] = {"Image", "Mode"};
static const char *const MENU_ROT[] = {"Image", "Image Rotation"};

static const pc_engine_command MODE[] = {
    {"image.rotation.arbitrary", "Arbitrary\xE2\x80\xA6", MENU_ROT, 2, NULL,
     "{\"angle\":-359.99..359.99=0,\"direction\":\"cw|ccw\"=\"cw\",\"interpolation\":\"bicubic|"
     "bilinear|nearest\"=\"bicubic\"} -> {width,height}",
     pc_engine_has_doc, run_rotate_arbitrary, true},
    {"image.mode.indexedColor", "Indexed Color\xE2\x80\xA6", MENU_MODE, 2, NULL,
     "{\"palette\":\"selective|perceptual|adaptive|exact|systemMac|systemWindows|web|uniform\"="
     "\"selective\",\"colors\":2..256=256,\"forced\":\"blackWhite|none|primaries|web\"="
     "\"blackWhite\",\"transparency\":bool=true,\"dither\":\"diffusion|none|pattern|noise\"="
     "\"diffusion\",\"amount\":0..100=75} (flattens)",
     en_indexed, run_indexed, true},
    {"image.mode.colorTable", "Color Table\xE2\x80\xA6", MENU_MODE, 2, NULL,
     "{\"table\":\"custom|blackBody|grayscale|spectrum|systemMac|systemWindows|web\"=\"custom\","
     "\"colors\":json,\"entries\":json,\"transparent\":json}",
     en_color_table, run_color_table, true},
    {"image.mode.bitmap", "Bitmap\xE2\x80\xA6", MENU_MODE, 2, NULL,
     "{\"method\":\"diffusion|threshold|pattern|halftone\"=\"diffusion\",\"frequency\":1..999=53,"
     "\"angle\":-180..180=45,\"shape\":\"round|ellipse|line|square|diamond|cross\"=\"round\"} "
     "(flattens)",
     en_bitmap, run_bitmap, true},
    {"image.mode.duotone", "Duotone\xE2\x80\xA6", MENU_MODE, 2, NULL,
     "{\"type\":\"duotone|monotone|tritone|quadtone\"=\"duotone\",\"inks\":json}", en_duotone,
     run_duotone, true},
};

const pc_engine_command *pc_engine_mode_specs(size_t *n)
{
    *n = sizeof MODE / sizeof MODE[0];
    return MODE;
}
