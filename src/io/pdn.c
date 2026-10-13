/* Native, import-only Paint.NET (.pdn, "PDN3") reader (port of crates/io/src/pdn.rs and
 * crates/io/src/pdn/nrbf.rs, R10).
 *
 * A PDN3 file is: the magic "PDN3", a 3-byte little-endian header length and that many bytes of
 * XML header, two marker bytes (0x00, 0x01), a .NET Remoting Binary Format (MS-NRBF) object graph,
 * and then, after the NRBF MessageEnd record, a sequence of per-MemoryBlock pixel chunk streams
 * (each gzip-compressed, or stored raw).
 *
 * The NRBF parser below is a data-only subset: object references stay as ids, no .NET type is
 * executed. Everything here treats the file as hostile: graph item count, string bytes, nesting
 * depth, layer count and decoded pixel bytes are all capped (the same constants as the Rust
 * reference), size arithmetic is overflow-checked, and nothing panics or aborts on bad input.
 * Errors are "PDN: <reason>" (pc_io_pdn_error). Thread-safe (no shared state). */
#include "io/pdn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/compress.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "color/blend.h"
#include "color/color.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* ---- limits (match crates/io/src/pdn.rs and pdn/nrbf.rs) ---- */

#define MAX_PIXEL_BYTES ((size_t)1 << 30) /* 1 GiB of decoded pixels */
#define MAX_LAYERS 1024u
#define MAX_ITEMS ((size_t)1000000)     /* serialized objects + members */
#define MAX_STRINGS ((size_t)64 << 20)  /* total serialized string bytes */
#define MAX_DEPTH 64u                   /* NRBF record nesting */
#define MAX_CHUNKS ((size_t)1000000)    /* pixel chunks in one memory block */

#define STR_EQ(p, l, lit) ((l) == (sizeof(lit) - 1) && memcmp((p), (lit), (l)) == 0)

/* ---- small open-addressing map: int32 id -> array index ---- */

typedef struct {
    int32_t key;
    size_t val;
    bool used;
} idmap_slot;

typedef struct {
    idmap_slot *slots;
    size_t cap;
    size_t count;
} idmap;

static void idmap_init(idmap *m)
{
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
}

static void idmap_fini(idmap *m)
{
    free(m->slots);
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
}

static size_t idmap_index(int32_t key, size_t cap)
{
    uint32_t h = (uint32_t)key * 2654435761u;
    return (size_t)h & (cap - 1);
}

static bool idmap_get(const idmap *m, int32_t key, size_t *out)
{
    size_t i;
    if (m->cap == 0) {
        return false;
    }
    i = idmap_index(key, m->cap);
    for (;;) {
        const idmap_slot *s = &m->slots[i];
        if (!s->used) {
            return false;
        }
        if (s->key == key) {
            *out = s->val;
            return true;
        }
        i = (i + 1) & (m->cap - 1);
    }
}

static pc_status idmap_grow(idmap *m, pc_error *err)
{
    size_t ncap = m->cap == 0 ? 16 : m->cap * 2;
    idmap_slot *ns = pc_calloc_array(ncap, sizeof *ns);
    size_t i;
    if (ns == NULL) {
        return pc_io_nomem(err, "parsing a Paint.NET document");
    }
    for (i = 0; i < m->cap; i++) {
        if (m->slots[i].used) {
            size_t j = idmap_index(m->slots[i].key, ncap);
            while (ns[j].used) {
                j = (j + 1) & (ncap - 1);
            }
            ns[j] = m->slots[i];
        }
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
    return PC_OK;
}

/* Inserts (key -> val). *existed is set true (and nothing inserted) when key is already present. */
static pc_status idmap_insert(idmap *m, int32_t key, size_t val, bool *existed, pc_error *err)
{
    size_t tmp;
    size_t i;
    if (idmap_get(m, key, &tmp)) {
        *existed = true;
        return PC_OK;
    }
    *existed = false;
    if ((m->count + 1) * 2 > m->cap) {
        pc_status st = idmap_grow(m, err);
        if (st != PC_OK) {
            return st;
        }
    }
    i = idmap_index(key, m->cap);
    while (m->slots[i].used) {
        i = (i + 1) & (m->cap - 1);
    }
    m->slots[i].key = key;
    m->slots[i].val = val;
    m->slots[i].used = true;
    m->count++;
    return PC_OK;
}

/* ---- bounds-checked reader over the whole file (shared by NRBF parse and pixel decode) ---- */

typedef struct {
    const uint8_t *bytes;
    size_t len;
    size_t pos;
} reader;

static pc_status rd_take(reader *r, size_t n, const uint8_t **out, pc_error *err)
{
    size_t end;
    if (!pc_size_add(r->pos, n, &end)) {
        return pc_io_pdn_error(err, "length overflow");
    }
    if (end > r->len) {
        return pc_io_pdn_error(err, "truncated document");
    }
    *out = r->bytes + r->pos;
    r->pos = end;
    return PC_OK;
}

static pc_status rd_byte(reader *r, uint8_t *out, pc_error *err)
{
    const uint8_t *b;
    pc_status st = rd_take(r, 1, &b, err);
    if (st != PC_OK) {
        return st;
    }
    *out = b[0];
    return PC_OK;
}

static pc_status rd_le_i32(reader *r, int32_t *out, pc_error *err)
{
    const uint8_t *b;
    pc_status st = rd_take(r, 4, &b, err);
    if (st != PC_OK) {
        return st;
    }
    *out = pc_i32_from_bits(pc_load_le32(b));
    return PC_OK;
}

static pc_status rd_be_u32(reader *r, size_t *out, pc_error *err)
{
    const uint8_t *b;
    pc_status st = rd_take(r, 4, &b, err);
    if (st != PC_OK) {
        return st;
    }
    *out = (size_t)pc_load_be32(b);
    return PC_OK;
}

/* le_i32 read as a non-negative, bounded length. */
static pc_status rd_count(reader *r, size_t *out, pc_error *err)
{
    int32_t n;
    pc_status st = rd_le_i32(r, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n < 0) {
        return pc_io_pdn_error(err, "negative count");
    }
    if ((size_t)n > MAX_ITEMS) {
        return pc_io_pdn_error(err, "serialized array or class is too large");
    }
    *out = (size_t)n;
    return PC_OK;
}

/* ---- the NRBF object graph ---- */

typedef struct {
    char *name;
    size_t name_len;
    char **mnames;      /* member names */
    size_t *mname_lens;
    int16_t *prims;     /* per member: -1 = a reference/object, else the primitive tag (0..255) */
    size_t nmembers;
} nclass;

typedef enum { V_NULL, V_BOOL, V_INT, V_STRING, V_REF, V_ARRAY, V_OBJECT } vkind;

typedef struct value value;
struct value {
    vkind kind;
    union {
        bool b;
        int64_t i;
        struct {
            char *p;
            size_t len;
        } s;
        int32_t ref;
        struct {
            value *items;
            size_t n;
        } arr;
        struct {
            const nclass *cls;
            value *members;
            size_t n;
        } obj;
    } u;
};

/* Array items and object members are only leaves or references, so this recurses at most one level
 * (no inline arrays or objects); it stays shallow however hostile the file is. */
static void value_fini(value *v)
{
    size_t i;
    switch (v->kind) {
    case V_STRING:
        free(v->u.s.p);
        break;
    case V_ARRAY:
        for (i = 0; i < v->u.arr.n; i++) {
            value_fini(&v->u.arr.items[i]);
        }
        free(v->u.arr.items);
        break;
    case V_OBJECT:
        for (i = 0; i < v->u.obj.n; i++) {
            value_fini(&v->u.obj.members[i]);
        }
        free(v->u.obj.members);
        break;
    default:
        break;
    }
    v->kind = V_NULL;
}

typedef struct {
    value root;        /* a V_REF to the root object */
    value *objects;    /* in serialized order (the reference's `order`) */
    int32_t *ids;      /* the id of objects[i] */
    size_t nobjects;
    size_t cap_objects;
    idmap obj_map;     /* id -> index into objects */
    nclass **classes;  /* owned; stable pointers (held by objects too) */
    size_t nclasses;
    size_t cap_classes;
} graph;

static void graph_init(graph *g)
{
    g->root.kind = V_NULL;
    g->objects = NULL;
    g->ids = NULL;
    g->nobjects = 0;
    g->cap_objects = 0;
    idmap_init(&g->obj_map);
    g->classes = NULL;
    g->nclasses = 0;
    g->cap_classes = 0;
}

static void graph_fini(graph *g)
{
    size_t i;
    size_t j;
    for (i = 0; i < g->nobjects; i++) {
        value_fini(&g->objects[i]);
    }
    free(g->objects);
    free(g->ids);
    idmap_fini(&g->obj_map);
    for (i = 0; i < g->nclasses; i++) {
        nclass *c = g->classes[i];
        if (c != NULL) {
            free(c->name);
            for (j = 0; j < c->nmembers; j++) {
                free(c->mnames[j]);
            }
            free(c->mnames);
            free(c->mname_lens);
            free(c->prims);
            free(c);
        }
    }
    free(g->classes);
}

/* Reserves a fresh object slot (Null placeholder) for id, returning its index; rejects id 0 or a
 * duplicate id, exactly like the reference's `reserve`. */
static pc_status graph_reserve(graph *g, int32_t id, size_t *idx, pc_error *err)
{
    size_t tmp;
    bool existed;
    pc_status st;
    if (id == 0 || idmap_get(&g->obj_map, id, &tmp)) {
        return pc_io_pdn_error(err, "duplicate or zero object id");
    }
    if (g->nobjects == g->cap_objects) {
        size_t ncap = g->cap_objects != 0 ? g->cap_objects * 2 : 16;
        value *no = pc_malloc_array(ncap, sizeof *no);
        int32_t *ni = pc_malloc_array(ncap, sizeof *ni);
        if (no == NULL || ni == NULL) {
            free(no);
            free(ni);
            return pc_io_nomem(err, "parsing a Paint.NET document");
        }
        if (g->nobjects != 0) {
            memcpy(no, g->objects, g->nobjects * sizeof *no);
            memcpy(ni, g->ids, g->nobjects * sizeof *ni);
        }
        free(g->objects);
        free(g->ids);
        g->objects = no;
        g->ids = ni;
        g->cap_objects = ncap;
    }
    st = idmap_insert(&g->obj_map, id, g->nobjects, &existed, err);
    if (st != PC_OK) {
        return st;
    }
    g->objects[g->nobjects].kind = V_NULL;
    g->ids[g->nobjects] = id;
    *idx = g->nobjects;
    g->nobjects++;
    return PC_OK;
}

static pc_status graph_add_class(graph *g, nclass *c, pc_error *err)
{
    if (g->nclasses == g->cap_classes) {
        void *nc = pc_grow_array(g->classes, &g->cap_classes, g->nclasses + 1, sizeof *g->classes);
        if (nc == NULL) {
            return pc_io_nomem(err, "parsing a Paint.NET document");
        }
        g->classes = nc;
    }
    g->classes[g->nclasses++] = c;
    return PC_OK;
}

/* ---- graph accessors (port of the Graph methods) ---- */

static const value *g_resolve(const graph *g, const value *v, pc_error *err)
{
    unsigned d;
    for (d = 0; d < MAX_DEPTH; d++) {
        if (v->kind == V_REF) {
            size_t idx;
            if (!idmap_get(&g->obj_map, v->u.ref, &idx)) {
                (void)pc_io_pdn_error(err, "missing object reference");
                return NULL;
            }
            v = &g->objects[idx];
        } else {
            return v;
        }
    }
    (void)pc_io_pdn_error(err, "cyclic or excessively deep object reference");
    return NULL;
}

static pc_status g_optional_field(const graph *g, const value *v, const char *name, const value **out,
                                  pc_error *err)
{
    const value *r = g_resolve(g, v, err);
    size_t nl;
    size_t i;
    if (r == NULL) {
        return PC_ERR_FORMAT;
    }
    if (r->kind != V_OBJECT) {
        return pc_io_pdn_error(err, "expected a serialized object");
    }
    nl = strlen(name);
    for (i = 0; i < r->u.obj.cls->nmembers; i++) {
        if (r->u.obj.cls->mname_lens[i] == nl && memcmp(r->u.obj.cls->mnames[i], name, nl) == 0) {
            *out = i < r->u.obj.n ? &r->u.obj.members[i] : NULL;
            return PC_OK;
        }
    }
    *out = NULL;
    return PC_OK;
}

static pc_status g_field(const graph *g, const value *v, const char *name, const value **out, pc_error *err)
{
    const value *f;
    pc_status st = g_optional_field(g, v, name, &f, err);
    if (st != PC_OK) {
        return st;
    }
    if (f == NULL) {
        return pc_io_pdn_error(err, "missing field %s", name);
    }
    *out = f;
    return PC_OK;
}

static pc_status g_class_name(const graph *g, const value *v, const char **out, size_t *out_len, pc_error *err)
{
    const value *r = g_resolve(g, v, err);
    if (r == NULL) {
        return PC_ERR_FORMAT;
    }
    if (r->kind != V_OBJECT) {
        return pc_io_pdn_error(err, "expected a serialized object");
    }
    *out = r->u.obj.cls->name;
    *out_len = r->u.obj.cls->name_len;
    return PC_OK;
}

static pc_status g_integer(const graph *g, const value *v, int64_t *out, pc_error *err)
{
    const value *r = g_resolve(g, v, err);
    if (r == NULL) {
        return PC_ERR_FORMAT;
    }
    if (r->kind != V_INT) {
        return pc_io_pdn_error(err, "expected an integer");
    }
    *out = r->u.i;
    return PC_OK;
}

static pc_status g_boolean(const graph *g, const value *v, bool *out, pc_error *err)
{
    const value *r = g_resolve(g, v, err);
    if (r == NULL) {
        return PC_ERR_FORMAT;
    }
    if (r->kind != V_BOOL) {
        return pc_io_pdn_error(err, "expected a boolean");
    }
    *out = r->u.b;
    return PC_OK;
}

static pc_status g_string(const graph *g, const value *v, const char **out, size_t *out_len, pc_error *err)
{
    const value *r = g_resolve(g, v, err);
    if (r == NULL) {
        return PC_ERR_FORMAT;
    }
    if (r->kind != V_STRING) {
        return pc_io_pdn_error(err, "expected a string");
    }
    *out = r->u.s.p;
    *out_len = r->u.s.len;
    return PC_OK;
}

static pc_status g_array(const graph *g, const value *v, const value **items, size_t *n, pc_error *err)
{
    const value *r = g_resolve(g, v, err);
    if (r == NULL) {
        return PC_ERR_FORMAT;
    }
    if (r->kind != V_ARRAY) {
        return pc_io_pdn_error(err, "expected an array");
    }
    *items = r->u.arr.items;
    *n = r->u.arr.n;
    return PC_OK;
}

/* Reads object.field as a non-negative, in-range count (the reference's `count` free function). */
static pc_status g_count(const graph *g, const value *obj, const char *field, size_t *out, pc_error *err)
{
    const value *f;
    int64_t n;
    pc_status st = g_field(g, obj, field, &f, err);
    if (st != PC_OK) {
        return st;
    }
    st = g_integer(g, f, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n < 0 || (uint64_t)n > (uint64_t)SIZE_MAX) {
        return pc_io_pdn_error(err, "invalid %s", field);
    }
    *out = (size_t)n;
    return PC_OK;
}

static pc_status gf_bool(const graph *g, const value *obj, const char *field, bool *out, pc_error *err)
{
    const value *f;
    pc_status st = g_field(g, obj, field, &f, err);
    if (st != PC_OK) {
        return st;
    }
    return g_boolean(g, f, out, err);
}

/* ---- blend-mode mapping (PDN mode index / legacy BlendOp class -> pc_blend_mode) ---- */

static const pc_blend_mode MODES[14] = {
    PC_BLEND_NORMAL,        PC_BLEND_MULTIPLY,          PC_BLEND_LINEAR_DODGE,
    PC_BLEND_PAINT_NET_COLOR_BURN, PC_BLEND_PAINT_NET_COLOR_DODGE, PC_BLEND_REFLECT,
    PC_BLEND_GLOW,          PC_BLEND_OVERLAY,           PC_BLEND_DIFFERENCE,
    PC_BLEND_NEGATION,      PC_BLEND_LIGHTEN,           PC_BLEND_DARKEN,
    PC_BLEND_SCREEN,        PC_BLEND_XOR};

static const char *const OLD_MODES[14] = {"Normal",     "Multiply", "Additive", "ColorBurn", "ColorDodge",
                                          "Reflect",    "Glow",     "Overlay",  "Difference", "Negation",
                                          "Lighten",    "Darken",   "Screen",   "Xor"};

static pc_status g_blend(const graph *g, const value *layer, const value *props, pc_blend_mode *out,
                         pc_error *err)
{
    const value *mode;
    const value *props2;
    const value *blend_op;
    const char *cname;
    size_t cnl;
    const char *pre = "PaintDotNet.UserBlendOps+";
    const char *suf = "BlendOp";
    size_t prel;
    size_t sufl;
    size_t midlen;
    const char *mid;
    size_t i;
    pc_status st = g_optional_field(g, props, "blendMode", &mode, err);
    if (st != PC_OK) {
        return st;
    }
    if (mode != NULL) {
        const value *r = g_resolve(g, mode, err);
        int64_t m;
        if (r == NULL) {
            return PC_ERR_FORMAT;
        }
        if (r->kind == V_INT) {
            m = r->u.i;
        } else {
            const value *f;
            st = g_field(g, mode, "value__", &f, err);
            if (st != PC_OK) {
                return st;
            }
            st = g_integer(g, f, &m, err);
            if (st != PC_OK) {
                return st;
            }
        }
        if (m < 0) {
            return pc_io_pdn_error(err, "invalid blend mode");
        }
        if ((uint64_t)m >= 14) {
            return pc_io_pdn_error(err, "unsupported blend mode %zu", (size_t)m);
        }
        *out = MODES[(size_t)m];
        return PC_OK;
    }
    st = g_field(g, layer, "properties", &props2, err);
    if (st != PC_OK) {
        return st;
    }
    st = g_field(g, props2, "blendOp", &blend_op, err);
    if (st != PC_OK) {
        return st;
    }
    st = g_class_name(g, blend_op, &cname, &cnl, err);
    if (st != PC_OK) {
        return st;
    }
    prel = strlen(pre);
    sufl = strlen(suf);
    if (cnl < prel + sufl || memcmp(cname, pre, prel) != 0 || memcmp(cname + cnl - sufl, suf, sufl) != 0) {
        int plen = cnl > 120 ? 120 : (int)cnl;
        return pc_io_pdn_error(err, "unsupported blend operation %.*s", plen, cname);
    }
    mid = cname + prel;
    midlen = cnl - prel - sufl;
    for (i = 0; i < 14; i++) {
        size_t ol = strlen(OLD_MODES[i]);
        if (ol == midlen && memcmp(mid, OLD_MODES[i], ol) == 0) {
            *out = MODES[i];
            return PC_OK;
        }
    }
    {
        int plen = midlen > 120 ? 120 : (int)midlen;
        return pc_io_pdn_error(err, "unsupported blend operation %.*s", plen, mid);
    }
}

/* ---- the NRBF parser (port of nrbf::Parser) ---- */

typedef struct {
    reader *r;
    const pc_interrupt *ctl;
    graph *g;
    idmap class_map; /* metadata id -> index into g->classes */
    size_t items;
    size_t strings;
} parser;

static pc_status p_record(parser *p, uint8_t tag, size_t depth, value *out, pc_error *err);
static pc_status p_value(parser *p, size_t depth, value *out, pc_error *err);
static pc_status p_primitive(parser *p, uint8_t tag, value *out, pc_error *err);
static pc_status p_array(parser *p, int32_t id, size_t len, int16_t prim, size_t depth, value *out,
                         pc_error *err);
static pc_status p_object(parser *p, int32_t id, const nclass *cls, size_t depth, value *out, pc_error *err);
static pc_status p_classdef(parser *p, uint8_t tag, size_t depth, value *out, pc_error *err);
static pc_status p_type_info(parser *p, uint8_t tag, int16_t *out, pc_error *err);
static pc_status p_string(parser *p, char **out, size_t *out_len, pc_error *err);

static pc_status p_check(parser *p, pc_error *err)
{
    return pc_interrupt_check(p->ctl, err);
}

static pc_status p_budget(parser *p, size_t n, pc_error *err)
{
    if (!pc_size_add(p->items, n, &p->items)) {
        return pc_io_pdn_error(err, "object count overflow");
    }
    if (p->items > MAX_ITEMS) {
        return pc_io_pdn_error(err, "too many serialized objects or members");
    }
    return PC_OK;
}

static bool valid_utf8(const uint8_t *s, size_t n)
{
    size_t i = 0;
    uint32_t cp;
    while (i < n) {
        if (!pc_utf8_next(s, n, &i, &cp)) {
            return false;
        }
    }
    return true;
}

static void str_to_value(value *out, char *p, size_t len)
{
    out->kind = V_STRING;
    out->u.s.p = p;
    out->u.s.len = len;
}

/* A length-prefixed (7-bit varint) UTF-8 string; the result is malloc'd and NUL-terminated. */
static pc_status p_string(parser *p, char **out, size_t *out_len, pc_error *err)
{
    size_t len = 0;
    int shift;
    for (shift = 0; shift < 35; shift += 7) {
        uint8_t b;
        pc_status st = rd_byte(p->r, &b, err);
        if (st != PC_OK) {
            return st;
        }
        if (shift == 28 && b > 7) {
            return pc_io_pdn_error(err, "invalid string length");
        }
        len |= (size_t)(b & 127u) << shift;
        if ((b & 128u) == 0) {
            const uint8_t *data;
            char *buf;
            if (!pc_size_add(p->strings, len, &p->strings)) {
                return pc_io_pdn_error(err, "string length overflow");
            }
            if (p->strings > MAX_STRINGS) {
                return pc_io_pdn_error(err, "serialized strings are too large");
            }
            st = rd_take(p->r, len, &data, err);
            if (st != PC_OK) {
                return st;
            }
            if (!valid_utf8(data, len)) {
                return pc_io_pdn_error(err, "invalid UTF-8 string");
            }
            buf = pc_malloc(len + 1);
            if (buf == NULL) {
                return pc_io_nomem(err, "parsing a Paint.NET document");
            }
            if (len != 0) {
                memcpy(buf, data, len);
            }
            buf[len] = '\0';
            *out = buf;
            *out_len = len;
            return PC_OK;
        }
    }
    return pc_io_pdn_error(err, "invalid string length");
}

static pc_status p_primitive(parser *p, uint8_t tag, value *out, pc_error *err)
{
    pc_status st;
    const uint8_t *b;
    switch (tag) {
    case 1: {
        uint8_t v;
        st = rd_byte(p->r, &v, err);
        if (st != PC_OK) {
            return st;
        }
        if (v == 0) {
            out->kind = V_BOOL;
            out->u.b = false;
        } else if (v == 1) {
            out->kind = V_BOOL;
            out->u.b = true;
        } else {
            return pc_io_pdn_error(err, "invalid boolean");
        }
        return PC_OK;
    }
    case 2: {
        uint8_t v;
        st = rd_byte(p->r, &v, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_INT;
        out->u.i = (int64_t)v;
        return PC_OK;
    }
    case 3: {
        uint8_t first;
        size_t extra;
        uint8_t chbuf[4];
        const uint8_t *rest;
        st = rd_byte(p->r, &first, err);
        if (st != PC_OK) {
            return st;
        }
        if (first <= 0x7f) {
            extra = 0;
        } else if (first >= 0xc2 && first <= 0xdf) {
            extra = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            extra = 2;
        } else {
            return pc_io_pdn_error(err, "invalid character");
        }
        chbuf[0] = first;
        st = rd_take(p->r, extra, &rest, err);
        if (st != PC_OK) {
            return st;
        }
        if (extra != 0) {
            memcpy(chbuf + 1, rest, extra);
        }
        if (!valid_utf8(chbuf, 1 + extra)) {
            return pc_io_pdn_error(err, "invalid character");
        }
        {
            char *buf = pc_malloc(2 + extra);
            if (buf == NULL) {
                return pc_io_nomem(err, "parsing a Paint.NET document");
            }
            memcpy(buf, chbuf, 1 + extra);
            buf[1 + extra] = '\0';
            str_to_value(out, buf, 1 + extra);
        }
        return PC_OK;
    }
    case 5:
    case 18: {
        char *s;
        size_t l;
        st = p_string(p, &s, &l, err);
        if (st != PC_OK) {
            return st;
        }
        str_to_value(out, s, l);
        return PC_OK;
    }
    case 6:
        st = rd_take(p->r, 8, &b, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_NULL;
        return PC_OK;
    case 7:
        st = rd_take(p->r, 2, &b, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_INT;
        out->u.i = (int64_t)pc_i16_from_bits(pc_load_le16(b));
        return PC_OK;
    case 8:
        st = rd_take(p->r, 4, &b, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_INT;
        out->u.i = (int64_t)pc_i32_from_bits(pc_load_le32(b));
        return PC_OK;
    case 9:
    case 12:
        st = rd_take(p->r, 8, &b, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_INT;
        out->u.i = pc_i64_from_bits(pc_load_le64(b));
        return PC_OK;
    case 10: {
        uint8_t v;
        st = rd_byte(p->r, &v, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_INT;
        out->u.i = (v & 0x80u) != 0 ? (int64_t)v - 256 : (int64_t)v;
        return PC_OK;
    }
    case 11:
        st = rd_take(p->r, 4, &b, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_NULL;
        return PC_OK;
    case 13:
    case 16:
        /* The metadata date's high bits are kept as the raw 64-bit pattern. */
        st = rd_take(p->r, 8, &b, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_INT;
        out->u.i = pc_i64_from_bits(pc_load_le64(b));
        return PC_OK;
    case 14:
        st = rd_take(p->r, 2, &b, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_INT;
        out->u.i = (int64_t)pc_load_le16(b);
        return PC_OK;
    case 15:
        st = rd_take(p->r, 4, &b, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_INT;
        out->u.i = (int64_t)pc_load_le32(b);
        return PC_OK;
    case 17:
        out->kind = V_NULL;
        return PC_OK;
    default:
        return pc_io_pdn_error(err, "unsupported primitive type %u", (unsigned)tag);
    }
}

static pc_status p_type_info(parser *p, uint8_t tag, int16_t *out, pc_error *err)
{
    pc_status st;
    switch (tag) {
    case 0: {
        uint8_t b;
        st = rd_byte(p->r, &b, err);
        if (st != PC_OK) {
            return st;
        }
        *out = (int16_t)b;
        return PC_OK;
    }
    case 1:
    case 2:
    case 5:
    case 6:
        *out = -1;
        return PC_OK;
    case 3: {
        char *s;
        size_t l;
        st = p_string(p, &s, &l, err);
        if (st != PC_OK) {
            return st;
        }
        free(s);
        *out = -1;
        return PC_OK;
    }
    case 4: {
        char *s;
        size_t l;
        int32_t d;
        st = p_string(p, &s, &l, err);
        if (st != PC_OK) {
            return st;
        }
        free(s);
        st = rd_le_i32(p->r, &d, err);
        if (st != PC_OK) {
            return st;
        }
        *out = -1;
        return PC_OK;
    }
    case 7: {
        uint8_t b;
        st = rd_byte(p->r, &b, err);
        if (st != PC_OK) {
            return st;
        }
        *out = -1;
        return PC_OK;
    }
    default:
        return pc_io_pdn_error(err, "unsupported binary type %u", (unsigned)tag);
    }
}

static pc_status p_object(parser *p, int32_t id, const nclass *cls, size_t depth, value *out, pc_error *err)
{
    size_t idx;
    value *members = NULL;
    size_t i;
    pc_status st = graph_reserve(p->g, id, &idx, err);
    if (st != PC_OK) {
        return st;
    }
    st = p_budget(p, cls->nmembers, err);
    if (st != PC_OK) {
        return st;
    }
    if (cls->nmembers != 0) {
        members = pc_malloc_array(cls->nmembers, sizeof *members);
        if (members == NULL) {
            return pc_io_nomem(err, "parsing a Paint.NET document");
        }
        for (i = 0; i < cls->nmembers; i++) {
            members[i].kind = V_NULL;
        }
    }
    for (i = 0; i < cls->nmembers; i++) {
        value mv;
        mv.kind = V_NULL;
        if (cls->prims[i] >= 0) {
            st = p_primitive(p, (uint8_t)cls->prims[i], &mv, err);
        } else {
            st = p_value(p, depth + 1, &mv, err);
        }
        if (st != PC_OK) {
            size_t k;
            for (k = 0; k < i; k++) {
                value_fini(&members[k]);
            }
            free(members);
            return st;
        }
        members[i] = mv;
    }
    /* The reserve placeholder is still V_NULL; nothing to free before overwriting it. */
    p->g->objects[idx].kind = V_OBJECT;
    p->g->objects[idx].u.obj.cls = cls;
    p->g->objects[idx].u.obj.members = members;
    p->g->objects[idx].u.obj.n = cls->nmembers;
    out->kind = V_REF;
    out->u.ref = id;
    return PC_OK;
}

/* A member value: skips any inline BinaryLibrary (record 12) records. */
static pc_status p_value(parser *p, size_t depth, value *out, pc_error *err)
{
    for (;;) {
        uint8_t tag;
        pc_status st = rd_byte(p->r, &tag, err);
        if (st != PC_OK) {
            return st;
        }
        if (tag == 12) {
            value tmp;
            tmp.kind = V_NULL;
            st = p_record(p, 12, depth, &tmp, err);
            if (st != PC_OK) {
                return st;
            }
            value_fini(&tmp);
            continue;
        }
        return p_record(p, tag, depth, out, err);
    }
}

static pc_status arr_push(value **items, size_t *cap, size_t *n, value v, pc_error *err)
{
    if (*n == *cap) {
        void *ni = pc_grow_array(*items, cap, *n + 1, sizeof(value));
        if (ni == NULL) {
            return pc_io_nomem(err, "parsing a Paint.NET document");
        }
        *items = ni;
    }
    (*items)[*n] = v;
    (*n)++;
    return PC_OK;
}

static pc_status p_array(parser *p, int32_t id, size_t len, int16_t prim, size_t depth, value *out,
                         pc_error *err)
{
    size_t idx;
    value *items = NULL;
    size_t cap = 0;
    size_t n = 0;
    pc_status st = graph_reserve(p->g, id, &idx, err);
    if (st != PC_OK) {
        return st;
    }
    st = p_budget(p, len, err);
    if (st != PC_OK) {
        return st;
    }
    if (prim == 2) {
        /* A byte array: its bytes are skipped and the id resolves to the Null placeholder. */
        const uint8_t *tmp;
        st = rd_take(p->r, len, &tmp, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_REF;
        out->u.ref = id;
        return PC_OK;
    }
    while (n < len) {
        uint8_t tag;
        st = p_check(p, err);
        if (st != PC_OK) {
            goto fail;
        }
        if (prim >= 0) {
            value mv;
            mv.kind = V_NULL;
            st = p_primitive(p, (uint8_t)prim, &mv, err);
            if (st != PC_OK) {
                goto fail;
            }
            st = arr_push(&items, &cap, &n, mv, err);
            if (st != PC_OK) {
                value_fini(&mv);
                goto fail;
            }
            continue;
        }
        st = rd_byte(p->r, &tag, err);
        if (st != PC_OK) {
            goto fail;
        }
        if (tag == 13 || tag == 14) {
            size_t cnt;
            size_t k;
            if (tag == 13) {
                uint8_t bb;
                st = rd_byte(p->r, &bb, err);
                if (st != PC_OK) {
                    goto fail;
                }
                cnt = bb;
            } else {
                st = rd_count(p->r, &cnt, err);
                if (st != PC_OK) {
                    goto fail;
                }
            }
            if (cnt == 0 || cnt > len - n) {
                st = pc_io_pdn_error(err, "invalid array null run");
                goto fail;
            }
            for (k = 0; k < cnt; k++) {
                value nv;
                nv.kind = V_NULL;
                st = arr_push(&items, &cap, &n, nv, err);
                if (st != PC_OK) {
                    goto fail;
                }
            }
        } else if (tag == 12) {
            value tmp;
            tmp.kind = V_NULL;
            st = p_record(p, 12, depth + 1, &tmp, err);
            if (st != PC_OK) {
                goto fail;
            }
            value_fini(&tmp);
        } else {
            value mv;
            mv.kind = V_NULL;
            st = p_record(p, tag, depth + 1, &mv, err);
            if (st != PC_OK) {
                goto fail;
            }
            st = arr_push(&items, &cap, &n, mv, err);
            if (st != PC_OK) {
                value_fini(&mv);
                goto fail;
            }
        }
    }
    p->g->objects[idx].kind = V_ARRAY;
    p->g->objects[idx].u.arr.items = items;
    p->g->objects[idx].u.arr.n = n;
    out->kind = V_REF;
    out->u.ref = id;
    return PC_OK;
fail: {
    size_t k;
    for (k = 0; k < n; k++) {
        value_fini(&items[k]);
    }
    free(items);
    return st;
}
}

static pc_status p_classdef(parser *p, uint8_t tag, size_t depth, value *out, pc_error *err)
{
    int32_t id;
    char *cname = NULL;
    size_t cnamel = 0;
    size_t count = 0;
    char **mnames = NULL;
    size_t *mlens = NULL;
    int16_t *prims = NULL;
    nclass *c;
    size_t i;
    size_t tmp;
    bool existed;
    pc_status st = rd_le_i32(p->r, &id, err);
    if (st != PC_OK) {
        return st;
    }
    st = p_string(p, &cname, &cnamel, err);
    if (st != PC_OK) {
        return st;
    }
    st = rd_count(p->r, &count, err);
    if (st != PC_OK) {
        free(cname);
        return st;
    }
    st = p_budget(p, count, err);
    if (st != PC_OK) {
        free(cname);
        return st;
    }
    if (count != 0) {
        mnames = pc_calloc_array(count, sizeof *mnames);
        mlens = pc_malloc_array(count, sizeof *mlens);
        prims = pc_malloc_array(count, sizeof *prims);
        if (mnames == NULL || mlens == NULL || prims == NULL) {
            st = pc_io_nomem(err, "parsing a Paint.NET document");
            goto fail;
        }
    }
    for (i = 0; i < count; i++) {
        st = p_string(p, &mnames[i], &mlens[i], err);
        if (st != PC_OK) {
            goto fail;
        }
    }
    if (tag >= 4) {
        const uint8_t *types;
        st = rd_take(p->r, count, &types, err);
        if (st != PC_OK) {
            goto fail;
        }
        for (i = 0; i < count; i++) {
            st = p_type_info(p, types[i], &prims[i], err);
            if (st != PC_OK) {
                goto fail;
            }
        }
    } else {
        for (i = 0; i < count; i++) {
            prims[i] = -1;
        }
    }
    if (tag == 3 || tag == 5) {
        int32_t dummy;
        st = rd_le_i32(p->r, &dummy, err);
        if (st != PC_OK) {
            goto fail;
        }
    }
    if (idmap_get(&p->class_map, id, &tmp)) {
        st = pc_io_pdn_error(err, "duplicate class metadata");
        goto fail;
    }
    c = pc_malloc(sizeof *c);
    if (c == NULL) {
        st = pc_io_nomem(err, "parsing a Paint.NET document");
        goto fail;
    }
    c->name = cname;
    c->name_len = cnamel;
    c->mnames = mnames;
    c->mname_lens = mlens;
    c->prims = prims;
    c->nmembers = count;
    st = graph_add_class(p->g, c, err);
    if (st != PC_OK) {
        free(c);
        goto fail;
    }
    /* c (and its buffers) are now owned by the graph and freed in graph_fini. */
    st = idmap_insert(&p->class_map, id, p->g->nclasses - 1, &existed, err);
    if (st != PC_OK) {
        return st;
    }
    return p_object(p, id, c, depth, out, err);
fail:
    free(cname);
    if (mnames != NULL) {
        for (i = 0; i < count; i++) {
            free(mnames[i]);
        }
        free(mnames);
    }
    free(mlens);
    free(prims);
    return st;
}

static pc_status p_record(parser *p, uint8_t tag, size_t depth, value *out, pc_error *err)
{
    pc_status st = p_check(p, err);
    if (st != PC_OK) {
        return st;
    }
    st = p_budget(p, 1, err);
    if (st != PC_OK) {
        return st;
    }
    if (depth > MAX_DEPTH) {
        return pc_io_pdn_error(err, "serialized objects nested too deeply");
    }
    switch (tag) {
    case 1: {
        int32_t id;
        int32_t meta;
        size_t ci;
        st = rd_le_i32(p->r, &id, err);
        if (st != PC_OK) {
            return st;
        }
        st = rd_le_i32(p->r, &meta, err);
        if (st != PC_OK) {
            return st;
        }
        if (!idmap_get(&p->class_map, meta, &ci)) {
            return pc_io_pdn_error(err, "missing class metadata");
        }
        return p_object(p, id, p->g->classes[ci], depth, out, err);
    }
    case 2:
    case 3:
    case 4:
    case 5:
        return p_classdef(p, tag, depth, out, err);
    case 6: {
        int32_t id;
        size_t idx;
        char *s;
        size_t sl;
        st = rd_le_i32(p->r, &id, err);
        if (st != PC_OK) {
            return st;
        }
        st = graph_reserve(p->g, id, &idx, err);
        if (st != PC_OK) {
            return st;
        }
        st = p_string(p, &s, &sl, err);
        if (st != PC_OK) {
            return st;
        }
        str_to_value(&p->g->objects[idx], s, sl);
        out->kind = V_REF;
        out->u.ref = id;
        return PC_OK;
    }
    case 7: {
        int32_t id;
        uint8_t kind;
        size_t rank;
        size_t alen = 1;
        size_t q;
        uint8_t t;
        int16_t prim;
        st = rd_le_i32(p->r, &id, err);
        if (st != PC_OK) {
            return st;
        }
        st = rd_byte(p->r, &kind, err);
        if (st != PC_OK) {
            return st;
        }
        st = rd_count(p->r, &rank, err);
        if (st != PC_OK) {
            return st;
        }
        if (kind > 5 || rank == 0 || rank > 32) {
            return pc_io_pdn_error(err, "invalid array dimensions");
        }
        for (q = 0; q < rank; q++) {
            size_t d2;
            st = rd_count(p->r, &d2, err);
            if (st != PC_OK) {
                return st;
            }
            if (!pc_size_mul(alen, d2, &alen)) {
                return pc_io_pdn_error(err, "array size overflow");
            }
        }
        if (kind >= 3) {
            for (q = 0; q < rank; q++) {
                int32_t lb;
                st = rd_le_i32(p->r, &lb, err);
                if (st != PC_OK) {
                    return st;
                }
                if (lb != 0) {
                    return pc_io_pdn_error(err, "nonzero array lower bounds are unsupported");
                }
            }
        }
        st = rd_byte(p->r, &t, err);
        if (st != PC_OK) {
            return st;
        }
        st = p_type_info(p, t, &prim, err);
        if (st != PC_OK) {
            return st;
        }
        return p_array(p, id, alen, prim, depth, out, err);
    }
    case 8: {
        uint8_t prim;
        st = rd_byte(p->r, &prim, err);
        if (st != PC_OK) {
            return st;
        }
        return p_primitive(p, prim, out, err);
    }
    case 9: {
        int32_t id;
        st = rd_le_i32(p->r, &id, err);
        if (st != PC_OK) {
            return st;
        }
        out->kind = V_REF;
        out->u.ref = id;
        return PC_OK;
    }
    case 10:
        out->kind = V_NULL;
        return PC_OK;
    case 12: {
        int32_t id;
        char *s;
        size_t sl;
        st = rd_le_i32(p->r, &id, err);
        if (st != PC_OK) {
            return st;
        }
        st = p_string(p, &s, &sl, err);
        if (st != PC_OK) {
            return st;
        }
        free(s);
        out->kind = V_NULL;
        return PC_OK;
    }
    case 15:
    case 16:
    case 17: {
        int32_t id;
        size_t alen;
        int16_t prim = -1;
        st = rd_le_i32(p->r, &id, err);
        if (st != PC_OK) {
            return st;
        }
        st = rd_count(p->r, &alen, err);
        if (st != PC_OK) {
            return st;
        }
        if (tag == 15) {
            uint8_t b;
            st = rd_byte(p->r, &b, err);
            if (st != PC_OK) {
                return st;
            }
            prim = (int16_t)b;
        }
        return p_array(p, id, alen, prim, depth, out, err);
    }
    default:
        return pc_io_pdn_error(err, "unsupported NRBF record %u", (unsigned)tag);
    }
}

static pc_status nrbf_parse(reader *r, const pc_interrupt *ctl, graph *g, pc_error *err)
{
    uint8_t b;
    int32_t root;
    int32_t hdr;
    int32_t major;
    int32_t minor;
    parser p;
    pc_status st = rd_byte(r, &b, err);
    if (st != PC_OK) {
        return st;
    }
    if (b != 0) {
        return pc_io_pdn_error(err, "missing NRBF stream header");
    }
    st = rd_le_i32(r, &root, err);
    if (st != PC_OK) {
        return st;
    }
    st = rd_le_i32(r, &hdr, err);
    if (st != PC_OK) {
        return st;
    }
    (void)hdr;
    if (root == 0) {
        return pc_io_pdn_error(err, "unsupported NRBF stream version");
    }
    st = rd_le_i32(r, &major, err);
    if (st != PC_OK) {
        return st;
    }
    if (major != 1) {
        return pc_io_pdn_error(err, "unsupported NRBF stream version");
    }
    st = rd_le_i32(r, &minor, err);
    if (st != PC_OK) {
        return st;
    }
    if (minor != 0) {
        return pc_io_pdn_error(err, "unsupported NRBF stream version");
    }
    g->root.kind = V_REF;
    g->root.u.ref = root;
    p.r = r;
    p.ctl = ctl;
    p.g = g;
    idmap_init(&p.class_map);
    p.items = 0;
    p.strings = 0;
    for (;;) {
        uint8_t tag;
        value tmp;
        st = rd_byte(r, &tag, err);
        if (st != PC_OK) {
            idmap_fini(&p.class_map);
            return st;
        }
        if (tag == 11) {
            break;
        }
        tmp.kind = V_NULL;
        st = p_record(&p, tag, 0, &tmp, err);
        if (st != PC_OK) {
            idmap_fini(&p.class_map);
            return st;
        }
        value_fini(&tmp);
    }
    idmap_fini(&p.class_map);
    if (g_resolve(g, &g->root, err) == NULL) {
        return PC_ERR_FORMAT;
    }
    return PC_OK;
}

/* ---- pixel chunk decode ---- */

/* Reads a memory block's numbered chunks in on-disk order; each chunk is gzip (version 0) or raw
 * (version 1). On success *out is a malloc'd buffer of `length` bytes (free() it). */
static pc_status pixels(reader *r, size_t length, const pc_interrupt *ctl, uint8_t **out, pc_error *err)
{
    uint8_t version;
    size_t chunk_size;
    size_t chunks;
    bool *seen;
    uint8_t *data;
    size_t c;
    pc_status st = rd_byte(r, &version, err);
    if (st != PC_OK) {
        return st;
    }
    if (version > 1) {
        return pc_io_pdn_error(err, "unsupported pixel compression %u", (unsigned)version);
    }
    st = rd_be_u32(r, &chunk_size, err);
    if (st != PC_OK) {
        return st;
    }
    if (chunk_size == 0) {
        return pc_io_pdn_error(err, "zero pixel chunk size");
    }
    chunks = length / chunk_size + (length % chunk_size != 0 ? 1 : 0);
    if (chunks > MAX_CHUNKS) {
        return pc_io_pdn_error(err, "too many pixel chunks");
    }
    if (version == 1 && r->len - r->pos < length) {
        return pc_io_pdn_error(err, "uncompressed pixel data is truncated");
    }
    seen = pc_calloc_array(chunks, sizeof *seen);
    data = pc_calloc_array(length, 1);
    if (seen == NULL || data == NULL) {
        free(seen);
        free(data);
        return pc_io_nomem(err, "decoding Paint.NET pixels");
    }
    for (c = 0; c < chunks; c++) {
        size_t index;
        size_t size;
        const uint8_t *raw;
        size_t start;
        size_t avail;
        size_t span;
        size_t end;
        size_t dlen;
        st = pc_interrupt_check(ctl, err);
        if (st != PC_OK) {
            goto fail;
        }
        st = rd_be_u32(r, &index, err);
        if (st != PC_OK) {
            goto fail;
        }
        if (index >= chunks) {
            st = pc_io_pdn_error(err, "pixel chunk index out of bounds");
            goto fail;
        }
        if (seen[index]) {
            st = pc_io_pdn_error(err, "duplicate pixel chunk");
            goto fail;
        }
        seen[index] = true;
        st = rd_be_u32(r, &size, err);
        if (st != PC_OK) {
            goto fail;
        }
        st = rd_take(r, size, &raw, err);
        if (st != PC_OK) {
            goto fail;
        }
        if (!pc_size_mul(index, chunk_size, &start)) {
            st = pc_io_pdn_error(err, "pixel offset overflow");
            goto fail;
        }
        avail = length > start ? length - start : 0;
        span = chunk_size < avail ? chunk_size : avail;
        if (!pc_size_add(start, span, &end) || start > length || end > length) {
            st = pc_io_pdn_error(err, "invalid pixel chunk range");
            goto fail;
        }
        dlen = end - start;
        if (version == 0) {
            pc_buf outb = PC_BUF_INIT;
            pc_decomp_info info;
            st = pc_inflate(PC_Z_GZIP, raw, size, dlen + 1, &outb, &info, err);
            if (st != PC_OK) {
                char inner[PC_ERROR_MSG_MAX];
                inner[0] = '\0';
                if (err != NULL) {
                    (void)snprintf(inner, sizeof inner, "%s", err->msg);
                }
                pc_buf_fini(&outb);
                st = pc_io_pdn_error(err, "invalid gzip pixel chunk: %s",
                                     inner[0] != '\0' ? inner : "corrupt deflate stream");
                goto fail;
            }
            if (outb.len < dlen) {
                pc_buf_fini(&outb);
                st = pc_io_pdn_error(err, "invalid gzip pixel chunk: unexpected end of file");
                goto fail;
            }
            if (outb.len > dlen || info.consumed != size) {
                pc_buf_fini(&outb);
                st = pc_io_pdn_error(err, "pixel chunk has excess data");
                goto fail;
            }
            if (dlen != 0) {
                memcpy(data + start, outb.data, dlen);
            }
            pc_buf_fini(&outb);
        } else {
            if (size != dlen) {
                st = pc_io_pdn_error(err, "incorrect uncompressed pixel chunk size");
                goto fail;
            }
            if (dlen != 0) {
                memcpy(data + start, raw, dlen);
            }
        }
    }
    free(seen);
    *out = data;
    return PC_OK;
fail:
    free(seen);
    free(data);
    return st;
}

/* ---- importer ---- */

typedef struct {
    int32_t id;
    size_t stride;
} layout;

typedef struct {
    int32_t block_id;
    pc_str name;
    pc_blend_mode blend;
    float opacity;
    bool visible;
} linfo;

typedef struct {
    int32_t id;
    const value *block;
} blockinfo;

typedef struct {
    int32_t id;
    pc_surface surf;
} surfmap;

pc_status pc_io_pdn_import(const char *name, const uint8_t *bytes, size_t len, const pc_interrupt *ctl,
                           pc_io_import_result *out, pc_error *err)
{
    reader r;
    graph g;
    const uint8_t *sig;
    const uint8_t *hs;
    size_t header_size;
    const value *root;
    const char *cn;
    size_t cnl;
    size_t width = 0;
    size_t height = 0;
    size_t row_bytes = 0;
    size_t canvas_bytes = 0;
    const value *list;
    const value *items_v;
    const value *items = NULL;
    size_t arr_n = 0;
    size_t llen = 0;
    layout *layouts = NULL;
    size_t nlayouts = 0;
    size_t caplayouts = 0;
    linfo *lis = NULL;
    size_t nlis = 0;
    blockinfo *blocks = NULL;
    size_t nblocks = 0;
    size_t capblocks = 0;
    surfmap *surfs = NULL;
    size_t nsurfs = 0;
    size_t total = 0;
    pc_io_import_result res;
    bool have_doc = false;
    size_t i;
    size_t k;
    pc_status st;

    r.bytes = bytes;
    r.len = len;
    r.pos = 0;
    graph_init(&g);
    memset(&res, 0, sizeof res);
    pc_io_warnings_init(&res.warnings);

    st = rd_take(&r, 4, &sig, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    if (memcmp(sig, "PDN3", 4) != 0) {
        st = pc_io_pdn_error(err, "unsupported document signature (expected PDN3)");
        goto cleanup;
    }
    st = rd_take(&r, 3, &hs, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    header_size = (size_t)hs[0] | (size_t)hs[1] << 8 | (size_t)hs[2] << 16;
    st = rd_take(&r, header_size, &sig, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    st = rd_take(&r, 2, &sig, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    if (sig[0] != 0 || sig[1] != 1) {
        st = pc_io_pdn_error(err, "unsupported PDN3 serialization version");
        goto cleanup;
    }
    st = nrbf_parse(&r, ctl, &g, err);
    if (st != PC_OK) {
        goto cleanup;
    }

    root = &g.root;
    st = g_class_name(&g, root, &cn, &cnl, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    if (!STR_EQ(cn, cnl, "PaintDotNet.Document")) {
        st = pc_io_pdn_error(err, "root object is not a Paint.NET document");
        goto cleanup;
    }
    st = g_count(&g, root, "width", &width, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    st = g_count(&g, root, "height", &height, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    if (!pc_size_mul(width, 4, &row_bytes) || !pc_size_mul(row_bytes, height, &canvas_bytes)) {
        st = pc_io_pdn_error(err, "canvas size overflow");
        goto cleanup;
    }
    if (width == 0 || height == 0 || width > (size_t)INT32_MAX || height > (size_t)INT32_MAX ||
        canvas_bytes > MAX_PIXEL_BYTES) {
        st = pc_io_pdn_error(err, "canvas is empty or exceeds the 1 GiB pixel limit");
        goto cleanup;
    }

    st = g_field(&g, root, "layers", &list, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    st = g_field(&g, list, "ArrayList+_items", &items_v, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    st = g_array(&g, items_v, &items, &arr_n, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    st = g_count(&g, list, "ArrayList+_size", &llen, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    if (llen == 0 || llen > MAX_LAYERS) {
        st = pc_io_pdn_error(err, "document must have 1..1024 bitmap layers");
        goto cleanup;
    }
    if (llen > arr_n) {
        st = pc_io_pdn_error(err, "layer list count exceeds its array");
        goto cleanup;
    }
    lis = pc_calloc_array(llen, sizeof *lis);
    if (lis == NULL) {
        st = pc_io_nomem(err, "importing Paint.NET layers");
        goto cleanup;
    }
    nlis = llen;

    for (i = 0; i < llen; i++) {
        const value *layer = &items[i];
        const value *surface;
        const value *memory;
        const value *props;
        const value *name_v;
        const char *nm;
        size_t nml;
        size_t lw;
        size_t lh;
        size_t sw;
        size_t sh;
        size_t stride;
        size_t opacity;
        bool vis;
        bool found;
        pc_blend_mode bm;
        st = pc_interrupt_check(ctl, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        st = g_class_name(&g, layer, &cn, &cnl, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        if (!STR_EQ(cn, cnl, "PaintDotNet.BitmapLayer")) {
            st = pc_io_pdn_error(err, "only PDN3 bitmap layers are supported");
            goto cleanup;
        }
        st = g_count(&g, layer, "Layer+width", &lw, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        st = g_count(&g, layer, "Layer+height", &lh, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        if (lw != width || lh != height) {
            st = pc_io_pdn_error(err, "layer dimensions do not match the canvas");
            goto cleanup;
        }
        st = g_field(&g, layer, "surface", &surface, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        st = g_class_name(&g, surface, &cn, &cnl, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        found = STR_EQ(cn, cnl, "PaintDotNet.Surface");
        if (found) {
            st = g_count(&g, surface, "width", &sw, err);
            if (st != PC_OK) {
                goto cleanup;
            }
            st = g_count(&g, surface, "height", &sh, err);
            if (st != PC_OK) {
                goto cleanup;
            }
            found = sw == width && sh == height;
        }
        if (!found) {
            st = pc_io_pdn_error(err, "invalid bitmap surface");
            goto cleanup;
        }
        st = g_count(&g, surface, "stride", &stride, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        if (stride < row_bytes) {
            st = pc_io_pdn_error(err, "surface stride is too short for BGRA32 pixels");
            goto cleanup;
        }
        st = g_field(&g, surface, "scan0", &memory, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        if (memory->kind != V_REF) {
            st = pc_io_pdn_error(err, "missing pixel memory reference");
            goto cleanup;
        }
        found = false;
        for (k = 0; k < nlayouts; k++) {
            if (layouts[k].id == memory->u.ref) {
                if (layouts[k].stride != stride) {
                    st = pc_io_pdn_error(err, "shared pixel memory has inconsistent strides");
                    goto cleanup;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            if (nlayouts == caplayouts) {
                void *nl = pc_grow_array(layouts, &caplayouts, nlayouts + 1, sizeof *layouts);
                if (nl == NULL) {
                    st = pc_io_nomem(err, "importing Paint.NET layers");
                    goto cleanup;
                }
                layouts = nl;
            }
            layouts[nlayouts].id = memory->u.ref;
            layouts[nlayouts].stride = stride;
            nlayouts++;
        }
        st = g_field(&g, layer, "Layer+properties", &props, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        st = g_count(&g, props, "opacity", &opacity, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        if (opacity > 255) {
            st = pc_io_pdn_error(err, "opacity exceeds 255");
            goto cleanup;
        }
        st = g_field(&g, props, "name", &name_v, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        st = g_string(&g, name_v, &nm, &nml, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        st = pc_str_set(&lis[i].name, nm, nml, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        st = gf_bool(&g, props, "visible", &vis, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        st = g_blend(&g, layer, props, &bm, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        lis[i].block_id = memory->u.ref;
        lis[i].opacity = (float)opacity / 255.0f;
        lis[i].visible = vis;
        lis[i].blend = bm;
    }

    /* Collect the MemoryBlock objects in serialized order and validate every allocation up front. */
    for (i = 0; i < g.nobjects; i++) {
        const value *v = &g.objects[i];
        if (v->kind == V_OBJECT &&
            STR_EQ(v->u.obj.cls->name, v->u.obj.cls->name_len, "PaintDotNet.MemoryBlock")) {
            if (nblocks == capblocks) {
                void *nb = pc_grow_array(blocks, &capblocks, nblocks + 1, sizeof *blocks);
                if (nb == NULL) {
                    st = pc_io_nomem(err, "importing Paint.NET pixels");
                    goto cleanup;
                }
                blocks = nb;
            }
            blocks[nblocks].id = g.ids[i];
            blocks[nblocks].block = v;
            nblocks++;
        }
    }
    for (i = 0; i < nblocks; i++) {
        size_t length;
        size_t stride = 0;
        size_t expect;
        bool found = false;
        bool has_parent;
        bool bad;
        st = g_count(&g, blocks[i].block, "length64", &length, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        for (k = 0; k < nlayouts; k++) {
            if (layouts[k].id == blocks[i].id) {
                stride = layouts[k].stride;
                found = true;
                break;
            }
        }
        if (!found) {
            st = pc_io_pdn_error(err, "unreferenced pixel memory block");
            goto cleanup;
        }
        if (!pc_size_mul(stride, height, &expect)) {
            st = pc_io_pdn_error(err, "pixel length overflow");
            goto cleanup;
        }
        if (length != expect) {
            st = pc_io_pdn_error(err, "pixel memory length does not match the surface");
            goto cleanup;
        }
        st = gf_bool(&g, blocks[i].block, "hasParent", &has_parent, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        bad = has_parent;
        if (!bad) {
            bool deferred;
            st = gf_bool(&g, blocks[i].block, "deferred", &deferred, err);
            if (st != PC_OK) {
                goto cleanup;
            }
            bad = !deferred;
        }
        if (bad) {
            st = pc_io_pdn_error(err, "unsupported non-deferred or parented pixel memory");
            goto cleanup;
        }
        if (!pc_size_add(total, length, &total)) {
            st = pc_io_pdn_error(err, "pixel length overflow");
            goto cleanup;
        }
        if (total > MAX_PIXEL_BYTES) {
            st = pc_io_pdn_error(err, "decoded pixels exceed the 1 GiB limit");
            goto cleanup;
        }
    }

    surfs = pc_malloc_array(nblocks, sizeof *surfs);
    if (surfs == NULL) {
        st = pc_io_nomem(err, "importing Paint.NET pixels");
        goto cleanup;
    }
    for (i = 0; i < nblocks; i++) {
        size_t length;
        size_t stride = 0;
        uint8_t *data;
        pc_surface s;
        size_t y;
        st = g_count(&g, blocks[i].block, "length64", &length, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        st = pixels(&r, length, ctl, &data, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        for (k = 0; k < nlayouts; k++) {
            if (layouts[k].id == blocks[i].id) {
                stride = layouts[k].stride;
                break;
            }
        }
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        for (y = 0; y < height; y++) {
            uint8_t *row = data + y * stride;
            size_t x;
            st = pc_interrupt_check(ctl, err);
            if (st != PC_OK) {
                pc_surface_fini(&s);
                free(data);
                goto cleanup;
            }
            for (x = 0; x < width; x++) {
                uint8_t *px = row + x * 4;
                uint8_t t = px[0];
                px[0] = px[2];
                px[2] = t;
            }
            st = pc_surface_write_interleaved(&s, pc_rect_from_xywh(0, (int32_t)y, (uint32_t)width, 1), row,
                                              row_bytes, err);
            if (st != PC_OK) {
                pc_surface_fini(&s);
                free(data);
                goto cleanup;
            }
        }
        free(data);
        surfs[nsurfs].id = blocks[i].id;
        surfs[nsurfs].surf = s;
        nsurfs++;
        pc_interrupt_progress(ctl, 0.05f + 0.95f * (float)(i + 1) / (float)nblocks);
    }

    if (r.pos != len) {
        st = pc_io_pdn_error(err, "unexpected trailing data");
        goto cleanup;
    }

    st = pc_document_init(&res.document, name, pc_size_make((uint32_t)width, (uint32_t)height),
                          PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    have_doc = true;
    for (i = 0; i < nlis; i++) {
        pc_surface *src = NULL;
        pc_surface clone;
        pc_layer layer;
        for (k = 0; k < nsurfs; k++) {
            if (surfs[k].id == lis[i].block_id) {
                src = &surfs[k].surf;
                break;
            }
        }
        if (src == NULL) {
            st = pc_io_pdn_error(err, "missing pixel memory block");
            goto cleanup;
        }
        st = pc_surface_clone(src, &clone, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        st = pc_io_raster_layer(pc_str_c(&lis[i].name), &clone, &layer, err);
        if (st != PC_OK) {
            pc_surface_fini(&clone);
            goto cleanup;
        }
        st = pc_str_set(&layer.name, lis[i].name.ptr, lis[i].name.len, err);
        if (st != PC_OK) {
            pc_layer_fini(&layer);
            goto cleanup;
        }
        layer.visible = lis[i].visible;
        layer.opacity = lis[i].opacity;
        layer.blend = lis[i].blend;
        st = pc_io_push_layer(&res.document, &layer, err);
        if (st != PC_OK) {
            pc_layer_fini(&layer);
            goto cleanup;
        }
    }

    {
        const value *meta;
        st = g_optional_field(&g, root, "userMetadataItems", &meta, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        if (meta != NULL) {
            const value *rm = g_resolve(&g, meta, err);
            if (rm == NULL) {
                st = PC_ERR_FORMAT;
                goto cleanup;
            }
            if (rm->kind != V_NULL) {
                const value *marr;
                size_t mn;
                st = g_array(&g, meta, &marr, &mn, err);
                if (st != PC_OK) {
                    goto cleanup;
                }
                if (mn > 0) {
                    st = pc_io_warn(&res.warnings, err, "Paint.NET document metadata was not imported");
                    if (st != PC_OK) {
                        goto cleanup;
                    }
                }
            }
        }
    }

    res.source_read_only = true;
    res.preview_only = false;
    *out = res;
    st = PC_OK;
    have_doc = false; /* ownership transferred to *out */

cleanup:
    free(layouts);
    if (lis != NULL) {
        for (i = 0; i < nlis; i++) {
            pc_str_fini(&lis[i].name);
        }
        free(lis);
    }
    free(blocks);
    if (surfs != NULL) {
        for (k = 0; k < nsurfs; k++) {
            pc_surface_fini(&surfs[k].surf);
        }
        free(surfs);
    }
    graph_fini(&g);
    if (st != PC_OK) {
        if (have_doc) {
            pc_document_fini(&res.document);
        }
        pc_io_warnings_fini(&res.warnings);
    }
    return st;
}
