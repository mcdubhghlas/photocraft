#include "psd/descriptor.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/utf8.h"
#include "psd/internal.h"

/* ---- strings and ids ---- */

void pc_psd_ustr_fini(pc_psd_ustr *s)
{
    if (s != NULL) {
        free(s->u);
        s->u = NULL;
        s->n = 0;
    }
}

pc_status pc_psd_ustr_from_utf8(const char *s, bool nul, pc_psd_ustr *out, pc_error *err)
{
    return pc_psd_ustr_from_utf8n(s, strlen(s), nul, out, err);
}

pc_status pc_psd_ustr_from_utf8n(const char *s, size_t len, bool nul, pc_psd_ustr *out, pc_error *err)
{
    uint16_t *u = NULL;
    size_t n = 0;
    pc_status st = psd_utf8n_to_units(s, len, &u, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (nul) {
        uint16_t *g = realloc(u, (n + 1) * sizeof *u);
        if (g == NULL) {
            free(u);
            return psd_nomem(err);
        }
        u = g;
        u[n++] = 0;
    }
    out->u = u;
    out->n = n;
    return PC_OK;
}

pc_status pc_psd_ustr_to_utf8(const pc_psd_ustr *s, char **out, pc_error *err)
{
    size_t end = s->n;
    while (end > 0 && s->u[end - 1] == 0) {
        end--;
    }
    return psd_units_to_utf8(s->u, end, out, err);
}

bool pc_psd_ustr_eq(const pc_psd_ustr *a, const pc_psd_ustr *b)
{
    return a->n == b->n && (a->n == 0 || memcmp(a->u, b->u, a->n * sizeof *a->u) == 0);
}

pc_status pc_psd_ustr_clone(const pc_psd_ustr *s, pc_psd_ustr *out, pc_error *err)
{
    uint16_t *u = NULL;
    if (s->n > 0) {
        u = pc_malloc_array(s->n, sizeof *u);
        if (u == NULL) {
            return psd_nomem(err);
        }
        memcpy(u, s->u, s->n * sizeof *u);
    }
    out->u = u;
    out->n = s->n;
    return PC_OK;
}

pc_status psd_read_ustr(psd_rd *r, pc_psd_ustr *out, pc_error *err)
{
    return psd_read_units(r, &out->u, &out->n, err);
}

void psd_write_ustr(psd_w *w, const pc_psd_ustr *s)
{
    psd_write_units(w, s->u, s->n);
}

pc_status pc_psd_id_make(const char *s, pc_psd_id *out, pc_error *err)
{
    size_t n = strlen(s);
    memset(out, 0, sizeof *out);
    if (n == 4) {
        out->is_code = true;
        memcpy(out->code, s, 4);
        return PC_OK;
    }
    return pc_psd_bytes_set(&out->str, s, n, err);
}

void pc_psd_id_fini(pc_psd_id *id)
{
    if (id != NULL) {
        pc_psd_bytes_fini(&id->str);
    }
}

const uint8_t *pc_psd_id_bytes(const pc_psd_id *id, size_t *len)
{
    if (id->is_code) {
        *len = 4;
        return id->code;
    }
    *len = id->str.len;
    return id->str.data;
}

bool pc_psd_id_is(const pc_psd_id *id, const char *s)
{
    size_t n;
    const uint8_t *b = pc_psd_id_bytes(id, &n);
    return strlen(s) == n && (n == 0 || memcmp(b, s, n) == 0);
}

bool pc_psd_id_eq(const pc_psd_id *a, const pc_psd_id *b)
{
    if (a->is_code != b->is_code) {
        return false;
    }
    return a->is_code ? memcmp(a->code, b->code, 4) == 0 : pc_psd_bytes_eq(&a->str, &b->str);
}

static pc_status id_clone(const pc_psd_id *a, pc_psd_id *out, pc_error *err)
{
    pc_psd_id c = *a;
    c.str.data = NULL;
    c.str.len = 0;
    if (!a->is_code) {
        pc_status st = pc_psd_bytes_clone(&a->str, &c.str, err);
        if (st != PC_OK) {
            return st;
        }
    }
    *out = c;
    return PC_OK;
}

static pc_status read_id(psd_rd *r, pc_psd_id *out, pc_error *err)
{
    uint32_t n;
    pc_status st = psd_u32(r, &n, err);
    memset(out, 0, sizeof *out);
    if (st != PC_OK) {
        return st;
    }
    if (n == 0) {
        out->is_code = true;
        return psd_arr4(r, out->code, err);
    }
    return psd_take_bytes(r, n, &out->str, err);
}

static void write_id(psd_w *w, const pc_psd_id *id)
{
    if (id->is_code) {
        psd_put_u32(w, 0);
        psd_put(w, id->code, 4);
    } else {
        psd_put_u32(w, (uint32_t)id->str.len);
        psd_put(w, id->str.data, id->str.len);
    }
}

static void class_fini(pc_psd_class *c)
{
    pc_psd_ustr_fini(&c->name);
    pc_psd_id_fini(&c->class_id);
}

static pc_status read_class(psd_rd *r, pc_psd_class *c, pc_error *err)
{
    pc_status st;
    memset(c, 0, sizeof *c);
    st = psd_read_ustr(r, &c->name, err);
    if (st == PC_OK) {
        st = read_id(r, &c->class_id, err);
    }
    if (st != PC_OK) {
        class_fini(c);
    }
    return st;
}

static void write_class(psd_w *w, const pc_psd_class *c)
{
    psd_write_ustr(w, &c->name);
    write_id(w, &c->class_id);
}

static bool class_eq(const pc_psd_class *a, const pc_psd_class *b)
{
    return pc_psd_ustr_eq(&a->name, &b->name) && pc_psd_id_eq(&a->class_id, &b->class_id);
}

static pc_status class_clone(const pc_psd_class *a, pc_psd_class *out, pc_error *err)
{
    pc_status st;
    memset(out, 0, sizeof *out);
    st = pc_psd_ustr_clone(&a->name, &out->name, err);
    if (st == PC_OK) {
        st = id_clone(&a->class_id, &out->class_id, err);
    }
    if (st != PC_OK) {
        class_fini(out);
    }
    return st;
}

/* ---- reference items ---- */

static void ref_fini(pc_psd_ref_item *it)
{
    class_fini(&it->cls);
    pc_psd_id_fini(&it->key);
    pc_psd_id_fini(&it->value);
    pc_psd_ustr_fini(&it->name);
}

static pc_status read_ref(psd_rd *r, pc_psd_ref_item *it, pc_error *err)
{
    uint8_t ty[4];
    pc_status st;
    memset(it, 0, sizeof *it);
    CK(psd_arr4(r, ty, err));
    if (memcmp(ty, "prop", 4) == 0) {
        it->kind = PC_PSD_REF_PROPERTY;
        CK(read_class(r, &it->cls, err));
        CK(read_id(r, &it->key, err));
    } else if (memcmp(ty, "Clss", 4) == 0) {
        it->kind = PC_PSD_REF_CLASS;
        CK(read_class(r, &it->cls, err));
    } else if (memcmp(ty, "Enmr", 4) == 0) {
        it->kind = PC_PSD_REF_ENUMERATED;
        CK(read_class(r, &it->cls, err));
        CK(read_id(r, &it->key, err));
        CK(read_id(r, &it->value, err));
    } else if (memcmp(ty, "rele", 4) == 0) {
        it->kind = PC_PSD_REF_OFFSET;
        CK(read_class(r, &it->cls, err));
        CK(psd_i32(r, &it->offset, err));
    } else if (memcmp(ty, "Idnt", 4) == 0) {
        it->kind = PC_PSD_REF_IDENTIFIER;
        CK(psd_u32(r, &it->number, err));
    } else if (memcmp(ty, "indx", 4) == 0) {
        it->kind = PC_PSD_REF_INDEX;
        CK(psd_u32(r, &it->number, err));
    } else if (memcmp(ty, "name", 4) == 0) {
        it->kind = PC_PSD_REF_NAME;
        CK(read_class(r, &it->cls, err));
        CK(psd_read_ustr(r, &it->name, err));
    } else {
        char q[32];
        pc_utf8_debug_quote(ty, 4, q, sizeof q);
        st = psd_unsupported(err, "reference item type %s", q);
        goto fail;
    }
    return PC_OK;
fail:
    ref_fini(it);
    return st;
}

static void write_ref(psd_w *w, const pc_psd_ref_item *it)
{
    switch (it->kind) {
    case PC_PSD_REF_PROPERTY:
        psd_put(w, "prop", 4);
        write_class(w, &it->cls);
        write_id(w, &it->key);
        break;
    case PC_PSD_REF_CLASS:
        psd_put(w, "Clss", 4);
        write_class(w, &it->cls);
        break;
    case PC_PSD_REF_ENUMERATED:
        psd_put(w, "Enmr", 4);
        write_class(w, &it->cls);
        write_id(w, &it->key);
        write_id(w, &it->value);
        break;
    case PC_PSD_REF_OFFSET:
        psd_put(w, "rele", 4);
        write_class(w, &it->cls);
        psd_put_i32(w, it->offset);
        break;
    case PC_PSD_REF_IDENTIFIER:
        psd_put(w, "Idnt", 4);
        psd_put_u32(w, it->number);
        break;
    case PC_PSD_REF_INDEX:
        psd_put(w, "indx", 4);
        psd_put_u32(w, it->number);
        break;
    case PC_PSD_REF_NAME:
        psd_put(w, "name", 4);
        write_class(w, &it->cls);
        psd_write_ustr(w, &it->name);
        break;
    }
}

static bool ref_eq(const pc_psd_ref_item *a, const pc_psd_ref_item *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_PSD_REF_PROPERTY:
        return class_eq(&a->cls, &b->cls) && pc_psd_id_eq(&a->key, &b->key);
    case PC_PSD_REF_CLASS:
        return class_eq(&a->cls, &b->cls);
    case PC_PSD_REF_ENUMERATED:
        return class_eq(&a->cls, &b->cls) && pc_psd_id_eq(&a->key, &b->key) && pc_psd_id_eq(&a->value, &b->value);
    case PC_PSD_REF_OFFSET:
        return class_eq(&a->cls, &b->cls) && a->offset == b->offset;
    case PC_PSD_REF_IDENTIFIER:
    case PC_PSD_REF_INDEX:
        return a->number == b->number;
    case PC_PSD_REF_NAME:
        return class_eq(&a->cls, &b->cls) && pc_psd_ustr_eq(&a->name, &b->name);
    }
    return false;
}

static pc_status ref_clone(const pc_psd_ref_item *a, pc_psd_ref_item *out, pc_error *err)
{
    pc_status st;
    memset(out, 0, sizeof *out);
    out->kind = a->kind;
    out->offset = a->offset;
    out->number = a->number;
    CK(class_clone(&a->cls, &out->cls, err));
    CK(id_clone(&a->key, &out->key, err));
    CK(id_clone(&a->value, &out->value, err));
    CK(pc_psd_ustr_clone(&a->name, &out->name, err));
    return PC_OK;
fail:
    ref_fini(out);
    return st;
}

/* ---- values ---- */

void pc_psd_value_fini(pc_psd_value *v)
{
    size_t i;
    if (v == NULL) {
        return;
    }
    pc_psd_desc_free(v->desc);
    for (i = 0; i < v->nlist; i++) {
        pc_psd_value_fini(&v->list[i]);
    }
    free(v->list);
    for (i = 0; i < v->nrefs; i++) {
        ref_fini(&v->refs[i]);
    }
    free(v->refs);
    free(v->floats);
    pc_psd_ustr_fini(&v->text);
    pc_psd_id_fini(&v->type_id);
    pc_psd_id_fini(&v->value);
    class_fini(&v->cls);
    pc_psd_bytes_fini(&v->raw);
    memset(v, 0, sizeof *v);
}

static const char *const VTYPES[] = {"obj ", "Objc", "GlbO", "VlLs", "doub", "UntF", "UnFl", "TEXT", "enum",
                                     "long", "comp", "bool", "type", "GlbC", "alis", "Pth ", "tdta", "ObAr"};

static pc_status read_desc_at(psd_rd *r, size_t depth, pc_psd_desc *out, pc_error *err);
static pc_status read_value(psd_rd *r, size_t depth, pc_psd_value *v, pc_error *err);

static pc_status heap_desc(psd_rd *r, size_t depth, pc_psd_desc **out, pc_error *err)
{
    pc_psd_desc *d = calloc(1, sizeof *d);
    pc_status st;
    if (d == NULL) {
        return psd_nomem(err);
    }
    st = read_desc_at(r, depth, d, err);
    if (st != PC_OK) {
        free(d);
        return st;
    }
    *out = d;
    return PC_OK;
}

static pc_status read_typed(psd_rd *r, const uint8_t ty[4], size_t depth, pc_psd_value *v, pc_error *err)
{
    uint32_t n;
    uint32_t i;
    uint8_t b;
    pc_status st;
    int k;
    memset(v, 0, sizeof *v);
    for (k = 0; k < 18; k++) {
        if (memcmp(ty, VTYPES[k], 4) == 0) {
            break;
        }
    }
    if (k == 18) {
        char q[32];
        pc_utf8_debug_quote(ty, 4, q, sizeof q);
        return psd_unsupported(err, "descriptor OSType %s", q);
    }
    v->kind = (pc_psd_vkind)k;
    switch (v->kind) {
    case PC_PSD_V_REFERENCE:
        CK(psd_u32(r, &n, err));
        CK(psd_check_count(r, n, 8, err));
        if (n > 0) {
            v->refs = pc_calloc_array(n, sizeof *v->refs);
            if (v->refs == NULL) {
                st = psd_nomem(err);
                goto fail;
            }
        }
        for (i = 0; i < n; i++) {
            CK(read_ref(r, &v->refs[i], err));
            v->nrefs++;
        }
        break;
    case PC_PSD_V_DESCRIPTOR:
    case PC_PSD_V_GLOBAL_OBJECT:
        CK(heap_desc(r, depth + 1, &v->desc, err));
        break;
    case PC_PSD_V_LIST:
        CK(psd_u32(r, &n, err));
        CK(psd_check_count(r, n, 4, err));
        if (n > 0) {
            v->list = pc_calloc_array(n, sizeof *v->list);
            if (v->list == NULL) {
                st = psd_nomem(err);
                goto fail;
            }
        }
        for (i = 0; i < n; i++) {
            CK(read_value(r, depth + 1, &v->list[i], err));
            v->nlist++;
        }
        break;
    case PC_PSD_V_DOUBLE:
        CK(psd_f64(r, &v->d, err));
        break;
    case PC_PSD_V_UNIT_FLOAT:
        CK(psd_arr4(r, v->unit, err));
        CK(psd_f64(r, &v->d, err));
        break;
    case PC_PSD_V_UNIT_FLOATS:
        CK(psd_arr4(r, v->unit, err));
        CK(psd_u32(r, &n, err));
        CK(psd_check_count(r, n, 8, err));
        if (n > 0) {
            v->floats = pc_malloc_array(n, sizeof *v->floats);
            if (v->floats == NULL) {
                st = psd_nomem(err);
                goto fail;
            }
        }
        for (i = 0; i < n; i++) {
            CK(psd_f64(r, &v->floats[i], err));
            v->nfloats++;
        }
        break;
    case PC_PSD_V_TEXT:
        CK(psd_read_ustr(r, &v->text, err));
        break;
    case PC_PSD_V_ENUMERATED:
        CK(read_id(r, &v->type_id, err));
        CK(read_id(r, &v->value, err));
        break;
    case PC_PSD_V_INTEGER:
        CK(psd_i32(r, &v->i, err));
        break;
    case PC_PSD_V_LARGE_INTEGER:
        CK(psd_i64(r, &v->l, err));
        break;
    case PC_PSD_V_BOOLEAN:
        CK(psd_u8(r, &b, err));
        v->b = b != 0;
        break;
    case PC_PSD_V_CLASS:
    case PC_PSD_V_GLOBAL_CLASS:
        CK(read_class(r, &v->cls, err));
        break;
    case PC_PSD_V_ALIAS:
    case PC_PSD_V_PATH:
    case PC_PSD_V_RAW_DATA:
        CK(psd_u32(r, &n, err));
        CK(psd_take_bytes(r, n, &v->raw, err));
        break;
    case PC_PSD_V_OBJECT_ARRAY:
        CK(psd_u32(r, &v->prefix, err));
        CK(heap_desc(r, depth + 1, &v->desc, err));
        break;
    }
    return PC_OK;
fail:
    pc_psd_value_fini(v);
    return st;
}

static pc_status read_value(psd_rd *r, size_t depth, pc_psd_value *v, pc_error *err)
{
    uint8_t ty[4];
    pc_status st;
    memset(v, 0, sizeof *v);
    if (depth > PC_PSD_DESC_MAX_DEPTH) {
        return psd_limit(err, "descriptor nesting too deep");
    }
    st = psd_arr4(r, ty, err);
    if (st != PC_OK) {
        return st;
    }
    return read_typed(r, ty, depth, v, err);
}

static void write_value(psd_w *w, const pc_psd_value *v)
{
    size_t i;
    psd_put(w, VTYPES[v->kind], 4);
    switch (v->kind) {
    case PC_PSD_V_REFERENCE:
        psd_put_u32(w, (uint32_t)v->nrefs);
        for (i = 0; i < v->nrefs; i++) {
            write_ref(w, &v->refs[i]);
        }
        break;
    case PC_PSD_V_DESCRIPTOR:
    case PC_PSD_V_GLOBAL_OBJECT:
        psd_write_desc(w, v->desc);
        break;
    case PC_PSD_V_LIST:
        psd_put_u32(w, (uint32_t)v->nlist);
        for (i = 0; i < v->nlist; i++) {
            write_value(w, &v->list[i]);
        }
        break;
    case PC_PSD_V_DOUBLE:
        psd_put_f64(w, v->d);
        break;
    case PC_PSD_V_UNIT_FLOAT:
        psd_put(w, v->unit, 4);
        psd_put_f64(w, v->d);
        break;
    case PC_PSD_V_UNIT_FLOATS:
        psd_put(w, v->unit, 4);
        psd_put_u32(w, (uint32_t)v->nfloats);
        for (i = 0; i < v->nfloats; i++) {
            psd_put_f64(w, v->floats[i]);
        }
        break;
    case PC_PSD_V_TEXT:
        psd_write_ustr(w, &v->text);
        break;
    case PC_PSD_V_ENUMERATED:
        write_id(w, &v->type_id);
        write_id(w, &v->value);
        break;
    case PC_PSD_V_INTEGER:
        psd_put_i32(w, v->i);
        break;
    case PC_PSD_V_LARGE_INTEGER:
        psd_put_i64(w, v->l);
        break;
    case PC_PSD_V_BOOLEAN:
        psd_put_u8(w, v->b ? 1 : 0);
        break;
    case PC_PSD_V_CLASS:
    case PC_PSD_V_GLOBAL_CLASS:
        write_class(w, &v->cls);
        break;
    case PC_PSD_V_ALIAS:
    case PC_PSD_V_PATH:
    case PC_PSD_V_RAW_DATA:
        psd_put_u32(w, (uint32_t)v->raw.len);
        psd_put(w, v->raw.data, v->raw.len);
        break;
    case PC_PSD_V_OBJECT_ARRAY:
        psd_put_u32(w, v->prefix);
        psd_write_desc(w, v->desc);
        break;
    }
}

bool pc_psd_value_eq(const pc_psd_value *a, const pc_psd_value *b)
{
    size_t i;
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_PSD_V_REFERENCE:
        if (a->nrefs != b->nrefs) {
            return false;
        }
        for (i = 0; i < a->nrefs; i++) {
            if (!ref_eq(&a->refs[i], &b->refs[i])) {
                return false;
            }
        }
        return true;
    case PC_PSD_V_DESCRIPTOR:
    case PC_PSD_V_GLOBAL_OBJECT:
        return pc_psd_desc_eq(a->desc, b->desc);
    case PC_PSD_V_LIST:
        if (a->nlist != b->nlist) {
            return false;
        }
        for (i = 0; i < a->nlist; i++) {
            if (!pc_psd_value_eq(&a->list[i], &b->list[i])) {
                return false;
            }
        }
        return true;
    case PC_PSD_V_DOUBLE:
        return a->d == b->d;
    case PC_PSD_V_UNIT_FLOAT:
        return memcmp(a->unit, b->unit, 4) == 0 && a->d == b->d;
    case PC_PSD_V_UNIT_FLOATS:
        if (memcmp(a->unit, b->unit, 4) != 0 || a->nfloats != b->nfloats) {
            return false;
        }
        for (i = 0; i < a->nfloats; i++) {
            if (!(a->floats[i] == b->floats[i])) {
                return false;
            }
        }
        return true;
    case PC_PSD_V_TEXT:
        return pc_psd_ustr_eq(&a->text, &b->text);
    case PC_PSD_V_ENUMERATED:
        return pc_psd_id_eq(&a->type_id, &b->type_id) && pc_psd_id_eq(&a->value, &b->value);
    case PC_PSD_V_INTEGER:
        return a->i == b->i;
    case PC_PSD_V_LARGE_INTEGER:
        return a->l == b->l;
    case PC_PSD_V_BOOLEAN:
        return a->b == b->b;
    case PC_PSD_V_CLASS:
    case PC_PSD_V_GLOBAL_CLASS:
        return class_eq(&a->cls, &b->cls);
    case PC_PSD_V_ALIAS:
    case PC_PSD_V_PATH:
    case PC_PSD_V_RAW_DATA:
        return pc_psd_bytes_eq(&a->raw, &b->raw);
    case PC_PSD_V_OBJECT_ARRAY:
        return a->prefix == b->prefix && pc_psd_desc_eq(a->desc, b->desc);
    }
    return false;
}

pc_status pc_psd_value_clone(const pc_psd_value *a, pc_psd_value *out, pc_error *err)
{
    pc_psd_value v;
    size_t i;
    pc_status st;
    memset(&v, 0, sizeof v);
    v.kind = a->kind;
    v.prefix = a->prefix;
    v.d = a->d;
    memcpy(v.unit, a->unit, 4);
    v.i = a->i;
    v.l = a->l;
    v.b = a->b;
    if (a->desc != NULL) {
        v.desc = calloc(1, sizeof *v.desc);
        if (v.desc == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
        st = pc_psd_desc_clone(a->desc, v.desc, err);
        if (st != PC_OK) {
            free(v.desc);
            v.desc = NULL;
            goto fail;
        }
    }
    if (a->nlist > 0) {
        v.list = pc_calloc_array(a->nlist, sizeof *v.list);
        if (v.list == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
        for (i = 0; i < a->nlist; i++) {
            CK(pc_psd_value_clone(&a->list[i], &v.list[i], err));
            v.nlist++;
        }
    }
    if (a->nrefs > 0) {
        v.refs = pc_calloc_array(a->nrefs, sizeof *v.refs);
        if (v.refs == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
        for (i = 0; i < a->nrefs; i++) {
            CK(ref_clone(&a->refs[i], &v.refs[i], err));
            v.nrefs++;
        }
    }
    if (a->nfloats > 0) {
        v.floats = pc_malloc_array(a->nfloats, sizeof *v.floats);
        if (v.floats == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
        memcpy(v.floats, a->floats, a->nfloats * sizeof *v.floats);
        v.nfloats = a->nfloats;
    }
    CK(pc_psd_ustr_clone(&a->text, &v.text, err));
    CK(id_clone(&a->type_id, &v.type_id, err));
    CK(id_clone(&a->value, &v.value, err));
    CK(class_clone(&a->cls, &v.cls, err));
    CK(pc_psd_bytes_clone(&a->raw, &v.raw, err));
    *out = v;
    return PC_OK;
fail:
    pc_psd_value_fini(&v);
    return st;
}

pc_psd_value pc_psd_value_int(int32_t i)
{
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_INTEGER;
    v.i = i;
    return v;
}

pc_psd_value pc_psd_value_double(double d)
{
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_DOUBLE;
    v.d = d;
    return v;
}

pc_psd_value pc_psd_value_bool(bool b)
{
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_BOOLEAN;
    v.b = b;
    return v;
}

pc_status pc_psd_value_text(const char *utf8, bool nul, pc_psd_value *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->kind = PC_PSD_V_TEXT;
    return pc_psd_ustr_from_utf8(utf8, nul, &out->text, err);
}

pc_status pc_psd_value_enum(const char *type_id, const char *value, pc_psd_value *out, pc_error *err)
{
    pc_status st;
    memset(out, 0, sizeof *out);
    out->kind = PC_PSD_V_ENUMERATED;
    st = pc_psd_id_make(type_id, &out->type_id, err);
    if (st == PC_OK) {
        st = pc_psd_id_make(value, &out->value, err);
    }
    if (st != PC_OK) {
        pc_psd_value_fini(out);
    }
    return st;
}

pc_status pc_psd_value_object(pc_psd_desc *d, pc_psd_value *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->kind = PC_PSD_V_DESCRIPTOR;
    out->desc = malloc(sizeof *out->desc);
    if (out->desc == NULL) {
        pc_psd_desc_fini(d);
        return psd_nomem(err);
    }
    *out->desc = *d;
    memset(d, 0, sizeof *d);
    return PC_OK;
}

pc_status pc_psd_value_list_push(pc_psd_value *list, pc_psd_value *item, pc_error *err)
{
    pc_psd_value *g = realloc(list->list, (list->nlist + 1) * sizeof *g);
    if (g == NULL) {
        pc_psd_value_fini(item);
        return psd_nomem(err);
    }
    list->list = g;
    list->list[list->nlist++] = *item;
    memset(item, 0, sizeof *item);
    return PC_OK;
}

/* ---- descriptors ---- */

pc_status pc_psd_desc_init(pc_psd_desc *d, const char *class_id, pc_error *err)
{
    memset(d, 0, sizeof *d);
    return pc_psd_id_make(class_id, &d->class_id, err);
}

void pc_psd_desc_fini(pc_psd_desc *d)
{
    size_t i;
    if (d == NULL) {
        return;
    }
    pc_psd_ustr_fini(&d->name);
    pc_psd_id_fini(&d->class_id);
    for (i = 0; i < d->n; i++) {
        pc_psd_id_fini(&d->items[i].key);
        pc_psd_value_fini(&d->items[i].value);
    }
    free(d->items);
    memset(d, 0, sizeof *d);
}

void pc_psd_desc_free(pc_psd_desc *d)
{
    if (d != NULL) {
        pc_psd_desc_fini(d);
        free(d);
    }
}

pc_status pc_psd_desc_clone(const pc_psd_desc *a, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    size_t i;
    pc_status st;
    memset(&d, 0, sizeof d);
    CK(pc_psd_ustr_clone(&a->name, &d.name, err));
    CK(id_clone(&a->class_id, &d.class_id, err));
    if (a->n > 0) {
        d.items = pc_calloc_array(a->n, sizeof *d.items);
        if (d.items == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
        d.cap = a->n;
        for (i = 0; i < a->n; i++) {
            CK(id_clone(&a->items[i].key, &d.items[i].key, err));
            st = pc_psd_value_clone(&a->items[i].value, &d.items[i].value, err);
            d.n++; /* key owned even if the value failed */
            if (st != PC_OK) {
                goto fail;
            }
        }
    }
    *out = d;
    return PC_OK;
fail:
    pc_psd_desc_fini(&d);
    return st;
}

bool pc_psd_desc_eq(const pc_psd_desc *a, const pc_psd_desc *b)
{
    size_t i;
    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (!pc_psd_ustr_eq(&a->name, &b->name) || !pc_psd_id_eq(&a->class_id, &b->class_id) || a->n != b->n) {
        return false;
    }
    for (i = 0; i < a->n; i++) {
        if (!pc_psd_id_eq(&a->items[i].key, &b->items[i].key) ||
            !pc_psd_value_eq(&a->items[i].value, &b->items[i].value)) {
            return false;
        }
    }
    return true;
}

static pc_status push_item(pc_psd_desc *d, pc_psd_id *key, pc_psd_value *value, pc_error *err)
{
    if (d->n == d->cap) {
        pc_psd_item *g = pc_grow_array(d->items, &d->cap, d->n + 1, sizeof *g);
        if (g == NULL) {
            pc_psd_id_fini(key);
            pc_psd_value_fini(value);
            return psd_nomem(err);
        }
        d->items = g;
    }
    d->items[d->n].key = *key;
    d->items[d->n].value = *value;
    d->n++;
    memset(key, 0, sizeof *key);
    memset(value, 0, sizeof *value);
    return PC_OK;
}

pc_status pc_psd_desc_add(pc_psd_desc *d, const char *key, pc_psd_value *value, pc_error *err)
{
    pc_psd_id k;
    pc_status st = pc_psd_id_make(key, &k, err);
    if (st != PC_OK) {
        pc_psd_value_fini(value);
        return st;
    }
    return push_item(d, &k, value, err);
}

const pc_psd_value *pc_psd_desc_get(const pc_psd_desc *d, const char *key)
{
    size_t i;
    for (i = 0; d != NULL && i < d->n; i++) {
        if (pc_psd_id_is(&d->items[i].key, key)) {
            return &d->items[i].value;
        }
    }
    return NULL;
}

static pc_status read_desc_at(psd_rd *r, size_t depth, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    uint32_t n;
    uint32_t i;
    pc_status st;
    memset(&d, 0, sizeof d);
    if (depth > PC_PSD_DESC_MAX_DEPTH) {
        return psd_limit(err, "descriptor nesting too deep");
    }
    CK(psd_read_ustr(r, &d.name, err));
    CK(read_id(r, &d.class_id, err));
    CK(psd_u32(r, &n, err));
    /* Each item needs at least 5 (1-byte string key) + 4 (type) + 1 (bool). */
    CK(psd_check_count(r, n, 10, err));
    if (n > 0) {
        d.items = pc_calloc_array(n, sizeof *d.items);
        if (d.items == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
        d.cap = n;
    }
    for (i = 0; i < n; i++) {
        pc_psd_id key;
        pc_psd_value v;
        CK(read_id(r, &key, err));
        st = read_value(r, depth + 1, &v, err);
        if (st != PC_OK) {
            pc_psd_id_fini(&key);
            goto fail;
        }
        d.items[d.n].key = key;
        d.items[d.n].value = v;
        d.n++;
    }
    *out = d;
    return PC_OK;
fail:
    pc_psd_desc_fini(&d);
    return st;
}

pc_status psd_read_desc(psd_rd *r, pc_psd_desc *out, pc_error *err)
{
    return read_desc_at(r, 0, out, err);
}

pc_status psd_read_vdesc(psd_rd *r, pc_psd_desc *out, pc_error *err)
{
    uint32_t version;
    pc_status st = psd_u32(r, &version, err);
    if (st != PC_OK) {
        return st;
    }
    if (version != 16) {
        return psd_invalid(err, "descriptor version %u, expected 16", (unsigned)version);
    }
    return psd_read_desc(r, out, err);
}

void psd_write_desc(psd_w *w, const pc_psd_desc *d)
{
    size_t i;
    psd_write_ustr(w, &d->name);
    write_id(w, &d->class_id);
    psd_put_u32(w, (uint32_t)d->n);
    for (i = 0; i < d->n; i++) {
        write_id(w, &d->items[i].key);
        write_value(w, &d->items[i].value);
    }
}

void psd_write_vdesc(psd_w *w, const pc_psd_desc *d)
{
    psd_put_u32(w, 16);
    psd_write_desc(w, d);
}

pc_status pc_psd_desc_parse(const uint8_t *data, size_t len, pc_psd_desc *out, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pc_psd_desc d;
    pc_status st = psd_read_desc(&r, &d, err);
    if (st != PC_OK) {
        return st;
    }
    if (!psd_empty(&r)) {
        pc_psd_desc_fini(&d);
        return psd_invalid(err, "trailing bytes after descriptor");
    }
    *out = d;
    return PC_OK;
}

pc_status pc_psd_vdesc_parse(const uint8_t *data, size_t len, pc_psd_desc *out, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pc_psd_desc d;
    pc_status st = psd_read_vdesc(&r, &d, err);
    if (st != PC_OK) {
        return st;
    }
    if (!psd_empty(&r)) {
        pc_psd_desc_fini(&d);
        return psd_invalid(err, "trailing bytes after versioned descriptor");
    }
    *out = d;
    return PC_OK;
}

pc_status pc_psd_vdesc_parse_prefix(const uint8_t *data, size_t len, pc_psd_desc *out, size_t *used, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pc_status st = psd_read_vdesc(&r, out, err);
    if (st == PC_OK) {
        *used = r.pos;
    }
    return st;
}

pc_status pc_psd_desc_to_bytes(const pc_psd_desc *d, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    psd_w_init(&w, err);
    psd_write_desc(&w, d);
    return psd_w_finish(&w, out);
}

pc_status pc_psd_vdesc_to_bytes(const pc_psd_desc *d, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    psd_w_init(&w, err);
    psd_write_vdesc(&w, d);
    return psd_w_finish(&w, out);
}
