/* The PSD/PSB thumbnail image resource (1036), written when Preferences > File Handling > Image
 * Previews saves them (Photoshop's default, Always Save). Port of crates/io/src/psd_thumbnail.rs.
 *
 * Layout (Adobe PSD spec, "Thumbnail resource format"): a 28-byte big-endian header (format 1 =
 * kJpegRGB, width, height, widthbytes = (width x 24 + 31) / 32 x 4, total size = widthbytes x
 * height, compressed size, bits per pixel 24, planes 1) followed by the JFIF stream. Photoshop
 * keeps the long side at 160 px or less. */
#include "io/psd_thumbnail.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "cms/builtin.h"
#include "cms/cms.h"
#include "cms/profile.h"
#include "cms/transform.h"
#include "codecs/codecs.h"
#include "codecs/format.h"
#include "codecs/image.h"
#include "compose/compose.h"

/* A sample clamped to 0-1 (NaN as 0): 32-bit composites can hold values past white. */
static float finite_sample(float v)
{
    if (v != v) {
        return 0.0f;
    }
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* The profile of the compositor's RGB output for doc: an RGB document's own (embedded when it
 * parses, else sRGB), a gray document's gray profile seen as RGB, and sRGB for CMYK and Lab
 * (which the compositor renders in sRGB). *out is an owned profile. */
static pc_status composite_profile(const pc_document *doc, pc_profile *out, pc_error *err)
{
    uint32_t space;
    bool want_gray;
    pc_profile own;
    bool have_own = false;
    pc_status st;
    switch (doc->mode) {
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_BITMAP:
    case PC_COLOR_MODE_DUOTONE:
        space = PC_CS_GRAY;
        want_gray = true;
        break;
    case PC_COLOR_MODE_CMYK:
    case PC_COLOR_MODE_LAB:
        return pc_profile_clone(pc_builtin_profile(PC_BUILTIN_SRGB), out, err);
    default:
        space = PC_CS_RGB;
        want_gray = false;
        break;
    }
    /* The document's own profile, when it parses and matches the model. */
    if (doc->icc_profile != NULL) {
        pc_error pe;
        if (pc_profile_parse(pc_blob_data(doc->icc_profile), pc_blob_len(doc->icc_profile), &own, &pe) == PC_OK) {
            if (own.space == space) {
                have_own = true;
            } else {
                pc_profile_fini(&own);
            }
        }
    }
    if (!want_gray) {
        if (have_own) {
            *out = own;
            return PC_OK;
        }
        return pc_profile_clone(pc_builtin_profile(PC_BUILTIN_SRGB), out, err);
    }
    /* Gray seen as RGB: the document's gray profile, else the built-in sGray, else sRGB. */
    {
        bool ok = false;
        if (have_own) {
            st = pc_profile_gray_as_rgb(&own, out, &ok, err);
            pc_profile_fini(&own);
            if (st != PC_OK) {
                return st;
            }
            if (ok) {
                return PC_OK;
            }
        }
        st = pc_profile_gray_as_rgb(pc_builtin_profile(PC_BUILTIN_SGRAY), out, &ok, err);
        if (st != PC_OK) {
            return st;
        }
        if (ok) {
            return PC_OK;
        }
        return pc_profile_clone(pc_builtin_profile(PC_BUILTIN_SRGB), out, err);
    }
}

/* Header plus JFIF data of a w x h thumbnail resource. */
static pc_status thumbnail_bytes(uint32_t w, uint32_t h, const uint8_t *jfif, size_t jfif_len, pc_buf *out,
                                 pc_error *err)
{
    uint32_t width_bytes;
    uint32_t total;
    uint8_t hdr[PC_IO_THUMBNAIL_HEADER_LEN];
    pc_status st;
    if (w > UINT32_MAX / 24) {
        return pc_io_unsupported(err, "image preview too large");
    }
    width_bytes = ((w * 24 + 31) / 32) * 4;
    if (h != 0 && width_bytes > UINT32_MAX / h) {
        return pc_io_unsupported(err, "image preview too large");
    }
    total = width_bytes * h;
    if (jfif_len > UINT32_MAX) {
        return pc_io_unsupported(err, "image preview too large");
    }
    pc_store_be32(hdr, 1);
    pc_store_be32(hdr + 4, w);
    pc_store_be32(hdr + 8, h);
    pc_store_be32(hdr + 12, width_bytes);
    pc_store_be32(hdr + 16, total);
    pc_store_be32(hdr + 20, (uint32_t)jfif_len);
    pc_store_be16(hdr + 24, 24);
    pc_store_be16(hdr + 26, 1);
    st = pc_buf_append(out, hdr, sizeof hdr, err);
    if (st == PC_OK) {
        st = pc_buf_append(out, jfif, jfif_len, err);
    }
    return st;
}

pc_status pc_io_psd_thumbnail_resource(const pc_document *doc, pc_psd_resource *out, pc_error *err)
{
    pc_compose_buffer buf;
    uint32_t w;
    uint32_t h;
    pc_profile src;
    const pc_profile *dst = pc_builtin_profile(PC_BUILTIN_SRGB);
    uint8_t *rgb = NULL;
    pc_cimage img;
    pc_buf jfif = PC_BUF_INIT;
    pc_buf bytes = PC_BUF_INIT;
    pc_cencode_opts opts;
    pc_status st = pc_compose_thumbnail_buffer(doc, PC_IO_THUMBNAIL_MAX_SIDE, &buf, err);
    if (st != PC_OK) {
        return st;
    }
    w = pc_rect_width(buf.rect);
    h = pc_rect_height(buf.rect);
    if (w == 0 || h == 0 || w > PC_IO_THUMBNAIL_MAX_SIDE || h > PC_IO_THUMBNAIL_MAX_SIDE ||
        buf.len != (size_t)w * h) {
        pc_compose_buffer_fini(&buf);
        return pc_io_unsupported(err, "the image preview could not be rendered");
    }
    st = composite_profile(doc, &src, err);
    if (st != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return st;
    }
    if (!pc_profile_same_colors(&src, dst)) {
        pc_transform *t = NULL;
        st = pc_transform_new(&src, dst, PC_INTENT_RELATIVE, true, &t, err);
        if (st == PC_OK) {
            st = pc_transform_apply(t, (float *)buf.px, buf.len * 4, 4, err);
        }
        pc_transform_release(t);
    }
    pc_profile_fini(&src);
    if (st == PC_OK) {
        rgb = pc_malloc_array(buf.len > 0 ? buf.len : 1, 3);
        if (rgb == NULL) {
            st = pc_io_nomem(err, "rendering the image preview");
        }
    }
    for (size_t i = 0; st == PC_OK && i < buf.len; i++) {
        float a = finite_sample(buf.px[i][3]);
        for (size_t c = 0; c < 3; c++) {
            /* Over white, as Photoshop shows a transparent document's preview. */
            float v = finite_sample(buf.px[i][c]) * a + (1.0f - a);
            rgb[i * 3 + c] = pc_f32_to_u8(roundf(v * 255.0f));
        }
    }
    pc_compose_buffer_fini(&buf);
    if (st == PC_OK) {
        st = pc_cimage_from_raw(w, h, PC_CL_RGB, PC_CS_U8, rgb, (size_t)w * h * 3, &img, err);
        if (st == PC_OK) {
            rgb = NULL; /* taken over by the image */
        }
    }
    if (st == PC_OK) {
        opts = pc_cencode_opts_default();
        opts.jpeg_quality = 80;
        opts.embed_icc = false;
        opts.embed_metadata = false;
        st = pc_cencode(&img, PC_CF_JPEG, &opts, &jfif, err);
        pc_cimage_fini(&img);
    }
    free(rgb);
    if (st == PC_OK) {
        st = thumbnail_bytes(w, h, jfif.data, jfif.len, &bytes, err);
    }
    pc_buf_fini(&jfif);
    if (st == PC_OK) {
        st = pc_psd_resource_init(out, PC_PSD_RES_THUMBNAIL, bytes.data, bytes.len, err);
    }
    pc_buf_fini(&bytes);
    return st;
}
