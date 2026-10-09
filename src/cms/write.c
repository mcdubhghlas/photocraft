/* ICC v4.3 profile writer (crates/cms/src/write.rs), byte-identical to the reference: same tag
 * order, same element layouts, identical payloads shared, same header fields. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "cms_internal.h"

static pc_status put_s15(pc_buf *o, double v, pc_error *err)
{
    return pc_buf_put_be32(o, (uint32_t)pc_f64_to_i32(round(v * 65536.0)), err);
}

static pc_status put_raw(pc_buf *o, const char *s, size_t n, pc_error *err)
{
    return pc_buf_append(o, s, n, err);
}

static pc_status xyz_type(pc_buf *o, const double v[3], pc_error *err)
{
    pc_status s = put_raw(o, "XYZ \0\0\0\0", 8, err);
    int i;
    for (i = 0; i < 3 && s == PC_OK; i++) {
        s = put_s15(o, v[i], err);
    }
    return s;
}

static pc_status mluc(pc_buf *o, const char *text, size_t len, pc_error *err)
{
    pc_buf units = PC_BUF_INIT;
    size_t i = 0;
    pc_status s = PC_OK;
    /* UTF-8 -> UTF-16BE (invalid bytes become U+FFFD). */
    while (s == PC_OK && i < len) {
        uint32_t cp;
        if (!pc_utf8_next((const uint8_t *)text, len, &i, &cp)) {
            cp = 0xFFFD;
            i++;
        }
        if (cp >= 0x10000) {
            uint32_t v = cp - 0x10000;
            s = pc_buf_put_be16(&units, (uint16_t)(0xD800 + (v >> 10)), err);
            if (s == PC_OK) {
                s = pc_buf_put_be16(&units, (uint16_t)(0xDC00 + (v & 0x3FF)), err);
            }
        } else {
            s = pc_buf_put_be16(&units, (uint16_t)cp, err);
        }
    }
    if (s == PC_OK) { s = put_raw(o, "mluc\0\0\0\0", 8, err); }
    if (s == PC_OK) { s = pc_buf_put_be32(o, 1, err); }
    if (s == PC_OK) { s = pc_buf_put_be32(o, 12, err); }
    if (s == PC_OK) { s = put_raw(o, "enUS", 4, err); }
    if (s == PC_OK) { s = pc_buf_put_be32(o, (uint32_t)units.len, err); }
    if (s == PC_OK) { s = pc_buf_put_be32(o, 28, err); }
    if (s == PC_OK && units.len > 0) { s = pc_buf_append(o, units.data, units.len, err); }
    pc_buf_fini(&units);
    return s;
}

static uint16_t u16q(float v)
{
    return pc_f32_to_u16(pc_clampf(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

static pc_status curve_elem(pc_buf *o, const pc_cms_curve *c, pc_error *err)
{
    pc_status s;
    size_t i;
    switch (c->kind) {
    case PC_CURVE_IDENTITY:
        s = put_raw(o, "curv\0\0\0\0", 8, err);
        return s == PC_OK ? pc_buf_put_be32(o, 0, err) : s;
    case PC_CURVE_GAMMA:
        s = put_raw(o, "para\0\0\0\0\0\0\0\0", 12, err);
        return s == PC_OK ? put_s15(o, c->gamma, err) : s;
    case PC_CURVE_PARAMETRIC:
        s = put_raw(o, "para\0\0\0\0", 8, err);
        if (s == PC_OK) { s = pc_buf_put_be16(o, c->ptype, err); }
        if (s == PC_OK) { s = put_raw(o, "\0\0", 2, err); }
        for (i = 0; i < pc_cms_curve_param_count(c->ptype) && s == PC_OK; i++) {
            s = put_s15(o, c->p[i], err);
        }
        return s;
    case PC_CURVE_TABLE:
        s = put_raw(o, "curv\0\0\0\0", 8, err);
        if (s == PC_OK) { s = pc_buf_put_be32(o, (uint32_t)c->n, err); }
        for (i = 0; i < c->n && s == PC_OK; i++) {
            s = pc_buf_put_be16(o, u16q(c->table[i]), err);
        }
        return s;
    }
    return PC_OK;
}

static pc_status pad4(pc_buf *o, pc_error *err)
{
    pc_status s = PC_OK;
    while (s == PC_OK && o->len % 4 != 0) {
        s = pc_buf_append_byte(o, 0, err);
    }
    return s;
}

static size_t table_len(const pc_cms_curve *cs, size_t n)
{
    size_t m = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        size_t l = cs[i].kind == PC_CURVE_TABLE ? cs[i].n : (cs[i].kind == PC_CURVE_IDENTITY ? 2 : 1024);
        if (i == 0 || l > m) {
            m = l;
        }
    }
    if (n == 0) {
        m = 2;
    }
    return m < 2 ? 2 : (m > 4096 ? 4096 : m);
}

/* Write the curve as n table values through put (1 or 2 bytes). */
static pc_status put_curve_table(pc_buf *o, const pc_cms_curve *c, size_t n, bool wide, pc_error *err)
{
    float *tmp = NULL;
    const float *t;
    size_t i;
    pc_status s = PC_OK;
    if (c->kind == PC_CURVE_TABLE && c->n == n) {
        t = c->table;
    } else {
        tmp = pc_malloc_array(n, sizeof *tmp);
        if (tmp == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        pc_cms_curve_sample(c, n, tmp);
        t = tmp;
    }
    for (i = 0; i < n && s == PC_OK; i++) {
        s = wide ? pc_buf_put_be16(o, u16q(t[i]), err)
                 : pc_buf_append_byte(o, pc_f32_to_u8(pc_clampf(t[i], 0.0f, 1.0f) * 255.0f + 0.5f), err);
    }
    free(tmp);
    return s;
}

/* mft1/mft2 for stages [matrix?] curves, clut, curves with a uniform grid; *ok false when the
 * stages don't have that shape. */
static pc_status encode_mft(const pc_cms_stages *st, bool wide, pc_buf *o, bool *ok, pc_error *err)
{
    static const double ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double *mat = ident;
    size_t i = 0;
    const pc_cms_stage *a;
    const pc_cms_stage *cl;
    const pc_cms_stage *b;
    const pc_cms_clut *clut;
    size_t g;
    size_t k;
    size_t n_in = 256;
    size_t n_out = 256;
    pc_status s = PC_OK;
    *ok = false;
    if (st->n > 0 && st->v[0].kind == PC_STAGE_MATRIX && st->v[0].rows == 3 && st->v[0].cols == 3) {
        mat = st->v[0].m;
        i = 1;
    }
    if (st->n - i != 3 || st->v[i].kind != PC_STAGE_CURVES || st->v[i + 1].kind != PC_STAGE_CLUT ||
        st->v[i + 2].kind != PC_STAGE_CURVES) {
        return PC_OK;
    }
    a = &st->v[i];
    cl = &st->v[i + 1];
    b = &st->v[i + 2];
    clut = &cl->clut;
    if (clut->inputs == 0) {
        return PC_OK;
    }
    g = clut->grid[0];
    for (k = 0; k < clut->inputs; k++) {
        if (clut->grid[k] != g) {
            return PC_OK;
        }
    }
    s = put_raw(o, wide ? "mft2\0\0\0\0" : "mft1\0\0\0\0", 8, err);
    if (s == PC_OK) {
        uint8_t dims[4];
        dims[0] = (uint8_t)clut->inputs;
        dims[1] = (uint8_t)clut->outputs;
        dims[2] = (uint8_t)g;
        dims[3] = 0;
        s = pc_buf_append(o, dims, 4, err);
    }
    for (k = 0; k < 9 && s == PC_OK; k++) {
        s = put_s15(o, mat[k], err);
    }
    if (wide) {
        n_in = table_len(a->curves, a->ncurves);
        n_out = table_len(b->curves, b->ncurves);
        if (s == PC_OK) { s = pc_buf_put_be16(o, (uint16_t)n_in, err); }
        if (s == PC_OK) { s = pc_buf_put_be16(o, (uint16_t)n_out, err); }
    }
    for (k = 0; k < a->ncurves && s == PC_OK; k++) {
        s = put_curve_table(o, &a->curves[k], n_in, wide, err);
    }
    for (k = 0; k < clut->len && s == PC_OK; k++) {
        s = wide ? pc_buf_put_be16(o, u16q(clut->data[k]), err)
                 : pc_buf_append_byte(o, pc_f32_to_u8(pc_clampf(clut->data[k], 0.0f, 1.0f) * 255.0f + 0.5f), err);
    }
    for (k = 0; k < b->ncurves && s == PC_OK; k++) {
        s = put_curve_table(o, &b->curves[k], n_out, wide, err);
    }
    *ok = s == PC_OK;
    return s;
}

static pc_status put_curves(pc_buf *o, const pc_cms_stage *cs, pc_error *err)
{
    size_t k;
    pc_status s = PC_OK;
    for (k = 0; k < cs->ncurves && s == PC_OK; k++) {
        s = curve_elem(o, &cs->curves[k], err);
        if (s == PC_OK) {
            s = pad4(o, err);
        }
    }
    return s;
}

/* mAB / mBA; *ok false when the stages aren't a layout this direction can store. */
static pc_status encode_ab(const pc_cms_stages *st, bool a2b, size_t inputs, size_t outputs, pc_buf *o, bool *ok,
                           pc_error *err)
{
    const pc_cms_stage *a_curves = NULL;
    const pc_cms_stage *clut = NULL;
    const pc_cms_stage *m_curves = NULL;
    const pc_cms_stage *matrix = NULL;
    const pc_cms_stage *b_curves = NULL;
    const pc_cms_stage *v = st->v;
    size_t n = st->n;
    uint32_t offs[5] = {0, 0, 0, 0, 0};
    size_t start = o->len;
    pc_status s;
    size_t k;
    *ok = false;
    if (a2b) {
        /* [A, CLUT] [M, matrix] B */
        size_t r = 0;
        if (n >= 2 && v[0].kind == PC_STAGE_CURVES && v[1].kind == PC_STAGE_CLUT) {
            a_curves = &v[0];
            clut = &v[1];
            r = 2;
        }
        if (n - r == 3 && v[r].kind == PC_STAGE_CURVES && v[r + 1].kind == PC_STAGE_MATRIX &&
            v[r + 2].kind == PC_STAGE_CURVES) {
            m_curves = &v[r];
            matrix = &v[r + 1];
            b_curves = &v[r + 2];
        } else if (n - r == 1 && v[r].kind == PC_STAGE_CURVES) {
            b_curves = &v[r];
        } else {
            return PC_OK;
        }
    } else {
        /* B [matrix, M] [CLUT, A] */
        size_t i = 0;
        if (n > 0 && v[0].kind == PC_STAGE_CURVES) {
            b_curves = &v[0];
            i = 1;
        }
        if (i + 1 < n && v[i].kind == PC_STAGE_MATRIX && v[i + 1].kind == PC_STAGE_CURVES) {
            matrix = &v[i];
            m_curves = &v[i + 1];
            i += 2;
        }
        if (i + 1 < n && v[i].kind == PC_STAGE_CLUT && v[i + 1].kind == PC_STAGE_CURVES) {
            clut = &v[i];
            a_curves = &v[i + 1];
            i += 2;
        }
        if (i != n) {
            return PC_OK;
        }
    }
    if (b_curves == NULL) {
        return PC_OK;
    }
    s = put_raw(o, a2b ? "mAB \0\0\0\0" : "mBA \0\0\0\0", 8, err);
    if (s == PC_OK) {
        uint8_t dims[4];
        dims[0] = (uint8_t)inputs;
        dims[1] = (uint8_t)outputs;
        dims[2] = 0;
        dims[3] = 0;
        s = pc_buf_append(o, dims, 4, err);
    }
    if (s == PC_OK) {
        static const uint8_t zeros[20] = {0};
        s = pc_buf_append(o, zeros, sizeof zeros, err); /* offsets, patched below */
    }
    if (s == PC_OK) {
        offs[0] = (uint32_t)(o->len - start);
        s = put_curves(o, b_curves, err);
    }
    if (s == PC_OK && matrix != NULL) {
        offs[1] = (uint32_t)(o->len - start);
        for (k = 0; k < matrix->rows * matrix->cols && s == PC_OK; k++) {
            s = put_s15(o, matrix->m[k], err);
        }
        for (k = 0; k < matrix->rows && s == PC_OK; k++) {
            s = put_s15(o, matrix->offset[k], err);
        }
        offs[2] = (uint32_t)(o->len - start);
        if (s == PC_OK) {
            if (m_curves == NULL) {
                pc_buf_truncate(o, start);
                return PC_OK;
            }
            s = put_curves(o, m_curves, err);
        }
    }
    if (s == PC_OK && clut != NULL) {
        uint8_t g[16];
        offs[3] = (uint32_t)(o->len - start);
        memset(g, 0, sizeof g);
        for (k = 0; k < clut->clut.inputs; k++) {
            g[k] = (uint8_t)clut->clut.grid[k];
        }
        s = pc_buf_append(o, g, sizeof g, err);
        if (s == PC_OK) {
            s = put_raw(o, "\2\0\0\0", 4, err);
        }
        for (k = 0; k < clut->clut.len && s == PC_OK; k++) {
            s = pc_buf_put_be16(o, u16q(clut->clut.data[k]), err);
        }
        if (s == PC_OK) {
            s = pad4(o, err);
        }
        offs[4] = (uint32_t)(o->len - start);
        if (s == PC_OK) {
            if (a_curves == NULL) {
                pc_buf_truncate(o, start);
                return PC_OK;
            }
            s = put_curves(o, a_curves, err);
        }
    }
    if (s != PC_OK) {
        pc_buf_truncate(o, start);
        return s;
    }
    for (k = 0; k < 5; k++) {
        pc_store_be32(o->data + start + 12 + k * 4, offs[k]);
    }
    *ok = true;
    return PC_OK;
}

typedef struct tagbuf {
    char sig[4];
    pc_buf data;
} tagbuf;

typedef struct tags {
    tagbuf v[24];
    size_t n;
} tags;

static pc_buf *new_tag(tags *t, const char *sig)
{
    tagbuf *tb = &t->v[t->n++];
    memcpy(tb->sig, sig, 4);
    pc_buf_init(&tb->data);
    return &tb->data;
}

pc_status pc_icc_encode(const pc_profile *p, pc_buf *out, pc_error *err)
{
    tags t;
    pc_buf data = PC_BUF_INIT;
    size_t table_len;
    size_t i;
    size_t j;
    size_t start = out->len;
    uint32_t offsets[24];
    uint32_t lens[24];
    pc_status s;
    int k;

    t.n = 0;
    s = mluc(new_tag(&t, "desc"), p->description, p->description_len, err);
    if (s == PC_OK) { s = mluc(new_tag(&t, "cprt"), p->copyright, p->copyright_len, err); }
    if (s == PC_OK) { s = xyz_type(new_tag(&t, "wtpt"), p->white_point, err); }
    if (s == PC_OK && p->has_black_point) { s = xyz_type(new_tag(&t, "bkpt"), p->black_point, err); }
    if (s == PC_OK && p->has_chad) {
        pc_buf *b = new_tag(&t, "chad");
        int a;
        int c;
        s = put_raw(b, "sf32\0\0\0\0", 8, err);
        for (a = 0; a < 3 && s == PC_OK; a++) {
            for (c = 0; c < 3 && s == PC_OK; c++) {
                s = put_s15(b, p->chad.m[a][c], err);
            }
        }
    }
    if (s == PC_OK && p->has_matrix) {
        static const char *const names[3] = {"rXYZ", "gXYZ", "bXYZ"};
        for (k = 0; k < 3 && s == PC_OK; k++) {
            double col[3];
            col[0] = p->matrix.m[0][k];
            col[1] = p->matrix.m[1][k];
            col[2] = p->matrix.m[2][k];
            s = xyz_type(new_tag(&t, names[k]), col, err);
        }
    }
    if (s == PC_OK && p->has_trc) {
        static const char *const names[3] = {"rTRC", "gTRC", "bTRC"};
        for (k = 0; k < 3 && s == PC_OK; k++) {
            s = curve_elem(new_tag(&t, names[k]), &p->trc[k], err);
        }
    }
    if (s == PC_OK && p->has_gray_trc) {
        s = curve_elem(new_tag(&t, "kTRC"), &p->gray_trc, err);
    }
    {
        static const char *const an[3] = {"A2B0", "A2B1", "A2B2"};
        static const char *const bn[3] = {"B2A0", "B2A1", "B2A2"};
        int dir;
        for (dir = 0; dir < 2 && s == PC_OK; dir++) {
            for (k = 0; k < 3 && s == PC_OK; k++) {
                const pc_lut *l = dir == 0 ? p->a2b[k] : p->b2a[k];
                pc_buf *b;
                bool ok = false;
                if (l == NULL) {
                    continue;
                }
                b = new_tag(&t, dir == 0 ? an[k] : bn[k]);
                switch (l->kind) {
                case PC_LUT16: s = encode_mft(&l->stages, true, b, &ok, err); break;
                case PC_LUT8: s = encode_mft(&l->stages, false, b, &ok, err); break;
                case PC_LUT_AB: s = encode_ab(&l->stages, dir == 0, l->inputs, l->outputs, b, &ok, err); break;
                }
                if (!ok) {
                    /* A layout this tag type can't store: leave the tag out. */
                    pc_buf_fini(b);
                    t.n--;
                }
            }
        }
    }
    if (s != PC_OK) {
        goto cleanup;
    }

    /* Layout: header, tag table, then data (identical payloads share one copy). */
    table_len = 4 + t.n * 12;
    for (i = 0; i < t.n && s == PC_OK; i++) {
        bool shared = false;
        for (j = 0; j < i; j++) {
            if (t.v[j].data.len == t.v[i].data.len &&
                (t.v[i].data.len == 0 || memcmp(t.v[j].data.data, t.v[i].data.data, t.v[i].data.len) == 0)) {
                offsets[i] = offsets[j];
                lens[i] = lens[j];
                shared = true;
                break;
            }
        }
        if (shared) {
            continue;
        }
        offsets[i] = (uint32_t)(128 + table_len + data.len);
        lens[i] = (uint32_t)t.v[i].data.len;
        if (t.v[i].data.len > 0) {
            s = pc_buf_append(&data, t.v[i].data.data, t.v[i].data.len, err);
        }
        if (s == PC_OK) {
            s = pad4(&data, err);
        }
    }
    if (s == PC_OK) {
        uint8_t hdr[128];
        uint32_t size = (uint32_t)(128 + table_len + data.len);
        static const uint16_t date[6] = {2026, 1, 1, 0, 0, 0};
        uint32_t pcs = p->pcs == PC_PCS_XYZ ? PC_SIG('X', 'Y', 'Z', ' ') : PC_SIG('L', 'a', 'b', ' ');
        memset(hdr, 0, sizeof hdr);
        pc_store_be32(hdr, size);
        hdr[8] = 4;
        hdr[9] = 0x30;
        pc_store_be32(hdr + 12, p->class_sig);
        pc_store_be32(hdr + 16, p->space);
        pc_store_be32(hdr + 20, pcs);
        for (k = 0; k < 6; k++) {
            pc_store_be16(hdr + 24 + 2 * k, date[k]);
        }
        memcpy(hdr + 36, "acsp", 4);
        pc_store_be32(hdr + 64, (uint32_t)p->rendering_intent);
        for (k = 0; k < 3; k++) {
            pc_store_be32(hdr + 68 + 4 * k, (uint32_t)pc_f64_to_i32(round(PC_D50[k] * 65536.0)));
        }
        s = pc_buf_append(out, hdr, sizeof hdr, err);
        if (s == PC_OK) {
            s = pc_buf_put_be32(out, (uint32_t)t.n, err);
        }
        for (i = 0; i < t.n && s == PC_OK; i++) {
            s = pc_buf_append(out, t.v[i].sig, 4, err);
            if (s == PC_OK) { s = pc_buf_put_be32(out, offsets[i], err); }
            if (s == PC_OK) { s = pc_buf_put_be32(out, lens[i], err); }
        }
        if (s == PC_OK && data.len > 0) {
            s = pc_buf_append(out, data.data, data.len, err);
        }
    }
cleanup:
    for (i = 0; i < t.n; i++) {
        pc_buf_fini(&t.v[i].data);
    }
    pc_buf_fini(&data);
    if (s != PC_OK) {
        pc_buf_truncate(out, start);
    }
    return s;
}
