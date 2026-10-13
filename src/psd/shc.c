#include "psd/shc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "psd/internal.h"

void pc_psd_shc_contour_fini(pc_psd_shc_contour *c)
{
    if (c == NULL) {
        return;
    }
    pc_psd_str_fini(&c->name);
    free(c->points);
    memset(c, 0, sizeof *c);
}

void pc_psd_shc_free(pc_psd_shc_contour *c, size_t n)
{
    size_t i;
    for (i = 0; c != NULL && i < n; i++) {
        pc_psd_shc_contour_fini(&c[i]);
    }
    free(c);
}

bool pc_psd_shc_contour_eq(const pc_psd_shc_contour *a, const pc_psd_shc_contour *b)
{
    size_t i;
    if (a->name.len != b->name.len ||
        memcmp(pc_psd_str_c(&a->name), pc_psd_str_c(&b->name), a->name.len) != 0 ||
        a->npoints != b->npoints) {
        return false;
    }
    for (i = 0; i < a->npoints; i++) {
        const pc_psd_shc_point *p = &a->points[i];
        const pc_psd_shc_point *q = &b->points[i];
        if (!(p->input == q->input && p->output == q->output && p->continuous == q->continuous)) {
            return false;
        }
    }
    return true;
}

static pc_status read_name(psd_rd *r, pc_psd_str *out, pc_error *err)
{
    uint16_t units[PC_PSD_SHC_MAX_NAME_UNITS];
    size_t k = 0;
    uint32_t n;
    uint32_t i;
    pc_status st = psd_u32(r, &n, err);
    if (st == PC_OK) {
        st = psd_check_count(r, n, 2, err);
    }
    for (i = 0; st == PC_OK && i < n; i++) {
        uint16_t u;
        st = psd_u16(r, &u, err);
        if (st == PC_OK && k < PC_PSD_SHC_MAX_NAME_UNITS) {
            units[k++] = u;
        }
    }
    if (st != PC_OK) {
        return st;
    }
    /* A count that leaves out the terminator: the null follows (a point count is never 0). */
    if (!(k > 0 && units[k - 1] == 0) && psd_remaining(r) >= 2 && psd_rest(r)[0] == 0 &&
        psd_rest(r)[1] == 0) {
        r->pos += 2;
    }
    while (k > 0 && units[k - 1] == 0) {
        k--;
    }
    return psd_str_from_units(units, k, out, err);
}

pc_status pc_psd_shc_parse(const uint8_t *data, size_t len, pc_psd_shc_contour **out, size_t *nout,
                           pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pc_psd_shc_contour *list = NULL;
    size_t nlist = 0;
    uint8_t sig[4];
    uint16_t version;
    uint32_t count;
    uint32_t i;
    pc_status st;
    CK(psd_arr4(&r, sig, err));
    if (memcmp(sig, "8BFS", 4) != 0) {
        st = psd_bad_sig(err, "8BFS", sig);
        goto fail;
    }
    CK(psd_u16(&r, &version, err));
    if (version != 1) {
        st = pc_fail(err, PC_ERR_UNSUPPORTED, "unsupported file version %u", (unsigned)version);
        goto fail;
    }
    CK(psd_u32(&r, &count, err));
    /* A contour takes at least 4 (version) + 4 (name) + 2 (points) + 8 (range) bytes. */
    CK(psd_check_count(&r, count, 18, err));
    if (count > PC_PSD_SHC_MAX_CONTOURS) {
        st = psd_limit(err, "too many contours in an .shc file");
        goto fail;
    }
    if (count > 0) {
        list = pc_calloc_array(count, sizeof *list);
        if (list == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
    }
    for (i = 0; i < count; i++) {
        pc_psd_shc_contour *c = &list[i];
        uint32_t v;
        uint16_t n;
        uint16_t k;
        CK(psd_u32(&r, &v, err));
        if (v != 1 && v != 2) {
            st = psd_invalid(err, "unknown contour version %u", (unsigned)v);
            goto fail;
        }
        nlist++;
        CK(read_name(&r, &c->name, err));
        CK(psd_u16(&r, &n, err));
        CK(psd_check_count(&r, n, v == 2 ? 5 : 4, err));
        if (n > PC_PSD_SHC_MAX_POINTS) {
            st = psd_limit(err, "too many points in a contour");
            goto fail;
        }
        if (n > 0) {
            c->points = pc_calloc_array(n, sizeof *c->points);
            if (c->points == NULL) {
                st = psd_nomem(err);
                goto fail;
            }
        }
        for (k = 0; k < n; k++) {
            uint16_t o;
            uint16_t in;
            CK(psd_u16(&r, &o, err));
            CK(psd_u16(&r, &in, err));
            c->points[k].output = o > 255 ? 255.0f : (float)o;
            c->points[k].input = in > 255 ? 255.0f : (float)in;
            c->points[k].continuous = true;
            c->npoints++;
        }
        if (v == 2) {
            for (k = 0; k < n; k++) {
                uint8_t b;
                CK(psd_u8(&r, &b, err));
                c->points[k].continuous = b != 0;
            }
        }
        /* Minimum and maximum input range: not needed to draw the curve. */
        CK(psd_skip(&r, 8, err));
    }
    *out = list;
    *nout = count;
    return PC_OK;
fail:
    pc_psd_shc_free(list, nlist);
    return st;
}

static uint16_t level(float v)
{
    return isfinite(v) ? (uint16_t)pc_clampf(roundf(v), 0.0f, 255.0f) : 0;
}

pc_status pc_psd_shc_write(const pc_psd_shc_contour *c, size_t n, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    size_t i;
    if (n > PC_PSD_SHC_MAX_CONTOURS) {
        return psd_limit(err, "too many contours for an .shc file");
    }
    for (i = 0; i < n; i++) {
        if (c[i].npoints > PC_PSD_SHC_MAX_POINTS) {
            return psd_limit(err, "too many points in a contour");
        }
    }
    psd_w_init(&w, err);
    psd_put(&w, "8BFS", 4);
    psd_put_u16(&w, 1);
    psd_put_u32(&w, (uint32_t)n);
    for (i = 0; i < n && psd_w_ok(&w); i++) {
        uint16_t *u = NULL;
        size_t nu = 0;
        size_t k;
        pc_status st = psd_utf8n_to_units(pc_psd_str_c(&c[i].name), c[i].name.len, &u, &nu, err);
        if (st != PC_OK) {
            psd_w_fail(&w, st);
            break;
        }
        /* The reference cuts at the cap even inside a surrogate pair (unlike .aco/.ase). */
        if (nu > PC_PSD_SHC_MAX_NAME_UNITS) {
            nu = PC_PSD_SHC_MAX_NAME_UNITS;
        }
        psd_put_u32(&w, 2);
        psd_put_u32(&w, (uint32_t)nu + 1);
        for (k = 0; k < nu; k++) {
            psd_put_u16(&w, u[k]);
        }
        free(u);
        psd_put_u16(&w, 0);
        psd_put_u16(&w, (uint16_t)c[i].npoints);
        for (k = 0; k < c[i].npoints; k++) {
            psd_put_u16(&w, level(c[i].points[k].output));
            psd_put_u16(&w, level(c[i].points[k].input));
        }
        for (k = 0; k < c[i].npoints; k++) {
            psd_put_u8(&w, c[i].points[k].continuous ? 1 : 0);
        }
        psd_put_u32(&w, 0);
        psd_put_u32(&w, 255);
    }
    return psd_w_finish(&w, out);
}
