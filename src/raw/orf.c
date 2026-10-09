/* Olympus ORF with uncompressed sensor data (port of crates/raw/src/orf.rs): TIFF with an
 * IIRO / IIRS / MMOR header, IFD0's strips of 16-bit samples, the EXIF CFAPattern, and levels from
 * the documented maker-note ImageProcessing tags. Olympus' compressed data is not decoded. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "raw/internal.h"

#define CAMERA_SETTINGS 0x2020
#define PREVIEW_IMAGE_START 0x0101
#define PREVIEW_IMAGE_LENGTH 0x0102
#define IMAGE_PROCESSING 0x2040
#define WB_RB_LEVELS 0x0100
#define WB_G_LEVEL 0x011F
#define BLACK_LEVEL_2 0x0600
#define VALID_BITS 0x0611
static const uint16_t CROP[4] = {0x0612, 0x0613, 0x0614, 0x0615};

/* A sub-IFD of the Olympus maker note with the view its offsets are relative to and that view's
 * absolute position. New-style notes ("OLYMPUS\0" + byte order) are relative to the note,
 * old-style ones ("OLYMP\0") to the file. */
static bool maker_subifd(const rx_tiff *t, const rx_ifd *ifds, size_t n, uint16_t sub, rx_tiff *view, rx_ifd *out,
                         size_t *base)
{
    rx_entry e;
    rx_entry ip;
    bool found = false;
    const uint8_t *head;
    rx_ifd main_ifd;
    size_t i;
    size_t at;
    for (i = 0; i < n && !found; i++) {
        found = rx_ifd_get(t, &ifds[i], RX_MAKER_NOTE, &e);
    }
    if (!found || (head = rx_bytes(t, e.at, 12)) == NULL) {
        return false;
    }
    if (memcmp(head, "OLYMPUS\0", 8) == 0) {
        bool le;
        if (head[8] == 'I' && head[9] == 'I') {
            le = true;
        } else if (head[8] == 'M' && head[9] == 'M') {
            le = false;
        } else {
            return false;
        }
        view->data = t->data + e.at;
        view->len = t->len - e.at;
        view->le = le;
        view->first_ifd = 12;
        if (!rx_ifd_at(view, 12, 0, &main_ifd)) {
            return false;
        }
        *base = e.at;
    } else if (memcmp(head, "OLYMP\0", 6) == 0) {
        *view = *t;
        if (e.at > SIZE_MAX - 8 || !rx_ifd_at(t, e.at + 8, 0, &main_ifd)) {
            return false;
        }
        *base = 0;
    } else {
        return false;
    }
    if (!rx_ifd_get(view, &main_ifd, sub, &ip)) {
        return false;
    }
    if (ip.typ == 4 || ip.typ == 13) {
        uint32_t v;
        if (!rx_uint(view, &ip, 0, &v)) {
            return false;
        }
        at = v;
    } else {
        at = ip.at;
    }
    return rx_ifd_at(view, at, 0, out);
}

bool rx_orf_preview_range(const rx_tiff *t, size_t *off, size_t *len)
{
    rx_ifd ifds[RX_MAX_IFDS];
    size_t n = rx_all_ifds(t, ifds);
    rx_tiff view;
    rx_ifd cs;
    size_t base;
    uint32_t start;
    uint32_t l;
    if (!maker_subifd(t, ifds, n, CAMERA_SETTINGS, &view, &cs, &base) ||
        !rx_tag_uint(&view, &cs, PREVIEW_IMAGE_START, &start) || !rx_tag_uint(&view, &cs, PREVIEW_IMAGE_LENGTH, &l) ||
        base > SIZE_MAX - start) {
        return false;
    }
    *off = base + start;
    *len = l;
    return true;
}

pc_status rx_orf_decode(const rx_tiff *t, const pc_raw_limits *limits, pc_raw_sensor *out, pc_error *err)
{
    rx_ifd ifds[RX_MAX_IFDS];
    size_t nifd = rx_all_ifds(t, ifds);
    rx_ifd raw;
    uint32_t v;
    uint64_t width;
    uint64_t height;
    rx_entry ce;
    size_t ncounts;
    uint64_t stored = 0;
    size_t i;
    rx_plane plane;
    pc_raw_cfa cfa;
    pc_raw_rect full;
    pc_raw_sensor s;
    rx_tiff view;
    rx_ifd ip;
    size_t base;
    bool has_ip;
    double f[4];
    size_t nf;
    float rggb[4];
    float white;
    double g;
    bool nomem = false;
    pc_status st;
    uint32_t comp;
    if (!rx_ifd_at(t, t->first_ifd, 0, &raw)) {
        return rx_malformed(err, "ORF has no IFD0");
    }
    width = rx_tag_uint(t, &raw, RX_IMAGE_WIDTH, &v) ? v : 0;
    height = rx_tag_uint(t, &raw, RX_IMAGE_LENGTH, &v) ? v : 0;
    /* Missing or unreadable counts mean IFD0 is cut short, not the compressed variant. */
    ncounts = rx_ifd_get(t, &raw, RX_STRIP_BYTE_COUNTS, &ce) ? rx_uints_n(t, &ce) : 0;
    if (ncounts == 0) {
        return rx_malformed(err, "ORF IFD0 has no readable strip byte counts");
    }
    for (i = 0; i < ncounts; i++) {
        (void)rx_uint(t, &ce, i, &v);
        stored += v;
    }
    comp = rx_tag_uint(t, &raw, RX_COMPRESSION, &v) ? v : 1;
    {
        uint64_t need = width * height;
        need = height != 0 && need / height != width ? UINT64_MAX : need;
        need = need > UINT64_MAX / 2 ? UINT64_MAX : need * 2;
        if (comp != 1 || stored < need) {
            return rx_unsupported(err, "Olympus compressed (or packed) ORF is not decoded");
        }
    }
    if ((st = rx_read_plane(t, &raw, limits, RX_FLAT, &plane, err)) != PC_OK) {
        return st;
    }
    pc_raw_sensor_init(&s);
    s.data = plane.data;
    s.ndata = plane.width * plane.height * plane.samples;
    if (plane.samples != 1) {
        st = rx_unsupported(err, "ORF with %zu samples per pixel", plane.samples);
        goto fail;
    }
    if (!rx_tiffep_cfa(t, &raw, ifds, nifd, &cfa)) {
        st = rx_unsupported(err, "ORF: CFA pattern not recorded");
        goto fail;
    }
    if (!pc_raw_cfa_is_bayer(&cfa)) {
        st = rx_unsupported(err, "ORF: non-Bayer CFA");
        goto fail;
    }
    full = pc_raw_rect_make(0, 0, plane.width, plane.height);
    has_ip = maker_subifd(t, ifds, nifd, IMAGE_PROCESSING, &view, &ip, &base);
    nf = has_ip ? rx_tag_floats(&view, &ip, BLACK_LEVEL_2, f, 4) : 0;
    if (rx_rggb_by_position(&cfa, f, nf, rggb)) {
        st = rx_black_2x2(&s.black, rggb, err);
    } else {
        st = rx_warn(&s.warnings, err, "ORF: black level not recorded; assumed 0");
        if (st == PC_OK) {
            st = rx_black_uniform(&s.black, 0.0f, err);
        }
    }
    if (st != PC_OK) {
        goto fail;
    }
    nf = has_ip ? rx_tag_floats(&view, &ip, VALID_BITS, f, 1) : 0;
    if (nf > 0 && f[0] >= 8.0 && f[0] <= 16.0) {
        uint32_t bits = (uint32_t)f[0];
        uint32_t top = (1u << bits) - 1;
        bool over = false;
        for (i = 0; i < s.ndata && !over; i++) {
            over = s.data[i] > top;
        }
        /* Observed: the E-1 / E-400 store the valid bits left-justified in 16-bit containers. */
        if (over) {
            uint32_t shift = 16 - bits;
            for (i = 0; i < s.ndata; i++) {
                s.data[i] = (uint16_t)(s.data[i] >> shift);
            }
        }
        white = (float)top;
    } else {
        white = rx_clip_level(s.data, s.ndata, plane.bits);
    }
    nf = has_ip ? rx_tag_floats(&view, &ip, WB_G_LEVEL, f, 1) : 0;
    g = nf > 0 && f[0] > 0.0 ? f[0] : 256.0;
    nf = has_ip ? rx_tag_floats(&view, &ip, WB_RB_LEVELS, f, 2) : 0;
    if (nf >= 2) {
        double m[3] = {f[0] / g, 1.0, f[1] / g};
        bool ok = true;
        int k;
        for (k = 0; k < 3; k++) {
            ok = ok && m[k] >= 0.25 && m[k] < 8.0;
        }
        if (ok) {
            s.has_camera_wb = true;
            memcpy(s.camera_wb, m, sizeof m);
        }
    }
    s.crop = full;
    {
        double c[4];
        bool ok = has_ip;
        int k;
        for (k = 0; k < 4 && ok; k++) {
            ok = rx_tag_floats(&view, &ip, CROP[k], &c[k], 1) > 0 && c[k] >= 0.0 && c[k] < 1e6;
        }
        if (ok) {
            pc_raw_rect r = pc_raw_rect_intersect(
                pc_raw_rect_make(pc_f64_to_size(c[0]), pc_f64_to_size(c[1]), pc_f64_to_size(c[2]),
                                 pc_f64_to_size(c[3])),
                full);
            s.crop = pc_raw_rect_is_empty(r) ? full : r;
        }
    }
    s.format = PC_RAW_ORF;
    s.make = rx_find_ascii(t, ifds, nifd, RX_MAKE, &nomem);
    s.model = rx_find_ascii(t, ifds, nifd, RX_MODEL, &nomem);
    if (nomem) {
        st = rx_nomem(err);
        goto fail;
    }
    s.width = plane.width;
    s.height = plane.height;
    s.samples = 1;
    s.has_cfa = true;
    s.cfa = cfa;
    s.white[0] = s.white[1] = s.white[2] = white;
    s.active = full;
    s.orientation = rx_orientation(t, &raw);
    *out = s;
    return PC_OK;
fail:
    pc_raw_sensor_fini(&s);
    return st;
}
