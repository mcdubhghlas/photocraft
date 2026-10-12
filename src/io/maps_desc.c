/* Descriptor helpers of chunk I4 (see maps_desc.h). */
#include "io/maps_desc.h"

#include "io/blocks.h"

#include <string.h>

#include "base/buf.h"
#include "base/utf8.h"
#include "io/core.h"

bool pc_io4_num(const pc_psd_value *v, double *out) { return pc_io_desc_num(v, out); }

bool pc_io4_num_of(const pc_psd_desc *d, const char *key, double *out)
{
    return d != NULL && pc_io4_num(pc_psd_desc_get(d, key), out);
}

const pc_psd_desc *pc_io4_obj(const pc_psd_desc *d, const char *key) { return pc_io_desc_get(d, key); }

const uint8_t *pc_io4_enum(const pc_psd_desc *d, const char *key, size_t *len)
{
    return pc_io_desc_enum(d, key, len);
}

bool pc_io4_enum_is(const pc_psd_desc *d, const char *key, const char *s)
{
    size_t len = 0;
    const uint8_t *e = pc_io4_enum(d, key, &len);
    size_t n = strlen(s);
    return e != NULL && len == n && (n == 0 || memcmp(e, s, n) == 0);
}

bool pc_io4_bool_of(const pc_psd_desc *d, const char *key) { return pc_io_desc_bool(d, key); }

pc_status pc_io4_vdesc_prefix(const uint8_t *data, size_t len, bool *found, pc_psd_desc *out, pc_error *err)
{
    return pc_io_parse_prefix_versioned(data, len, found, out, err);
}

pc_status pc_io4_ustr_lossy(const pc_psd_ustr *s, pc_str *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    size_t end = s->n;
    pc_status st = PC_OK;
    pc_str r;
    while (end > 0 && s->u[end - 1] == 0) {
        end--;
    }
    for (size_t i = 0; i < end && st == PC_OK; i++) {
        uint32_t u = s->u[i];
        uint32_t cp = u;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < end && s->u[i + 1] >= 0xDC00 && s->u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((u - 0xD800) << 10) + (uint32_t)(s->u[i + 1] - 0xDC00);
            i++;
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            cp = 0xFFFD;
        }
        st = pc_utf8_put(&b, cp, err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    {
        size_t n = 0;
        char *p = (char *)pc_buf_take(&b, &n);
        r = pc_str_adopt(p, n);
    }
    pc_str_fini(out);
    *out = r;
    return PC_OK;
}

pc_psd_value pc_io4_unit(const char unit[4], double v)
{
    pc_psd_value r;
    memset(&r, 0, sizeof r);
    r.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(r.unit, unit, 4);
    r.d = v;
    return r;
}

pc_psd_value pc_io4_list(void)
{
    pc_psd_value r;
    memset(&r, 0, sizeof r);
    r.kind = PC_PSD_V_LIST;
    return r;
}

void pc_io4_put(pc_psd_desc *d, const char *key, pc_psd_value v, pc_status *st, pc_error *err)
{
    if (*st != PC_OK) {
        pc_psd_value_fini(&v);
        return;
    }
    *st = pc_psd_desc_add(d, key, &v, err);
}

void pc_io4_put_text(pc_psd_desc *d, const char *key, const char *s, size_t len, bool nul, pc_status *st,
                     pc_error *err)
{
    pc_psd_value v;
    if (*st != PC_OK) {
        return;
    }
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_TEXT;
    *st = pc_psd_ustr_from_utf8n(s != NULL ? s : "", len, nul, &v.text, err);
    if (*st == PC_OK) {
        *st = pc_psd_desc_add(d, key, &v, err);
    }
}

void pc_io4_put_enum(pc_psd_desc *d, const char *key, const char *type_id, const char *value, pc_status *st,
                     pc_error *err)
{
    pc_psd_value v;
    if (*st != PC_OK) {
        return;
    }
    *st = pc_psd_value_enum(type_id, value, &v, err);
    if (*st == PC_OK) {
        *st = pc_psd_desc_add(d, key, &v, err);
    }
}

void pc_io4_put_obj(pc_psd_desc *d, const char *key, pc_psd_desc *child, pc_status *st, pc_error *err)
{
    pc_psd_value v;
    if (*st != PC_OK) {
        pc_psd_desc_fini(child);
        memset(child, 0, sizeof *child);
        return;
    }
    *st = pc_psd_value_object(child, &v, err);
    if (*st == PC_OK) {
        *st = pc_psd_desc_add(d, key, &v, err);
    }
}

void pc_io4_desc(pc_psd_desc *d, const char *class_id, pc_status *st, pc_error *err)
{
    memset(d, 0, sizeof *d);
    if (*st == PC_OK) {
        *st = pc_psd_desc_init(d, class_id, err);
    }
}

void pc_io4_list_push(pc_psd_value *list, pc_psd_value v, pc_status *st, pc_error *err)
{
    if (*st != PC_OK) {
        pc_psd_value_fini(&v);
        return;
    }
    *st = pc_psd_value_list_push(list, &v, err);
}
