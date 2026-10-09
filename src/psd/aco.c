#include "psd/aco.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "psd/internal.h"

pc_psd_aco_color pc_psd_aco_from_record(uint16_t space, const uint16_t v[4])
{
    pc_psd_aco_color c;
    memset(&c, 0, sizeof c);
    switch (space) {
    case PC_PSD_ACO_SPACE_RGB:
    case PC_PSD_ACO_SPACE_HSB:
    case PC_PSD_ACO_SPACE_LAB:
        c.kind = space == PC_PSD_ACO_SPACE_RGB ? PC_PSD_ACO_RGB
                 : space == PC_PSD_ACO_SPACE_HSB ? PC_PSD_ACO_HSB
                                                 : PC_PSD_ACO_LAB;
        memcpy(c.v, v, 3 * sizeof *v);
        break;
    case PC_PSD_ACO_SPACE_CMYK: {
        int i;
        c.kind = PC_PSD_ACO_CMYK;
        for (i = 0; i < 4; i++) {
            c.v[i] = (uint16_t)(0xFFFFu - v[i]);
        }
        break;
    }
    case PC_PSD_ACO_SPACE_GRAY:
        c.kind = PC_PSD_ACO_GRAY;
        c.v[0] = v[0];
        break;
    default:
        c.kind = PC_PSD_ACO_OTHER;
        c.space = space;
        memcpy(c.v, v, 4 * sizeof *v);
    }
    return c;
}

void pc_psd_aco_to_record(pc_psd_aco_color c, uint16_t *space, uint16_t v[4])
{
    int i;
    memset(v, 0, 4 * sizeof *v);
    switch (c.kind) {
    case PC_PSD_ACO_RGB:
    case PC_PSD_ACO_HSB:
    case PC_PSD_ACO_LAB:
        *space = c.kind == PC_PSD_ACO_RGB ? PC_PSD_ACO_SPACE_RGB
                 : c.kind == PC_PSD_ACO_HSB ? PC_PSD_ACO_SPACE_HSB
                                            : PC_PSD_ACO_SPACE_LAB;
        memcpy(v, c.v, 3 * sizeof *v);
        break;
    case PC_PSD_ACO_CMYK:
        *space = PC_PSD_ACO_SPACE_CMYK;
        for (i = 0; i < 4; i++) {
            v[i] = (uint16_t)(0xFFFFu - c.v[i]);
        }
        break;
    case PC_PSD_ACO_GRAY:
        *space = PC_PSD_ACO_SPACE_GRAY;
        v[0] = c.v[0];
        break;
    default:
        *space = c.space;
        memcpy(v, c.v, 4 * sizeof *v);
    }
}

bool pc_psd_aco_color_eq(pc_psd_aco_color a, pc_psd_aco_color b)
{
    size_t n = a.kind == PC_PSD_ACO_GRAY ? 1 : a.kind == PC_PSD_ACO_CMYK || a.kind == PC_PSD_ACO_OTHER ? 4 : 3;
    return a.kind == b.kind && (a.kind != PC_PSD_ACO_OTHER || a.space == b.space) &&
           memcmp(a.v, b.v, n * sizeof a.v[0]) == 0;
}

bool pc_psd_aco_swatch_eq(const pc_psd_aco_swatch *a, const pc_psd_aco_swatch *b)
{
    return pc_psd_str_eq(&a->name, &b->name) && pc_psd_aco_color_eq(a->color, b->color);
}

void pc_psd_aco_fini(pc_psd_aco *f)
{
    size_t i;
    if (f == NULL) {
        return;
    }
    for (i = 0; i < f->n; i++) {
        pc_psd_str_fini(&f->v[i].name);
    }
    free(f->v);
    f->v = NULL;
    f->n = 0;
    f->has_names = false;
}

static pc_status read_name(psd_rd *r, pc_psd_str *out, pc_error *err)
{
    uint32_t n;
    uint16_t units[PC_PSD_ACO_MAX_NAME_UNITS];
    size_t k = 0;
    uint32_t i;
    pc_status st = psd_u32(r, &n, err);
    if (st == PC_OK) {
        st = psd_check_count(r, n, 2, err);
    }
    for (i = 0; st == PC_OK && i < n; i++) {
        uint16_t u;
        st = psd_u16(r, &u, err);
        if (st == PC_OK && k < PC_PSD_ACO_MAX_NAME_UNITS) {
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

static void free_swatches(pc_psd_aco_swatch *v, size_t n)
{
    pc_psd_aco f = {v, n, false};
    pc_psd_aco_fini(&f);
}

static pc_status read_section(psd_rd *r, bool named, pc_psd_aco_swatch **out, size_t *nout, pc_error *err)
{
    uint16_t count;
    pc_psd_aco_swatch *v = NULL;
    size_t n = 0;
    pc_status st = psd_u16(r, &count, err);
    if (st == PC_OK) {
        /* Each record is at least 10 bytes (plus a 4-byte name length in version 2). */
        st = psd_check_count(r, count, named ? 14 : 10, err);
    }
    if (st == PC_OK && count > 0) {
        v = pc_calloc_array(count, sizeof *v);
        if (v == NULL) {
            st = psd_nomem(err);
        }
    }
    while (st == PC_OK && n < count) {
        uint16_t space;
        uint16_t vals[4];
        int k;
        st = psd_u16(r, &space, err);
        for (k = 0; st == PC_OK && k < 4; k++) {
            st = psd_u16(r, &vals[k], err);
        }
        if (st != PC_OK) {
            break;
        }
        v[n].color = pc_psd_aco_from_record(space, vals);
        if (named) {
            st = read_name(r, &v[n].name, err);
        }
        n++;
    }
    if (st != PC_OK) {
        free_swatches(v, n);
        return st;
    }
    *out = v;
    *nout = n;
    return PC_OK;
}

pc_status pc_psd_aco_parse(const uint8_t *data, size_t len, pc_psd_aco *out, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    uint16_t version;
    pc_psd_aco f = {NULL, 0, false};
    pc_status st = psd_u16(&r, &version, err);
    if (st != PC_OK) {
        return st;
    }
    if (version == 1) {
        st = read_section(&r, false, &f.v, &f.n, err);
        if (st != PC_OK) {
            return st;
        }
        /* A version 2 section follows in files written by Photoshop 6 and later; anything else
         * after version 1 (padding some writers add) is ignored. */
        if (psd_remaining(&r) >= 4 && psd_rest(&r)[0] == 0 && psd_rest(&r)[1] == 2) {
            pc_psd_aco v2 = {NULL, 0, true};
            r.pos += 2;
            st = read_section(&r, true, &v2.v, &v2.n, err);
            pc_psd_aco_fini(&f);
            if (st != PC_OK) {
                return st;
            }
            f = v2;
        }
    } else if (version == 2) {
        f.has_names = true;
        st = read_section(&r, true, &f.v, &f.n, err);
        if (st != PC_OK) {
            return st;
        }
    } else {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "unsupported file version %u", (unsigned)version);
    }
    *out = f;
    return PC_OK;
}

pc_status pc_psd_aco_write(const pc_psd_aco_swatch *s, size_t n, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    uint16_t version;
    size_t i;
    if (n > PC_PSD_ACO_MAX_SWATCHES) {
        return psd_limit(err, "an .aco file holds at most 65535 swatches");
    }
    psd_w_init(&w, err);
    for (version = 1; version <= 2; version++) {
        psd_put_u16(&w, version);
        psd_put_u16(&w, (uint16_t)n);
        for (i = 0; i < n && psd_w_ok(&w); i++) {
            uint16_t space;
            uint16_t v[4];
            int k;
            pc_psd_aco_to_record(s[i].color, &space, v);
            psd_put_u16(&w, space);
            for (k = 0; k < 4; k++) {
                psd_put_u16(&w, v[k]);
            }
            if (version == 2) {
                uint16_t *u = NULL;
                size_t nu = 0;
                pc_status st = psd_utf8n_to_units(pc_psd_str_c(&s[i].name), s[i].name.len, &u, &nu, err);
                if (st != PC_OK) {
                    psd_w_fail(&w, st);
                    break;
                }
                if (nu > PC_PSD_ACO_MAX_NAME_UNITS) {
                    nu = PC_PSD_ACO_MAX_NAME_UNITS;
                }
                /* The length counts the terminating NUL. */
                psd_put_u32(&w, (uint32_t)nu + 1);
                for (k = 0; (size_t)k < nu; k++) {
                    psd_put_u16(&w, u[k]);
                }
                psd_put_u16(&w, 0);
                free(u);
            }
        }
    }
    return psd_w_finish(&w, out);
}
