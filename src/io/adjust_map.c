/* PSD adjustment-layer blocks -> adjustments (port of the parsing half of
 * crates/io/src/adjust_map.rs; the writer is adjust_write.c). Binary layouts follow the Adobe
 * spec ("Adjustment layer" section). Blocks that can't be read (noise gradients, profile lookups,
 * CMYK channel mixers, unknown versions, short data) become PC_ADJ_UNSUPPORTED with the raw
 * bytes, which export writes back verbatim. */
#include "io/adjust_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "cms/lutfile.h"
#include "color/convert.h"
#include "io/blocks.h"
#include "io/channel_map.h"
#include "io/desc_util.h"
#include "io/gradient_bake.h"

const char PC_IO_ADJUSTMENT_KEYS[16][5] = {"levl", "curv", "hue2", "brit", "nvrt", "thrs", "post", "expA",
                                           "vibA", "blnc", "mixr", "grdm", "phfl", "selc", "blwh", "clrL"};

/* Bounds-checked big-endian readers over (d, len). */
typedef struct adj_rd {
    const uint8_t *d;
    size_t len;
} adj_rd;

static bool be16(const adj_rd *r, size_t at, uint16_t *out)
{
    if (at > r->len || r->len - at < 2) {
        return false;
    }
    *out = (uint16_t)(r->d[at] << 8 | r->d[at + 1]);
    return true;
}

static bool bei16(const adj_rd *r, size_t at, int16_t *out)
{
    uint16_t v;
    if (!be16(r, at, &v)) {
        return false;
    }
    *out = (int16_t)(v > 0x7fff ? (int32_t)v - 0x10000 : (int32_t)v);
    return true;
}

static bool be32(const adj_rd *r, size_t at, uint32_t *out)
{
    if (at > r->len || r->len - at < 4) {
        return false;
    }
    *out =
        (uint32_t)r->d[at] << 24 | (uint32_t)r->d[at + 1] << 16 | (uint32_t)r->d[at + 2] << 8 | r->d[at + 3];
    return true;
}

static bool bef32(const adj_rd *r, size_t at, float *out)
{
    uint32_t u;
    if (!be32(r, at, &u)) {
        return false;
    }
    memcpy(out, &u, 4);
    return true;
}

static bool byte_at(const adj_rd *r, size_t at, uint8_t *out)
{
    if (at >= r->len) {
        return false;
    }
    *out = r->d[at];
    return true;
}

static size_t sat_add(size_t a, size_t b)
{
    return a > SIZE_MAX - b ? SIZE_MAX : a + b;
}

/* The parse result: an adjustment, "not readable" (keep raw), or out of memory. */
typedef enum parsed { P_OK, P_NONE, P_NOMEM } parsed;

static bool levels_rec(const adj_rd *r, size_t at, pc_levels_channel *out)
{
    uint16_t v[5];
    for (size_t i = 0; i < 5; i++) {
        if (!be16(r, at + 2 * i, &v[i])) {
            return false;
        }
    }
    out->in_black = (float)v[0] / 255.0f;
    out->in_white = (float)v[1] / 255.0f;
    out->out_black = (float)v[2] / 255.0f;
    out->out_white = (float)v[3] / 255.0f;
    out->gamma = (float)v[4] / 100.0f;
    return true;
}

static bool line(pc_curve *c)
{
    c->pts = pc_malloc_array(2, sizeof *c->pts);
    if (c->pts == NULL) {
        c->len = 0;
        return false;
    }
    c->pts[0] = (pc_curve_point){0.0f, 0.0f};
    c->pts[1] = (pc_curve_point){1.0f, 1.0f};
    c->len = 2;
    return true;
}

static parsed parse_curves(const adj_rd *r, pc_adjustment *a)
{
    uint16_t version;
    uint32_t bits;
    size_t at = 7;
    uint8_t flag = 0;
    bool pencil;
    pc_curve curves[5];
    memset(curves, 0, sizeof curves);
    /* map flag(1) version(2) bitmap(4). A map (Photoshop's pencil-drawn curves) stores 256 output
     * bytes per channel instead of a point list. */
    if (!byte_at(r, 0, &flag) || !be16(r, 1, &version) || (version != 1 && version != 4) ||
        !be32(r, 3, &bits)) {
        return P_NONE;
    }
    pencil = flag != 0;
    for (size_t i = 0; i < 5; i++) {
        if (!line(&curves[i])) {
            goto nomem;
        }
    }
    for (size_t bit = 0; bit < 32; bit++) {
        uint16_t n;
        pc_curve pts = {NULL, 0};
        if ((bits & (1u << bit)) == 0) {
            continue;
        }
        if (pencil) {
            if (r->len < at || r->len - at < PC_CURVE_MAP_LEN) {
                goto none;
            }
            if (bit < 5) {
                float outs[PC_CURVE_MAP_LEN];
                pc_curve m = {NULL, 0};
                for (size_t k = 0; k < PC_CURVE_MAP_LEN; k++) {
                    outs[k] = (float)r->d[at + k] / 255.0f;
                }
                if (pc_curve_map(outs, PC_CURVE_MAP_LEN, &m, NULL) != PC_OK) {
                    goto nomem;
                }
                pc_curve_fini(&curves[bit]);
                curves[bit] = m;
            }
            at += PC_CURVE_MAP_LEN;
            continue;
        }
        if (!be16(r, at, &n)) {
            goto none;
        }
        at += 2;
        /* Every point needs 4 bytes: a short block fails like the reference's reads would. */
        if (r->len < at || (r->len - at) / 4 < n) {
            goto none;
        }
        if (n > 0 && bit < 5) {
            pts.pts = pc_malloc_array(n, sizeof *pts.pts);
            if (pts.pts == NULL) {
                goto nomem;
            }
        }
        for (size_t k = 0; k < n; k++) {
            uint16_t out = 0;
            uint16_t inp = 0;
            (void)be16(r, at, &out);
            (void)be16(r, at + 2, &inp);
            at += 4;
            if (pts.pts != NULL) {
                pts.pts[pts.len++] = (pc_curve_point){(float)inp / 255.0f, (float)out / 255.0f};
            }
        }
        if (bit < 5) {
            pc_curve_fini(&curves[bit]);
            curves[bit] = pts;
        }
    }
    a->kind = PC_ADJ_CURVES;
    a->u.curves.master = curves[0];
    a->u.curves.per_channel[0] = curves[1];
    a->u.curves.per_channel[1] = curves[2];
    a->u.curves.per_channel[2] = curves[3];
    a->u.curves.space = PC_TONE_RGB;
    a->u.curves.black = curves[4];
    a->u.curves.pencil = pencil;
    return P_OK;
none:
    for (size_t i = 0; i < 5; i++) {
        pc_curve_fini(&curves[i]);
    }
    return P_NONE;
nomem:
    for (size_t i = 0; i < 5; i++) {
        pc_curve_fini(&curves[i]);
    }
    return P_NOMEM;
}

/* adjust_map::desc_num: Integer, Double, UnitFloat as f32. */
static bool desc_num(const pc_psd_desc *d, const char *key, float *out)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    if (v == NULL) {
        return false;
    }
    switch (v->kind) {
    case PC_PSD_V_INTEGER:
        *out = (float)v->i;
        return true;
    case PC_PSD_V_DOUBLE:
    case PC_PSD_V_UNIT_FLOAT:
        *out = pc_f64_to_f32(v->d);
        return true;
    default:
        return false;
    }
}

static bool desc_bool(const pc_psd_desc *d, const char *key, bool *out)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    if (v == NULL || v->kind != PC_PSD_V_BOOLEAN) {
        return false;
    }
    *out = v->b;
    return true;
}

/* A versioned descriptor at the start of data; P_NONE when it doesn't parse. */
static parsed versioned(const uint8_t *data, size_t len, pc_psd_desc *out)
{
    bool found = false;
    pc_status st = pc_io_parse_prefix_versioned(data, len, &found, out, NULL);
    if (st != PC_OK) {
        return P_NOMEM;
    }
    return found ? P_OK : P_NONE;
}

static parsed parse_brit(const adj_rd *r, const uint8_t *cged, size_t cged_len, pc_adjustment *a)
{
    int16_t b;
    int16_t c;
    if (cged != NULL) {
        pc_psd_desc d;
        parsed p = versioned(cged, cged_len, &d);
        if (p == P_NOMEM) {
            return p;
        }
        if (p == P_OK) {
            float br;
            float co;
            bool legacy = false;
            bool ok = desc_num(&d, "Brgh", &br) && desc_num(&d, "Cntr", &co);
            (void)desc_bool(&d, "useLegacy", &legacy);
            pc_psd_desc_fini(&d);
            if (ok) {
                a->kind = PC_ADJ_BRIGHTNESS_CONTRAST;
                a->u.brightness_contrast.brightness = br;
                a->u.brightness_contrast.contrast = co;
                a->u.brightness_contrast.legacy = legacy;
                return P_OK;
            }
        }
    }
    if (!bei16(r, 0, &b) || !bei16(r, 2, &c)) {
        return P_NONE;
    }
    a->kind = PC_ADJ_BRIGHTNESS_CONTRAST;
    a->u.brightness_contrast.brightness = (float)b;
    a->u.brightness_contrast.contrast = (float)c;
    a->u.brightness_contrast.legacy = true;
    return P_OK;
}

static parsed parse_hue(const adj_rd *r, pc_adjustment *a)
{
    uint8_t col;
    size_t base;
    int16_t h;
    int16_t s;
    int16_t l;
    pc_hue_range ranges[6];
    if (!byte_at(r, 2, &col)) {
        return P_NONE;
    }
    base = col != 0 ? 4 : 10;
    /* Six ranges from byte 16: four range values (degrees) then hue, saturation, lightness. */
    pc_hue_range_defaults(ranges);
    for (size_t i = 0; i < 6; i++) {
        size_t at = 16 + i * 14;
        int16_t v[7];
        bool all = true;
        for (size_t k = 0; k < 7 && all; k++) {
            all = bei16(r, at + 2 * k, &v[k]);
        }
        if (all) {
            float b[4] = {(float)v[0], (float)v[1], (float)v[2], (float)v[3]};
            ranges[i].hue = (float)v[4];
            ranges[i].saturation = (float)v[5];
            ranges[i].lightness = (float)v[6];
            pc_hue_range_canonical_bounds(b, ranges[i].bounds);
        }
    }
    if (!bei16(r, base, &h) || !bei16(r, base + 2, &s) || !bei16(r, base + 4, &l)) {
        return P_NONE;
    }
    a->kind = PC_ADJ_HUE_SATURATION;
    a->u.hue_saturation.hue = (float)h;
    a->u.hue_saturation.saturation = (float)s;
    a->u.hue_saturation.lightness = (float)l;
    a->u.hue_saturation.colorize = col != 0;
    memcpy(a->u.hue_saturation.ranges, ranges, sizeof ranges);
    return P_OK;
}

/* grdm (Adobe spec, "Gradient settings"): version (1, or 3 with an interpolation method key),
 * reverse, dither, [method], name, colour stops (location /4096, midpoint %, colour space + four
 * u16 components, 2 bytes), transparency stops, then smoothness (/4096) among noise-gradient
 * fields. Smoothness, midpoints and the method are baked into dense stops like gradient fills.
 * Stops may be in any plain colour space Photoshop writes (RGB, HSB, CMYK, Lab, Grayscale);
 * they are converted to RGB. Noise gradients (no colour stops) and stops in other spaces (book
 * colours) stay unsupported. */
static parsed parse_gradient_map(const adj_rd *r, pc_adjustment *a)
{
    uint16_t version;
    uint8_t rev;
    uint8_t dith = 0;
    const uint8_t *method = NULL;
    size_t at;
    uint32_t name_len;
    uint16_t n;
    uint16_t nt;
    uint16_t sm;
    pc_color_stop *stops = NULL;
    float *mids_raw = NULL;
    float *mids = NULL;
    float *keys = NULL;
    size_t *order = NULL;
    pc_io_color_stop *in = NULL;
    pc_io_color_stop *baked = NULL;
    size_t nbaked = 0;
    size_t ns = 0;
    parsed res = P_NONE;
    if (!be16(r, 0, &version) || !byte_at(r, 2, &rev)) {
        return P_NONE;
    }
    (void)byte_at(r, 3, &dith);
    if (version == 1) {
        at = 4;
    } else if (version == 3) {
        if (r->len < 8) {
            return P_NONE;
        }
        method = r->d + 4;
        at = 8;
    } else {
        return P_NONE;
    }
    if (!be32(r, at, &name_len)) {
        return P_NONE;
    }
    at = sat_add(sat_add(at, 4), (size_t)name_len * 2);
    if (!be16(r, at, &n)) {
        return P_NONE;
    }
    at = sat_add(at, 2);
    stops = pc_malloc_array(n > 0 ? n : 1, sizeof *stops);
    mids_raw = pc_malloc_array(n > 0 ? n : 1, sizeof *mids_raw);
    if (stops == NULL || mids_raw == NULL) {
        res = P_NOMEM;
        goto done;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t loc;
        uint32_t mid;
        uint16_t space;
        uint16_t c[4];
        if (!be32(r, at, &loc) || !be32(r, at + 4, &mid) || !be16(r, at + 8, &space) ||
            !be16(r, at + 10, &c[0]) || !be16(r, at + 12, &c[1]) || !be16(r, at + 14, &c[2]) ||
            !be16(r, at + 16, &c[3]) || !pc_io_decode_known_color(space, c, &stops[ns].color)) {
            goto done;
        }
        stops[ns].pos = pc_clampf((float)loc / 4096.0f, 0.0f, 1.0f);
        mids_raw[ns] = (float)mid / 100.0f;
        ns++;
        at = sat_add(at, 20);
    }
    if (ns < 2 || !be16(r, at, &nt)) {
        goto done;
    }
    /* transparency stops, expansion count */
    at = sat_add(sat_add(at, 2), (size_t)nt * 10 + 2);
    if (!be16(r, at, &sm)) {
        goto done;
    }
    /* Midpoint k applies to the segment after stop k (in location order). */
    order = pc_malloc_array(ns, sizeof *order);
    keys = pc_malloc_array(ns, sizeof *keys);
    mids = pc_malloc_array(ns, sizeof *mids);
    in = pc_malloc_array(ns, sizeof *in);
    if (order == NULL || keys == NULL || mids == NULL || in == NULL) {
        res = P_NOMEM;
        goto done;
    }
    for (size_t i = 0; i < ns; i++) {
        order[i] = i;
        keys[i] = stops[i].pos;
        in[i].at = stops[i].pos;
        in[i].color = stops[i].color;
    }
    if (!pc_io_sort_idx_f32(order, ns, keys)) {
        res = P_NOMEM;
        goto done;
    }
    for (size_t k = 1; k < ns; k++) {
        mids[k - 1] = mids_raw[order[k]];
    }
    if (pc_io_gradient_bake(in, ns, mids, ns - 1, (float)sm / 4096.0f,
                            pc_io_bake_method_from_code(method, method != NULL ? 4 : 0), &baked, &nbaked,
                            NULL) != PC_OK) {
        res = P_NOMEM;
        goto done;
    }
    {
        pc_gradient_map_stop *out = pc_malloc_array(nbaked > 0 ? nbaked : 1, sizeof *out);
        size_t *o2;
        float *k2;
        if (out == NULL) {
            res = P_NOMEM;
            goto done;
        }
        o2 = pc_malloc_array(nbaked > 0 ? nbaked : 1, sizeof *o2);
        k2 = pc_malloc_array(nbaked > 0 ? nbaked : 1, sizeof *k2);
        if (o2 == NULL || k2 == NULL) {
            free(out);
            free(o2);
            free(k2);
            res = P_NOMEM;
            goto done;
        }
        for (size_t i = 0; i < nbaked; i++) {
            o2[i] = i;
            k2[i] = baked[i].at;
        }
        if (!pc_io_sort_idx_f32(o2, nbaked, k2)) {
            free(out);
            free(o2);
            free(k2);
            res = P_NOMEM;
            goto done;
        }
        for (size_t i = 0; i < nbaked; i++) {
            out[i].pos = baked[o2[i]].at;
            pc_color_to_rgb(&baked[o2[i]].color, out[i].rgb);
        }
        free(o2);
        free(k2);
        a->kind = PC_ADJ_GRADIENT_MAP;
        a->u.gradient_map.stops = nbaked > 0 ? out : NULL;
        if (nbaked == 0) {
            free(out);
        }
        a->u.gradient_map.nstops = nbaked;
        a->u.gradient_map.reverse = rev != 0;
        a->u.gradient_map.dither = dith != 0;
        res = P_OK;
    }
done:
    free(stops);
    free(mids_raw);
    free(mids);
    free(keys);
    free(order);
    free(in);
    free(baked);
    return res;
}

/* selc: version, method (0 relative / 1 absolute), then 10 CMYK records of i16 percentages;
 * record 0 is reserved, records 1..=9 are reds ... blacks. */
static parsed parse_selective(const adj_rd *r, pc_adjustment *a)
{
    uint16_t v;
    uint16_t method;
    if (!be16(r, 0, &v) || v != 1 || r->len < 84 || !be16(r, 2, &method)) {
        return P_NONE;
    }
    for (size_t rec = 0; rec < 9; rec++) {
        for (size_t k = 0; k < 4; k++) {
            int16_t x = 0;
            (void)bei16(r, 4 + (rec + 1) * 8 + k * 2, &x);
            a->u.selective_color.adjustments[rec][k] = (float)x;
        }
    }
    a->kind = PC_ADJ_SELECTIVE_COLOR;
    a->u.selective_color.relative = method == 0;
    return P_OK;
}

static bool enum_is(const pc_psd_desc *d, const char *key, const char *s)
{
    size_t n = 0;
    const uint8_t *e = pc_io_desc_enum(d, key, &n);
    return e != NULL && n == strlen(s) && memcmp(e, s, n) == 0;
}

/* clrL: version 1 + a versioned descriptor carrying the LUT file itself (LUT3DFileData, in the
 * format named by LUTFormat). Profile-based lookups (abstract / device link) have no table and
 * stay unsupported. On P_NONE, when reason != NULL, the import-warning reason is appended to it
 * (matching adjust_map::read_lookup's Err text). */
static parsed read_lookup(const adj_rd *r, pc_adjustment *a, pc_buf *reason)
{
    uint16_t v;
    pc_psd_desc d;
    const pc_psd_value *data;
    const char *ext;
    const char *format;
    pc_lutfile lut;
    pc_error e;
    pc_str name = PC_STR_INIT;
    bool found = false;
    bool dither = false;
    size_t used = 0;
    pc_blob *blob = NULL;
    pc_status st;
    if (!be16(r, 0, &v)) {
        if (reason != NULL) {
            (void)pc_buf_append_str(reason, "the block is truncated", NULL);
        }
        return P_NONE;
    }
    if (v != 1) {
        if (reason != NULL) {
            (void)pc_buf_appendf(reason, NULL, "unknown version %u", (unsigned)v);
        }
        return P_NONE;
    }
    st = pc_psd_vdesc_parse_prefix(r->d + 2, r->len - 2, &d, &used, &e);
    if (st == PC_ERR_NOMEM) {
        return P_NOMEM;
    }
    if (st != PC_OK) {
        if (reason != NULL) {
            (void)pc_buf_appendf(reason, NULL, "unreadable settings (%s)", e.msg);
        }
        return P_NONE;
    }
    data = pc_psd_desc_get(&d, "LUT3DFileData");
    if (data == NULL) {
        pc_psd_desc_fini(&d);
        if (reason != NULL) {
            (void)pc_buf_append_str(reason,
                                    "it uses a colour profile (abstract or device link), not a "
                                    "LUT file, which isn't supported yet",
                                    NULL);
        }
        return P_NONE;
    }
    if (data->kind != PC_PSD_V_RAW_DATA || data->raw.len == 0) {
        pc_psd_desc_fini(&d);
        if (reason != NULL) {
            (void)pc_buf_append_str(reason, "its LUT data is empty", NULL);
        }
        return P_NONE;
    }
    if (enum_is(&d, "LUTFormat", "LUTFormat3DL")) {
        ext = "x.3dl";
        format = "3DL";
    } else if (enum_is(&d, "LUTFormat", "LUTFormatLOOK")) {
        ext = "x.look";
        format = "LOOK";
    } else {
        ext = "x.cube";
        format = "CUBE";
    }
    st = pc_lutfile_parse(ext, data->raw.data, data->raw.len, &lut, &e);
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        if (st == PC_ERR_NOMEM) {
            return P_NOMEM;
        }
        if (reason != NULL) {
            (void)pc_buf_appendf(reason, NULL, "its %s LUT can't be read (%s)", format, e.msg);
        }
        return P_NONE;
    }
    st = pc_io_desc_text(&d, "LUT3DFileName", &found, &name, NULL);
    if (st == PC_OK && !found) {
        st = pc_io_desc_text(&d, "NM  ", &found, &name, NULL);
    }
    (void)desc_bool(&d, "Dthr", &dither);
    pc_psd_desc_fini(&d);
    if (st == PC_OK) {
        st = pc_blob_new(lut.data, lut.ndata * sizeof(float), &blob, NULL);
    }
    if (st != PC_OK) {
        pc_lutfile_fini(&lut);
        pc_str_fini(&name);
        return P_NOMEM;
    }
    a->kind = PC_ADJ_COLOR_LOOKUP;
    a->u.color_lookup.name = name;
    a->u.color_lookup.size = (uint32_t)lut.size;
    a->u.color_lookup.lut = blob;
    a->u.color_lookup.tetrahedral = false;
    a->u.color_lookup.dither = dither;
    pc_lutfile_fini(&lut);
    return P_OK;
}

static parsed parse_lookup(const adj_rd *r, pc_adjustment *a)
{
    return read_lookup(r, a, NULL);
}

/* vibA (Adobe spec, "Vibrance"): a versioned descriptor with vibrance and Strt (saturation),
 * both integers in -100..=100. Photoshop omits a key whose value is 0. */
static parsed parse_vibrance(const adj_rd *r, pc_adjustment *a)
{
    pc_psd_desc d;
    float v = 0.0f;
    float s = 0.0f;
    parsed p = versioned(r->d, r->len, &d);
    if (p != P_OK) {
        return p;
    }
    (void)desc_num(&d, "vibrance", &v);
    (void)desc_num(&d, "Strt", &s);
    pc_psd_desc_fini(&d);
    a->kind = PC_ADJ_VIBRANCE;
    a->u.vibrance.vibrance = pc_clampf(v, -100.0f, 100.0f);
    a->u.vibrance.saturation = pc_clampf(s, -100.0f, 100.0f);
    return P_OK;
}

/* blnc (Adobe spec, "Color Balance"): shadows, midtones and highlights as three i16 each
 * (cyan-red, magenta-green, yellow-blue, -100..=100), then preserve luminosity (1 byte). */
static parsed parse_color_balance(const adj_rd *r, pc_adjustment *a)
{
    float t[9];
    uint8_t pl;
    for (size_t i = 0; i < 9; i++) {
        int16_t x;
        if (!bei16(r, i * 2, &x)) {
            return P_NONE;
        }
        t[i] = pc_clampf((float)x, -100.0f, 100.0f);
    }
    if (!byte_at(r, 18, &pl)) {
        return P_NONE;
    }
    a->kind = PC_ADJ_COLOR_BALANCE;
    memcpy(a->u.color_balance.shadows, t, sizeof(float) * 3);
    memcpy(a->u.color_balance.midtones, t + 3, sizeof(float) * 3);
    memcpy(a->u.color_balance.highlights, t + 6, sizeof(float) * 3);
    a->u.color_balance.preserve_luminosity = pl != 0;
    return P_OK;
}

/* Photoshop's default Black & White mix (reds, yellows, greens, cyans, blues, magentas). */
static const float BW_DEFAULT[6] = {40.0f, 60.0f, 40.0f, 60.0f, 20.0f, 80.0f};
static const char BW_KEYS[6][5] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};

static void bw_tint(float out[3])
{
    out[0] = 225.0f / 255.0f;
    out[1] = 211.0f / 255.0f;
    out[2] = 179.0f / 255.0f;
}

/* An RGBC colour (0..255 doubles) read in f64 so the writer's encoding comes back bit-exact;
 * other colour classes go through pc_io_color_from_desc. */
static bool desc_rgb(const pc_psd_desc *d, float out[3])
{
    size_t n = 0;
    const uint8_t *cls = pc_psd_id_bytes(&d->class_id, &n);
    if (cls != NULL && n == 4 && memcmp(cls, "RGBC", 4) == 0 && pc_psd_desc_get(d, "redFloat") == NULL) {
        static const char keys[3][5] = {"Rd  ", "Grn ", "Bl  "};
        double c[3];
        for (size_t i = 0; i < 3; i++) {
            if (!pc_io_desc_num(pc_psd_desc_get(d, keys[i]), &c[i])) {
                return false;
            }
        }
        for (size_t i = 0; i < 3; i++) {
            out[i] = pc_f64_to_f32(pc_clampd(c[i] / 255.0, 0.0, 1.0));
        }
        return true;
    }
    {
        pc_color col;
        float rgb[3];
        if (!pc_io_color_from_desc(d, &col)) {
            return false;
        }
        pc_color_to_rgb(&col, rgb);
        for (size_t i = 0; i < 3; i++) {
            out[i] = pc_clampf(rgb[i], 0.0f, 1.0f);
        }
        return true;
    }
}

/* blwh (Adobe spec, "Black and White"): a versioned descriptor with the six colour weights
 * (percent), useTint and tintColor. */
static parsed parse_black_white(const adj_rd *r, pc_adjustment *a)
{
    pc_psd_desc d;
    bool use = false;
    parsed p = versioned(r->d, r->len, &d);
    if (p != P_OK) {
        return p;
    }
    a->kind = PC_ADJ_BLACK_WHITE;
    for (size_t i = 0; i < 6; i++) {
        float v;
        a->u.black_white.weights[i] =
            desc_num(&d, BW_KEYS[i], &v) ? pc_clampf(v, -200.0f, 300.0f) : BW_DEFAULT[i];
    }
    (void)desc_bool(&d, "useTint", &use);
    a->u.black_white.has_tint = use;
    if (use) {
        const pc_psd_desc *t = pc_io_desc_get(&d, "tintColor");
        if (t == NULL || !desc_rgb(t, a->u.black_white.tint)) {
            bw_tint(a->u.black_white.tint);
        }
    }
    pc_psd_desc_fini(&d);
    return P_OK;
}

/* phfl (Adobe spec, "Photo Filter"): version (2 or 3); version 2 has a colour structure (colour
 * space + four u16), version 3 "4 bytes each for XYZ color"; then density (u32 percent) and
 * preserve luminosity (1 byte). The XYZ values are read as D50 XYZ (the ICC PCS) with 1.0 = 32768
 * (the ICC 16-bit PCSXYZ encoding); inferred, not documented. Version 3 is read, never written. */
static parsed parse_photo_filter(const adj_rd *r, pc_adjustment *a)
{
    uint16_t version;
    float rgb[3];
    size_t at;
    uint32_t density;
    uint8_t pl;
    if (!be16(r, 0, &version)) {
        return P_NONE;
    }
    if (version == 2) {
        uint16_t space;
        uint16_t c[4];
        pc_color col;
        if (!be16(r, 2, &space) || (space != 0 && space != 2 && space != 7 && space != 8)) {
            return P_NONE;
        }
        for (size_t i = 0; i < 4; i++) {
            if (!be16(r, 4 + 2 * i, &c[i])) {
                return P_NONE;
            }
        }
        col = pc_io_decode_color(space, c);
        pc_color_to_rgb(&col, rgb);
        at = 12;
    } else if (version == 3) {
        uint32_t u[3];
        float xyz[3];
        float lin[3];
        for (size_t i = 0; i < 3; i++) {
            if (!be32(r, 2 + 4 * i, &u[i])) {
                return P_NONE;
            }
            /* The stored components are an XYZ colour scaled by 32768 (unsigned). */
            xyz[i] = (float)u[i] / 32768.0f;
        }
        /* Real colours stay below about 1.0; anything past 2.0 is not an XYZ colour. */
        if (xyz[0] > 2.0f || xyz[1] > 2.0f || xyz[2] > 2.0f) {
            return P_NONE;
        }
        pc_color_mat3_mul(PC_XYZ_D50_TO_SRGB, xyz, lin);
        for (size_t i = 0; i < 3; i++) {
            rgb[i] = pc_linear_to_srgb(pc_clampf(lin[i], 0.0f, 1.0f));
        }
        at = 14;
    } else {
        return P_NONE;
    }
    if (!be32(r, at, &density) || !byte_at(r, at + 4, &pl)) {
        return P_NONE;
    }
    a->kind = PC_ADJ_PHOTO_FILTER;
    for (size_t i = 0; i < 3; i++) {
        a->u.photo_filter.color[i] = pc_clampf(rgb[i], 0.0f, 1.0f);
    }
    a->u.photo_filter.density = pc_clampf((float)density / 100.0f, 0.0f, 1.0f);
    a->u.photo_filter.preserve_luminosity = pl != 0;
    return P_OK;
}

/* One 10-byte Channel Mixer record: red, green, blue sources and the constant (fourth source
 * skipped), percent / 100. */
static bool mixer_rec(const adj_rd *r, size_t rec, float out[4])
{
    static const size_t ks[4] = {0, 1, 2, 4};
    for (size_t i = 0; i < 4; i++) {
        int16_t x;
        if (!bei16(r, 4 + rec * 10 + ks[i] * 2, &x)) {
            return false;
        }
        out[i] = pc_clampf((float)x, -200.0f, 200.0f) / 100.0f;
    }
    return true;
}

/* mixr (Adobe spec, "Channel Mixer"): version (1), monochrome (u16), then one 10-byte record per
 * output channel. With monochrome on, the first record is the gray mix (ag-psd, MIT). */
static parsed parse_channel_mixer(const adj_rd *r, pc_adjustment *a)
{
    uint16_t v;
    uint16_t mono;
    float m[3][4];
    if (!be16(r, 0, &v) || v != 1 || !be16(r, 2, &mono) || !mixer_rec(r, 0, m[0])) {
        return P_NONE;
    }
    if (mono != 0) {
        /* A monochrome block may carry only the gray record. */
        for (size_t k = 1; k < 3; k++) {
            if (!mixer_rec(r, k, m[k])) {
                memset(m[k], 0, sizeof m[k]);
            }
        }
    } else if (!mixer_rec(r, 1, m[1]) || !mixer_rec(r, 2, m[2])) {
        return P_NONE;
    }
    a->kind = PC_ADJ_CHANNEL_MIXER;
    memcpy(a->u.channel_mixer.matrix, m, sizeof m);
    a->u.channel_mixer.monochrome = mono != 0;
    return P_OK;
}

static parsed parse_any(const uint8_t key[4], const adj_rd *r, const uint8_t *cged, size_t cged_len,
                        pc_adjustment *a)
{
    uint16_t v;
    if (memcmp(key, "nvrt", 4) == 0) {
        a->kind = PC_ADJ_INVERT;
        return P_OK;
    }
    if (memcmp(key, "thrs", 4) == 0) {
        if (!be16(r, 0, &v)) {
            return P_NONE;
        }
        a->kind = PC_ADJ_THRESHOLD;
        a->u.threshold.level = (float)v / 255.0f;
        return P_OK;
    }
    if (memcmp(key, "post", 4) == 0) {
        if (!be16(r, 0, &v)) {
            return P_NONE;
        }
        a->kind = PC_ADJ_POSTERIZE;
        a->u.posterize.levels = v;
        return P_OK;
    }
    if (memcmp(key, "brit", 4) == 0) {
        return parse_brit(r, cged, cged_len, a);
    }
    if (memcmp(key, "hue2", 4) == 0) {
        return parse_hue(r, a);
    }
    if (memcmp(key, "expA", 4) == 0) {
        float e;
        float o;
        float g;
        if (!bef32(r, 2, &e) || !bef32(r, 6, &o) || !bef32(r, 10, &g)) {
            return P_NONE;
        }
        a->kind = PC_ADJ_EXPOSURE;
        a->u.exposure.exposure = e;
        a->u.exposure.offset = o;
        a->u.exposure.gamma = g;
        return P_OK;
    }
    if (memcmp(key, "levl", 4) == 0) {
        pc_levels_channel c[5];
        if (!levels_rec(r, 2, &c[0]) || !levels_rec(r, 12, &c[1]) || !levels_rec(r, 22, &c[2]) ||
            !levels_rec(r, 32, &c[3])) {
            return P_NONE;
        }
        if (!levels_rec(r, 42, &c[4])) {
            c[4] = pc_levels_channel_default();
        }
        a->kind = PC_ADJ_LEVELS;
        a->u.levels.master = c[0];
        a->u.levels.per_channel[0] = c[1];
        a->u.levels.per_channel[1] = c[2];
        a->u.levels.per_channel[2] = c[3];
        a->u.levels.space = PC_TONE_RGB;
        a->u.levels.black = c[4];
        return P_OK;
    }
    if (memcmp(key, "curv", 4) == 0) {
        return parse_curves(r, a);
    }
    if (memcmp(key, "selc", 4) == 0) {
        return parse_selective(r, a);
    }
    if (memcmp(key, "clrL", 4) == 0) {
        return parse_lookup(r, a);
    }
    if (memcmp(key, "grdm", 4) == 0) {
        return parse_gradient_map(r, a);
    }
    if (memcmp(key, "vibA", 4) == 0) {
        return parse_vibrance(r, a);
    }
    if (memcmp(key, "blnc", 4) == 0) {
        return parse_color_balance(r, a);
    }
    if (memcmp(key, "blwh", 4) == 0) {
        return parse_black_white(r, a);
    }
    if (memcmp(key, "phfl", 4) == 0) {
        return parse_photo_filter(r, a);
    }
    if (memcmp(key, "mixr", 4) == 0) {
        return parse_channel_mixer(r, a);
    }
    return P_NONE;
}

static pc_status unsupported(const uint8_t key[4], const uint8_t *data, size_t len, pc_adjustment *out,
                             pc_error *err)
{
    pc_adjustment a;
    pc_buf k = PC_BUF_INIT;
    pc_status st;
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_UNSUPPORTED;
    st = pc_utf8_lossy(key, 4, &k, err);
    st = st != PC_OK ? st : pc_str_set(&a.u.unsupported.psd_key, (const char *)k.data, k.len, err);
    pc_buf_fini(&k);
    if (st == PC_OK && len > 0) {
        st = pc_blob_new(data, len, &a.u.unsupported.raw, err);
    }
    if (st != PC_OK) {
        pc_adjustment_fini(&a);
        return st;
    }
    *out = a;
    return PC_OK;
}

/* Makes c = [g, g, g] from per_channel[0] (curves). */
static bool copy_curve(const pc_curve *src, pc_curve *dst)
{
    pc_curve c = {NULL, 0};
    if (src->len > 0) {
        c.pts = pc_malloc_array(src->len, sizeof *c.pts);
        if (c.pts == NULL) {
            return false;
        }
        memcpy(c.pts, src->pts, src->len * sizeof *c.pts);
        c.len = src->len;
    }
    *dst = c;
    return true;
}

const char *pc_io_adjust_label(const uint8_t key[4])
{
    static const struct {
        char k[5];
        const char *name;
    } labels[] = {
        {"levl", "Levels"},    {"curv", "Curves"},     {"hue2", "Hue/Saturation"},
        {"brit", "Brightness/Contrast"}, {"nvrt", "Invert"}, {"thrs", "Threshold"},
        {"post", "Posterize"}, {"expA", "Exposure"},   {"vibA", "Vibrance"},
        {"blnc", "Color Balance"}, {"mixr", "Channel Mixer"}, {"grdm", "Gradient Map"},
        {"phfl", "Photo Filter"}, {"selc", "Selective Color"}, {"blwh", "Black & White"},
        {"clrL", "Color Lookup"},
    };
    for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++) {
        if (memcmp(key, labels[i].k, 4) == 0) {
            return labels[i].name;
        }
    }
    return "adjustment";
}

bool pc_io_adjust_unreadable_reason(const uint8_t key[4], const uint8_t *data, size_t len, pc_buf *reason)
{
    adj_rd r = {data != NULL ? data : (const uint8_t *)"", data != NULL ? len : 0};
    pc_adjustment a;
    parsed p;
    size_t start = reason != NULL ? reason->len : 0;
    if (memcmp(key, "clrL", 4) != 0) {
        return false;
    }
    memset(&a, 0, sizeof a);
    p = read_lookup(&r, &a, reason);
    if (p == P_OK) {
        pc_adjustment_fini(&a);
        return false;
    }
    /* P_NOMEM (and any path that appended nothing) leaves no reason. */
    return reason != NULL && reason->len > start;
}

pc_status pc_io_adjust_parse(const uint8_t key[4], const uint8_t *data, size_t len, const uint8_t *cged,
                             size_t cged_len, pc_io_adj_channels channels, pc_adjustment *out, pc_error *err)
{
    adj_rd r = {data != NULL ? data : (const uint8_t *)"", data != NULL ? len : 0};
    pc_adjustment a;
    parsed p;
    /* A CMYK Channel Mixer (any record using the fourth, black, source) has no RGB meaning: keep
     * it raw. Mixers our writer made leave that source at 0 and map in every mode. */
    if (memcmp(key, "mixr", 4) == 0 && (channels == PC_IO_ADJ_CMYK || channels == PC_IO_ADJ_OTHER)) {
        int16_t k;
        for (size_t rec = 0; bei16(&r, sat_add(4 + 6, rec * 10), &k); rec++) {
            if (k != 0) {
                return unsupported(key, data, len, out, err);
            }
        }
    }
    memset(&a, 0, sizeof a);
    p = parse_any(key, &r, cged, cged_len, &a);
    if (p == P_NOMEM) {
        return pc_io_nomem(err, "reading an adjustment layer");
    }
    if (p == P_NONE) {
        pc_adjustment_fini(&a);
        return unsupported(key, data, len, out, err);
    }
    if (a.kind == PC_ADJ_LEVELS) {
        if (channels == PC_IO_ADJ_CMYK || channels == PC_IO_ADJ_LAB) {
            a.u.levels.space = channels == PC_IO_ADJ_CMYK ? PC_TONE_CMYK : PC_TONE_LAB;
            if (channels == PC_IO_ADJ_LAB) {
                a.u.levels.black = pc_levels_channel_default();
            }
        } else {
            a.u.levels.black = pc_levels_channel_default();
        }
        if (channels == PC_IO_ADJ_GRAY) {
            a.u.levels.per_channel[1] = a.u.levels.per_channel[0];
            a.u.levels.per_channel[2] = a.u.levels.per_channel[0];
        } else if (channels == PC_IO_ADJ_OTHER) {
            for (size_t i = 0; i < 3; i++) {
                a.u.levels.per_channel[i] = pc_levels_channel_default();
            }
        }
    } else if (a.kind == PC_ADJ_CURVES) {
        pc_curve *black = &a.u.curves.black;
        if (channels == PC_IO_ADJ_CMYK) {
            a.u.curves.space = PC_TONE_CMYK;
            if (pc_curve_is_identity(black->pts, black->len)) {
                pc_curve_fini(black);
            }
        } else {
            if (channels == PC_IO_ADJ_LAB) {
                a.u.curves.space = PC_TONE_LAB;
            }
            pc_curve_fini(black);
        }
        if (channels == PC_IO_ADJ_GRAY || channels == PC_IO_ADJ_OTHER) {
            for (size_t i = 1; i < 3; i++) {
                pc_curve_fini(&a.u.curves.per_channel[i]);
            }
            if (channels == PC_IO_ADJ_OTHER) {
                pc_curve_fini(&a.u.curves.per_channel[0]);
            }
            for (size_t i = 0; i < 3; i++) {
                bool ok = channels == PC_IO_ADJ_OTHER ? line(&a.u.curves.per_channel[i])
                          : i == 0 ? true
                                   : copy_curve(&a.u.curves.per_channel[0], &a.u.curves.per_channel[i]);
                if (!ok) {
                    pc_adjustment_fini(&a);
                    return pc_io_nomem(err, "reading an adjustment layer");
                }
            }
        }
    }
    *out = a;
    return PC_OK;
}
