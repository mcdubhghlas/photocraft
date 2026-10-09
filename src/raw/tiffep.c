/* TIFF/EP-structured raws (NEF, ARW, PEF and others), a port of crates/raw/src/tiffep.rs: sensor
 * data uncompressed or lossless JPEG in a standard CFA IFD, plus Sony cRAW and Nikon's
 * Huffman-compressed NEF. Black and white levels from DNG-style tags, then the vendors' documented
 * tags, else 0 with a warning; the white level falls back to the clipping point in the data; the
 * as-shot white balance from the documented vendor tags. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "raw/internal.h"

#define PHOTOMETRIC_CFA 32803u
#define NIKON_WB_RB_LEVELS 0x000C
#define NIKON_BLACK_LEVEL 0x003D
#define SONY_BLACK_LEVEL 0x7310
#define SONY_WB_RGGB_LEVELS 0x7313

static uint64_t area(const rx_tiff *t, const rx_ifd *i)
{
    uint32_t w = 0;
    uint32_t h = 0;
    (void)rx_tag_uint(t, i, RX_IMAGE_WIDTH, &w);
    (void)rx_tag_uint(t, i, RX_IMAGE_LENGTH, &h);
    return (uint64_t)w * h;
}

/* The largest CFA IFD (the last of equals, like max_by_key). */
static bool raw_ifd(const rx_tiff *t, const rx_ifd *ifds, size_t n, rx_ifd *out)
{
    size_t i;
    bool found = false;
    uint64_t best = 0;
    for (i = 0; i < n; i++) {
        uint32_t p;
        if (rx_tag_uint(t, &ifds[i], RX_PHOTOMETRIC, &p) && p == PHOTOMETRIC_CFA) {
            uint64_t a = area(t, &ifds[i]);
            if (!found || a >= best) {
                best = a;
                *out = ifds[i];
                found = true;
            }
        }
    }
    return found;
}

static bool starts_with_upper(const char *s, const char *prefix)
{
    size_t i;
    for (i = 0; prefix[i] != '\0'; i++) {
        char c = s[i];
        if (c == '\0') {
            return false;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != prefix[i]) {
            return false;
        }
    }
    return true;
}

bool rx_is_sony_non_cfa(const rx_tiff *t, const rx_ifd *ifds, size_t n)
{
    size_t i;
    bool has_type = false;
    bool sony = false;
    for (i = 0; i < n; i++) {
        has_type = has_type || rx_ifd_has(t, &ifds[i], RX_SONY_RAW_FILE_TYPE);
    }
    if (!has_type) {
        return false;
    }
    for (i = 0; i < n && !sony; i++) {
        bool nomem = false;
        char *m = rx_tag_ascii(t, &ifds[i], RX_MAKE, &nomem);
        sony = m != NULL && starts_with_upper(m, "SONY");
        free(m);
    }
    return sony;
}

bool rx_tiffep_cfa(const rx_tiff *t, const rx_ifd *raw, const rx_ifd *ifds, size_t n, pc_raw_cfa *out)
{
    uint32_t dim[2];
    size_t ndim = rx_tag_uints(t, raw, RX_CFA_REPEAT_PATTERN_DIM, dim, 2);
    rx_entry pe;
    size_t npat = rx_ifd_get(t, raw, RX_CFA_PATTERN, &pe) ? rx_uints_n(t, &pe) : 0;
    size_t rows;
    size_t cols;
    uint8_t colors[256];
    size_t i;
    pc_raw_cfa c;
    if (ndim == 2 && npat > 0 && npat == (size_t)dim[0] * dim[1]) {
        rows = dim[0];
        cols = dim[1];
        if (rows == 0 || cols == 0 || rows > 16 || cols > 16) {
            return false;
        }
        for (i = 0; i < npat; i++) {
            uint32_t v = 0;
            (void)rx_uint(t, &pe, i, &v);
            if (v > 2) {
                return false;
            }
            colors[i] = (uint8_t)v;
        }
    } else {
        /* EXIF CFAPattern: two 16-bit counts (columns, rows) then the colours. */
        rx_entry e;
        bool found = false;
        const uint8_t *b;
        size_t len;
        size_t rc;
        for (i = 0; i < n && !found; i++) {
            found = rx_ifd_get(t, &ifds[i], RX_EXIF_CFA_PATTERN, &e);
        }
        if (!found || (b = rx_raw(t, &e, &len)) == NULL || len < 4) {
            return false;
        }
        cols = t->le ? (size_t)(b[0] | b[1] << 8) : (size_t)(b[0] << 8 | b[1]);
        rows = t->le ? (size_t)(b[2] | b[3] << 8) : (size_t)(b[2] << 8 | b[3]);
        rc = rows * cols;
        if (len - 4 < rc) {
            return false;
        }
        if (rows == 0 || cols == 0 || rows > 16 || cols > 16) {
            return false;
        }
        for (i = 0; i < rc; i++) {
            if (b[4 + i] > 2) {
                return false;
            }
            colors[i] = b[4 + i];
        }
    }
    memset(&c, 0, sizeof c);
    c.width = cols;
    c.height = rows;
    memcpy(c.colors, colors, rows * cols);
    *out = c;
    return true;
}

/* Nikon maker note: "Nikon\0", version, then an embedded TIFF relative to itself. */
static bool nikon_maker_note(const rx_tiff *t, const rx_ifd *ifds, size_t n, rx_tiff *inner, rx_ifd *ifd)
{
    rx_entry e;
    bool found = false;
    const uint8_t *b;
    size_t len;
    size_t i;
    size_t base;
    for (i = 0; i < n && !found; i++) {
        found = rx_ifd_get(t, &ifds[i], RX_MAKER_NOTE, &e);
    }
    if (!found || (b = rx_raw(t, &e, &len)) == NULL || len < 6 || memcmp(b, "Nikon\0", 6) != 0) {
        return false;
    }
    if (e.at > SIZE_MAX - 10) {
        return false;
    }
    base = e.at + 10;
    if (base > t->len || !rx_tiff_new(t->data + base, t->len - base, inner)) {
        return false;
    }
    return rx_ifd_at(inner, inner->first_ifd, 0, ifd);
}

bool rx_rggb_by_position(const pc_raw_cfa *cfa, const double *v, size_t n, float out[4])
{
    double r;
    double g1;
    double g2;
    double b;
    int greens = 0;
    size_t k;
    if (n != 4) {
        return false;
    }
    for (k = 0; k < 4; k++) {
        if (!isfinite(v[k]) || v[k] < 0.0) {
            return false;
        }
    }
    r = v[0];
    g1 = v[1];
    g2 = v[2];
    b = v[3];
    for (k = 0; k < 4; k++) {
        double x;
        switch (pc_raw_cfa_color(cfa, k & 1, k >> 1)) {
        case 0:
            x = r;
            break;
        case 2:
            x = b;
            break;
        default:
            x = greens == 0 ? g1 : greens == 1 ? g2 : g1;
            greens++;
        }
        out[k] = pc_f64_to_f32(x);
    }
    return true;
}

size_t rx_padded_width(const uint16_t *data, size_t len, size_t w, size_t h, uint32_t bits)
{
    uint16_t fill;
    size_t aw;
    size_t at;
    uint32_t top = (1u << (bits < 16 ? bits : 16)) / 4;
    if (w < 256 || h < 2 || (h != 0 && w > SIZE_MAX / h) || len < w * h) {
        return w;
    }
    at = (h / 2) * w + w - 1;
    if (at >= len) {
        return w;
    }
    fill = data[at];
    if ((uint32_t)fill >= top) {
        return w;
    }
    aw = w;
    while (aw > w - 128) {
        size_t x = aw - 1;
        size_t y;
        size_t count = 0;
        for (y = 0; y < h; y++) {
            count += data[y * w + x] == fill;
        }
        if (count * 10 < h * 9) {
            break;
        }
        aw--;
    }
    return aw & ~(size_t)1;
}

pc_status rx_tiffep_decode(const rx_tiff *t, pc_raw_format format, const pc_raw_limits *limits, pc_raw_sensor *out,
                           pc_error *err)
{
    rx_ifd ifds[RX_MAX_IFDS];
    size_t n = rx_all_ifds(t, ifds);
    rx_ifd raw;
    rx_tiff note_t;
    rx_ifd note;
    bool nikon = false;
    rx_plane plane;
    pc_raw_cfa cfa;
    pc_raw_sensor s;
    pc_status st;
    uint32_t comp;
    size_t width;
    pc_raw_rect full;
    double bl[1];
    double vb[4];
    size_t nbl;
    size_t nvb;
    double wl[1];
    double v[4];
    size_t nv;
    float rggb[4];
    float white;
    bool nomem = false;
    double co[2];
    double cs[2];
    rx_ifd ifd0;
    memset(&plane, 0, sizeof plane);
    if (!raw_ifd(t, ifds, n, &raw)) {
        if (rx_is_sony_non_cfa(t, ifds, n)) {
            return rx_unsupported(err, "Sony reduced-size (M / S) lossless ARW");
        }
        return rx_unsupported(err, "%s: no CFA image found", pc_raw_format_name(format));
    }
    if (format == PC_RAW_NEF) {
        nikon = nikon_maker_note(t, ifds, n, &note_t, &note);
    }
    if (format == PC_RAW_ARW && rx_sony_is_craw(t, &raw)) {
        st = rx_sony_read_craw(t, &raw, limits, &plane, err);
    } else if (format == PC_RAW_NEF && rx_tag_uint(t, &raw, RX_COMPRESSION, &comp) && comp == RX_NIKON_COMPRESSION) {
        st = rx_nef_read_compressed(t, &raw, nikon ? &note_t : NULL, nikon ? &note : NULL, limits, &plane, err);
    } else {
        st = rx_read_plane(t, &raw, limits, RX_QUADS, &plane, err);
    }
    if (st != PC_OK) {
        return st;
    }
    pc_raw_sensor_init(&s);
    s.data = plane.data;
    if (plane.samples != 1) {
        st = rx_unsupported(err, "CFA data with %zu samples per pixel", plane.samples);
        goto fail;
    }
    if (!rx_tiffep_cfa(t, &raw, ifds, n, &cfa)) {
        st = rx_unsupported(err, "%s: CFA pattern not recorded", pc_raw_format_name(format));
        goto fail;
    }
    if (!pc_raw_cfa_is_bayer(&cfa)) {
        st = rx_unsupported(err, "%zux%zu non-Bayer CFA", cfa.height, cfa.width);
        goto fail;
    }
    /* Nikon pads some sensor rows on the right; they are not image data. */
    width = format == PC_RAW_NEF
                ? rx_padded_width(plane.data, plane.width * plane.height, plane.width, plane.height, plane.bits)
                : plane.width;
    full = pc_raw_rect_make(0, 0, width, plane.height);
    s.make = rx_find_ascii(t, ifds, n, RX_MAKE, &nomem);
    s.model = rx_find_ascii(t, ifds, n, RX_MODEL, &nomem);
    if (nomem) {
        st = rx_nomem(err);
        goto fail;
    }
    nbl = rx_tag_floats(t, &raw, RX_BLACK_LEVEL, bl, 1);
    if (nikon) {
        size_t k;
        nvb = rx_tag_floats(&note_t, &note, NIKON_BLACK_LEVEL, vb, 4);
        /* Nikon's BlackLevel is in 14-bit units whatever the sample depth. */
        for (k = 0; k < nvb && k < 4; k++) {
            vb[k] = plane.bits == 12 ? vb[k] / 4.0 : vb[k];
        }
    } else {
        nvb = rx_tag_floats(t, &raw, SONY_BLACK_LEVEL, vb, 4);
    }
    if (nbl > 0) {
        st = rx_black_uniform(&s.black, pc_f64_to_f32(bl[0]), err);
    } else if (rx_rggb_by_position(&cfa, vb, nvb, rggb)) {
        st = rx_black_2x2(&s.black, rggb, err);
    } else if (nikon) {
        /* Nikon bodies that write no BlackLevel tag subtract the black level in camera. */
        st = rx_black_uniform(&s.black, 0.0f, err);
    } else {
        st = rx_warn(&s.warnings, err, "%s: black level not recorded in a documented tag; assumed 0",
                     pc_raw_format_name(format));
        if (st == PC_OK) {
            st = rx_black_uniform(&s.black, 0.0f, err);
        }
    }
    if (st != PC_OK) {
        goto fail;
    }
    if (rx_tag_floats(t, &raw, RX_WHITE_LEVEL, wl, 1) > 0 && wl[0] > 0.0) {
        white = pc_f64_to_f32(wl[0]);
    } else {
        white = rx_clip_level(plane.data, plane.width * plane.height, plane.bits);
    }
    if (nikon) {
        nv = rx_tag_floats(&note_t, &note, NIKON_WB_RB_LEVELS, v, 2);
        if (nv >= 2) {
            s.has_camera_wb = true;
            s.camera_wb[0] = v[0];
            s.camera_wb[1] = 1.0;
            s.camera_wb[2] = v[1];
        }
    } else {
        nv = rx_tag_floats(t, &raw, SONY_WB_RGGB_LEVELS, v, 4);
        if (nv == 4 && v[1] + v[2] > 0.0) {
            s.has_camera_wb = true;
            s.camera_wb[0] = 2.0 * v[0] / (v[1] + v[2]);
            s.camera_wb[1] = 1.0;
            s.camera_wb[2] = 2.0 * v[3] / (v[1] + v[2]);
        }
    }
    if (s.has_camera_wb) {
        int k;
        for (k = 0; k < 3; k++) {
            if (!(s.camera_wb[k] >= 0.25 && s.camera_wb[k] < 8.0)) {
                s.has_camera_wb = false;
            }
        }
    }
    /* DNG-style DefaultCropOrigin / DefaultCropSize (some vendors write them in the raw IFD). */
    s.crop = full;
    if (rx_tag_floats(t, &raw, RX_DEFAULT_CROP_ORIGIN, co, 2) == 2 &&
        rx_tag_floats(t, &raw, RX_DEFAULT_CROP_SIZE, cs, 2) == 2) {
        size_t f[4];
        double src[4] = {co[0], co[1], cs[0], cs[1]};
        int k;
        pc_raw_rect r;
        for (k = 0; k < 4; k++) {
            f[k] = isfinite(src[k]) && src[k] >= 0.0 ? pc_f64_to_size(round(src[k])) : 0;
        }
        r = pc_raw_rect_intersect(pc_raw_rect_make(f[0], f[1], f[2], f[3]), full);
        if (!pc_raw_rect_is_empty(r)) {
            s.crop = r;
        }
    }
    s.format = format;
    s.width = plane.width;
    s.height = plane.height;
    s.samples = 1;
    s.ndata = plane.width * plane.height;
    s.has_cfa = true;
    s.cfa = cfa;
    s.white[0] = s.white[1] = s.white[2] = white;
    s.active = full;
    s.orientation = rx_ifd_at(t, t->first_ifd, 0, &ifd0) ? rx_orientation(t, &ifd0) : 1;
    if ((st = rx_cameras_apply(&s, err)) != PC_OK) {
        goto fail;
    }
    *out = s;
    return PC_OK;
fail:
    pc_raw_sensor_fini(&s);
    return st;
}
