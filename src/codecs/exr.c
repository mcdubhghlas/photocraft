/* OpenEXR (port of crates/codecs/src/codecs/exr.rs; REWRITE_PLAN.md D27): flat parts through
 * OpenEXRCore (the C API): F16/F32 (32-bit unsigned read as f32), Y/YA/RGB/RGBA channels
 * (an optional "layer." prefix is ignored), lossless compression, the first flat part at full
 * resolution, pixels taken from the data window. Declared sizes are checked before anything is
 * decoded, and a file with deep data is composited by deep_exr.c. Built when OpenEXR is found
 * (WITH_EXR); without it the format is neither read nor written. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/half.h"
#include "base/mem.h"
#include "base/num.h"
#include "codecs/deep_exr.h"
#include "codecs/internal.h"

#define F PC_CF_EXR

#ifndef PC_NO_EXR

#include <openexr.h>

bool cx_exr_available(void) { return true; }

typedef struct stream {
    const uint8_t *b;
    size_t n;
    pc_buf *out; /* writing */
    char msg[160];
} stream;

static void on_error(exr_const_context_t ctxt, exr_result_t code, const char *msg)
{
    void *ud = NULL;
    (void)code;
    if (exr_get_user_data(ctxt, &ud) == EXR_ERR_SUCCESS && ud != NULL) {
        stream *s = ud;
        if (s->msg[0] == '\0') {
            (void)snprintf(s->msg, sizeof s->msg, "%s", msg);
        }
    }
}

static int64_t read_fn(exr_const_context_t ctxt, void *ud, void *buf, uint64_t sz, uint64_t off,
                       exr_stream_error_func_ptr_t error_cb)
{
    stream *s = ud;
    uint64_t k;
    (void)ctxt;
    (void)error_cb;
    if (off >= s->n) {
        return 0;
    }
    k = s->n - off < sz ? s->n - off : sz;
    memcpy(buf, s->b + off, (size_t)k);
    return (int64_t)k;
}

static int64_t size_fn(exr_const_context_t ctxt, void *ud)
{
    stream *s = ud;
    (void)ctxt;
    return (int64_t)s->n;
}

static int64_t write_fn(exr_const_context_t ctxt, void *ud, const void *buf, uint64_t sz, uint64_t off,
                        exr_stream_error_func_ptr_t error_cb)
{
    stream *s = ud;
    pc_buf *o = s->out;
    (void)ctxt;
    (void)error_cb;
    if (off > SIZE_MAX - sz) {
        return -1;
    }
    if (o->len < off + sz) {
        size_t old = o->len;
        if (pc_buf_reserve(o, (size_t)(off + sz) - old, NULL) != PC_OK) {
            return -1;
        }
        memset(o->data + old, 0, (size_t)(off + sz) - old);
        o->len = (size_t)(off + sz);
    }
    memcpy(o->data + off, buf, (size_t)sz);
    return (int64_t)sz;
}

static const char *base_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL ? dot + 1 : name;
}

static pc_status exr_fail(pc_error *err, const stream *s, exr_result_t r)
{
    return cx_malformed(err, F, "%s", s->msg[0] != '\0' ? s->msg : exr_get_default_error_message(r));
}

static pc_status open_read(const uint8_t *b, size_t n, stream *s, exr_context_t *ctx, pc_error *err)
{
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    exr_result_t r;
    memset(s, 0, sizeof *s);
    s->b = b;
    s->n = n;
    init.user_data = s;
    init.read_fn = read_fn;
    init.size_fn = size_fn;
    init.error_handler_fn = on_error;
    init.flags = EXR_CONTEXT_FLAG_SILENT_HEADER_PARSE | EXR_CONTEXT_FLAG_DISABLE_CHUNK_RECONSTRUCTION;
    *ctx = NULL;
    r = exr_start_read(ctx, "memory", &init);
    if (r != EXR_ERR_SUCCESS) {
        pc_status st = exr_fail(err, s, r);
        exr_finish(ctx);
        return st;
    }
    return PC_OK;
}

/* Where one channel's samples go: element (x, y) of the part's data window lands at
 * base + y * line + x * pixel. */
typedef struct target {
    int ch; /* index into the part's channel list */
    uint8_t *base;
    size_t pixel;
    size_t line;
} target;

/* Decodes the given channels of one flat part at full resolution into the targets, as half
 * (type EXR_PIXEL_HALF) or float samples. */
static pc_status run_decode(exr_context_t ctx, const stream *s, int part, const target *t, size_t nt,
                            uint16_t type, pc_error *err)
{
    exr_storage_t storage;
    const exr_attr_chlist_t *chl = NULL;
    exr_attr_box2i_t dw;
    exr_decode_pipeline_t dec;
    bool dec_init = false;
    int32_t tx = 0;
    int32_t ty = 0;
    int32_t ntx = 1;
    int32_t nty = 1;
    int32_t spc = 1;
    int32_t y;
    int32_t tw = 0;
    int32_t th = 0;
    int16_t bps = type == EXR_PIXEL_HALF ? 2 : 4;
    exr_result_t r;
    pc_status st = PC_OK;
    if (exr_get_storage(ctx, part, &storage) != EXR_ERR_SUCCESS ||
        exr_get_channels(ctx, part, &chl) != EXR_ERR_SUCCESS ||
        exr_get_data_window(ctx, part, &dw) != EXR_ERR_SUCCESS) {
        return exr_fail(err, s, EXR_ERR_INVALID_ATTR);
    }
    if (storage != EXR_STORAGE_SCANLINE && storage != EXR_STORAGE_TILED) {
        return cx_unsupported(err, F, "not supported: deep data in a flat part");
    }
    y = dw.min.y;
    if (storage == EXR_STORAGE_TILED) {
        if (exr_get_tile_counts(ctx, part, 0, 0, &ntx, &nty) != EXR_ERR_SUCCESS ||
            exr_get_tile_sizes(ctx, part, 0, 0, &tw, &th) != EXR_ERR_SUCCESS) {
            return exr_fail(err, s, EXR_ERR_INVALID_ATTR);
        }
    } else if (exr_get_scanlines_per_chunk(ctx, part, &spc) != EXR_ERR_SUCCESS || spc < 1) {
        return exr_fail(err, s, EXR_ERR_INVALID_ATTR);
    }
    for (;;) {
        exr_chunk_info_t ci;
        int c;
        if (storage == EXR_STORAGE_TILED) {
            if (ty >= nty) {
                break;
            }
            r = exr_read_tile_chunk_info(ctx, part, tx, ty, 0, 0, &ci);
        } else {
            if ((int64_t)y > dw.max.y) {
                break;
            }
            r = exr_read_scanline_chunk_info(ctx, part, y, &ci);
        }
        if (r != EXR_ERR_SUCCESS) {
            st = exr_fail(err, s, r);
            break;
        }
        r = dec_init ? exr_decoding_update(ctx, part, &ci, &dec)
                     : exr_decoding_initialize(ctx, part, &ci, &dec);
        if (r != EXR_ERR_SUCCESS) {
            st = exr_fail(err, s, r);
            break;
        }
        dec_init = true;
        for (c = 0; c < dec.channel_count; c++) {
            exr_coding_channel_info_t *ch = &dec.channels[c];
            size_t k;
            ch->decode_to_ptr = NULL;
            for (k = 0; k < nt; k++) {
                if (strcmp(ch->channel_name, chl->entries[t[k].ch].name.str) == 0) {
                    size_t x0 = (size_t)((int64_t)ci.start_x - dw.min.x);
                    size_t y0 = (size_t)((int64_t)ci.start_y - dw.min.y);
                    ch->decode_to_ptr = t[k].base + y0 * t[k].line + x0 * t[k].pixel;
                    ch->user_pixel_stride = (int32_t)t[k].pixel;
                    ch->user_line_stride = (int32_t)t[k].line;
                    ch->user_data_type = type;
                    ch->user_bytes_per_element = bps;
                    break;
                }
            }
        }
        r = exr_decoding_choose_default_routines(ctx, part, &dec);
        if (r == EXR_ERR_SUCCESS) {
            r = exr_decoding_run(ctx, part, &dec);
        }
        if (r != EXR_ERR_SUCCESS) {
            st = exr_fail(err, s, r);
            break;
        }
        if (storage == EXR_STORAGE_TILED) {
            if (++tx >= ntx) {
                tx = 0;
                ty++;
            }
        } else {
            y += spc;
        }
    }
    if (dec_init) {
        exr_decoding_destroy(ctx, &dec);
    }
    return st;
}

static pc_status part_window(exr_context_t ctx, const stream *s, int part, uint32_t *w, uint32_t *h,
                             pc_error *err)
{
    exr_attr_box2i_t dw;
    int64_t ww;
    int64_t hh;
    if (exr_get_data_window(ctx, part, &dw) != EXR_ERR_SUCCESS) {
        return exr_fail(err, s, EXR_ERR_INVALID_ATTR);
    }
    ww = (int64_t)dw.max.x - dw.min.x + 1;
    hh = (int64_t)dw.max.y - dw.min.y + 1;
    if (ww < 0 || hh < 0 || ww > UINT32_MAX || hh > UINT32_MAX) {
        return cx_malformed(err, F, "invalid data window size");
    }
    *w = (uint32_t)ww;
    *h = (uint32_t)hh;
    return PC_OK;
}

/* One flat part as an image: R/G/B(/A) channels (a "layer." prefix ignored) as RGB(A), else Y
 * (/A), else a lone channel as gray. F16 when every used channel is half, else F32. */
static pc_status decode_layer(const uint8_t *b, size_t n, int part, const pc_climits *l, pc_cimage *out,
                              pc_error *err)
{
    stream s;
    exr_context_t ctx;
    const exr_attr_chlist_t *chl = NULL;
    int idx[4] = {-1, -1, -1, -1};
    size_t nc = 0;
    pc_clayout layout;
    bool all_half = true;
    pc_csample sample;
    uint32_t w;
    uint32_t h;
    pc_cimage img;
    target t[4];
    size_t k;
    pc_status st = open_read(b, n, &s, &ctx, err);
    if (st != PC_OK) {
        return st;
    }
    if (exr_get_channels(ctx, part, &chl) != EXR_ERR_SUCCESS) {
        st = exr_fail(err, &s, EXR_ERR_INVALID_ATTR);
        exr_finish(&ctx);
        return st;
    }
    {
        static const char *const RGBA[4] = {"R", "G", "B", "A"};
        int c;
        int q;
        for (q = 0; q < 4; q++) {
            for (c = 0; c < chl->num_channels; c++) {
                if (strcmp(base_name(chl->entries[c].name.str), RGBA[q]) == 0) {
                    idx[q] = c;
                    break;
                }
            }
        }
        if (idx[0] >= 0 && idx[1] >= 0 && idx[2] >= 0) {
            layout = idx[3] >= 0 ? PC_CL_RGBA : PC_CL_RGB;
            nc = idx[3] >= 0 ? 4 : 3;
        } else {
            int yy = -1;
            for (c = 0; c < chl->num_channels; c++) {
                if (strcmp(base_name(chl->entries[c].name.str), "Y") == 0) {
                    yy = c;
                    break;
                }
            }
            if (yy < 0 && chl->num_channels == 1) {
                yy = 0;
            }
            if (yy < 0) {
                exr_finish(&ctx);
                return cx_unsupported(err, F, "no R/G/B or Y channels");
            }
            idx[0] = yy;
            idx[1] = idx[3];
            layout = idx[1] >= 0 ? PC_CL_GRAYA : PC_CL_GRAY;
            nc = idx[1] >= 0 ? 2 : 1;
        }
        for (q = 0; q < (int)nc; q++) {
            const exr_attr_chlist_entry_t *e = &chl->entries[idx[q]];
            if (e->pixel_type != EXR_PIXEL_HALF) {
                all_half = false;
            }
            if (e->x_sampling != 1 || e->y_sampling != 1) {
                exr_finish(&ctx);
                return cx_malformed(err, F, "channel sample count mismatch (subsampled channels are unsupported)");
            }
        }
    }
    sample = all_half ? PC_CS_F16 : PC_CS_F32;
    if ((st = part_window(ctx, &s, part, &w, &h, err)) != PC_OK ||
        (st = pc_climits_check(l, w, h, layout, sample, err)) != PC_OK ||
        (st = pc_cimage_new(w, h, layout, sample, &img, err)) != PC_OK) {
        exr_finish(&ctx);
        return st;
    }
    for (k = 0; k < nc; k++) {
        size_t bps = pc_csample_bytes(sample);
        t[k].ch = idx[k];
        t[k].base = img.data + k * bps;
        t[k].pixel = nc * bps;
        t[k].line = (size_t)w * nc * bps;
    }
    st = run_decode(ctx, &s, part, t, nc, (uint16_t)(all_half ? EXR_PIXEL_HALF : EXR_PIXEL_FLOAT), err);
    exr_finish(&ctx);
    if (st != PC_OK) {
        pc_cimage_fini(&img);
        return st;
    }
    *out = img;
    return PC_OK;
}

/* The headers (own parser) with every part's size, and all of them together, checked. */
static pc_status read_meta(const uint8_t *b, size_t n, const pc_climits *l, exr_meta *m, pc_error *err)
{
    return cx_exr_meta_checked(b, n, l, m, err);
}

static size_t pick_part(const exr_meta *m)
{
    size_t best = 0;
    uint8_t best_rank = 0;
    size_t i;
    for (i = 0; i < m->n; i++) {
        uint8_t r = cx_exr_hdr_rank(&m->parts[i]);
        if (i == 0 || r > best_rank) {
            best = i;
            best_rank = r;
        }
    }
    return best;
}

pc_status cx_exr_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    exr_meta m;
    size_t pick;
    size_t nparts;
    pc_cimage img;
    pc_status st = read_meta(b, n, l, &m, err);
    if (st != PC_OK) {
        return st;
    }
    /* Deep parts: walk their chunks ourselves and composite the samples. */
    if (m.has_deep) {
        pc_cdeep d;
        st = cx_exr_decode_deep(&m, b, n, l, &d, err);
        cx_exr_meta_fini(&m);
        if (st != PC_OK) {
            return st;
        }
        st = cx_exr_flatten(&d, out, err);
        pc_cdeep_fini(&d);
        return st;
    }
    pick = pick_part(&m);
    nparts = m.n;
    cx_exr_meta_fini(&m);
    if ((st = decode_layer(b, n, (int)pick, l, &img, err)) != PC_OK) {
        return st;
    }
    if (nparts > 1) {
        pc_cwarning w;
        memset(&w, 0, sizeof w);
        w.kind = PC_CW_MORE_PARTS;
        w.has_total = nparts <= UINT32_MAX;
        w.total = (uint32_t)nparts;
        if ((st = pc_cimage_add_warning(&img, w, err)) != PC_OK) {
            pc_cimage_fini(&img);
            return st;
        }
    }
    *out = img;
    return PC_OK;
}

/* The flat part `part` of a file without deep parts. */
static pc_status flat_part(const uint8_t *b, size_t n, size_t part, const pc_climits *l, pc_error *err)
{
    exr_meta m;
    size_t parts;
    bool deep;
    pc_status st = read_meta(b, n, l, &m, err);
    if (st != PC_OK) {
        return st;
    }
    deep = m.has_deep;
    parts = m.n;
    cx_exr_meta_fini(&m);
    if (deep) {
        return cx_unsupported(
            err, F,
            "this file has deep parts; the deep decoder composites those (mixing flat and deep parts "
            "is not supported)");
    }
    if (part >= parts) {
        return cx_malformed(err, F, "part index %zu is out of range (%zu parts)", part, parts);
    }
    return PC_OK;
}

pc_status cx_exr_decode_part(const uint8_t *b, size_t n, size_t part, const pc_climits *l, pc_cimage *out,
                             pc_error *err)
{
    pc_status st = flat_part(b, n, part, l, err);
    if (st != PC_OK) {
        return st;
    }
    return decode_layer(b, n, (int)part, l, out, err);
}

pc_status cx_exr_decode_channels(const uint8_t *b, size_t n, size_t part, const char *const *names,
                                 size_t nnames, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    stream s;
    exr_context_t ctx;
    const exr_attr_chlist_t *chl = NULL;
    int *picked = NULL;
    int order[4] = {-1, -1, -1, -1};
    size_t i;
    pc_clayout layout;
    bool all_half = true;
    pc_csample sample;
    uint32_t w;
    uint32_t h;
    pc_cimage img;
    target t[4];
    size_t nt = 0;
    size_t nc;
    size_t bps;
    pc_status st = flat_part(b, n, part, l, err);
    if (st != PC_OK) {
        return st;
    }
    if ((st = open_read(b, n, &s, &ctx, err)) != PC_OK) {
        return st;
    }
    if (exr_get_channels(ctx, (int)part, &chl) != EXR_ERR_SUCCESS) {
        st = exr_fail(err, &s, EXR_ERR_INVALID_ATTR);
        exr_finish(&ctx);
        return st;
    }
    picked = malloc((nnames > 0 ? nnames : 1) * sizeof *picked);
    if (picked == NULL) {
        exr_finish(&ctx);
        return cx_nomem(err);
    }
    for (i = 0; i < nnames; i++) {
        int c;
        picked[i] = -1;
        for (c = 0; c < chl->num_channels; c++) {
            if (cx_latin1_eq_utf8(chl->entries[c].name.str, names[i])) {
                picked[i] = c;
                break;
            }
        }
        if (picked[i] < 0) {
            st = cx_malformed(err, F, "the part has no channel %s", names[i]);
            goto done;
        }
        if (chl->entries[picked[i]].x_sampling != 1 || chl->entries[picked[i]].y_sampling != 1) {
            st = cx_malformed(err, F, "channel sample count mismatch (subsampled channels are unsupported)");
            goto done;
        }
    }
    {
#define AT(nm, out)                                                                                          \
    do {                                                                                                     \
        size_t q_;                                                                                           \
        (out) = -1;                                                                                          \
        for (q_ = 0; q_ < nnames; q_++) {                                                                    \
            if (strcmp(base_name(chl->entries[picked[q_]].name.str), (nm)) == 0) {                           \
                (out) = (int)q_;                                                                             \
                break;                                                                                       \
            }                                                                                                \
        }                                                                                                    \
    } while (0)
        int r;
        int g;
        int bb;
        int a;
        int yy;
        AT("R", r);
        AT("G", g);
        AT("B", bb);
        AT("A", a);
        AT("Y", yy);
#undef AT
        if (r >= 0 && g >= 0 && bb >= 0) {
            layout = a >= 0 ? PC_CL_RGBA : PC_CL_RGB;
            order[0] = r;
            order[1] = g;
            order[2] = bb;
            order[3] = a;
        } else if (yy >= 0) {
            layout = a >= 0 ? PC_CL_GRAYA : PC_CL_GRAY;
            order[0] = yy;
            order[1] = a;
        } else if (nnames == 1) {
            layout = PC_CL_GRAY;
            order[0] = 0;
        } else {
            st = cx_unsupported(err, F, "the named channels are no R/G/B or Y set");
            goto done;
        }
    }
    for (i = 0; i < nnames; i++) {
        if (chl->entries[picked[i]].pixel_type != EXR_PIXEL_HALF) {
            all_half = false;
        }
    }
    sample = all_half ? PC_CS_F16 : PC_CS_F32;
    nc = pc_clayout_channels(layout);
    bps = pc_csample_bytes(sample);
    if ((st = part_window(ctx, &s, (int)part, &w, &h, err)) != PC_OK ||
        (st = pc_climits_check(l, w, h, layout, sample, err)) != PC_OK ||
        (st = pc_cimage_new(w, h, layout, sample, &img, err)) != PC_OK) {
        goto done;
    }
    for (i = 0; i < nc; i++) {
        if (order[i] < 0) {
            continue;
        }
        t[nt].ch = picked[order[i]];
        t[nt].base = img.data + i * bps;
        t[nt].pixel = nc * bps;
        t[nt].line = (size_t)w * nc * bps;
        nt++;
    }
    st = run_decode(ctx, &s, (int)part, t, nt, (uint16_t)(all_half ? EXR_PIXEL_HALF : EXR_PIXEL_FLOAT), err);
    if (st != PC_OK) {
        pc_cimage_fini(&img);
        goto done;
    }
    *out = img;
done:
    free(picked);
    exr_finish(&ctx);
    return st;
}

pc_status cx_exr_read_part_f32(const uint8_t *b, size_t n, size_t part, uint32_t *w, uint32_t *h,
                               float ***chans, char ***names, size_t *nch, pc_error *err)
{
    char **nm = NULL;
    stream s;
    exr_context_t ctx;
    const exr_attr_chlist_t *chl = NULL;
    target *t = NULL;
    float **v = NULL;
    size_t k;
    size_t px;
    size_t cnt = 0;
    size_t nt = 0;
    pc_status st = open_read(b, n, &s, &ctx, err);
    *chans = NULL;
    *nch = 0;
    if (st != PC_OK) {
        return st;
    }
    if (exr_get_channels(ctx, (int)part, &chl) != EXR_ERR_SUCCESS || chl->num_channels < 0) {
        st = exr_fail(err, &s, EXR_ERR_INVALID_ATTR);
        exr_finish(&ctx);
        return st;
    }
    if ((st = part_window(ctx, &s, (int)part, w, h, err)) != PC_OK) {
        exr_finish(&ctx);
        return st;
    }
    cnt = (size_t)chl->num_channels;
    px = (size_t)*w * *h;
    t = calloc(cnt > 0 ? cnt : 1, sizeof *t);
    v = calloc(cnt > 0 ? cnt : 1, sizeof *v);
    nm = calloc(cnt > 0 ? cnt : 1, sizeof *nm);
    if (t == NULL || v == NULL || nm == NULL) {
        st = cx_nomem(err);
        goto fail;
    }
    for (k = 0; k < cnt; k++) {
        nm[k] =
            cx_latin1_to_utf8((const uint8_t *)chl->entries[k].name.str, strlen(chl->entries[k].name.str));
        if (nm[k] == NULL) {
            st = cx_nomem(err);
            goto fail;
        }
    }
    for (k = 0; k < cnt; k++) {
        if (chl->entries[k].x_sampling != 1 || chl->entries[k].y_sampling != 1) {
            continue; /* subsampled: left NULL */
        }
        v[k] = calloc(px > 0 ? px : 1, sizeof **v);
        if (v[k] == NULL) {
            st = cx_nomem(err);
            goto fail;
        }
        t[nt].ch = (int)k;
        t[nt].base = (uint8_t *)v[k];
        t[nt].pixel = 4;
        t[nt].line = (size_t)*w * 4;
        nt++;
    }
    st = run_decode(ctx, &s, (int)part, t, nt, EXR_PIXEL_FLOAT, err);
    if (st != PC_OK) {
        goto fail;
    }
    free(t);
    exr_finish(&ctx);
    *chans = v;
    *names = nm;
    *nch = cnt;
    return PC_OK;
fail:
    for (k = 0; k < cnt; k++) {
        free(v != NULL ? v[k] : NULL);
        free(nm != NULL ? nm[k] : NULL);
    }
    free(v);
    free(nm);
    free(t);
    exr_finish(&ctx);
    return st;
}

pc_status cx_exr_encode(const pc_cimage *src, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    static const char *const NAMES[4][4] = {{"Y"}, {"Y", "A"}, {"R", "G", "B"}, {"R", "G", "B", "A"}};
    const char *const *names;
    size_t nc;
    stream s;
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    exr_context_t ctx = NULL;
    exr_result_t r;
    int part = 0;
    exr_compression_t comp;
    exr_encode_pipeline_t enc;
    bool enc_init = false;
    pc_buf b = PC_BUF_INIT;
    size_t bps;
    size_t i;
    int32_t spc = 1;
    int32_t y;
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    if ((int)img->layout > PC_CL_RGBA) {
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "unsupported layout %s", pc_clayout_name(img->layout));
    }
    if (img->sample != PC_CS_F16 && img->sample != PC_CS_F32) {
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "unsupported sample %s", pc_csample_name(img->sample));
    }
    names = NAMES[img->layout];
    nc = pc_clayout_channels(img->layout);
    bps = pc_csample_bytes(img->sample);
    switch (o->exr_compression) {
    case PC_EXR_NONE:
        comp = EXR_COMPRESSION_NONE;
        break;
    case PC_EXR_RLE:
        comp = EXR_COMPRESSION_RLE;
        break;
    case PC_EXR_ZIP1:
        comp = EXR_COMPRESSION_ZIPS;
        break;
    case PC_EXR_PIZ:
        comp = EXR_COMPRESSION_PIZ;
        break;
    default:
        comp = EXR_COMPRESSION_ZIP;
    }
    memset(&s, 0, sizeof s);
    s.out = &b;
    init.user_data = &s;
    init.write_fn = write_fn;
    init.error_handler_fn = on_error;
    r = exr_start_write(&ctx, "memory", EXR_WRITE_FILE_DIRECTLY, &init);
    if (r == EXR_ERR_SUCCESS) {
        r = exr_add_part(ctx, "", EXR_STORAGE_SCANLINE, &part);
    }
    if (r == EXR_ERR_SUCCESS) {
        r = exr_initialize_required_attr_simple(ctx, part, (int32_t)img->width, (int32_t)img->height, comp);
    }
    for (i = 0; r == EXR_ERR_SUCCESS && i < nc; i++) {
        r = exr_add_channel(ctx, part, names[i], img->sample == PC_CS_F16 ? EXR_PIXEL_HALF : EXR_PIXEL_FLOAT,
                            EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1);
    }
    if (r == EXR_ERR_SUCCESS) {
        r = exr_set_lineorder(ctx, part, EXR_LINEORDER_INCREASING_Y);
    }
    if (r == EXR_ERR_SUCCESS) {
        r = exr_write_header(ctx);
    }
    if (r == EXR_ERR_SUCCESS) {
        r = exr_get_scanlines_per_chunk(ctx, part, &spc);
    }
    for (y = 0; r == EXR_ERR_SUCCESS && y < (int32_t)img->height; y += spc) {
        exr_chunk_info_t ci;
        int c;
        r = exr_write_scanline_chunk_info(ctx, part, y, &ci);
        if (r != EXR_ERR_SUCCESS) {
            break;
        }
        r = enc_init ? exr_encoding_update(ctx, part, &ci, &enc) : exr_encoding_initialize(ctx, part, &ci, &enc);
        if (r != EXR_ERR_SUCCESS) {
            break;
        }
        enc_init = true;
        for (c = 0; c < enc.channel_count; c++) {
            exr_coding_channel_info_t *ch = &enc.channels[c];
            size_t k;
            for (k = 0; k < nc; k++) {
                if (strcmp(ch->channel_name, names[k]) == 0) {
                    ch->encode_from_ptr = img->data + ((size_t)ci.start_y * img->width) * nc * bps + k * bps;
                    ch->user_pixel_stride = (int32_t)(nc * bps);
                    ch->user_line_stride = (int32_t)((size_t)img->width * nc * bps);
                    ch->user_data_type = (uint16_t)(img->sample == PC_CS_F16 ? EXR_PIXEL_HALF : EXR_PIXEL_FLOAT);
                    ch->user_bytes_per_element = (int16_t)bps;
                }
            }
        }
        r = exr_encoding_choose_default_routines(ctx, part, &enc);
        if (r == EXR_ERR_SUCCESS) {
            r = exr_encoding_run(ctx, part, &enc);
        }
    }
    if (enc_init) {
        exr_encoding_destroy(ctx, &enc);
    }
    if (r == EXR_ERR_SUCCESS) {
        r = exr_finish(&ctx);
    } else {
        exr_finish(&ctx);
    }
    if (r != EXR_ERR_SUCCESS) {
        st = cx_encode(err, F, "%s", s.msg[0] != '\0' ? s.msg : exr_get_default_error_message(r));
        pc_buf_fini(&b);
    } else {
        pc_buf_fini(out);
        *out = b;
    }
    pc_cimage_fini(&tmp);
    return st;
}

#else

bool cx_exr_available(void) { return false; }

pc_status cx_exr_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    (void)b;
    (void)n;
    (void)l;
    (void)out;
    return cx_unsupported(err, F, "decoding is not available for this format");
}

pc_status cx_exr_encode(const pc_cimage *src, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err)
{
    (void)src;
    (void)p;
    (void)o;
    (void)out;
    return cx_unsupported(err, F, "encoding is not available in this build");
}

pc_status cx_exr_decode_part(const uint8_t *b, size_t n, size_t part, const pc_climits *l, pc_cimage *out,
                             pc_error *err)
{
    (void)b;
    (void)n;
    (void)part;
    (void)l;
    (void)out;
    return cx_unsupported(err, F, "decoding is not available for this format");
}

pc_status cx_exr_decode_channels(const uint8_t *b, size_t n, size_t part, const char *const *names,
                                 size_t nnames, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    (void)b;
    (void)n;
    (void)part;
    (void)names;
    (void)nnames;
    (void)l;
    (void)out;
    return cx_unsupported(err, F, "decoding is not available for this format");
}

pc_status cx_exr_read_part_f32(const uint8_t *b, size_t n, size_t part, uint32_t *w, uint32_t *h,
                               float ***chans, char ***names, size_t *nch, pc_error *err)
{
    *names = NULL;
    (void)b;
    (void)n;
    (void)part;
    (void)w;
    (void)h;
    *chans = NULL;
    *nch = 0;
    return cx_unsupported(err, F, "decoding is not available for this format");
}

#endif
