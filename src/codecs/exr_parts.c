/* Multi-part OpenEXR headers (port of the info side of crates/codecs/src/codecs/exr.rs): the
 * part list with names, views, sizes and channels, from the own header parser (deep_exr.c), so it
 * works without OpenEXRCore too. */
#include <stdlib.h>
#include <string.h>

#include "codecs/deep_exr.h"
#include "codecs/internal.h"

#define F PC_CF_EXR

char *cx_latin1_to_utf8(const uint8_t *s, size_t n)
{
    size_t i;
    size_t o = 0;
    char *d;
    if (n > (SIZE_MAX - 1) / 2) {
        return NULL;
    }
    d = malloc(2 * n + 1);
    if (d == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (s[i] < 0x80) {
            d[o++] = (char)s[i];
        } else {
            d[o++] = (char)(0xC0 | (s[i] >> 6));
            d[o++] = (char)(0x80 | (s[i] & 0x3F));
        }
    }
    d[o] = '\0';
    return d;
}

bool cx_latin1_eq_utf8(const char *latin1, const char *utf8)
{
    const uint8_t *a = (const uint8_t *)latin1;
    const uint8_t *b = (const uint8_t *)utf8;
    for (;;) {
        if (*a < 0x80) {
            if (*a != *b) {
                return false;
            }
            if (*a == 0) {
                return true;
            }
            a++;
            b++;
        } else {
            if (b[0] != (uint8_t)(0xC0 | (*a >> 6)) || b[1] != (uint8_t)(0x80 | (*a & 0x3F))) {
                return false;
            }
            a++;
            b += 2;
        }
    }
}

pc_status cx_exr_meta_checked(const uint8_t *b, size_t n, const pc_climits *l, exr_meta *out, pc_error *err)
{
    exr_meta m;
    size_t i;
    uint64_t total = 0;
    pc_status st = cx_exr_meta_parse(b, n, &m, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < m.n; i++) {
        uint32_t w = m.parts[i].w > UINT32_MAX ? UINT32_MAX : (uint32_t)m.parts[i].w;
        uint32_t h = m.parts[i].h > UINT32_MAX ? UINT32_MAX : (uint32_t)m.parts[i].h;
        uint64_t bpp = (uint64_t)(m.parts[i].nch > 0 ? m.parts[i].nch : 1) * 4;
        uint64_t bytes;
        if ((st = cx_check_bytes(l, w, h, bpp, err)) != PC_OK) {
            cx_exr_meta_fini(&m);
            return st;
        }
        /* The flat decoder reads every part, so the parts are also capped together. */
        bytes = (uint64_t)w * h;
        bytes = bytes > UINT64_MAX / bpp ? UINT64_MAX : bytes * bpp;
        total = total > UINT64_MAX - bytes ? UINT64_MAX : total + bytes;
    }
    if (total > l->max_alloc) {
        st = cx_limit(err, "%llu bytes across %zu parts exceed max_alloc %llu", (unsigned long long)total,
                      m.n, (unsigned long long)l->max_alloc);
        cx_exr_meta_fini(&m);
        return st;
    }
    *out = m;
    return PC_OK;
}

static const char *base_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL ? dot + 1 : name;
}

static bool hdr_has(const exr_hdr *h, const char *c)
{
    size_t i;
    for (i = 0; i < h->nch; i++) {
        if (strcmp(base_of(h->ch[i].name), c) == 0) {
            return true;
        }
    }
    return false;
}

uint8_t cx_exr_hdr_rank(const exr_hdr *h)
{
    if (hdr_has(h, "R") && hdr_has(h, "G") && hdr_has(h, "B")) {
        return hdr_has(h, "A") ? 4 : 3;
    }
    if (hdr_has(h, "Y")) {
        return hdr_has(h, "A") ? 2 : 1;
    }
    return h->nch == 1 ? 1 : 0;
}

pc_status pc_cexr_info(const uint8_t *b, size_t n, const pc_climits *l, pc_exr_part_info **parts,
                       size_t *nparts, pc_error *err)
{
    exr_meta m;
    pc_exr_part_info *v;
    size_t i;
    pc_status st = cx_exr_meta_checked(b, n, l, &m, err);
    *parts = NULL;
    *nparts = 0;
    if (st != PC_OK) {
        return st;
    }
    v = calloc(m.n > 0 ? m.n : 1, sizeof *v);
    if (v == NULL) {
        cx_exr_meta_fini(&m);
        return cx_nomem(err);
    }
    for (i = 0; i < m.n; i++) {
        const exr_hdr *h = &m.parts[i];
        pc_exr_part_info *p = &v[i];
        size_t c;
        p->index = i;
        p->width = h->w > UINT32_MAX ? UINT32_MAX : (uint32_t)h->w;
        p->height = h->h > UINT32_MAX ? UINT32_MAX : (uint32_t)h->h;
        p->deep = h->deep;
        p->tiled = h->tiled;
        if ((h->name != NULL && (p->name = cx_latin1_to_utf8(h->name, h->name_len)) == NULL) ||
            (h->view != NULL && (p->view = cx_latin1_to_utf8(h->view, h->view_len)) == NULL) ||
            (p->channels = calloc(h->nch > 0 ? h->nch : 1, sizeof *p->channels)) == NULL) {
            goto oom;
        }
        for (c = 0; c < h->nch; c++) {
            p->channels[c].name = cx_latin1_to_utf8((const uint8_t *)h->ch[c].name, strlen(h->ch[c].name));
            if (p->channels[c].name == NULL) {
                goto oom;
            }
            /* 32-bit unsigned channels (ID buffers) are listed as F32: exact up to 2^24. */
            p->channels[c].sample = h->ch[c].type == 1 ? PC_CS_F16 : PC_CS_F32;
            p->nchannels = c + 1;
        }
    }
    *parts = v;
    *nparts = m.n;
    cx_exr_meta_fini(&m);
    return PC_OK;
oom:
    pc_exr_parts_free(v, m.n);
    cx_exr_meta_fini(&m);
    return cx_nomem(err);
}
