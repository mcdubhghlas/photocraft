/* Adjustments -> PSD adjustment blocks (port of the writing half of crates/io/src/adjust_map.rs).
 * Every kind is written (vibA, blnc, blwh, phfl and mixr included); a Color Lookup without a table
 * is written as an identity cube and a Gradient Map with fewer than two stops as the equivalent
 * two-stop gradient, so nothing is dropped on save. Unsupported blocks go back verbatim. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "cms/lutfile.h"
#include "io/adjust_map.h"
#include "io/desc_util.h"

static pc_status put16(pc_buf *v, uint16_t x, pc_error *err)
{
    uint8_t b[2] = {(uint8_t)(x >> 8), (uint8_t)(x & 0xff)};
    return pc_buf_append(v, b, 2, err);
}

static pc_status put32(pc_buf *v, uint32_t x, pc_error *err)
{
    uint8_t b[4] = {(uint8_t)(x >> 24), (uint8_t)(x >> 16), (uint8_t)(x >> 8), (uint8_t)x};
    return pc_buf_append(v, b, 4, err);
}

static pc_status put_i16(pc_buf *v, int16_t x, pc_error *err)
{
    return put16(v, (uint16_t)x, err);
}

static pc_status put_byte(pc_buf *v, uint8_t x, pc_error *err)
{
    return pc_buf_append_byte(v, x, err);
}

static uint16_t q255(float v)
{
    return pc_f32_to_u16(roundf(pc_clampf(v, 0.0f, 1.0f) * 255.0f));
}

static int16_t clamp_i16(float v)
{
    return (int16_t)pc_f32_to_i32(pc_clampf(roundf(v), -32768.0f, 32767.0f));
}

static float rem_euclid(float a, float b)
{
    float r = fmodf(a, b);
    return r < 0.0f ? r + fabsf(b) : r;
}

static pc_status levels_write(pc_buf *v, const pc_levels_channel *c, pc_error *err)
{
    pc_status st = put16(v, q255(c->in_black), err);
    st = st != PC_OK ? st : put16(v, q255(c->in_white), err);
    st = st != PC_OK ? st : put16(v, q255(c->out_black), err);
    st = st != PC_OK ? st : put16(v, q255(c->out_white), err);
    return st != PC_OK ? st
                       : put16(v, pc_f32_to_u16(pc_clampf(roundf(c->gamma * 100.0f), 1.0f, 999.0f)), err);
}

/* grdm version 1 for a Gradient Map (Classic interpolation, Smoothness 0: the stops are
 * reproduced exactly). */
static pc_status write_gradient_map(const pc_gradient_map_stop *stops, size_t n, bool reverse, bool dither,
                                    pc_buf *v, pc_error *err)
{
    static const char name[] = "Custom";
    size_t count = n < 65535 ? n : 65535;
    pc_status st = put16(v, 1, err);
    st = st != PC_OK ? st : put_byte(v, reverse, err);
    st = st != PC_OK ? st : put_byte(v, dither, err);
    st = st != PC_OK ? st : put32(v, 7, err);
    for (size_t i = 0; i < 7 && st == PC_OK; i++) {
        st = put16(v, i < 6 ? (uint16_t)name[i] : 0, err);
    }
    st = st != PC_OK ? st : put16(v, (uint16_t)count, err);
    for (size_t i = 0; i < count && st == PC_OK; i++) {
        st = put32(v, pc_f32_to_u32(roundf(pc_clampf(stops[i].pos, 0.0f, 1.0f) * 4096.0f)), err);
        st = st != PC_OK ? st : put32(v, 50, err);
        st = st != PC_OK ? st : put16(v, 0, err);
        for (size_t k = 0; k < 3 && st == PC_OK; k++) {
            st = put16(v, pc_f32_to_u16(roundf(pc_clampf(stops[i].rgb[k], 0.0f, 1.0f) * 65535.0f)), err);
        }
        st = st != PC_OK ? st : put16(v, 0, err); /* fourth component */
        st = st != PC_OK ? st : put16(v, 0, err);
    }
    st = st != PC_OK ? st : put16(v, 2, err);
    for (uint32_t t = 0; t <= 4096 && st == PC_OK; t += 4096) {
        st = put32(v, t, err);
        st = st != PC_OK ? st : put32(v, 50, err);
        st = st != PC_OK ? st : put16(v, 255, err);
    }
    st = st != PC_OK ? st : put16(v, 2, err);    /* expansion count */
    st = st != PC_OK ? st : put16(v, 0, err);    /* smoothness */
    st = st != PC_OK ? st : put16(v, 32, err);   /* length */
    st = st != PC_OK ? st : put16(v, 0, err);    /* mode */
    st = st != PC_OK ? st : put32(v, 0, err);    /* random seed */
    st = st != PC_OK ? st : put16(v, 0, err);    /* showing transparency */
    st = st != PC_OK ? st : put16(v, 0, err);    /* using vector colour */
    st = st != PC_OK ? st : put32(v, 2048, err); /* roughness */
    st = st != PC_OK ? st : put16(v, 3, err);    /* colour model */
    {
        static const uint16_t mm[8] = {0, 0, 0, 0, 0x8000, 0x8000, 0x8000, 0x8000};
        for (size_t i = 0; i < 8 && st == PC_OK; i++) {
            st = put16(v, mm[i], err);
        }
    }
    return st != PC_OK ? st : put16(v, 0, err);
}

static pc_status add_value(pc_psd_desc *d, const char *key, pc_psd_value v, pc_error *err)
{
    return pc_psd_desc_add(d, key, &v, err);
}

static pc_status add_enum(pc_psd_desc *d, const char *key, const char *type, const char *value, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_enum(type, value, &v, err);
    return st != PC_OK ? st : pc_psd_desc_add(d, key, &v, err);
}

static pc_status add_text(pc_psd_desc *d, const char *key, const char *s, size_t len, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_io_text_value(s, len, &v, err);
    return st != PC_OK ? st : pc_psd_desc_add(d, key, &v, err);
}

static pc_status add_raw(pc_psd_desc *d, const char *key, const uint8_t *b, size_t len, pc_error *err)
{
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_RAW_DATA;
    if (len > 0) {
        pc_status st = pc_psd_bytes_set(&v.raw, b, len, err);
        if (st != PC_OK) {
            return st;
        }
    }
    return pc_psd_desc_add(d, key, &v, err);
}

/* Appends the versioned descriptor bytes of d (finalised) to v. */
static pc_status put_vdesc(pc_buf *v, pc_psd_desc *d, pc_error *err)
{
    pc_psd_bytes b = {NULL, 0};
    pc_status st = pc_psd_vdesc_to_bytes(d, &b, err);
    pc_psd_desc_fini(d);
    if (st == PC_OK) {
        st = pc_buf_append(v, b.data, b.len, err);
    }
    pc_psd_bytes_fini(&b);
    return st;
}

static pc_status write_brit(const pc_adjustment *adj, pc_buf *v, pc_buf *cged, pc_error *err)
{
    float b = adj->u.brightness_contrast.brightness;
    float c = adj->u.brightness_contrast.contrast;
    pc_psd_desc d;
    pc_status st = put_i16(v, clamp_i16(b), err);
    st = st != PC_OK ? st : put_i16(v, clamp_i16(c), err);
    st = st != PC_OK ? st : put16(v, 127, err);
    st = st != PC_OK ? st : put_byte(v, 0, err);
    st = st != PC_OK ? st : pc_psd_desc_init(&d, "null", err);
    if (st != PC_OK) {
        return st;
    }
    st = add_value(&d, "Vrsn", pc_psd_value_int(1), err);
    st = st != PC_OK ? st : add_value(&d, "Brgh", pc_psd_value_int(pc_f32_to_i32(roundf(b))), err);
    st = st != PC_OK ? st : add_value(&d, "Cntr", pc_psd_value_int(pc_f32_to_i32(roundf(c))), err);
    st = st != PC_OK ? st : add_value(&d, "means", pc_psd_value_int(127), err);
    st = st != PC_OK ? st : add_value(&d, "Lab ", pc_psd_value_bool(false), err);
    st = st != PC_OK ? st
                     : add_value(&d, "useLegacy", pc_psd_value_bool(adj->u.brightness_contrast.legacy), err);
    st = st != PC_OK ? st : add_value(&d, "Auto", pc_psd_value_bool(false), err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    return put_vdesc(cged, &d, err);
}

static pc_status write_hue(const pc_adjustment *adj, pc_buf *v, pc_error *err)
{
    const float hsl[3] = {adj->u.hue_saturation.hue, adj->u.hue_saturation.saturation,
                          adj->u.hue_saturation.lightness};
    pc_status st = put16(v, 2, err);
    st = st != PC_OK ? st : put_byte(v, adj->u.hue_saturation.colorize, err);
    st = st != PC_OK ? st : put_byte(v, 0, err);
    for (size_t t = 0; t < 2; t++) {
        for (size_t i = 0; i < 3 && st == PC_OK; i++) {
            st = put_i16(v, clamp_i16(hsl[i]), err);
        }
    }
    /* Six hue ranges (reds ... magentas): range values in 0..360, then hue, saturation, lightness. */
    for (size_t r = 0; r < 6 && st == PC_OK; r++) {
        const pc_hue_range *h = &adj->u.hue_saturation.ranges[r];
        for (size_t k = 0; k < 4 && st == PC_OK; k++) {
            st = put16(v, (uint16_t)(pc_f32_to_u16(roundf(rem_euclid(h->bounds[k], 360.0f))) % 360), err);
        }
        st = st != PC_OK ? st : put_i16(v, clamp_i16(h->hue), err);
        st = st != PC_OK ? st : put_i16(v, clamp_i16(h->saturation), err);
        st = st != PC_OK ? st : put_i16(v, clamp_i16(h->lightness), err);
    }
    return st;
}

static pc_status put_f32(pc_buf *v, float x, pc_error *err)
{
    uint32_t u;
    memcpy(&u, &x, 4);
    return put32(v, u, err);
}

static pc_status write_curve(pc_buf *v, const pc_curve *c, bool pencil, pc_error *err)
{
    size_t n;
    pc_status st;
    if (pencil) {
        /* A map (pencil) stores 256 output bytes, one per 8-bit input level. */
        float outs[PC_CURVE_MAP_LEN];
        st = pc_curve_map_outputs(c->pts, c->len, outs, err);
        for (size_t k = 0; k < PC_CURVE_MAP_LEN && st == PC_OK; k++) {
            st = put_byte(v, (uint8_t)q255(outs[k]), err);
        }
        return st;
    }
    n = c->len < 19 ? c->len : 19;
    st = put16(v, (uint16_t)n, err);
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        st = put16(v, q255(c->pts[i].output), err);
        st = st != PC_OK ? st : put16(v, q255(c->pts[i].input), err);
    }
    return st;
}

static pc_status write_lookup(const pc_adjustment *adj, pc_buf *v, pc_error *err)
{
    /* A lookup without a usable table (none chosen yet, or a bad size) renders as the identity,
     * so it is written as a 2^3 identity cube rather than dropped. */
    uint32_t size = adj->u.color_lookup.size;
    size_t need = 0;
    size_t nf = 0;
    const float *table = pc_adjustment_lut(adj, &nf);
    char empty[1] = {0};
    pc_lutfile file;
    pc_lutfile ident;
    bool have_ident = false;
    bool dither;
    pc_buf cube = PC_BUF_INIT;
    pc_psd_desc d;
    const pc_str *name = &adj->u.color_lookup.name;
    pc_status st;
    memset(&file, 0, sizeof file);
    memset(&ident, 0, sizeof ident);
    if (table != NULL && pc_lut3d_len(size, &need) && nf >= need && size <= 256) {
        file.title = empty;
        file.size = size;
        file.data = (float *)(uintptr_t)table;
        file.ndata = need;
        file.domain_max[0] = file.domain_max[1] = file.domain_max[2] = 1.0f;
        dither = adj->u.color_lookup.dither;
    } else {
        st = pc_lutfile_identity(2, &ident, err);
        if (st != PC_OK) {
            return st;
        }
        have_ident = true;
        file = ident;
        file.title = empty;
        dither = false;
    }
    st = pc_lutfile_write_cube(&file, &cube, err);
    if (have_ident) {
        pc_lutfile_fini(&ident);
    }
    st = st != PC_OK ? st : pc_psd_desc_init(&d, "null", err);
    if (st != PC_OK) {
        pc_buf_fini(&cube);
        return st;
    }
    st = add_enum(&d, "lookupType", "colorLookupType", "3DLUT", err);
    st = st != PC_OK ? st : add_text(&d, "NM  ", name->ptr, name->len, err);
    st = st != PC_OK ? st : add_value(&d, "Dthr", pc_psd_value_bool(dither), err);
    st = st != PC_OK ? st : add_raw(&d, "profile", NULL, 0, err);
    st = st != PC_OK ? st : add_enum(&d, "LUTFormat", "LUTFormatType", "LUTFormatCUBE", err);
    st = st != PC_OK ? st : add_enum(&d, "dataOrder", "colorLookupOrder", "rgbOrder", err);
    st = st != PC_OK ? st : add_enum(&d, "tableOrder", "colorLookupOrder", "bgrOrder", err);
    st = st != PC_OK ? st : add_raw(&d, "LUT3DFileData", cube.data, cube.len, err);
    st = st != PC_OK ? st : add_text(&d, "LUT3DFileName", name->ptr, name->len, err);
    pc_buf_fini(&cube);
    st = st != PC_OK ? st : put16(v, 1, err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    return put_vdesc(v, &d, err);
}

static pc_status write_bw(const pc_adjustment *adj, pc_buf *v, pc_error *err)
{
    static const char keys[6][5] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
    const float tint_default[3] = {225.0f / 255.0f, 211.0f / 255.0f, 179.0f / 255.0f};
    const float *tint = adj->u.black_white.has_tint ? adj->u.black_white.tint : tint_default;
    pc_psd_desc d;
    pc_psd_desc t;
    pc_psd_value tv;
    pc_status st = pc_psd_desc_init(&d, "null", err);
    if (st != PC_OK) {
        return st;
    }
    for (size_t i = 0; i < 6 && st == PC_OK; i++) {
        st = add_value(&d, keys[i],
                       pc_psd_value_int(clamp_i16(pc_clampf(adj->u.black_white.weights[i], -200.0f, 300.0f))),
                       err);
    }
    st = st != PC_OK ? st : add_value(&d, "useTint", pc_psd_value_bool(adj->u.black_white.has_tint), err);
    st = st != PC_OK ? st : pc_psd_desc_init(&t, "RGBC", err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    {
        static const char ck[3][5] = {"Rd  ", "Grn ", "Bl  "};
        for (size_t i = 0; i < 3 && st == PC_OK; i++) {
            st = add_value(&t, ck[i], pc_psd_value_double((double)pc_clampf(tint[i], 0.0f, 1.0f) * 255.0),
                           err);
        }
    }
    st = st != PC_OK ? st : pc_psd_value_object(&t, &tv, err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&t);
        pc_psd_desc_fini(&d);
        return st;
    }
    st = pc_psd_desc_add(&d, "tintColor", &tv, err);
    st = st != PC_OK ? st : add_value(&d, "bwPresetKind", pc_psd_value_int(1), err);
    st = st != PC_OK ? st : add_text(&d, "blackAndWhitePresetFileName", "", 0, err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    return put_vdesc(v, &d, err);
}

/* The main block data of adj; *key set, *cged filled for Brightness/Contrast. *ok false when
 * there is no encoding (an Unsupported block whose key is not four bytes). */
static pc_status write_main(const pc_adjustment *adj, char key[5], pc_buf *v, pc_buf *cged, bool *ok,
                            pc_error *err)
{
    pc_status st = PC_OK;
    *ok = true;
    switch (adj->kind) {
    case PC_ADJ_INVERT:
        memcpy(key, "nvrt", 5);
        return PC_OK;
    case PC_ADJ_THRESHOLD: {
        uint16_t q = q255(adj->u.threshold.level);
        memcpy(key, "thrs", 5);
        st = put16(v, q > 1 ? q : 1, err);
        return st != PC_OK ? st : put16(v, 0, err);
    }
    case PC_ADJ_POSTERIZE: {
        uint32_t l = adj->u.posterize.levels;
        memcpy(key, "post", 5);
        st = put16(v, (uint16_t)(l < 2 ? 2 : l > 255 ? 255 : l), err);
        return st != PC_OK ? st : put16(v, 0, err);
    }
    case PC_ADJ_BRIGHTNESS_CONTRAST:
        memcpy(key, "brit", 5);
        return write_brit(adj, v, cged, err);
    case PC_ADJ_HUE_SATURATION:
        memcpy(key, "hue2", 5);
        return write_hue(adj, v, err);
    case PC_ADJ_EXPOSURE:
        memcpy(key, "expA", 5);
        st = put16(v, 1, err);
        st = st != PC_OK ? st : put_f32(v, adj->u.exposure.exposure, err);
        st = st != PC_OK ? st : put_f32(v, adj->u.exposure.offset, err);
        st = st != PC_OK ? st : put_f32(v, adj->u.exposure.gamma, err);
        return st != PC_OK ? st : put_byte(v, 1, err); /* color space flag (spec: "1 byte") */
    case PC_ADJ_LEVELS: {
        pc_levels_channel ident = pc_levels_channel_default();
        memcpy(key, "levl", 5);
        st = put16(v, 2, err);
        st = st != PC_OK ? st : levels_write(v, &adj->u.levels.master, err);
        for (size_t i = 0; i < 3 && st == PC_OK; i++) {
            st = levels_write(v, &adj->u.levels.per_channel[i], err);
        }
        st = st != PC_OK
                 ? st
                 : levels_write(v, adj->u.levels.space == PC_TONE_CMYK ? &adj->u.levels.black : &ident, err);
        for (size_t i = 5; i < 29 && st == PC_OK; i++) {
            st = levels_write(v, &ident, err);
        }
        return st;
    }
    case PC_ADJ_CURVES: {
        bool ink = adj->u.curves.space == PC_TONE_CMYK && adj->u.curves.black.len >= 2;
        bool pencil = adj->u.curves.pencil;
        memcpy(key, "curv", 5);
        st = put_byte(v, pencil ? 1 : 0, err);
        st = st != PC_OK ? st : put16(v, 1, err);
        st = st != PC_OK ? st : put32(v, ink ? 0x1f : 0xf, err);
        st = st != PC_OK ? st : write_curve(v, &adj->u.curves.master, pencil, err);
        for (size_t i = 0; i < 3 && st == PC_OK; i++) {
            st = write_curve(v, &adj->u.curves.per_channel[i], pencil, err);
        }
        if (st == PC_OK && ink) {
            st = write_curve(v, &adj->u.curves.black, pencil, err);
        }
        return st;
    }
    case PC_ADJ_SELECTIVE_COLOR:
        memcpy(key, "selc", 5);
        st = put16(v, 1, err);
        st = st != PC_OK ? st : put16(v, adj->u.selective_color.relative ? 0 : 1, err);
        st = st != PC_OK ? st : pc_buf_append(v, (const uint8_t[8]){0}, 8, err);
        for (size_t r = 0; r < 9 && st == PC_OK; r++) {
            for (size_t k = 0; k < 4 && st == PC_OK; k++) {
                st = put_i16(
                    v, clamp_i16(pc_clampf(adj->u.selective_color.adjustments[r][k], -100.0f, 100.0f)), err);
            }
        }
        return st;
    case PC_ADJ_COLOR_LOOKUP:
        memcpy(key, "clrL", 5);
        return write_lookup(adj, v, err);
    case PC_ADJ_GRADIENT_MAP: {
        /* Photoshop needs two stops; fewer are written as the gradient they render as (none:
         * black to white, i.e. the gray value itself; one: that colour throughout). */
        pc_gradient_map_stop two[2];
        const pc_gradient_map_stop *s = adj->u.gradient_map.stops;
        size_t n = adj->u.gradient_map.nstops;
        memcpy(key, "grdm", 5);
        if (n == 0) {
            two[0] = (pc_gradient_map_stop){0.0f, {0.0f, 0.0f, 0.0f}};
            two[1] = (pc_gradient_map_stop){1.0f, {1.0f, 1.0f, 1.0f}};
            s = two;
            n = 2;
        } else if (n == 1) {
            two[0] = (pc_gradient_map_stop){0.0f, {s[0].rgb[0], s[0].rgb[1], s[0].rgb[2]}};
            two[1] = (pc_gradient_map_stop){1.0f, {s[0].rgb[0], s[0].rgb[1], s[0].rgb[2]}};
            s = two;
            n = 2;
        }
        return write_gradient_map(s, n, adj->u.gradient_map.reverse, adj->u.gradient_map.dither, v, err);
    }
    case PC_ADJ_VIBRANCE: {
        pc_psd_desc d;
        memcpy(key, "vibA", 5);
        st = pc_psd_desc_init(&d, "null", err);
        if (st != PC_OK) {
            return st;
        }
        st =
            add_value(&d, "vibrance",
                      pc_psd_value_int(clamp_i16(pc_clampf(adj->u.vibrance.vibrance, -100.0f, 100.0f))), err);
        st = st != PC_OK ? st
                         : add_value(&d, "Strt",
                                     pc_psd_value_int(
                                         clamp_i16(pc_clampf(adj->u.vibrance.saturation, -100.0f, 100.0f))),
                                     err);
        if (st != PC_OK) {
            pc_psd_desc_fini(&d);
            return st;
        }
        return put_vdesc(v, &d, err);
    }
    case PC_ADJ_COLOR_BALANCE: {
        const float *parts[3] = {adj->u.color_balance.shadows, adj->u.color_balance.midtones,
                                 adj->u.color_balance.highlights};
        memcpy(key, "blnc", 5);
        for (size_t p = 0; p < 3; p++) {
            for (size_t i = 0; i < 3 && st == PC_OK; i++) {
                st = put_i16(v, clamp_i16(pc_clampf(parts[p][i], -100.0f, 100.0f)), err);
            }
        }
        st = st != PC_OK ? st : put_byte(v, adj->u.color_balance.preserve_luminosity, err);
        return st != PC_OK ? st : put_byte(v, 0, err);
    }
    case PC_ADJ_BLACK_WHITE:
        memcpy(key, "blwh", 5);
        return write_bw(adj, v, err);
    case PC_ADJ_PHOTO_FILTER:
        /* Version 2: an RGB colour structure keeps the colour at 16 bits. */
        memcpy(key, "phfl", 5);
        st = put16(v, 2, err);
        st = st != PC_OK ? st : put16(v, 0, err);
        for (size_t i = 0; i < 3 && st == PC_OK; i++) {
            st = put16(v,
                       pc_f32_to_u16(roundf(pc_clampf(adj->u.photo_filter.color[i], 0.0f, 1.0f) * 65535.0f)),
                       err);
        }
        st = st != PC_OK ? st : put16(v, 0, err);
        st =
            st != PC_OK
                ? st
                : put32(v, pc_f32_to_u32(roundf(pc_clampf(adj->u.photo_filter.density, 0.0f, 1.0f) * 100.0f)),
                        err);
        st = st != PC_OK ? st : put_byte(v, adj->u.photo_filter.preserve_luminosity, err);
        /* Photoshop requires the 17-byte payload after the version padded to a 4-byte boundary
         * INSIDE the block's length: an 18-byte phfl block does not open there (#1455). */
        return st != PC_OK ? st : pc_buf_append(v, (const uint8_t[3]){0, 0, 0}, 3, err);
    case PC_ADJ_CHANNEL_MIXER: {
        memcpy(key, "mixr", 5);
        st = put16(v, 1, err);
        st = st != PC_OK ? st : put16(v, adj->u.channel_mixer.monochrome ? 1 : 0, err);
        /* Red, green, blue, then the gray record (the first row, which monochrome uses). */
        for (size_t r = 0; r < 4 && st == PC_OK; r++) {
            const float *row = adj->u.channel_mixer.matrix[r < 3 ? r : 0];
            for (size_t k = 0; k < 3 && st == PC_OK; k++) {
                st = put_i16(v, clamp_i16(pc_clampf(row[k] * 100.0f, -200.0f, 200.0f)), err);
            }
            st = st != PC_OK ? st : put16(v, 0, err);
            st = st != PC_OK ? st : put_i16(v, clamp_i16(pc_clampf(row[3] * 100.0f, -200.0f, 200.0f)), err);
        }
        return st;
    }
    case PC_ADJ_UNSUPPORTED:
    default:
        if (adj->u.unsupported.psd_key.len != 4) {
            *ok = false;
            return PC_OK;
        }
        memcpy(key, adj->u.unsupported.psd_key.ptr, 4);
        key[4] = '\0';
        return pc_buf_append(v, pc_blob_data(adj->u.unsupported.raw), pc_blob_len(adj->u.unsupported.raw),
                             err);
    }
}

static pc_status push_buf(pc_io_raw_blocks *out, const char key[4], pc_buf *b, pc_error *err)
{
    size_t len = b->len;
    uint8_t *data = pc_buf_take(b, NULL);
    if (len == 0) {
        free(data);
        data = NULL;
    }
    return pc_io_raw_blocks_push_take(out, (const uint8_t *)key, &data, len, err);
}

pc_status pc_io_adjust_write(const pc_adjustment *adj, pc_io_raw_blocks *out, pc_error *err)
{
    char key[5] = {0};
    pc_buf v = PC_BUF_INIT;
    pc_buf cged = PC_BUF_INIT;
    bool ok = true;
    size_t n0 = out->n;
    pc_status st = write_main(adj, key, &v, &cged, &ok, err);
    if (st == PC_OK && ok) {
        st = push_buf(out, key, &v, err);
        if (st == PC_OK && adj->kind == PC_ADJ_BRIGHTNESS_CONTRAST) {
            st = push_buf(out, "CgEd", &cged, err);
        }
    }
    pc_buf_fini(&v);
    pc_buf_fini(&cged);
    if (st != PC_OK) {
        while (out->n > n0) {
            out->n--;
            free(out->v[out->n].data);
        }
    }
    return st;
}
