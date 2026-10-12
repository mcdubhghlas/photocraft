/* The named documents of rustref io-core-vectors (export_docs, multichannel_docs, the estimate and
 * layered-TIFF extras), built the same way here. */
#ifndef PC_TESTS_IO_DOCS_H
#define PC_TESTS_IO_DOCS_H

#include "common.h"

static inline pc_alpha_channel t_channel(const char *name, pc_surface s)
{
    pc_alpha_channel c;
    must(pc_alpha_channel_init(&c, name, &s, NULL));
    return c;
}

static inline void t_add_channel(pc_document *d, pc_alpha_channel c)
{
    pc_alpha_channel *v = realloc(d->channels, (d->nchannels + 1) * sizeof *v);
    if (v == NULL) {
        abort();
    }
    d->channels = v;
    v[d->nchannels++] = c;
}

static inline bool parse_md(const char *mode, const char *depth, pc_color_mode *m, pc_sample_type *s)
{
    return pc_color_mode_from_name(mode, strlen(mode), m) &&
           pc_sample_type_from_name(depth, strlen(depth), s);
}

/* multichannel_docs(). */
static inline bool t_multichannel_doc(const char *name, pc_document *out)
{
    pc_sample_type depth;
    if (strncmp(name, "mc-", 3) == 0 && pc_sample_type_from_name(name + 3, strlen(name + 3), &depth)) {
        pc_document d = t_doc("mc", 7, 5, PC_COLOR_MODE_MULTICHANNEL, depth);
        pc_pixel_format fmt = {PC_COLOR_MODE_GRAYSCALE, depth, false};
        pc_alpha_channel a = t_channel("Cyan", t_pattern(fmt, pc_rect_make(0, 0, 7, 5), 4, false));
        pc_alpha_channel b = t_channel("", t_pattern(fmt, pc_rect_make(1, 1, 5, 4), 9, false));
        pc_surface empty;
        pc_alpha_channel c;
        a.has_spot = true;
        a.spot_color = pc_color_rgb(0.0f, 0.6f, 1.0f);
        a.spot_solidity = 0.9f;
        b.has_spot = true;
        b.spot_color = PC_COLOR_BLACK;
        b.spot_solidity = 0.0f;
        pc_surface_init(&empty, fmt);
        c = t_channel("Tint \xC3\xA9\xF0\x9F\x98\x80", empty);
        t_add_channel(&d, a);
        t_add_channel(&d, b);
        t_add_channel(&d, c);
        d.resolution_dpi = 150.0f;
        t_set_xmp(&d, "<x/>");
        *out = d;
        return true;
    }
    if (strcmp(name, "mc-empty") == 0) {
        pc_document d = t_doc("mc0", 3, 2, PC_COLOR_MODE_MULTICHANNEL, PC_SAMPLE_TYPE_U8);
        pc_layer l;
        must(pc_layer_raster(&l, "stray", PC_PIXEL_FORMAT_RGBA8, NULL));
        t_push(&d, &l);
        *out = d;
        return true;
    }
    if (strcmp(name, "mc-60") == 0) {
        pc_document d = t_doc("mc60", 2, 2, PC_COLOR_MODE_MULTICHANNEL, PC_SAMPLE_TYPE_U8);
        for (int i = 0; i < 60; i++) {
            char nm[16];
            pc_surface s;
            pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
            (void)snprintf(nm, sizeof nm, "C%d", i);
            t_add_channel(&d, t_channel(nm, s));
        }
        *out = d;
        return true;
    }
    return false;
}

/* export_docs() by name, plus the estimate extras. */
static inline bool t_export_doc(const char *name, pc_document *out)
{
    char a[32], b[32];
    int alpha = 0;
    pc_color_mode m;
    pc_sample_type s;
    if (sscanf(name, "single-%31[A-Za-z]-%31[A-Z0-9]-%d", a, b, &alpha) == 3 && parse_md(a, b, &m, &s)) {
        *out = t_single(m, s, alpha != 0);
        return true;
    }
    if (sscanf(name, "gen-%31[A-Za-z]-%31[A-Z0-9]", a, b) == 2 && parse_md(a, b, &m, &s)) {
        *out = t_gen_doc(m, s);
        return true;
    }
    {
        static const float rgb[12] = {0, 0, 0, 0, 0, 0, 1, 0.5f, 0, 0.63f, 0, 1};
        static const float gray[4] = {0, 0, 0, 0.5f};
        static const float cmyk[10] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 0.5f};
        static const float tagged[12] = {0, 0.05f, 0, 1, 0.2f, 0.5f, 0.8f, 1, 1, 0.25f, 0, 1};
        if (strcmp(name, "cols-rgb8") == 0 || strcmp(name, "cols-rgb16") == 0 ||
            strcmp(name, "cols-rgbf") == 0) {
            pc_sample_type st = name[8] == '8'   ? PC_SAMPLE_TYPE_U8
                                : name[8] == '1' ? PC_SAMPLE_TYPE_U16
                                                 : PC_SAMPLE_TYPE_F32;
            *out = t_columns(PC_COLOR_MODE_RGB, st, rgb, 3, 4);
            return true;
        }
        if (strcmp(name, "cols-gray") == 0) {
            *out = t_columns(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, gray, 2, 2);
            return true;
        }
        if (strcmp(name, "cols-cmyk") == 0) {
            *out = t_columns(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, cmyk, 2, 5);
            return true;
        }
        if (strncmp(name, "icc-", 4) == 0 && strcmp(name, "icc-cmyk") != 0 && strcmp(name, "icc-gray") != 0) {
            pc_builtin bi = strncmp(name + 4, "LinearSrgb", 10) == 0 ? PC_BUILTIN_LINEAR_SRGB
                            : strncmp(name + 4, "DisplayP3", 9) == 0 ? PC_BUILTIN_DISPLAY_P3
                                                                     : PC_BUILTIN_SRGB;
            const char *dep = strrchr(name, '-') + 1;
            if (!pc_sample_type_from_name(dep, strlen(dep), &s)) {
                return false;
            }
            *out = t_columns(PC_COLOR_MODE_RGB, s, tagged, 3, 4);
            t_set_icc(out, bi);
            return true;
        }
    }
    if (strcmp(name, "icc-cmyk") == 0) {
        *out = t_single(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, false);
        t_set_icc(out, PC_BUILTIN_COATED_CMYK);
        return true;
    }
    if (strcmp(name, "icc-gray") == 0) {
        *out = t_single(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false);
        t_set_icc(out, PC_BUILTIN_GRAY_GAMMA22);
        return true;
    }
    if (strcmp(name, "meta") == 0) {
        uint8_t ex[26];
        size_t n = t_exif(6, ex);
        *out = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
        out->resolution_dpi = 300.0f;
        t_set_xmp(
            out, "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:Description tiff:Orientation=\"6\"/></x:xmpmeta>");
        must(pc_io_set_blob(&out->metadata.exif, ex, n, NULL));
        return true;
    }
    if (strcmp(name, "indexed") == 0 || strcmp(name, "indexed-t") == 0) {
        bool t = name[7] == '-';
        pc_document d;
        if (t) {
            d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        } else {
            must(pc_document_with_background(&d, "m", pc_size_make(4, 2), PC_COLOR_MODE_RGB,
                                             PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 0, 0, 1), NULL));
        }
        d.mode = PC_COLOR_MODE_INDEXED;
        d.has_color_table = true;
        d.color_table.ncolors = t ? 4 : 2;
        d.color_table.colors = xmalloc(4 * sizeof *d.color_table.colors);
        memcpy(d.color_table.colors,
               t ? (const uint8_t[4][3]){{0, 0, 0}, {255, 255, 255}, {200, 30, 30}, {30, 200, 30}}
                 : (const uint8_t[4][3]){{0, 0, 0}, {255, 0, 0}},
               4 * 3);
        d.color_table.has_transparent = t;
        d.color_table.transparent = 1;
        *out = d;
        return true;
    }
    if (strcmp(name, "duotone") == 0) {
        static const float black[3] = {0, 0, 0};
        static const float orange[3] = {1, 0.5f, 0};
        pc_document d;
        must(pc_document_with_background(&d, "m", pc_size_make(4, 2), PC_COLOR_MODE_GRAYSCALE,
                                         PC_SAMPLE_TYPE_U8, pc_color_rgba(0.5f, 0.5f, 0.5f, 1), NULL));
        d.mode = PC_COLOR_MODE_DUOTONE;
        d.has_duotone = true;
        d.duotone.inks = xmalloc(2 * sizeof *d.duotone.inks);
        must(pc_duotone_ink_new("Black", black, &d.duotone.inks[0], NULL));
        must(pc_duotone_ink_new("Orange", orange, &d.duotone.inks[1], NULL));
        d.duotone.ninks = 2;
        *out = d;
        return true;
    }
    if (strcmp(name, "half") == 0) {
        *out = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true);
        out->layers[0].opacity = 0.5f;
        return true;
    }
    if (strcmp(name, "empty") == 0) {
        *out = t_doc("e", 5, 3, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        return true;
    }
    if (strcmp(name, "blocks") == 0) {
        pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        uint8_t data[77];
        pc_doc_psd_block *pb = xmalloc(sizeof *pb);
        pc_surface qm;
        memset(data, 1, sizeof data);
        memcpy(pb->key, "abcd", 4);
        pb->data = NULL;
        must(pc_blob_new(data, sizeof data, &pb->data, NULL));
        d.layers[1].psd_blocks = pb;
        d.layers[1].npsd_blocks = 1;
        d.metadata.psd_resources = xmalloc(sizeof *d.metadata.psd_resources);
        memset(d.metadata.psd_resources, 0, sizeof *d.metadata.psd_resources);
        d.metadata.psd_resources[0].id = 4000;
        must(pc_str_set_c(&d.metadata.psd_resources[0].name, "nm", NULL));
        must(pc_blob_new((const uint8_t[3]){9, 9, 9}, 3, &d.metadata.psd_resources[0].data, NULL));
        d.metadata.npsd_resources = 1;
        d.metadata.psd_global_blocks = xmalloc(sizeof *d.metadata.psd_global_blocks);
        memcpy(d.metadata.psd_global_blocks[0].signature, "8BIM", 4);
        memcpy(d.metadata.psd_global_blocks[0].key, "Patt", 4);
        d.metadata.psd_global_blocks[0].data = NULL;
        must(pc_blob_new((const uint8_t[13]){0}, 13, &d.metadata.psd_global_blocks[0].data, NULL));
        d.metadata.npsd_global_blocks = 1;
        t_add_channel(
            &d, t_channel("Alpha", t_mask_surface(PC_SAMPLE_TYPE_U8, pc_rect_make(0, 0, 24, 16), 0, 3)));
        pc_surface_init(&qm, PC_PIXEL_FORMAT_GRAY8);
        must(pc_alpha_channel_init(&d.quick_mask, "Quick Mask", &qm, NULL));
        d.has_quick_mask = true;
        *out = d;
        return true;
    }
    if (strcmp(name, "huge") == 0) {
        *out = t_doc("h", 4000000000u, 4000000000u, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32);
        return true;
    }
    if (strcmp(name, "big") == 0) {
        *out = t_doc("b", 30001, 2, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        return true;
    }
    if (strcmp(name, "2g") == 0) {
        *out = t_doc("g", 30000, 30000, PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U16);
        return true;
    }
    return t_multichannel_doc(name, out);
}

/* The layered-TIFF documents of tiff_layers(). */
static inline bool t_tiff_doc(const char *name, pc_document *out)
{
    if (strcmp(name, "gen8") == 0) {
        *out = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    } else if (strcmp(name, "gen16") == 0) {
        *out = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16);
    } else if (strcmp(name, "genf") == 0) {
        *out = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32);
    } else if (strcmp(name, "gray") == 0) {
        *out = t_gen_doc(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8);
    } else if (strcmp(name, "cmyk") == 0) {
        *out = t_gen_doc(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U16);
    } else if (strcmp(name, "lab") == 0) {
        *out = t_gen_doc(PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U8);
    } else if (strcmp(name, "single") == 0) {
        *out = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    } else if (strcmp(name, "mc") == 0) {
        return t_multichannel_doc("mc-U8", out);
    } else if (strcmp(name, "translucent") == 0) {
        pc_document d = t_doc("t", 4, 4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        pc_surface s;
        pc_layer l;
        float px[64];
        for (int i = 0; i < 16; i++) {
            px[i * 4] = 1;
            px[i * 4 + 1] = 0;
            px[i * 4 + 2] = 0;
            px[i * 4 + 3] = 0.5f;
        }
        pc_surface_init(&s, pc_document_pixel_format(&d));
        must(pc_surface_write_region(&s, pc_rect_make(0, 0, 4, 4), px, 64, NULL));
        must(pc_io_raster_layer("Red", &s, &l, NULL));
        pc_surface_fini(&s);
        t_push(&d, &l);
        *out = d;
    } else {
        return false;
    }
    return true;
}

#endif
