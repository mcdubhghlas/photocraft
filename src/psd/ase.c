#include "psd/ase.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "psd/internal.h"

#define GROUP_START 0xC001u
#define GROUP_END 0xC002u
#define COLOR 0x0001u

void pc_psd_ase_swatch_fini(pc_psd_ase_swatch *s)
{
    if (s != NULL) {
        pc_psd_str_fini(&s->name);
        pc_psd_bytes_fini(&s->color.data);
    }
}

static void free_swatches(pc_psd_ase_swatch *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        pc_psd_ase_swatch_fini(&v[i]);
    }
    free(v);
}

void pc_psd_ase_entry_fini(pc_psd_ase_entry *e)
{
    if (e == NULL) {
        return;
    }
    pc_psd_ase_swatch_fini(&e->swatch);
    pc_psd_str_fini(&e->name);
    free_swatches(e->swatches, e->n);
    e->swatches = NULL;
    e->n = 0;
}

void pc_psd_ase_fini(pc_psd_ase *f)
{
    size_t i;
    if (f == NULL) {
        return;
    }
    for (i = 0; i < f->n; i++) {
        pc_psd_ase_entry_fini(&f->entries[i]);
    }
    free(f->entries);
    f->entries = NULL;
    f->n = 0;
}

static size_t ncomp(pc_psd_ase_model m)
{
    return m == PC_PSD_ASE_CMYK ? 4 : m == PC_PSD_ASE_GRAY ? 1 : m == PC_PSD_ASE_OTHER ? 0 : 3;
}

bool pc_psd_ase_swatch_eq(const pc_psd_ase_swatch *a, const pc_psd_ase_swatch *b)
{
    size_t i;
    if (!pc_psd_str_eq(&a->name, &b->name) || a->kind != b->kind || a->color.model != b->color.model) {
        return false;
    }
    for (i = 0; i < ncomp(a->color.model); i++) {
        if (!(a->color.c[i] == b->color.c[i])) {
            return false;
        }
    }
    return a->color.model != PC_PSD_ASE_OTHER ||
           (memcmp(a->color.tag, b->color.tag, 4) == 0 && pc_psd_bytes_eq(&a->color.data, &b->color.data));
}

bool pc_psd_ase_eq(const pc_psd_ase *a, const pc_psd_ase *b)
{
    size_t i;
    size_t k;
    if (a->n != b->n) {
        return false;
    }
    for (i = 0; i < a->n; i++) {
        const pc_psd_ase_entry *x = &a->entries[i];
        const pc_psd_ase_entry *y = &b->entries[i];
        if (x->group != y->group) {
            return false;
        }
        if (!x->group) {
            if (!pc_psd_ase_swatch_eq(&x->swatch, &y->swatch)) {
                return false;
            }
            continue;
        }
        if (!pc_psd_str_eq(&x->name, &y->name) || x->n != y->n) {
            return false;
        }
        for (k = 0; k < x->n; k++) {
            if (!pc_psd_ase_swatch_eq(&x->swatches[k], &y->swatches[k])) {
                return false;
            }
        }
    }
    return true;
}

size_t pc_psd_ase_swatch_count(const pc_psd_ase *f)
{
    size_t i;
    size_t n = 0;
    for (i = 0; i < f->n; i++) {
        n += f->entries[i].group ? f->entries[i].n : 1;
    }
    return n;
}

/* ---- reading ---- */

static pc_status f32_be(psd_rd *r, float *out, pc_error *err)
{
    uint32_t bits;
    float v;
    pc_status st = psd_u32(r, &bits, err);
    if (st != PC_OK) {
        return st;
    }
    memcpy(&v, &bits, sizeof v);
    /* Non-finite components would poison every conversion downstream. */
    *out = isfinite(v) ? v : 0.0f;
    return PC_OK;
}

static pc_status read_name(psd_rd *r, pc_psd_str *out, pc_error *err)
{
    uint16_t n;
    uint16_t units[PC_PSD_ASE_MAX_NAME_UNITS];
    size_t k = 0;
    uint32_t i;
    pc_status st;
    if (psd_empty(r)) {
        pc_psd_str_fini(out);
        return PC_OK;
    }
    st = psd_u16(r, &n, err);
    if (st == PC_OK) {
        st = psd_check_count(r, n, 2, err);
    }
    for (i = 0; st == PC_OK && i < n; i++) {
        uint16_t u;
        st = psd_u16(r, &u, err);
        if (st == PC_OK && k < PC_PSD_ASE_MAX_NAME_UNITS) {
            units[k++] = u;
        }
    }
    if (st != PC_OK) {
        return st;
    }
    while (k > 0 && units[k - 1] == 0) {
        k--;
    }
    return psd_str_from_units(units, k, out, err);
}

static pc_status read_color(psd_rd *body, pc_psd_ase_swatch *out, pc_error *err)
{
    pc_psd_ase_swatch s;
    pc_status st;
    size_t i;
    memset(&s, 0, sizeof s);
    CK(read_name(body, &s.name, err));
    CK(psd_arr4(body, s.color.tag, err));
    if (memcmp(s.color.tag, "RGB ", 4) == 0) {
        s.color.model = PC_PSD_ASE_RGB;
    } else if (memcmp(s.color.tag, "CMYK", 4) == 0) {
        s.color.model = PC_PSD_ASE_CMYK;
    } else if (memcmp(s.color.tag, "LAB ", 4) == 0) {
        s.color.model = PC_PSD_ASE_LAB;
    } else if (memcmp(s.color.tag, "Gray", 4) == 0) {
        s.color.model = PC_PSD_ASE_GRAY;
    } else {
        /* Unknown model: everything but the trailing colour type is its data. */
        size_t n = psd_remaining(body) >= 2 ? psd_remaining(body) - 2 : 0;
        s.color.model = PC_PSD_ASE_OTHER;
        CK(psd_take_bytes(body, n, &s.color.data, err));
    }
    for (i = 0; i < ncomp(s.color.model); i++) {
        CK(f32_be(body, &s.color.c[i], err));
    }
    if (s.color.model != PC_PSD_ASE_OTHER) {
        memset(s.color.tag, 0, 4);
    }
    /* Some writers omit the colour type; it defaults to process. */
    s.kind = PC_PSD_ASE_PROCESS;
    if (psd_remaining(body) >= 2) {
        uint16_t k;
        CK(psd_u16(body, &k, err));
        s.kind = k == 0 ? PC_PSD_ASE_GLOBAL : k == 1 ? PC_PSD_ASE_SPOT : PC_PSD_ASE_PROCESS;
    }
    *out = s;
    return PC_OK;
fail:
    pc_psd_ase_swatch_fini(&s);
    return st;
}

typedef struct entries {
    pc_psd_ase_entry *v;
    size_t n;
    size_t cap;
} entries;

static pc_status push_entry(entries *es, pc_psd_ase_entry *e, pc_error *err)
{
    pc_psd_ase_entry *g = pc_grow_array(es->v, &es->cap, es->n + 1, sizeof *g);
    if (g == NULL) {
        pc_psd_ase_entry_fini(e);
        return psd_nomem(err);
    }
    es->v = g;
    es->v[es->n++] = *e;
    memset(e, 0, sizeof *e);
    return PC_OK;
}

pc_status pc_psd_ase_parse(const uint8_t *data, size_t len, pc_psd_ase *out, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    uint8_t sig[4];
    uint16_t major;
    uint16_t minor;
    uint32_t count;
    uint32_t i;
    entries es = {NULL, 0, 0};
    pc_psd_ase_entry open;
    bool is_open = false;
    size_t open_cap = 0;
    size_t total = 0;
    pc_status st;
    memset(&open, 0, sizeof open);
    open.group = true;
    CK(psd_arr4(&r, sig, err));
    if (memcmp(sig, "ASEF", 4) != 0) {
        return psd_bad_sig(err, "ASEF", sig);
    }
    CK(psd_u16(&r, &major, err));
    CK(psd_u16(&r, &minor, err));
    if (major != 1) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "unsupported file version %u", (unsigned)major);
    }
    CK(psd_u32(&r, &count, err));
    /* Every block has at least a type and a length. */
    CK(psd_check_count(&r, count, 6, err));
    for (i = 0; i < count; i++) {
        uint16_t kind;
        uint32_t blen;
        psd_rd body;
        CK(psd_u16(&r, &kind, err));
        CK(psd_u32(&r, &blen, err));
        CK(psd_sub(&r, blen, &body, err));
        if (kind == GROUP_START || kind == GROUP_END) {
            if (is_open) {
                is_open = false;
                open_cap = 0;
                CK(push_entry(&es, &open, err));
                open.group = true;
            }
            if (kind == GROUP_START) {
                is_open = true;
                CK(read_name(&body, &open.name, err));
            }
        } else if (kind == COLOR) {
            pc_psd_ase_swatch s;
            total++;
            if (total > PC_PSD_ASE_MAX_SWATCHES) {
                st = psd_limit(err, "too many swatches in an .ase file");
                goto fail;
            }
            CK(read_color(&body, &s, err));
            if (is_open) {
                pc_psd_ase_swatch *g = pc_grow_array(open.swatches, &open_cap, open.n + 1, sizeof *g);
                if (g == NULL) {
                    pc_psd_ase_swatch_fini(&s);
                    st = psd_nomem(err);
                    goto fail;
                }
                open.swatches = g;
                open.swatches[open.n++] = s;
            } else {
                pc_psd_ase_entry e;
                memset(&e, 0, sizeof e);
                e.swatch = s;
                CK(push_entry(&es, &e, err));
            }
        }
    }
    if (is_open) {
        CK(push_entry(&es, &open, err));
    }
    out->entries = es.v;
    out->n = es.n;
    return PC_OK;
fail:
    pc_psd_ase_entry_fini(&open);
    {
        pc_psd_ase f = {es.v, es.n};
        pc_psd_ase_fini(&f);
    }
    return st;
}

/* ---- writing ---- */

/* put_name writes the length as units + 1 in a u16, so the cap must leave room. */
_Static_assert(PC_PSD_ASE_MAX_NAME_UNITS < 65535u, "ASE name cap must fit a u16 length with the NUL");

static void put_name(psd_w *w, const pc_psd_str *name)
{
    uint16_t *u = NULL;
    size_t n = 0;
    size_t i;
    pc_status st;
    if (!psd_w_ok(w)) {
        return;
    }
    st = psd_utf8n_to_units(pc_psd_str_c(name), name->len, &u, &n, w->err);
    if (st != PC_OK) {
        psd_w_fail(w, st);
        return;
    }
    n = psd_units_capped(u, n, PC_PSD_ASE_MAX_NAME_UNITS);
    psd_put_u16(w, (uint16_t)(n + 1));
    for (i = 0; i < n; i++) {
        psd_put_u16(w, u[i]);
    }
    psd_put_u16(w, 0);
    free(u);
}

static void put_block(psd_w *w, uint16_t kind, const pc_psd_bytes *body)
{
    if (!psd_w_ok(w)) {
        return;
    }
    if (body->len > UINT32_MAX) {
        psd_w_fail(w, psd_limit(w->err, "swatch block too large"));
        return;
    }
    psd_put_u16(w, kind);
    psd_put_u32(w, (uint32_t)body->len);
    psd_put(w, body->data, body->len);
}

static void put_color(psd_w *w, const pc_psd_ase_swatch *s)
{
    psd_w b;
    pc_psd_bytes body = {NULL, 0};
    size_t i;
    static const char *const tags[4] = {"RGB ", "CMYK", "LAB ", "Gray"};
    psd_w_init(&b, w->err);
    put_name(&b, &s->name);
    if (s->color.model == PC_PSD_ASE_OTHER) {
        psd_put(&b, s->color.tag, 4);
        psd_put(&b, s->color.data.data, s->color.data.len);
    } else {
        psd_put(&b, tags[s->color.model], 4);
        for (i = 0; i < ncomp(s->color.model); i++) {
            uint32_t bits;
            memcpy(&bits, &s->color.c[i], sizeof bits);
            psd_put_u32(&b, bits);
        }
    }
    psd_put_u16(&b, (uint16_t)s->kind);
    {
        pc_status st = psd_w_finish(&b, &body);
        if (st != PC_OK) {
            psd_w_fail(w, st);
            return;
        }
    }
    put_block(w, COLOR, &body);
    pc_psd_bytes_fini(&body);
}

pc_status pc_psd_ase_write(const pc_psd_ase *f, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    psd_w body;
    pc_psd_bytes bb = {NULL, 0};
    uint64_t blocks = 0;
    size_t i;
    size_t k;
    pc_status st;
    if (pc_psd_ase_swatch_count(f) > PC_PSD_ASE_MAX_SWATCHES) {
        return psd_limit(err, "too many swatches for an .ase file");
    }
    psd_w_init(&body, err);
    for (i = 0; i < f->n && psd_w_ok(&body); i++) {
        const pc_psd_ase_entry *e = &f->entries[i];
        if (!e->group) {
            put_color(&body, &e->swatch);
            blocks++;
        } else {
            psd_w g;
            pc_psd_bytes gb = {NULL, 0};
            pc_psd_bytes empty = {NULL, 0};
            psd_w_init(&g, err);
            put_name(&g, &e->name);
            st = psd_w_finish(&g, &gb);
            if (st != PC_OK) {
                psd_w_fail(&body, st);
                break;
            }
            put_block(&body, GROUP_START, &gb);
            pc_psd_bytes_fini(&gb);
            for (k = 0; k < e->n; k++) {
                put_color(&body, &e->swatches[k]);
            }
            put_block(&body, GROUP_END, &empty);
            blocks += 2 + (uint64_t)e->n;
        }
    }
    st = psd_w_finish(&body, &bb);
    if (st != PC_OK) {
        return st;
    }
    if (blocks > UINT32_MAX) {
        pc_psd_bytes_fini(&bb);
        return psd_limit(err, "too many blocks");
    }
    psd_w_init(&w, err);
    psd_put(&w, "ASEF", 4);
    psd_put_u16(&w, 1);
    psd_put_u16(&w, 0);
    psd_put_u32(&w, (uint32_t)blocks);
    psd_put(&w, bb.data, bb.len);
    pc_psd_bytes_fini(&bb);
    return psd_w_finish(&w, out);
}
