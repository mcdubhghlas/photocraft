/* Canon CR2 (port of crates/raw/src/cr2.rs; see it for the format notes and sources). One lossless
 * JPEG fills the sensor image in vertical slices (tag 0xC640). Geometry and the as-shot white
 * balance come from the Canon maker note (SensorInfo 0x00E0, ColorData 0x4001); the black level is
 * measured on the masked border, the white level is the clipping point in the data, and the
 * colour-filter row phase is measured from the data (a small model table decides when the
 * measurement is inconclusive). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "raw/internal.h"

#define CANON_SENSOR_INFO 0x00E0
#define CANON_COLOR_DATA 0x4001
#define CANON_MODEL_ID 0x0010

/* Canon model IDs whose raw data starts with a green/blue row: EOS 5D Mark II, 7D, 550D, 600D,
 * 1200D. Only consulted when the measurement is inconclusive. */
static const uint32_t GREEN_FIRST_MODELS[5] = {0x80000218u, 0x80000250u, 0x80000270u, 0x80000286u, 0x80000327u};

static size_t sat_add(size_t a, size_t b) { return a > SIZE_MAX - b ? SIZE_MAX : a + b; }

/* data[y * width .. (y + 1) * width], or NULL. */
static const uint16_t *row_at(const uint16_t *data, size_t n, size_t width, size_t y)
{
    size_t a;
    size_t b;
    if (!pc_size_mul(y, width, &a) || y == SIZE_MAX || !pc_size_mul(y + 1, width, &b) || b > n) {
        return NULL;
    }
    return data + a;
}

bool rx_measured_red_row(const uint16_t *data, size_t n, size_t width, pc_raw_rect area, float black, float white,
                         size_t *out)
{
    double diff[2] = {0.0, 0.0};
    double upper[2] = {0.0, 0.0};
    double lower[2] = {0.0, 0.0};
    uint64_t cnt[2] = {0, 0};
    float lo = black + 1.0f;
    float hi = black + (white - black) * 0.98f;
    size_t xe = sat_add(area.x, area.width);
    size_t x_end = (xe < width ? xe : width);
    size_t y_end = sat_add(area.y, area.height);
    size_t y = sat_add(area.y, area.y & 1);
    double d[2];
    double rel[2];
    int p;
    x_end = x_end > 0 ? x_end - 1 : 0;
    y_end = y_end > 0 ? y_end - 1 : 0;
    while (y < y_end) {
        const uint16_t *r0 = row_at(data, n, width, y);
        const uint16_t *r1 = y < SIZE_MAX ? row_at(data, n, width, y + 1) : NULL;
        size_t x;
        if (r0 == NULL || r1 == NULL) {
            break;
        }
        for (x = area.x; x < x_end; x++) {
            float a;
            float b;
            if (x >= width || x + 1 >= width) {
                break;
            }
            a = (float)r0[x];
            b = (float)r1[x + 1];
            if (a < lo || b < lo || a > hi || b > hi) {
                continue;
            }
            p = (int)(x & 1);
            diff[p] += (double)fabsf(a - b);
            upper[p] += (double)(a - black);
            lower[p] += (double)(b - black);
            cnt[p]++;
        }
        y = sat_add(y, 4);
    }
    if (cnt[0] < 256 || cnt[1] < 256) {
        return false;
    }
    for (p = 0; p < 2; p++) {
        d[p] = diff[p] / (double)cnt[p];
        rel[p] = fabs(upper[p] - lower[p]) / pc_rs_max_k(upper[p] + lower[p], 1.0);
    }
    if (d[0] < 0.8 * d[1] && rel[0] < rel[1]) {
        *out = 1;
        return true;
    }
    if (d[1] < 0.8 * d[0] && rel[1] < rel[0]) {
        *out = 0;
        return true;
    }
    return false;
}

float rx_clip_level(const uint16_t *data, size_t n, uint32_t precision)
{
    float full = (float)((1u << (precision < 16 ? precision : 16)) - 1);
    size_t step = n / 2000000;
    uint16_t max = 0;
    size_t i;
    size_t near = 0;
    size_t sampled;
    if (step < 1) {
        step = 1;
    }
    for (i = 0; i < n; i += step) {
        max = data[i] > max ? data[i] : max;
    }
    if (max == 0) {
        return full;
    }
    for (i = 0; i < n; i += step) {
        near += (uint32_t)data[i] + (uint32_t)(max / 256) >= (uint32_t)max;
    }
    sampled = n / step + (n % step != 0);
    if (near * 2000 >= sampled) {
        /* At least 0.05 % of samples at the top: that is the clipping point (backed off slightly
         * so the clipped plateau maps to white). */
        return (float)max * 0.995f;
    }
    return full;
}

bool rx_masked_black(const uint16_t *data, size_t n, size_t width, pc_raw_rect area, float out[4])
{
    uint64_t sum[4] = {0, 0, 0, 0};
    uint64_t cnt[4] = {0, 0, 0, 0};
    size_t y;
    int k;
    if (pc_raw_rect_is_empty(area) || area.width < 2 || area.height < 2 || area.y > SIZE_MAX - area.height ||
        area.x > SIZE_MAX - area.width) {
        return false;
    }
    for (y = area.y; y < area.y + area.height; y++) {
        size_t x;
        for (x = area.x; x < area.x + area.width; x++) {
            size_t i;
            if (!pc_size_mul(y, width, &i) || i > SIZE_MAX - x || i + x >= n) {
                return false;
            }
            k = (int)((y & 1) * 2 + (x & 1));
            sum[k] += data[i + x];
            cnt[k]++;
        }
    }
    for (k = 0; k < 4; k++) {
        if (cnt[k] == 0) {
            return false;
        }
    }
    for (k = 0; k < 4; k++) {
        out[k] = (float)sum[k] / (float)cnt[k];
    }
    return true;
}

static bool maker_note(const rx_tiff *t, const rx_ifd *ifds, size_t n, rx_ifd *out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        rx_entry e;
        if (rx_ifd_get(t, &ifds[i], RX_MAKER_NOTE, &e)) {
            return rx_ifd_at(t, e.at, 0, out);
        }
    }
    return false;
}

/* ColorData's words (file byte order) at word offset off, within the entry's own bytes. */
static bool word(const rx_tiff *t, const rx_entry *e, size_t bytes, size_t off, double *v)
{
    size_t at = off * 2;
    uint16_t w;
    if (at > SIZE_MAX - e->at || e->at > SIZE_MAX - bytes) {
        return false;
    }
    at += e->at;
    if (at > SIZE_MAX - 2 || at + 2 > e->at + bytes || !rx_u16_at(t, at, &w)) {
        return false;
    }
    *v = w;
    return true;
}

static bool as_shot_wb(const rx_tiff *t, const rx_ifd *mn, double out[3])
{
    rx_entry e;
    size_t unit;
    size_t bytes;
    static const size_t OFFS[4] = {0x3F, 0x47, 0x19, 0x22};
    size_t k;
    if (!rx_ifd_get(t, mn, CANON_COLOR_DATA, &e)) {
        return false;
    }
    unit = rx_type_size(e.typ);
    if (unit != 0 && e.count > SIZE_MAX / unit) {
        return false;
    }
    bytes = unit * e.count;
    for (k = 0; k < 4; k++) {
        double r;
        double g1;
        double g2;
        double b;
        double g;
        if (!word(t, &e, bytes, OFFS[k], &r) || !word(t, &e, bytes, OFFS[k] + 1, &g1) ||
            !word(t, &e, bytes, OFFS[k] + 2, &g2) || !word(t, &e, bytes, OFFS[k] + 3, &b)) {
            continue;
        }
        g = (g1 + g2) / 2.0;
        if (g >= 256.0 && g < 16384.0 && fabs(g1 - g2) <= g * 0.05 && r / g >= 0.25 && r / g < 8.0 &&
            b / g >= 0.25 && b / g < 8.0) {
            out[0] = r / g;
            out[1] = 1.0;
            out[2] = b / g;
            return true;
        }
    }
    return false;
}

/* Black levels measured per data-parity 2x2 position, re-indexed from the active origin. */
static void realign_black(pc_raw_black *b, pc_raw_rect active)
{
    float v[4];
    int k;
    if (b->rows != 2 || b->cols != 2 || b->nvalues != 4) {
        return;
    }
    memcpy(v, b->values, sizeof v);
    for (k = 0; k < 4; k++) {
        size_t x = (active.x + (size_t)(k & 1)) & 1;
        size_t y = (active.y + (size_t)(k >> 1)) & 1;
        b->values[k] = v[y * 2 + x];
    }
}

pc_status rx_cr2_decode(const rx_tiff *t, const pc_raw_limits *limits, pc_raw_sensor *out, pc_error *err)
{
    rx_ifd ifds[RX_MAX_IFDS];
    size_t nifd = rx_all_ifds(t, ifds);
    rx_ifd ifd0;
    rx_ifd raw;
    uint32_t raw_off = 0;
    uint32_t v;
    size_t off;
    size_t len;
    const uint8_t *src;
    size_t srclen;
    rx_frame frame;
    size_t total;
    uint32_t sl[3];
    size_t widths[65];
    size_t nwidths;
    size_t width;
    size_t height;
    uint16_t *samples = NULL;
    size_t nsamples;
    pc_raw_sensor s;
    size_t pos = 0;
    size_t x0 = 0;
    size_t i;
    rx_ifd mn;
    bool has_mn;
    pc_raw_rect full;
    pc_raw_rect active;
    uint32_t info[9];
    float bv[4];
    bool have_black;
    float white;
    float mean_black;
    size_t red_row;
    bool nomem = false;
    pc_status st;
    pc_error e2;
    if (!rx_ifd_at(t, t->first_ifd, 0, &ifd0)) {
        return rx_malformed(err, "CR2 has no IFD0");
    }
    (void)rx_u32_at(t, 12, &raw_off);
    if (!rx_ifd_at(t, raw_off, 0, &raw)) {
        return rx_malformed(err, "CR2 raw IFD is missing");
    }
    if (!rx_tag_uint(t, &raw, RX_STRIP_OFFSETS, &v)) {
        return rx_malformed(err, "CR2 raw data offset missing");
    }
    off = v;
    len = rx_tag_uint(t, &raw, RX_STRIP_BYTE_COUNTS, &v) ? v : (t->len > off ? t->len - off : 0);
    src = rx_bytes(t, off, len);
    srclen = len;
    if (src == NULL) {
        if (off > t->len) {
            return rx_malformed(err, "CR2 raw data lies outside the file");
        }
        src = t->data + off;
        srclen = t->len - off;
    }
    st = rx_ljpeg_frame(src, srclen, &frame, &e2);
    if (st != PC_OK) {
        if (st == PC_ERR_UNSUPPORTED && strstr(e2.msg, "subsampled") != NULL) {
            return rx_unsupported(err, "Canon sRAW / mRAW");
        }
        if (err != NULL) {
            *err = e2;
        }
        return st;
    }
    if (frame.height > SIZE_MAX / frame.width || frame.width * frame.height > SIZE_MAX / frame.components) {
        return rx_malformed(err, "CR2 frame too large");
    }
    total = frame.width * frame.height * frame.components;
    if (rx_tag_uints(t, &raw, RX_CR2_SLICE, sl, 3) == 3 && sl[0] < 64 && sl[1] > 0 && sl[2] > 0) {
        size_t k;
        for (k = 0; k < sl[0]; k++) {
            widths[k] = sl[1];
        }
        widths[sl[0]] = sl[2];
        nwidths = (size_t)sl[0] + 1;
        width = (size_t)sl[0] * sl[1] + sl[2];
    } else {
        width = frame.width * frame.components;
        widths[0] = width;
        nwidths = 1;
    }
    if (width == 0 || total % width != 0) {
        return rx_malformed(err, "CR2 slices do not match the JPEG frame");
    }
    height = total / width;
    if ((st = pc_raw_limits_check(limits, width, height, 2, err)) != PC_OK ||
        (st = rx_ljpeg_decode(src, srclen, total, &frame, &samples, &nsamples, err)) != PC_OK) {
        return st;
    }
    pc_raw_sensor_init(&s);
    s.data = pc_calloc_array(width * height, sizeof *s.data);
    if (s.data == NULL) {
        free(samples);
        return rx_nomem(err);
    }
    /* De-slice. */
    for (i = 0; i < nwidths; i++) {
        size_t sw = widths[i];
        size_t y;
        for (y = 0; y < height; y++) {
            if (pos > nsamples || nsamples - pos < sw) {
                free(samples);
                st = rx_malformed(err, "CR2 slice data is short");
                goto fail;
            }
            memcpy(s.data + y * width + x0, samples + pos, sw * sizeof *samples);
            pos += sw;
        }
        x0 += sw;
    }
    free(samples);
    s.ndata = width * height;
    has_mn = maker_note(t, ifds, nifd, &mn);
    full = pc_raw_rect_make(0, 0, width, height);
    /* SensorInfo: [1] width, [2] height, [5] left, [6] top, [7] right, [8] bottom border. */
    active = full;
    if (has_mn && rx_tag_uints(t, &mn, CANON_SENSOR_INFO, info, 9) >= 9 && info[7] > info[5] && info[8] > info[6]) {
        active = pc_raw_rect_intersect(pc_raw_rect_make(info[5], info[6], (uint32_t)(info[7] - info[5] + 1),
                                                         (uint32_t)(info[8] - info[6] + 1)),
                                        full);
    }
    if (pc_raw_rect_is_empty(active)) {
        active = full;
    }
    /* Black: the outer half of the masked columns left of the image. */
    have_black = active.x >= 16 && rx_masked_black(s.data, s.ndata, width,
                                                   pc_raw_rect_make(4, active.y, active.x / 2 - 4, active.height), bv);
    if (have_black) {
        st = rx_black_2x2(&s.black, bv, err);
    } else {
        st = rx_warn(&s.warnings, err, "no masked sensor area; black level assumed to be 0");
        if (st == PC_OK) {
            st = rx_black_uniform(&s.black, 0.0f, err);
        }
    }
    if (st != PC_OK) {
        goto fail;
    }
    white = rx_clip_level(s.data, s.ndata, frame.precision);
    {
        float sum = -0.0f;
        for (i = 0; i < s.black.nvalues; i++) {
            sum += s.black.values[i];
        }
        mean_black = sum / (float)(s.black.nvalues > 0 ? s.black.nvalues : 1);
    }
    if (!rx_measured_red_row(s.data, s.ndata, width, active, mean_black, white, &red_row)) {
        uint32_t id;
        bool known = false;
        if (has_mn && rx_tag_uint(t, &mn, CANON_MODEL_ID, &id)) {
            for (i = 0; i < 5; i++) {
                known = known || GREEN_FIRST_MODELS[i] == id;
            }
        }
        if (!known && (st = rx_warn(&s.warnings, err,
                                    "CR2 colour filter phase could not be measured; assumed red/green in the "
                                    "first row")) != PC_OK) {
            goto fail;
        }
        red_row = known ? 1 : 0;
    }
    memset(&s.cfa, 0, sizeof s.cfa);
    s.cfa.width = 2;
    s.cfa.height = 2;
    if (red_row == 0) {
        static const uint8_t C[4] = {0, 1, 1, 2};
        memcpy(s.cfa.colors, C, 4);
    } else {
        static const uint8_t C[4] = {1, 2, 0, 1};
        memcpy(s.cfa.colors, C, 4);
    }
    s.has_cfa = true;
    s.has_camera_wb = has_mn && as_shot_wb(t, &mn, s.camera_wb);
    realign_black(&s.black, active);
    s.format = PC_RAW_CR2;
    s.make = rx_tag_ascii(t, &ifd0, RX_MAKE, &nomem);
    s.model = rx_tag_ascii(t, &ifd0, RX_MODEL, &nomem);
    if (nomem) {
        st = rx_nomem(err);
        goto fail;
    }
    s.width = width;
    s.height = height;
    s.samples = 1;
    s.white[0] = s.white[1] = s.white[2] = white;
    s.active = active;
    s.crop = active;
    s.orientation = rx_orientation(t, &ifd0);
    *out = s;
    return PC_OK;
fail:
    pc_raw_sensor_fini(&s);
    return st;
}
