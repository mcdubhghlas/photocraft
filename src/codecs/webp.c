/* WebP (port of crates/codecs/src/codecs/webp.rs; REWRITE_PLAN.md D9): libwebp decodes lossy
 * and lossless (an animation's first frame, composited on its canvas, with a MoreFrames
 * warning) and encodes lossless (exact: RGB under transparent pixels is kept) or lossy; the
 * ICCP / EXIF / XMP chunks are read with WebPDemux and written with WebPMux. */
#include <stdlib.h>
#include <string.h>

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include "base/utf8.h"
#include "codecs/internal.h"
#include "codecs/orientation.h"

#define F PC_CF_WEBP
#define MAX_DIMENSION 16383u

static pc_status chunk_blob(WebPDemuxer *dmx, const char *id, pc_meta_blob *out, bool utf8, pc_error *err)
{
    WebPChunkIterator it;
    pc_status st = PC_OK;
    if (WebPDemuxGetChunk(dmx, id, 1, &it)) {
        bool ok = true;
        if (utf8) {
            size_t i = 0;
            uint32_t cp;
            while (i < it.chunk.size) {
                if (!pc_utf8_next(it.chunk.bytes, it.chunk.size, &i, &cp)) {
                    ok = false;
                    break;
                }
            }
        }
        if (ok) {
            st = pc_meta_blob_set(out, it.chunk.bytes, it.chunk.size, err);
        }
        WebPDemuxReleaseChunkIterator(&it);
    }
    return st;
}

pc_status cx_webp_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    WebPBitstreamFeatures feat;
    VP8StatusCode vs = WebPGetFeatures(b, n, &feat);
    pc_clayout layout;
    pc_cimage img;
    WebPData data;
    WebPDemuxer *dmx;
    uint32_t frames = 1;
    pc_status st;
    if (vs != VP8_STATUS_OK) {
        return cx_malformed(err, F, "invalid WebP data (libwebp status %d)", (int)vs);
    }
    layout = feat.has_alpha ? PC_CL_RGBA : PC_CL_RGB;
    if ((st = pc_climits_check(l, (uint32_t)feat.width, (uint32_t)feat.height, layout, PC_CS_U8, err)) != PC_OK) {
        return st;
    }
    if ((st = pc_cimage_new((uint32_t)feat.width, (uint32_t)feat.height, layout, PC_CS_U8, &img, err)) != PC_OK) {
        return st;
    }
    data.bytes = b;
    data.size = n;
    if (feat.has_animation) {
        WebPAnimDecoderOptions ao;
        WebPAnimDecoder *ad;
        WebPAnimInfo info;
        uint8_t *frame = NULL;
        int ts = 0;
        if (!WebPAnimDecoderOptionsInit(&ao)) {
            pc_cimage_fini(&img);
            return cx_malformed(err, F, "libwebp version mismatch");
        }
        ao.color_mode = MODE_RGBA;
        ao.use_threads = 0;
        ad = WebPAnimDecoderNew(&data, &ao);
        if (ad == NULL || !WebPAnimDecoderGetInfo(ad, &info) || !WebPAnimDecoderGetNext(ad, &frame, &ts) ||
            info.canvas_width != (uint32_t)feat.width || info.canvas_height != (uint32_t)feat.height) {
            WebPAnimDecoderDelete(ad);
            pc_cimage_fini(&img);
            return cx_malformed(err, F, "invalid animated WebP data");
        }
        frames = info.frame_count;
        {
            size_t px = (size_t)feat.width * (size_t)feat.height;
            size_t i;
            if (layout == PC_CL_RGBA) {
                memcpy(img.data, frame, px * 4);
            } else {
                for (i = 0; i < px; i++) {
                    memcpy(img.data + 3 * i, frame + 4 * i, 3);
                }
            }
        }
        WebPAnimDecoderDelete(ad);
    } else {
        uint8_t *r;
        int stride = feat.width * (layout == PC_CL_RGBA ? 4 : 3);
        if (layout == PC_CL_RGBA) {
            r = WebPDecodeRGBAInto(b, n, img.data, img.len, stride);
        } else {
            r = WebPDecodeRGBInto(b, n, img.data, img.len, stride);
        }
        if (r == NULL) {
            pc_cimage_fini(&img);
            return cx_malformed(err, F, "invalid WebP image data");
        }
    }
    dmx = WebPDemux(&data);
    if (dmx != NULL) {
        st = chunk_blob(dmx, "ICCP", &img.icc, false, err);
        if (st == PC_OK) {
            st = chunk_blob(dmx, "EXIF", &img.meta.exif, false, err);
        }
        if (st == PC_OK) {
            st = chunk_blob(dmx, "XMP ", &img.meta.xmp, true, err);
        }
        WebPDemuxDelete(dmx);
        if (st != PC_OK) {
            pc_cimage_fini(&img);
            return st;
        }
    }
    if (feat.has_animation && frames > 1) {
        pc_cwarning w;
        memset(&w, 0, sizeof w);
        w.kind = PC_CW_MORE_FRAMES;
        w.has_total = true;
        w.total = frames;
        if ((st = pc_cimage_add_warning(&img, w, err)) != PC_OK) {
            pc_cimage_fini(&img);
            return st;
        }
    }
    *out = img;
    return PC_OK;
}

static int write_fn(const uint8_t *data, size_t size, const WebPPicture *pic)
{
    pc_buf *b = pic->custom_ptr;
    return pc_buf_append(b, data, size, NULL) == PC_OK;
}

pc_status cx_webp_encode(const pc_cimage *src, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    WebPConfig cfg;
    WebPPicture pic;
    pc_buf bits = PC_BUF_INIT;
    bool rgba;
    uint8_t *ex = NULL;
    size_t exn = 0;
    char *xm = NULL;
    size_t xmn = 0;
    int ok;
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    if (img->layout != PC_CL_RGB && img->layout != PC_CL_RGBA) {
        st = cx_encode(err, F, "unsupported layout %s", pc_clayout_name(img->layout));
        goto done;
    }
    if (img->width > MAX_DIMENSION || img->height > MAX_DIMENSION) {
        st = cx_encode(err, F, "%s WebP holds at most %u pixels per side", o->webp_lossless ? "lossless" : "lossy",
                       MAX_DIMENSION);
        goto done;
    }
    rgba = img->layout == PC_CL_RGBA;
    if (rgba && !o->webp_lossless) {
        /* Fully opaque alpha is dropped rather than stored. */
        size_t i;
        bool opaque = true;
        for (i = 3; i < img->len; i += 4) {
            if (img->data[i] != 255) {
                opaque = false;
                break;
            }
        }
        if (opaque) {
            rgba = false;
        }
    }
    if (!WebPConfigInit(&cfg) || !WebPPictureInit(&pic)) {
        st = cx_encode(err, F, "libwebp version mismatch");
        goto done;
    }
    if (o->webp_lossless) {
        cfg.lossless = 1;
        cfg.exact = 1;
        cfg.quality = 75.0f;
        cfg.method = 4;
    } else {
        cfg.lossless = 0;
        cfg.quality = (float)(o->webp_quality > 100 ? 100 : o->webp_quality);
        cfg.method = 4;
        cfg.alpha_quality = 100;
    }
    pic.use_argb = 1;
    pic.width = (int)img->width;
    pic.height = (int)img->height;
    if (img->layout == PC_CL_RGBA) {
        if (rgba) {
            ok = WebPPictureImportRGBA(&pic, img->data, (int)img->width * 4);
        } else {
            ok = WebPPictureImportRGBX(&pic, img->data, (int)img->width * 4);
        }
    } else {
        ok = WebPPictureImportRGB(&pic, img->data, (int)img->width * 3);
    }
    if (!ok) {
        WebPPictureFree(&pic);
        st = cx_encode(err, F, "out of memory");
        goto done;
    }
    pic.writer = write_fn;
    pic.custom_ptr = &bits;
    ok = WebPEncode(&cfg, &pic);
    if (!ok) {
        st = cx_encode(err, F, "libwebp error %d", (int)pic.error_code);
        WebPPictureFree(&pic);
        goto done;
    }
    WebPPictureFree(&pic);
    {
        bool icc = o->embed_icc && img->icc.has;
        bool exif = o->embed_metadata && img->meta.exif.has;
        bool xmp = o->embed_metadata && img->meta.xmp.has;
        if (!icc && !exif && !xmp) {
            pc_buf_fini(out);
            *out = bits;
            bits.data = NULL;
            bits.len = bits.cap = 0;
        } else {
            WebPMux *mux;
            WebPData in;
            WebPData chunk;
            WebPData assembled;
            in.bytes = bits.data;
            in.size = bits.len;
            mux = WebPMuxCreate(&in, 1);
            if (mux == NULL) {
                st = cx_encode(err, F, "cannot add metadata chunks");
                goto done;
            }
            /* The pixels are written as they are shown: never let a viewer rotate them again. */
            if (exif && (st = pc_upright_exif(img->meta.exif.data, img->meta.exif.len, &ex, &exn, err)) != PC_OK) {
                WebPMuxDelete(mux);
                goto done;
            }
            if (xmp && (st = pc_upright_xmp((const char *)img->meta.xmp.data, img->meta.xmp.len, &xm, &xmn, err)) !=
                           PC_OK) {
                WebPMuxDelete(mux);
                goto done;
            }
            ok = 1;
            if (icc) {
                chunk.bytes = img->icc.data;
                chunk.size = img->icc.len;
                ok = ok && WebPMuxSetChunk(mux, "ICCP", &chunk, 1) == WEBP_MUX_OK;
            }
            if (exif) {
                chunk.bytes = ex != NULL ? ex : img->meta.exif.data;
                chunk.size = ex != NULL ? exn : img->meta.exif.len;
                ok = ok && WebPMuxSetChunk(mux, "EXIF", &chunk, 1) == WEBP_MUX_OK;
            }
            if (xmp) {
                chunk.bytes = xm != NULL ? (const uint8_t *)xm : img->meta.xmp.data;
                chunk.size = xm != NULL ? xmn : img->meta.xmp.len;
                ok = ok && WebPMuxSetChunk(mux, "XMP ", &chunk, 1) == WEBP_MUX_OK;
            }
            WebPDataInit(&assembled);
            ok = ok && WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK;
            WebPMuxDelete(mux);
            if (!ok) {
                WebPDataClear(&assembled);
                st = cx_encode(err, F, "cannot add metadata chunks");
                goto done;
            }
            pc_buf_truncate(out, 0);
            st = pc_buf_append(out, assembled.bytes, assembled.size, err);
            WebPDataClear(&assembled);
        }
    }
done:
    free(ex);
    free(xm);
    pc_buf_fini(&bits);
    pc_cimage_fini(&tmp);
    return st;
}
