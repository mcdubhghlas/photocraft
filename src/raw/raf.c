/* Fujifilm RAF, uncompressed Bayer and X-Trans (port of crates/raw/src/raf.rs; see it for the
 * sources: ExifTool's FujiFilm RAF and FujiIFD tables and observation of CC0 samples).
 *
 * A big-endian header (magic, model name at 28, then offset / length pairs: the preview JPEG at 84,
 * a metadata directory at 92, the CFA region at 100). The metadata directory is a record count
 * followed by (tag, size, data) records. The CFA region is a small TIFF whose IFD0 points (0xF000)
 * at a Fuji IFD with the raw size, bit depth, strip, black level and white balance. Samples are
 * 16-bit containers, 12-bit LSB-first bit streams or 14-bit samples MSB first in little-endian
 * 32-bit words, told apart by the strip's byte count; anything shorter is Fujifilm's undocumented
 * compression (unsupported). XTransLayout / FujiLayout list the CFA reversed. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "raw/internal.h"

#define CROP_TOP_LEFT 0x0110
#define CROPPED_SIZE 0x0111
#define FUJI_LAYOUT 0x0130
#define XTRANS_LAYOUT 0x0131
#define WB_GRGB_LEVELS 0x2FF0
#define RAW_EXPOSURE_BIAS 0x9650

#define FUJI_IFD 0xF000
#define RAW_WIDTH 0xF001
#define RAW_HEIGHT 0xF002
#define RAW_BITS 0xF003
#define STRIP_OFFSET 0xF007
#define STRIP_BYTE_COUNT 0xF008
#define BLACK_LEVEL 0xF00A
#define WB_GRB_LEVELS 0xF00E

/* Most metadata records read. */
#define MAX_RECORDS 512

static bool be32(const uint8_t *b, size_t n, size_t at, size_t *v)
{
    if (at > n || n - at < 4) {
        return false;
    }
    *v = (size_t)b[at] << 24 | (size_t)b[at + 1] << 16 | (size_t)b[at + 2] << 8 | b[at + 3];
    return true;
}

static bool be16(const uint8_t *b, size_t n, size_t at, uint16_t *v)
{
    if (at > n || n - at < 2) {
        return false;
    }
    *v = (uint16_t)(b[at] << 8 | b[at + 1]);
    return true;
}

bool rx_raf_preview_range(const uint8_t *b, size_t n, size_t *off, size_t *len)
{
    return be32(b, n, 84, off) && be32(b, n, 88, len);
}

typedef struct record {
    uint16_t tag;
    const uint8_t *data;
    size_t len;
} record;

/* The metadata directory's records (at most MAX_RECORDS). */
static size_t records(const uint8_t *bytes, size_t n, record *out)
{
    size_t at;
    size_t len;
    const uint8_t *dir;
    size_t count;
    size_t p = 4;
    size_t k = 0;
    size_t i;
    if (!be32(bytes, n, 92, &at) || !be32(bytes, n, 96, &len) || at > n || n - at < len) {
        return 0;
    }
    dir = bytes + at;
    if (!be32(dir, len, 0, &count)) {
        count = 0;
    }
    count = count < MAX_RECORDS ? count : MAX_RECORDS;
    for (i = 0; i < count; i++) {
        uint16_t tag;
        uint16_t size;
        size_t start;
        if (!be16(dir, len, p, &tag) || !be16(dir, len, p + 2, &size)) {
            break;
        }
        start = p + 4;
        if (start > len || len - start < size) {
            break;
        }
        out[k].tag = tag;
        out[k].data = dir + start;
        out[k].len = size;
        k++;
        p = start + size;
    }
    return k;
}

static const record *find(const record *recs, size_t n, uint16_t tag)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (recs[i].tag == tag) {
            return &recs[i];
        }
    }
    return NULL;
}

/* A record holding two big-endian 16-bit values. */
static bool pair(const record *recs, size_t n, uint16_t tag, size_t *a, size_t *b)
{
    const record *r = find(recs, n, tag);
    uint16_t x;
    uint16_t y;
    if (r == NULL || !be16(r->data, r->len, 0, &x) || !be16(r->data, r->len, 2, &y)) {
        return false;
    }
    *a = x;
    *b = y;
    return true;
}

/* The CFA from XTransLayout (6x6) or FujiLayout (2x2), both stored reversed. */
static bool layout_cfa(const record *recs, size_t n, pc_raw_cfa *out)
{
    const record *x = find(recs, n, XTRANS_LAYOUT);
    const record *f;
    pc_raw_cfa c;
    size_t i;
    memset(&c, 0, sizeof c);
    if (x != NULL && x->len == 36) {
        for (i = 0; i < 36; i++) {
            uint8_t v = x->data[35 - i];
            if (v > 2) {
                return false;
            }
            c.colors[i] = v;
        }
        c.width = 6;
        c.height = 6;
        *out = c;
        return true;
    }
    f = find(recs, n, FUJI_LAYOUT);
    if (f == NULL || f->len != 4) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        uint8_t v = f->data[3 - i] & 3;
        c.colors[i] = v == 3 ? 1 : v;
    }
    c.width = 2;
    c.height = 2;
    if (!pc_raw_cfa_is_bayer(&c)) {
        return false;
    }
    *out = c;
    return true;
}

static uint16_t packed_sample(const uint8_t *src, size_t n, size_t i, uint32_t bits, rx_raf_packing packing)
{
    size_t at = i * bits;
    size_t byte = at / 8;
    uint32_t shift = (uint32_t)(at % 8);
    uint32_t b = bits < 16 ? bits : 16;
    uint32_t mask = (1u << b) - 1;
    uint32_t w = 0;
    size_t k;
    switch (packing) {
    case RX_RAF_LSB_FIRST:
        for (k = 0; k < 4; k++) {
            w |= (uint32_t)(byte + k < n ? src[byte + k] : 0) << (8 * k);
        }
        return (uint16_t)((w >> shift) & mask);
    case RX_RAF_WORDS32_MSB_FIRST:
        /* Logical byte k is physical byte k with its position in the 32-bit word reversed. */
        for (k = 0; k < 4; k++) {
            size_t q = byte + k;
            size_t phys = (q & ~(size_t)3) | (3 - (q & 3));
            w = (w << 8) | (phys < n ? src[phys] : 0u);
        }
        return (uint16_t)((w >> (32 - shift - b)) & mask);
    case RX_RAF_U16_LE:
    case RX_RAF_U16_BE:
    default: {
        uint8_t c0 = 2 * i < n ? src[2 * i] : 0;
        uint8_t c1 = 2 * i + 1 < n ? src[2 * i + 1] : 0;
        return (uint16_t)(packing == RX_RAF_U16_LE ? (c0 | c1 << 8) : (c0 << 8 | c1));
    }
    }
}

typedef struct unpack_ctx {
    const uint8_t *src;
    size_t n;
    size_t width;
    uint32_t bits;
    rx_raf_packing packing;
    uint16_t *out;
} unpack_ctx;

static pc_status unpack_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    unpack_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin * c->width; i < end * c->width; i++) {
        c->out[i] = packed_sample(c->src, c->n, i, c->bits, c->packing);
    }
    return PC_OK;
}

pc_status rx_raf_unpack(const uint8_t *src, size_t n, size_t width, size_t height, uint32_t bits,
                        rx_raf_packing packing, uint16_t **out, pc_error *err)
{
    unpack_ctx c;
    size_t total;
    if (!pc_size_mul(width, height, &total)) {
        return rx_malformed(err, "RAF raw data is too large");
    }
    c.src = src;
    c.n = n;
    c.width = width;
    c.bits = bits;
    c.packing = packing;
    c.out = pc_malloc_array(total > 0 ? total : 1, sizeof *c.out);
    if (c.out == NULL) {
        return rx_nomem(err);
    }
    (void)pc_pool_for(pc_pool_global(), height, rx_band_rows(width), unpack_rows, &c);
    *out = c.out;
    return PC_OK;
}

/* Orientation, make and model from the preview JPEG's EXIF (APP1) block. */
static pc_status preview_exif(const uint8_t *bytes, size_t n, uint16_t *orientation, char **make,
                              char **model, pc_error *err)
{
    size_t off;
    size_t len;
    const uint8_t *jpeg;
    rx_tiff t;
    rx_ifd ifd0;
    uint32_t o;
    bool nomem = false;
    *orientation = 0;
    *make = NULL;
    *model = NULL;
    if (!rx_raf_preview_range(bytes, n, &off, &len) || off > n || n - off < len) {
        return PC_OK;
    }
    jpeg = bytes + off;
    if (len < 12 || memcmp(jpeg, "\xFF\xD8\xFF\xE1", 4) != 0 || memcmp(jpeg + 6, "Exif\0\0", 6) != 0) {
        return PC_OK;
    }
    if (!rx_tiff_new(jpeg + 12, len - 12, &t) || !rx_ifd_at(&t, t.first_ifd, 0, &ifd0)) {
        return PC_OK;
    }
    if (rx_tag_uint(&t, &ifd0, RX_ORIENTATION, &o) && o >= 1 && o <= 8) {
        *orientation = (uint16_t)o;
    }
    *make = rx_tag_ascii(&t, &ifd0, RX_MAKE, &nomem);
    if (!nomem) {
        *model = rx_tag_ascii(&t, &ifd0, RX_MODEL, &nomem);
    }
    if (nomem) {
        free(*make);
        free(*model);
        *make = NULL;
        *model = NULL;
        return rx_nomem(err);
    }
    return PC_OK;
}

/* The model name in the RAF header (bytes 28..60). */
static char *header_model(const uint8_t *bytes, size_t n, bool *nomem)
{
    if (n < 60) {
        return NULL;
    }
    return rx_lossy_trimmed(bytes + 28, 32, nomem);
}

static void raf_wb(const rx_tiff *t, const rx_ifd *fuji, const record *recs, size_t nrec, pc_raw_sensor *s)
{
    double grb[3];
    double m[3];
    bool have = false;
    int k;
    if (rx_tag_floats(t, fuji, WB_GRB_LEVELS, grb, 3) >= 3 && grb[0] > 0.0) {
        m[0] = grb[1] / grb[0];
        m[1] = 1.0;
        m[2] = grb[2] / grb[0];
        have = true;
    } else {
        const record *r = find(recs, nrec, WB_GRGB_LEVELS);
        uint16_t v[4];
        if (r != NULL && be16(r->data, r->len, 0, &v[0]) && be16(r->data, r->len, 2, &v[1]) &&
            be16(r->data, r->len, 4, &v[2]) && be16(r->data, r->len, 6, &v[3])) {
            double g = (double)v[0] + (double)v[2];
            if (g > 0.0) {
                m[0] = 2.0 * (double)v[1] / g;
                m[1] = 1.0;
                m[2] = 2.0 * (double)v[3] / g;
                have = true;
            }
        }
    }
    for (k = 0; k < 3 && have; k++) {
        have = m[k] >= 0.25 && m[k] < 8.0;
    }
    if (have) {
        s->has_camera_wb = true;
        memcpy(s->camera_wb, m, sizeof m);
    }
}

static pc_status raf_black(const rx_tiff *t, const rx_ifd *fuji, pc_raw_sensor *s, pc_error *err)
{
    rx_entry e;
    size_t n = rx_ifd_get(t, fuji, BLACK_LEVEL, &e) ? rx_floats_n(t, &e) : 0;
    double sum = -0.0;
    size_t i;
    bool negative = false;
    for (i = 0; i < n; i++) {
        double v = 0.0;
        (void)rx_float(t, &e, i, &v);
        negative = negative || v < 0.0;
        sum += v;
    }
    if (n == 0 || negative) {
        pc_status st = rx_warn(&s->warnings, err, "RAF: black level not recorded; assumed 0");
        return st != PC_OK ? st : rx_black_uniform(&s->black, 0.0f, err);
    }
    /* One value per CFA position; their order is not established and they differ by a couple of
     * units at most, so their mean. */
    return rx_black_uniform(&s->black, pc_f64_to_f32(sum / (double)n), err);
}

pc_status rx_raf_decode(const uint8_t *bytes, size_t n, const pc_raw_limits *limits, pc_raw_sensor *out,
                        pc_error *err)
{
    size_t cfa_at;
    size_t cfa_len;
    const uint8_t *region;
    size_t rlen;
    rx_tiff t;
    rx_ifd ifd0;
    rx_ifd fuji;
    uint32_t v;
    size_t width;
    size_t height;
    uint32_t bits;
    size_t offset;
    size_t count;
    const uint8_t *src;
    size_t slen;
    size_t pixels;
    size_t packed;
    rx_raf_packing packing;
    size_t need;
    record *recs = NULL;
    size_t nrec;
    pc_raw_sensor s;
    pc_raw_rect full;
    size_t a;
    size_t b;
    size_t c2;
    size_t d;
    const record *r;
    uint16_t orientation;
    bool nomem = false;
    pc_status st;
    if (!be32(bytes, n, 100, &cfa_at)) {
        return rx_malformed(err, "RAF header is truncated");
    }
    if (!be32(bytes, n, 104, &cfa_len)) {
        cfa_len = 0;
    }
    if (cfa_at > n) {
        return rx_malformed(err, "RAF CFA region lies outside the file");
    }
    region = bytes + cfa_at;
    rlen = n - cfa_at;
    if (cfa_len > 0 && cfa_len <= rlen) {
        rlen = cfa_len;
    }
    if (!rx_tiff_new(region, rlen, &t)) {
        return rx_unsupported(err, "Fujifilm RAF of an early FinePix / SuperCCD body (no CFA TIFF) is not "
                                   "decoded yet");
    }
    if (!rx_ifd_at(&t, t.first_ifd, 0, &ifd0)) {
        return rx_malformed(err, "RAF CFA TIFF has no IFD0");
    }
    if (!rx_tag_uint(&t, &ifd0, FUJI_IFD, &v)) {
        return rx_unsupported(err, "RAF without a Fuji raw IFD");
    }
    if (!rx_ifd_at(&t, v, 0, &fuji)) {
        return rx_malformed(err, "RAF Fuji raw IFD is unreadable");
    }
    if (!rx_tag_uint(&t, &fuji, RAW_WIDTH, &v)) {
        return rx_malformed(err, "RAF has no raw width");
    }
    width = v;
    if (!rx_tag_uint(&t, &fuji, RAW_HEIGHT, &v)) {
        return rx_malformed(err, "RAF has no raw height");
    }
    height = v;
    bits = rx_tag_uint(&t, &fuji, RAW_BITS, &v) ? v : 14;
    if (bits < 8 || bits > 16) {
        return rx_unsupported(err, "RAF with %u-bit samples", (unsigned)bits);
    }
    if ((st = pc_raw_limits_check(limits, width, height, 2, err)) != PC_OK) {
        return st;
    }
    if (!rx_tag_uint(&t, &fuji, STRIP_OFFSET, &v)) {
        return rx_malformed(err, "RAF has no strip offset");
    }
    offset = v;
    count = rx_tag_uint(&t, &fuji, STRIP_BYTE_COUNT, &v) ? v : 0;
    if (offset > rlen) {
        return rx_malformed(err, "RAF raw data lies outside the file");
    }
    src = region + offset;
    slen = rlen - offset;
    if (count > 0 && count <= slen) {
        slen = count;
    }
    pixels = width * height; /* bounded by the limits */
    packed = pixels > SIZE_MAX / bits ? SIZE_MAX : pixels * bits;
    packed = packed / 8 + (packed % 8 != 0);
    if (count >= pixels * 2) {
        packing = t.le ? RX_RAF_U16_LE : RX_RAF_U16_BE;
    } else if (count >= packed && bits == 12) {
        packing = RX_RAF_LSB_FIRST;
    } else if (count >= packed && bits == 14) {
        packing = RX_RAF_WORDS32_MSB_FIRST;
    } else if (count >= packed) {
        return rx_unsupported(err, "Fujifilm RAF with packed %u-bit samples", (unsigned)bits);
    } else {
        return rx_unsupported(err,
                              "Fujifilm compressed RAF is not decoded (no public description of the format)");
    }
    need = packing == RX_RAF_U16_LE || packing == RX_RAF_U16_BE ? pixels * 2 : packed;
    if (slen < need) {
        return rx_malformed(err, "RAF raw data is truncated");
    }
    pc_raw_sensor_init(&s);
    if ((st = rx_raf_unpack(src, slen, width, height, bits, packing, &s.data, err)) != PC_OK) {
        return st;
    }
    s.ndata = pixels;
    recs = pc_malloc_array(MAX_RECORDS, sizeof *recs);
    if (recs == NULL) {
        st = rx_nomem(err);
        goto fail;
    }
    nrec = records(bytes, n, recs);
    if (!layout_cfa(recs, nrec, &s.cfa)) {
        st = rx_unsupported(err, "Fujifilm RAF without a recognised CFA layout");
        goto fail;
    }
    s.has_cfa = true;
    full = pc_raw_rect_make(0, 0, width, height);
    if ((st = raf_black(&t, &fuji, &s, err)) != PC_OK) {
        goto fail;
    }
    s.white[0] = s.white[1] = s.white[2] = rx_clip_level(s.data, pixels, bits);
    raf_wb(&t, &fuji, recs, nrec, &s);
    s.crop = full;
    if (pair(recs, nrec, CROP_TOP_LEFT, &a, &b) && pair(recs, nrec, CROPPED_SIZE, &c2, &d)) {
        s.crop = pc_raw_rect_intersect(pc_raw_rect_make(b, a, d, c2), full);
    }
    if (pc_raw_rect_is_empty(s.crop)) {
        s.crop = full;
    }
    /* RawExposureBias: how far the raw was exposed below the camera's rendering (dynamic-range
     * modes); undone like a DNG BaselineExposure. */
    r = find(recs, nrec, RAW_EXPOSURE_BIAS);
    {
        uint16_t num;
        uint16_t den;
        if (r != NULL && be16(r->data, r->len, 0, &num) && be16(r->data, r->len, 2, &den) && den != 0) {
            double e = -(double)(int16_t)num / (double)(int16_t)den;
            if (isfinite(e) && fabs(e) <= 8.0) {
                s.baseline_exposure = e;
            }
        }
    }
    if ((st = preview_exif(bytes, n, &orientation, &s.make, &s.model, err)) != PC_OK) {
        goto fail;
    }
    if (s.make == NULL && (s.make = pc_strndup("FUJIFILM", 8)) == NULL) {
        st = rx_nomem(err);
        goto fail;
    }
    if (s.model == NULL) {
        s.model = header_model(bytes, n, &nomem);
        if (nomem) {
            st = rx_nomem(err);
            goto fail;
        }
    }
    free(recs);
    s.format = PC_RAW_RAF;
    s.width = width;
    s.height = height;
    s.samples = 1;
    s.active = full;
    s.orientation = orientation != 0 ? orientation : 1;
    *out = s;
    return PC_OK;
fail:
    free(recs);
    pc_raw_sensor_fini(&s);
    return st;
}
