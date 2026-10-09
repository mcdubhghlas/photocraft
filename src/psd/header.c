#include "psd/header.h"

#include <string.h>

#include "base/bytes.h"
#include "psd/internal.h"

pc_psd_header pc_psd_header_make(pc_psd_version v, uint32_t width, uint32_t height, uint16_t channels, uint16_t depth,
                                 uint16_t color_mode)
{
    pc_psd_header h;
    memset(&h, 0, sizeof h);
    h.version = v;
    h.channels = channels;
    h.height = height;
    h.width = width;
    h.depth = depth;
    h.color_mode = color_mode;
    return h;
}

pc_status pc_psd_header_validate(const pc_psd_header *h, pc_error *err)
{
    if (h->channels == 0 || h->channels > PC_PSD_MAX_CHANNELS) {
        return psd_invalid(err, "channel count %u out of range 1..=56", (unsigned)h->channels);
    }
    if (h->width == 0 || h->height == 0) {
        return psd_invalid(err, "image dimensions %ux%u must be at least 1x1", (unsigned)h->width, (unsigned)h->height);
    }
    if (h->width > PC_PSD_MAX_DIMENSION || h->height > PC_PSD_MAX_DIMENSION) {
        return psd_limit(err, "image dimensions exceed 300000");
    }
    if (h->depth != 1 && h->depth != 8 && h->depth != 16 && h->depth != 32) {
        return psd_invalid(err, "unsupported bit depth %u", (unsigned)h->depth);
    }
    return PC_OK;
}

pc_status pc_psd_header_parse(const uint8_t *data, size_t len, pc_psd_header *out, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    uint8_t sig[4];
    uint16_t version;
    const uint8_t *res;
    pc_psd_header h;
    pc_status st;
    memset(&h, 0, sizeof h);
    CK(psd_arr4(&r, sig, err));
    if (memcmp(sig, "8BPS", 4) != 0) {
        return psd_bad_sig(err, "8BPS", sig);
    }
    CK(psd_u16(&r, &version, err));
    if (version != 1 && version != 2) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "unsupported file version %u", (unsigned)version);
    }
    h.version = (pc_psd_version)version;
    CK(psd_bytes(&r, 6, &res, err));
    memcpy(h.reserved, res, 6);
    CK(psd_u16(&r, &h.channels, err));
    CK(psd_u32(&r, &h.height, err));
    CK(psd_u32(&r, &h.width, err));
    CK(psd_u16(&r, &h.depth, err));
    CK(psd_u16(&r, &h.color_mode, err));
    CK(pc_psd_header_validate(&h, err));
    *out = h;
    return PC_OK;
fail:
    return st;
}

void pc_psd_header_write(const pc_psd_header *h, uint8_t out[26])
{
    memcpy(out, "8BPS", 4);
    pc_store_be16(out + 4, (uint16_t)h->version);
    memcpy(out + 6, h->reserved, 6);
    pc_store_be16(out + 12, h->channels);
    pc_store_be32(out + 14, h->height);
    pc_store_be32(out + 18, h->width);
    pc_store_be16(out + 22, h->depth);
    pc_store_be16(out + 24, h->color_mode);
}

bool pc_psd_header_eq(const pc_psd_header *a, const pc_psd_header *b)
{
    return a->version == b->version && memcmp(a->reserved, b->reserved, 6) == 0 && a->channels == b->channels &&
           a->height == b->height && a->width == b->width && a->depth == b->depth && a->color_mode == b->color_mode;
}

size_t pc_psd_row_bytes(size_t width, uint16_t depth)
{
    size_t per;
    if (depth == 1) {
        return width / 8 + (width % 8 != 0);
    }
    per = depth / 8 > 0 ? depth / 8 : 1;
    return width > SIZE_MAX / per ? SIZE_MAX : width * per;
}
