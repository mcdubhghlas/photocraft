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
#include "codecs/source_settings.h"
#include "io/tiff_layers.h"
#include "raster/surface.h"

/* The lossy settings `bytes` were encoded with (File > Save keeps them): a JPEG's quality
 * (estimated from its quantisation tables) and a WebP's lossless/lossy encoding and quality. */
static void set_file_encoding(const uint8_t *bytes, size_t len, pc_file_encoding *e)
{
    pc_cformat f;
    pc_webp_encoding w;
    uint8_t q;
    *e = (pc_file_encoding){0};
    if (!pc_cformat_detect(bytes, len, &f)) {
        return;
    }
    if (f == PC_CF_JPEG) {
        if (pc_jpeg_quality(bytes, len, &q)) {
            e->has_jpeg_quality = true;
            e->jpeg_quality = q;
        }
    } else if (f == PC_CF_WEBP && pc_webp_encoding_of(bytes, len, &w)) {
        if (w.kind == PC_WEBP_LOSSLESS) {
            e->has_webp_lossless = true;
            e->webp_lossless = true;
        } else {
            e->has_webp_lossless = true;
            e->webp_lossless = false;
            e->has_webp_quality = true;
            e->webp_quality = w.has_quality ? w.quality : PC_IO_DEFAULT_LOSSY_WEBP_QUALITY;
        }
    }
}

/* Photoshop opens a 32-bit Targa as an opaque Background plus an "Alpha 1" channel holding the
 * fourth byte, even when the header declares no alpha bits, keeping the stored colours (#2225).
 * Layer transparency would hide those colours and lose the channel on the next save. */
static pc_status tga_alpha_to_channel(pc_document *doc, pc_error *err)
{
    pc_rect canvas = pc_document_bounds(doc);
    pc_sample_type depth = doc->depth;
    pc_layer *layer;
    pc_surface *s;
    size_t ch;
    float *v = NULL;
    size_t len = 0;
    size_t npx;
    pc_surface alpha;
    float *a = NULL;
    pc_alpha_channel channel;
    pc_alpha_channel *nv;
    size_t cap;
    pc_status st;
    if (doc->nlayers == 0 || doc->layers[0].content.kind != PC_LAYER_RASTER) {
        return PC_OK;
    }
    layer = &doc->layers[0];
    s = &layer->content.u.raster;
    ch = pc_surface_channels(s);
    if (!pc_surface_format(s).alpha || ch < 2) {
        return PC_OK;
    }
    st = pc_surface_read_region(s, canvas, &v, &len, err);
    if (st != PC_OK) {
        return st;
    }
    npx = len / ch;
    a = pc_malloc_array(npx > 0 ? npx : 1, sizeof *a);
    if (a == NULL) {
        free(v);
        return pc_io_nomem(err, "reading the Targa alpha channel");
    }
    for (size_t i = 0; i < npx; i++) {
        a[i] = v[i * ch + ch - 1];
        v[i * ch + ch - 1] = 1.0f;
    }
    st = pc_surface_write_region(s, canvas, v, len, err);
    free(v);
    pc_surface_init(&alpha, (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, depth, false});
    if (st == PC_OK) {
        st = pc_surface_write_region(&alpha, canvas, a, npx, err);
    }
    free(a);
    if (st != PC_OK) {
        pc_surface_fini(&alpha);
        return st;
    }
    pc_surface_prune(s);
    pc_surface_prune(&alpha);
    st = pc_str_set_c(&layer->name, "Background", err);
    if (st != PC_OK) {
        pc_surface_fini(&alpha);
        return st;
    }
    layer->locks.transparency = true;
    layer->locks.position = true;
    st = pc_alpha_channel_init(&channel, "Alpha 1", &alpha, err);
    if (st != PC_OK) {
        pc_surface_fini(&alpha);
        return st;
    }
    cap = doc->nchannels;
    nv = pc_grow_array(doc->channels, &cap, doc->nchannels + 1, sizeof *nv);
    if (nv == NULL) {
        pc_alpha_channel_fini(&channel);
        return pc_io_nomem(err, "adding the Targa alpha channel");
    }
    doc->channels = nv;
    doc->channels[doc->nchannels++] = channel;
    return PC_OK;
}

pc_status pc_io_import_flat(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                            pc_error *err)
{
    pc_cformat f;
    bool known = pc_cformat_detect(bytes, len, &f);
    pc_cimage img;
    pc_clayout img_layout;
    pc_io_import_result r;
    pc_status st;
    if (known && f == PC_CF_TIFF) {
        return pc_io_import_tiff_page(name, bytes, len, false, 0, out, err);
    }
    st = pc_cdecode(bytes, len, &img, err);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "codec: ");
    }
    img_layout = img.layout;
    st = pc_io_image_to_document(name, &img, &r, err);
    pc_cimage_fini(&img);
    if (st != PC_OK) {
        return st;
    }
    /* A palette BMP opens in Indexed Color mode with its colour table, as in Photoshop (#3802). */
    if (known && f == PC_CF_BMP && r.document.mode == PC_COLOR_MODE_RGB) {
        bool has = false;
        uint8_t (*palette)[3] = NULL;
        size_t npal = 0;
        st = pc_cbmp_palette(bytes, len, &has, &palette, &npal, err);
        if (st == PC_OK && has && npal > 0) {
            r.document.color_table.colors = palette;
            r.document.color_table.ncolors = npal;
            r.document.color_table.has_transparent = false;
            r.document.has_color_table = true;
            r.document.mode = PC_COLOR_MODE_INDEXED;
            palette = NULL;
        }
        free(palette);
        if (st != PC_OK) {
            pc_io_import_result_fini(&r);
            return pc_io_err_prefix(st, err, "codec: ");
        }
    }
    set_file_encoding(bytes, len, &r.document.file_encoding);
    if (known && f == PC_CF_TGA && img_layout == PC_CL_RGBA) {
        st = tga_alpha_to_channel(&r.document, err);
        if (st != PC_OK) {
            pc_io_import_result_fini(&r);
            return st;
        }
    }
    /* OpenEXR and Radiance HDR hold linear, scene-referred values (Rec. 709 primaries unless
     * stated otherwise; an OpenEXR `Y` is linear luminance): tag them linear sRGB or linear gray
     * so they display and convert correctly. */
    if (r.document.icc_profile == NULL && known && (f == PC_CF_EXR || f == PC_CF_HDR) &&
        (r.document.mode == PC_COLOR_MODE_RGB || r.document.mode == PC_COLOR_MODE_GRAYSCALE)) {
        const pc_profile *p = r.document.mode == PC_COLOR_MODE_RGB ? pc_builtin_profile(PC_BUILTIN_LINEAR_SRGB)
                                                                   : pc_builtin_linear_gray();
        pc_buf icc = PC_BUF_INIT;
        st = p != NULL ? pc_profile_to_bytes(p, &icc, err) : pc_io_nomem(err, "building a linear profile");
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

pc_status pc_io_image_surface(const pc_cimage *img, pc_clayout target, pc_csample cs, pc_surface *out,
                              pc_error *err)
{
    pc_color_mode mode = PC_COLOR_MODE_RGB;
    pc_sample_type depth = cs == PC_CS_U8 ? PC_SAMPLE_TYPE_U8 : cs == PC_CS_U16 ? PC_SAMPLE_TYPE_U16
                                                                                 : PC_SAMPLE_TYPE_F32;
    pc_surface s;
    pc_status st;
    if (pc_clayout_is_gray(target)) {
        mode = PC_COLOR_MODE_GRAYSCALE;
    } else if (target == PC_CL_CMYK || target == PC_CL_CMYKA) {
        mode = PC_COLOR_MODE_CMYK;
    }
    pc_surface_init(&s, (pc_pixel_format){mode, depth, true});
    st = fill_surface(img, target, cs, &s, err);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
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
    /* As in Photoshop: an opaque image opens as the locked Background layer; one with
     * transparency (an alpha channel, or a tRNS chunk the decoder expands to one) opens as a
     * normal layer named "Layer 0", since a Background can't hold transparency. */
    if (st == PC_OK) {
        bool has_alpha = pc_clayout_has_alpha(img->layout);
        pc_surface_prune(&s);
        st = pc_io_raster_layer(has_alpha ? "Layer 0" : "Background", &s, &bg, err);
        if (st == PC_OK && !has_alpha) {
            bg.locks.transparency = true;
            bg.locks.position = true;
        }
    }
    pc_surface_fini(&s);
    if (st == PC_OK) {
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
    /* Free-form text metadata is kept in the document (metadata.text), in file order. */
    if (st == PC_OK && img->meta.ntext > 0) {
        pc_doc_text_entry *t = pc_malloc_array(img->meta.ntext, sizeof *t);
        if (t == NULL) {
            st = pc_io_nomem(err, "reading text metadata");
        }
        for (size_t i = 0; st == PC_OK && i < img->meta.ntext; i++) {
            t[i].key = (pc_str){NULL, 0};
            t[i].value = (pc_str){NULL, 0};
            st = pc_str_set(&t[i].key, img->meta.text[i].key, img->meta.text[i].key_len, err);
            if (st == PC_OK) {
                st = pc_str_set(&t[i].value, img->meta.text[i].value, img->meta.text[i].value_len, err);
            }
            if (st != PC_OK) {
                pc_str_fini(&t[i].key);
                pc_str_fini(&t[i].value);
                for (size_t j = 0; j < i; j++) {
                    pc_str_fini(&t[j].key);
                    pc_str_fini(&t[j].value);
                }
                free(t);
                t = NULL;
            }
        }
        if (st == PC_OK) {
            r.document.metadata.text = t;
            r.document.metadata.ntext = img->meta.ntext;
        }
    }
    if (st != PC_OK) {
        pc_io_import_result_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}
