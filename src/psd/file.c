#include "psd/file.h"

#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/buf.h"
#include "base/file.h"
#include "base/mem.h"
#include "psd/internal.h"

void pc_psd_file_init(pc_psd_file *f)
{
    memset(f, 0, sizeof *f);
    f->header.version = PC_PSD_V1;
}

void pc_psd_file_fini(pc_psd_file *f)
{
    if (f == NULL) {
        return;
    }
    pc_psd_bytes_fini(&f->color_mode_data);
    pc_psd_resources_free(f->resources, f->nresources);
    pc_psd_layer_info_fini(&f->layer_info);
    pc_psd_opt_bytes_fini(&f->placement.padding);
    pc_psd_bytes_fini(&f->global_mask);
    pc_psd_blocks_free(f->global_blocks, f->nglobal);
    pc_psd_bytes_fini(&f->layer_mask_trailing);
    pc_psd_bytes_fini(&f->image_data);
    pc_psd_file_init(f);
}

pc_status pc_psd_file_clone(const pc_psd_file *a, pc_psd_file *out, pc_error *err)
{
    pc_psd_file c;
    size_t i;
    pc_status st;
    pc_psd_file_init(&c);
    c.header = a->header;
    c.has_layer_info = a->has_layer_info;
    c.placement.where = a->placement.where;
    c.placement.index = a->placement.index;
    memcpy(c.placement.signature, a->placement.signature, 4);
    memcpy(c.placement.key, a->placement.key, 4);
    c.has_global_mask = a->has_global_mask;
    c.image_compression = a->image_compression;
    CK(pc_psd_bytes_clone(&a->color_mode_data, &c.color_mode_data, err));
    if (a->nresources > 0) {
        c.resources = pc_calloc_array(a->nresources, sizeof *c.resources);
        if (c.resources == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
        for (i = 0; i < a->nresources; i++) {
            CK(pc_psd_resource_clone(&a->resources[i], &c.resources[i], err));
            c.nresources++;
        }
    }
    if (a->has_layer_info) {
        CK(pc_psd_layer_info_clone(&a->layer_info, &c.layer_info, err));
    }
    CK(pc_psd_opt_bytes_clone(&a->placement.padding, &c.placement.padding, err));
    CK(pc_psd_bytes_clone(&a->global_mask, &c.global_mask, err));
    CK(pc_psd_blocks_clone(a->global_blocks, a->nglobal, &c.global_blocks, err));
    c.nglobal = a->nglobal;
    CK(pc_psd_bytes_clone(&a->layer_mask_trailing, &c.layer_mask_trailing, err));
    CK(pc_psd_bytes_clone(&a->image_data, &c.image_data, err));
    *out = c;
    return PC_OK;
fail:
    pc_psd_file_fini(&c);
    return st;
}

static bool placement_eq(const pc_psd_li_placement *a, const pc_psd_li_placement *b)
{
    if (a->where != b->where) {
        return false;
    }
    return a->where == PC_PSD_LI_SECTION ||
           (a->index == b->index && memcmp(a->signature, b->signature, 4) == 0 && memcmp(a->key, b->key, 4) == 0 &&
            pc_psd_opt_bytes_eq(&a->padding, &b->padding));
}

bool pc_psd_file_eq(const pc_psd_file *a, const pc_psd_file *b)
{
    size_t i;
    if (!pc_psd_header_eq(&a->header, &b->header) || !pc_psd_bytes_eq(&a->color_mode_data, &b->color_mode_data) ||
        a->nresources != b->nresources || a->has_layer_info != b->has_layer_info ||
        (a->has_layer_info && !pc_psd_layer_info_eq(&a->layer_info, &b->layer_info)) ||
        !placement_eq(&a->placement, &b->placement) || a->has_global_mask != b->has_global_mask ||
        (a->has_global_mask && !pc_psd_bytes_eq(&a->global_mask, &b->global_mask)) ||
        !pc_psd_blocks_eq(a->global_blocks, a->nglobal, b->global_blocks, b->nglobal) ||
        !pc_psd_bytes_eq(&a->layer_mask_trailing, &b->layer_mask_trailing) ||
        a->image_compression != b->image_compression || !pc_psd_bytes_eq(&a->image_data, &b->image_data)) {
        return false;
    }
    for (i = 0; i < a->nresources; i++) {
        if (!pc_psd_resource_eq(&a->resources[i], &b->resources[i])) {
            return false;
        }
    }
    return true;
}

pc_psd_layout pc_psd_image_layout(const pc_psd_header *h)
{
    pc_psd_layout l;
    l.planes = h->channels;
    l.width = h->width;
    l.height = h->height;
    l.depth = h->depth;
    l.version = h->version;
    return l;
}

pc_status pc_psd_image_encode(uint16_t compression, const uint8_t *decoded, size_t len, const pc_psd_header *h,
                              pc_psd_bytes *out, pc_error *err)
{
    pc_psd_layout l = pc_psd_image_layout(h);
    return pc_psd_encode_planes(compression, decoded, len, &l, out, err);
}

pc_status pc_psd_file_decode_merged(const pc_psd_file *f, pc_psd_bytes *out, pc_error *err)
{
    pc_psd_layout l = pc_psd_image_layout(&f->header);
    return pc_psd_decode_planes(f->image_compression, f->image_data.data, f->image_data.len, &l, out, err);
}

pc_status pc_psd_file_decode_merged_channel(const pc_psd_file *f, size_t index, pc_psd_bytes *out, pc_error *err)
{
    pc_psd_bytes all = {NULL, 0};
    size_t plane = pc_psd_header_row_bytes(&f->header) * (size_t)f->header.height;
    pc_status st = pc_psd_file_decode_merged(f, &all, err);
    if (st != PC_OK) {
        return st;
    }
    if (index >= f->header.channels || (index + 1) * plane > all.len) {
        pc_psd_bytes_fini(&all);
        return psd_invalid(err, "no merged channel %zu", index);
    }
    st = pc_psd_bytes_set(out, all.data + index * plane, plane, err);
    pc_psd_bytes_fini(&all);
    return st;
}

pc_status pc_psd_file_parse(const uint8_t *data, size_t len, pc_psd_file *out, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pc_psd_file f;
    bool psb;
    uint32_t cmd_len;
    uint64_t lm_len;
    psd_rd s;
    pc_psd_layout layout;
    pc_status st;
    pc_psd_file_init(&f);
    CK(pc_psd_header_parse(data, len, &f.header, err));
    r.pos = 26;
    psb = f.header.version == PC_PSD_V2;
    CK(psd_u32(&r, &cmd_len, err));
    CK(psd_take_bytes(&r, cmd_len, &f.color_mode_data, err));
    CK(psd_read_resources(&r, &f.resources, &f.nresources, err));
    CK(psd_len_field(&r, psb, &lm_len, err));
    CK(psd_sub(&r, lm_len, &s, err));
    if (!psd_empty(&s)) {
        uint64_t li_len;
        CK(psd_len_field(&s, psb, &li_len, err));
        if (li_len > 0) {
            const uint8_t *body;
            size_t blen;
            CK(psd_bytes_u64(&s, li_len, &body, &blen, err));
            CK(pc_psd_layer_info_parse(body, blen, f.header.version, &f.layer_info, err));
            f.has_layer_info = true;
        }
        if (psd_remaining(&s) >= 4) {
            uint32_t n;
            CK(psd_u32(&s, &n, err));
            CK(psd_take_bytes(&s, n, &f.global_mask, err));
            f.has_global_mask = true;
            CK(psd_read_blocks(&s, f.header.version, &f.global_blocks, &f.nglobal, &f.layer_mask_trailing, err));
        } else {
            CK(pc_psd_bytes_set(&f.layer_mask_trailing, psd_rest(&s), psd_remaining(&s), err));
        }
    }
    /* 16/32-bit files keep their layers in a global Lr16/Lr32 block. */
    if (!f.has_layer_info) {
        size_t i;
        for (i = 0; i < f.nglobal; i++) {
            pc_psd_block *b = &f.global_blocks[i];
            if (pc_psd_block_is(b, "Lr16") || pc_psd_block_is(b, "Lr32") || pc_psd_block_is(b, "Layr")) {
                break;
            }
        }
        if (i < f.nglobal) {
            pc_psd_layer_info li;
            pc_psd_block *b = &f.global_blocks[i];
            st = pc_psd_layer_info_parse(b->data.data, b->data.len, f.header.version, &li, NULL);
            if (st == PC_ERR_NOMEM) {
                st = psd_nomem(err);
                goto fail;
            }
            if (st == PC_OK) {
                f.layer_info = li;
                f.has_layer_info = true;
                f.placement.where = PC_PSD_LI_GLOBAL_BLOCK;
                f.placement.index = i;
                memcpy(f.placement.signature, b->signature, 4);
                memcpy(f.placement.key, b->key, 4);
                f.placement.padding = b->padding;
                b->padding.has = false;
                b->padding.v.data = NULL;
                b->padding.v.len = 0;
                pc_psd_block_fini(b);
                memmove(&f.global_blocks[i], &f.global_blocks[i + 1], (f.nglobal - i - 1) * sizeof *b);
                f.nglobal--;
            }
        }
    }
    CK(psd_u16(&r, &f.image_compression, err));
    layout = pc_psd_image_layout(&f.header);
    CK(pc_psd_validate_planes(f.image_compression, psd_rest(&r), psd_remaining(&r), &layout, err));
    CK(pc_psd_bytes_set(&f.image_data, psd_rest(&r), psd_remaining(&r), err));
    *out = f;
    return PC_OK;
fail:
    pc_psd_file_fini(&f);
    return st;
}

pc_status pc_psd_file_to_bytes(const pc_psd_file *f, pc_psd_bytes *out, pc_error *err)
{
    pc_psd_version v = f->header.version;
    bool psb = v == PC_PSD_V2;
    bool in_block = f->placement.where == PC_PSD_LI_GLOBAL_BLOCK;
    bool section_empty = !f->has_layer_info && !f->has_global_mask && f->nglobal == 0 && f->layer_mask_trailing.len == 0;
    uint8_t hdr[26];
    psd_w w;
    size_t at;
    psd_w_init(&w, err);
    pc_psd_header_write(&f->header, hdr);
    psd_put(&w, hdr, 26);
    psd_put_len(&w, f->color_mode_data.len, false);
    psd_put(&w, f->color_mode_data.data, f->color_mode_data.len);
    psd_write_resources(&w, f->resources, f->nresources);
    /* An entirely empty section is written as a zero length (a section with only a zero
     * layer-info length is normalised to this form). */
    at = psd_begin_len(&w, psb);
    if (!section_empty) {
        size_t li = psd_begin_len(&w, psb);
        bool need_mask_field;
        if (f->has_layer_info && !in_block) {
            psd_write_layer_info(&w, &f->layer_info, v);
        }
        psd_end_len(&w, li, psb);
        need_mask_field = f->has_global_mask || f->nglobal > 0 || (in_block && f->has_layer_info);
        if (need_mask_field) {
            size_t glen = f->has_global_mask ? f->global_mask.len : 0;
            psd_put_len(&w, glen, false);
            psd_put(&w, f->global_mask.data, glen);
        }
        if (f->has_layer_info && in_block) {
            size_t idx = f->placement.index < f->nglobal ? f->placement.index : f->nglobal;
            bool wide = pc_psd_uses_long_length(v, f->placement.key);
            size_t bat;
            size_t start;
            size_t blen;
            psd_write_blocks(&w, f->global_blocks, idx, v);
            psd_put(&w, f->placement.signature, 4);
            psd_put(&w, f->placement.key, 4);
            bat = psd_begin_len(&w, wide);
            start = w.b.len;
            psd_write_layer_info(&w, &f->layer_info, v);
            blen = w.b.len - start;
            psd_end_len(&w, bat, wide);
            if (f->placement.padding.has) {
                psd_put(&w, f->placement.padding.v.data, f->placement.padding.v.len);
            } else {
                psd_put_zeros(&w, blen % 2);
            }
            psd_write_blocks(&w, f->global_blocks + idx, f->nglobal - idx, v);
        } else {
            psd_write_blocks(&w, f->global_blocks, f->nglobal, v);
        }
        psd_put(&w, f->layer_mask_trailing.data, f->layer_mask_trailing.len);
    }
    psd_end_len(&w, at, psb);
    psd_put_u16(&w, f->image_compression);
    psd_put(&w, f->image_data.data, f->image_data.len);
    return psd_w_finish(&w, out);
}

pc_status pc_psd_file_open(const char *path, pc_psd_file *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status st = pc_file_read(path, PC_MAX_ALLOC, &b, err);
    if (st == PC_OK) {
        st = pc_psd_file_parse(b.data, b.len, out, err);
    }
    pc_buf_fini(&b);
    return st;
}

pc_status pc_psd_file_save(const pc_psd_file *f, const char *path, pc_error *err)
{
    pc_psd_bytes b = {NULL, 0};
    pc_status st = pc_psd_file_to_bytes(f, &b, err);
    if (st == PC_OK) {
        st = pc_file_write_atomic(path, b.data, b.len, err);
    }
    pc_psd_bytes_fini(&b);
    return st;
}

pc_status pc_psd_file_validate(const pc_psd_file *f, pc_error *err)
{
    pc_status st = pc_psd_header_validate(&f->header, err);
    if (st != PC_OK) {
        return st;
    }
    if (f->header.version == PC_PSD_V1 && (f->header.width > 30000 || f->header.height > 30000)) {
        return psd_limit(err, "PSD dimensions exceed 30000; use PSB");
    }
    if (f->header.color_mode == PC_PSD_MODE_INDEXED && f->color_mode_data.len != 768) {
        return psd_invalid(err, "indexed color mode requires a 768-byte palette");
    }
    return PC_OK;
}

const pc_psd_layer *pc_psd_file_layers(const pc_psd_file *f, size_t *n)
{
    if (!f->has_layer_info) {
        *n = 0;
        return NULL;
    }
    *n = f->layer_info.n;
    return f->layer_info.layers;
}

const pc_psd_resource *pc_psd_file_resource(const pc_psd_file *f, uint16_t id)
{
    size_t i;
    for (i = 0; i < f->nresources; i++) {
        if (f->resources[i].id == id) {
            return &f->resources[i];
        }
    }
    return NULL;
}

const pc_psd_block *pc_psd_file_global_block(const pc_psd_file *f, const char key[4])
{
    size_t i;
    for (i = 0; i < f->nglobal; i++) {
        if (pc_psd_block_is(&f->global_blocks[i], key)) {
            return &f->global_blocks[i];
        }
    }
    return NULL;
}

const uint8_t *pc_psd_file_icc(const pc_psd_file *f, size_t *len)
{
    const pc_psd_resource *r = pc_psd_file_resource(f, PC_PSD_RES_ICC_PROFILE);
    if (r == NULL) {
        return NULL;
    }
    *len = r->data.len;
    return r->data.data != NULL ? r->data.data : (const uint8_t *)"";
}

bool pc_psd_file_resolution(const pc_psd_file *f, pc_psd_resolution *out)
{
    const pc_psd_resource *r = pc_psd_file_resource(f, PC_PSD_RES_RESOLUTION_INFO);
    return r != NULL && pc_psd_resolution_parse(r->data.data, r->data.len, out, NULL) == PC_OK;
}

bool pc_psd_file_has_real_merged_data(const pc_psd_file *f, bool *out)
{
    const pc_psd_resource *r = pc_psd_file_resource(f, PC_PSD_RES_VERSION_INFO);
    pc_psd_version_info v;
    if (r == NULL || pc_psd_version_info_parse(r->data.data, r->data.len, &v, NULL) != PC_OK) {
        return false;
    }
    *out = v.has_real_merged_data;
    pc_psd_version_info_fini(&v);
    return true;
}

bool pc_psd_file_merged_has_alpha(const pc_psd_file *f)
{
    uint16_t color = pc_psd_mode_color_channels(f->header.color_mode);
    if (color == 0 || f->header.channels <= color) {
        return false;
    }
    return f->has_layer_info ? f->layer_info.merged_alpha : true;
}

bool pc_psd_global_mask_be16(const pc_psd_file *f, size_t at, uint16_t *out)
{
    if (!f->has_global_mask || at + 2 > f->global_mask.len) {
        return false;
    }
    *out = pc_load_be16(f->global_mask.data + at);
    return true;
}

bool pc_psd_global_mask_kind(const pc_psd_file *f, uint8_t *out)
{
    if (!f->has_global_mask || f->global_mask.len <= 12) {
        return false;
    }
    *out = f->global_mask.data[12];
    return true;
}
