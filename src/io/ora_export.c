/* OpenRaster (.ora) export (port of the write half of crates/io/src/ora.rs, R10): raster layers
 * and groups as PNGs under a stack.xml, plus mergedimage.png and Thumbnails/thumbnail.png. Other
 * layer kinds are written as their rendered pixels, layer masks are baked into the pixels, and
 * whatever ORA cannot hold is reported in the warnings. See ora_internal.h for the shared parts.
 * Thread-safe (no shared state). */
#include "io/ora.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/num.h"
#include "base/str.h"
#include "base/utf8.h"
#include "codecs/codecs.h"
#include "color/blend.h"
#include "compose/compose.h"
#include "format/zip.h"
#include "geom/geom.h"
#include "io/core.h"
#include "io/flat.h"
#include "io/ora_internal.h"
#include "raster/pixel.h"
#include "raster/surface.h"

/* The Krita composite-op id written for `mode` (NULL for modes Krita has no counterpart for). */
static const char *ora_krita_id(pc_blend_mode mode)
{
    switch (mode) {
    case PC_BLEND_NORMAL:
    case PC_BLEND_PASS_THROUGH:
        return "normal";
    case PC_BLEND_DISSOLVE:
        return "dissolve";
    case PC_BLEND_DARKEN:
        return "darken";
    case PC_BLEND_MULTIPLY:
        return "multiply";
    case PC_BLEND_COLOR_BURN:
        return "burn";
    case PC_BLEND_LINEAR_BURN:
        return "linear_burn";
    case PC_BLEND_DARKER_COLOR:
        return "darker color";
    case PC_BLEND_LIGHTEN:
        return "lighten";
    case PC_BLEND_SCREEN:
        return "screen";
    case PC_BLEND_COLOR_DODGE:
        return "dodge";
    case PC_BLEND_LINEAR_DODGE:
        return "linear_dodge";
    case PC_BLEND_LIGHTER_COLOR:
        return "lighter color";
    case PC_BLEND_OVERLAY:
        return "overlay";
    case PC_BLEND_SOFT_LIGHT:
        return "soft_light";
    case PC_BLEND_HARD_LIGHT:
        return "hard_light";
    case PC_BLEND_VIVID_LIGHT:
        return "vivid_light";
    case PC_BLEND_LINEAR_LIGHT:
        return "linear light";
    case PC_BLEND_PIN_LIGHT:
        return "pin_light";
    case PC_BLEND_HARD_MIX:
        return "hard_mix_photoshop";
    case PC_BLEND_DIFFERENCE:
        return "diff";
    case PC_BLEND_EXCLUSION:
        return "exclusion";
    case PC_BLEND_SUBTRACT:
        return "subtract";
    case PC_BLEND_DIVIDE:
        return "divide";
    case PC_BLEND_HUE:
        return "hue";
    case PC_BLEND_SATURATION:
        return "saturation";
    case PC_BLEND_COLOR:
        return "color";
    case PC_BLEND_LUMINOSITY:
        return "luminize";
    case PC_BLEND_REFLECT:
        return "reflect";
    case PC_BLEND_GLOW:
        return "glow";
    case PC_BLEND_NEGATION:
        return "negation";
    case PC_BLEND_XOR:
    case PC_BLEND_PAINT_NET_COLOR_BURN:
    case PC_BLEND_PAINT_NET_COLOR_DODGE:
        return NULL;
    default:
        return NULL;
    }
}


/* Attribute text with XML's five special characters escaped (and control characters dropped). */
static pc_status ora_escape(const char *s, pc_buf *out, pc_error *err)
{
    size_t len = strlen(s);
    size_t i = 0;
    pc_status st = PC_OK;
    while (i < len && st == PC_OK) {
        size_t start = i;
        uint32_t cp;
        if (!pc_utf8_next((const uint8_t *)s, len, &i, &cp)) {
            /* Not reachable for valid UTF-8 names; copy the raw byte to stay safe. */
            st = pc_buf_append_byte(out, (uint8_t)s[i], err);
            i++;
            continue;
        }
        switch (cp) {
        case '&':
            st = pc_buf_append_str(out, "&amp;", err);
            break;
        case '<':
            st = pc_buf_append_str(out, "&lt;", err);
            break;
        case '>':
            st = pc_buf_append_str(out, "&gt;", err);
            break;
        case '"':
            st = pc_buf_append_str(out, "&quot;", err);
            break;
        case '\'':
            st = pc_buf_append_str(out, "&apos;", err);
            break;
        case '\t':
            st = pc_buf_append_str(out, "&#9;", err);
            break;
        case '\n':
            st = pc_buf_append_str(out, "&#10;", err);
            break;
        default:
            if (cp <= 0x1f || (cp >= 0x7f && cp <= 0x9f)) {
                break; /* other control characters are dropped */
            }
            st = pc_buf_append(out, s + start, i - start, err);
            break;
        }
    }
    return st;
}

/* The composite-op attribute for `mode`: the SVG name where the spec has one, else krita:<id>. */
static bool ora_op_name(pc_blend_mode mode, const char **svg, char *kbuf, size_t kcap)
{
    size_t i;
    const char *id;
    if (mode == PC_BLEND_PASS_THROUGH) {
        *svg = "svg:src-over";
        return true;
    }
    if (mode == PC_BLEND_LINEAR_DODGE) {
        *svg = "svg:plus";
        return true;
    }
    for (i = 0; i < ORA_SVG_OP_COUNT; i++) {
        if (ORA_SVG_OPS[i].mode == mode) {
            *svg = ORA_SVG_OPS[i].name;
            return true;
        }
    }
    id = ora_krita_id(mode);
    if (id != NULL) {
        (void)snprintf(kbuf, kcap, "krita:%s", id);
        *svg = kbuf;
        return true;
    }
    return false;
}

/* The shortest decimal that round-trips to the f32 value (like Rust's float Display). */
static void ora_fmt_f32(float v, char *buf, size_t cap)
{
    int p;
    for (p = 1; p <= 9; p++) {
        (void)snprintf(buf, cap, "%.*g", p, (double)v);
        if (strtof(buf, NULL) == v) {
            return;
        }
    }
    (void)snprintf(buf, cap, "%.9g", (double)v);
}

typedef struct {
    pc_rect rect;
    float (*px)[4];
    size_t n;
} ora_px;

static void ora_px_fini(ora_px *p)
{
    free(p->px);
    p->px = NULL;
    p->n = 0;
}

/* px (row-major over rect) cropped to its non-transparent pixels, transparent pixels zeroed. */
static pc_status ora_crop(ora_px *p, pc_error *err)
{
    uint32_t w = pc_rect_width(p->rect);
    size_t x0 = SIZE_MAX, y0 = SIZE_MAX, x1 = 0, y1 = 0;
    size_t i;
    size_t ow;
    size_t oh;
    float (*out)[4];
    size_t yy;
    if (w == 0) {
        ora_px_fini(p);
        p->rect = PC_RECT_EMPTY;
        return PC_OK;
    }
    for (i = 0; i < p->n; i++) {
        if (p->px[i][3] <= 0.0f) {
            p->px[i][0] = 0.0f;
            p->px[i][1] = 0.0f;
            p->px[i][2] = 0.0f;
            p->px[i][3] = 0.0f;
            continue;
        }
        {
            size_t x = i % w;
            size_t y = i / w;
            if (x < x0) {
                x0 = x;
            }
            if (y < y0) {
                y0 = y;
            }
            if (x + 1 > x1) {
                x1 = x + 1;
            }
            if (y + 1 > y1) {
                y1 = y + 1;
            }
        }
    }
    if (x0 >= x1) {
        ora_px_fini(p);
        p->rect = PC_RECT_EMPTY;
        return PC_OK;
    }
    if (x0 == 0 && y0 == 0 && x1 == w && y1 == p->n / w) {
        return PC_OK; /* already tight */
    }
    ow = x1 - x0;
    oh = y1 - y0;
    out = malloc(ow * oh * sizeof *out);
    if (out == NULL) {
        return pc_io_nomem(err, "exporting an OpenRaster document");
    }
    for (yy = 0; yy < oh; yy++) {
        memcpy(out[yy * ow], p->px[(y0 + yy) * w + x0], ow * sizeof *out);
    }
    p->rect = pc_rect_make(p->rect.x0 + (int32_t)x0, p->rect.y0 + (int32_t)y0, p->rect.x0 + (int32_t)x1,
                           p->rect.y0 + (int32_t)y1);
    free(p->px);
    p->px = out;
    p->n = ow * oh;
    return PC_OK;
}

/* Straight RGBA floats encoded as an interleaved PNG at `sample` depth. */
static pc_status ora_png(uint32_t w, uint32_t h, const float (*px)[4], size_t n, pc_csample sample,
                         pc_buf *out, pc_error *err)
{
    size_t samples;
    size_t bpp = sample == PC_CS_U16 ? 2 : 1;
    size_t nbytes;
    uint8_t *data;
    size_t i;
    pc_cimage img;
    pc_cencode_opts opts;
    pc_status st;

    if (!pc_size_mul(n, 4, &samples) || !pc_size_mul(samples, bpp, &nbytes)) {
        return pc_io_ora_error(err, "layer image size overflow");
    }
    data = nbytes == 0 ? NULL : malloc(nbytes);
    if (nbytes != 0 && data == NULL) {
        return pc_io_nomem(err, "exporting an OpenRaster document");
    }
    for (i = 0; i < n && data != NULL; i++) {
        size_t c;
        for (c = 0; c < 4; c++) {
            float v = pc_clampf(px[i][c], 0.0f, 1.0f);
            if (sample == PC_CS_U16) {
                uint16_t u = pc_f32_to_u16(v * 65535.0f + 0.5f);
                data[(i * 4 + c) * 2] = (uint8_t)(u & 0xff);
                data[(i * 4 + c) * 2 + 1] = (uint8_t)(u >> 8);
            } else {
                data[i * 4 + c] = pc_f32_to_u8(v * 255.0f + 0.5f);
            }
        }
    }
    st = pc_cimage_from_raw(w, h, PC_CL_RGBA, sample, data, nbytes, &img, err);
    if (st != PC_OK) {
        return st; /* from_raw freed data */
    }
    opts = pc_cencode_opts_default();
    opts.embed_icc = false;
    st = pc_cencode(&img, PC_CF_PNG, &opts, out, err);
    pc_cimage_fini(&img);
    return st;
}

typedef struct {
    const pc_document *doc;
    pc_zip_writer zip;
    pc_buf xml;
    size_t next;
    pc_csample sample;
    ora_warnings warn;
} ora_writer;

/* The layer's own straight-RGBA pixels and their bounds, with its pixel mask applied. */
static pc_status ora_pixels(ora_writer *wr, const pc_layer *layer, ora_px *out, pc_error *err)
{
    pc_rect canvas = pc_rect_from_size(wr->doc->size);
    pc_compose_buffer buf;
    pc_rect rect;
    pc_status st;
    const pc_surface *surf = NULL;

    memset(out, 0, sizeof *out);
    if (layer->content.kind == PC_LAYER_RASTER) {
        surf = &layer->content.u.raster;
    }
    if (surf != NULL) {
        rect = pc_surface_content_bounds(surf);
        st = pc_compose_surface_to_buffer(surf, rect, &buf, err);
        if (st != PC_OK) {
            return st;
        }
    } else {
        /* Rendered as it shows on its own, before the layer's blending options. */
        pc_layer solo;
        pc_rect b;
        char buf2[256];
        st = pc_layer_clone(layer, &solo, err);
        if (st != PC_OK) {
            return st;
        }
        solo.visible = true;
        solo.opacity = 1.0f;
        solo.fill_opacity = 1.0f;
        solo.blend = PC_BLEND_NORMAL;
        solo.clipped = false;
        if (solo.has_mask) {
            pc_layer_mask_fini(&solo.mask);
            solo.has_mask = false;
        }
        pc_effects_fini(&solo.effects);
        memset(&solo.effects, 0, sizeof solo.effects);
        rect = canvas;
        if (pc_compose_composite_bounds(&solo, canvas, &b)) {
            rect = pc_rect_intersect(b, canvas);
        }
        (void)snprintf(buf2, sizeof buf2, "%s layers are saved to OpenRaster as pixels", "");
        {
            /* lowercase the content kind name */
            const char *kn = pc_layer_content_kind_name(&layer->content);
            char lower[64];
            size_t k;
            for (k = 0; kn[k] != '\0' && k + 1 < sizeof lower; k++) {
                char ch = kn[k];
                lower[k] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
            }
            lower[k] = '\0';
            (void)snprintf(buf2, sizeof buf2, "%s layers are saved to OpenRaster as pixels", lower);
        }
        st = ora_layer_warn(&wr->warn, pc_str_c(&layer->name), buf2, err);
        if (st != PC_OK) {
            pc_layer_fini(&solo);
            return st;
        }
        st = pc_compose_render_layer(&solo, rect, &buf, err);
        pc_layer_fini(&solo);
        if (st != PC_OK) {
            return st;
        }
    }
    /* Steal the buffer's pixels. */
    out->rect = rect;
    out->px = buf.px;
    out->n = buf.len;
    buf.px = NULL;
    buf.len = 0;
    pc_compose_buffer_fini(&buf);

    if (layer->has_mask && layer->mask.enabled) {
        float *values;
        uint32_t mw = pc_rect_width(rect);
        uint32_t mh = pc_rect_height(rect);
        size_t mn = (size_t)mw * mh;
        if (layer->mask.feather > 0.0f) {
            st = ora_layer_warn(&wr->warn, pc_str_c(&layer->name),
                                "layer mask feathering is not applied in OpenRaster", err);
            if (st != PC_OK) {
                return st;
            }
        }
        st = ora_layer_warn(&wr->warn, pc_str_c(&layer->name),
                            "layer masks were applied to the layer pixels", err);
        if (st != PC_OK) {
            return st;
        }
        values = mn == 0 ? NULL : malloc(mn * sizeof *values);
        if (mn != 0 && values == NULL) {
            return pc_io_nomem(err, "exporting an OpenRaster document");
        }
        st = pc_layer_mask_values_into(&layer->mask, rect, values, mn, err);
        if (st != PC_OK) {
            free(values);
            return st;
        }
        if (mn == out->n) {
            size_t i;
            for (i = 0; i < out->n; i++) {
                out->px[i][3] *= values[i];
            }
        }
        free(values);
    }
    if (layer->fill_opacity < 1.0f) {
        float fo = pc_clampf(layer->fill_opacity, 0.0f, 1.0f);
        size_t i;
        for (i = 0; i < out->n; i++) {
            out->px[i][3] *= fo;
        }
    }
    return ora_crop(out, err);
}

/* The shared stack.xml attributes for a layer or group. */
static pc_status ora_common(ora_writer *wr, const pc_layer *layer, pc_buf *attrs, pc_error *err)
{
    bool is_group = layer->content.kind == PC_LAYER_GROUP;
    float opacity = is_group ? pc_clampf(layer->opacity * layer->fill_opacity, 0.0f, 1.0f)
                             : pc_clampf(layer->opacity, 0.0f, 1.0f);
    char num[32];
    const char *op;
    char kbuf[32];
    pc_status st;

    st = pc_buf_append_str(attrs, "name=\"", err);
    if (st == PC_OK) {
        st = ora_escape(pc_str_c(&layer->name), attrs, err);
    }
    if (st == PC_OK) {
        st = pc_buf_appendf(attrs, err, "\" visibility=\"%s\" opacity=\"",
                            layer->visible ? "visible" : "hidden");
    }
    if (st != PC_OK) {
        return st;
    }
    ora_fmt_f32(opacity, num, sizeof num);
    st = pc_buf_appendf(attrs, err, "%s\"", num);
    if (st != PC_OK) {
        return st;
    }
    if (!ora_op_name(layer->blend, &op, kbuf, sizeof kbuf)) {
        char buf[256];
        (void)snprintf(buf, sizeof buf,
                       "the %s blend mode has no OpenRaster equivalent; it was saved as Normal",
                       pc_blend_label(layer->blend));
        st = ora_layer_warn(&wr->warn, pc_str_c(&layer->name), buf, err);
        if (st != PC_OK) {
            return st;
        }
        op = "svg:src-over";
    }
    st = pc_buf_appendf(attrs, err, " composite-op=\"%s\"", op);
    if (st != PC_OK) {
        return st;
    }
    if (layer->locks.all || layer->locks.pixels) {
        st = pc_buf_append_str(attrs, " edit-locked=\"true\"", err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (layer->locks.transparency) {
        st = pc_buf_append_str(attrs, " alpha-preserve=\"true\"", err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (layer->clipped) {
        st = ora_layer_warn(&wr->warn, pc_str_c(&layer->name),
                            "OpenRaster has no clipping masks; these layers were saved unclipped", err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (layer->effects.nitems > 0 || layer->effects.psd_raw != NULL) {
        st = ora_layer_warn(&wr->warn, pc_str_c(&layer->name),
                            "layer styles are not saved to OpenRaster", err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (layer->has_vector_mask && layer->vector_mask.enabled) {
        st = ora_layer_warn(&wr->warn, pc_str_c(&layer->name),
                            "vector masks are not saved to OpenRaster", err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (layer->fill_opacity < 1.0f) {
        st = ora_layer_warn(&wr->warn, pc_str_c(&layer->name),
                            is_group ? "fill opacity was combined into the layer opacity"
                                     : "fill opacity was applied to the saved layer pixels",
                            err);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

static pc_status ora_write_layers(ora_writer *wr, const pc_layer *layers, size_t n, size_t depth,
                                  pc_error *err);

static pc_status ora_write_one(ora_writer *wr, const pc_layer *layer, size_t depth, pc_error *err)
{
    pc_buf attrs = PC_BUF_INIT;
    pc_status st;

    if (layer->content.kind == PC_LAYER_GROUP) {
        const char *iso = layer->blend == PC_BLEND_PASS_THROUGH ? "auto" : "isolate";
        st = ora_common(wr, layer, &attrs, err);
        if (st == PC_OK && layer->has_mask && layer->mask.enabled) {
            st = ora_layer_warn(&wr->warn, pc_str_c(&layer->name),
                                "group masks are not saved to OpenRaster", err);
        }
        if (st == PC_OK) {
            st = pc_buf_appendf(&wr->xml, err, "<stack %s isolation=\"%s\">", pc_buf_cstr(&attrs), iso);
        }
        pc_buf_fini(&attrs);
        if (st != PC_OK) {
            return st;
        }
        st = ora_write_layers(wr, layer->content.u.group.children, layer->content.u.group.nchildren,
                              depth + 1, err);
        if (st != PC_OK) {
            return st;
        }
        return pc_buf_append_str(&wr->xml, "</stack>", err);
    }
    if (layer->content.kind == PC_LAYER_ADJUSTMENT) {
        return ora_layer_warn(&wr->warn, pc_str_c(&layer->name),
                              "adjustment layers cannot be saved to OpenRaster and were left out (the "
                              "merged image keeps their look)",
                              err);
    }
    {
        ora_px px;
        pc_buf png = PC_BUF_INIT;
        char src[32];
        uint32_t rw;
        uint32_t rh;
        st = ora_common(wr, layer, &attrs, err);
        if (st != PC_OK) {
            pc_buf_fini(&attrs);
            return st;
        }
        st = ora_pixels(wr, layer, &px, err);
        if (st != PC_OK) {
            pc_buf_fini(&attrs);
            return st;
        }
        if (pc_rect_is_empty(px.rect)) {
            /* Give an empty layer a 1x1 transparent pixel at the origin. */
            ora_px_fini(&px);
            px.px = malloc(sizeof *px.px);
            if (px.px == NULL) {
                pc_buf_fini(&attrs);
                return pc_io_nomem(err, "exporting an OpenRaster document");
            }
            px.px[0][0] = 0.0f;
            px.px[0][1] = 0.0f;
            px.px[0][2] = 0.0f;
            px.px[0][3] = 0.0f;
            px.n = 1;
            px.rect = pc_rect_from_xywh(0, 0, 1, 1);
        }
        rw = pc_rect_width(px.rect);
        rh = pc_rect_height(px.rect);
        (void)snprintf(src, sizeof src, "data/layer%zu.png", wr->next);
        wr->next++;
        st = ora_png(rw, rh, (const float (*)[4])px.px, px.n, wr->sample, &png, err);
        if (st == PC_OK) {
            st = pc_zip_writer_add(&wr->zip, src, strlen(src), png.data, png.len, err);
        }
        if (st == PC_OK) {
            st = pc_buf_appendf(&wr->xml, err, "<layer %s src=\"%s\" x=\"%d\" y=\"%d\"/>",
                                pc_buf_cstr(&attrs), src, px.rect.x0, px.rect.y0);
        }
        pc_buf_fini(&png);
        ora_px_fini(&px);
        pc_buf_fini(&attrs);
        return st;
    }
}

static pc_status ora_write_layers(ora_writer *wr, const pc_layer *layers, size_t n, size_t depth,
                                  pc_error *err)
{
    size_t i;
    pc_status st;
    if (depth > PC_MAX_GROUP_DEPTH) {
        return pc_io_unsupported(err, "layer groups nested deeper than %d", PC_MAX_GROUP_DEPTH);
    }
    /* stack.xml lists the topmost layer first. */
    for (i = 0; i < n; i++) {
        st = ora_write_one(wr, &layers[n - 1 - i], depth, err);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

pc_status pc_io_ora_export(const pc_document *doc, pc_io_export_result *out, pc_error *err)
{
    ora_writer wr;
    pc_io_export_result res;
    pc_csample sample;
    float dpi;
    char dpibuf[32];
    pc_buf stack = PC_BUF_INIT;
    pc_compose_buffer merged;
    bool merged_open = false;
    pc_buf mergedpng = PC_BUF_INIT;
    pc_rgba8_image thumb;
    bool thumb_open = false;
    pc_cimage thumbimg;
    bool thumbimg_open = false;
    pc_buf thumbpng = PC_BUF_INIT;
    pc_buf archive = PC_BUF_INIT;
    pc_status st;

    memset(&res, 0, sizeof res);
    pc_io_warnings_init(&res.warnings);
    memset(&wr, 0, sizeof wr);
    wr.doc = doc;
    ora_warnings_init(&wr.warn);
    pc_zip_writer_init(&wr.zip);

    if (doc->mode != PC_COLOR_MODE_RGB && doc->mode != PC_COLOR_MODE_GRAYSCALE) {
        st = pc_io_unsupported(
            err, "OpenRaster holds RGB layers; convert the document to RGB or Grayscale first");
        goto done;
    }
    sample = doc->depth == PC_SAMPLE_TYPE_U8 ? PC_CS_U8 : PC_CS_U16;
    wr.sample = sample;

    st = pc_zip_writer_add(&wr.zip, "mimetype", 8, (const uint8_t *)ORA_MIMETYPE, ORA_MIMETYPE_LEN, err);
    if (st != PC_OK) {
        goto done;
    }
    if (doc->depth == PC_SAMPLE_TYPE_F32) {
        st = ora_note(&wr.warn, "32-bit float layers are saved as 16-bit PNGs", err);
        if (st != PC_OK) {
            goto done;
        }
    }
    dpi = fmaxf(roundf(doc->resolution_dpi), 1.0f);
    (void)snprintf(dpibuf, sizeof dpibuf, "%.0f", (double)dpi);
    st = pc_buf_appendf(&wr.xml, err,
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<image version=\"0.0.6\" w=\"%u\" "
                        "h=\"%u\" xres=\"%s\" yres=\"%s\"><stack>",
                        doc->size.width, doc->size.height, dpibuf, dpibuf);
    if (st != PC_OK) {
        goto done;
    }
    st = ora_write_layers(&wr, doc->layers, doc->nlayers, 1, err);
    if (st != PC_OK) {
        goto done;
    }
    st = pc_buf_append_str(&wr.xml, "</stack></image>\n", err);
    if (st != PC_OK) {
        goto done;
    }
    st = pc_zip_writer_add(&wr.zip, "stack.xml", 9, wr.xml.data, wr.xml.len, err);
    if (st != PC_OK) {
        goto done;
    }
    /* mergedimage.png over the full canvas. */
    st = pc_compose_flatten(doc, &merged, err);
    if (st != PC_OK) {
        goto done;
    }
    merged_open = true;
    st = ora_png(doc->size.width, doc->size.height, (const float (*)[4])merged.px, merged.len, sample,
                 &mergedpng, err);
    if (st != PC_OK) {
        goto done;
    }
    st = pc_zip_writer_add(&wr.zip, "mergedimage.png", 15, mergedpng.data, mergedpng.len, err);
    if (st != PC_OK) {
        goto done;
    }
    /* Thumbnails/thumbnail.png. */
    st = pc_compose_thumbnail(doc, ORA_THUMBNAIL_SIDE, &thumb, err);
    if (st != PC_OK) {
        goto done;
    }
    thumb_open = true;
    {
        size_t tlen = (size_t)thumb.width * thumb.height * 4;
        uint8_t *tpx = thumb.pixels;
        thumb.pixels = NULL; /* ownership moves to from_raw */
        st = pc_cimage_from_raw(thumb.width, thumb.height, PC_CL_RGBA, PC_CS_U8, tpx, tlen, &thumbimg,
                                err);
        if (st != PC_OK) {
            goto done; /* from_raw freed tpx */
        }
        thumbimg_open = true;
    }
    {
        pc_cencode_opts opts = pc_cencode_opts_default();
        st = pc_cencode(&thumbimg, PC_CF_PNG, &opts, &thumbpng, err);
        if (st != PC_OK) {
            goto done;
        }
    }
    st = pc_zip_writer_add(&wr.zip, "Thumbnails/thumbnail.png", 24, thumbpng.data, thumbpng.len, err);
    if (st != PC_OK) {
        goto done;
    }
    /* Fidelity warnings for things ORA cannot carry. */
    if (doc->nchannels > 0) {
        st = ora_note(&wr.warn, "alpha channels are not saved to OpenRaster", err);
        if (st != PC_OK) {
            goto done;
        }
    }
    if (doc->metadata.ntext > 0 || doc->metadata.has_xmp || doc->metadata.exif != NULL) {
        st = ora_note(&wr.warn, "metadata (XMP, EXIF, text) is not saved to OpenRaster", err);
        if (st != PC_OK) {
            goto done;
        }
    }
    if (doc->icc_profile != NULL) {
        st = ora_note(&wr.warn, "the colour profile is not saved to OpenRaster; readers assume sRGB",
                      err);
        if (st != PC_OK) {
            goto done;
        }
    }
    if (doc->mode == PC_COLOR_MODE_GRAYSCALE) {
        st = ora_note(&wr.warn, "grayscale layers are saved as RGB", err);
        if (st != PC_OK) {
            goto done;
        }
    }
    st = pc_zip_writer_finish(&wr.zip, &archive, err);
    if (st != PC_OK) {
        goto done;
    }
    res.bytes = pc_buf_take(&archive, &res.len);
    st = ora_warnings_finish(&wr.warn, &res.warnings, err);
    if (st != PC_OK) {
        goto done;
    }
    *out = res;
    memset(&res, 0, sizeof res);
done:
    (void)stack;
    pc_buf_fini(&archive);
    pc_buf_fini(&thumbpng);
    if (thumbimg_open) {
        pc_cimage_fini(&thumbimg);
    }
    if (thumb_open) {
        pc_rgba8_image_fini(&thumb);
    }
    pc_buf_fini(&mergedpng);
    if (merged_open) {
        pc_compose_buffer_fini(&merged);
    }
    pc_buf_fini(&wr.xml);
    pc_zip_writer_fini(&wr.zip);
    ora_warnings_fini(&wr.warn);
    pc_io_export_result_fini(&res);
    return st;
}
