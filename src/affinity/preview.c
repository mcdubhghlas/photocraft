/* The indexed PNG thumbnail Affinity stores next to the archive (port of
 * crates/affinity/src/preview.rs): Affinity's own render of the document, never the document. */
#include <string.h>

#include "affinity/internal.h"
#include "base/compress.h"

static bool bytes_at(const uint8_t *b, size_t n, size_t off, size_t len, const uint8_t **out, pc_error *err,
                     pc_status *st)
{
    if (off > SIZE_MAX - len) {
        *st = ax_malformed(err, "offset overflow");
        return false;
    }
    if (off + len > n) {
        *st = ax_malformed(err, "truncated record");
        return false;
    }
    *out = b + off;
    return true;
}

static bool le32(const uint8_t *b, size_t n, size_t off, uint32_t *v, pc_error *err, pc_status *st)
{
    const uint8_t *p;
    if (!bytes_at(b, n, off, 4, &p, err, st)) {
        return false;
    }
    *v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return true;
}

static bool be32(const uint8_t *b, size_t n, size_t off, uint32_t *v, pc_error *err, pc_status *st)
{
    const uint8_t *p;
    if (!bytes_at(b, n, off, 4, &p, err, st)) {
        return false;
    }
    *v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
    return true;
}

static bool is_alpha(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

static bool kind_is(const uint8_t *k, const char *s) { return memcmp(k, s, 4) == 0; }

pc_status pc_af_preview_read(const uint8_t *bytes, size_t n, pc_af_preview *out, pc_error *err)
{
    pc_af_header h;
    const uint8_t *record;
    size_t rn;
    const uint8_t *p;
    const uint8_t *png;
    uint32_t v;
    uint32_t v16;
    uint32_t v20 = 0;
    uint32_t v12 = 0;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    size_t pos = 8;
    bool palette = false;
    bool image_data = false;
    bool image_data_ended = false;
    size_t offset;
    pc_status st = PC_OK;
    if ((st = pc_af_header_read(bytes, n, &h, err)) != PC_OK) {
        return st;
    }
    if (h.thumbnail_offset == 0) {
        return ax_unsupported(err, "no embedded preview");
    }
    if (h.thumbnail_offset > SIZE_MAX) {
        return ax_malformed(err, "thumbnail offset overflow");
    }
    offset = (size_t)h.thumbnail_offset;
    if (offset < 72) {
        return ax_malformed(err, "thumbnail overlaps the file header");
    }
    if (offset > n) {
        return ax_malformed(err, "thumbnail offset outside file");
    }
    record = bytes + offset;
    rn = n - offset;
    if (!bytes_at(record, rn, 0, 8, &p, err, &st)) {
        return st;
    }
    if (memcmp(p, "\xff\xff\xff\xffThmb", 8) != 0) {
        return ax_malformed(err, "missing indexed thumbnail");
    }
    if (!le32(record, rn, 8, &v, err, &st)) {
        return st;
    }
    if (v != 1) {
        return ax_unsupported(err, "thumbnail record version or encoding");
    }
    if (!bytes_at(record, rn, 28, 1, &p, err, &st)) {
        return st;
    }
    if (p[0] != 1) {
        return ax_unsupported(err, "thumbnail record version or encoding");
    }
    if (!le32(record, rn, 24, &size, err, &st)) {
        return st;
    }
    if (size > PC_AF_MAX_PREVIEW_BYTES) {
        return ax_limit(err, "encoded preview exceeds 16 MiB");
    }
    if (!le32(record, rn, 16, &v16, err, &st)) {
        return st;
    }
    if (v16 == 29) {
        if (!le32(record, rn, 20, &v20, err, &st)) {
            return st;
        }
        if (v20 == 0) {
            if (!le32(record, rn, 12, &v12, err, &st)) {
                return st;
            }
        }
    }
    if (v16 != 29 || v20 != 0 || (uint64_t)v12 != (uint64_t)size + 13) {
        return ax_malformed(err, "inconsistent thumbnail record lengths");
    }
    if (!bytes_at(record, rn, 29, size, &png, err, &st)) {
        return st;
    }
    if (!bytes_at(png, size, 0, 8, &p, err, &st)) {
        return st;
    }
    if (memcmp(p, "\x89PNG\r\n\x1a\n", 8) != 0) {
        return ax_malformed(err, "thumbnail is not a PNG");
    }
    if (!be32(png, size, 8, &v, err, &st)) {
        return st;
    }
    if (v != 13) {
        return ax_malformed(err, "thumbnail is not a PNG");
    }
    if (!bytes_at(png, size, 12, 4, &p, err, &st)) {
        return st;
    }
    if (memcmp(p, "IHDR", 4) != 0) {
        return ax_malformed(err, "thumbnail is not a PNG");
    }
    if (!be32(png, size, 16, &width, err, &st) || !be32(png, size, 20, &height, err, &st)) {
        return st;
    }
    if (width == 0 || height == 0 || width > PC_AF_MAX_PREVIEW_DIMENSION || height > PC_AF_MAX_PREVIEW_DIMENSION) {
        return ax_limit(err, "preview dimensions must be 1..4096");
    }
    /* Bound work by bytes, not a file-supplied chunk count; require exactly one complete PNG. */
    for (;;) {
        uint32_t length;
        size_t chunk_size;
        const uint8_t *chunk;
        const uint8_t *kind;
        uint32_t crc;
        int i;
        if (!be32(png, size, pos, &length, err, &st)) {
            return st;
        }
        chunk_size = (size_t)length + 12;
        if (!bytes_at(png, size, pos, chunk_size, &chunk, err, &st)) {
            return st;
        }
        if (!be32(chunk, chunk_size, chunk_size - 4, &crc, err, &st)) {
            return st;
        }
        if (pc_crc32(0, chunk + 4, chunk_size - 8) != crc) {
            return ax_malformed(err, "PNG chunk checksum");
        }
        kind = chunk + 4;
        for (i = 0; i < 4; i++) {
            if (!is_alpha(kind[i])) {
                return ax_malformed(err, "PNG chunk type");
            }
        }
        if (kind_is(kind, "IHDR")) {
            if (pos != 8) {
                return ax_malformed(err, "duplicate PNG header");
            }
        } else if (kind_is(kind, "PLTE")) {
            if (palette || image_data) {
                return ax_malformed(err, "PNG palette order");
            }
            palette = true;
        } else if (kind_is(kind, "IDAT")) {
            if (image_data_ended) {
                return ax_malformed(err, "non-contiguous PNG image data");
            }
            image_data = true;
        } else if (kind_is(kind, "IEND")) {
            if (!image_data) {
                return ax_malformed(err, "PNG has no image data");
            }
        } else if (kind[0] >= 'A' && kind[0] <= 'Z') {
            return ax_unsupported(err, "unknown critical PNG chunk");
        }
        if (image_data && !kind_is(kind, "IDAT")) {
            image_data_ended = true;
        }
        if (kind_is(kind, "zTXt") || kind_is(kind, "iTXt") || kind_is(kind, "iCCP")) {
            return ax_unsupported(err, "compressed PNG metadata in the preview");
        }
        if (kind_is(kind, "acTL") || kind_is(kind, "fcTL") || kind_is(kind, "fdAT")) {
            return ax_unsupported(err, "animated PNG preview");
        }
        if (pos > SIZE_MAX - chunk_size) {
            return ax_malformed(err, "PNG offset overflow");
        }
        pos += chunk_size;
        if (kind_is(kind, "IEND")) {
            if (length != 0 || pos != size) {
                return ax_malformed(err, "PNG end or trailing data");
            }
            break;
        }
    }
    out->png = png;
    out->len = size;
    out->width = width;
    out->height = height;
    return PC_OK;
}
