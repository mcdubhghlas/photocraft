/* PSD export, layer records (port of the Ex half of crates/io/src/psd_export.rs): pixel and
 * mask channels, the tagged blocks of every layer kind (preserved raw blocks reused while they
 * still decode to the layer, regenerated otherwise), and the record order Photoshop expects (a
 * group's bounding divider, its children, then the group). The file itself (merged image,
 * resources, global blocks) is assembled in psd_export_file.c; smart objects in
 * psd_export_smart.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/utf8.h"
#include "compose/compose.h"
#include "compose/gradient_fill.h"
#include "io/adjust_map.h"
#include "io/blocks.h"
#include "io/comps_map.h"
#include "io/effects_map.h"
#include "io/pixels.h"
#include "io/psd_export_internal.h"
#include "io/psd_import.h"
#include "io/text_styles_map.h"
#include "io/vector_map.h"
#include "raster/pixel.h"
#include "text/psd.h"

/* Deepest group nesting exported (the compositor's limit; legal documents stop at 100). */
#define MAX_EMIT_DEPTH 128

/* ---- small helpers ---- */

uint8_t pc_io_q255(float v)
{
    float c = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; /* NaN stays NaN and casts to 0 */
    return pc_f32_to_u8(roundf(c * 255.0f));
}

/* legacy_name: printable ASCII kept, every other char '?', at most 255. */
pc_status pc_io_legacy_name(const char *s, size_t len, pc_psd_bytes *out, pc_error *err)
{
    uint8_t b[255];
    size_t n = 0;
    size_t i = 0;
    while (i < len && n < 255) {
        uint32_t cp;
        if (pc_utf8_next((const uint8_t *)s, len, &i, &cp)) {
            b[n++] = cp < 0x80 && cp >= 0x20 && cp != 0x7F ? (uint8_t)cp : (uint8_t)'?';
        } else {
            /* Not UTF-8 (a Rust String can't hold it): one '?' per byte. */
            b[n++] = '?';
            i++;
        }
    }
    out->data = NULL;
    out->len = 0;
    return pc_psd_bytes_set(out, b, n, err);
}

/* TaggedBlock::unicode_name for a length-carrying name: UTF-16 count + units, padded to 4. */
static pc_status unicode_name_block(const pc_str *name, pc_psd_block *out, pc_error *err)
{
    pc_psd_ustr u = {NULL, 0};
    pc_buf b = PC_BUF_INIT;
    uint8_t tmp[4];
    pc_status st = pc_psd_ustr_from_utf8n(pc_str_c(name), name->len, false, &u, err);
    if (st != PC_OK) {
        return st;
    }
    pc_store_be32(tmp, (uint32_t)u.n);
    st = pc_buf_append(&b, tmp, 4, err);
    for (size_t i = 0; i < u.n && st == PC_OK; i++) {
        pc_store_be16(tmp, u.u[i]);
        st = pc_buf_append(&b, tmp, 2, err);
    }
    while (st == PC_OK && b.len % 4 != 0) {
        st = pc_buf_append_byte(&b, 0, err);
    }
    pc_psd_ustr_fini(&u);
    if (st == PC_OK) {
        st = pc_psd_block_init(out, "luni", b.data, b.len, err);
    }
    pc_buf_fini(&b);
    return st;
}

/* A raw channel holding plane as it is (the last resort of Ex::encode). */
static pc_status raw_channel(int16_t id, const uint8_t *plane, size_t len, pc_psd_channel *out, pc_error *err)
{
    pc_psd_channel c;
    memset(&c, 0, sizeof c);
    c.id = id;
    c.has_compression = true;
    c.compression = PC_PSD_RAW;
    if (pc_psd_bytes_set(&c.data, plane, len, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    *out = c;
    return PC_OK;
}

/* Ex::encode: Photoshop writes empty layer/mask channels as just the raw compression code (a
 * ZIP stream for a zero-sized channel makes readers decompress an invalid extent). */
static pc_status encode_channel(const pc_io_ex *ex, int16_t id, const uint8_t *plane, size_t len, size_t w,
                                size_t h, pc_psd_channel *out, pc_error *err)
{
    uint16_t depth = pc_io_psd_depth(ex->fmt.sample);
    uint16_t comp = w == 0 || h == 0                       ? PC_PSD_RAW
                    : ex->fmt.sample == PC_SAMPLE_TYPE_F32 ? PC_PSD_ZIP_PREDICT
                                                           : PC_PSD_RLE;
    pc_status st = pc_psd_channel_encode(id, comp, plane, len, w, h, depth, ex->version, out, err);
    if (st == PC_OK || st == PC_ERR_NOMEM) {
        return st;
    }
    st = pc_psd_channel_encode(id, PC_PSD_RAW, plane, len, w, h, depth, ex->version, out, err);
    if (st == PC_OK || st == PC_ERR_NOMEM) {
        return st;
    }
    return raw_channel(id, plane, len, out, err);
}

static void channels_free(pc_psd_channel *c, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pc_psd_bytes_fini(&c[i].data);
    }
    free(c);
}

/* Appends cc + 1 empty channels (-1, 0, .., cc - 1). */
static pc_status empty_channels(const pc_io_ex *ex, pc_psd_channel **out, size_t *n, pc_error *err)
{
    pc_psd_channel *c = pc_calloc_array(ex->cc + 1, sizeof *c);
    pc_status st = PC_OK;
    if (c == NULL) {
        return pc_io_nomem(err, "writing layer channels");
    }
    for (size_t i = 0; i <= ex->cc && st == PC_OK; i++) {
        st = encode_channel(ex, (int16_t)((int)i - 1), NULL, 0, 0, 0, &c[i], err);
    }
    if (st != PC_OK) {
        channels_free(c, ex->cc + 1);
        return st;
    }
    *out = c;
    *n = ex->cc + 1;
    return PC_OK;
}

static const char *fmt_debug(pc_pixel_format f, char *buf, size_t cap)
{
    (void)snprintf(buf, cap, "PixelFormat { mode: %s, sample: %s, alpha: %s }", pc_color_mode_name(f.mode),
                   pc_sample_type_name(f.sample), f.alpha ? "true" : "false");
    return buf;
}

typedef struct enc_job {
    const pc_io_ex *ex;
    const uint8_t *planes;
    size_t plane_len;
    size_t w;
    size_t h;
    pc_psd_channel *out; /* alpha first, then the colour channels */
    pc_error *errs;
} enc_job;

static pc_status enc_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    enc_job *j = ctx;
    (void)chunk;
    for (size_t i = begin; i < end; i++) {
        size_t plane = i == 0 ? j->ex->cc : i - 1;
        int16_t id = (int16_t)((int)i - 1);
        pc_status st = encode_channel(j->ex, id, j->planes + plane * j->plane_len, j->plane_len, j->w, j->h,
                                      &j->out[i], &j->errs[i]);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

/* Ex::pixel_channels: the content's rect and channels (alpha first), each compressed on the
 * pool. */
static pc_status pixel_channels(pc_io_ex *ex, const pc_surface *src, const pc_str *name, pc_psd_rect *rect,
                                pc_psd_channel **out, size_t *n, pc_error *err)
{
    pc_surface conv;
    const pc_surface *s = src;
    bool owned = false;
    pc_rect r;
    uint8_t *bytes = NULL;
    size_t blen = 0;
    uint8_t *planes = NULL;
    size_t plane_len = 0;
    bool invert[PC_RASTER_MAX_CHANNELS + 1];
    pc_psd_channel *ch;
    pc_error *errs;
    enc_job job;
    pc_status st = PC_OK;
    if (!pc_pixel_format_eq(pc_surface_format(src), ex->fmt)) {
        char a[96];
        char b[96];
        st = pc_io_warn(&ex->warnings, err, "layer \"%s\": pixels converted from %s to %s", pc_str_c(name),
                        fmt_debug(pc_surface_format(src), a, sizeof a), fmt_debug(ex->fmt, b, sizeof b));
        if (st == PC_OK) {
            st = pc_surface_convert(src, ex->fmt, &conv, err);
        }
        if (st != PC_OK) {
            return st;
        }
        s = &conv;
        owned = true;
    }
    r = pc_surface_content_bounds(s);
    if (pc_rect_is_empty(r)) {
        if (owned) {
            pc_surface_fini(&conv);
        }
        memset(rect, 0, sizeof *rect);
        return empty_channels(ex, out, n, err);
    }
    st = pc_surface_to_interleaved(s, r, &bytes, &blen, err);
    if (owned) {
        pc_surface_fini(&conv);
    }
    if (st != PC_OK) {
        return st;
    }
    if (ex->fmt.mode == PC_COLOR_MODE_LAB && ex->fmt.sample == PC_SAMPLE_TYPE_U16) {
        pc_io_lab16_chroma(bytes, blen, ex->cc + 1, false);
    }
    for (size_t c = 0; c < ex->cc && c < PC_RASTER_MAX_CHANNELS; c++) {
        invert[c] = ex->cmyk;
    }
    invert[ex->cc] = false;
    st = pc_io_deinterleave(bytes, blen, ex->cc + 1, ex->fmt.sample, invert, &planes, &plane_len, err);
    free(bytes);
    if (st != PC_OK) {
        return st;
    }
    ch = pc_calloc_array(ex->cc + 1, sizeof *ch);
    errs = pc_calloc_array(ex->cc + 1, sizeof *errs);
    if (ch == NULL || errs == NULL) {
        free(ch);
        free(errs);
        free(planes);
        return pc_io_nomem(err, "writing layer channels");
    }
    job.ex = ex;
    job.planes = planes;
    job.plane_len = plane_len;
    job.w = pc_rect_width(r);
    job.h = pc_rect_height(r);
    job.out = ch;
    job.errs = errs;
    st = pc_pool_for(pc_pool_global(), ex->cc + 1, 1, enc_chunk, &job);
    free(planes);
    if (st != PC_OK) {
        for (size_t i = 0; i <= ex->cc; i++) {
            if (errs[i].code != PC_OK && errs[i].msg[0] != '\0') {
                if (err != NULL) {
                    *err = errs[i];
                }
                break;
            }
        }
        free(errs);
        channels_free(ch, ex->cc + 1);
        return st;
    }
    free(errs);
    *rect = (pc_psd_rect){r.y0, r.x0, r.y1, r.x1};
    *out = ch;
    *n = ex->cc + 1;
    return PC_OK;
}

/* PSD mask parameters for a user mask's and a vector mask's (density, feather); *has false when
 * every value is the default (density 1, feather 0). */
static bool mask_parameters(const float *user, const float *vector, pc_psd_mask_params *out)
{
    pc_psd_mask_params p;
    memset(&p, 0, sizeof p);
    if (user != NULL && user[0] < 1.0f) {
        p.has_user_density = true;
        p.user_density = pc_io_q255(user[0]);
    }
    if (user != NULL && user[1] != 0.0f) {
        p.has_user_feather = true;
        p.user_feather = (double)user[1];
    }
    if (vector != NULL && vector[0] < 1.0f) {
        p.has_vector_density = true;
        p.vector_density = pc_io_q255(vector[0]);
    }
    if (vector != NULL && vector[1] != 0.0f) {
        p.has_vector_feather = true;
        p.vector_feather = (double)vector[1];
    }
    p.flags = (uint8_t)((p.has_user_density ? 1 : 0) | (p.has_user_feather ? 2 : 0) |
                        (p.has_vector_density ? 4 : 0) | (p.has_vector_feather ? 8 : 0));
    if (p.flags == 0) {
        return false;
    }
    *out = p;
    return true;
}

/* Ex::mask: the user mask record and its -2 channel. */
static pc_status mask_record(const pc_io_ex *ex, const pc_layer_mask *m, const float *vector,
                             pc_psd_mask_data *md, pc_psd_channel *ch, pc_error *err)
{
    pc_surface conv;
    const pc_surface *s = &m->surface;
    bool owned = false;
    float dp[PC_RASTER_MAX_CHANNELS];
    pc_rect r;
    uint8_t *plane = NULL;
    size_t plen = 0;
    uint8_t flags = 0;
    float user[2] = {m->density, m->feather};
    pc_psd_layer_mask pm;
    pc_status st = PC_OK;
    if (!pc_pixel_format_eq(pc_surface_format(s), ex->mask_fmt)) {
        st = pc_surface_convert(s, ex->mask_fmt, &conv, err);
        if (st != PC_OK) {
            return st;
        }
        s = &conv;
        owned = true;
    }
    pc_surface_default_pixel(s, dp);
    r = pc_surface_content_bounds(s);
    if (!pc_rect_is_empty(r)) {
        uint8_t *bytes = NULL;
        size_t blen = 0;
        bool inv = false;
        st = pc_surface_to_interleaved(s, r, &bytes, &blen, err);
        if (st == PC_OK) {
            st = pc_io_deinterleave(bytes, blen, 1, ex->mask_fmt.sample, &inv, &plane, &plen, err);
        }
        free(bytes);
    }
    if (owned) {
        pc_surface_fini(&conv);
    }
    if (st != PC_OK) {
        free(plane);
        return st;
    }
    if (!m->linked) {
        flags |= PC_PSD_MF_RELATIVE;
    }
    if (!m->enabled) {
        flags |= PC_PSD_MF_DISABLED;
    }
    memset(&pm, 0, sizeof pm);
    pm.has_params = mask_parameters(user, vector, &pm.params);
    if (pm.has_params) {
        flags |= PC_PSD_MF_PARAMETERS;
    }
    pm.rect = pc_rect_is_empty(r) ? (pc_psd_rect){0, 0, 0, 0} : (pc_psd_rect){r.y0, r.x0, r.y1, r.x1};
    pm.default_color = pc_io_q255(dp[0]);
    pm.flags = flags;
    pm.real_first = true;
    if (!pm.has_params) {
        static const uint8_t PAD[2] = {0, 0};
        st = pc_psd_bytes_set(&pm.trailing, PAD, 2, err);
    }
    if (st == PC_OK) {
        st = encode_channel(ex, PC_PSD_CH_USER_MASK, plane, plen, pc_rect_is_empty(r) ? 0 : pc_rect_width(r),
                            pc_rect_is_empty(r) ? 0 : pc_rect_height(r), ch, err);
    }
    free(plane);
    if (st != PC_OK) {
        pc_psd_layer_mask_fini(&pm);
        return st;
    }
    md->kind = PC_PSD_MASK_PARSED;
    md->mask = pm;
    return PC_OK;
}

/* Section-divider ids: above every layer id. */
static uint32_t fresh_id(pc_io_ex *ex)
{
    ex->next_id++;
    return ex->next_id;
}

static uint32_t layer_id(pc_io_ex *ex, const pc_layer *l)
{
    uint32_t id;
    return pc_io_ex_idmap_get(&ex->layer_ids, l->id, &id) ? id : fresh_id(ex);
}

/* Whether the layer's effects still equal what its preserved lmfx / lfx2 decodes to; *known
 * false when nothing is preserved or it can't be decoded. */
static pc_status effects_unchanged(const pc_layer *l, bool *known, bool *same, pc_error *err)
{
    const pc_blob *src = NULL;
    bool found = false;
    bool master = false;
    pc_effect *items = NULL;
    size_t n = 0;
    pc_status st;
    *known = false;
    *same = false;
    for (size_t i = 0; i < l->npsd_blocks && src == NULL; i++) {
        if (memcmp(l->psd_blocks[i].key, "lmfx", 4) == 0) {
            src = l->psd_blocks[i].data;
        }
    }
    if (src == NULL) {
        src = l->effects.psd_raw;
    }
    if (src == NULL) {
        return PC_OK;
    }
    st = pc_io_parse_lfx2(pc_blob_data(src), pc_blob_len(src), &found, &master, &items, &n, err);
    if (st != PC_OK || !found) {
        return st;
    }
    *known = true;
    *same = master == l->effects.enabled && n == l->effects.nitems;
    for (size_t i = 0; i < n; i++) {
        *same = *same && pc_effect_eq(&items[i], &l->effects.items[i]);
        pc_effect_fini(&items[i]);
    }
    free(items);
    return PC_OK;
}

static pc_status push_block(pc_psd_block **v, size_t *n, size_t *cap, pc_psd_block *b, pc_error *err)
{
    pc_psd_block *g = pc_grow_array(*v, cap, *n + 1, sizeof *g);
    if (g == NULL) {
        pc_psd_block_fini(b);
        return pc_io_nomem(err, "writing layer blocks");
    }
    *v = g;
    g[(*n)++] = *b;
    memset(b, 0, sizeof *b);
    return PC_OK;
}

/* lspf bits (Adobe spec: bit 0 transparency, 1 composite, 2 position; artboard bit 3 and "all"
 * bit 31 as reference files write them). */
static uint32_t lspf_from_locks(const pc_locks *l)
{
    return (l->transparency ? 1u : 0u) | (l->pixels ? 2u : 0u) | (l->position ? 4u : 0u) |
           (l->artboard ? 8u : 0u) | (l->all ? 0x80000000u : 0u);
}

/* The lclr index of a label (the enum is declared in lclr order). */
static uint16_t label_index(pc_label_color c)
{
    return (uint16_t)c;
}

/* Ex::record: the layer record of l with extra blocks (taken over) and its pixels (NULL = none). */
static pc_status record(pc_io_ex *ex, const pc_layer *l, pc_psd_block *extra, size_t nextra,
                        const pc_surface *pixels, pc_psd_layer *out, pc_error *err)
{
    pc_psd_layer rec;
    uint32_t id = layer_id(ex, l);
    float vector[2];
    const float *vp = NULL;
    pc_psd_block b;
    size_t cap = 0;
    bool fx_known = false;
    bool fx_same = false;
    pc_buf lfx2 = PC_BUF_INIT;
    bool has_lfx2 = false;
    uint32_t lspf = lspf_from_locks(&l->locks);
    pc_status st;
    pc_psd_layer_init(&rec);
    if (pixels != NULL) {
        st = pixel_channels(ex, pixels, &l->name, &rec.rect, &rec.channels, &rec.nchannels, err);
    } else {
        st = empty_channels(ex, &rec.channels, &rec.nchannels, err);
    }
    /* Vector mask density/feather live in the mask parameters, with or without a user mask. */
    if (l->has_vector_mask) {
        vector[0] = l->vector_mask.density;
        vector[1] = l->vector_mask.feather;
        vp = vector;
    }
    if (st == PC_OK && l->has_mask) {
        pc_psd_channel mc;
        st = mask_record(ex, &l->mask, vp, &rec.mask, &mc, err);
        if (st == PC_OK) {
            st = pc_psd_layer_push_channel(&rec, &mc, err);
        }
    } else if (st == PC_OK) {
        pc_psd_mask_params p;
        if (mask_parameters(NULL, vp, &p)) {
            /* Parameters only: no user mask channel, so nothing hides on re-import. */
            rec.mask.kind = PC_PSD_MASK_PARSED;
            memset(&rec.mask.mask, 0, sizeof rec.mask.mask);
            rec.mask.mask.default_color = 255;
            rec.mask.mask.flags = PC_PSD_MF_PARAMETERS;
            rec.mask.mask.has_params = true;
            rec.mask.mask.params = p;
            rec.mask.mask.real_first = true;
        }
    }
    rec.flags = (uint8_t)((l->visible ? 0 : PC_PSD_LF_HIDDEN) |
                          (l->locks.transparency ? PC_PSD_LF_TRANSPARENCY_PROTECTED : 0));
    if (st == PC_OK) {
        st = unicode_name_block(&l->name, &b, err);
        st = st == PC_OK ? push_block(&rec.blocks, &rec.nblocks, &cap, &b, err) : st;
    }
    if (st == PC_OK) {
        st = pc_psd_block_make_u32("lyid", id, &b, err);
        st = st == PC_OK ? push_block(&rec.blocks, &rec.nblocks, &cap, &b, err) : st;
    }
    if (st == PC_OK && lspf != 0) {
        st = pc_psd_block_make_u32("lspf", lspf, &b, err);
        st = st == PC_OK ? push_block(&rec.blocks, &rec.nblocks, &cap, &b, err) : st;
    }
    if (st == PC_OK && l->label != PC_LABEL_NONE) {
        st = pc_psd_block_make_sheet_color(label_index(l->label), &b, err);
        st = st == PC_OK ? push_block(&rec.blocks, &rec.nblocks, &cap, &b, err) : st;
    }
    if (st == PC_OK) {
        st = pc_psd_block_make_flag("iOpa", pc_io_q255(l->fill_opacity), &b, err);
        st = st == PC_OK ? push_block(&rec.blocks, &rec.nblocks, &cap, &b, err) : st;
    }
    /* A blend mode with no PSD equivalent is written as Normal; warn so it can be preserved by
     * saving as .pcraft. Effects first (highlight then shadow for a bevel), then the layer. */
    for (size_t i = 0; i < l->effects.nitems && st == PC_OK; i++) {
        const pc_effect *fx = &l->effects.items[i];
        pc_blend_mode modes[2];
        size_t nm = 1;
        switch (fx->kind) {
        case PC_FX_DROP_SHADOW:
        case PC_FX_INNER_SHADOW:
            modes[0] = fx->u.shadow.common.blend;
            break;
        case PC_FX_OUTER_GLOW:
        case PC_FX_INNER_GLOW:
            modes[0] = fx->u.glow.common.blend;
            break;
        case PC_FX_STROKE:
            modes[0] = fx->u.stroke.common.blend;
            break;
        case PC_FX_COLOR_OVERLAY:
            modes[0] = fx->u.color_overlay.common.blend;
            break;
        case PC_FX_GRADIENT_OVERLAY:
            modes[0] = fx->u.gradient_overlay.common.blend;
            break;
        case PC_FX_PATTERN_OVERLAY:
            modes[0] = fx->u.pattern_overlay.common.blend;
            break;
        case PC_FX_SATIN:
            modes[0] = fx->u.satin.common.blend;
            break;
        case PC_FX_BEVEL_EMBOSS:
            modes[0] = fx->u.bevel.highlight.blend;
            modes[1] = fx->u.bevel.shadow.blend;
            nm = 2;
            break;
        default:
            nm = 0;
            break;
        }
        for (size_t m = 0; m < nm && st == PC_OK; m++) {
            if (!pc_blend_has_psd_equivalent(modes[m])) {
                st = pc_io_warn(&ex->warnings, err,
                                "layer \"%s\", %s: %s has no PSD equivalent and was written as Normal; "
                                "save as .pcraft to preserve it",
                                pc_str_c(&l->name), pc_effect_label(fx), pc_blend_label(modes[m]));
            }
        }
    }
    /* Effects: the original lfx2 while the effects are unchanged (or could not be decoded at
     * all); otherwise regenerated. */
    if (st == PC_OK) {
        st = effects_unchanged(l, &fx_known, &fx_same, err);
    }
    if (st == PC_OK) {
        if ((!fx_known || fx_same) && l->effects.psd_raw != NULL) {
            st = pc_buf_append(&lfx2, pc_blob_data(l->effects.psd_raw), pc_blob_len(l->effects.psd_raw), err);
            has_lfx2 = true;
        } else if (l->effects.nitems > 0) {
            st = pc_io_write_lfx2(l->effects.enabled, l->effects.items, l->effects.nitems, &lfx2, err);
            has_lfx2 = true;
        }
    }
    if (st == PC_OK && has_lfx2) {
        st = pc_psd_block_init(&b, pc_layer_is_group(l) ? "lfxs" : "lfx2", lfx2.data, lfx2.len, err);
        st = st == PC_OK ? push_block(&rec.blocks, &rec.nblocks, &cap, &b, err) : st;
    }
    pc_buf_fini(&lfx2);
    for (size_t i = 0; i < nextra; i++) {
        if (st == PC_OK) {
            st = push_block(&rec.blocks, &rec.nblocks, &cap, &extra[i], err);
        } else {
            pc_psd_block_fini(&extra[i]);
        }
    }
    free(extra);
    /* Layer-level blocks keep their pad byte inside the length, as Photoshop writes them:
     * readers such as psd-tools do not skip a pad after an odd length (#200). */
    for (size_t i = 0; i < rec.nblocks && st == PC_OK; i++) {
        pc_psd_block *x = &rec.blocks[i];
        if (x->data.len % 2 == 1 && !x->padding.has) {
            uint8_t *d = pc_malloc(x->data.len + 1);
            if (d == NULL) {
                st = pc_io_nomem(err, "padding a layer block");
                break;
            }
            memcpy(d, x->data.data, x->data.len);
            d[x->data.len] = 0;
            free(x->data.data);
            x->data.data = d;
            x->data.len++;
        }
    }
    if (st == PC_OK && !pc_blend_has_psd_equivalent(l->blend)) {
        st = pc_io_warn(&ex->warnings, err,
                        "layer \"%s\": %s has no PSD equivalent and was written as Normal; save as "
                        ".pcraft to preserve it",
                        pc_str_c(&l->name), pc_blend_label(l->blend));
    }
    if (st == PC_OK) {
        memcpy(rec.blend, pc_blend_psd_key(l->blend), 4);
        rec.opacity = pc_io_q255(l->opacity);
        rec.clipping = l->clipped ? 1 : 0;
        rec.filler = 0;
        pc_psd_bytes_fini(&rec.blending_ranges);
        st = pc_io_ranges_from_blend_if(&l->blend_if, ex->cc, &rec.blending_ranges, err);
    }
    if (st == PC_OK) {
        pc_psd_bytes_fini(&rec.name);
        st = pc_io_legacy_name(pc_str_c(&l->name), l->name.len, &rec.name, err);
    }
    if (st != PC_OK) {
        pc_psd_layer_fini(&rec);
        return st;
    }
    *out = rec;
    return PC_OK;
}

/* ---- content blocks ---- */

static bool key_is(const uint8_t k[4], const char *s)
{
    return memcmp(k, s, 4) == 0;
}

static bool regenerated_key(const uint8_t k[4])
{
    for (size_t i = 0; i < 9; i++) {
        if (memcmp(k, PC_IO_REGENERATED[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

static bool adjustment_key(const uint8_t k[4])
{
    for (size_t i = 0; i < 16; i++) {
        if (memcmp(k, PC_IO_ADJUSTMENT_KEYS[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

/* Blocks that define a layer's content as something other than pixels: fill (SoCo, GdFl, PtFl),
 * placed / smart object (SoLd, PlLd, SoLE), type (TySh) and shape fill (vscg). A raster layer
 * never writes them, nor adjustment blocks. */
static bool raster_stale_key(const uint8_t k[4])
{
    static const char STALE[8][5] = {"SoCo", "GdFl", "PtFl", "SoLd", "PlLd", "SoLE", "TySh", "vscg"};
    for (size_t i = 0; i < 8; i++) {
        if (memcmp(k, STALE[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

/* set_principal: data under the first of keys present (priority order, not file order: Photoshop
 * writes PlLd before SoLd, #200), else appended under keys[0]. */
static pc_status set_principal(pc_io_raw_blocks *raw, const char *const *keys, size_t nkeys,
                               const uint8_t *data, size_t len, pc_error *err)
{
    for (size_t k = 0; k < nkeys; k++) {
        for (size_t i = 0; i < raw->n; i++) {
            if (key_is(raw->v[i].key, keys[k])) {
                uint8_t *d = pc_malloc(len);
                if (d == NULL) {
                    return pc_io_nomem(err, "writing a layer block");
                }
                if (len > 0 && data != NULL) {
                    memcpy(d, data, len);
                }
                free(raw->v[i].data);
                raw->v[i].data = d;
                raw->v[i].len = len;
                return PC_OK;
            }
        }
    }
    return pc_io_raw_blocks_push(raw, (const uint8_t *)keys[0], data, len, err);
}

/* Block keys a smart object's psd_raw may be written under, in priority order: PlLd holds a plcL
 * structure, SoLd / SoLE a soLD one; writing one under the other's key makes it unreadable. */
static const char *const *smart_keys(const pc_blob *raw, size_t *n)
{
    static const char *const PLCL[1] = {"PlLd"};
    static const char *const SOLD[2] = {"SoLd", "SoLE"};
    static const char *const ANY[3] = {"SoLd", "PlLd", "SoLE"};
    const uint8_t *d = pc_blob_data(raw);
    if (pc_blob_len(raw) >= 4 && memcmp(d, "plcL", 4) == 0) {
        *n = 1;
        return PLCL;
    }
    if (pc_blob_len(raw) >= 4 && memcmp(d, "soLD", 4) == 0) {
        *n = 2;
        return SOLD;
    }
    *n = 3;
    return ANY;
}

/* Fill layers: a preserved SoCo / GdFl / PtFl block still describing f is kept; otherwise they
 * are dropped and f is written (write_fill). */
static pc_status fill_rule(pc_io_raw_blocks *raw, pc_io_raw_blocks *regen, const pc_fill *f, pc_error *err)
{
    bool keep = false;
    uint8_t key[4];
    pc_psd_desc d;
    pc_psd_bytes vb = {NULL, 0};
    pc_status st = PC_OK;
    for (size_t i = 0; i < raw->n && !keep && st == PC_OK; i++) {
        const uint8_t *k = raw->v[i].key;
        if (key_is(k, "SoCo") || key_is(k, "GdFl") || key_is(k, "PtFl")) {
            bool found = false;
            pc_fill pf;
            st = pc_io_parse_fill(k, raw->v[i].data, raw->v[i].len, &found, &pf, err);
            if (st == PC_OK && found) {
                keep = pc_fill_eq(&pf, f);
                pc_fill_fini(&pf);
            }
        }
    }
    if (st != PC_OK || keep) {
        return st;
    }
    (void)pc_io_raw_blocks_remove_key(raw, (const uint8_t *)"SoCo");
    (void)pc_io_raw_blocks_remove_key(raw, (const uint8_t *)"GdFl");
    (void)pc_io_raw_blocks_remove_key(raw, (const uint8_t *)"PtFl");
    st = pc_io_fill_to_desc(f, key, &d, err);
    if (st == PC_OK) {
        st = pc_psd_vdesc_to_bytes(&d, &vb, err);
        pc_psd_desc_fini(&d);
    }
    /* Photoshop pads layer-level blocks to a multiple of 4 inside the length (every one in its
     * own files is). A fill descriptor of 2 mod 4 bytes, as a pattern name of the wrong length
     * gives, left the record's extra data unaligned. */
    if (st == PC_OK && vb.len % 4 != 0) {
        uint8_t *d2 = realloc(vb.data, vb.len + (4 - vb.len % 4));
        if (d2 == NULL) {
            st = pc_io_nomem(err, "writing a fill block");
        } else {
            size_t pad = (4 - vb.len % 4);
            memset(d2 + vb.len, 0, pad);
            vb.data = d2;
            vb.len += pad;
        }
    }
    if (st == PC_OK) {
        st = pc_io_raw_blocks_push(regen, key, vb.data, vb.len, err);
    }
    pc_psd_bytes_fini(&vb);
    return st;
}

/* Whether a preserved adjustment block still decodes to a (adjust_map::parse equality). A
 * PhotoCraft v2 Photo Filter block of 18 bytes parses, but Photoshop requires four-byte
 * alignment, so that legacy record is repaired on re-save rather than kept. */
static pc_status adjustment_block_matches(const pc_io_ex *ex, const pc_io_raw_blocks *raw,
                                          const pc_adjustment *a, bool *keep, pc_error *err)
{
    pc_io_adj_channels ch;
    const pc_io_raw_block *cged = pc_io_raw_blocks_find(raw, (const uint8_t *)"CgEd");
    pc_status st = PC_OK;
    switch (ex->fmt.mode) {
    case PC_COLOR_MODE_RGB:
        ch = PC_IO_ADJ_RGB;
        break;
    case PC_COLOR_MODE_GRAYSCALE:
        ch = PC_IO_ADJ_GRAY;
        break;
    case PC_COLOR_MODE_CMYK:
        ch = PC_IO_ADJ_CMYK;
        break;
    case PC_COLOR_MODE_LAB:
        ch = PC_IO_ADJ_LAB;
        break;
    default:
        ch = PC_IO_ADJ_OTHER;
        break;
    }
    *keep = false;
    for (size_t i = 0; i < raw->n && !*keep && st == PC_OK; i++) {
        const pc_io_raw_block *b = &raw->v[i];
        bool legacy_phfl =
            key_is(b->key, "phfl") && b->len >= 2 && b->data[0] == 0 && b->data[1] == 2 && b->len % 4 != 0;
        pc_adjustment parsed;
        if (legacy_phfl || !adjustment_key(b->key)) {
            continue;
        }
        st = pc_io_adjust_parse(b->key, b->data, b->len, cged != NULL ? cged->data : NULL,
                                cged != NULL ? cged->len : 0, ch, &parsed, err);
        if (st == PC_OK) {
            *keep = pc_adjustment_eq(&parsed, a);
            pc_adjustment_fini(&parsed);
        }
    }
    return st;
}

static pc_status adjustment_blocks(pc_io_ex *ex, const pc_layer *l, const pc_adjustment *a,
                                   pc_io_raw_blocks *raw, pc_io_raw_blocks *regen, pc_error *err)
{
    bool keep = false;
    size_t before;
    pc_status st = adjustment_block_matches(ex, raw, a, &keep, err);
    if (st != PC_OK || keep) {
        return st;
    }
    for (size_t i = 0; i < raw->n;) {
        if (adjustment_key(raw->v[i].key) || key_is(raw->v[i].key, "CgEd")) {
            uint8_t k[4];
            memcpy(k, raw->v[i].key, 4);
            (void)pc_io_raw_blocks_remove_key(raw, k);
        } else {
            i++;
        }
    }
    before = regen->n;
    st = pc_io_adjust_write(a, regen, err);
    if (st == PC_OK && regen->n == before) {
        st = pc_io_warn(&ex->warnings, err, "layer \"%s\": %s adjustment is not yet written to PSD",
                        pc_str_c(&l->name), pc_adjustment_label(a));
    }
    /* Photoshop's clrL descriptor has no interpolation setting, so the reopened layer renders
     * trilinear. */
    if (st == PC_OK && a->kind == PC_ADJ_COLOR_LOOKUP && a->u.color_lookup.tetrahedral) {
        st = pc_io_warn(
            &ex->warnings, err,
            "layer \"%s\": Color Lookup tetrahedral interpolation is saved as trilinear (PSD has no "
            "interpolation setting)",
            pc_str_c(&l->name));
    }
    return st;
}

static pc_status text_blocks(pc_io_ex *ex, const pc_layer *l, const pc_text_layer *t, pc_io_raw_blocks *raw,
                             pc_error *err)
{
    static const char *const TYSH[1] = {"TySh"};
    pc_psd_bytes gen = {NULL, 0};
    const uint8_t *src;
    size_t slen;
    bool styled = false;
    pc_buf sb = PC_BUF_INIT;
    pc_status st = PC_OK;
    if (t->psd_raw == NULL) {
        /* Text layers without PSD data (created here) get a generated TySh. */
        st = pc_text_build_tysh(t, ex->dpi, NULL, &gen, err);
        if (st != PC_OK) {
            return st;
        }
        src = gen.data;
        slen = gen.len;
    } else {
        src = pc_blob_data(t->psd_raw);
        slen = pc_blob_len(t->psd_raw);
    }
    /* Character/paragraph style sheets from the document's styles. */
    st = pc_io_export_tysh(src, slen, t, &ex->doc->text_styles, ex->dpi, &styled, &sb, err);
    if (st == PC_OK) {
        /* TextIndex names the layer's object in the document's Txt2 (where the auto-kern mode
         * lives); left at 0, every type layer would claim object 0 (#1348). Keep an unchanged
         * preserved TySh byte for byte. A duplicate or out-of-range file index is renumbered
         * even when no style sheet needed rebuilding. */
        bool generated = t->psd_raw == NULL;
        uint32_t idx = 0;
        const uint8_t *rw = NULL;
        size_t rwl = 0;
        pc_psd_bytes fresh = {NULL, 0};
        bool have_fresh = false;
        (void)pc_io_ex_idmap_get(&ex->text_index, l->id, &idx);
        if (styled) {
            rw = sb.data;
            rwl = sb.len;
        } else if (generated) {
            rw = gen.data;
            rwl = gen.len;
        } else {
            bool has = false;
            int32_t ti = 0;
            st = pc_tysh_text_index(src, slen, &has, &ti, err);
            if (st == PC_OK && !(has && ti == (int32_t)idx)) {
                rw = src;
                rwl = slen;
            }
        }
        if (st == PC_OK && rw != NULL) {
            pc_error e2;
            pc_status s2 = pc_tysh_set_text_index(rw, rwl, (int32_t)idx, &fresh, &e2);
            if (s2 == PC_OK) {
                have_fresh = true;
            } else if (s2 == PC_ERR_NOMEM) {
                st = pc_fail(err, PC_ERR_NOMEM, "%s", e2.msg);
            }
            /* PC_ERR_FORMAT: keep the styled/preserved bytes as they are (fall back). */
        }
        if (st == PC_OK && have_fresh) {
            st = set_principal(raw, TYSH, 1, fresh.data, fresh.len, err);
        } else if (st == PC_OK) {
            st = styled ? set_principal(raw, TYSH, 1, sb.data, sb.len, err)
                        : set_principal(raw, TYSH, 1, src, slen, err);
        }
        pc_psd_bytes_fini(&fresh);
    }
    pc_buf_fini(&sb);
    pc_psd_bytes_fini(&gen);
    if (st == PC_OK && pc_io_raw_blocks_find(raw, (const uint8_t *)"TySh") == NULL) {
        st = pc_io_warn(&ex->warnings, err, "layer \"%s\": text layer written as pixels (no TySh data)",
                        pc_str_c(&l->name));
    }
    return st;
}

static pc_status smart_layer_blocks(pc_io_ex *ex, const pc_layer *l, const pc_smart_object *sm,
                                    pc_io_raw_blocks *raw, pc_error *err)
{
    pc_io_placed parsed;
    bool found = false;
    pc_status st = PC_OK;
    if (sm->psd_raw != NULL) {
        st = pc_io_parse_sold(pc_blob_data(sm->psd_raw), pc_blob_len(sm->psd_raw), &found, &parsed, err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (sm->psd_raw != NULL && !found) {
        /* Placed-layer data we don't parse (PlLd only): kept verbatim. */
        size_t n = 0;
        const char *const *keys = smart_keys(sm->psd_raw, &n);
        ex->smart.prune_ok = false;
        return set_principal(raw, keys, n, pc_blob_data(sm->psd_raw), pc_blob_len(sm->psd_raw), err);
    }
    st = pc_io_ex_smart_blocks(ex, l, sm, found ? &parsed : NULL, raw, err);
    if (found) {
        pc_io_placed_fini(&parsed);
    }
    return st;
}

/* Keeps, regenerates or removes the vmsk / vsms block of a non-shape layer. */
static pc_status vector_mask_block(pc_io_ex *ex, const pc_layer *l, pc_io_raw_blocks *raw, pc_error *err)
{
    uint32_t w = pc_rect_width(ex->canvas);
    uint32_t h = pc_rect_height(ex->canvas);
    size_t pos = raw->n;
    pc_buf b = PC_BUF_INIT;
    bool matches = false;
    pc_status st;
    for (size_t i = 0; i < raw->n && pos == raw->n; i++) {
        if (key_is(raw->v[i].key, "vsms") || key_is(raw->v[i].key, "vmsk")) {
            pos = i;
        }
    }
    if (!l->has_vector_mask) {
        if (pos < raw->n) {
            (void)pc_io_raw_blocks_remove_key(raw, (const uint8_t *)"vsms");
            (void)pc_io_raw_blocks_remove_key(raw, (const uint8_t *)"vmsk");
        }
        return PC_OK;
    }
    if (pos < raw->n) {
        st = pc_io_vector_mask_block_matches(raw->v[pos].data, raw->v[pos].len, &l->vector_mask, w, h,
                                             &matches, err);
        if (st != PC_OK || matches) {
            return st;
        }
    }
    st = pc_io_vector_mask_bytes(&l->vector_mask, w, h, &b, err);
    if (st == PC_OK) {
        if (pos < raw->n) {
            size_t len = 0;
            uint8_t *d = pc_buf_take(&b, &len);
            free(raw->v[pos].data);
            raw->v[pos].data = d;
            raw->v[pos].len = len;
        } else {
            st = pc_io_raw_blocks_push(raw, (const uint8_t *)"vmsk", b.data, b.len, err);
        }
    }
    pc_buf_fini(&b);
    return st;
}

/* Replaces the data of the first block with key in place, else appends. */
static pc_status put_in_place(pc_io_raw_blocks *raw, const char *key, const uint8_t *data, size_t len,
                              pc_error *err)
{
    for (size_t i = 0; i < raw->n; i++) {
        if (key_is(raw->v[i].key, key)) {
            uint8_t *d = pc_malloc(len);
            if (d == NULL) {
                return pc_io_nomem(err, "writing a layer block");
            }
            if (len > 0) {
                memcpy(d, data, len);
            }
            free(raw->v[i].data);
            raw->v[i].data = d;
            raw->v[i].len = len;
            return PC_OK;
        }
    }
    return pc_io_raw_blocks_push(raw, (const uint8_t *)key, data, len, err);
}

/* Ex::content_blocks: preserved raw blocks (with psd_raw overrides) plus regenerated ones, as
 * tagged blocks (*out malloc'd). */
static pc_status content_blocks(pc_io_ex *ex, const pc_layer *l, pc_psd_block **out, size_t *nout,
                                pc_error *err)
{
    pc_io_raw_blocks raw = {NULL, 0, 0};
    pc_io_raw_blocks regen = {NULL, 0, 0};
    bool fx_known = false;
    bool fx_same = false;
    pc_psd_block *blocks = NULL;
    size_t n = 0;
    pc_status st = PC_OK;
    for (size_t i = 0; i < l->npsd_blocks && st == PC_OK; i++) {
        if (!regenerated_key(l->psd_blocks[i].key)) {
            st = pc_io_raw_blocks_push(&raw, l->psd_blocks[i].key, pc_blob_data(l->psd_blocks[i].data),
                                       pc_blob_len(l->psd_blocks[i].data), err);
        }
    }
    /* A stale multi-effects block would override the regenerated lfx2. */
    if (st == PC_OK) {
        st = effects_unchanged(l, &fx_known, &fx_same, err);
    }
    if (st == PC_OK && fx_known && !fx_same) {
        (void)pc_io_raw_blocks_remove_key(&raw, (const uint8_t *)"lmfx");
    }
    if (st == PC_OK) {
        const pc_layer_content *c = &l->content;
        switch (c->kind) {
        case PC_LAYER_ADJUSTMENT:
            st = adjustment_blocks(ex, l, &c->u.adjustment, &raw, &regen, err);
            break;
        case PC_LAYER_FILL:
            st = fill_rule(&raw, &regen, &c->u.fill, err);
            break;
        case PC_LAYER_SHAPE:
            if (c->u.shape.has_fill) {
                st = fill_rule(&raw, &regen, &c->u.shape.fill, err);
            }
            if (st == PC_OK) {
                st = pc_io_shape_blocks(&c->u.shape, &raw, pc_rect_width(ex->canvas),
                                        pc_rect_height(ex->canvas), ex->dpi, err);
            }
            break;
        case PC_LAYER_TEXT:
            st = text_blocks(ex, l, &c->u.text, &raw, err);
            break;
        case PC_LAYER_SMART:
            st = smart_layer_blocks(ex, l, &c->u.smart, &raw, err);
            break;
        case PC_LAYER_RASTER: {
            /* A pixel layer carries no content-defining blocks: a fill, placed (smart), type or
             * shape-fill block left over from before Rasterize would make a reader reopen the
             * layer as that kind and discard the pixels. A shape stroke (vstk) beside a vector
             * mask makes a shape too; without one, Photoshop keeps it. A vector mask stays. */
            bool masked = l->has_vector_mask;
            for (size_t i = 0; i < raw.n;) {
                const uint8_t *k = raw.v[i].key;
                if (raster_stale_key(k) || adjustment_key(k) || key_is(k, "CgEd") ||
                    (key_is(k, "vstk") && masked)) {
                    uint8_t kc[4];
                    memcpy(kc, k, 4);
                    (void)pc_io_raw_blocks_remove_key(&raw, kc);
                } else {
                    i++;
                }
            }
            break;
        }
        case PC_LAYER_GROUP:
        default:
            break;
        }
    }
    /* Effects reference point: written from the field (in place, keeping block order). */
    if (st == PC_OK && l->effects.has_reference) {
        uint8_t d[16];
        pc_store_be64(d, pc_f64_to_bits(l->effects.reference[0]));
        pc_store_be64(d + 8, pc_f64_to_bits(l->effects.reference[1]));
        st = put_in_place(&raw, "fxrp", d, 16, err);
    } else if (st == PC_OK) {
        (void)pc_io_raw_blocks_remove_key(&raw, (const uint8_t *)"fxrp");
    }
    /* Advanced Blending channel restrictions: written from the field (in place). */
    if (st == PC_OK) {
        bool has = false;
        pc_buf b = PC_BUF_INIT;
        st = pc_io_brst_data(l->excluded_channels, &has, &b, err);
        if (st == PC_OK && has) {
            st = put_in_place(&raw, "brst", b.data, b.len, err);
        } else if (st == PC_OK) {
            (void)pc_io_raw_blocks_remove_key(&raw, (const uint8_t *)"brst");
        }
        pc_buf_fini(&b);
    }
    if (st == PC_OK) {
        st = pc_io_put_advanced(&l->advanced, &raw, err);
    }
    if (st == PC_OK && l->content.kind != PC_LAYER_SHAPE) {
        st = vector_mask_block(ex, l, &raw, err);
    }
    if (st == PC_OK) {
        st = pc_io_artboard_block(&ex->doc->guides, l, &raw, err);
    }
    if (st == PC_OK && ex->comps) {
        /* Ids are assigned to every layer before emitting. */
        uint32_t id = 0;
        bool has = false;
        pc_buf b = PC_BUF_INIT;
        uint8_t *d = NULL;
        size_t len = 0;
        (void)pc_io_ex_idmap_get(&ex->layer_ids, l->id, &id);
        st = pc_io_write_cmls(ex->doc->layer_comps, ex->doc->nlayer_comps,
                              ex->doc->has_last_document_state ? &ex->doc->last_document_state : NULL, l, id,
                              &has, &b, err);
        if (st == PC_OK) {
            d = has ? pc_buf_take(&b, &len) : NULL;
            if (has && d == NULL) {
                d = pc_malloc(0);
                if (d == NULL) {
                    st = pc_io_nomem(err, "writing layer comps");
                }
            }
        }
        if (st == PC_OK) {
            st = pc_io_set_cmls(&raw, &d, len, err);
        }
        free(d);
        pc_buf_fini(&b);
    }
    if (st == PC_OK && regen.n + raw.n > 0) {
        blocks = pc_calloc_array(regen.n + raw.n, sizeof *blocks);
        if (blocks == NULL) {
            st = pc_io_nomem(err, "writing layer blocks");
        }
    }
    for (size_t i = 0; i < regen.n + raw.n && st == PC_OK; i++) {
        const pc_io_raw_block *rb = i < regen.n ? &regen.v[i] : &raw.v[i - regen.n];
        char key[4];
        memcpy(key, rb->key, 4);
        st = pc_psd_block_init(&blocks[n], key, rb->data, rb->len, err);
        if (st == PC_OK) {
            n++;
        }
    }
    pc_io_raw_blocks_fini(&raw);
    pc_io_raw_blocks_fini(&regen);
    if (st != PC_OK) {
        pc_psd_blocks_free(blocks, n);
        return st;
    }
    *out = blocks;
    *nout = n;
    return PC_OK;
}

/* ---- emitting ---- */

/* Pixels for a fill layer: Photoshop's cached rendering while valid, otherwise our own rendering
 * of the fill over the canvas, in the frame the layer's masks give it (no document patterns,
 * as the reference). */
static pc_status fill_pixels(const pc_io_ex *ex, const pc_layer *l, const pc_fill *f, pc_surface *out,
                             pc_error *err)
{
    pc_compose_buffer buf;
    size_t ch = pc_pixel_format_channels(ex->fmt);
    float *vals;
    pc_surface s;
    pc_status st;
    if (l->has_fill_cache && pc_fill_eq(&l->fill_cache.fill, f)) {
        return pc_surface_clone(&l->fill_cache.surface, out, err);
    }
    /* A native CMYK solid/gradient fill keeps the same samples for rendering and the PSD cache,
     * so the fill is not separated through RGB. Patterns fall through (found false). */
    if (ex->cmyk) {
        bool found = false;
        pc_surface cs;
        st = pc_compose_render_cmyk_fill(f, ex->canvas, pc_compose_fill_frame(l, ex->canvas), ex->fmt.sample,
                                         &found, &cs, err);
        if (st != PC_OK) {
            return st;
        }
        if (found) {
            *out = cs;
            return PC_OK;
        }
    }
    /* Readers composite these pixels (ours keeps them as the fill's rendering), so a pattern fill
     * must be rendered from the document's patterns, not left transparent (#1907). */
    st = pc_compose_render_fill_content(l, f, ex->canvas, ex->doc->patterns, ex->doc->npatterns, &buf, err);
    if (st != PC_OK) {
        return st;
    }
    vals = pc_malloc_array(buf.len > 0 ? buf.len : 1, ch * sizeof *vals);
    if (vals == NULL) {
        pc_compose_buffer_fini(&buf);
        return pc_io_nomem(err, "rendering a fill layer");
    }
    for (size_t i = 0; i < buf.len; i++) {
        float v[PC_RASTER_MAX_CHANNELS];
        (void)pc_from_rgba(ex->fmt, buf.px[i], v);
        memcpy(vals + i * ch, v, ch * sizeof *v);
    }
    pc_surface_init(&s, ex->fmt);
    st = pc_surface_write_region(&s, ex->canvas, vals, buf.len * ch, err);
    free(vals);
    pc_compose_buffer_fini(&buf);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    pc_surface_prune(&s);
    *out = s;
    return PC_OK;
}

/* Appends a record and its link group (dividers have none). */
static pc_status push_record(pc_io_ex *ex, pc_psd_layer *rec, bool has_link, uint64_t link, pc_error *err)
{
    pc_psd_layer *g = pc_grow_array(ex->records, &ex->caprecords, ex->nrecords + 1, sizeof *g);
    if (g == NULL) {
        pc_psd_layer_fini(rec);
        return pc_io_nomem(err, "writing layer records");
    }
    ex->records = g;
    if (ex->nrecords + 1 > ex->caplinks) {
        size_t cap = ex->caprecords;
        bool *h = realloc(ex->link_has, cap * sizeof *h);
        uint64_t *v;
        if (h == NULL) {
            pc_psd_layer_fini(rec);
            return pc_io_nomem(err, "writing layer records");
        }
        ex->link_has = h;
        v = realloc(ex->link_group, cap * sizeof *v);
        if (v == NULL) {
            pc_psd_layer_fini(rec);
            return pc_io_nomem(err, "writing layer records");
        }
        ex->link_group = v;
        ex->caplinks = cap;
    }
    ex->link_has[ex->nrecords] = has_link;
    ex->link_group[ex->nrecords] = link;
    g[ex->nrecords++] = *rec;
    return PC_OK;
}

/* The bounding divider that opens a group (in file order, below its children). */
static pc_status divider_record(pc_io_ex *ex, pc_error *err)
{
    static const pc_str END = {(char *)"</Layer group>", 14};
    pc_psd_layer rec;
    pc_psd_block b;
    size_t cap = 0;
    pc_status st;
    pc_psd_layer_init(&rec);
    st = empty_channels(ex, &rec.channels, &rec.nchannels, err);
    if (st == PC_OK) {
        pc_psd_bytes_fini(&rec.blending_ranges);
        st = pc_psd_blending_ranges_full(ex->cc, &rec.blending_ranges, err);
    }
    if (st == PC_OK) {
        st = pc_psd_bytes_set(&rec.name, END.ptr, END.len, err);
    }
    if (st == PC_OK) {
        st = unicode_name_block(&END, &b, err);
        st = st == PC_OK ? push_block(&rec.blocks, &rec.nblocks, &cap, &b, err) : st;
    }
    if (st == PC_OK) {
        st = pc_psd_block_make_u32("lyid", fresh_id(ex), &b, err);
        st = st == PC_OK ? push_block(&rec.blocks, &rec.nblocks, &cap, &b, err) : st;
    }
    if (st == PC_OK) {
        st = pc_psd_block_make_section(PC_PSD_SECTION_DIVIDER, NULL, NULL, &b, err);
        st = st == PC_OK ? push_block(&rec.blocks, &rec.nblocks, &cap, &b, err) : st;
    }
    if (st != PC_OK) {
        pc_psd_layer_fini(&rec);
        return st;
    }
    return push_record(ex, &rec, false, 0, err);
}

pc_status pc_io_ex_emit(pc_io_ex *ex, const pc_layer *layers, size_t n, size_t depth, pc_error *err)
{
    if (depth > MAX_EMIT_DEPTH) {
        return pc_fail(err, PC_ERR_LIMIT, "limit exceeded: layer groups nested more than %d deep",
                       MAX_EMIT_DEPTH);
    }
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = &layers[i];
        pc_psd_block *extra = NULL;
        size_t nextra = 0;
        const pc_surface *px = NULL;
        pc_surface fill;
        bool own_fill = false;
        pc_psd_layer rec;
        pc_status st = content_blocks(ex, l, &extra, &nextra, err);
        if (st != PC_OK) {
            return st;
        }
        switch (l->content.kind) {
        case PC_LAYER_GROUP: {
            const pc_group *g = &l->content.u.group;
            pc_psd_block lsct;
            pc_psd_block *all;
            uint8_t blend[4];
            st = divider_record(ex, err);
            if (st == PC_OK) {
                st = pc_io_ex_emit(ex, g->children, g->nchildren, depth + 1, err);
            }
            memcpy(blend, pc_blend_psd_key(l->blend), 4);
            if (st == PC_OK) {
                st = pc_psd_block_make_section(g->expanded ? PC_PSD_SECTION_OPEN : PC_PSD_SECTION_CLOSED,
                                               blend, NULL, &lsct, err);
            }
            if (st != PC_OK) {
                pc_psd_blocks_free(extra, nextra);
                return st;
            }
            all = pc_malloc_array(nextra + 1, sizeof *all);
            if (all == NULL) {
                pc_psd_block_fini(&lsct);
                pc_psd_blocks_free(extra, nextra);
                return pc_io_nomem(err, "writing a group");
            }
            all[0] = lsct;
            if (nextra > 0) {
                memcpy(all + 1, extra, nextra * sizeof *all);
            }
            free(extra);
            extra = all;
            nextra++;
            break;
        }
        case PC_LAYER_RASTER:
            px = &l->content.u.raster;
            break;
        case PC_LAYER_FILL:
            st = fill_pixels(ex, l, &l->content.u.fill, &fill, err);
            if (st != PC_OK) {
                pc_psd_blocks_free(extra, nextra);
                return st;
            }
            own_fill = true;
            px = &fill;
            break;
        case PC_LAYER_TEXT:
            px = l->content.u.text.has_cache ? &l->content.u.text.cache : NULL;
            break;
        case PC_LAYER_SHAPE:
            px = l->content.u.shape.has_cache ? &l->content.u.shape.cache : NULL;
            break;
        case PC_LAYER_SMART:
            px = l->content.u.smart.has_cache ? &l->content.u.smart.cache : NULL;
            break;
        case PC_LAYER_ADJUSTMENT:
        default:
            break;
        }
        st = record(ex, l, extra, nextra, px, &rec, err);
        if (own_fill) {
            pc_surface_fini(&fill);
        }
        if (st == PC_OK) {
            st = push_record(ex, &rec, l->has_link_group, l->link_group, err);
        }
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

/* ---- the id map ---- */

static size_t id_slot(const pc_io_ex_idmap *m, uint64_t key)
{
    uint64_t h = key * 0x9E3779B97F4A7C15u;
    size_t i = (size_t)(h >> 17) & (m->cap - 1);
    while (m->used[i] && m->keys[i] != key) {
        i = (i + 1) & (m->cap - 1);
    }
    return i;
}

bool pc_io_ex_idmap_get(const pc_io_ex_idmap *m, uint64_t key, uint32_t *out)
{
    size_t i;
    if (m->cap == 0) {
        return false;
    }
    i = id_slot(m, key);
    if (!m->used[i]) {
        return false;
    }
    *out = m->vals[i];
    return true;
}

pc_status pc_io_ex_idmap_init(pc_io_ex_idmap *m, size_t n, pc_error *err)
{
    size_t cap = 16;
    while (cap < n * 2 + 1) {
        if (cap > SIZE_MAX / 4) {
            return pc_fail(err, PC_ERR_LIMIT, "limit exceeded: %zu layers", n);
        }
        cap *= 2;
    }
    memset(m, 0, sizeof *m);
    m->keys = pc_calloc_array(cap, sizeof *m->keys);
    m->vals = pc_calloc_array(cap, sizeof *m->vals);
    m->used = pc_calloc_array(cap, sizeof *m->used);
    if (m->keys == NULL || m->vals == NULL || m->used == NULL) {
        free(m->keys);
        free(m->vals);
        free(m->used);
        memset(m, 0, sizeof *m);
        return pc_io_nomem(err, "numbering layers");
    }
    m->cap = cap;
    return PC_OK;
}

void pc_io_ex_idmap_fini(pc_io_ex_idmap *m)
{
    free(m->keys);
    free(m->vals);
    free(m->used);
    memset(m, 0, sizeof *m);
}

/* Inserts or overwrites (HashMap::insert); returns whether the key was new. */
bool pc_io_ex_idmap_put(pc_io_ex_idmap *m, uint64_t key, uint32_t val)
{
    size_t i = id_slot(m, key);
    bool fresh = !m->used[i];
    m->used[i] = true;
    m->keys[i] = key;
    m->vals[i] = val;
    return fresh;
}
