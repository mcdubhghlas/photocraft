#include "psd/resources.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "psd/internal.h"

pc_status pc_psd_resource_init(pc_psd_resource *r, uint16_t id, const void *data, size_t len, pc_error *err)
{
    memset(r, 0, sizeof *r);
    memcpy(r->signature, "8BIM", 4);
    r->id = id;
    return pc_psd_bytes_set(&r->data, data, len, err);
}

void pc_psd_resource_fini(pc_psd_resource *r)
{
    if (r != NULL) {
        pc_psd_bytes_fini(&r->name);
        pc_psd_bytes_fini(&r->data);
    }
}

pc_status pc_psd_resource_clone(const pc_psd_resource *r, pc_psd_resource *out, pc_error *err)
{
    pc_psd_resource c;
    pc_status st;
    memset(&c, 0, sizeof c);
    memcpy(c.signature, r->signature, 4);
    c.id = r->id;
    st = pc_psd_bytes_clone(&r->name, &c.name, err);
    if (st == PC_OK) {
        st = pc_psd_bytes_clone(&r->data, &c.data, err);
    }
    if (st != PC_OK) {
        pc_psd_resource_fini(&c);
        return st;
    }
    *out = c;
    return PC_OK;
}

bool pc_psd_resource_eq(const pc_psd_resource *a, const pc_psd_resource *b)
{
    return memcmp(a->signature, b->signature, 4) == 0 && a->id == b->id && pc_psd_bytes_eq(&a->name, &b->name) &&
           pc_psd_bytes_eq(&a->data, &b->data);
}

pc_psd_resolution pc_psd_resolution_from_dpi(double dpi)
{
    pc_psd_resolution r;
    double f = pc_clampd(round(dpi * 65536.0), 0.0, 4294967295.0);
    r.h_res_fixed = pc_f64_to_u32(f);
    r.h_res_unit = 1;
    r.width_unit = 1;
    r.v_res_fixed = r.h_res_fixed;
    r.v_res_unit = 1;
    r.height_unit = 1;
    return r;
}

double pc_psd_resolution_h(const pc_psd_resolution *r)
{
    return (double)r->h_res_fixed / 65536.0;
}

double pc_psd_resolution_v(const pc_psd_resolution *r)
{
    return (double)r->v_res_fixed / 65536.0;
}

pc_status pc_psd_resolution_parse(const uint8_t *b, size_t len, pc_psd_resolution *out, pc_error *err)
{
    psd_rd r = psd_rd_make(b, len);
    pc_psd_resolution v;
    pc_status st;
    CK(psd_u32(&r, &v.h_res_fixed, err));
    CK(psd_u16(&r, &v.h_res_unit, err));
    CK(psd_u16(&r, &v.width_unit, err));
    CK(psd_u32(&r, &v.v_res_fixed, err));
    CK(psd_u16(&r, &v.v_res_unit, err));
    CK(psd_u16(&r, &v.height_unit, err));
    *out = v;
    return PC_OK;
fail:
    return st;
}

void pc_psd_resolution_write(const pc_psd_resolution *r, uint8_t out[16])
{
    pc_store_be32(out, r->h_res_fixed);
    pc_store_be16(out + 4, r->h_res_unit);
    pc_store_be16(out + 6, r->width_unit);
    pc_store_be32(out + 8, r->v_res_fixed);
    pc_store_be16(out + 12, r->v_res_unit);
    pc_store_be16(out + 14, r->height_unit);
}

void pc_psd_version_info_fini(pc_psd_version_info *v)
{
    if (v != NULL) {
        pc_psd_ustr_fini(&v->writer);
        pc_psd_ustr_fini(&v->reader);
    }
}

pc_status pc_psd_version_info_parse(const uint8_t *b, size_t len, pc_psd_version_info *out, pc_error *err)
{
    psd_rd r = psd_rd_make(b, len);
    pc_psd_version_info v;
    uint8_t flag;
    pc_status st;
    memset(&v, 0, sizeof v);
    CK(psd_u32(&r, &v.version, err));
    CK(psd_u8(&r, &flag, err));
    v.has_real_merged_data = flag != 0;
    CK(psd_read_ustr(&r, &v.writer, err));
    CK(psd_read_ustr(&r, &v.reader, err));
    CK(psd_u32(&r, &v.file_version, err));
    *out = v;
    return PC_OK;
fail:
    pc_psd_version_info_fini(&v);
    return st;
}

pc_status pc_psd_version_info_to_bytes(const pc_psd_version_info *v, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    psd_w_init(&w, err);
    psd_put_u32(&w, v->version);
    psd_put_u8(&w, v->has_real_merged_data ? 1 : 0);
    psd_write_ustr(&w, &v->writer);
    psd_write_ustr(&w, &v->reader);
    psd_put_u32(&w, v->file_version);
    return psd_w_finish(&w, out);
}

pc_status pc_psd_version_info_resource(bool has_real_merged_data, pc_psd_resource *out, pc_error *err)
{
    pc_psd_version_info v;
    pc_psd_bytes data = {NULL, 0};
    pc_status st;
    memset(&v, 0, sizeof v);
    v.version = 1;
    v.has_real_merged_data = has_real_merged_data;
    v.file_version = 1;
    CK(pc_psd_ustr_from_utf8("Adobe Photoshop", true, &v.writer, err));
    CK(pc_psd_ustr_from_utf8("Adobe Photoshop CS6", true, &v.reader, err));
    CK(pc_psd_version_info_to_bytes(&v, &data, err));
    memset(out, 0, sizeof *out);
    memcpy(out->signature, "8BIM", 4);
    out->id = PC_PSD_RES_VERSION_INFO;
    out->data = data;
    pc_psd_version_info_fini(&v);
    return PC_OK;
fail:
    pc_psd_version_info_fini(&v);
    return st;
}

pc_status pc_psd_res_layer_state(const pc_psd_resource *r, uint16_t *out, pc_error *err)
{
    psd_rd rd = psd_rd_make(r->data.data, r->data.len);
    return psd_u16(&rd, out, err);
}

pc_status pc_psd_res_i32(const pc_psd_resource *r, int32_t *out, pc_error *err)
{
    psd_rd rd = psd_rd_make(r->data.data, r->data.len);
    return psd_i32(&rd, out, err);
}

pc_status pc_psd_res_group_ids(const pc_psd_resource *r, uint16_t **ids, size_t *n, pc_error *err)
{
    size_t k = r->data.len / 2;
    size_t i;
    uint16_t *v = NULL;
    if (r->data.len % 2 != 0) {
        return psd_invalid(err, "layer group info has odd length");
    }
    if (k > 0) {
        v = pc_malloc_array(k, sizeof *v);
        if (v == NULL) {
            return psd_nomem(err);
        }
        for (i = 0; i < k; i++) {
            v[i] = pc_load_be16(r->data.data + 2 * i);
        }
    }
    *ids = v;
    *n = k;
    return PC_OK;
}

bool pc_psd_res_is_known(uint16_t id)
{
    switch (id) {
    case PC_PSD_RES_RESOLUTION_INFO:
    case PC_PSD_RES_LAYER_STATE:
    case PC_PSD_RES_LAYER_GROUP_INFO:
    case PC_PSD_RES_THUMBNAIL:
    case PC_PSD_RES_THUMBNAIL_PS4:
    case PC_PSD_RES_GLOBAL_ANGLE:
    case PC_PSD_RES_ICC_PROFILE:
    case PC_PSD_RES_GLOBAL_ALTITUDE:
    case PC_PSD_RES_VERSION_INFO:
    case PC_PSD_RES_EXIF:
    case PC_PSD_RES_XMP:
        return true;
    default:
        return false;
    }
}

const char *pc_psd_res_xmp(const pc_psd_resource *r, size_t *len)
{
    size_t i = 0;
    uint32_t cp;
    if (r->id != PC_PSD_RES_XMP) {
        return NULL;
    }
    while (i < r->data.len) {
        if (!pc_utf8_next(r->data.data, r->data.len, &i, &cp)) {
            return NULL;
        }
    }
    *len = r->data.len;
    return r->data.len > 0 ? (const char *)r->data.data : "";
}

static pc_status read_resource(psd_rd *r, pc_psd_resource *out, pc_error *err)
{
    pc_psd_resource res;
    uint32_t len;
    pc_status st;
    static const char *const SIGS[5] = {"8BIM", "MeSa", "AgHg", "PHUT", "DCSR"};
    int k;
    memset(&res, 0, sizeof res);
    CK(psd_arr4(r, res.signature, err));
    for (k = 0; k < 5; k++) {
        if (memcmp(res.signature, SIGS[k], 4) == 0) {
            break;
        }
    }
    if (k == 5) {
        return psd_bad_sig(err, "8BIM resource", res.signature);
    }
    CK(psd_u16(r, &res.id, err));
    CK(psd_read_pascal(r, 2, &res.name, err));
    CK(psd_u32(r, &len, err));
    CK(psd_take_bytes(r, len, &res.data, err));
    if (len % 2 == 1) {
        CK(psd_skip(r, 1, err));
    }
    *out = res;
    return PC_OK;
fail:
    pc_psd_resource_fini(&res);
    return st;
}

void pc_psd_resources_free(pc_psd_resource *res, size_t n)
{
    size_t i;
    for (i = 0; res != NULL && i < n; i++) {
        pc_psd_resource_fini(&res[i]);
    }
    free(res);
}

pc_status psd_read_resources(psd_rd *r, pc_psd_resource **out, size_t *n, pc_error *err)
{
    uint32_t len;
    psd_rd s;
    pc_psd_resource *v = NULL;
    size_t count = 0;
    size_t cap = 0;
    pc_status st;
    CK(psd_u32(r, &len, err));
    CK(psd_sub(r, len, &s, err));
    while (!psd_empty(&s)) {
        pc_psd_resource res;
        if (count == cap) {
            pc_psd_resource *g = pc_grow_array(v, &cap, count + 1, sizeof *g);
            if (g == NULL) {
                st = psd_nomem(err);
                goto fail;
            }
            v = g;
        }
        CK(read_resource(&s, &res, err));
        v[count++] = res;
    }
    *out = v;
    *n = count;
    return PC_OK;
fail:
    pc_psd_resources_free(v, count);
    return st;
}

void psd_write_resources(psd_w *w, const pc_psd_resource *res, size_t n)
{
    size_t at = psd_begin_len(w, false);
    size_t i;
    for (i = 0; i < n; i++) {
        const pc_psd_resource *r = &res[i];
        psd_put(w, r->signature, 4);
        psd_put_u16(w, r->id);
        psd_write_pascal(w, r->name.data, r->name.len, 2);
        psd_put_len(w, r->data.len, false);
        psd_put(w, r->data.data, r->data.len);
        if (r->data.len % 2 == 1) {
            psd_put_u8(w, 0);
        }
    }
    psd_end_len(w, at, false);
}
