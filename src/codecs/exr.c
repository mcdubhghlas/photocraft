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

pc_status cx_exr_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    exr_meta m;
    size_t i;
    stream s;
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    exr_context_t ctx = NULL;
    exr_result_t r;
    int part = -1;
    int nparts = 0;
    exr_storage_t storage = EXR_STORAGE_SCANLINE;
    const exr_attr_chlist_t *chl = NULL;
    exr_attr_box2i_t dw;
    int idx[4] = {-1, -1, -1, -1};
    size_t nc = 0;
    pc_clayout layout;
    bool all_half = true;
    pc_csample sample;
    uint64_t w;
    uint64_t h;
    pc_cimage img;
    exr_decode_pipeline_t dec;
    bool dec_init = false;
    pc_status st;
    if ((st = cx_exr_meta_parse(b, n, &m, err)) != PC_OK) {
        return st;
    }
    /* Check declared sizes before any pixel allocation. */
    for (i = 0; i < m.n; i++) {
        uint32_t ww = m.parts[i].w > UINT32_MAX ? UINT32_MAX : (uint32_t)m.parts[i].w;
        uint32_t hh = m.parts[i].h > UINT32_MAX ? UINT32_MAX : (uint32_t)m.parts[i].h;
        if ((st = cx_check_bytes(l, ww, hh, (uint64_t)(m.parts[i].nch > 0 ? m.parts[i].nch : 1) * 4, err)) != PC_OK) {
            cx_exr_meta_fini(&m);
            return st;
        }
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
    cx_exr_meta_fini(&m);
    memset(&s, 0, sizeof s);
    s.b = b;
    s.n = n;
    init.user_data = &s;
    init.read_fn = read_fn;
    init.size_fn = size_fn;
    init.error_handler_fn = on_error;
    init.flags = EXR_CONTEXT_FLAG_SILENT_HEADER_PARSE | EXR_CONTEXT_FLAG_DISABLE_CHUNK_RECONSTRUCTION;
    r = exr_start_read(&ctx, "memory", &init);
    if (r != EXR_ERR_SUCCESS) {
        st = exr_fail(err, &s, r);
        exr_finish(&ctx);
        return st;
    }
    if (exr_get_count(ctx, &nparts) != EXR_ERR_SUCCESS) {
        nparts = 0;
    }
    for (i = 0; i < (size_t)nparts; i++) {
        if (exr_get_storage(ctx, (int)i, &storage) == EXR_ERR_SUCCESS &&
            (storage == EXR_STORAGE_SCANLINE || storage == EXR_STORAGE_TILED)) {
            part = (int)i;
            break;
        }
    }
    if (part < 0 || exr_get_channels(ctx, part, &chl) != EXR_ERR_SUCCESS ||
        exr_get_data_window(ctx, part, &dw) != EXR_ERR_SUCCESS) {
        st = exr_fail(err, &s, EXR_ERR_INVALID_ATTR);
        exr_finish(&ctx);
        return st;
    }
    {
        static const char *const RGBA[4] = {"R", "G", "B", "A"};
        int c;
        int k;
        for (k = 0; k < 4; k++) {
            for (c = 0; c < chl->num_channels; c++) {
                if (strcmp(base_name(chl->entries[c].name.str), RGBA[k]) == 0) {
                    idx[k] = c;
                    break;
                }
            }
        }
        if (idx[0] >= 0 && idx[1] >= 0 && idx[2] >= 0) {
            layout = idx[3] >= 0 ? PC_CL_RGBA : PC_CL_RGB;
            nc = idx[3] >= 0 ? 4 : 3;
            if (nc == 3) {
                idx[3] = -1;
            }
        } else {
            int y = -1;
            for (c = 0; c < chl->num_channels; c++) {
                if (strcmp(base_name(chl->entries[c].name.str), "Y") == 0) {
                    y = c;
                    break;
                }
            }
            if (y < 0 && chl->num_channels == 1) {
                y = 0;
            }
            if (y < 0) {
                exr_finish(&ctx);
                return cx_unsupported(err, F, "no R/G/B or Y channels");
            }
            idx[0] = y;
            idx[1] = idx[3];
            idx[2] = -1;
            idx[3] = -1;
            layout = idx[1] >= 0 ? PC_CL_GRAYA : PC_CL_GRAY;
            nc = idx[1] >= 0 ? 2 : 1;
        }
        for (k = 0; k < (int)nc; k++) {
            const exr_attr_chlist_entry_t *e = &chl->entries[idx[k]];
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
    w = (uint64_t)((int64_t)dw.max.x - dw.min.x + 1);
    h = (uint64_t)((int64_t)dw.max.y - dw.min.y + 1);
    if ((st = pc_climits_check(l, (uint32_t)w, (uint32_t)h, layout, sample, err)) != PC_OK) {
        exr_finish(&ctx);
        return st;
    }
    if ((st = pc_cimage_new((uint32_t)w, (uint32_t)h, layout, sample, &img, err)) != PC_OK) {
        exr_finish(&ctx);
        return st;
    }
    {
        size_t bps = pc_csample_bytes(sample);
        size_t px = nc * bps;
        int32_t tx = 0;
        int32_t ty = 0;
        int32_t ntx = 1;
        int32_t nty = 1;
        int32_t spc = 1;
        int32_t y = dw.min.y;
        uint32_t tw = 0;
        uint32_t th = 0;
        if (storage == EXR_STORAGE_TILED) {
            if (exr_get_tile_counts(ctx, part, 0, 0, &ntx, &nty) != EXR_ERR_SUCCESS ||
                exr_get_tile_sizes(ctx, part, 0, 0, (int32_t *)&tw, (int32_t *)&th) != EXR_ERR_SUCCESS) {
                st = exr_fail(err, &s, EXR_ERR_INVALID_ATTR);
                goto fail;
            }
        } else if (exr_get_scanlines_per_chunk(ctx, part, &spc) != EXR_ERR_SUCCESS || spc < 1) {
            st = exr_fail(err, &s, EXR_ERR_INVALID_ATTR);
            goto fail;
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
                st = exr_fail(err, &s, r);
                goto fail;
            }
            r = dec_init ? exr_decoding_update(ctx, part, &ci, &dec) : exr_decoding_initialize(ctx, part, &ci, &dec);
            if (r != EXR_ERR_SUCCESS) {
                st = exr_fail(err, &s, r);
                goto fail;
            }
            dec_init = true;
            for (c = 0; c < dec.channel_count; c++) {
                exr_coding_channel_info_t *ch = &dec.channels[c];
                int k;
                ch->decode_to_ptr = NULL;
                for (k = 0; k < (int)nc; k++) {
                    if (idx[k] >= 0 && strcmp(ch->channel_name, chl->entries[idx[k]].name.str) == 0) {
                        size_t x0 = (size_t)((int64_t)ci.start_x - dw.min.x);
                        size_t y0 = (size_t)((int64_t)ci.start_y - dw.min.y);
                        ch->decode_to_ptr = img.data + (y0 * (size_t)w + x0) * px + (size_t)k * bps;
                        ch->user_pixel_stride = (int32_t)px;
                        ch->user_line_stride = (int32_t)((size_t)w * px);
                        ch->user_data_type = (uint16_t)(sample == PC_CS_F16 ? EXR_PIXEL_HALF : EXR_PIXEL_FLOAT);
                        ch->user_bytes_per_element = (int16_t)bps;
                        break;
                    }
                }
            }
            r = exr_decoding_choose_default_routines(ctx, part, &dec);
            if (r == EXR_ERR_SUCCESS) {
                r = exr_decoding_run(ctx, part, &dec);
            }
            if (r != EXR_ERR_SUCCESS) {
                st = exr_fail(err, &s, r);
                goto fail;
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
    }
    if (dec_init) {
        exr_decoding_destroy(ctx, &dec);
    }
    exr_finish(&ctx);
    *out = img;
    return PC_OK;
fail:
    if (dec_init) {
        exr_decoding_destroy(ctx, &dec);
    }
    exr_finish(&ctx);
    pc_cimage_fini(&img);
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

#endif
