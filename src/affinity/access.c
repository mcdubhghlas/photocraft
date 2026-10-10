/* Typed reads of stream objects (port of crates/affinity/src/access.rs). Every accessor reports
 * absence when a field is missing or has another type, so the document mapping treats odd data as
 * absent rather than failing. */
#include <math.h>
#include <string.h>

#include "affinity/internal.h"

const pc_af_object *ax_object(const pc_af_stream *s, size_t id) { return id < s->nobjects ? &s->objects[id] : NULL; }

const pc_af_value *ax_field(const pc_af_stream *s, size_t id, const char *tag)
{
    const pc_af_object *o = ax_object(s, id);
    return o != NULL ? pc_af_object_get(o, PC_AF_TAG(tag)) : NULL;
}

bool ax_class(const pc_af_stream *s, size_t id, pc_af_tag *out)
{
    const pc_af_object *o = ax_object(s, id);
    if (o == NULL) {
        return false;
    }
    *out = o->cls;
    return true;
}

bool ax_is(const pc_af_stream *s, size_t id, const char *cls)
{
    pc_af_tag t;
    return ax_class(s, id, &t) && t == PC_AF_TAG(cls);
}

bool ax_obj(const pc_af_stream *s, size_t id, const char *tag, size_t *out)
{
    const pc_af_value *v = ax_field(s, id, tag);
    if (v == NULL || v->kind != PC_AF_OBJ || !v->as.obj.some) {
        return false;
    }
    *out = v->as.obj.id;
    return true;
}

const pc_af_value *ax_objs(const pc_af_stream *s, size_t id, const char *tag)
{
    const pc_af_value *v = ax_field(s, id, tag);
    return v != NULL && v->kind == PC_AF_ARRAY ? v : NULL;
}

bool ax_next_obj(const pc_af_value *arr, size_t *i, size_t *id)
{
    if (arr == NULL) {
        return false;
    }
    while (*i < arr->as.arr.n) {
        const pc_af_value *e = &arr->as.arr.v[(*i)++];
        if (e->kind == PC_AF_OBJ && e->as.obj.some) {
            *id = e->as.obj.id;
            return true;
        }
    }
    return false;
}

size_t ax_objs_count(const pc_af_value *arr)
{
    size_t i = 0;
    size_t id;
    size_t n = 0;
    while (ax_next_obj(arr, &i, &id)) {
        n++;
    }
    return n;
}

bool ax_f64(const pc_af_stream *s, size_t id, const char *tag, double *out)
{
    const pc_af_value *v = ax_field(s, id, tag);
    if (v == NULL || v->kind != PC_AF_FLOAT || !isfinite(v->as.f)) {
        return false;
    }
    *out = v->as.f;
    return true;
}

bool ax_int(const pc_af_stream *s, size_t id, const char *tag, int64_t *out)
{
    const pc_af_value *v = ax_field(s, id, tag);
    if (v == NULL) {
        return false;
    }
    if (v->kind == PC_AF_INT) {
        *out = v->as.i;
        return true;
    }
    if (v->kind == PC_AF_UINT && v->as.u <= (uint64_t)INT64_MAX) {
        *out = (int64_t)v->as.u;
        return true;
    }
    return false;
}

bool ax_bool(const pc_af_stream *s, size_t id, const char *tag, bool *out)
{
    const pc_af_value *v = ax_field(s, id, tag);
    if (v == NULL || v->kind != PC_AF_BOOL) {
        return false;
    }
    *out = v->as.b;
    return true;
}

const char *ax_str(const pc_af_stream *s, size_t id, const char *tag)
{
    const pc_af_value *v = ax_field(s, id, tag);
    return v != NULL && v->kind == PC_AF_STR ? v->as.str.s : NULL;
}

bool ax_enum(const pc_af_stream *s, size_t id, const char *tag, uint16_t *eid, uint16_t *ver)
{
    const pc_af_value *v = ax_field(s, id, tag);
    if (v == NULL || v->kind != PC_AF_ENUM) {
        return false;
    }
    *eid = v->as.e.id;
    *ver = v->as.e.version;
    return true;
}

bool ax_floats(const pc_af_stream *s, size_t id, const char *tag, size_t n, double *out)
{
    const pc_af_value *v = ax_field(s, id, tag);
    size_t i;
    if (v == NULL || v->kind != PC_AF_FLOATS || v->as.floats.n != n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (!isfinite(v->as.floats.v[i])) {
            return false;
        }
    }
    memcpy(out, v->as.floats.v, n * sizeof *out);
    return true;
}

const uint8_t *ax_bytes(const pc_af_stream *s, size_t id, const char *tag, size_t *n)
{
    const pc_af_value *v = ax_field(s, id, tag);
    if (v == NULL || (v->kind != PC_AF_BYTES && v->kind != PC_AF_BLOB)) {
        return NULL;
    }
    *n = v->as.bytes.n;
    return v->as.bytes.v != NULL ? v->as.bytes.v : (const uint8_t *)"";
}

const char *ax_entry(const pc_af_stream *s, size_t id, const char *tag)
{
    const pc_af_value *v = ax_field(s, id, tag);
    return v != NULL && v->kind == PC_AF_ENTRY ? v->as.str.s : NULL;
}
