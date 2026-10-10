/* The tagged object stream inside doc.dat (port of crates/affinity/src/stream.rs): a
 * self-describing tree of objects whose fields carry a type code and a four-character tag. No
 * schema is needed; unknown fields are kept. Every count is checked against the remaining input
 * and a global value budget before anything is allocated. */
#include <stdlib.h>
#include <string.h>

#include "affinity/internal.h"
#include "base/mem.h"

#define MAX_OBJECTS 8000000u

typedef struct parser {
    ax_cursor c;
    pc_af_object *objects;
    size_t nobjects;
    size_t cap;
    /* shared ids -> object (open addressing) */
    uint32_t *keys;
    size_t *vals;
    uint8_t *used;
    size_t hcap;
    size_t hn;
    size_t depth;
    size_t values;
} parser;

static void value_fini(pc_af_value *v);

static void fields_fini(pc_af_field *f, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        value_fini(&f[i].v);
    }
    free(f);
}

static void value_fini(pc_af_value *v)
{
    size_t i;
    switch (v->kind) {
    case PC_AF_STR:
    case PC_AF_ENTRY:
        free(v->as.str.s);
        break;
    case PC_AF_INTS:
        free(v->as.ints.v);
        break;
    case PC_AF_FLOATS:
        free(v->as.floats.v);
        break;
    case PC_AF_BYTES:
    case PC_AF_BLOB:
        free(v->as.bytes.v);
        break;
    case PC_AF_ARRAY:
        for (i = 0; i < v->as.arr.n; i++) {
            value_fini(&v->as.arr.v[i]);
        }
        free(v->as.arr.v);
        break;
    default:
        break;
    }
    v->kind = PC_AF_INT;
}

static void object_fini(pc_af_object *o)
{
    free(o->chain);
    fields_fini(o->fields, o->nfields);
    memset(o, 0, sizeof *o);
}

void pc_af_stream_fini(pc_af_stream *s)
{
    size_t i;
    if (s == NULL) {
        return;
    }
    for (i = 0; i < s->nobjects; i++) {
        object_fini(&s->objects[i]);
    }
    free(s->objects);
    memset(s, 0, sizeof *s);
}

const pc_af_value *pc_af_object_get(const pc_af_object *o, pc_af_tag tag)
{
    size_t i;
    for (i = 0; i < o->nfields; i++) {
        if (o->fields[i].tag == tag) {
            return &o->fields[i].v;
        }
    }
    return NULL;
}

static pc_status push(parser *p, pc_af_object o, size_t *id, pc_error *err)
{
    if (p->nobjects >= MAX_OBJECTS) {
        return ax_limit(err, "too many objects");
    }
    if (p->nobjects == p->cap) {
        size_t cap = p->cap > 0 ? p->cap * 2 : 256;
        pc_af_object *v = realloc(p->objects, cap * sizeof *v);
        if (v == NULL) {
            return ax_nomem(err);
        }
        p->objects = v;
        p->cap = cap;
    }
    p->objects[p->nobjects] = o;
    *id = p->nobjects++;
    return PC_OK;
}

/* Counts n more values against the budget, before anything is allocated for them. */
static pc_status spend(parser *p, size_t n, pc_error *err)
{
    if (p->values > SIZE_MAX - n || p->values + n > PC_AF_MAX_VALUES) {
        return ax_limit(err, "too many values in the document stream");
    }
    p->values += n;
    return PC_OK;
}

static pc_status vec(parser *p, size_t n, pc_af_value **out, pc_error *err)
{
    pc_status st = spend(p, n, err);
    if (st != PC_OK) {
        return st;
    }
    *out = pc_calloc_array(n > 0 ? n : 1, sizeof **out);
    if (*out == NULL) {
        return ax_limit(err, "not enough memory for the document stream");
    }
    return PC_OK;
}

/* Element count of an array: each element takes at least min bytes. */
static pc_status count(parser *p, size_t min, size_t *n, pc_error *err)
{
    uint32_t v;
    pc_status st = ax_u32(&p->c, &v, err);
    size_t m = min > 0 ? min : 1;
    if (st != PC_OK) {
        return st;
    }
    if (v > SIZE_MAX / m || (size_t)v * m > ax_remaining(&p->c)) {
        return ax_malformed(err, "array longer than its data");
    }
    *n = v;
    return PC_OK;
}

/* ---- shared-id map ---- */

static bool shared_find(const parser *p, uint32_t key, size_t *id)
{
    size_t i;
    if (p->hcap == 0) {
        return false;
    }
    i = (size_t)(key * 2654435761u) & (p->hcap - 1);
    while (p->used[i]) {
        if (p->keys[i] == key) {
            *id = p->vals[i];
            return true;
        }
        i = (i + 1) & (p->hcap - 1);
    }
    return false;
}

static bool shared_insert(parser *p, uint32_t key, size_t id)
{
    size_t i;
    if (p->hcap == 0 || (p->hn + 1) * 2 > p->hcap) {
        size_t cap = p->hcap > 0 ? p->hcap * 2 : 256;
        uint32_t *k = calloc(cap, sizeof *k);
        size_t *v = calloc(cap, sizeof *v);
        uint8_t *u = calloc(cap, 1);
        size_t j;
        if (k == NULL || v == NULL || u == NULL) {
            free(k);
            free(v);
            free(u);
            return false;
        }
        for (j = 0; j < p->hcap; j++) {
            if (p->used[j]) {
                size_t s = (size_t)(p->keys[j] * 2654435761u) & (cap - 1);
                while (u[s]) {
                    s = (s + 1) & (cap - 1);
                }
                u[s] = 1;
                k[s] = p->keys[j];
                v[s] = p->vals[j];
            }
        }
        free(p->keys);
        free(p->vals);
        free(p->used);
        p->keys = k;
        p->vals = v;
        p->used = u;
        p->hcap = cap;
    }
    i = (size_t)(key * 2654435761u) & (p->hcap - 1);
    while (p->used[i]) {
        i = (i + 1) & (p->hcap - 1);
    }
    p->used[i] = 1;
    p->keys[i] = key;
    p->vals[i] = id;
    p->hn++;
    return true;
}

/* ---- parsing ---- */

static pc_status fields(parser *p, bool tagged, pc_af_tag cls, pc_af_field **out, size_t *nout, pc_error *err);
static pc_status scalar(parser *p, uint8_t code, bool tagged, pc_af_tag cls, pc_af_value *v, pc_error *err);
static pc_status inline_obj(parser *p, bool has_cls, pc_af_tag cls, pc_af_value *v, pc_error *err);
static pc_status shared(parser *p, pc_af_value *v, pc_error *err);

static pc_status string(parser *p, pc_af_value *v, pc_af_vkind kind, pc_error *err)
{
    uint32_t n;
    const uint8_t *b;
    pc_status st;
    if ((st = ax_u32(&p->c, &n, err)) != PC_OK || (st = ax_take(&p->c, n, &b, err)) != PC_OK) {
        return st;
    }
    v->as.str.s = ax_lossy(b, n, &v->as.str.n);
    if (v->as.str.s == NULL) {
        return ax_nomem(err);
    }
    v->kind = kind;
    return PC_OK;
}

static double rd_f32(const uint8_t *b)
{
    uint32_t u = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    float f;
    memcpy(&f, &u, 4);
    return (double)f;
}

static double rd_f64(const uint8_t *b)
{
    uint64_t u = 0;
    double d;
    int i;
    for (i = 7; i >= 0; i--) {
        u = u << 8 | b[i];
    }
    memcpy(&d, &u, 8);
    return d;
}

static pc_status floats(parser *p, size_t n, bool wide, pc_af_value *v, pc_error *err)
{
    double *f = malloc(n * sizeof *f);
    size_t i;
    if (f == NULL) {
        return ax_nomem(err);
    }
    for (i = 0; i < n; i++) {
        const uint8_t *b;
        pc_status st = ax_take(&p->c, wide ? 8 : 4, &b, err);
        if (st != PC_OK) {
            free(f);
            return st;
        }
        f[i] = wide ? rd_f64(b) : rd_f32(b);
    }
    v->kind = PC_AF_FLOATS;
    v->as.floats.v = f;
    v->as.floats.n = n;
    return PC_OK;
}

static pc_status bytes_val(parser *p, size_t n, pc_af_vkind kind, pc_af_value *v, pc_error *err)
{
    const uint8_t *b;
    pc_status st = ax_take(&p->c, n, &b, err);
    if (st != PC_OK) {
        return st;
    }
    v->as.bytes.v = malloc(n > 0 ? n : 1);
    if (v->as.bytes.v == NULL) {
        return ax_nomem(err);
    }
    if (n > 0) {
        memcpy(v->as.bytes.v, b, n);
    }
    v->as.bytes.n = n;
    v->kind = kind;
    return PC_OK;
}

static pc_status scalar(parser *p, uint8_t code, bool tagged, pc_af_tag cls, pc_af_value *v, pc_error *err)
{
    ax_cursor *c = &p->c;
    pc_status st = PC_OK;
    const uint8_t *b;
    memset(v, 0, sizeof *v);
    switch (code) {
    case 0x01: {
        uint8_t x;
        st = ax_u8(c, &x, err);
        v->kind = PC_AF_UINT;
        v->as.u = x;
        return st;
    }
    case 0x02: {
        uint16_t x;
        st = ax_u16(c, &x, err);
        v->kind = PC_AF_UINT;
        v->as.u = x;
        return st;
    }
    case 0x03:
    case 0x2f:
    case 0x34: {
        uint32_t x;
        st = ax_u32(c, &x, err);
        v->kind = PC_AF_UINT;
        v->as.u = x;
        return st;
    }
    case 0x04:
        v->kind = PC_AF_UINT;
        return ax_u64(c, &v->as.u, err);
    case 0x05:
        if ((st = ax_take(c, 1, &b, err)) == PC_OK) {
            v->kind = PC_AF_INT;
            v->as.i = (int8_t)b[0];
        }
        return st;
    case 0x06: {
        uint16_t x;
        if ((st = ax_u16(c, &x, err)) == PC_OK) {
            v->kind = PC_AF_INT;
            v->as.i = (int16_t)x;
        }
        return st;
    }
    case 0x07: {
        uint32_t x;
        if ((st = ax_u32(c, &x, err)) == PC_OK) {
            v->kind = PC_AF_INT;
            v->as.i = (int32_t)x;
        }
        return st;
    }
    case 0x08: {
        uint64_t x;
        if ((st = ax_u64(c, &x, err)) == PC_OK) {
            v->kind = PC_AF_INT;
            v->as.i = (int64_t)x;
        }
        return st;
    }
    case 0x09:
        if ((st = ax_take(c, 4, &b, err)) == PC_OK) {
            v->kind = PC_AF_FLOAT;
            v->as.f = rd_f32(b);
        }
        return st;
    case 0x0a:
        if ((st = ax_take(c, 8, &b, err)) == PC_OK) {
            v->kind = PC_AF_FLOAT;
            v->as.f = rd_f64(b);
        }
        return st;
    case 0x29: {
        uint8_t x;
        if ((st = ax_u8(c, &x, err)) == PC_OK) {
            v->kind = PC_AF_BOOL;
            v->as.b = x != 0;
        }
        return st;
    }
    case 0x2a:
        if ((st = ax_u16(c, &v->as.e.id, err)) == PC_OK && (st = ax_u16(c, &v->as.e.version, err)) == PC_OK) {
            v->kind = PC_AF_ENUM;
        }
        return st;
    case 0x2b:
    case 0x2e:
        return string(p, v, PC_AF_STR, err);
    case 0x2c: {
        uint16_t n;
        if ((st = ax_u16(c, &n, err)) != PC_OK) {
            return st;
        }
        return bytes_val(p, n, PC_AF_BYTES, v, err);
    }
    case 0x2d: {
        uint32_t n;
        if ((st = ax_u32(c, &n, err)) != PC_OK) {
            return st;
        }
        return bytes_val(p, n, PC_AF_BLOB, v, err);
    }
    case 0x33: {
        uint32_t skip;
        if ((st = ax_u32(c, &skip, err)) != PC_OK) {
            return st;
        }
        return string(p, v, PC_AF_ENTRY, err);
    }
    case 0x75: {
        uint16_t skip;
        uint8_t n;
        uint64_t f = 0;
        size_t i;
        if ((st = ax_u16(c, &skip, err)) != PC_OK || (st = ax_u8(c, &n, err)) != PC_OK) {
            return st;
        }
        if (n > 8) {
            return ax_malformed(err, "flags wider than 64 bits");
        }
        if ((st = ax_take(c, n, &b, err)) != PC_OK) {
            return st;
        }
        for (i = n; i > 0; i--) {
            f = f << 8 | b[i - 1];
        }
        v->kind = PC_AF_FLAGS;
        v->as.u = f;
        return PC_OK;
    }
    case 0x30: {
        pc_af_object o;
        size_t id;
        memset(&o, 0, sizeof o);
        o.cls = cls;
        o.kind = PC_AF_POSITIONAL;
        if ((st = fields(p, false, cls, &o.fields, &o.nfields, err)) != PC_OK) {
            return st;
        }
        if ((st = push(p, o, &id, err)) != PC_OK) {
            fields_fini(o.fields, o.nfields);
            return st;
        }
        v->kind = PC_AF_OBJ;
        v->as.obj.some = true;
        v->as.obj.id = id;
        return PC_OK;
    }
    case 0x31:
    case 0x32:
        if (!tagged) {
            return ax_malformed(err, "object without a field tag");
        }
        return code == 0x31 ? shared(p, v, err) : inline_obj(p, false, 0, v, err);
    default:
        break;
    }
    if (code >= 0x15 && code <= 0x19) {
        size_t n = (size_t)code - 0x13;
        int64_t *iv = malloc(n * sizeof *iv);
        size_t i;
        if (iv == NULL) {
            return ax_nomem(err);
        }
        for (i = 0; i < n; i++) {
            uint32_t x;
            if ((st = ax_u32(c, &x, err)) != PC_OK) {
                free(iv);
                return st;
            }
            iv[i] = (int32_t)x;
        }
        v->kind = PC_AF_INTS;
        v->as.ints.v = iv;
        v->as.ints.n = n;
        return PC_OK;
    }
    if (code >= 0x1f && code <= 0x23) {
        return floats(p, (size_t)code - 0x1d, false, v, err);
    }
    if (code >= 0x24 && code <= 0x28) {
        return floats(p, (size_t)code - 0x22, true, v, err);
    }
    if (code >= 0x35 && code <= 0x74) {
        return bytes_val(p, (size_t)code - 0x34, PC_AF_BYTES, v, err);
    }
    return ax_unsupported(err, "unknown field type in document stream");
}

static size_t min_size(uint8_t code)
{
    switch (code) {
    case 0x01:
    case 0x05:
    case 0x29:
        return 1;
    case 0x02:
    case 0x06:
        return 2;
    case 0x03:
    case 0x07:
    case 0x09:
    case 0x2f:
    case 0x34:
        return 4;
    case 0x04:
    case 0x08:
    case 0x0a:
        return 8;
    default:
        break;
    }
    if (code >= 0x15 && code <= 0x19) {
        return 4 * ((size_t)code - 0x13);
    }
    if (code >= 0x1f && code <= 0x23) {
        return 4 * ((size_t)code - 0x1d);
    }
    if (code >= 0x24 && code <= 0x28) {
        return 8 * ((size_t)code - 0x22);
    }
    if (code >= 0x35 && code <= 0x74) {
        return (size_t)code - 0x34;
    }
    return 1;
}

static pc_status array(parser *p, uint8_t code, pc_af_tag cls, pc_af_value *v, pc_error *err)
{
    size_t n = 0;
    size_t i;
    pc_af_value *items = NULL;
    pc_status st;
    memset(v, 0, sizeof *v);
    switch (code) {
    case 0x29: {
        uint32_t m;
        const uint8_t *bits;
        if ((st = ax_u32(&p->c, &m, err)) != PC_OK) {
            return st;
        }
        n = m;
        if ((st = ax_take(&p->c, n / 8 + (n % 8 != 0), &bits, err)) != PC_OK ||
            (st = vec(p, n, &items, err)) != PC_OK) {
            return st;
        }
        for (i = 0; i < n; i++) {
            items[i].kind = PC_AF_BOOL;
            items[i].as.b = ((bits[i / 8] >> (i % 8)) & 1) != 0;
        }
        break;
    }
    case 0x2a: {
        uint16_t version;
        if ((st = count(p, 2, &n, err)) != PC_OK || (st = ax_u16(&p->c, &version, err)) != PC_OK ||
            (st = vec(p, n, &items, err)) != PC_OK) {
            return st;
        }
        for (i = 0; i < n; i++) {
            if ((st = ax_u16(&p->c, &items[i].as.e.id, err)) != PC_OK) {
                goto fail;
            }
            items[i].kind = PC_AF_ENUM;
            items[i].as.e.version = version;
        }
        break;
    }
    case 0x2b:
    case 0x2e: {
        uint32_t skip;
        if ((st = ax_u32(&p->c, &skip, err)) != PC_OK || (st = count(p, 4, &n, err)) != PC_OK ||
            (st = vec(p, n, &items, err)) != PC_OK) {
            return st;
        }
        for (i = 0; i < n; i++) {
            if ((st = string(p, &items[i], PC_AF_STR, err)) != PC_OK) {
                goto fail;
            }
        }
        break;
    }
    case 0x2c: {
        uint32_t m;
        uint16_t size;
        if ((st = ax_u32(&p->c, &m, err)) != PC_OK || (st = ax_u16(&p->c, &size, err)) != PC_OK) {
            return st;
        }
        n = m;
        if ((size_t)size * n > ax_remaining(&p->c)) {
            return ax_malformed(err, "array longer than its data");
        }
        if ((st = vec(p, n, &items, err)) != PC_OK) {
            return st;
        }
        for (i = 0; i < n; i++) {
            if ((st = bytes_val(p, size, PC_AF_BYTES, &items[i], err)) != PC_OK) {
                goto fail;
            }
        }
        break;
    }
    case 0x32: {
        uint32_t c32;
        uint16_t skip;
        if ((st = count(p, 1, &n, err)) != PC_OK || (st = ax_u32(&p->c, &c32, err)) != PC_OK ||
            (st = ax_u16(&p->c, &skip, err)) != PC_OK || (st = vec(p, n, &items, err)) != PC_OK) {
            return st;
        }
        for (i = 0; i < n; i++) {
            if ((st = inline_obj(p, true, c32, &items[i], err)) != PC_OK) {
                goto fail;
            }
        }
        break;
    }
    case 0x2d:
    case 0x33:
    case 0x75:
        return ax_malformed(err, "array of an unarrayable type");
    default:
        if ((st = count(p, min_size(code), &n, err)) != PC_OK || (st = vec(p, n, &items, err)) != PC_OK) {
            return st;
        }
        for (i = 0; i < n; i++) {
            if ((st = scalar(p, code, true, cls, &items[i], err)) != PC_OK) {
                goto fail;
            }
        }
    }
    v->kind = PC_AF_ARRAY;
    v->as.arr.v = items;
    v->as.arr.n = n;
    return PC_OK;
fail:
    while (i > 0) {
        value_fini(&items[--i]);
    }
    free(items);
    return st;
}

static pc_status fields(parser *p, bool tagged, pc_af_tag cls, pc_af_field **out, size_t *nout, pc_error *err)
{
    pc_af_field *f = NULL;
    size_t n = 0;
    size_t cap = 0;
    pc_status st;
    p->depth++;
    if (p->depth > PC_AF_MAX_DEPTH) {
        return ax_limit(err, "objects nested too deeply");
    }
    for (;;) {
        uint8_t t;
        uint8_t code;
        pc_af_tag tag = 0;
        pc_af_value v;
        if ((st = ax_u8(&p->c, &t, err)) != PC_OK) {
            goto fail;
        }
        code = t & 0x7F;
        if (code == 0) {
            break;
        }
        if ((st = spend(p, 1, err)) != PC_OK) {
            goto fail;
        }
        if (tagged && (st = ax_u32(&p->c, &tag, err)) != PC_OK) {
            goto fail;
        }
        st = (t & 0x80) != 0 ? array(p, code, cls, &v, err) : scalar(p, code, tagged, cls, &v, err);
        if (st != PC_OK) {
            goto fail;
        }
        if (n == cap) {
            size_t nc = cap > 0 ? cap * 2 : 8;
            pc_af_field *g = realloc(f, nc * sizeof *g);
            if (g == NULL) {
                value_fini(&v);
                st = ax_nomem(err);
                goto fail;
            }
            f = g;
            cap = nc;
        }
        f[n].tag = tag;
        f[n].v = v;
        n++;
    }
    p->depth--;
    *out = f;
    *nout = n;
    return PC_OK;
fail:
    fields_fini(f, n);
    return st;
}

/* Inline object; the class is given once for a whole array of them. */
static pc_status inline_obj(parser *p, bool has_cls, pc_af_tag cls, pc_af_value *v, pc_error *err)
{
    uint8_t marker;
    pc_af_object o;
    size_t id;
    pc_status st;
    memset(v, 0, sizeof *v);
    if ((st = ax_u8(&p->c, &marker, err)) != PC_OK) {
        return st;
    }
    if (marker == 0) {
        v->kind = PC_AF_OBJ;
        v->as.obj.some = false;
        return PC_OK;
    }
    if (marker != 1) {
        return ax_malformed(err, "object marker");
    }
    if (!has_cls) {
        uint16_t skip;
        if ((st = ax_u32(&p->c, &cls, err)) != PC_OK || (st = ax_u16(&p->c, &skip, err)) != PC_OK) {
            return st;
        }
    }
    memset(&o, 0, sizeof o);
    o.cls = cls;
    o.kind = PC_AF_INLINE;
    o.chain = malloc(sizeof *o.chain);
    if (o.chain == NULL) {
        return ax_nomem(err);
    }
    o.chain[0] = cls;
    o.nchain = 1;
    if ((st = fields(p, true, cls, &o.fields, &o.nfields, err)) != PC_OK) {
        free(o.chain);
        return st;
    }
    if ((st = push(p, o, &id, err)) != PC_OK) {
        object_fini(&o);
        return st;
    }
    v->kind = PC_AF_OBJ;
    v->as.obj.some = true;
    v->as.obj.id = id;
    return PC_OK;
}

static pc_status shared(parser *p, pc_af_value *v, pc_error *err)
{
    uint8_t flag;
    uint32_t key;
    size_t id;
    pc_af_object ph;
    pc_af_tag *chain = NULL;
    size_t nchain = 0;
    pc_af_field *f = NULL;
    size_t nf = 0;
    pc_af_tag cls;
    pc_status st;
    memset(v, 0, sizeof *v);
    if ((st = ax_u8(&p->c, &flag, err)) != PC_OK) {
        return st;
    }
    if (flag == 0) {
        v->kind = PC_AF_OBJ;
        v->as.obj.some = false;
        return PC_OK;
    }
    if ((st = ax_u32(&p->c, &key, err)) != PC_OK) {
        return st;
    }
    if (flag == 2) {
        if (!shared_find(p, key, &id)) {
            return ax_malformed(err, "reference to an undefined object");
        }
        v->kind = PC_AF_OBJ;
        v->as.obj.some = true;
        v->as.obj.id = id;
        return PC_OK;
    }
    if (flag != 1) {
        return ax_malformed(err, "object marker");
    }
    if (shared_find(p, key, &id)) {
        return ax_malformed(err, "object defined twice");
    }
    /* Register before the fields: an object's own fields may refer back to it. */
    memset(&ph, 0, sizeof ph);
    ph.kind = PC_AF_SHARED;
    if ((st = push(p, ph, &id, err)) != PC_OK) {
        return st;
    }
    if (!shared_insert(p, key, id)) {
        return ax_nomem(err);
    }
    chain = malloc(65 * sizeof *chain);
    if (chain == NULL) {
        return ax_nomem(err);
    }
    for (;;) {
        uint8_t m;
        if ((st = ax_u8(&p->c, &m, err)) != PC_OK) {
            goto fail;
        }
        if (m == 0) {
            pc_af_tag tag;
            uint16_t skip;
            pc_af_field *more;
            size_t nmore;
            if ((st = ax_u32(&p->c, &tag, err)) != PC_OK || (st = ax_u16(&p->c, &skip, err)) != PC_OK) {
                goto fail;
            }
            chain[nchain++] = tag;
            if ((st = fields(p, true, tag, &more, &nmore, err)) != PC_OK) {
                goto fail;
            }
            if (nmore > 0) {
                pc_af_field *g = realloc(f, (nf + nmore) * sizeof *g);
                if (g == NULL) {
                    fields_fini(more, nmore);
                    st = ax_nomem(err);
                    goto fail;
                }
                f = g;
                memcpy(f + nf, more, nmore * sizeof *more);
                nf += nmore;
            }
            free(more);
            if (nchain > 64) {
                st = ax_limit(err, "object class chain too long");
                goto fail;
            }
        } else if (m == 1) {
            if ((st = ax_u32(&p->c, &chain[nchain], err)) != PC_OK) {
                goto fail;
            }
            nchain++;
            break;
        } else if (m == 2) {
            break;
        } else {
            st = ax_malformed(err, "object class chain");
            goto fail;
        }
    }
    cls = nchain > 0 ? chain[0] : 0;
    {
        pc_af_field *more;
        size_t nmore;
        if ((st = fields(p, true, cls, &more, &nmore, err)) != PC_OK) {
            goto fail;
        }
        if (nmore > 0) {
            pc_af_field *g = realloc(f, (nf + nmore) * sizeof *g);
            if (g == NULL) {
                fields_fini(more, nmore);
                st = ax_nomem(err);
                goto fail;
            }
            f = g;
            memcpy(f + nf, more, nmore * sizeof *more);
            nf += nmore;
        }
        free(more);
    }
    p->objects[id].cls = cls;
    p->objects[id].kind = PC_AF_SHARED;
    p->objects[id].chain = chain;
    p->objects[id].nchain = nchain;
    p->objects[id].fields = f;
    p->objects[id].nfields = nf;
    v->kind = PC_AF_OBJ;
    v->as.obj.some = true;
    v->as.obj.id = id;
    return PC_OK;
fail:
    free(chain);
    fields_fini(f, nf);
    return st;
}

pc_status pc_af_stream_parse(const uint8_t *b, size_t n, pc_af_stream *out, pc_error *err)
{
    parser p;
    const uint8_t *sig;
    uint16_t version;
    uint32_t cls;
    uint16_t skip;
    pc_af_object root;
    size_t rid;
    pc_af_field *f;
    size_t nf;
    pc_status st;
    memset(&p, 0, sizeof p);
    p.c.b = b;
    p.c.n = n;
    if ((st = ax_take(&p.c, 4, &sig, err)) != PC_OK) {
        return st;
    }
    if (memcmp(sig, "\x00\xffKS", 4) != 0) {
        return ax_malformed(err, "document stream signature");
    }
    if ((st = ax_u16(&p.c, &version, err)) != PC_OK) {
        return st;
    }
    if (version > 2) {
        return ax_unsupported(err, "document stream version");
    }
    if ((st = ax_u32(&p.c, &cls, err)) != PC_OK || (st = ax_u16(&p.c, &skip, err)) != PC_OK) {
        return st;
    }
    if (version == 2) {
        uint32_t x;
        if ((st = ax_u32(&p.c, &x, err)) != PC_OK) {
            return st;
        }
    }
    memset(&root, 0, sizeof root);
    root.cls = cls;
    root.kind = PC_AF_ROOT;
    root.chain = malloc(sizeof *root.chain);
    if (root.chain == NULL) {
        return ax_nomem(err);
    }
    root.chain[0] = cls;
    root.nchain = 1;
    if ((st = push(&p, root, &rid, err)) != PC_OK) {
        free(root.chain);
        goto fail;
    }
    if ((st = fields(&p, true, cls, &f, &nf, err)) != PC_OK) {
        goto fail;
    }
    p.objects[rid].fields = f;
    p.objects[rid].nfields = nf;
    free(p.keys);
    free(p.vals);
    free(p.used);
    out->root = rid;
    out->objects = p.objects;
    out->nobjects = p.nobjects;
    return PC_OK;
fail:
    {
        pc_af_stream s;
        s.objects = p.objects;
        s.nobjects = p.nobjects;
        s.root = 0;
        pc_af_stream_fini(&s);
    }
    free(p.keys);
    free(p.vals);
    free(p.used);
    return st;
}
