/* HEIF / HEIC, read-only (port of crates/codecs/src/codecs/heif.rs and crates/heif): libheif
 * (linked dynamically, REWRITE_PLAN.md D18) decodes the primary image, 8-bit or (above 8 bits)
 * 16-bit RGB(A), with its ICC profile, EXIF (as a TIFF structure, without HEIF's offset header)
 * and XMP. libheif only decodes the coded pictures (and assembles grids) to its native YCbCr /
 * monochrome planes; the colour conversion is a port of heic-rs 0.1.1 (the reference's decoder:
 * color/fixed.rs, color/kernel.rs, upsample.rs, transform.rs), so every sample matches it. HEIF
 * records orientation in the container (irot/imir/clap), applied to the converted pixels unless
 * keep_orientation; the EXIF orientation is then rewritten to 1 (it only mirrors the container).
 * Built when libheif is found (WITH_HEIF); without it HEIF is detected, and opening one says the
 * build has no HEIF support. */
#include <stdlib.h>
#include <string.h>

#include "base/pool.h"
#include "base/utf8.h"
#include "codecs/internal.h"
#include "codecs/orientation.h"

#define F PC_CF_HEIF
#define SEQUENCE "this file holds an image sequence; only HEIF still images can be opened"

#ifndef PC_NO_HEIF

#include <libheif/heif.h>
#include <pthread.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static bool have_hevc;

static void init_once(void)
{
    (void)heif_init(NULL);
    have_hevc = heif_have_decoder_for_format(heif_compression_HEVC) != 0;
}

bool cx_heif_available(void)
{
    (void)pthread_once(&once, init_once);
    return have_hevc;
}

static pc_status map_err(struct heif_error e, pc_error *err)
{
    /* Some libheif messages end in a newline; it is not part of ours. */
    const char *m = e.message != NULL ? e.message : "";
    int n = (int)strnlen(m, 1024);
    while (n > 0 && (m[n - 1] == '\n' || m[n - 1] == '\r')) {
        n--;
    }
    switch (e.code) {
    case heif_error_Unsupported_feature:
    case heif_error_Unsupported_filetype:
    case heif_error_Plugin_loading_error:
        return cx_unsupported(err, F, "%.*s", n, m);
    case heif_error_Memory_allocation_error:
        return cx_limit(err, "%.*s", n, m);
    default:
        if (e.subcode == heif_suberror_Security_limit_exceeded) {
            return cx_limit(err, "%.*s", n, m);
        }
        return cx_malformed(err, F, "%.*s", n, m);
    }
}

/* A top-level box of this type (the file has a meta box unless it is only a sequence). */
static bool has_box(const uint8_t *b, size_t n, const char *type)
{
    size_t at = 0;
    while (n - at >= 8) {
        uint64_t size = cx_be32(b + at);
        size_t hdr = 8;
        if (memcmp(b + at + 4, type, 4) == 0) {
            return true;
        }
        if (size == 1) {
            if (n - at < 16) {
                return false;
            }
            size = ((uint64_t)cx_be32(b + at + 8) << 32) | cx_be32(b + at + 12);
            hdr = 16;
        } else if (size == 0) {
            return false;
        }
        if (size < hdr || size > n - at) {
            return false;
        }
        at += (size_t)size;
    }
    return false;
}

static pc_status read_metadata(const struct heif_image_handle *h, pc_cimage *img, bool keep, pc_error *err)
{
    heif_item_id ids[16];
    int n;
    int i;
    pc_status st = PC_OK;
    size_t sz = heif_image_handle_get_raw_color_profile_size(h);
    if (sz > 0) {
        uint8_t *p = malloc(sz);
        if (p == NULL) {
            return cx_nomem(err);
        }
        if (heif_image_handle_get_raw_color_profile(h, p).code == heif_error_Ok) {
            st = pc_meta_blob_set(&img->icc, p, sz, err);
        }
        free(p);
        if (st != PC_OK) {
            return st;
        }
    }
    n = heif_image_handle_get_list_of_metadata_block_IDs(h, "Exif", ids, 16);
    for (i = 0; i < n && !img->meta.exif.has; i++) {
        size_t len = heif_image_handle_get_metadata_size(h, ids[i]);
        uint8_t *p = len >= 4 ? malloc(len) : NULL;
        if (p != NULL && heif_image_handle_get_metadata(h, ids[i], p).code == heif_error_Ok) {
            /* a 4-byte offset to the TIFF header comes first */
            uint32_t off = cx_be32(p);
            if (off <= len - 4) {
                const uint8_t *t = p + 4 + off;
                size_t tn = len - 4 - off;
                uint8_t *up = NULL;
                size_t un = 0;
                if (!keep) {
                    st = pc_upright_exif(t, tn, &up, &un, err);
                }
                if (st == PC_OK) {
                    st = pc_meta_blob_set(&img->meta.exif, up != NULL ? up : t, up != NULL ? un : tn, err);
                }
                free(up);
            }
        }
        free(p);
        if (st != PC_OK) {
            return st;
        }
    }
    n = heif_image_handle_get_list_of_metadata_block_IDs(h, "mime", ids, 16);
    for (i = 0; i < n && !img->meta.xmp.has; i++) {
        const char *ct = heif_image_handle_get_metadata_content_type(h, ids[i]);
        size_t len = heif_image_handle_get_metadata_size(h, ids[i]);
        uint8_t *p;
        if (ct == NULL || strcmp(ct, "application/rdf+xml") != 0 || len == 0) {
            continue;
        }
        p = malloc(len);
        if (p != NULL && heif_image_handle_get_metadata(h, ids[i], p).code == heif_error_Ok) {
            size_t k = 0;
            uint32_t cp;
            bool ok = true;
            while (k < len) {
                if (!pc_utf8_next(p, len, &k, &cp)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                char *up = NULL;
                size_t un = 0;
                while (len > 0 && p[len - 1] == 0) {
                    len--;
                }
                if (!keep) {
                    st = pc_upright_xmp((const char *)p, len, &up, &un, err);
                }
                if (st == PC_OK) {
                    st = pc_meta_blob_set(&img->meta.xmp, up != NULL ? (const uint8_t *)up : p,
                                          up != NULL ? un : len, err);
                }
                free(up);
            }
        }
        free(p);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

/* ---- the primary item's colr (nclx) and transforms, read the way heic-rs reads them ----
 *
 * libheif decodes the pictures (and assembles grids) to its native YCbCr planes; the colour
 * conversion and the container transforms are ours, so they follow the reference exactly. The
 * container is walked here for the two things that decide them: the last `nclx` colr associated
 * with the primary item (none: BT.709 limited range) and its irot / imir / clap properties in
 * association order. A container this walk cannot follow (heic-rs refuses it) keeps the defaults. */

enum { HX_CROP, HX_ROTATE, HX_MIRROR };

typedef struct hx_xform {
    int kind;
    uint8_t v; /* ROTATE: quarter turns anticlockwise; MIRROR: 0 left-right, 1 top-bottom */
    uint32_t wn, wd, hn, hd, hod, vod;
    int32_t hon, von;
} hx_xform;

typedef struct hx_props {
    bool full;       /* nclx full range */
    uint16_t matrix; /* nclx matrix coefficients (1 = BT.709 without an nclx) */
    bool zero_den;   /* a clap with a zero denominator (heic-rs refuses the file) */
    bool ispe;       /* iw x ih from the item's (last) ispe */
    uint32_t iw, ih;
    hx_xform *xf;
    size_t nxf;
    size_t cap;
} hx_props;

typedef struct hx_box {
    uint8_t type[4];
    const uint8_t *p;
    size_t n;
} hx_box;

typedef struct hx_iter {
    const uint8_t *d;
    size_t n;
    size_t pos;
} hx_iter;

/* heic-rs BoxIter: 1 = a box, 0 = the end, -1 = broken. */
static int hx_next(hx_iter *it, hx_box *b)
{
    size_t start = it->pos;
    size_t rest;
    uint64_t total;
    size_t hdr = 8;
    size_t body;
    if (start >= it->n) {
        return 0;
    }
    rest = it->n - start;
    if (rest < 8) {
        return -1;
    }
    total = cx_be32(it->d + start);
    memcpy(b->type, it->d + start + 4, 4);
    if (total == 1) {
        if (rest < 16) {
            return -1;
        }
        total = ((uint64_t)cx_be32(it->d + start + 8) << 32) | cx_be32(it->d + start + 12);
        hdr = 16;
    } else if (total == 0) {
        total = rest;
    }
    body = hdr;
    if (memcmp(b->type, "uuid", 4) == 0) {
        if (rest - hdr < 16) {
            return -1;
        }
        body += 16;
    }
    if (total < body || total > ((uint64_t)1 << 30) || total > rest) {
        return -1;
    }
    b->p = it->d + start + body;
    b->n = (size_t)total - body;
    it->pos = start + (size_t)total;
    return 1;
}

static bool hx_push(hx_props *pr, hx_xform x)
{
    if (pr->nxf == pr->cap) {
        size_t cap = pr->cap == 0 ? 4 : pr->cap * 2;
        hx_xform *p = realloc(pr->xf, cap * sizeof *p);
        if (p == NULL) {
            return false;
        }
        pr->xf = p;
        pr->cap = cap;
    }
    pr->xf[pr->nxf++] = x;
    return true;
}

/* One associated property; false when out of memory. */
static bool hx_prop(hx_props *pr, const hx_box *b)
{
    hx_xform x;
    memset(&x, 0, sizeof x);
    if (memcmp(b->type, "colr", 4) == 0) {
        if (b->n >= 11 && memcmp(b->p, "nclx", 4) == 0) {
            pr->matrix = cx_be16(b->p + 8);
            pr->full = (b->p[10] & 0x80) != 0;
        }
        return true;
    }
    if (memcmp(b->type, "ispe", 4) == 0 && b->n >= 12) {
        pr->ispe = true;
        pr->iw = cx_be32(b->p + 4);
        pr->ih = cx_be32(b->p + 8);
        return true;
    }
    if (memcmp(b->type, "irot", 4) == 0 && b->n >= 1) {
        x.kind = HX_ROTATE;
        x.v = b->p[0] & 3;
        return hx_push(pr, x);
    }
    if (memcmp(b->type, "imir", 4) == 0 && b->n >= 1) {
        x.kind = HX_MIRROR;
        x.v = b->p[0] & 1;
        return hx_push(pr, x);
    }
    if (memcmp(b->type, "clap", 4) == 0 && b->n >= 32) {
        x.kind = HX_CROP;
        x.wn = cx_be32(b->p);
        x.wd = cx_be32(b->p + 4);
        x.hn = cx_be32(b->p + 8);
        x.hd = cx_be32(b->p + 12);
        x.hon = (int32_t)cx_be32(b->p + 16);
        x.hod = cx_be32(b->p + 20);
        x.von = (int32_t)cx_be32(b->p + 24);
        x.vod = cx_be32(b->p + 28);
        if (x.wd == 0 || x.hd == 0 || x.hod == 0 || x.vod == 0) {
            pr->zero_den = true;
            return true;
        }
        return hx_push(pr, x);
    }
    return true;
}

/* An ipma box: the associations of `item`, applied in order. 0 ok, -1 broken, -2 out of memory. */
static int hx_ipma(hx_props *pr, const hx_box *ipma, uint32_t item, const hx_box *boxes, size_t nboxes)
{
    const uint8_t *p = ipma->p;
    size_t n = ipma->n;
    size_t at = 8;
    uint32_t count;
    uint32_t e;
    uint8_t version;
    bool wide;
    if (n < 8) {
        return -1;
    }
    version = p[0];
    wide = (p[3] & 1) != 0;
    count = cx_be32(p + 4);
    if (count > (n - at) / 3 + 1) {
        return -1;
    }
    for (e = 0; e < count; e++) {
        uint32_t id;
        uint8_t k;
        uint8_t na;
        if (version < 1) {
            if (n - at < 2) {
                return -1;
            }
            id = cx_be16(p + at);
            at += 2;
        } else {
            if (n - at < 4) {
                return -1;
            }
            id = cx_be32(p + at);
            at += 4;
        }
        if (n - at < 1) {
            return -1;
        }
        na = p[at++];
        for (k = 0; k < na; k++) {
            size_t idx;
            if (wide) {
                if (n - at < 2) {
                    return -1;
                }
                idx = cx_be16(p + at) & 0x7fffu;
                at += 2;
            } else {
                if (n - at < 1) {
                    return -1;
                }
                idx = p[at] & 0x7fu;
                at++;
            }
            if (id == item && idx != 0 && idx <= nboxes && !hx_prop(pr, &boxes[idx - 1])) {
                return -2;
            }
        }
    }
    return 0;
}

static void hx_props_reset(hx_props *pr)
{
    free(pr->xf);
    memset(pr, 0, sizeof *pr);
    pr->matrix = 1;
}

/* The children of every ipco box in iprp, stored in out unless NULL; 0, or -1 when broken. */
static int hx_ipco(const hx_box *iprp, hx_box *out, size_t *count)
{
    hx_iter it = {iprp->p, iprp->n, 0};
    hx_box bx;
    size_t k = 0;
    int r;
    while ((r = hx_next(&it, &bx)) == 1) {
        hx_iter in = {bx.p, bx.n, 0};
        hx_box c;
        if (memcmp(bx.type, "ipco", 4) != 0) {
            continue;
        }
        while ((r = hx_next(&in, &c)) == 1) {
            if (out != NULL && k < *count) {
                out[k] = c;
            }
            k++;
        }
        if (r < 0) {
            return -1;
        }
    }
    *count = k;
    return r < 0 ? -1 : 0;
}

/* Fills pr for `item` (defaults when the container can't be followed). Fails only on memory. */
static pc_status hx_read_props(const uint8_t *b, size_t n, uint32_t *item, bool *have_item, hx_props *pr,
                               pc_error *err)
{
    hx_iter it = {b, n, 0};
    hx_box bx;
    hx_box meta;
    hx_box iprp = {{0, 0, 0, 0}, NULL, 0};
    hx_box *boxes = NULL;
    size_t nboxes = 0;
    bool found = false;
    bool has_pitm = false;
    uint32_t pitm = 0;
    int r = 0;
    int st = 0;
    hx_props_reset(pr);
    while ((r = hx_next(&it, &bx)) == 1 && memcmp(bx.type, "meta", 4) != 0) {
    }
    if (r != 1 || bx.n < 4) {
        return PC_OK;
    }
    meta = bx;
    it = (hx_iter){meta.p + 4, meta.n - 4, 0};
    while ((r = hx_next(&it, &bx)) == 1) {
        if (memcmp(bx.type, "iprp", 4) == 0) {
            iprp = bx;
            found = true;
        } else if (!*have_item && memcmp(bx.type, "pitm", 4) == 0 && bx.n >= 6) {
            pitm = bx.p[0] == 0 ? cx_be16(bx.p + 4) : bx.n >= 8 ? cx_be32(bx.p + 4) : 0;
            has_pitm = bx.p[0] == 0 || bx.n >= 8;
        }
    }
    if (!*have_item) {
        if (r < 0 || !has_pitm) {
            return PC_OK;
        }
        *item = pitm;
        *have_item = true;
    }
    if (r < 0 || !found) {
        return PC_OK;
    }
    /* every ipco child, in order (property indices are 1-based across them): counted, then kept */
    if ((st = hx_ipco(&iprp, NULL, &nboxes)) == 0) {
        boxes = malloc((nboxes > 0 ? nboxes : 1) * sizeof *boxes);
        if (boxes == NULL) {
            return cx_nomem(err);
        }
        st = hx_ipco(&iprp, boxes, &nboxes);
    }
    it = (hx_iter){iprp.p, iprp.n, 0};
    while (st == 0 && (r = hx_next(&it, &bx)) == 1) {
        if (memcmp(bx.type, "ipma", 4) == 0) {
            st = hx_ipma(pr, &bx, *item, boxes, nboxes);
        }
    }
    st = st == 0 && r < 0 ? -1 : st;
    free(boxes);
    if (st == -2) {
        hx_props_reset(pr);
        return cx_nomem(err);
    }
    if (st != 0) {
        hx_props_reset(pr);
    }
    return PC_OK;
}

/* ---- transforms (heic-rs transform.rs) ---- */

static pc_status hx_crop_size(uint32_t w, uint32_t h, const hx_xform *c, uint32_t *cw, uint32_t *ch,
                              pc_error *err)
{
    *cw = c->wn / c->wd;
    *ch = c->hn / c->hd;
    if (*cw == 0 || *ch == 0 || *cw > w || *ch > h) {
        return cx_malformed(err, F, "malformed file: clap declares an impossible cropped size");
    }
    return PC_OK;
}

static pc_status hx_corner(uint32_t full, uint32_t cropped, int32_t on, uint32_t od, uint32_t *out,
                           pc_error *err)
{
    int64_t off2 = 2 * (int64_t)on / (int64_t)od;
    int64_t left2 = (int64_t)full - 1 + off2 - ((int64_t)cropped - 1);
    if (left2 < 0) {
        return cx_malformed(err, F, "malformed file: clap selects a rectangle outside the image");
    }
    if (left2 / 2 > (int64_t)UINT32_MAX) {
        return cx_malformed(err, F, "malformed file: clap offset is out of range");
    }
    *out = (uint32_t)(left2 / 2);
    return PC_OK;
}

static pc_status hx_transformed_size(uint32_t *w, uint32_t *h, const hx_props *pr, pc_error *err)
{
    size_t i;
    for (i = 0; i < pr->nxf; i++) {
        const hx_xform *t = &pr->xf[i];
        pc_status st;
        if (t->kind == HX_CROP) {
            if ((st = hx_crop_size(*w, *h, t, w, h, err)) != PC_OK) {
                return st;
            }
        } else if (t->kind == HX_ROTATE && (t->v & 1) != 0) {
            uint32_t s = *w;
            *w = *h;
            *h = s;
        }
    }
    return PC_OK;
}

/* What the reference refuses before decoding: a zero clap denominator, or (with the item's ispe)
 * a clap that cannot apply to the coded size. */
static pc_status hx_check_props(const hx_props *pr, pc_error *err)
{
    uint32_t w = pr->iw;
    uint32_t h = pr->ih;
    if (pr->zero_den) {
        return cx_malformed(err, F, "malformed file: clap has a zero denominator");
    }
    return pr->ispe ? hx_transformed_size(&w, &h, pr, err) : PC_OK;
}

static pc_status hx_apply(pc_cimage *img, const hx_props *pr, pc_error *err)
{
    size_t bpp = pc_clayout_channels(img->layout) * pc_csample_bytes(img->sample);
    size_t i;
    for (i = 0; i < pr->nxf; i++) {
        const hx_xform *t = &pr->xf[i];
        size_t w = img->width;
        size_t h = img->height;
        size_t row = w * bpp;
        size_t x;
        size_t y;
        uint8_t *d;
        if (t->kind == HX_CROP) {
            uint32_t cw;
            uint32_t ch;
            uint32_t left;
            uint32_t top;
            pc_status st;
            if ((st = hx_crop_size(img->width, img->height, t, &cw, &ch, err)) != PC_OK ||
                (st = hx_corner(img->width, cw, t->hon, t->hod, &left, err)) != PC_OK ||
                (st = hx_corner(img->height, ch, t->von, t->vod, &top, err)) != PC_OK) {
                return st;
            }
            if ((uint64_t)left + cw > img->width || (uint64_t)top + ch > img->height) {
                return cx_malformed(err, F, "malformed file: clap selects a rectangle outside the image");
            }
            d = malloc((size_t)cw * ch * bpp + 1);
            if (d == NULL) {
                return cx_nomem(err);
            }
            for (y = 0; y < ch; y++) {
                memcpy(d + y * cw * bpp, img->data + (top + y) * row + left * bpp, (size_t)cw * bpp);
            }
            free(img->data);
            img->data = d;
            img->width = cw;
            img->height = ch;
            img->len = (size_t)cw * ch * bpp;
        } else if (t->kind == HX_ROTATE) {
            size_t ow = (t->v & 1) != 0 ? h : w;
            if (t->v == 0) {
                continue;
            }
            d = malloc(w * h * bpp + 1);
            if (d == NULL) {
                return cx_nomem(err);
            }
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    size_t nx;
                    size_t ny;
                    if (t->v == 1) {
                        nx = y;
                        ny = w - 1 - x;
                    } else if (t->v == 2) {
                        nx = w - 1 - x;
                        ny = h - 1 - y;
                    } else {
                        nx = h - 1 - y;
                        ny = x;
                    }
                    memcpy(d + (ny * ow + nx) * bpp, img->data + (y * w + x) * bpp, bpp);
                }
            }
            free(img->data);
            img->data = d;
            img->width = (uint32_t)ow;
            img->height = (uint32_t)(ow == w ? h : w);
        } else if (t->v == 0) { /* mirror left-right */
            uint8_t tmp[16];
            for (y = 0; y < h; y++) {
                uint8_t *r = img->data + y * row;
                for (x = 0; x < w / 2; x++) {
                    memcpy(tmp, r + x * bpp, bpp);
                    memcpy(r + x * bpp, r + (w - 1 - x) * bpp, bpp);
                    memcpy(r + (w - 1 - x) * bpp, tmp, bpp);
                }
            }
        } else { /* mirror top-bottom */
            for (y = 0; y < h / 2; y++) {
                uint8_t *a = img->data + y * row;
                uint8_t *c = img->data + (h - 1 - y) * row;
                for (x = 0; x < row; x++) {
                    uint8_t s = a[x];
                    a[x] = c[x];
                    c[x] = s;
                }
            }
        }
    }
    return PC_OK;
}

/* ---- YCbCr to RGB(A) (heic-rs color/fixed.rs, color/kernel.rs, upsample.rs) ---- */

typedef struct hx_coeffs {
    uint32_t shift;
    int32_t round, max_out, y_off, c_off, ky, rv, gu, gv, bu;
} hx_coeffs;

/* Round away from zero, as heic-rs's q(). */
static int32_t hx_q(float v)
{
    return v < 0.0f ? (int32_t)(v - 0.5f) : (int32_t)(v + 0.5f);
}

static hx_coeffs hx_coeffs_new(bool full, uint16_t matrix, unsigned depth, bool wide)
{
    hx_coeffs c;
    uint32_t shift = wide ? 13u : 16u;
    int32_t max_out = wide ? 65535 : 255;
    float unit = (float)max_out * (float)(1u << shift);
    float max = (float)(int32_t)((1u << depth) - 1);
    float yo;
    float yg;
    float co;
    float cg;
    float kr = 0.2126f;
    float kb = 0.0722f;
    float kg;
    float rv;
    float bu;
    float gu;
    float gv;
    if (full) {
        yo = 0.0f;
        yg = 1.0f / max;
        co = (float)(1u << (depth - 1));
        cg = 1.0f / max;
    } else {
        float step = (float)(1u << (depth - 8));
        yo = 16.0f * step;
        yg = 1.0f / (219.0f * step);
        co = 128.0f * step;
        cg = 1.0f / (224.0f * step);
    }
    switch (matrix) {
    case 5:
    case 6:
        kr = 0.299f;
        kb = 0.114f;
        break;
    case 7:
        kr = 0.212f;
        kb = 0.087f;
        break;
    case 9:
        kr = 0.2627f;
        kb = 0.0593f;
        break;
    default:
        break;
    }
    kg = 1.0f - kr - kb;
    rv = 2.0f * (1.0f - kr);
    bu = 2.0f * (1.0f - kb);
    gu = -2.0f * kb * (1.0f - kb) / kg;
    gv = -2.0f * kr * (1.0f - kr) / kg;
    c.shift = shift;
    c.round = (int32_t)(1u << (shift - 1));
    c.max_out = max_out;
    c.y_off = (int32_t)yo;
    c.c_off = (int32_t)co;
    c.ky = hx_q(yg * unit);
    c.rv = hx_q(rv * cg * unit);
    c.gu = hx_q(gu * cg * unit);
    c.gv = hx_q(gv * cg * unit);
    c.bu = hx_q(bu * cg * unit);
    return c;
}

/* i32 arithmetic that wraps like the reference's release build (no overflow UB). */
static inline uint32_t hx_w(int32_t a)
{
    return (uint32_t)a;
}
static inline int32_t hx_i(uint32_t a)
{
    return (int32_t)a;
}

static inline int32_t hx_finish(const hx_coeffs *c, uint32_t acc)
{
    int32_t v = hx_i(acc + hx_w(c->round)) >> c->shift;
    return v < 0 ? 0 : v > c->max_out ? c->max_out : v;
}

static inline uint16_t hx_vblend(uint16_t a, uint16_t b, uint32_t w0)
{
    return (uint16_t)(((uint32_t)a * w0 + (uint32_t)b * (4 - w0) + 2) >> 2);
}

static inline uint16_t hx_havg(uint16_t a, uint16_t b)
{
    return (uint16_t)(((uint32_t)a + b + 1) >> 1);
}

enum { HX_LUMA, HX_IDENTITY, HX_MATRIX };

/* One plane of the decoded image: `bytes` per sample (1, or 2 native-endian). */
typedef struct hx_plane {
    const uint8_t *p;
    size_t stride;
    unsigned bytes;
    uint32_t w, h;
} hx_plane;

static inline uint16_t hx_px(const hx_plane *pl, size_t y, size_t x)
{
    const uint8_t *s = pl->p + y * pl->stride;
    uint16_t v;
    if (pl->bytes == 1) {
        return s[x];
    }
    memcpy(&v, s + 2 * x, 2);
    return v;
}

typedef struct hx_job {
    hx_plane y, cb, cr, a;
    bool has_a;
    uint32_t w, h, cw, ch;
    unsigned xs, ys; /* chroma subsampling shifts */
    int mode;
    hx_coeffs c;
    int32_t a_gain, a_max_in;
    uint8_t *out;
    size_t bpp;
    bool wide;
    bool rgba;
} hx_job;

/* Chroma row r of one plane expanded horizontally to w samples (upsample::horizontal). */
static void hx_horizontal(uint16_t *dst, const hx_job *j, const hx_plane *pl, size_t r)
{
    size_t n = j->cw;
    size_t w = j->w;
    size_t done;
    size_t i;
    if (j->xs == 0) {
        done = n < w ? n : w;
        for (i = 0; i < done; i++) {
            dst[i] = hx_px(pl, r, i);
        }
    } else {
        size_t body = n - 1 < w / 2 ? n - 1 : w / 2;
        for (i = 0; i < body; i++) {
            uint16_t a = hx_px(pl, r, i);
            dst[2 * i] = a;
            dst[2 * i + 1] = hx_havg(a, hx_px(pl, r, i + 1));
        }
        done = body * 2;
    }
    for (i = done; i < w; i++) {
        dst[i] = hx_px(pl, r, n - 1);
    }
}

/* A band's three expanded chroma rows (Cb then Cr in each slot), keyed by chroma row. */
typedef struct hx_ring {
    uint16_t *rows;
    size_t key[3];
    bool used[3];
} hx_ring;

static size_t hx_slot(hx_ring *g, const hx_job *j, size_t r)
{
    size_t s;
    size_t best = 0;
    size_t far = 0;
    for (s = 0; s < 3; s++) {
        if (g->used[s] && g->key[s] == r) {
            return s;
        }
    }
    for (s = 0; s < 3; s++) {
        size_t d = !g->used[s] ? SIZE_MAX : g->key[s] > r ? g->key[s] - r : r - g->key[s];
        if (d >= far) {
            far = d;
            best = s;
        }
    }
    hx_horizontal(g->rows + best * 2 * j->w, j, &j->cb, r);
    hx_horizontal(g->rows + best * 2 * j->w + j->w, j, &j->cr, r);
    g->key[best] = r;
    g->used[best] = true;
    return best;
}

static inline void hx_store(const hx_job *j, uint8_t *px, int32_t r, int32_t g, int32_t b)
{
    if (j->wide) {
        uint16_t v[4];
        v[0] = (uint16_t)r;
        v[1] = (uint16_t)g;
        v[2] = (uint16_t)b;
        v[3] = (uint16_t)j->c.max_out;
        memcpy(px, v, j->bpp);
    } else {
        px[0] = (uint8_t)r;
        px[1] = (uint8_t)g;
        px[2] = (uint8_t)b;
        if (j->rgba) {
            px[3] = (uint8_t)j->c.max_out;
        }
    }
}

/* kernel::alpha for one row: the alpha frame's samples read as one flat run from row y. */
static void hx_alpha_row(const hx_job *j, uint8_t *dst, size_t y)
{
    size_t aw = j->a.w;
    size_t total = aw * j->a.h;
    size_t k = y * aw;
    size_t x;
    for (x = 0; x < j->w && k < total; x++, k++) {
        int32_t v = hx_px(&j->a, k / aw, k % aw);
        int32_t a;
        v = v < j->a_max_in ? v : j->a_max_in;
        a = (v * j->a_gain + (1 << 13)) >> 14;
        a = a < 0 ? 0 : a > j->c.max_out ? j->c.max_out : a;
        if (j->wide) {
            uint16_t s = (uint16_t)a;
            memcpy(dst + x * 8 + 6, &s, 2);
        } else {
            dst[x * 4 + 3] = (uint8_t)a;
        }
    }
}

static pc_status hx_band(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const hx_job *j = ctx;
    const hx_coeffs *c = &j->c;
    hx_ring g;
    size_t y;
    (void)chunk;
    memset(&g, 0, sizeof g);
    if (j->mode != HX_LUMA) {
        g.rows = malloc(6 * (size_t)j->w * sizeof *g.rows);
        if (g.rows == NULL) {
            return PC_ERR_NOMEM;
        }
    }
    for (y = begin; y < end; y++) {
        uint8_t *dst = j->out + y * j->w * j->bpp;
        size_t x;
        if (j->mode == HX_LUMA) {
            for (x = 0; x < j->w; x++) {
                int32_t v = hx_finish(c, hx_w(hx_px(&j->y, y, x) - c->y_off) * hx_w(c->ky));
                hx_store(j, dst + x * j->bpp, v, v, v);
            }
        } else {
            /* upsample::row_pair */
            size_t r0;
            size_t r1;
            uint32_t w0;
            const uint16_t *cb0;
            const uint16_t *cb1;
            const uint16_t *cr0;
            const uint16_t *cr1;
            if (j->ys == 0) {
                r0 = y < j->ch - 1 ? y : j->ch - 1;
                r1 = r0;
                w0 = 4;
            } else {
                r0 = (y >> 1) < j->ch - 1 ? y >> 1 : j->ch - 1;
                if ((y & 1) == 0) {
                    r1 = r0 > 0 ? r0 - 1 : 0;
                } else {
                    r1 = r0 + 1 < j->ch - 1 ? r0 + 1 : j->ch - 1;
                }
                w0 = 3;
            }
            {
                size_t s0 = hx_slot(&g, j, r0);
                size_t s1 = hx_slot(&g, j, r1);
                cb0 = g.rows + s0 * 2 * j->w;
                cr0 = cb0 + j->w;
                cb1 = g.rows + s1 * 2 * j->w;
                cr1 = cb1 + j->w;
            }
            for (x = 0; x < j->w; x++) {
                int32_t u = hx_vblend(cb0[x], cb1[x], w0);
                int32_t v = hx_vblend(cr0[x], cr1[x], w0);
                uint32_t yy = hx_w(hx_px(&j->y, y, x) - c->y_off) * hx_w(c->ky);
                if (j->mode == HX_IDENTITY) {
                    int32_t gg = hx_finish(c, yy);
                    int32_t bb = hx_finish(c, hx_w(u - c->y_off) * hx_w(c->ky));
                    int32_t rr = hx_finish(c, hx_w(v - c->y_off) * hx_w(c->ky));
                    hx_store(j, dst + x * j->bpp, rr, gg, bb);
                } else {
                    uint32_t uu = hx_w(u - c->c_off);
                    uint32_t vv = hx_w(v - c->c_off);
                    int32_t rr = hx_finish(c, yy + hx_w(c->rv) * vv);
                    int32_t gg = hx_finish(c, yy + hx_w(c->gu) * uu + hx_w(c->gv) * vv);
                    int32_t bb = hx_finish(c, yy + hx_w(c->bu) * uu);
                    hx_store(j, dst + x * j->bpp, rr, gg, bb);
                }
            }
        }
        if (j->has_a) {
            hx_alpha_row(j, dst, y);
        }
    }
    free(g.rows);
    return PC_OK;
}

/* A plane of im, checked to hold at least w x h samples; false when it doesn't. */
static bool hx_get_plane(const struct heif_image *im, enum heif_channel ch, uint32_t w, uint32_t h,
                         hx_plane *pl)
{
    int stride = 0;
    int bits = heif_image_get_bits_per_pixel(im, ch);
    int pw = heif_image_get_width(im, ch);
    int ph = heif_image_get_height(im, ch);
    pl->p = heif_image_get_plane_readonly(im, ch, &stride);
    if (pl->p == NULL || stride <= 0 || bits < 1 || bits > 16 || pw <= 0 || ph <= 0) {
        return false;
    }
    pl->bytes = bits > 8 ? 2u : 1u;
    pl->stride = (size_t)stride;
    pl->w = (uint32_t)pw;
    pl->h = (uint32_t)ph;
    return pl->w >= w && pl->h >= h && pl->stride / pl->bytes >= pl->w;
}

/* Converts a natively decoded YCbCr / monochrome image. Sets *other (and returns PC_OK with img
 * untouched) when im is in another colour model, so the caller can ask libheif for RGB. */
static pc_status hx_convert(const struct heif_image *im, const hx_props *pr, bool alpha, bool sixteen,
                            const pc_climits *l, pc_cimage *img, bool *other, pc_error *err)
{
    enum heif_colorspace cs = heif_image_get_colorspace(im);
    enum heif_chroma chroma = heif_image_get_chroma_format(im);
    pc_clayout layout = alpha ? PC_CL_RGBA : PC_CL_RGB;
    pc_csample sample = sixteen ? PC_CS_U16 : PC_CS_U8;
    hx_job j;
    int depth;
    int w;
    int h;
    pc_status st;
    memset(&j, 0, sizeof j);
    j.mode = pr->matrix == 0 ? HX_IDENTITY : HX_MATRIX;
    if (cs == heif_colorspace_monochrome) {
        j.mode = HX_LUMA;
    } else if (cs == heif_colorspace_YCbCr && chroma == heif_chroma_420) {
        j.xs = 1;
        j.ys = 1;
    } else if (cs == heif_colorspace_YCbCr && chroma == heif_chroma_422) {
        j.xs = 1;
    } else if (cs != heif_colorspace_YCbCr || chroma != heif_chroma_444) {
        *other = true;
        return PC_OK;
    }
    w = heif_image_get_width(im, heif_channel_Y);
    h = heif_image_get_height(im, heif_channel_Y);
    if (w <= 0 || h <= 0) {
        return cx_malformed(err, F, "the decoder returned no image");
    }
    j.w = (uint32_t)w;
    j.h = (uint32_t)h;
    if (!hx_get_plane(im, heif_channel_Y, j.w, j.h, &j.y)) {
        return cx_malformed(err, F, "malformed file: luma plane is shorter than its geometry");
    }
    depth = heif_image_get_bits_per_pixel_range(im, heif_channel_Y);
    if (depth < 8 || depth > 16) {
        return cx_unsupported(err, F, "sample bit depth is outside 8 to 16");
    }
    if (j.mode != HX_LUMA) {
        j.cw = (j.w + (1u << j.xs) - 1) >> j.xs;
        j.ch = (j.h + (1u << j.ys) - 1) >> j.ys;
        if (!hx_get_plane(im, heif_channel_Cb, j.cw, j.ch, &j.cb) ||
            !hx_get_plane(im, heif_channel_Cr, j.cw, j.ch, &j.cr)) {
            return cx_malformed(err, F, "malformed file: chroma plane is shorter than its geometry");
        }
    }
    j.c = hx_coeffs_new(pr->full, pr->matrix, (unsigned)depth, sixteen);
    if (alpha && heif_image_has_channel(im, heif_channel_Alpha)) {
        int ad = heif_image_get_bits_per_pixel_range(im, heif_channel_Alpha);
        if (!hx_get_plane(im, heif_channel_Alpha, 1, 1, &j.a)) {
            return cx_malformed(err, F, "the decoder returned no alpha plane");
        }
        if (ad < 8 || ad > 16) {
            return cx_unsupported(err, F, "sample bit depth is outside 8 to 16");
        }
        j.has_a = true;
        j.a_max_in = (int32_t)((1u << ad) - 1);
        j.a_gain = hx_q((float)j.c.max_out * (float)(1u << 14) / (float)j.a_max_in);
    }
    if ((st = pc_climits_check(l, j.w, j.h, layout, sample, err)) != PC_OK ||
        (st = pc_cimage_new(j.w, j.h, layout, sample, img, err)) != PC_OK) {
        return st;
    }
    j.out = img->data;
    j.wide = sixteen;
    j.rgba = alpha;
    j.bpp = pc_clayout_channels(layout) * pc_csample_bytes(sample);
    st = pc_pool_for((uint64_t)j.w * j.h >= 768u * 768u ? pc_pool_global() : NULL, j.h, 16, hx_band, &j);
    if (st != PC_OK) {
        pc_cimage_fini(img);
        return cx_nomem(err);
    }
    return PC_OK;
}

/* libheif's own RGB conversion, for a decoded image in another colour model. */
static pc_status hx_rgb(struct heif_image_handle *h, struct heif_decoding_options *opts, bool alpha,
                        bool sixteen, const pc_climits *l, pc_cimage *img, pc_error *err)
{
    struct heif_image *im = NULL;
    struct heif_error e;
    enum heif_chroma chroma;
    pc_clayout layout = alpha ? PC_CL_RGBA : PC_CL_RGB;
    pc_csample sample = sixteen ? PC_CS_U16 : PC_CS_U8;
    bool le;
    const uint16_t one = 1;
    uint8_t first;
    int stride = 0;
    const uint8_t *plane;
    uint32_t iw;
    uint32_t ih;
    int dbits;
    size_t row;
    size_t y;
    pc_status st;
    memcpy(&first, &one, 1);
    le = first == 1;
    if (sixteen) {
        chroma = alpha ? (le ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBBAA_BE)
                       : (le ? heif_chroma_interleaved_RRGGBB_LE : heif_chroma_interleaved_RRGGBB_BE);
    } else {
        chroma = alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB;
    }
    e = heif_decode_image(h, &im, heif_colorspace_RGB, chroma, opts);
    if (e.code != heif_error_Ok) {
        return map_err(e, err);
    }
    plane = heif_image_get_plane_readonly(im, heif_channel_interleaved, &stride);
    iw = (uint32_t)heif_image_get_width(im, heif_channel_interleaved);
    ih = (uint32_t)heif_image_get_height(im, heif_channel_interleaved);
    dbits = heif_image_get_bits_per_pixel_range(im, heif_channel_interleaved);
    if (plane == NULL || stride <= 0) {
        heif_image_release(im);
        return cx_malformed(err, F, "the decoder returned no image");
    }
    if ((st = pc_climits_check(l, iw, ih, layout, sample, err)) != PC_OK ||
        (st = pc_cimage_new(iw, ih, layout, sample, img, err)) != PC_OK) {
        heif_image_release(im);
        return st;
    }
    row = (size_t)iw * pc_clayout_channels(layout) * pc_csample_bytes(sample);
    if ((size_t)stride < row) {
        pc_cimage_fini(img);
        heif_image_release(im);
        return cx_malformed(err, F, "short image rows");
    }
    for (y = 0; y < ih; y++) {
        memcpy(img->data + y * row, plane + y * (size_t)stride, row);
    }
    if (sixteen && dbits > 0 && dbits < 16) {
        /* widen e.g. 10-bit samples to the 16-bit range */
        uint32_t max = (1u << dbits) - 1;
        size_t i;
        for (i = 0; i < img->len / 2; i++) {
            uint16_t v;
            memcpy(&v, img->data + 2 * i, 2);
            v = (uint16_t)(((uint32_t)(v > max ? max : v) * 65535u + max / 2) / max);
            memcpy(img->data + 2 * i, &v, 2);
        }
    }
    heif_image_release(im);
    return PC_OK;
}

pc_status cx_heif_decode(const uint8_t *b, size_t n, const pc_climits *l, bool keep_orientation,
                         pc_cimage *out, pc_error *err)
{
    struct heif_context *ctx;
    struct heif_image_handle *h = NULL;
    struct heif_image *im = NULL;
    struct heif_decoding_options *opts = NULL;
    struct heif_error e;
    struct heif_color_profile_nclx *nclx = NULL;
    hx_props pr;
    uint32_t item = 0;
    bool have_item = false;
    bool other = false;
    bool alpha;
    bool sixteen;
    uint32_t w;
    uint32_t hh;
    pc_cimage img;
    pc_status st;
    if (!cx_heif_available()) {
        return cx_unsupported(err, F, "this build's libheif has no HEVC decoder");
    }
    if (n >= 8 && has_box(b, n, "ftyp") && !has_box(b, n, "meta")) {
        return cx_unsupported(err, F, SEQUENCE);
    }
    memset(&pr, 0, sizeof pr);
    pc_cimage_init(&img);
    /* The reference reads the properties before decoding anything, so a broken clap is reported
     * in its words, not libheif's. */
    if ((st = hx_read_props(b, n, &item, &have_item, &pr, err)) != PC_OK ||
        (st = hx_check_props(&pr, err)) != PC_OK) {
        hx_props_reset(&pr);
        return st;
    }
    ctx = heif_context_alloc();
    if (ctx == NULL) {
        hx_props_reset(&pr);
        return cx_nomem(err);
    }
    e = heif_context_read_from_memory_without_copy(ctx, b, n, NULL);
    if (e.code == heif_error_Ok) {
        e = heif_context_get_primary_image_handle(ctx, &h);
    }
    if (e.code != heif_error_Ok) {
        st = map_err(e, err);
        goto done;
    }
    if (!have_item || item != heif_image_handle_get_item_id(h)) {
        item = heif_image_handle_get_item_id(h);
        have_item = true;
        if ((st = hx_read_props(b, n, &item, &have_item, &pr, err)) != PC_OK ||
            (st = hx_check_props(&pr, err)) != PC_OK) {
            goto done;
        }
    }
    alpha = heif_image_handle_has_alpha_channel(h) != 0;
    sixteen = heif_image_handle_get_luma_bits_per_pixel(h) > 8;
    w = (uint32_t)heif_image_handle_get_ispe_width(h);
    hh = (uint32_t)heif_image_handle_get_ispe_height(h);
    {
        /* The transformed size is checked (and an impossible clap refused) even when the
         * pixels come back as coded, as in the reference. */
        uint32_t tw = w;
        uint32_t th = hh;
        if ((st = hx_transformed_size(&tw, &th, &pr, err)) != PC_OK) {
            goto done;
        }
        if (!keep_orientation) {
            w = tw;
            hh = th;
        }
    }
    /* The declared size is checked before any pixel is decoded. */
    if ((st = pc_climits_check(l, w, hh, alpha ? PC_CL_RGBA : PC_CL_RGB, sixteen ? PC_CS_U16 : PC_CS_U8,
                               err)) != PC_OK) {
        goto done;
    }
    opts = heif_decoding_options_alloc();
    if (opts == NULL) {
        st = cx_nomem(err);
        goto done;
    }
    /* The transforms are applied below, to the converted pixels. A truncated or damaged file is
     * an error, as in the reference (libheif would otherwise fill missing grid tiles and only
     * warn). */
    opts->ignore_transformations = 1;
    opts->strict_decoding = 1;
    /* The planes as coded: libheif 1.21+ would otherwise re-matrix YCbCr whose nclx isn't sRGB's
     * (the field exists when the library's options struct is version 10, libheif 1.23). With
     * 1.21 / 1.22, asking for the item's own nclx has the same effect when the file has one. */
#if defined(LIBHEIF_HAVE_VERSION) && LIBHEIF_HAVE_VERSION(1, 23, 0)
    if (opts->version >= 10) {
        opts->output_image_nclx_profile_passthrough = 1;
    }
#elif defined(LIBHEIF_HAVE_VERSION) && LIBHEIF_HAVE_VERSION(1, 21, 0)
    if (opts->version >= 8 && heif_image_handle_get_nclx_color_profile(h, &nclx).code == heif_error_Ok) {
        opts->output_image_nclx_profile = nclx;
    }
#endif
    e = heif_decode_image(h, &im, heif_colorspace_undefined, heif_chroma_undefined, opts);
    if (e.code != heif_error_Ok) {
        st = map_err(e, err);
        goto done;
    }
    st = hx_convert(im, &pr, alpha, sixteen, l, &img, &other, err);
    heif_image_release(im);
    if (st == PC_OK && other) {
        st = hx_rgb(h, opts, alpha, sixteen, l, &img, err);
    }
    if (st == PC_OK && !keep_orientation) {
        uint32_t tw = img.width;
        uint32_t th = img.height;
        /* the final size is checked before the transforms allocate it */
        if ((st = hx_transformed_size(&tw, &th, &pr, err)) == PC_OK &&
            (st = pc_climits_check(l, tw, th, img.layout, img.sample, err)) == PC_OK) {
            st = hx_apply(&img, &pr, err);
        }
    }
    if (st == PC_OK) {
        st = read_metadata(h, &img, keep_orientation, err);
    }
done:
    heif_decoding_options_free(opts);
    if (nclx != NULL) {
        heif_nclx_color_profile_free(nclx);
    }
    if (h != NULL) {
        heif_image_handle_release(h);
    }
    heif_context_free(ctx);
    hx_props_reset(&pr);
    if (st != PC_OK) {
        pc_cimage_fini(&img);
        return st;
    }
    *out = img;
    return PC_OK;
}

#else

bool cx_heif_available(void)
{
    return false;
}

pc_status cx_heif_decode(const uint8_t *b, size_t n, const pc_climits *l, bool keep_orientation,
                         pc_cimage *out, pc_error *err)
{
    (void)b;
    (void)n;
    (void)l;
    (void)keep_orientation;
    (void)out;
    return cx_unsupported(err, F, "HEIC/HEIF support isn't included in this build of PhotoCraft");
}

#endif
