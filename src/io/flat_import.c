/* Flat raster import through the codecs (port of the import half of crates/io/src/flat.rs). */
#include "io/flat.h"

#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "base/num.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "codecs/orientation.h"
#include "io/tiff_layers.h"
#include "raster/surface.h"

pc_status pc_io_import_flat(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                            pc_error *err)
{
    pc_cformat f;
    bool known = pc_cformat_detect(bytes, len, &f);
    pc_cimage img;
    pc_io_import_result r;
    pc_status st;
    if (known && f == PC_CF_TIFF) {
        return pc_io_import_tiff_page(name, bytes, len, false, 0, out, err);
    }
    st = pc_cdecode(bytes, len, &img, err);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "codec: ");
    }
    st = pc_io_image_to_document(name, &img, &r, err);
    pc_cimage_fini(&img);
    if (st != PC_OK) {
        return st;
    }
    /* OpenEXR and Radiance HDR hold linear, scene-referred values (Rec. 709 primaries unless
     * stated otherwise): tag them linear sRGB so they display and convert correctly. */
    if (r.document.icc_profile == NULL && r.document.mode == PC_COLOR_MODE_RGB && known &&
        (f == PC_CF_EXR || f == PC_CF_HDR)) {
        const pc_profile *p = pc_builtin_profile(PC_BUILTIN_LINEAR_SRGB);
        pc_buf icc = PC_BUF_INIT;
        st = p != NULL ? pc_profile_to_bytes(p, &icc, err) : pc_io_nomem(err, "building linear sRGB");
        if (st == PC_OK) {
            st = pc_io_set_blob(&r.document.icc_profile, icc.data, icc.len, err);
        }
        pc_buf_fini(&icc);
        if (st != PC_OK) {
            pc_io_import_result_fini(&r);
            return st;
        }
    }
    *out = r;
    return PC_OK;
}

pc_status pc_io_import_tiff_page(const char *name, const uint8_t *bytes, size_t len, bool has_page,
                                 size_t page, pc_io_import_result *out, pc_error *err)
{
    /* The orientation is applied after the layer check: rotating the composite but not the
     * layers would misalign them, so a layered TIFF keeps its stored orientation. */
    pc_cdecode_opts keep = pc_cdecode_opts_default();
    pc_cimage img;
    pc_cimage up;
    uint16_t orientation;
    pc_status st;
    keep.keep_orientation = true;
    if (!has_page) {
        st = pc_cdecode_with(bytes, len, &keep, &img, err);
        orientation = pc_ctiff_orientation(bytes, len);
    } else {
        st = pc_cdecode_tiff_page(bytes, len, page, &keep, &img, err);
        orientation = pc_ctiff_page_orientation(bytes, len, page);
    }
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "codec: ");
    }
    if (img.meta.photoshop_layers.has && img.meta.photoshop_layers.len > 0) {
        pc_io_import_result r;
        st = pc_io_import_layered(name, &img, img.meta.photoshop_layers.data, img.meta.photoshop_layers.len,
                                  &r, err);
        pc_cimage_fini(&img);
        if (st == PC_OK && orientation != 1) {
            st = pc_io_warn(
                &r.warnings, err,
                "the TIFF's orientation tag was ignored so the layers stay aligned with the image");
            if (st != PC_OK) {
                pc_io_import_result_fini(&r);
            }
        }
        if (st == PC_OK) {
            *out = r;
        }
        return st;
    }
    st = pc_cimage_oriented(&img, orientation, &up, err);
    pc_cimage_fini(&img);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "codec: ");
    }
    st = pc_io_image_to_document(name, &up, out, err);
    pc_cimage_fini(&up);
    return st;
}

/* Writes the image's pixels into s (target layout and sample), converting a band of rows at a
 * time so no second full-size copy is made. */
static pc_status fill_surface(const pc_cimage *img, pc_clayout target, pc_csample cs, pc_surface *s,
                              pc_error *err)
{
    uint32_t w = img->width;
    uint32_t h = img->height;
    size_t row;
    size_t band;
    size_t chunk;
    pc_status st = PC_OK;
    if (img->layout == target && img->sample == cs) {
        return pc_surface_write_interleaved(s, pc_rect_make(0, 0, pc_u32_as_i32(w), pc_u32_as_i32(h)),
                                            img->data, img->len, err);
    }
    row = img->len / (h > 0 ? h : 1);
    band = PC_IO_BAND_BYTES / (row > 0 ? row : 1);
    band = band > 0 ? band : 1;
    chunk = (row > 0 ? row : 1) * band;
    for (size_t at = 0, i = 0; at < img->len && st == PC_OK; at += chunk, i++) {
        size_t bytes = img->len - at < chunk ? img->len - at : chunk;
        uint32_t n = (uint32_t)(bytes / (row > 0 ? row : 1));
        int32_t y0 = pc_u32_as_i32((uint32_t)(i * band));
        pc_cimage part;
        pc_cimage conv;
        st = pc_cimage_from_copy(w, n, img->layout, img->sample, img->data + at, bytes, &part, err);
        if (st != PC_OK) {
            return pc_io_err_prefix(st, err, "codec: ");
        }
        st = pc_cimage_convert(&part, target, cs, &conv, err);
        pc_cimage_fini(&part);
        if (st != PC_OK) {
            return pc_io_err_prefix(st, err, "codec: ");
        }
        st = pc_surface_write_interleaved(
            s, pc_rect_make(0, y0, pc_u32_as_i32(w), pc_i32_sat_add(y0, (int32_t)n)), conv.data, conv.len,
            err);
        pc_cimage_fini(&conv);
    }
    return st;
}

pc_status pc_io_image_to_document(const char *name, const pc_cimage *img, pc_io_import_result *out,
                                  pc_error *err)
{
    pc_io_import_result r;
    pc_color_mode mode;
    pc_clayout target;
    pc_sample_type depth;
    pc_csample cs;
    pc_surface s;
    pc_layer bg;
    pc_status st = PC_OK;
    memset(&r, 0, sizeof r);
    pc_io_warnings_init(&r.warnings);
    /* What the decoder noticed (frames or pages left out, data ending early) comes first. */
    for (size_t i = 0; i < img->nwarnings && st == PC_OK; i++) {
        char text[512];
        pc_cwarning_text(&img->warnings[i], text, sizeof text);
        st = pc_io_warn(&r.warnings, err, "%s", text);
    }
    if (pc_clayout_is_gray(img->layout)) {
        mode = PC_COLOR_MODE_GRAYSCALE;
        target = PC_CL_GRAYA;
    } else if (pc_clayout_is_rgb(img->layout)) {
        mode = PC_COLOR_MODE_RGB;
        target = PC_CL_RGBA;
    } else {
        mode = PC_COLOR_MODE_CMYK;
        target = PC_CL_CMYKA;
    }
    switch (img->sample) {
    case PC_CS_U8:
        depth = PC_SAMPLE_TYPE_U8;
        cs = PC_CS_U8;
        break;
    case PC_CS_U16:
        depth = PC_SAMPLE_TYPE_U16;
        cs = PC_CS_U16;
        break;
    case PC_CS_F16:
        if (st == PC_OK) {
            st = pc_io_warn(&r.warnings, err, "16-bit float samples are stored as 32-bit float");
        }
        depth = PC_SAMPLE_TYPE_F32;
        cs = PC_CS_F32;
        break;
    default:
        depth = PC_SAMPLE_TYPE_F32;
        cs = PC_CS_F32;
        break;
    }
    if (st != PC_OK) {
        pc_io_warnings_fini(&r.warnings);
        return st;
    }
    st = pc_document_init(&r.document, name, pc_size_make(img->width, img->height), mode, depth, err);
    if (st != PC_OK) {
        pc_io_warnings_fini(&r.warnings);
        return st;
    }
    pc_surface_init(&s, (pc_pixel_format){mode, depth, true});
    st = fill_surface(img, target, cs, &s, err);
    if (st == PC_OK) {
        pc_surface_prune(&s);
        st = pc_io_raster_layer("Background", &s, &bg, err);
    }
    pc_surface_fini(&s);
    if (st == PC_OK) {
        if (!pc_clayout_has_alpha(img->layout)) {
            bg.locks.transparency = true;
            bg.locks.position = true;
        }
        st = pc_io_push_layer(&r.document, &bg, err);
        if (st != PC_OK) {
            pc_layer_fini(&bg);
        }
    }
    if (st == PC_OK && img->icc.has) {
        st = pc_io_set_blob(&r.document.icc_profile, img->icc.data, img->icc.len, err);
    }
    if (st == PC_OK && img->meta.exif.has) {
        st = pc_io_set_blob(&r.document.metadata.exif, img->meta.exif.data, img->meta.exif.len, err);
    }
    if (st == PC_OK && img->meta.xmp.has) {
        st = pc_str_set(&r.document.metadata.xmp, (const char *)img->meta.xmp.data, img->meta.xmp.len, err);
        r.document.metadata.has_xmp = st == PC_OK;
    }
    if (st == PC_OK && img->meta.has_dpi) {
        r.document.resolution_dpi = img->meta.dpi_x;
        st = pc_io_unequal_resolution_warning((double)img->meta.dpi_x, (double)img->meta.dpi_y, &r.warnings,
                                              err);
    }
    if (st == PC_OK && img->meta.ntext > 0) {
        st = pc_io_warn(&r.warnings, err, "%zu text metadata entries are not kept in the document",
                        img->meta.ntext);
    }
    if (st != PC_OK) {
        pc_io_import_result_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}
