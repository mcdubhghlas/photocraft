#include "psd/abr.h"

#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "psd/compression.h"
#include "psd/internal.h"

/* ---- model ---- */

void pc_psd_abr_sample_fini(pc_psd_abr_sample *s)
{
    if (s != NULL) {
        pc_psd_str_fini(&s->id);
        pc_psd_bytes_fini(&s->data);
    }
}

bool pc_psd_abr_sample_eq(const pc_psd_abr_sample *a, const pc_psd_abr_sample *b)
{
    return pc_psd_str_eq(&a->id, &b->id) && a->width == b->width && a->height == b->height && a->depth == b->depth &&
           pc_psd_bytes_eq(&a->data, &b->data);
}

pc_status pc_psd_abr_sample_to_unit(const pc_psd_abr_sample *s, float **out, size_t *n, pc_error *err)
{
    size_t count = s->depth == 16 ? s->data.len / 2 : s->data.len;
    float *v = NULL;
    size_t i;
    if (count > 0 && (v = pc_malloc_array(count, sizeof *v)) == NULL) {
        return psd_nomem(err);
    }
    for (i = 0; i < count; i++) {
        v[i] = s->depth == 16 ? (float)((s->data.data[2 * i] << 8) | s->data.data[2 * i + 1]) / 65535.0f
                              : (float)s->data.data[i] / 255.0f;
    }
    *out = v;
    *n = count;
    return PC_OK;
}

void pc_psd_abr_legacy_fini(pc_psd_abr_legacy *b)
{
    if (b != NULL) {
        pc_psd_str_fini(&b->name);
        pc_psd_abr_sample_fini(&b->sample);
    }
}

bool pc_psd_abr_legacy_eq(const pc_psd_abr_legacy *a, const pc_psd_abr_legacy *b)
{
    if (!pc_psd_str_eq(&a->name, &b->name) || a->spacing != b->spacing || a->anti_alias != b->anti_alias ||
        a->tip != b->tip) {
        return false;
    }
    if (a->tip == PC_PSD_ABR_COMPUTED) {
        return a->diameter == b->diameter && a->hardness == b->hardness && a->angle == b->angle &&
               a->roundness == b->roundness;
    }
    return pc_psd_abr_sample_eq(&a->sample, &b->sample);
}

void pc_psd_abr_fini(pc_psd_abr *f)
{
    size_t i;
    if (f == NULL) {
        return;
    }
    for (i = 0; i < f->nlegacy; i++) {
        pc_psd_abr_legacy_fini(&f->legacy[i]);
    }
    free(f->legacy);
    for (i = 0; i < f->nsamples; i++) {
        pc_psd_abr_sample_fini(&f->samples[i]);
    }
    free(f->samples);
    pc_psd_patterns_free(f->patterns, f->npatterns);
    for (i = 0; i < f->npresets; i++) {
        pc_psd_desc_fini(&f->presets[i]);
    }
    free(f->presets);
    pc_psd_abr_folders_free(f->folders, f->nfolders);
    pc_psd_warnings_fini(&f->warnings);
    memset(f, 0, sizeof *f);
}

/* ---- reading ---- */

typedef struct parse_state {
    pc_psd_abr *out;
    size_t cap_legacy;
    size_t cap_samples;
    size_t cap_presets;
    size_t budget; /* decoded sample bytes left */
} parse_state;

static pc_status lossy(const uint8_t *b, size_t n, pc_psd_str *out, pc_error *err)
{
    pc_buf t = PC_BUF_INIT;
    pc_status st = pc_utf8_lossy(b, n, &t, err);
    if (st == PC_OK) {
        st = pc_psd_str_set(out, (const char *)t.data, t.len, err);
    }
    pc_buf_fini(&t);
    return st;
}

/* The hard error that stops the whole parse (the shared sample budget ran out). */
static bool is_budget_error(pc_status st, const pc_error *e)
{
    return st == PC_ERR_LIMIT && strstr(e->msg, "MAX_TOTAL_BYTES") != NULL;
}

static pc_status rethrow(pc_status st, const pc_error *e, pc_error *err)
{
    if (err != NULL) {
        *err = *e;
    }
    return st;
}

static pc_status read_i32x4(psd_rd *r, int32_t v[4], pc_error *err)
{
    int i;
    pc_status st = PC_OK;
    for (i = 0; i < 4 && st == PC_OK; i++) {
        st = psd_i32(r, &v[i], err);
    }
    return st;
}

/* Bounds (top, left, bottom, right), depth and compression, then the pixel data. Takes *id. */
static pc_status read_sample_body(psd_rd *r, pc_psd_str *id, size_t *budget, pc_psd_abr_sample *out, pc_error *err)
{
    int32_t b[4];
    int64_t w;
    int64_t h;
    uint16_t depth;
    uint8_t comp;
    pc_psd_layout l;
    size_t need;
    const uint8_t *rest;
    size_t nrest;
    pc_psd_abr_sample s;
    pc_status st;
    memset(&s, 0, sizeof s);
    s.id = *id;
    memset(id, 0, sizeof *id);
    CK(read_i32x4(r, b, err));
    w = (int64_t)b[3] - b[1];
    h = (int64_t)b[2] - b[0];
    if (w <= 0 || h <= 0) {
        st = psd_invalid(err, "brush sample bounds %lld\xc3\x97%lld", (long long)w, (long long)h);
        goto fail;
    }
    if (w > PC_PSD_ABR_MAX_EDGE || h > PC_PSD_ABR_MAX_EDGE) {
        st = psd_limit(err, "brush sample larger than MAX_EDGE");
        goto fail;
    }
    s.width = (uint32_t)w;
    s.height = (uint32_t)h;
    CK(psd_u16(r, &depth, err));
    if (depth != 8 && depth != 16) {
        st = psd_invalid(err, "brush sample depth %u", (unsigned)depth);
        goto fail;
    }
    s.depth = depth;
    CK(psd_u8(r, &comp, err));
    if (comp > 1) {
        st = psd_unsupported(err, "brush sample compression %u", (unsigned)comp);
        goto fail;
    }
    l.planes = 1;
    l.width = s.width;
    l.height = s.height;
    l.depth = depth;
    l.version = PC_PSD_V1;
    CK(pc_psd_layout_decoded_len(&l, &need, err));
    if (need > *budget) {
        st = psd_limit(err, "abr samples exceed MAX_TOTAL_BYTES");
        goto fail;
    }
    *budget -= need;
    nrest = psd_remaining(r);
    CK(psd_bytes(r, nrest, &rest, err));
    CK(pc_psd_decode_planes(comp, rest, nrest, &l, &s.data, err));
    *out = s;
    return PC_OK;
fail:
    pc_psd_abr_sample_fini(&s);
    return st;
}

static pc_status read_ucs2(psd_rd *r, pc_psd_str *out, pc_error *err)
{
    uint32_t n;
    const uint8_t *raw;
    uint16_t *u = NULL;
    size_t end;
    size_t i;
    pc_status st = psd_u32(r, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n > 4096) {
        return psd_limit(err, "abr name length");
    }
    if ((st = psd_bytes(r, (size_t)n * 2, &raw, err)) != PC_OK) {
        return st;
    }
    if (n > 0 && (u = pc_malloc_array(n, sizeof *u)) == NULL) {
        return psd_nomem(err);
    }
    for (i = 0; i < n; i++) {
        u[i] = (uint16_t)((raw[2 * i] << 8) | raw[2 * i + 1]);
    }
    end = n;
    while (end > 0 && u[end - 1] == 0) {
        end--;
    }
    st = psd_str_from_units(u, end, out, err);
    free(u);
    return st;
}

static pc_status read_legacy(psd_rd *b, uint16_t ty, uint16_t version, size_t *budget, pc_psd_abr_legacy *out,
                             pc_error *err)
{
    pc_psd_abr_legacy lb;
    uint32_t misc;
    uint16_t angle;
    uint8_t aa;
    pc_psd_str id = {NULL, 0};
    pc_status st;
    memset(&lb, 0, sizeof lb);
    CK(psd_u32(b, &misc, err));
    CK(psd_u16(b, &lb.spacing, err));
    if (ty == 1) {
        lb.tip = PC_PSD_ABR_COMPUTED;
        lb.anti_alias = true;
        CK(psd_u16(b, &lb.diameter, err));
        CK(psd_u16(b, &lb.roundness, err));
        CK(psd_u16(b, &angle, err));
        lb.angle = (int16_t)angle;
        CK(psd_u16(b, &lb.hardness, err));
    } else {
        lb.tip = PC_PSD_ABR_SAMPLED;
        if (version == 2) {
            CK(read_ucs2(b, &lb.name, err));
        }
        CK(psd_u8(b, &aa, err));
        lb.anti_alias = aa != 0;
        CK(psd_skip(b, 8, err)); /* short bounds (superseded by the 32-bit bounds) */
        CK(read_sample_body(b, &id, budget, &lb.sample, err));
    }
    *out = lb;
    return PC_OK;
fail:
    pc_psd_abr_legacy_fini(&lb);
    return st;
}

static pc_status parse_v12(psd_rd *r, uint16_t version, parse_state *ps, pc_error *err)
{
    pc_psd_abr *out = ps->out;
    uint16_t count;
    uint16_t i;
    pc_status st = psd_u16(r, &count, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < count; i++) {
        uint16_t ty;
        uint32_t len;
        psd_rd body;
        pc_psd_abr_legacy lb;
        pc_error e;
        if (psd_empty(r)) {
            return psd_warn(&out->warnings, err, "file ends after %u of %u brushes", (unsigned)i, (unsigned)count);
        }
        if ((st = psd_u16(r, &ty, err)) != PC_OK || (st = psd_u32(r, &len, err)) != PC_OK) {
            return st;
        }
        if (psd_sub(r, len, &body, NULL) != PC_OK) {
            return psd_warn(&out->warnings, err, "brush %u: truncated", (unsigned)i + 1);
        }
        if (ty != 1 && ty != 2) {
            if ((st = psd_warn(&out->warnings, err, "brush %u: unknown brush type %u skipped", (unsigned)i + 1,
                               (unsigned)ty)) != PC_OK) {
                return st;
            }
            continue;
        }
        st = read_legacy(&body, ty, version, &ps->budget, &lb, &e);
        if (st == PC_OK) {
            pc_psd_abr_legacy *g = pc_grow_array(out->legacy, &ps->cap_legacy, out->nlegacy + 1, sizeof *g);
            if (g == NULL) {
                pc_psd_abr_legacy_fini(&lb);
                return psd_nomem(err);
            }
            out->legacy = g;
            out->legacy[out->nlegacy++] = lb;
        } else if (is_budget_error(st, &e) || st == PC_ERR_NOMEM) {
            return rethrow(st, &e, err);
        } else if ((st = psd_warn(&out->warnings, err, "brush %u: %s; skipped", (unsigned)i + 1, e.msg)) != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

static pc_status read_samp_entry(psd_rd *b, uint16_t subversion, size_t *budget, pc_psd_abr_sample *out,
                                 pc_error *err)
{
    uint8_t n;
    const uint8_t *idb;
    pc_psd_str id = {NULL, 0};
    pc_status st;
    if ((st = psd_u8(b, &n, err)) != PC_OK || (st = psd_bytes(b, n, &idb, err)) != PC_OK ||
        (st = lossy(idb, n, &id, err)) != PC_OK) {
        return st;
    }
    st = psd_skip(b, subversion == 1 ? 10 : 264, err);
    if (st == PC_OK) {
        st = read_sample_body(b, &id, budget, out, err);
    }
    pc_psd_str_fini(&id);
    return st;
}

static pc_status read_samp(const uint8_t *data, size_t len, uint16_t subversion, parse_state *ps, pc_error *err)
{
    pc_psd_abr *out = ps->out;
    psd_rd r = psd_rd_make(data, len);
    pc_status st;
    while (psd_remaining(&r) >= 4) {
        uint32_t elen;
        uint64_t padded;
        size_t take;
        const uint8_t *body;
        psd_rd b;
        pc_psd_abr_sample s;
        pc_error e;
        if (out->nsamples >= PC_PSD_ABR_MAX_BRUSHES) {
            return psd_warn(&out->warnings, err, "too many sampled tips; the rest were skipped");
        }
        if ((st = psd_u32(&r, &elen, err)) != PC_OK) {
            return st;
        }
        if (elen == 0) {
            break;
        }
        padded = ((uint64_t)elen + 3) & ~(uint64_t)3;
        take = padded < psd_remaining(&r) ? (size_t)padded : psd_remaining(&r);
        if ((st = psd_bytes(&r, take, &body, err)) != PC_OK) {
            return st;
        }
        if (take < elen) {
            return psd_warn(&out->warnings, err, "sampled tip %zu: truncated", out->nsamples + 1);
        }
        b = psd_rd_make(body, take);
        st = read_samp_entry(&b, subversion, &ps->budget, &s, &e);
        if (st == PC_OK) {
            pc_psd_abr_sample *g = pc_grow_array(out->samples, &ps->cap_samples, out->nsamples + 1, sizeof *g);
            if (g == NULL) {
                pc_psd_abr_sample_fini(&s);
                return psd_nomem(err);
            }
            out->samples = g;
            out->samples[out->nsamples++] = s;
        } else if (is_budget_error(st, &e) || st == PC_ERR_NOMEM) {
            return rethrow(st, &e, err);
        } else if ((st = psd_warn(&out->warnings, err, "sampled tip %zu: %s; skipped", out->nsamples + 1, e.msg)) !=
                   PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

static pc_status read_desc(const uint8_t *data, size_t len, parse_state *ps, pc_error *err)
{
    pc_psd_abr *out = ps->out;
    pc_psd_desc vd;
    size_t used;
    pc_error e;
    const pc_psd_value *v;
    size_t i;
    pc_status st = pc_psd_vdesc_parse_prefix(data, len, &vd, &used, &e);
    if (st == PC_ERR_NOMEM) {
        return rethrow(st, &e, err);
    }
    if (st != PC_OK) {
        return psd_warn(&out->warnings, err, "brush settings (desc) unreadable: %s", e.msg);
    }
    v = pc_psd_desc_get(&vd, "Brsh");
    if (v == NULL || v->kind != PC_PSD_V_LIST) {
        st = psd_warn(&out->warnings, err, "brush settings (desc) hold no brush list");
    } else {
        for (i = 0; i < v->nlist && i < PC_PSD_ABR_MAX_BRUSHES && st == PC_OK; i++) {
            pc_psd_desc *g;
            if (v->list[i].kind != PC_PSD_V_DESCRIPTOR) {
                continue;
            }
            g = pc_grow_array(out->presets, &ps->cap_presets, out->npresets + 1, sizeof *g);
            if (g == NULL) {
                st = psd_nomem(err);
                break;
            }
            out->presets = g;
            st = pc_psd_desc_clone(v->list[i].desc, &out->presets[out->npresets], err);
            if (st == PC_OK) {
                out->npresets++;
            }
        }
    }
    pc_psd_desc_fini(&vd);
    return st;
}

static pc_status parse_v6(psd_rd *r, parse_state *ps, pc_error *err)
{
    pc_psd_abr *out = ps->out;
    pc_status st = psd_u16(r, &out->subversion, err);
    if (st != PC_OK) {
        return st;
    }
    const uint8_t *phry = NULL;
    size_t phry_len = 0;
    if (out->subversion != 1 && out->subversion != 2) {
        return psd_unsupported(err, "brush file subversion %u", (unsigned)out->subversion);
    }
    while (psd_remaining(r) >= 12) {
        uint8_t sig[4];
        uint8_t key[4];
        uint32_t len;
        const uint8_t *data;
        size_t dlen;
        size_t pad;
        if ((st = psd_arr4(r, sig, err)) != PC_OK) {
            return st;
        }
        if (memcmp(sig, "8BIM", 4) != 0) {
            if ((st = psd_warn(&out->warnings, err, "unexpected data at offset %zu ignored", r->pos - 4)) !=
                PC_OK) {
                return st;
            }
            break;
        }
        if ((st = psd_arr4(r, key, err)) != PC_OK || (st = psd_u32(r, &len, err)) != PC_OK) {
            return st;
        }
        dlen = len;
        if (psd_bytes(r, dlen, &data, NULL) != PC_OK) {
            pc_psd_str k = {NULL, 0};
            if ((st = lossy(key, 4, &k, err)) != PC_OK) {
                return st;
            }
            st = psd_warn(&out->warnings, err, "section %s truncated", pc_psd_str_c(&k));
            pc_psd_str_fini(&k);
            if (st != PC_OK) {
                return st;
            }
            dlen = psd_remaining(r);
            if ((st = psd_bytes(r, dlen, &data, err)) != PC_OK) {
                return st;
            }
        }
        /* Sections are padded to 4 bytes; tolerate writers that don't pad. */
        pad = (4 - len % 4) % 4;
        if (pad > 0 && !(psd_remaining(r) >= 4 && memcmp(psd_rest(r), "8BIM", 4) == 0) &&
            psd_remaining(r) >= pad + 4 && memcmp(psd_rest(r) + pad, "8BIM", 4) == 0) {
            r->pos += pad;
        }
        if (memcmp(key, "samp", 4) == 0) {
            st = read_samp(data, dlen, out->subversion, ps, err);
        } else if (memcmp(key, "patt", 4) == 0) {
            pc_psd_pattern *p = NULL;
            size_t np = 0;
            pc_error e;
            st = pc_psd_parse_pattern_block(data, dlen, &p, &np, &e);
            if (st == PC_OK) {
                pc_psd_pattern *g = np == 0 ? out->patterns
                                            : realloc(out->patterns, (out->npatterns + np) * sizeof *g);
                if (np > 0 && g == NULL) {
                    pc_psd_patterns_free(p, np);
                    return psd_nomem(err);
                }
                out->patterns = g;
                if (np > 0) {
                    memcpy(out->patterns + out->npatterns, p, np * sizeof *p);
                }
                out->npatterns += np;
                free(p);
            } else if (st == PC_ERR_NOMEM) {
                return rethrow(st, &e, err);
            } else {
                st = psd_warn(&out->warnings, err, "embedded patterns unreadable: %s", e.msg);
            }
        } else if (memcmp(key, "desc", 4) == 0) {
            st = read_desc(data, dlen, ps, err);
        } else if (memcmp(key, "phry", 4) == 0) {
            /* Read once desc gives the preset count; phry may come first. */
            phry = data;
            phry_len = dlen;
        } else if (memcmp(key, "lPdc", 4) == 0) {
            /* Tool presets: not needed for the brushes. */
        } else {
            pc_psd_str k = {NULL, 0};
            if ((st = lossy(key, 4, &k, err)) != PC_OK) {
                return st;
            }
            st = psd_warn(&out->warnings, err, "section %s ignored", pc_psd_str_c(&k));
            pc_psd_str_fini(&k);
        }
        if (st != PC_OK) {
            return st;
        }
    }
    if (phry != NULL) {
        return psd_abr_read_phry(phry, phry_len, out->npresets, &out->warnings, &out->folders, &out->nfolders,
                                 err);
    }
    return PC_OK;
}

pc_status pc_psd_abr_parse(const uint8_t *data, size_t len, pc_psd_abr *out, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pc_psd_abr f;
    parse_state ps;
    uint16_t version;
    pc_status st;
    memset(&f, 0, sizeof f);
    memset(&ps, 0, sizeof ps);
    ps.out = &f;
    ps.budget = PC_PSD_ABR_MAX_TOTAL_BYTES;
    CK(psd_u16(&r, &version, err));
    f.version = version;
    if (version == 1 || version == 2) {
        CK(parse_v12(&r, version, &ps, err));
    } else if (version >= 6 && version <= 10) {
        CK(parse_v6(&r, &ps, err));
    } else {
        st = psd_unsupported(err, "brush file version %u", (unsigned)version);
        goto fail;
    }
    if (f.nlegacy == 0 && f.nsamples == 0 && f.npresets == 0) {
        if (f.warnings.n > 0) {
            st = psd_invalid(err, "the brush file holds no readable brushes (%s)", f.warnings.v[0]);
        } else {
            st = psd_invalid(err, "the brush file holds no readable brushes");
        }
        goto fail;
    }
    *out = f;
    return PC_OK;
fail:
    pc_psd_abr_fini(&f);
    return st;
}

/* ---- writing ---- */

static void write_sample_body(psd_w *o, const pc_psd_abr_sample *s, bool rle)
{
    pc_psd_layout l;
    pc_psd_bytes enc = {NULL, 0};
    pc_status st;
    if (!psd_w_ok(o)) {
        return;
    }
    if (s->width == 0 || s->height == 0 || s->width > PC_PSD_ABR_MAX_EDGE || s->height > PC_PSD_ABR_MAX_EDGE ||
        (s->depth != 8 && s->depth != 16)) {
        psd_w_fail(o, psd_limit(o->err, "brush sample size or depth"));
        return;
    }
    psd_put_i32(o, 0);
    psd_put_i32(o, 0);
    psd_put_i32(o, (int32_t)s->height);
    psd_put_i32(o, (int32_t)s->width);
    psd_put_u16(o, s->depth);
    psd_put_u8(o, rle ? 1 : 0);
    l.planes = 1;
    l.width = s->width;
    l.height = s->height;
    l.depth = s->depth;
    l.version = PC_PSD_V1;
    st = pc_psd_encode_planes(rle ? PC_PSD_RLE : PC_PSD_RAW, s->data.data, s->data.len, &l, &enc, o->err);
    if (st != PC_OK) {
        psd_w_fail(o, st);
        return;
    }
    psd_put(o, enc.data, enc.len);
    pc_psd_bytes_fini(&enc);
}

static void put_name_units(psd_w *w, const pc_psd_str *name)
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
    psd_put_u32(w, (uint32_t)(n + 1));
    for (i = 0; i < n; i++) {
        psd_put_u16(w, u[i]);
    }
    psd_put_u16(w, 0);
    free(u);
}

pc_status pc_psd_abr_write_v12(uint16_t version, const pc_psd_abr_legacy *b, size_t n, bool rle, pc_psd_bytes *out,
                               pc_error *err)
{
    psd_w o;
    size_t i;
    if ((version != 1 && version != 2) || n > 0xFFFF) {
        return psd_unsupported(err, "writing brush file version %u", (unsigned)version);
    }
    psd_w_init(&o, err);
    psd_put_u16(&o, version);
    psd_put_u16(&o, (uint16_t)n);
    for (i = 0; i < n && psd_w_ok(&o); i++) {
        psd_w body;
        pc_psd_bytes bb = {NULL, 0};
        uint16_t ty;
        pc_status st;
        psd_w_init(&body, err);
        psd_put_u32(&body, 0);
        psd_put_u16(&body, b[i].spacing);
        if (b[i].tip == PC_PSD_ABR_COMPUTED) {
            psd_put_u16(&body, b[i].diameter);
            psd_put_u16(&body, b[i].roundness);
            psd_put_u16(&body, (uint16_t)b[i].angle);
            psd_put_u16(&body, b[i].hardness);
            ty = 1;
        } else {
            const pc_psd_abr_sample *s = &b[i].sample;
            if (version == 2) {
                put_name_units(&body, &b[i].name);
            }
            psd_put_u8(&body, b[i].anti_alias ? 1 : 0);
            psd_put_u16(&body, 0);
            psd_put_u16(&body, 0);
            psd_put_u16(&body, (uint16_t)(s->height < 0xFFFF ? s->height : 0xFFFF));
            psd_put_u16(&body, (uint16_t)(s->width < 0xFFFF ? s->width : 0xFFFF));
            write_sample_body(&body, s, rle);
            ty = 2;
        }
        st = psd_w_finish(&body, &bb);
        if (st != PC_OK) {
            psd_w_fail(&o, st);
            break;
        }
        psd_put_u16(&o, ty);
        psd_put_u32(&o, (uint32_t)bb.len);
        psd_put(&o, bb.data, bb.len);
        pc_psd_bytes_fini(&bb);
    }
    return psd_w_finish(&o, out);
}

static void section(psd_w *o, const char key[4], const pc_psd_bytes *data)
{
    size_t pad = (4 - data->len % 4) % 4;
    psd_put(o, "8BIM", 4);
    psd_put(o, key, 4);
    psd_put_u32(o, (uint32_t)(data->len + pad));
    psd_put(o, data->data, data->len);
    psd_put_zeros(o, pad);
}

pc_status pc_psd_abr_write_v6(uint16_t subversion, const pc_psd_abr_sample *s, size_t ns, const pc_psd_pattern *p,
                              size_t np, const pc_psd_desc *presets, size_t npresets, bool rle, pc_psd_bytes *out,
                              pc_error *err)
{
    return pc_psd_abr_write_v6_folders(subversion, s, ns, p, np, presets, npresets, NULL, 0, rle, out, err);
}

pc_status pc_psd_abr_write_v6_folders(uint16_t subversion, const pc_psd_abr_sample *s, size_t ns,
                                      const pc_psd_pattern *p, size_t np, const pc_psd_desc *presets,
                                      size_t npresets, const pc_psd_abr_folder *folders, size_t nfolders,
                                      bool rle, pc_psd_bytes *out, pc_error *err)
{
    pc_psd_bytes hb = {NULL, 0};
    bool any_folder = false;
    psd_w o;
    psd_w samp;
    pc_psd_bytes sb = {NULL, 0};
    pc_psd_bytes pb = {NULL, 0};
    pc_psd_bytes db = {NULL, 0};
    pc_psd_desc root;
    pc_psd_value list;
    size_t i;
    pc_status st;
    if (nfolders != 0 && nfolders != npresets) {
        return psd_misuse(err, "%zu folder paths for %zu brush presets", nfolders, npresets);
    }
    if (subversion != 1 && subversion != 2) {
        return psd_unsupported(err, "brush file subversion %u", (unsigned)subversion);
    }
    for (i = 0; i < nfolders; i++) {
        any_folder = any_folder || folders[i].n > 0;
    }
    psd_w_init(&samp, err);
    for (i = 0; i < ns && psd_w_ok(&samp); i++) {
        psd_w e;
        pc_psd_bytes eb = {NULL, 0};
        size_t idl = s[i].id.len < 255 ? s[i].id.len : 255;
        psd_w_init(&e, err);
        psd_put_u8(&e, (uint8_t)idl);
        psd_put(&e, s[i].id.s, idl);
        psd_put_zeros(&e, subversion == 1 ? 10 : 264);
        write_sample_body(&e, &s[i], rle);
        st = psd_w_finish(&e, &eb);
        if (st != PC_OK) {
            psd_w_fail(&samp, st);
            break;
        }
        psd_put_u32(&samp, (uint32_t)eb.len);
        psd_put(&samp, eb.data, eb.len);
        psd_put_zeros(&samp, (4 - eb.len % 4) % 4);
        pc_psd_bytes_fini(&eb);
    }
    st = psd_w_finish(&samp, &sb);
    if (st != PC_OK) {
        return st;
    }
    if (np > 0 && (st = pc_psd_write_pattern_block(p, np, &pb, err)) != PC_OK) {
        pc_psd_bytes_fini(&sb);
        return st;
    }
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    st = pc_psd_desc_init(&root, "null", err);
    for (i = 0; i < npresets && st == PC_OK; i++) {
        pc_psd_desc c;
        pc_psd_value v;
        st = pc_psd_desc_clone(&presets[i], &c, err);
        if (st == PC_OK) {
            st = pc_psd_value_object(&c, &v, err);
        }
        if (st == PC_OK) {
            st = pc_psd_value_list_push(&list, &v, err);
        }
    }
    if (st == PC_OK) {
        st = pc_psd_desc_add(&root, "Brsh", &list, err);
    }
    if (st == PC_OK) {
        st = pc_psd_vdesc_to_bytes(&root, &db, err);
    }
    if (st == PC_OK && any_folder) {
        st = psd_abr_phry_bytes(folders, nfolders, &hb, err);
    }
    pc_psd_value_fini(&list);
    pc_psd_desc_fini(&root);
    if (st != PC_OK) {
        pc_psd_bytes_fini(&sb);
        pc_psd_bytes_fini(&pb);
        pc_psd_bytes_fini(&db);
        return st;
    }
    psd_w_init(&o, err);
    psd_put_u16(&o, 6);
    psd_put_u16(&o, subversion);
    section(&o, "samp", &sb);
    if (np > 0) {
        section(&o, "patt", &pb);
    }
    section(&o, "desc", &db);
    if (any_folder) {
        section(&o, "phry", &hb);
    }
    pc_psd_bytes_fini(&sb);
    pc_psd_bytes_fini(&pb);
    pc_psd_bytes_fini(&db);
    pc_psd_bytes_fini(&hb);
    return psd_w_finish(&o, out);
}
