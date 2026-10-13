/* ICC reader and profile model (crates/cms/src/profile.rs). Every read is bounds-checked and
 * errors are raised in the reference's order, so the same file fails with the same message. */
#include "profile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/hash.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "cms_internal.h"

/* Upper bound on CLUT nodes x outputs accepted from a file. */
#define MAX_CLUT_VALUES ((uint64_t)16 * 1024 * 1024)

/* ---------------------------------------------------------------- helpers */

uint64_t pc_cms_hash_bytes(const uint8_t *b, size_t len)
{
    uint8_t h[PC_BLAKE3_LEN];
    pc_blake3(b, len, h);
    return pc_load_le64(h);
}

void pc_cms_debug_str(const uint8_t *b, size_t len, char *out, size_t cap)
{
    pc_utf8_debug_quote(b, len, out, cap);
}

/* Bounded big-endian reads at absolute offsets (the reference's Reader). */
typedef struct rd {
    const uint8_t *b;
    size_t len;
} rd;

static bool rd_slice(const rd *r, size_t off, size_t n, const uint8_t **out)
{
    size_t end;
    if (!pc_size_add(off, n, &end) || end > r->len) {
        return false;
    }
    *out = r->b + off;
    return true;
}

static bool rd_u8(const rd *r, size_t off, uint8_t *v)
{
    const uint8_t *p;
    if (!rd_slice(r, off, 1, &p)) {
        return false;
    }
    *v = p[0];
    return true;
}

static bool rd_u16(const rd *r, size_t off, uint16_t *v)
{
    const uint8_t *p;
    if (!rd_slice(r, off, 2, &p)) {
        return false;
    }
    *v = pc_load_be16(p);
    return true;
}

static bool rd_u32(const rd *r, size_t off, uint32_t *v)
{
    const uint8_t *p;
    if (!rd_slice(r, off, 4, &p)) {
        return false;
    }
    *v = pc_load_be32(p);
    return true;
}

static bool rd_s15(const rd *r, size_t off, double *v)
{
    uint32_t u;
    if (!rd_u32(r, off, &u)) {
        return false;
    }
    *v = (double)pc_i32_from_bits(u) / 65536.0;
    return true;
}

static bool rd_xyz(const rd *r, size_t off, double v[3])
{
    double a;
    double b;
    double c;
    if (!rd_s15(r, off, &a) || !rd_s15(r, off + 4, &b) || !rd_s15(r, off + 8, &c)) {
        return false;
    }
    v[0] = a;
    v[1] = b;
    v[2] = c;
    return true;
}

static bool is_type(const uint8_t *d, size_t len, const char *four)
{
    return len >= 4 && memcmp(d, four, 4) == 0;
}

static char *dup_text(const char *s, size_t len)
{
    return pc_strndup(s, len);
}

/* ---------------------------------------------------------------- model basics */

void pc_lut_free(pc_lut *l)
{
    if (l != NULL) {
        pc_cms_stages_fini(&l->stages);
        free(l);
    }
}

pc_status pc_profile_init_empty(pc_profile *p, pc_error *err)
{
    memset(p, 0, sizeof *p);
    p->description = dup_text("", 0);
    p->copyright = dup_text("", 0);
    if (p->description == NULL || p->copyright == NULL) {
        pc_profile_fini(p);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    p->trc[0] = pc_cms_curve_identity();
    p->trc[1] = pc_cms_curve_identity();
    p->trc[2] = pc_cms_curve_identity();
    p->gray_trc = pc_cms_curve_identity();
    memcpy(p->white_point, PC_D50, sizeof p->white_point);
    p->pcs = PC_PCS_XYZ;
    p->rendering_intent = PC_INTENT_PERCEPTUAL;
    return PC_OK;
}

void pc_profile_fini(pc_profile *p)
{
    int i;
    if (p == NULL) {
        return;
    }
    free(p->description);
    free(p->copyright);
    for (i = 0; i < 3; i++) {
        pc_cms_curve_fini(&p->trc[i]);
        pc_lut_free(p->a2b[i]);
        pc_lut_free(p->b2a[i]);
    }
    pc_cms_curve_fini(&p->gray_trc);
    free(p->bytes);
    memset(p, 0, sizeof *p);
}

static pc_status lut_clone(const pc_lut *l, pc_lut **out, pc_error *err)
{
    pc_lut *c;
    pc_status s;
    if (l == NULL) {
        *out = NULL;
        return PC_OK;
    }
    c = calloc(1, sizeof *c);
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *c = *l;
    s = pc_cms_stages_clone(&l->stages, &c->stages, err);
    if (s != PC_OK) {
        free(c);
        return s;
    }
    *out = c;
    return PC_OK;
}

pc_status pc_profile_clone(const pc_profile *p, pc_profile *out, pc_error *err)
{
    pc_profile c;
    pc_status s = PC_OK;
    int i;
    c = *p;
    /* Null every owned pointer first so a partial failure can be finalised safely. */
    c.description = NULL;
    c.copyright = NULL;
    c.bytes = NULL;
    for (i = 0; i < 3; i++) {
        c.trc[i].table = NULL;
        c.a2b[i] = NULL;
        c.b2a[i] = NULL;
    }
    c.gray_trc.table = NULL;
    c.description = pc_strndup(p->description, p->description_len);
    c.copyright = pc_strndup(p->copyright, p->copyright_len);
    if (c.description == NULL || c.copyright == NULL) {
        s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; s == PC_OK && i < 3; i++) {
        s = pc_cms_curve_clone(&p->trc[i], &c.trc[i], err);
    }
    if (s == PC_OK) {
        s = pc_cms_curve_clone(&p->gray_trc, &c.gray_trc, err);
    }
    for (i = 0; s == PC_OK && i < 3; i++) {
        s = lut_clone(p->a2b[i], &c.a2b[i], err);
        if (s == PC_OK) {
            s = lut_clone(p->b2a[i], &c.b2a[i], err);
        }
    }
    if (s == PC_OK && p->bytes != NULL) {
        c.bytes = pc_malloc(p->nbytes);
        if (c.bytes == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        } else {
            memcpy(c.bytes, p->bytes, p->nbytes);
        }
    }
    if (s != PC_OK) {
        pc_profile_fini(&c);
        return s;
    }
    *out = c;
    return PC_OK;
}

/* ---------------------------------------------------------------- curves */

/* Parse a curv or para element; *elem_len gets its unpadded byte length. */
static pc_status parse_curve(const uint8_t *d, size_t len, pc_cms_curve *out, size_t *elem_len, pc_error *err)
{
    rd r = {d, len};
    const uint8_t *ty;
    if (!rd_slice(&r, 0, 4, &ty)) {
        return cms_truncated(err);
    }
    if (memcmp(ty, "curv", 4) == 0) {
        uint32_t n;
        uint16_t g;
        if (!rd_u32(&r, 8, &n)) {
            return cms_truncated(err);
        }
        *elem_len = 12 + (size_t)n * 2;
        if (n == 0) {
            *out = pc_cms_curve_identity();
            return PC_OK;
        }
        if (n == 1) {
            if (!rd_u16(&r, 12, &g)) {
                return cms_truncated(err);
            }
            *out = pc_cms_curve_gamma((double)g / 256.0);
            return PC_OK;
        }
        if (n > (1u << 20)) {
            return cms_invalid(err, "curve too long");
        }
        {
            const uint8_t *s;
            float *t;
            size_t i;
            if (!rd_slice(&r, 12, (size_t)n * 2, &s)) {
                return cms_truncated(err);
            }
            t = pc_malloc_array(n, sizeof *t);
            if (t == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            for (i = 0; i < n; i++) {
                t[i] = (float)pc_load_be16(s + 2 * i) / 65535.0f;
            }
            *out = pc_cms_curve_table(t, n);
            return PC_OK;
        }
    }
    if (memcmp(ty, "para", 4) == 0) {
        uint16_t kind;
        size_t n;
        size_t i;
        double p[7] = {0, 0, 0, 0, 0, 0, 0};
        if (!rd_u16(&r, 8, &kind)) {
            return cms_truncated(err);
        }
        if (kind > 4) {
            return cms_unsupported(err, "parametric curve type %u", (unsigned)kind);
        }
        n = pc_cms_curve_param_count(kind);
        for (i = 0; i < n; i++) {
            if (!rd_s15(&r, 12 + i * 4, &p[i])) {
                return cms_truncated(err);
            }
        }
        *elem_len = 12 + n * 4;
        *out = kind == 0 ? pc_cms_curve_gamma(p[0]) : pc_cms_curve_parametric(kind, p);
        return PC_OK;
    }
    {
        char dbg[64];
        pc_cms_debug_str(ty, 4, dbg, sizeof dbg);
        return cms_unsupported(err, "curve type %s", dbg);
    }
}

static void curves_free(pc_cms_curve *c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        pc_cms_curve_fini(&c[i]);
    }
    free(c);
}

/* n consecutive (4-byte aligned) curve elements from offset off of d, as a curves stage. */
static pc_status curves_at(const uint8_t *d, size_t len, size_t off, size_t n, pc_cms_stages *st, pc_error *err)
{
    pc_cms_curve *c = pc_calloc_array(n, sizeof *c);
    pc_cms_stage stage;
    size_t o = off;
    size_t i;
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n; i++) {
        size_t elen = 0;
        pc_status s;
        if (o > len) {
            curves_free(c, i);
            return cms_truncated(err);
        }
        s = parse_curve(d + o, len - o, &c[i], &elen, err);
        if (s != PC_OK) {
            curves_free(c, i);
            return s;
        }
        if (!pc_size_add(elen, 3, &elen) || !pc_size_add(o, elen & ~(size_t)3, &o)) {
            o = SIZE_MAX; /* the next read reports truncation */
        }
    }
    stage = pc_cms_stage_curves(PC_STAGE_CURVES, c, n);
    return pc_cms_stages_push(st, &stage, err);
}

/* ---------------------------------------------------------------- LUT tags */

static pc_status read_tables(const rd *r, size_t *off, size_t count, size_t n, bool wide, pc_cms_stages *st,
                             pc_error *err)
{
    pc_cms_curve *c = pc_calloc_array(count, sizeof *c);
    pc_cms_stage stage;
    size_t bps = wide ? 2 : 1;
    size_t k;
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (k = 0; k < count; k++) {
        const uint8_t *s;
        float *t;
        size_t i;
        if (!rd_slice(r, *off, n * bps, &s)) {
            curves_free(c, k);
            return cms_truncated(err);
        }
        t = pc_malloc_array(n, sizeof *t);
        if (t == NULL) {
            curves_free(c, k);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < n; i++) {
            t[i] = wide ? (float)pc_load_be16(s + 2 * i) / 65535.0f : (float)s[i] / 255.0f;
        }
        c[k] = pc_cms_curve_table(t, n);
        *off += n * bps;
    }
    stage = pc_cms_stage_curves(PC_STAGE_CURVES, c, count);
    return pc_cms_stages_push(st, &stage, err);
}

/* CLUT node values: count values of 1 or 2 bytes from s. */
static float *read_values(const uint8_t *s, size_t count, bool wide)
{
    float *v = pc_malloc_array(count, sizeof *v);
    size_t i;
    if (v == NULL) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        v[i] = wide ? (float)pc_load_be16(s + 2 * i) / 65535.0f : (float)s[i] / 255.0f;
    }
    return v;
}

static pc_status parse_mft(const rd *r, bool wide, bool a2b, pc_lut *lut, pc_error *err)
{
    uint8_t i8;
    uint8_t o8;
    uint8_t g8;
    size_t i;
    size_t o;
    size_t g;
    pc_mat3 m;
    size_t n_in = 256;
    size_t n_out = 256;
    size_t off = 48;
    size_t bps = wide ? 2 : 1;
    uint64_t nodes = 1;
    size_t total;
    size_t a;
    size_t b;
    const uint8_t *s;
    float *data;
    pc_cms_stages st = PC_CMS_STAGES_INIT;
    pc_cms_stage stage;
    pc_cms_clut clut;
    size_t grid[PC_CLUT_MAX_CHANNELS];
    bool ident = true;
    pc_status ps;

    if (!rd_u8(r, 8, &i8) || !rd_u8(r, 9, &o8) || !rd_u8(r, 10, &g8)) {
        return cms_truncated(err);
    }
    i = i8;
    o = o8;
    g = g8;
    if (i < 1 || i > 15 || o < 1 || o > 15 || g == 0) {
        return cms_invalid(err, "bad lut dimensions");
    }
    for (a = 0; a < 3; a++) {
        for (b = 0; b < 3; b++) {
            if (!rd_s15(r, 12 + (a * 3 + b) * 4, &m.m[a][b])) {
                return cms_truncated(err);
            }
        }
    }
    if (wide) {
        uint16_t ni;
        uint16_t no;
        if (!rd_u16(r, 48, &ni) || !rd_u16(r, 50, &no)) {
            return cms_truncated(err);
        }
        n_in = ni;
        n_out = no;
        off = 52;
        if (n_in < 2 || n_in > 4096 || n_out < 2 || n_out > 4096) {
            return cms_invalid(err, "bad lut16 table size");
        }
    }
    /* Read the input tables first (as the reference does) into a scratch list. */
    {
        pc_cms_stages in_st = PC_CMS_STAGES_INIT;
        ps = read_tables(r, &off, i, n_in, wide, &in_st, err);
        if (ps != PC_OK) {
            return ps;
        }
        for (a = 0; a < i; a++) {
            if (nodes <= MAX_CLUT_VALUES) {
                nodes *= g; /* stays far below 2^64 while <= the cap */
            }
        }
        if (nodes > MAX_CLUT_VALUES || nodes * o > MAX_CLUT_VALUES) {
            pc_cms_stages_fini(&in_st);
            return cms_invalid(err, "CLUT too large");
        }
        total = (size_t)nodes * o;
        if (!rd_slice(r, off, total * bps, &s)) {
            pc_cms_stages_fini(&in_st);
            return cms_truncated(err);
        }
        data = read_values(s, total, wide);
        if (data == NULL) {
            pc_cms_stages_fini(&in_st);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        off += total * bps;
        for (a = 0; a < i; a++) {
            grid[a] = g;
        }
        ps = pc_cms_clut_init(&clut, grid, i, o, data, total, err);
        if (ps == PC_OK) {
            pc_cms_stages out_st = PC_CMS_STAGES_INIT;
            ps = read_tables(r, &off, o, n_out, wide, &out_st, err);
            if (ps != PC_OK) {
                pc_cms_clut_fini(&clut);
            } else {
                /* The matrix only applies when the input is XYZ (BToA with an XYZ PCS); for
                 * a Lab PCS writers must store the identity, so keep it when it isn't. */
                for (a = 0; a < 3; a++) {
                    for (b = 0; b < 3; b++) {
                        if (!(fabs(m.m[a][b] - (a == b ? 1.0 : 0.0)) < 1e-6)) {
                            ident = false;
                        }
                    }
                }
                if (!a2b && i == 3 && !ident) {
                    ps = pc_cms_stage_matrix3(&m, &stage, err);
                    if (ps == PC_OK) {
                        ps = pc_cms_stages_push(&st, &stage, err);
                    }
                }
                if (ps == PC_OK) {
                    ps = pc_cms_stages_append(&st, &in_st, err);
                }
                stage = pc_cms_stage_clut(&clut);
                if (ps == PC_OK) {
                    ps = pc_cms_stages_push(&st, &stage, err);
                } else {
                    pc_cms_stage_fini(&stage);
                }
                if (ps == PC_OK) {
                    ps = pc_cms_stages_append(&st, &out_st, err);
                }
                pc_cms_stages_fini(&out_st);
            }
        }
        pc_cms_stages_fini(&in_st);
    }
    if (ps != PC_OK) {
        pc_cms_stages_fini(&st);
        return ps;
    }
    lut->kind = wide ? PC_LUT16 : PC_LUT8;
    lut->inputs = i;
    lut->outputs = o;
    lut->stages = st;
    return PC_OK;
}

static pc_status ab_matrix(const rd *r, size_t off, pc_cms_stages *st, pc_error *err)
{
    double m[9];
    double offset[3];
    size_t k;
    pc_cms_stage stage;
    pc_status s;
    for (k = 0; k < 9; k++) {
        if (!rd_s15(r, off + k * 4, &m[k])) {
            return cms_truncated(err);
        }
    }
    for (k = 0; k < 3; k++) {
        if (!rd_s15(r, off + 36 + k * 4, &offset[k])) {
            return cms_truncated(err);
        }
    }
    s = pc_cms_stage_matrix(3, 3, m, offset, &stage, err);
    return s == PC_OK ? pc_cms_stages_push(st, &stage, err) : s;
}

static pc_status ab_clut(const rd *r, size_t off, size_t cin, size_t cout, pc_cms_stages *st, pc_error *err)
{
    size_t grid[PC_CLUT_MAX_CHANNELS];
    uint8_t prec;
    uint64_t nodes = 1;
    size_t total;
    size_t k;
    const uint8_t *s;
    float *data;
    pc_cms_clut clut;
    pc_cms_stage stage;
    pc_status ps;
    for (k = 0; k < cin; k++) {
        uint8_t gk;
        if (!rd_u8(r, off + k, &gk)) {
            return cms_truncated(err);
        }
        if (gk < 1) {
            return cms_invalid(err, "zero grid points");
        }
        grid[k] = gk;
    }
    if (!rd_u8(r, off + 16, &prec)) {
        return cms_truncated(err);
    }
    for (k = 0; k < cin; k++) {
        if (nodes <= MAX_CLUT_VALUES) {
            nodes *= grid[k];
        }
    }
    if (nodes > MAX_CLUT_VALUES || nodes * cout > MAX_CLUT_VALUES) {
        return cms_invalid(err, "CLUT too large");
    }
    total = (size_t)nodes * cout;
    if (prec == 1 || prec == 2) {
        if (!rd_slice(r, off + 20, total * prec, &s)) {
            return cms_truncated(err);
        }
        data = read_values(s, total, prec == 2);
        if (data == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    } else {
        return cms_invalid(err, "CLUT precision %u", (unsigned)prec);
    }
    ps = pc_cms_clut_init(&clut, grid, cin, cout, data, total, err);
    if (ps != PC_OK) {
        return ps;
    }
    stage = pc_cms_stage_clut(&clut);
    return pc_cms_stages_push(st, &stage, err);
}

static pc_status parse_ab(const rd *r, bool is_mab, pc_lut *lut, pc_error *err)
{
    uint8_t i8;
    uint8_t o8;
    uint32_t ob;
    uint32_t om;
    uint32_t omc;
    uint32_t oc;
    uint32_t oa;
    size_t i;
    size_t o;
    pc_cms_stages st = PC_CMS_STAGES_INIT;
    pc_status s = PC_OK;
    if (!rd_u8(r, 8, &i8) || !rd_u8(r, 9, &o8)) {
        return cms_truncated(err);
    }
    i = i8;
    o = o8;
    if (i < 1 || i > 15 || o < 1 || o > 15) {
        return cms_invalid(err, "bad lutAB dimensions");
    }
    if (!rd_u32(r, 12, &ob) || !rd_u32(r, 16, &om) || !rd_u32(r, 20, &omc) || !rd_u32(r, 24, &oc) ||
        !rd_u32(r, 28, &oa)) {
        return cms_truncated(err);
    }
    if (is_mab) {
        /* A curves -> CLUT -> M curves -> matrix -> B curves. */
        if (oc != 0) {
            if (oa == 0) {
                return cms_invalid(err, "lutAtoB with CLUT but no A curves");
            }
            s = curves_at(r->b, r->len, oa, i, &st, err);
            if (s == PC_OK) {
                s = ab_clut(r, oc, i, o, &st, err);
            }
        } else if (i != o) {
            return cms_invalid(err, "lutAtoB without CLUT changes channel count");
        }
        if (s == PC_OK && om != 0) {
            s = curves_at(r->b, r->len, omc, o, &st, err);
            if (s == PC_OK) {
                s = ab_matrix(r, om, &st, err);
            }
        }
        if (s == PC_OK && ob == 0) {
            s = cms_invalid(err, "lutAtoB without B curves");
        }
        if (s == PC_OK) {
            s = curves_at(r->b, r->len, ob, o, &st, err);
        }
    } else {
        /* B curves -> matrix -> M curves -> CLUT -> A curves. */
        if (ob == 0) {
            return cms_invalid(err, "lutBtoA without B curves");
        }
        s = curves_at(r->b, r->len, ob, i, &st, err);
        if (s == PC_OK && om != 0) {
            s = ab_matrix(r, om, &st, err);
            if (s == PC_OK) {
                s = curves_at(r->b, r->len, omc, i, &st, err);
            }
        }
        if (s == PC_OK) {
            if (oc != 0) {
                if (oa == 0) {
                    s = cms_invalid(err, "lutBtoA with CLUT but no A curves");
                } else {
                    s = ab_clut(r, oc, i, o, &st, err);
                    if (s == PC_OK) {
                        s = curves_at(r->b, r->len, oa, o, &st, err);
                    }
                }
            } else if (i != o) {
                s = cms_invalid(err, "lutBtoA without CLUT changes channel count");
            }
        }
    }
    if (s != PC_OK) {
        pc_cms_stages_fini(&st);
        return s;
    }
    lut->kind = PC_LUT_AB;
    lut->inputs = i;
    lut->outputs = o;
    lut->stages = st;
    return PC_OK;
}

static pc_status parse_lut(const uint8_t *d, size_t len, bool a2b, pc_lut **out, pc_error *err)
{
    rd r = {d, len};
    const uint8_t *ty;
    pc_lut *lut;
    pc_status s;
    if (!rd_slice(&r, 0, 4, &ty)) {
        return cms_truncated(err);
    }
    lut = calloc(1, sizeof *lut);
    if (lut == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (memcmp(ty, "mft1", 4) == 0 || memcmp(ty, "mft2", 4) == 0) {
        s = parse_mft(&r, ty[3] == '2', a2b, lut, err);
    } else if (memcmp(ty, "mAB ", 4) == 0 || memcmp(ty, "mBA ", 4) == 0) {
        s = parse_ab(&r, ty[1] == 'A', lut, err);
    } else {
        char dbg[64];
        pc_cms_debug_str(ty, 4, dbg, sizeof dbg);
        s = cms_unsupported(err, "LUT type %s", dbg);
    }
    if (s != PC_OK) {
        free(lut);
        return s;
    }
    *out = lut;
    return PC_OK;
}

/* ---------------------------------------------------------------- text */

/* Bytes up to the first NUL, lossily decoded and trimmed. */
static pc_status ascii_text(const uint8_t *s, size_t n, pc_buf *out, pc_error *err)
{
    const uint8_t *nul = n > 0 ? memchr(s, 0, n) : NULL;
    pc_buf tmp = PC_BUF_INIT;
    size_t start;
    size_t len;
    pc_status st = pc_utf8_lossy(s, nul != NULL ? (size_t)(nul - s) : n, &tmp, err);
    if (st == PC_OK) {
        len = pc_utf8_trim(pc_buf_cstr(&tmp), tmp.len, &start);
        st = pc_buf_append(out, tmp.data != NULL ? tmp.data + start : (const uint8_t *)"", len, err);
    }
    pc_buf_fini(&tmp);
    return st;
}

/* Text from desc (textDescriptionType), mluc (English or first record) or text. Unreadable
 * text gives "" (never an error), as in the reference. */
static pc_status parse_text(const uint8_t *d, size_t len, pc_buf *out, pc_error *err)
{
    rd r = {d, len};
    const uint8_t *ty;
    if (!rd_slice(&r, 0, 4, &ty)) {
        return PC_OK;
    }
    if (memcmp(ty, "desc", 4) == 0) {
        uint32_t n = 0;
        const uint8_t *s;
        (void)rd_u32(&r, 8, &n);
        return rd_slice(&r, 12, n, &s) ? ascii_text(s, n, out, err) : PC_OK;
    }
    if (memcmp(ty, "text", 4) == 0) {
        return ascii_text(len > 8 ? d + 8 : d, len > 8 ? len - 8 : 0, out, err);
    }
    if (memcmp(ty, "mluc", 4) == 0) {
        uint32_t n;
        uint32_t size;
        bool have = false;
        size_t best_off = 0;
        size_t best_len = 0;
        size_t k;
        const uint8_t *s;
        pc_buf tmp = PC_BUF_INIT;
        pc_status st;
        size_t start;
        size_t tl;
        if (!rd_u32(&r, 8, &n) || !rd_u32(&r, 12, &size)) {
            return PC_OK;
        }
        for (k = 0; k < (n < 256 ? n : 256); k++) {
            uint16_t lang;
            uint32_t l;
            uint32_t o;
            size_t at;
            if (!pc_size_mul(k, size, &at) || !pc_size_add(at, 16, &at) || !rd_u16(&r, at, &lang) ||
                !rd_u32(&r, at + 4, &l) || !rd_u32(&r, at + 8, &o)) {
                break;
            }
            if (!have || lang == PC_SIG(0, 0, 'e', 'n')) {
                have = true;
                best_off = o;
                best_len = l;
                if (lang == PC_SIG(0, 0, 'e', 'n')) {
                    break;
                }
            }
        }
        if (!have || !rd_slice(&r, best_off, best_len, &s)) {
            return PC_OK;
        }
        st = pc_utf16be_lossy(s, best_len / 2, &tmp, err);
        if (st == PC_OK) {
            size_t l = tmp.len;
            while (l > 0 && tmp.data[l - 1] == 0) { /* trim_end_matches('\0') */
                l--;
            }
            tl = pc_utf8_trim(pc_buf_cstr(&tmp), l, &start);
            st = pc_buf_append(out, tmp.data != NULL ? tmp.data + start : (const uint8_t *)"", tl, err);
        }
        pc_buf_fini(&tmp);
        return st;
    }
    return PC_OK;
}

/* ---------------------------------------------------------------- the reader */

typedef struct tag {
    uint32_t sig;
    size_t off;
    size_t len;
} tag;

typedef struct tags {
    const rd *r;
    tag *v;
    size_t n;
} tags;

/* Tag data: *found false when absent; truncated slices are an error. */
static pc_status tag_data(const tags *t, uint32_t sig, const uint8_t **d, size_t *len, bool *found, pc_error *err)
{
    size_t i;
    *found = false;
    for (i = 0; i < t->n; i++) {
        if (t->v[i].sig == sig) {
            if (!rd_slice(t->r, t->v[i].off, t->v[i].len, d)) {
                return cms_truncated(err);
            }
            *len = t->v[i].len;
            *found = true;
            return PC_OK;
        }
    }
    return PC_OK;
}

static pc_status xyz_tag(const tags *t, const char *name, double out[3], bool *found, pc_error *err)
{
    const uint8_t *d;
    size_t len;
    pc_status s = tag_data(t, PC_SIG(name[0], name[1], name[2], name[3]), &d, &len, found, err);
    rd r;
    if (s != PC_OK || !*found) {
        return s;
    }
    r.b = d;
    r.len = len;
    if (len >= 20 && is_type(d, len, "XYZ ")) {
        return rd_xyz(&r, 8, out) ? PC_OK : cms_truncated(err);
    }
    return cms_invalid(err, "tag %.4s is not XYZType", name);
}

static pc_status curve_tag(const tags *t, const char *name, pc_cms_curve *out, bool *found, pc_error *err)
{
    const uint8_t *d;
    size_t len;
    size_t elen;
    pc_status s = tag_data(t, PC_SIG(name[0], name[1], name[2], name[3]), &d, &len, found, err);
    if (s != PC_OK || !*found) {
        return s;
    }
    return parse_curve(d, len, out, &elen, err);
}

static pc_status lut_tag(const tags *t, const char *name, bool a2b, pc_lut **out, pc_error *err)
{
    const uint8_t *d;
    size_t len;
    bool found;
    pc_status s = tag_data(t, PC_SIG(name[0], name[1], name[2], name[3]), &d, &len, &found, err);
    *out = NULL;
    if (s != PC_OK || !found) {
        return s;
    }
    return parse_lut(d, len, a2b, out, err);
}

static pc_status text_tag(const tags *t, const char *name, char **out, size_t *out_len, pc_error *err)
{
    const uint8_t *d;
    size_t len;
    bool found;
    pc_buf b = PC_BUF_INIT;
    pc_status s = tag_data(t, PC_SIG(name[0], name[1], name[2], name[3]), &d, &len, &found, err);
    if (s == PC_OK && found) {
        s = parse_text(d, len, &b, err);
    }
    if (s == PC_OK) {
        char *str = pc_strndup(pc_buf_cstr(&b), b.len);
        if (str == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        } else {
            free(*out);
            *out = str;
            *out_len = b.len;
        }
    }
    pc_buf_fini(&b);
    return s;
}

static pc_status parse_profile(const uint8_t *bytes, size_t nbytes, pc_profile *p, pc_error *err)
{
    rd whole = {bytes, nbytes};
    rd r;
    tags t;
    uint32_t declared = 0;
    uint32_t v = 0;
    uint32_t pcs_sig = 0;
    uint32_t count = 0;
    const uint8_t *sp;
    size_t i;
    bool found;
    bool f1;
    bool f2;
    bool f3;
    double rx[3];
    double gx[3];
    double bx[3];
    pc_status s;
    int k;
    char csn[32];

    if (nbytes < 132) {
        return cms_truncated(err);
    }
    if (!rd_slice(&whole, 36, 4, &sp) || memcmp(sp, "acsp", 4) != 0) {
        return cms_bad_signature(err);
    }
    (void)rd_u32(&whole, 0, &declared);
    /* Some writers pad or truncate; trust the smaller of the two. */
    r.b = bytes;
    r.len = (declared >= 132 && declared <= nbytes) ? declared : nbytes;
    p->version_major = bytes[8];
    p->version_minor = bytes[9];
    (void)rd_u32(&r, 12, &p->class_sig);
    (void)rd_u32(&r, 16, &p->space);
    (void)rd_u32(&r, 20, &pcs_sig);
    p->pcs = pcs_sig == PC_SIG('L', 'a', 'b', ' ') ? PC_PCS_LAB : PC_PCS_XYZ;
    (void)rd_u32(&r, 64, &v);
    if (!pc_intent_from_u32(v & 0xFFFFu, &p->rendering_intent)) {
        p->rendering_intent = PC_INTENT_PERCEPTUAL;
    }
    (void)rd_u32(&r, 128, &count);
    if (count > 1024) {
        return cms_invalid(err, "%lu tags", (unsigned long)count);
    }
    t.r = &r;
    t.n = count;
    t.v = pc_calloc_array(count, sizeof *t.v);
    if (t.v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < count; i++) {
        size_t o = 132 + i * 12;
        uint32_t sig;
        uint32_t off;
        uint32_t len;
        if (!rd_u32(&r, o, &sig) || !rd_u32(&r, o + 4, &off) || !rd_u32(&r, o + 8, &len)) {
            free(t.v);
            return cms_truncated(err);
        }
        t.v[i].sig = sig;
        t.v[i].off = off;
        t.v[i].len = len;
    }

    s = xyz_tag(&t, "wtpt", p->white_point, &found, err);
    if (s == PC_OK && !found) {
        memcpy(p->white_point, PC_D50, sizeof p->white_point);
    }
    if (s == PC_OK) {
        /* bkpt errors are ignored. */
        pc_error ignored;
        double bk[3];
        p->has_black_point = xyz_tag(&t, "bkpt", bk, &found, &ignored) == PC_OK && found;
        if (p->has_black_point) {
            memcpy(p->black_point, bk, sizeof bk);
        }
    }
    if (s == PC_OK) {
        const uint8_t *d;
        size_t len;
        s = tag_data(&t, PC_SIG('c', 'h', 'a', 'd'), &d, &len, &found, err);
        if (s == PC_OK && found && len >= 44 && is_type(d, len, "sf32")) {
            rd cr = {d, len};
            int a;
            int b;
            p->has_chad = true;
            for (a = 0; a < 3 && s == PC_OK; a++) {
                for (b = 0; b < 3 && s == PC_OK; b++) {
                    if (!rd_s15(&cr, 8 + (size_t)(a * 3 + b) * 4, &p->chad.m[a][b])) {
                        s = cms_truncated(err);
                    }
                }
            }
        }
    }
    if (s == PC_OK) {
        s = xyz_tag(&t, "rXYZ", rx, &f1, err);
    }
    if (s == PC_OK) {
        s = xyz_tag(&t, "gXYZ", gx, &f2, err);
    }
    if (s == PC_OK) {
        s = xyz_tag(&t, "bXYZ", bx, &f3, err);
    }
    if (s == PC_OK && f1 && f2 && f3) {
        p->has_matrix = true;
        for (k = 0; k < 3; k++) {
            p->matrix.m[k][0] = rx[k];
            p->matrix.m[k][1] = gx[k];
            p->matrix.m[k][2] = bx[k];
        }
    }
    if (s == PC_OK) {
        pc_cms_curve c[3];
        static const char *const names[3] = {"rTRC", "gTRC", "bTRC"};
        bool f[3] = {false, false, false};
        for (k = 0; k < 3; k++) {
            c[k] = pc_cms_curve_identity();
        }
        for (k = 0; k < 3 && s == PC_OK; k++) {
            s = curve_tag(&t, names[k], &c[k], &f[k], err);
        }
        if (s == PC_OK && f[0] && f[1] && f[2]) {
            p->has_trc = true;
            for (k = 0; k < 3; k++) {
                pc_cms_curve_fini(&p->trc[k]);
                p->trc[k] = c[k];
            }
        } else {
            for (k = 0; k < 3; k++) {
                pc_cms_curve_fini(&c[k]);
            }
        }
    }
    if (s == PC_OK) {
        pc_cms_curve c = pc_cms_curve_identity();
        s = curve_tag(&t, "kTRC", &c, &found, err);
        if (s == PC_OK && found) {
            p->has_gray_trc = true;
            pc_cms_curve_fini(&p->gray_trc);
            p->gray_trc = c;
        }
    }
    {
        static const char *const an[3] = {"A2B0", "A2B1", "A2B2"};
        static const char *const bn[3] = {"B2A0", "B2A1", "B2A2"};
        for (k = 0; k < 3 && s == PC_OK; k++) {
            s = lut_tag(&t, an[k], true, &p->a2b[k], err);
        }
        for (k = 0; k < 3 && s == PC_OK; k++) {
            s = lut_tag(&t, bn[k], false, &p->b2a[k], err);
        }
    }
    if (s == PC_OK) {
        s = text_tag(&t, "desc", &p->description, &p->description_len, err);
    }
    if (s == PC_OK) {
        s = text_tag(&t, "cprt", &p->copyright, &p->copyright_len, err);
    }
    free(t.v);
    if (s != PC_OK) {
        return s;
    }
    /* Validate LUT channel counts against the header. A DeviceLink connects two device spaces:
     * its header PCS field holds the output colour space (ICC.1:2010 7.2.7), so a CMYK->CMYK link
     * has a 4->4 AToB0. */
    {
        size_t dev = pc_cs_channels(p->space);
        size_t conn = p->class_sig == PC_CLASS_DEVICE_LINK ? pc_cs_channels(pcs_sig) : 3;
        pc_cs_debug_name(p->space, csn);
        for (k = 0; k < 3; k++) {
            const pc_lut *l = p->a2b[k];
            if (l != NULL && ((dev != 0 && l->inputs != dev) || (conn != 0 && l->outputs != conn))) {
                return cms_invalid(err, "AToB tag has %zu\xe2\x86\x92%zu channels for a %s profile", l->inputs,
                                   l->outputs, csn);
            }
        }
        for (k = 0; k < 3; k++) {
            const pc_lut *l = p->b2a[k];
            if (l != NULL && ((conn != 0 && l->inputs != conn) || (dev != 0 && l->outputs != dev))) {
                return cms_invalid(err, "BToA tag has %zu\xe2\x86\x92%zu channels for a %s profile", l->inputs,
                                   l->outputs, csn);
            }
        }
    }
    return PC_OK;
}

pc_status pc_profile_parse(const uint8_t *data, size_t len, pc_profile *out, pc_error *err)
{
    pc_profile p;
    pc_status s = pc_profile_init_empty(&p, err);
    if (s != PC_OK) {
        return s;
    }
    if (data == NULL && len > 0) {
        pc_profile_fini(&p);
        return pc_fail(err, PC_ERR_INVALID, "no ICC data");
    }
    s = parse_profile(data, len, &p, err);
    if (s == PC_OK) {
        p.bytes = pc_malloc(len);
        if (p.bytes == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        } else {
            if (len > 0) {
                memcpy(p.bytes, data, len);
            }
            p.nbytes = len;
            p.hash = pc_cms_hash_bytes(p.bytes, len);
        }
    }
    if (s != PC_OK) {
        pc_profile_fini(&p);
        return s;
    }
    *out = p;
    return PC_OK;
}

/* ---------------------------------------------------------------- bytes */

pc_status pc_profile_encode_bytes(pc_profile *p, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status s = pc_icc_encode(p, &b, err);
    if (s != PC_OK) {
        pc_buf_fini(&b);
        return s;
    }
    free(p->bytes);
    p->bytes = pc_buf_take(&b, &p->nbytes);
    p->hash = pc_cms_hash_bytes(p->bytes, p->nbytes);
    return PC_OK;
}

pc_status pc_profile_to_bytes(const pc_profile *p, pc_buf *out, pc_error *err)
{
    if (p->bytes != NULL) {
        return pc_buf_append(out, p->bytes, p->nbytes, err);
    }
    return pc_icc_encode(p, out, err);
}

pc_status pc_profile_content_hash(const pc_profile *p, uint64_t *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status s;
    if (p->bytes != NULL) {
        *out = p->hash;
        return PC_OK;
    }
    s = pc_icc_encode(p, &b, err);
    if (s == PC_OK) {
        *out = pc_cms_hash_bytes(b.data, b.len);
    }
    pc_buf_fini(&b);
    return s;
}

/* ---------------------------------------------------------------- model queries */

size_t pc_profile_channels(const pc_profile *p)
{
    return pc_cs_channels(p->space);
}

void pc_profile_media_white(const pc_profile *p, double out[3])
{
    if (p->version_major < 4 && p->class_sig == PC_CLASS_DISPLAY) {
        memcpy(out, PC_D50, 3 * sizeof *out);
    } else {
        memcpy(out, p->white_point, 3 * sizeof *out);
    }
}

pc_status pc_profile_gray_as_rgb(const pc_profile *p, pc_profile *out, bool *ok, pc_error *err)
{
    pc_profile c;
    pc_status s;
    pc_buf desc = PC_BUF_INIT;
    int k;
    int j;
    *ok = false;
    if (!p->has_gray_trc || p->pcs != PC_PCS_XYZ) {
        return PC_OK;
    }
    s = pc_profile_clone(p, &c, err);
    if (s != PC_OK) {
        return s;
    }
    c.space = PC_CS_RGB;
    s = pc_buf_append(&desc, p->description, p->description_len, err);
    if (s == PC_OK) {
        s = pc_buf_append_str(&desc, " (as RGB)", err);
    }
    if (s == PC_OK) {
        free(c.description);
        c.description_len = desc.len;
        c.description = (char *)pc_buf_take(&desc, NULL);
    }
    pc_buf_fini(&desc);
    for (k = 0; k < 3; k++) {
        for (j = 0; j < 3; j++) {
            c.matrix.m[k][j] = PC_D50[k] / 3.0;
        }
    }
    c.has_matrix = true;
    for (k = 0; k < 3 && s == PC_OK; k++) {
        pc_cms_curve_fini(&c.trc[k]);
        c.trc[k] = pc_cms_curve_identity();
        s = pc_cms_curve_clone(&p->gray_trc, &c.trc[k], err);
    }
    c.has_trc = true;
    pc_cms_curve_fini(&c.gray_trc);
    c.gray_trc = pc_cms_curve_identity();
    c.has_gray_trc = false;
    /* The gray LUTs take one input channel; this profile is the matrix/TRC model above. */
    for (k = 0; k < 3; k++) {
        pc_lut_free(c.a2b[k]);
        pc_lut_free(c.b2a[k]);
        c.a2b[k] = NULL;
        c.b2a[k] = NULL;
    }
    free(c.bytes);
    c.bytes = NULL;
    c.nbytes = 0;
    if (s != PC_OK) {
        pc_profile_fini(&c);
        return s;
    }
    *out = c;
    *ok = true;
    return PC_OK;
}

bool pc_profile_is_matrix_shaper(const pc_profile *p)
{
    return p->a2b[0] == NULL && p->a2b[1] == NULL && p->a2b[2] == NULL &&
           ((p->has_matrix && p->has_trc) || p->has_gray_trc);
}

static int intent_slot(pc_intent intent)
{
    switch (intent) {
    case PC_INTENT_PERCEPTUAL: return 0;
    case PC_INTENT_SATURATION: return 2;
    case PC_INTENT_RELATIVE:
    case PC_INTENT_ABSOLUTE: break;
    }
    return 1;
}

static const pc_lut *lut_for(pc_lut *const luts[3], pc_intent intent)
{
    const pc_lut *l = luts[intent_slot(intent)];
    return l != NULL ? l : luts[0];
}

bool pc_profile_supports_intent(const pc_profile *p, pc_intent intent, bool input)
{
    pc_lut *const *luts = input ? p->a2b : p->b2a;
    return luts[intent_slot(intent)] != NULL || (luts[0] == NULL && luts[1] == NULL && luts[2] == NULL);
}

static pc_status push_simple(pc_cms_stages *st, pc_cms_stage_kind kind, pc_error *err)
{
    pc_cms_stage s = pc_cms_stage_simple(kind);
    return pc_cms_stages_push(st, &s, err);
}

static pc_status push_curves(pc_cms_stages *st, pc_cms_stage_kind kind, const pc_cms_curve *c, size_t n,
                             pc_error *err)
{
    pc_cms_stage s;
    pc_status ps = pc_cms_stage_curves_copy(kind, c, n, &s, err);
    return ps == PC_OK ? pc_cms_stages_push(st, &s, err) : ps;
}

static pc_status push_matrix(pc_cms_stages *st, size_t rows, size_t cols, const double *m, const double *off,
                             pc_error *err)
{
    pc_cms_stage s;
    pc_status ps = pc_cms_stage_matrix(rows, cols, m, off, &s, err);
    return ps == PC_OK ? pc_cms_stages_push(st, &s, err) : ps;
}

pc_status pc_profile_device_to_pcs(const pc_profile *p, pc_intent intent, pc_cms_stages *out, pc_pcs *pcs,
                                   pc_error *err)
{
    pc_cms_stages st = PC_CMS_STAGES_INIT;
    const pc_lut *l = lut_for(p->a2b, intent);
    pc_pcs result = p->pcs;
    pc_status s;
    if (l != NULL) {
        s = pc_cms_stages_append_copy(&st, &l->stages, err);
        if (s == PC_OK) {
            s = push_simple(&st, p->pcs == PC_PCS_XYZ ? PC_STAGE_DECODE_XYZ
                                 : l->kind == PC_LUT16 ? PC_STAGE_DECODE_LAB_V2
                                                       : PC_STAGE_DECODE_LAB_V4,
                            err);
        }
    } else if (p->space == PC_CS_RGB) {
        if (!p->has_matrix || !p->has_trc) {
            return cms_unsupported(err, "RGB profile without matrix/TRC or AToB tags");
        }
        s = push_curves(&st, PC_STAGE_CURVES, p->trc, 3, err);
        if (s == PC_OK) {
            s = push_matrix(&st, 3, 3, &p->matrix.m[0][0], NULL, err);
        }
        result = PC_PCS_XYZ;
    } else if (p->space == PC_CS_GRAY) {
        if (!p->has_gray_trc) {
            return cms_unsupported(err, "gray profile without kTRC");
        }
        s = push_curves(&st, PC_STAGE_CURVES, &p->gray_trc, 1, err);
        if (p->pcs == PC_PCS_LAB) {
            static const double m[3] = {100.0, 0.0, 0.0}; /* the curve yields L-star / 100 */
            if (s == PC_OK) {
                s = push_matrix(&st, 3, 1, m, NULL, err);
            }
            result = PC_PCS_LAB;
        } else {
            if (s == PC_OK) {
                s = push_matrix(&st, 3, 1, PC_D50, NULL, err);
            }
            result = PC_PCS_XYZ;
        }
    } else if (p->space == PC_CS_LAB) {
        s = push_simple(&st, PC_STAGE_DECODE_LAB_V4, err);
        result = PC_PCS_LAB;
    } else if (p->space == PC_CS_XYZ) {
        s = push_simple(&st, PC_STAGE_DECODE_XYZ, err);
        result = PC_PCS_XYZ;
    } else {
        char csn[32];
        pc_cs_debug_name(p->space, csn);
        return cms_unsupported(err, "%s profile without AToB tags", csn);
    }
    if (s != PC_OK) {
        pc_cms_stages_fini(&st);
        return s;
    }
    *out = st;
    *pcs = result;
    return PC_OK;
}

pc_status pc_profile_pcs_to_device(const pc_profile *p, pc_intent intent, pc_cms_stages *out, pc_pcs *pcs,
                                   pc_error *err)
{
    pc_cms_stages st = PC_CMS_STAGES_INIT;
    const pc_lut *l = lut_for(p->b2a, intent);
    pc_pcs result = p->pcs;
    pc_status s;
    if (l != NULL) {
        s = push_simple(&st, p->pcs == PC_PCS_XYZ ? PC_STAGE_ENCODE_XYZ
                             : l->kind == PC_LUT16 ? PC_STAGE_ENCODE_LAB_V2
                                                   : PC_STAGE_ENCODE_LAB_V4,
                        err);
        if (s == PC_OK) {
            s = pc_cms_stages_append_copy(&st, &l->stages, err);
        }
    } else if (p->space == PC_CS_RGB) {
        pc_mat3 inv;
        if (!p->has_matrix || !p->has_trc) {
            return cms_unsupported(err, "RGB profile without matrix/TRC or BToA tags");
        }
        if (!pc_mat3_invert(&p->matrix, &inv)) {
            return cms_invalid(err, "singular colorant matrix");
        }
        s = push_matrix(&st, 3, 3, &inv.m[0][0], NULL, err);
        if (s == PC_OK) {
            s = push_curves(&st, PC_STAGE_INV_CURVES, p->trc, 3, err);
        }
        result = PC_PCS_XYZ;
    } else if (p->space == PC_CS_GRAY) {
        static const double off0[1] = {0.0};
        if (!p->has_gray_trc) {
            return cms_unsupported(err, "gray profile without kTRC");
        }
        if (p->pcs == PC_PCS_LAB) {
            static const double m[3] = {0.01, 0.0, 0.0};
            s = push_matrix(&st, 1, 3, m, off0, err);
            result = PC_PCS_LAB;
        } else {
            static const double m[3] = {0.0, 1.0, 0.0};
            s = push_matrix(&st, 1, 3, m, off0, err);
            result = PC_PCS_XYZ;
        }
        if (s == PC_OK) {
            s = push_curves(&st, PC_STAGE_INV_CURVES, &p->gray_trc, 1, err);
        }
    } else if (p->space == PC_CS_LAB) {
        s = push_simple(&st, PC_STAGE_ENCODE_LAB_V4, err);
        result = PC_PCS_LAB;
    } else if (p->space == PC_CS_XYZ) {
        s = push_simple(&st, PC_STAGE_ENCODE_XYZ, err);
        result = PC_PCS_XYZ;
    } else {
        char csn[32];
        pc_cs_debug_name(p->space, csn);
        return cms_unsupported(err, "%s profile without BToA tags", csn);
    }
    if (s != PC_OK) {
        pc_cms_stages_fini(&st);
        return s;
    }
    *out = st;
    *pcs = result;
    return PC_OK;
}

bool pc_profile_can_be_destination(const pc_profile *p)
{
    static const pc_intent intents[3] = {PC_INTENT_PERCEPTUAL, PC_INTENT_RELATIVE, PC_INTENT_SATURATION};
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_cms_stages st = PC_CMS_STAGES_INIT;
        pc_pcs pcs;
        if (pc_profile_pcs_to_device(p, intents[i], &st, &pcs, NULL) != PC_OK) {
            return false;
        }
        pc_cms_stages_fini(&st);
    }
    return true;
}
