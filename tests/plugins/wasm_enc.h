/* A tiny WebAssembly module encoder for the plug-in tests (the C twin of the encoder in
 * tools/rustref/src/plugins_vectors.rs, which the parity test checks byte for byte). Bodies are
 * raw bytecode. Fixed capacities; tests only. */
#ifndef PC_TESTS_PLUGINS_WASM_ENC_H
#define PC_TESTS_PLUGINS_WASM_ENC_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"

#define WI32 0x7f
#define WI64 0x7e

enum { WM_MAX = 32 };

typedef struct wm_bytes {
    uint8_t b[512];
    size_t n;
} wm_bytes;

typedef struct wm {
    wm_bytes types[WM_MAX];
    size_t ntypes;
    wm_bytes imports[4];
    int import_is_func[4];
    size_t nimports;
    uint32_t func_type[WM_MAX];
    wm_bytes func_body[WM_MAX]; /* locals declaration + code */
    size_t nfuncs;
    wm_bytes memories[2];
    size_t nmemories;
    wm_bytes exports[WM_MAX];
    char export_name[WM_MAX][32];
    uint8_t export_kind[WM_MAX];
    uint32_t export_idx[WM_MAX];
    size_t nexports;
    int has_start;
    uint32_t start;
    wm_bytes data[4];
    size_t ndata;
} wm;

static inline void wm_put(wm_bytes *o, const void *p, size_t n)
{
    if (n > 0 && o->n + n <= sizeof o->b) {
        memcpy(o->b + o->n, p, n);
        o->n += n;
    }
}

static inline void wm_byte(wm_bytes *o, uint8_t b)
{
    wm_put(o, &b, 1);
}

static inline void wm_uleb(wm_bytes *o, uint64_t v)
{
    do {
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7;
        wm_byte(o, (uint8_t)(v != 0 ? b | 0x80 : b));
    } while (v != 0);
}

static inline void wm_sleb(wm_bytes *o, int64_t v)
{
    for (;;) {
        uint8_t b = (uint8_t)((uint64_t)v & 0x7f);
        int done;
        v >>= 7; /* arithmetic shift on every supported compiler */
        done = (v == 0 && (b & 0x40) == 0) || (v == -1 && (b & 0x40) != 0);
        wm_byte(o, (uint8_t)(done ? b : b | 0x80));
        if (done) {
            return;
        }
    }
}

static inline uint32_t wm_type(wm *m, const uint8_t *p, size_t np, const uint8_t *r, size_t nr)
{
    wm_bytes t;
    memset(&t, 0, sizeof t);
    wm_byte(&t, 0x60);
    wm_uleb(&t, np);
    wm_put(&t, p, np);
    wm_uleb(&t, nr);
    wm_put(&t, r, nr);
    for (size_t i = 0; i < m->ntypes; i++) {
        if (m->types[i].n == t.n && memcmp(m->types[i].b, t.b, t.n) == 0) {
            return (uint32_t)i;
        }
    }
    m->types[m->ntypes] = t;
    return (uint32_t)m->ntypes++;
}

static inline uint32_t wm_func_index_base(const wm *m)
{
    uint32_t n = 0;
    for (size_t i = 0; i < m->nimports; i++) {
        n += m->import_is_func[i] ? 1u : 0u;
    }
    return n;
}

/* A function; locals as (count, type) pairs. Returns its index in the function space. */
static inline uint32_t wm_func(wm *m, const uint8_t *p, size_t np, const uint8_t *r, size_t nr,
                               const uint8_t (*locals)[2], size_t nlocals, const uint8_t *body, size_t nbody)
{
    wm_bytes *f = &m->func_body[m->nfuncs];
    memset(f, 0, sizeof *f);
    m->func_type[m->nfuncs] = wm_type(m, p, np, r, nr);
    wm_uleb(f, nlocals);
    for (size_t i = 0; i < nlocals; i++) {
        wm_uleb(f, locals[i][0]);
        wm_byte(f, locals[i][1]);
    }
    wm_put(f, body, nbody);
    m->nfuncs++;
    return wm_func_index_base(m) + (uint32_t)m->nfuncs - 1;
}

static inline void wm_export(wm *m, const char *name, uint8_t kind, uint32_t idx)
{
    (void)snprintf(m->export_name[m->nexports], sizeof m->export_name[0], "%s", name);
    m->export_kind[m->nexports] = kind;
    m->export_idx[m->nexports] = idx;
    m->nexports++;
}

static inline void wm_memory(wm *m, uint64_t min)
{
    wm_bytes *t = &m->memories[m->nmemories++];
    memset(t, 0, sizeof *t);
    wm_byte(t, 0);
    wm_uleb(t, min);
}

static inline void wm_data(wm *m, int32_t offset, const void *bytes, size_t n)
{
    wm_bytes *d = &m->data[m->ndata++];
    memset(d, 0, sizeof *d);
    wm_byte(d, 0);
    wm_byte(d, 0x41);
    wm_sleb(d, offset);
    wm_byte(d, 0x0b);
    wm_uleb(d, n);
    wm_put(d, bytes, n);
}

/* An import: kind byte + description (desc[0] is the kind). Shifts function exports. */
static inline void wm_import(wm *m, const char *mod, const char *name, const uint8_t *desc, size_t n)
{
    wm_bytes *e = &m->imports[m->nimports];
    memset(e, 0, sizeof *e);
    wm_uleb(e, strlen(mod));
    wm_put(e, mod, strlen(mod));
    wm_uleb(e, strlen(name));
    wm_put(e, name, strlen(name));
    wm_put(e, desc, n);
    m->import_is_func[m->nimports] = desc[0] == 0;
    m->nimports++;
    if (desc[0] == 0) {
        for (size_t i = 0; i < m->nexports; i++) {
            m->export_idx[i] += m->export_kind[i] == 0 ? 1u : 0u;
        }
    }
}

static inline void wm_section(pc_buf *out, uint8_t id, const pc_buf *payload)
{
    wm_bytes len;
    memset(&len, 0, sizeof len);
    wm_uleb(&len, payload->len);
    (void)pc_buf_append_byte(out, id, NULL);
    (void)pc_buf_append(out, len.b, len.n, NULL);
    (void)pc_buf_append(out, payload->data, payload->len, NULL);
}

static inline void wm_vec(pc_buf *p, const wm_bytes *items, size_t n)
{
    wm_bytes c;
    memset(&c, 0, sizeof c);
    wm_uleb(&c, n);
    (void)pc_buf_append(p, c.b, c.n, NULL);
    for (size_t i = 0; i < n; i++) {
        (void)pc_buf_append(p, items[i].b, items[i].n, NULL);
    }
}

/* The encoded module (append to out). */
static inline void wm_encode(const wm *m, pc_buf *out)
{
    pc_buf p = PC_BUF_INIT;
    (void)pc_buf_append(out, "\0asm\x01\0\0\0", 8, NULL);
    if (m->ntypes > 0) {
        wm_vec(&p, m->types, m->ntypes);
        wm_section(out, 1, &p);
        pc_buf_truncate(&p, 0);
    }
    if (m->nimports > 0) {
        wm_vec(&p, m->imports, m->nimports);
        wm_section(out, 2, &p);
        pc_buf_truncate(&p, 0);
    }
    if (m->nfuncs > 0) {
        wm_bytes *items = calloc(m->nfuncs, sizeof *items);
        for (size_t i = 0; items != NULL && i < m->nfuncs; i++) {
            wm_uleb(&items[i], m->func_type[i]);
        }
        if (items != NULL) {
            wm_vec(&p, items, m->nfuncs);
        }
        free(items);
        wm_section(out, 3, &p);
        pc_buf_truncate(&p, 0);
    }
    if (m->nmemories > 0) {
        wm_vec(&p, m->memories, m->nmemories);
        wm_section(out, 5, &p);
        pc_buf_truncate(&p, 0);
    }
    if (m->nexports > 0) {
        wm_bytes *items = calloc(m->nexports, sizeof *items);
        for (size_t i = 0; items != NULL && i < m->nexports; i++) {
            wm_uleb(&items[i], strlen(m->export_name[i]));
            wm_put(&items[i], m->export_name[i], strlen(m->export_name[i]));
            wm_byte(&items[i], m->export_kind[i]);
            wm_uleb(&items[i], m->export_idx[i]);
        }
        if (items != NULL) {
            wm_vec(&p, items, m->nexports);
        }
        free(items);
        wm_section(out, 7, &p);
        pc_buf_truncate(&p, 0);
    }
    if (m->has_start) {
        wm_bytes s;
        memset(&s, 0, sizeof s);
        wm_uleb(&s, m->start);
        (void)pc_buf_append(&p, s.b, s.n, NULL);
        wm_section(out, 8, &p);
        pc_buf_truncate(&p, 0);
    }
    if (m->nfuncs > 0) {
        wm_bytes *items = calloc(m->nfuncs, sizeof *items);
        for (size_t i = 0; items != NULL && i < m->nfuncs; i++) {
            wm_uleb(&items[i], m->func_body[i].n);
            wm_put(&items[i], m->func_body[i].b, m->func_body[i].n);
        }
        if (items != NULL) {
            wm_vec(&p, items, m->nfuncs);
        }
        free(items);
        wm_section(out, 10, &p);
        pc_buf_truncate(&p, 0);
    }
    if (m->ndata > 0) {
        wm_vec(&p, m->data, m->ndata);
        wm_section(out, 11, &p);
    }
    pc_buf_fini(&p);
}

static const uint8_t WM_ALLOC_BODY[] = {0x20, 0x00, 0x41, 0xff, 0xff, 0x03, 0x6a, 0x41, 0x10, 0x76, 0x40,
                                        0x00, 0x22, 0x01, 0x41, 0x7f, 0x46, 0x04, 0x7f, 0x41, 0x00, 0x05,
                                        0x20, 0x01, 0x41, 0x80, 0x80, 0x04, 0x6c, 0x0b, 0x0b};

static const char WM_MANIFEST[] =
    "{\"id\":\"test.plugin\",\"name\":\"Test\",\"version\":\"1\",\"kind\":\"filter\"}";

/* abi.rs `module`: the manifest at offset 16, pc_abi_version returning `version`, the bump
 * allocator, and pc_filter with `body` (params $buf=0 $len=1 $w=2 $h=3 $ch=4 $fmt=5 $pp=6 $pl=7;
 * nlocals extra i32 locals from index 8). */
static inline void wm_abi(wm *m, const char *manifest, int32_t version, uint32_t nlocals, const uint8_t *body,
                          size_t nbody)
{
    static const uint8_t I32_[1] = {WI32}, I64_[1] = {WI64},
                         I32x8[8] = {WI32, WI32, WI32, WI32, WI32, WI32, WI32, WI32};
    static const uint8_t one_local[1][2] = {{1, WI32}};
    uint8_t locals[1][2] = {{(uint8_t)nlocals, WI32}};
    wm_bytes v, mb;
    size_t len = strlen(manifest);
    uint32_t fv, fm, fa, ff;
    memset(m, 0, sizeof *m);
    memset(&v, 0, sizeof v);
    memset(&mb, 0, sizeof mb);
    wm_memory(m, 1);
    wm_data(m, 16, manifest, len);
    wm_byte(&v, 0x41);
    wm_sleb(&v, version);
    wm_byte(&v, 0x0b);
    fv = wm_func(m, NULL, 0, I32_, 1, NULL, 0, v.b, v.n);
    wm_byte(&mb, 0x42);
    wm_sleb(&mb, (int64_t)(((uint64_t)len << 32) | 16u));
    wm_byte(&mb, 0x0b);
    fm = wm_func(m, NULL, 0, I64_, 1, NULL, 0, mb.b, mb.n);
    fa = wm_func(m, I32_, 1, I32_, 1, one_local, 1, WM_ALLOC_BODY, sizeof WM_ALLOC_BODY);
    ff = wm_func(m, I32x8, 8, I32_, 1, nlocals > 0 ? (const uint8_t (*)[2])locals : NULL, nlocals > 0 ? 1 : 0,
                 body, nbody);
    wm_export(m, "memory", 2, 0);
    wm_export(m, "pc_abi_version", 0, fv);
    wm_export(m, "pc_manifest", 0, fm);
    wm_export(m, "pc_alloc", 0, fa);
    wm_export(m, "pc_filter", 0, ff);
}

/* Builds an ABI module into a fresh buffer (free with pc_buf_fini). */
static inline pc_buf wm_abi_bytes(const char *manifest, int32_t version, uint32_t nlocals,
                                  const uint8_t *body, size_t nbody)
{
    wm *m = calloc(1, sizeof *m);
    pc_buf out = PC_BUF_INIT;
    if (m != NULL) {
        wm_abi(m, manifest, version, nlocals, body, nbody);
        wm_encode(m, &out);
        free(m);
    }
    return out;
}

#endif
