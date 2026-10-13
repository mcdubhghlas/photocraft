/* Finding the camera-rendered JPEG preview embedded in a raw file (port of
 * crates/raw/src/preview.rs). */
#include <stdlib.h>
#include <string.h>

#include "raw/internal.h"

/* Dimensions of a DCT-coded JPEG (baseline / extended / progressive), or false (lossless JPEG raw
 * data, or not a JPEG). */
static bool dct_jpeg_size(const uint8_t *b, size_t n, uint32_t *w, uint32_t *h)
{
    size_t pos = 2;
    int it;
    if (n < 3 || b[0] != 0xFF || b[1] != 0xD8 || b[2] != 0xFF) {
        return false;
    }
    for (it = 0; it < 256; it++) {
        uint8_t m;
        size_t len;
        if (pos >= n || b[pos] != 0xFF || pos + 1 >= n) {
            return false;
        }
        m = b[pos + 1];
        if (m == 0xFF) {
            pos++;
            continue;
        }
        if (pos + 3 >= n) {
            return false;
        }
        len = (size_t)b[pos + 2] << 8 | b[pos + 3];
        if (m >= 0xC0 && m <= 0xC2) {
            uint16_t hh;
            uint16_t ww;
            if (pos + 8 >= n) {
                return false;
            }
            hh = (uint16_t)(b[pos + 5] << 8 | b[pos + 6]);
            ww = (uint16_t)(b[pos + 7] << 8 | b[pos + 8]);
            if (ww == 0 || hh == 0) {
                return false;
            }
            *w = ww;
            *h = hh;
            return true;
        }
        if (m == 0xC3 || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF) ||
            m == 0xDA || m == 0xD9 || len < 2) {
            return false;
        }
        if (pos > SIZE_MAX - 2 - len) {
            return false;
        }
        pos += 2 + len;
    }
    return false;
}

typedef struct found {
    pc_raw_preview v[PC_RAW_MAX_PREVIEWS];
    size_t n;
} found;

static void offer(found *f, const uint8_t *jpeg, size_t len)
{
    uint32_t w;
    uint32_t h;
    if (f->n >= PC_RAW_MAX_PREVIEWS || !dct_jpeg_size(jpeg, len, &w, &h)) {
        return;
    }
    f->v[f->n].jpeg = jpeg;
    f->v[f->n].len = len;
    f->v[f->n].width = w;
    f->v[f->n].height = h;
    f->n++;
}

static void candidate(found *f, const uint8_t *data, size_t n, size_t off, size_t len)
{
    if (off > SIZE_MAX - len || off + len > n) {
        return;
    }
    offer(f, data + off, len);
}

static void collect(const uint8_t *bytes, size_t n, found *f)
{
    rx_tiff t;
    f->n = 0;
    if (n >= 15 && memcmp(bytes, RX_RAF_MAGIC, 15) == 0) {
        size_t off;
        size_t len;
        if (rx_raf_preview_range(bytes, n, &off, &len)) {
            candidate(f, bytes, n, off, len);
        }
    } else if (rx_tiff_new(bytes, n, &t)) {
        rx_ifd ifds[RX_MAX_IFDS];
        size_t nifd;
        size_t i;
        size_t off;
        size_t len;
        /* Olympus ORF: the preview is referenced from the maker note. */
        if ((memcmp(bytes, "IIRO", 4) == 0 || memcmp(bytes, "IIRS", 4) == 0 || memcmp(bytes, "MMOR", 4) == 0) &&
            rx_orf_preview_range(&t, &off, &len)) {
            candidate(f, bytes, n, off, len);
        }
        nifd = rx_all_ifds(&t, ifds);
        for (i = 0; i < nifd; i++) {
            uint32_t o;
            uint32_t l;
            uint32_t c;
            rx_entry e;
            if (rx_tag_uint(&t, &ifds[i], RX_JPEG_INTERCHANGE_FORMAT, &o) &&
                rx_tag_uint(&t, &ifds[i], RX_JPEG_INTERCHANGE_FORMAT_LENGTH, &l)) {
                candidate(f, bytes, n, o, l);
            }
            if (rx_tag_uint(&t, &ifds[i], RX_COMPRESSION, &c) && (c == 6 || c == 7)) {
                uint32_t so[1];
                uint32_t sl[1];
                if (rx_tag_uints(&t, &ifds[i], RX_STRIP_OFFSETS, so, 1) == 1 &&
                    rx_tag_uints(&t, &ifds[i], RX_STRIP_BYTE_COUNTS, sl, 1) == 1) {
                    candidate(f, bytes, n, so[0], sl[0]);
                }
            }
            /* Panasonic RW2 JpgFromRaw (0x002E) holds a complete JPEG. */
            if (rx_ifd_get(&t, &ifds[i], 0x002E, &e) && e.typ == 7) {
                size_t rl;
                const uint8_t *r = rx_raw(&t, &e, &rl);
                if (r != NULL) {
                    offer(f, r, rl);
                }
            }
        }
    }
    /* Each stream once (cameras often store the same rendering twice). */
    {
        size_t i;
        size_t k = 0;
        for (i = 0; i < f->n; i++) {
            size_t j;
            bool dup = false;
            for (j = 0; j < k && !dup; j++) {
                dup = f->v[j].jpeg == f->v[i].jpeg && f->v[j].len == f->v[i].len;
            }
            if (!dup) {
                f->v[k++] = f->v[i];
            }
        }
        f->n = k;
    }
}

size_t pc_raw_embedded_previews(const uint8_t *bytes, size_t n, pc_raw_preview *out, size_t cap)
{
    found *f = malloc(sizeof *f);
    size_t k;
    if (f == NULL) {
        return 0;
    }
    collect(bytes, n, f);
    k = f->n;
    memcpy(out, f->v, (k < cap ? k : cap) * sizeof *out);
    free(f);
    return k;
}

bool pc_raw_embedded_preview(const uint8_t *bytes, size_t n, pc_raw_preview *out)
{
    found *f = malloc(sizeof *f);
    size_t i;
    size_t best = 0;
    uint64_t area = 0;
    if (f == NULL) {
        return false;
    }
    collect(bytes, n, f);
    /* The largest (the last of equals, like max_by_key). */
    for (i = 0; i < f->n; i++) {
        uint64_t a = (uint64_t)f->v[i].width * f->v[i].height;
        if (i == 0 || a >= area) {
            best = i;
            area = a;
        }
    }
    if (f->n == 0) {
        free(f);
        return false;
    }
    *out = f->v[best];
    free(f);
    return true;
}

bool pc_raw_embedded_preview_color_space(const uint8_t *bytes, size_t n, pc_raw_preview_color_space *out)
{
    rx_tiff t;
    rx_ifd ifd0;
    rx_ifd exif;
    rx_ifd interop;
    rx_entry e;
    uint32_t v;
    const uint8_t *r;
    size_t rl;
    if (!rx_tiff_new(bytes, n, &t) || !rx_ifd_at(&t, t.first_ifd, 0, &ifd0) ||
        rx_ifd_has(&t, &ifd0, RX_DNG_VERSION)) {
        return false;
    }
    if (!rx_tag_uint(&t, &ifd0, RX_EXIF_IFD, &v) || !rx_ifd_at(&t, v, 0, &exif) ||
        !rx_tag_uint(&t, &exif, RX_COLOR_SPACE, &v)) {
        return false;
    }
    if (v == 1) {
        *out = PC_RAW_PREVIEW_SRGB;
        return true;
    }
    if (v != 0xFFFF || !rx_tag_uint(&t, &exif, RX_INTEROP_IFD, &v) || !rx_ifd_at(&t, v, 0, &interop) ||
        !rx_ifd_get(&t, &interop, RX_INTEROP_INDEX, &e) || (r = rx_raw(&t, &e, &rl)) == NULL || rl < 3 ||
        memcmp(r, "R03", 3) != 0) {
        return false;
    }
    *out = PC_RAW_PREVIEW_ADOBE_RGB;
    return true;
}
