/* Digital Negative (Adobe DNG Specification 1.7), a port of crates/raw/src/dng.rs: raw IFD
 * selection, CFA description, linearization, black/white levels, active area, default crop,
 * opcode lists, colour calibration and white balance. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/mem.h"
#include "base/num.h"
#include "raw/internal.h"

#define PHOTOMETRIC_CFA 32803u
#define PHOTOMETRIC_LINEAR_RAW 34892u

/* The main raw image: NewSubFileType 0 with CFA or LinearRaw data, largest (last of equals). */
static bool raw_ifd(const rx_tiff *t, const rx_ifd *ifds, size_t n, rx_ifd *out)
{
    size_t i;
    bool found = false;
    uint64_t best = 0;
    for (i = 0; i < n; i++) {
        uint32_t v;
        uint32_t w = 0;
        uint32_t h = 0;
        uint64_t a;
        if (rx_tag_uint(t, &ifds[i], RX_NEW_SUBFILE_TYPE, &v) && v != 0) {
            continue;
        }
        if (!rx_tag_uint(t, &ifds[i], RX_PHOTOMETRIC, &v) || (v != PHOTOMETRIC_CFA && v != PHOTOMETRIC_LINEAR_RAW)) {
            continue;
        }
        (void)rx_tag_uint(t, &ifds[i], RX_IMAGE_WIDTH, &w);
        (void)rx_tag_uint(t, &ifds[i], RX_IMAGE_LENGTH, &h);
        a = (uint64_t)w * h;
        if (!found || a >= best) {
            best = a;
            *out = ifds[i];
            found = true;
        }
    }
    return found;
}

static bool matrix(const rx_tiff *t, const rx_ifd *ifd, uint16_t tg, pc_raw_mat3 *out)
{
    double v[9];
    return rx_tag_floats(t, ifd, tg, v, 9) == 9 && rx_from_slice(v, 9, out);
}

static bool triple(const rx_tiff *t, const rx_ifd *ifd, uint16_t tg, double out[3])
{
    double v[3];
    int i;
    if (rx_tag_floats(t, ifd, tg, v, 3) != 3) {
        return false;
    }
    for (i = 0; i < 3; i++) {
        if (!(isfinite(v[i]) && v[i] > 0.0)) {
            return false;
        }
    }
    memcpy(out, v, sizeof v);
    return true;
}

/* f64::total_cmp(a, b) > 0. */
static bool total_gt(double a, double b)
{
    int64_t x;
    int64_t y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    x ^= (int64_t)((uint64_t)(x >> 63) >> 1);
    y ^= (int64_t)((uint64_t)(y >> 63) >> 1);
    return x > y;
}

static void color_info(const rx_tiff *t, const rx_ifd *ifd0, pc_raw_color_info *out)
{
    static const uint16_t CM[2] = {RX_COLOR_MATRIX_1, RX_COLOR_MATRIX_2};
    static const uint16_t CC[2] = {RX_CAMERA_CALIBRATION_1, RX_CAMERA_CALIBRATION_2};
    static const uint16_t FM[2] = {RX_FORWARD_MATRIX_1, RX_FORWARD_MATRIX_2};
    static const uint16_t ILL[2] = {RX_CALIBRATION_ILLUMINANT_1, RX_CALIBRATION_ILLUMINANT_2};
    static const double DEFAULT_T[2] = {2856.0, 6504.0};
    pc_raw_color_info c;
    int k;
    double v[2];
    memset(&c, 0, sizeof c);
    for (k = 0; k < 2; k++) {
        pc_raw_calibration cal;
        pc_raw_mat3 inv;
        uint32_t ill;
        memset(&cal, 0, sizeof cal);
        if (!matrix(t, ifd0, CM[k], &cal.color_matrix) || !rx_invert(&cal.color_matrix, &inv)) {
            continue;
        }
        if (!(rx_tag_uint(t, ifd0, ILL[k], &ill) && rx_illuminant_temperature(ill, &cal.temperature))) {
            cal.temperature = DEFAULT_T[k];
        }
        cal.has_forward_matrix = matrix(t, ifd0, FM[k], &cal.forward_matrix) && rx_invert(&cal.forward_matrix, &inv);
        if (!(matrix(t, ifd0, CC[k], &cal.camera_calibration) && rx_invert(&cal.camera_calibration, &inv))) {
            cal.camera_calibration = RX_IDENTITY;
        }
        c.calibrations[c.ncalibrations++] = cal;
    }
    if (c.ncalibrations == 2 && total_gt(c.calibrations[0].temperature, c.calibrations[1].temperature)) {
        pc_raw_calibration tmp = c.calibrations[0];
        c.calibrations[0] = c.calibrations[1];
        c.calibrations[1] = tmp;
    }
    if (c.ncalibrations == 2 && c.calibrations[0].has_forward_matrix != c.calibrations[1].has_forward_matrix) {
        c.calibrations[0].has_forward_matrix = false;
        c.calibrations[1].has_forward_matrix = false;
    }
    if (rx_tag_floats(t, ifd0, RX_AS_SHOT_WHITE_XY, v, 2) == 2 && v[0] > 0.0 && v[0] < 1.0 && v[1] > 0.0 &&
        v[1] < 1.0) {
        c.has_as_shot_white_xy = true;
        c.as_shot_white_xy[0] = v[0];
        c.as_shot_white_xy[1] = v[1];
    }
    c.has_analog_balance = triple(t, ifd0, RX_ANALOG_BALANCE, c.analog_balance);
    c.has_as_shot_neutral = triple(t, ifd0, RX_AS_SHOT_NEUTRAL, c.as_shot_neutral);
    *out = c;
}

/* CFA pattern of a DNG raw IFD, anchored at the active area origin. */
static pc_status cfa_of(const rx_tiff *t, const rx_ifd *ifd, pc_raw_rect active, pc_raw_cfa *out, pc_error *err)
{
    uint32_t dim[2];
    size_t rows = 2;
    size_t cols = 2;
    rx_entry pe;
    size_t npat;
    rx_entry ce;
    size_t nplanes;
    uint32_t layout;
    pc_raw_cfa c;
    size_t i;
    if (rx_tag_uints(t, ifd, RX_CFA_REPEAT_PATTERN_DIM, dim, 2) == 2) {
        rows = dim[0];
        cols = dim[1];
    }
    npat = rx_ifd_get(t, ifd, RX_CFA_PATTERN, &pe) ? rx_uints_n(t, &pe) : 0;
    if (rows == 0 || cols == 0 || rows > 16 || cols > 16 || npat != rows * cols) {
        return rx_malformed(err, "bad CFA pattern");
    }
    nplanes = rx_ifd_get(t, ifd, RX_CFA_PLANE_COLOR, &ce) ? rx_uints_n(t, &ce) : 0;
    if (nplanes != 0) {
        uint32_t p[3] = {9, 9, 9};
        size_t k;
        for (k = 0; k < 3 && k < nplanes; k++) {
            (void)rx_uint(t, &ce, k, &p[k]);
        }
        if (nplanes != 3 || p[0] != 0 || p[1] != 1 || p[2] != 2) {
            char list[160];
            size_t o = 0;
            o += (size_t)snprintf(list + o, sizeof list - o, "[");
            for (k = 0; k < nplanes && o + 16 < sizeof list; k++) {
                uint32_t q = 0;
                (void)rx_uint(t, &ce, k, &q);
                o += (size_t)snprintf(list + o, sizeof list - o, "%s%u", k > 0 ? ", " : "", (unsigned)q);
            }
            (void)snprintf(list + o, sizeof list - o, "%s]", k < nplanes ? ", ..." : "");
            return rx_unsupported(err, "CFA plane colours %s (only red, green, blue)", list);
        }
    }
    if (rx_tag_uint(t, ifd, RX_CFA_LAYOUT, &layout) && layout != 1) {
        return rx_unsupported(err, "non-rectangular CFA layout");
    }
    memset(&c, 0, sizeof c);
    for (i = 0; i < npat; i++) {
        uint32_t v = 0;
        (void)rx_uint(t, &pe, i, &v);
        if (v > 2) {
            return rx_unsupported(err, "CFA colours other than red, green and blue");
        }
        c.colors[i] = (uint8_t)v;
    }
    c.width = cols;
    c.height = rows;
    c.origin_x = active.x;
    c.origin_y = active.y;
    if (!pc_raw_cfa_is_bayer(&c)) {
        return rx_unsupported(err, "%zux%zu non-Bayer CFA (e.g. X-Trans)", rows, cols);
    }
    *out = c;
    return PC_OK;
}

static pc_raw_rect rect_from(const rx_tiff *t, const rx_ifd *ifd, size_t width, size_t height)
{
    pc_raw_rect full = pc_raw_rect_make(0, 0, width, height);
    uint32_t a[4];
    if (rx_tag_uints(t, ifd, RX_ACTIVE_AREA, a, 4) == 4 && a[2] > a[0] && a[3] > a[1]) {
        return pc_raw_rect_intersect(pc_raw_rect_make(a[1], a[0], a[3] - a[1], a[2] - a[0]), full);
    }
    return full;
}

static pc_status crop_value(double value, const char *name, size_t *out, pc_error *err)
{
    double rounded;
    if (!isfinite(value) || value < 0.0) {
        return rx_malformed(err, "invalid DNG %s", name);
    }
    rounded = round(value);
    if (rounded >= 18446744073709551616.0) {
        return rx_malformed(err, "DNG %s is not representable", name);
    }
    *out = pc_f64_to_size(rounded);
    return PC_OK;
}

/* A present tag must hold two values; an absent one takes its default. */
static bool pair(const rx_tiff *t, const rx_ifd *raw, uint16_t tg, double d0, double d1, double out[2])
{
    if (!rx_ifd_has(t, raw, tg)) {
        out[0] = d0;
        out[1] = d1;
        return true;
    }
    return rx_tag_floats(t, raw, tg, out, 2) == 2;
}

/* A float list tag, malloc'd as f32 when it has exactly want values. */
static pc_status float_list(const rx_tiff *t, const rx_ifd *ifd, uint16_t tg, size_t want, float **out, size_t *n,
                            pc_error *err)
{
    rx_entry e;
    size_t k;
    size_t i;
    if (!rx_ifd_get(t, ifd, tg, &e) || (k = rx_floats_n(t, &e)) != want || k == 0) {
        return PC_OK;
    }
    *out = pc_malloc_array(k, sizeof **out);
    if (*out == NULL) {
        return rx_nomem(err);
    }
    for (i = 0; i < k; i++) {
        double v = 0.0;
        (void)rx_float(t, &e, i, &v);
        (*out)[i] = pc_f64_to_f32(v);
    }
    *n = k;
    return PC_OK;
}

pc_status rx_dng_decode(const rx_tiff *t, const pc_raw_limits *limits, pc_raw_sensor *out, pc_error *err)
{
    rx_ifd ifds[RX_MAX_IFDS];
    size_t nifd = rx_all_ifds(t, ifds);
    rx_ifd ifd0;
    rx_ifd raw;
    uint32_t photometric;
    rx_plane plane;
    size_t samples;
    bool is_cfa;
    pc_raw_rect active;
    pc_raw_sensor s;
    pc_status st;
    double sc[2];
    float max;
    float white_default;
    double wl[3];
    size_t nwl;
    uint32_t rc[2];
    size_t rows = 1;
    size_t cols = 1;
    rx_entry e;
    size_t nbl;
    double be;
    double beo;
    bool nomem = false;
    int k;
    if (!rx_ifd_at(t, t->first_ifd, 0, &ifd0)) {
        return rx_malformed(err, "DNG has no IFD0");
    }
    if (!raw_ifd(t, ifds, nifd, &raw)) {
        return rx_malformed(err, "DNG has no raw image");
    }
    photometric = 0;
    (void)rx_tag_uint(t, &raw, RX_PHOTOMETRIC, &photometric);
    if ((st = rx_read_plane(t, &raw, limits, RX_FLAT, &plane, err)) != PC_OK) {
        return st;
    }
    pc_raw_sensor_init(&s);
    s.data = plane.data;
    s.ndata = plane.width * plane.height * plane.samples;
    samples = plane.samples;
    is_cfa = photometric == PHOTOMETRIC_CFA;
    if (is_cfa && samples != 1) {
        st = rx_unsupported(err, "CFA data with %zu samples per pixel", samples);
        goto fail;
    }
    /* LinearRaw: three colour samples, or one for a monochrome camera or scanner (DNG 1.7,
     * ColorPlanes 1). */
    if (!is_cfa && samples != 1 && samples != 3) {
        st = rx_unsupported(err, "LinearRaw data with %zu samples per pixel", samples);
        goto fail;
    }
    active = rect_from(t, &raw, plane.width, plane.height);
    if (pc_raw_rect_is_empty(active)) {
        st = rx_malformed(err, "empty active area");
        goto fail;
    }
    if (is_cfa) {
        if ((st = cfa_of(t, &raw, active, &s.cfa, err)) != PC_OK) {
            goto fail;
        }
        s.has_cfa = true;
    }
    /* Default crop, relative to the active area; each tag has its own default. */
    s.crop = active;
    if (rx_ifd_has(t, &raw, RX_DEFAULT_CROP_ORIGIN) || rx_ifd_has(t, &raw, RX_DEFAULT_CROP_SIZE)) {
        double o[2];
        double z[2];
        if (pair(t, &raw, RX_DEFAULT_CROP_ORIGIN, 0.0, 0.0, o) &&
            pair(t, &raw, RX_DEFAULT_CROP_SIZE, (double)plane.width, (double)plane.height, z)) {
            size_t ox;
            size_t oy;
            size_t cw;
            size_t ch;
            size_t ar;
            size_t ab;
            bool ok = false;
            if ((st = crop_value(o[0], "DefaultCropOrigin", &ox, err)) != PC_OK ||
                (st = crop_value(o[1], "DefaultCropOrigin", &oy, err)) != PC_OK ||
                (st = crop_value(z[0], "DefaultCropSize", &cw, err)) != PC_OK ||
                (st = crop_value(z[1], "DefaultCropSize", &ch, err)) != PC_OK) {
                goto fail;
            }
            if (active.x > SIZE_MAX - active.width) {
                st = rx_malformed(err, "DNG active-area right edge overflow");
                goto fail;
            }
            if (active.y > SIZE_MAX - active.height) {
                st = rx_malformed(err, "DNG active-area bottom edge overflow");
                goto fail;
            }
            ar = active.x + active.width;
            ab = active.y + active.height;
            if (active.x <= SIZE_MAX - ox && active.y <= SIZE_MAX - oy) {
                size_t x = active.x + ox;
                size_t y = active.y + oy;
                if (x <= SIZE_MAX - cw && y <= SIZE_MAX - ch && x >= active.x && y >= active.y && x + cw <= ar &&
                    y + ch <= ab) {
                    pc_raw_rect r = pc_raw_rect_make(x, y, cw, ch);
                    if (!pc_raw_rect_is_empty(r)) {
                        s.crop = r;
                        ok = true;
                    }
                }
            }
            if (!ok && (st = rx_warn(&s.warnings, err, "out-of-range or empty DNG default crop; active area used")) !=
                           PC_OK) {
                goto fail;
            }
        } else if ((st = rx_warn(&s.warnings, err, "incomplete DNG default crop; active area used")) != PC_OK) {
            goto fail;
        }
    }
    if (rx_tag_floats(t, &raw, RX_DEFAULT_SCALE, sc, 2) == 2 && fabs(sc[0] - sc[1]) > 1e-6 &&
        (st = rx_warn(&s.warnings, err, "non-square pixels (DefaultScale) are not resampled")) != PC_OK) {
        goto fail;
    }
    /* Opcode lists: OpcodeList2 gain maps are applied; everything else is reported. */
    for (k = 1; k <= 3; k++) {
        static const uint16_t TAGS[4] = {0, RX_OPCODE_LIST_1, RX_OPCODE_LIST_2, RX_OPCODE_LIST_3};
        const uint8_t *bytes;
        size_t blen;
        pc_raw_gain_map *maps;
        size_t nmaps;
        pc_raw_strings skipped;
        if (!rx_ifd_get(t, &raw, TAGS[k], &e) || (bytes = rx_raw(t, &e, &blen)) == NULL) {
            continue;
        }
        if ((st = rx_opcodes_parse(bytes, blen, &maps, &nmaps, &skipped, err)) != PC_OK) {
            goto fail;
        }
        if (k == 2) {
            size_t q;
            for (q = 0; q < s.ngain_maps; q++) {
                rx_gain_map_fini(&s.gain_maps[q]);
            }
            free(s.gain_maps);
            s.gain_maps = maps;
            s.ngain_maps = nmaps;
        } else {
            size_t q;
            for (q = 0; q < nmaps; q++) {
                rx_gain_map_fini(&maps[q]);
            }
            free(maps);
            if (nmaps > 0 && (st = rx_warn(&s.warnings, err, "DNG OpcodeList%d: GainMap is not applied", k)) != PC_OK) {
                pc_raw_strings_fini(&skipped);
                goto fail;
            }
        }
        if (skipped.n > 0) {
            pc_buf list = PC_BUF_INIT;
            size_t q;
            for (q = 0; q < skipped.n && st == PC_OK; q++) {
                st = pc_buf_appendf(&list, err, "%s%s", q > 0 ? ", " : "", skipped.v[q]);
            }
            if (st == PC_OK) {
                st = rx_warn(&s.warnings, err, "DNG OpcodeList%d: %s not applied", k, pc_buf_cstr(&list));
            }
            pc_buf_fini(&list);
        }
        pc_raw_strings_fini(&skipped);
        if (st != PC_OK) {
            goto fail;
        }
    }
    /* DNG 1.6 ProfileGainTableMap: the local tone mapping phones rely on for their default look. */
    if ((rx_ifd_has(t, &ifd0, RX_PROFILE_GAIN_TABLE_MAP) || rx_ifd_has(t, &raw, RX_PROFILE_GAIN_TABLE_MAP)) &&
        (st = rx_warn(&s.warnings, err, "DNG ProfileGainTableMap (local tone mapping) is not applied")) !=
            PC_OK) {
        goto fail;
    }
    max = (float)((1u << plane.bits) - 1);
    if (rx_ifd_get(t, &raw, RX_LINEARIZATION_TABLE, &e)) {
        size_t n = rx_uints_n(t, &e);
        if (n > 0 && n <= 65536) {
            size_t i;
            s.linearization = pc_malloc_array(n, sizeof *s.linearization);
            if (s.linearization == NULL) {
                st = rx_nomem(err);
                goto fail;
            }
            for (i = 0; i < n; i++) {
                uint32_t v = 0;
                (void)rx_uint(t, &e, i, &v);
                s.linearization[i] = (uint16_t)(v < 65535 ? v : 65535);
            }
            s.nlinearization = n;
        }
    }
    if (s.linearization != NULL) {
        uint16_t m = 0;
        size_t i;
        for (i = 0; i < s.nlinearization; i++) {
            m = s.linearization[i] > m ? s.linearization[i] : m;
        }
        white_default = (float)m;
    } else {
        white_default = max;
    }
    nwl = rx_tag_floats(t, &raw, RX_WHITE_LEVEL, wl, 3);
    for (k = 0; k < 3; k++) {
        if (nwl > 0) {
            float w = pc_f64_to_f32((size_t)k < nwl ? wl[k] : wl[0]);
            s.white[k] = w > 0.0f ? w : white_default;
        } else {
            s.white[k] = white_default;
        }
    }
    if (rx_tag_uints(t, &raw, RX_BLACK_LEVEL_REPEAT_DIM, rc, 2) == 2 && rc[0] >= 1 && rc[0] <= 16 && rc[1] >= 1 &&
        rc[1] <= 16) {
        rows = rc[0];
        cols = rc[1];
    }
    nbl = rx_ifd_get(t, &raw, RX_BLACK_LEVEL, &e) ? rx_floats_n(t, &e) : 0;
    if (nbl > 0 && nbl == rows * cols * samples) {
        if ((st = float_list(t, &raw, RX_BLACK_LEVEL, nbl, &s.black.values, &s.black.nvalues, err)) != PC_OK) {
            goto fail;
        }
        s.black.rows = rows;
        s.black.cols = cols;
    } else {
        double first = 0.0;
        if (nbl > 0) {
            (void)rx_float(t, &e, 0, &first);
        }
        if ((st = rx_black_uniform(&s.black, pc_f64_to_f32(first), err)) != PC_OK) {
            goto fail;
        }
    }
    if ((st = float_list(t, &raw, RX_BLACK_LEVEL_DELTA_H, active.width, &s.black.delta_h, &s.black.ndelta_h, err)) !=
            PC_OK ||
        (st = float_list(t, &raw, RX_BLACK_LEVEL_DELTA_V, active.height, &s.black.delta_v, &s.black.ndelta_v, err)) !=
            PC_OK) {
        goto fail;
    }
    color_info(t, &ifd0, &s.color);
    /* A monochrome image has no colour to calibrate: it develops as neutral grey. */
    if (s.color.ncalibrations == 0 && (is_cfa || samples != 1) &&
        (st = rx_warn(&s.warnings, err, "the DNG has no colour matrix; colours are approximate")) != PC_OK) {
        goto fail;
    }
    be = rx_tag_floats(t, &ifd0, RX_BASELINE_EXPOSURE, wl, 1) > 0 ? wl[0] : 0.0;
    beo = rx_tag_floats(t, &raw, RX_BASELINE_EXPOSURE_OFFSET, wl, 1) > 0 ? wl[0] : 0.0;
    be = be + beo;
    s.baseline_exposure = isfinite(be) ? pc_clampd(be, -10.0, 10.0) : 0.0;
    s.make = rx_tag_ascii(t, &ifd0, RX_MAKE, &nomem);
    s.model = rx_tag_ascii(t, &ifd0, RX_UNIQUE_CAMERA_MODEL, &nomem);
    if (s.model == NULL && !nomem) {
        s.model = rx_tag_ascii(t, &ifd0, RX_MODEL, &nomem);
    }
    if (nomem) {
        st = rx_nomem(err);
        goto fail;
    }
    s.format = PC_RAW_DNG;
    s.width = plane.width;
    s.height = plane.height;
    s.samples = samples;
    s.active = active;
    s.orientation = rx_orientation(t, &ifd0);
    *out = s;
    return PC_OK;
fail:
    pc_raw_sensor_fini(&s);
    return st;
}
