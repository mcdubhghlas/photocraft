/* Finding the camera-rendered JPEG preview embedded in a raw file (port of
 * crates/raw/src/preview.rs). */
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

typedef struct best {
    bool found;
    uint64_t area;
    pc_raw_preview p;
} best;

/* Keeps the largest (the last of equals, like max_by_key). */
static void offer(best *b, const uint8_t *jpeg, size_t len)
{
    uint32_t w;
    uint32_t h;
    if (!dct_jpeg_size(jpeg, len, &w, &h)) {
        return;
    }
    if (!b->found || (uint64_t)w * h >= b->area) {
        b->found = true;
        b->area = (uint64_t)w * h;
        b->p.jpeg = jpeg;
        b->p.len = len;
        b->p.width = w;
        b->p.height = h;
    }
}

static void candidate(best *b, const uint8_t *data, size_t n, size_t off, size_t len)
{
    if (off > SIZE_MAX - len || off + len > n) {
        return;
    }
    offer(b, data + off, len);
}

bool pc_raw_embedded_preview(const uint8_t *bytes, size_t n, pc_raw_preview *out)
{
    best b;
    rx_tiff t;
    memset(&b, 0, sizeof b);
    if (n >= 15 && memcmp(bytes, "FUJIFILMCCD-RAW", 15) == 0) {
        /* RAF header: big-endian JPEG offset and length at bytes 84 and 88. */
        if (n >= 92) {
            size_t off = (size_t)bytes[84] << 24 | (size_t)bytes[85] << 16 | (size_t)bytes[86] << 8 | bytes[87];
            size_t len = (size_t)bytes[88] << 24 | (size_t)bytes[89] << 16 | (size_t)bytes[90] << 8 | bytes[91];
            candidate(&b, bytes, n, off, len);
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
            candidate(&b, bytes, n, off, len);
        }
        nifd = rx_all_ifds(&t, ifds);
        for (i = 0; i < nifd; i++) {
            uint32_t o;
            uint32_t l;
            uint32_t c;
            rx_entry e;
            if (rx_tag_uint(&t, &ifds[i], RX_JPEG_INTERCHANGE_FORMAT, &o) &&
                rx_tag_uint(&t, &ifds[i], RX_JPEG_INTERCHANGE_FORMAT_LENGTH, &l)) {
                candidate(&b, bytes, n, o, l);
            }
            if (rx_tag_uint(&t, &ifds[i], RX_COMPRESSION, &c) && (c == 6 || c == 7)) {
                uint32_t so[1];
                uint32_t sl[1];
                if (rx_tag_uints(&t, &ifds[i], RX_STRIP_OFFSETS, so, 1) == 1 &&
                    rx_tag_uints(&t, &ifds[i], RX_STRIP_BYTE_COUNTS, sl, 1) == 1) {
                    candidate(&b, bytes, n, so[0], sl[0]);
                }
            }
            /* Panasonic RW2 JpgFromRaw (0x002E) holds a complete JPEG. */
            if (rx_ifd_get(&t, &ifds[i], 0x002E, &e) && e.typ == 7) {
                size_t rl;
                const uint8_t *r = rx_raw(&t, &e, &rl);
                if (r != NULL) {
                    offer(&b, r, rl);
                }
            }
        }
    }
    if (!b.found) {
        return false;
    }
    *out = b.p;
    return true;
}
