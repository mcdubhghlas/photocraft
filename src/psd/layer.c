#include "psd/layer.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "psd/compression.h"
#include "psd/internal.h"

uint8_t pc_psd_flags_set_pixel_data_irrelevant(uint8_t flags, bool v)
{
    flags = (uint8_t)(flags | PC_PSD_LF_BIT4_USEFUL);
    return v ? (uint8_t)(flags | PC_PSD_LF_PIXEL_DATA_IRRELEVANT) : (uint8_t)(flags & ~PC_PSD_LF_PIXEL_DATA_IRRELEVANT);
}

/* ---- rects ---- */

static pc_status read_rect(psd_rd *r, pc_psd_rect *out, pc_error *err)
{
    pc_status st;
    CK(psd_i32(r, &out->top, err));
    CK(psd_i32(r, &out->left, err));
    CK(psd_i32(r, &out->bottom, err));
    CK(psd_i32(r, &out->right, err));
    return PC_OK;
fail:
    return st;
}

static void write_rect(psd_w *w, pc_psd_rect r)
{
    psd_put_i32(w, r.top);
    psd_put_i32(w, r.left);
    psd_put_i32(w, r.bottom);
    psd_put_i32(w, r.right);
}

/* ---- masks ---- */

size_t pc_psd_mask_parameter_bytes(uint8_t pf)
{
    return (size_t)((pf & 1) != 0) + 8 * (size_t)((pf & 2) != 0) + (size_t)((pf & 4) != 0) + 8 * (size_t)((pf & 8) != 0);
}

/* Parameters at pf_at, then 0..=3 bytes of padding, and nothing else. */
static bool fits(const uint8_t *d, size_t len, size_t pf_at)
{
    size_t lo;
    if (pf_at >= len) {
        return false;
    }
    lo = pf_at + 1 + pc_psd_mask_parameter_bytes(d[pf_at]);
    return len >= lo && len <= lo + 3;
}

static bool real_background_ok(const uint8_t *d, size_t len, size_t at)
{
    return at + 1 < len && (d[at + 1] == 0 || d[at + 1] == 255);
}

bool pc_psd_mask_real_before_parameters(const uint8_t *d, size_t len)
{
    bool photoshop_plausible;
    bool spec_plausible = false;
    if (fits(d, len, 18)) {
        return false;
    }
    if (!fits(d, len, 36)) {
        return false;
    }
    /* Photoshop's reading: real-mask fields at 18, parameters at 36. */
    photoshop_plausible = real_background_ok(d, len, 18) && 36 < len && (d[36] & 0xF0) == 0;
    /* The spec's reading: parameters at 18, then the 18 real-mask bytes, then padding. */
    if (18 < len) {
        uint8_t pf = d[18];
        size_t real_at = 19 + pc_psd_mask_parameter_bytes(pf);
        spec_plausible = (pf & 0xF0) == 0 && len >= real_at + 18 && len <= real_at + 21 &&
                         real_background_ok(d, len, real_at);
    }
    return photoshop_plausible || !spec_plausible;
}

void pc_psd_layer_mask_fini(pc_psd_layer_mask *m)
{
    if (m != NULL) {
        pc_psd_bytes_fini(&m->trailing);
    }
}

pc_status pc_psd_layer_mask_make(pc_psd_rect rect, uint8_t default_color, uint8_t flags, pc_psd_layer_mask *out,
                                 pc_error *err)
{
    static const uint8_t pad[2] = {0, 0};
    memset(out, 0, sizeof *out);
    out->rect = rect;
    out->default_color = default_color;
    out->flags = (uint8_t)(flags & ~PC_PSD_MF_PARAMETERS);
    out->real_first = true;
    return pc_psd_bytes_set(&out->trailing, pad, 2, err);
}

static pc_status read_real(psd_rd *r, pc_psd_layer_mask *m, pc_error *err)
{
    pc_status st;
    if (psd_remaining(r) < 18) {
        return PC_OK;
    }
    CK(psd_u8(r, &m->real.flags, err));
    CK(psd_u8(r, &m->real.background, err));
    CK(read_rect(r, &m->real.rect, err));
    m->has_real = true;
    return PC_OK;
fail:
    return st;
}

pc_status pc_psd_layer_mask_parse(const uint8_t *d, size_t len, pc_psd_layer_mask *out, pc_error *err)
{
    psd_rd r = psd_rd_make(d, len);
    pc_psd_layer_mask m;
    bool has_params;
    bool real_first;
    pc_status st;
    memset(&m, 0, sizeof m);
    CK(read_rect(&r, &m.rect, err));
    CK(psd_u8(&r, &m.default_color, err));
    CK(psd_u8(&r, &m.flags, err));
    has_params = (m.flags & PC_PSD_MF_PARAMETERS) != 0;
    real_first = has_params && pc_psd_mask_real_before_parameters(d, len);
    if (real_first) {
        CK(read_real(&r, &m, err));
    }
    if (has_params) {
        pc_psd_mask_params *p = &m.params;
        CK(psd_u8(&r, &p->flags, err));
        if (p->flags & 1) {
            CK(psd_u8(&r, &p->user_density, err));
            p->has_user_density = true;
        }
        if (p->flags & 2) {
            CK(psd_f64(&r, &p->user_feather, err));
            p->has_user_feather = true;
        }
        if (p->flags & 4) {
            CK(psd_u8(&r, &p->vector_density, err));
            p->has_vector_density = true;
        }
        if (p->flags & 8) {
            CK(psd_f64(&r, &p->vector_feather, err));
            p->has_vector_feather = true;
        }
        m.has_params = true;
    }
    if (!real_first) {
        CK(read_real(&r, &m, err));
    }
    /* The order only matters when both are present; otherwise default to Photoshop's. */
    m.real_first = real_first || !m.has_real || !m.has_params;
    CK(pc_psd_bytes_set(&m.trailing, psd_rest(&r), psd_remaining(&r), err));
    *out = m;
    return PC_OK;
fail:
    pc_psd_layer_mask_fini(&m);
    return st;
}

static void write_real(psd_w *w, const pc_psd_layer_mask *m)
{
    if (m->has_real) {
        psd_put_u8(w, m->real.flags);
        psd_put_u8(w, m->real.background);
        write_rect(w, m->real.rect);
    }
}

static void write_mask(psd_w *w, const pc_psd_layer_mask *m)
{
    write_rect(w, m->rect);
    psd_put_u8(w, m->default_color);
    psd_put_u8(w, m->flags);
    if (m->real_first) {
        write_real(w, m);
    }
    if (m->has_params) {
        const pc_psd_mask_params *p = &m->params;
        psd_put_u8(w, p->flags);
        if (p->has_user_density) {
            psd_put_u8(w, p->user_density);
        }
        if (p->has_user_feather) {
            psd_put_f64(w, p->user_feather);
        }
        if (p->has_vector_density) {
            psd_put_u8(w, p->vector_density);
        }
        if (p->has_vector_feather) {
            psd_put_f64(w, p->vector_feather);
        }
    }
    if (!m->real_first) {
        write_real(w, m);
    }
    psd_put(w, m->trailing.data, m->trailing.len);
}

pc_status pc_psd_layer_mask_to_bytes(const pc_psd_layer_mask *m, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    psd_w_init(&w, err);
    write_mask(&w, m);
    return psd_w_finish(&w, out);
}

static bool params_eq(const pc_psd_mask_params *a, const pc_psd_mask_params *b)
{
    return a->flags == b->flags && a->has_user_density == b->has_user_density &&
           (!a->has_user_density || a->user_density == b->user_density) && a->has_user_feather == b->has_user_feather &&
           (!a->has_user_feather || a->user_feather == b->user_feather) &&
           a->has_vector_density == b->has_vector_density &&
           (!a->has_vector_density || a->vector_density == b->vector_density) &&
           a->has_vector_feather == b->has_vector_feather &&
           (!a->has_vector_feather || a->vector_feather == b->vector_feather);
}

bool pc_psd_layer_mask_eq(const pc_psd_layer_mask *a, const pc_psd_layer_mask *b)
{
    return pc_psd_rect_eq(a->rect, b->rect) && a->default_color == b->default_color && a->flags == b->flags &&
           a->has_params == b->has_params && (!a->has_params || params_eq(&a->params, &b->params)) &&
           a->has_real == b->has_real &&
           (!a->has_real || (a->real.flags == b->real.flags && a->real.background == b->real.background &&
                             pc_psd_rect_eq(a->real.rect, b->real.rect))) &&
           pc_psd_bytes_eq(&a->trailing, &b->trailing) && a->real_first == b->real_first;
}

static void mask_data_fini(pc_psd_mask_data *m)
{
    pc_psd_layer_mask_fini(&m->mask);
    pc_psd_bytes_fini(&m->raw);
    m->kind = PC_PSD_MASK_NONE;
}

static bool mask_data_eq(const pc_psd_mask_data *a, const pc_psd_mask_data *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_PSD_MASK_PARSED:
        return pc_psd_layer_mask_eq(&a->mask, &b->mask);
    case PC_PSD_MASK_RAW:
        return pc_psd_bytes_eq(&a->raw, &b->raw);
    default:
        return true;
    }
}

static pc_status mask_data_clone(const pc_psd_mask_data *a, pc_psd_mask_data *out, pc_error *err)
{
    pc_status st;
    memset(out, 0, sizeof *out);
    out->kind = a->kind;
    out->mask = a->mask;
    out->mask.trailing.data = NULL;
    out->mask.trailing.len = 0;
    st = pc_psd_bytes_clone(&a->mask.trailing, &out->mask.trailing, err);
    if (st == PC_OK) {
        st = pc_psd_bytes_clone(&a->raw, &out->raw, err);
    }
    if (st != PC_OK) {
        mask_data_fini(out);
    }
    return st;
}

pc_status pc_psd_blending_ranges_full(size_t channels, pc_psd_bytes *out, pc_error *err)
{
    static const uint8_t entry[8] = {0, 0, 255, 255, 0, 0, 255, 255};
    psd_w w;
    size_t i;
    psd_w_init(&w, err);
    for (i = 0; i <= channels; i++) {
        psd_put(&w, entry, 8);
    }
    return psd_w_finish(&w, out);
}

/* ---- channels ---- */

static pc_psd_layout channel_layout(size_t w, size_t h, uint16_t depth, pc_psd_version v)
{
    pc_psd_layout l;
    l.planes = 1;
    l.width = w;
    l.height = h;
    l.depth = depth;
    l.version = v;
    return l;
}

pc_status pc_psd_channel_encode(int16_t id, uint16_t compression, const uint8_t *decoded, size_t len, size_t width,
                                size_t height, uint16_t depth, pc_psd_version v, pc_psd_channel *out, pc_error *err)
{
    pc_psd_layout l = channel_layout(width, height, depth, v);
    pc_psd_bytes d = {NULL, 0};
    pc_status st = pc_psd_encode_planes(compression, decoded, len, &l, &d, err);
    if (st != PC_OK) {
        return st;
    }
    out->id = id;
    out->has_compression = true;
    out->compression = compression;
    out->data = d;
    return PC_OK;
}

pc_status pc_psd_channel_decode(const pc_psd_channel *c, size_t width, size_t height, uint16_t depth, pc_psd_version v,
                                pc_psd_bytes *out, pc_error *err)
{
    pc_psd_layout l = channel_layout(width, height, depth, v);
    size_t n;
    pc_status st;
    if (c->has_compression) {
        return pc_psd_decode_planes(c->compression, c->data.data, c->data.len, &l, out, err);
    }
    st = pc_psd_layout_decoded_len(&l, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n != 0) {
        return psd_invalid(err, "channel has no data");
    }
    pc_psd_bytes_fini(out);
    return PC_OK;
}

static uint64_t stored_len(const pc_psd_channel *c)
{
    return c->has_compression ? 2 + (uint64_t)c->data.len : 0;
}

/* ---- layer records ---- */

void pc_psd_layer_init(pc_psd_layer *l)
{
    memset(l, 0, sizeof *l);
    memcpy(l->blend, "norm", 4);
    l->opacity = 255;
}

void pc_psd_layer_fini(pc_psd_layer *l)
{
    size_t i;
    if (l == NULL) {
        return;
    }
    for (i = 0; i < l->nchannels; i++) {
        pc_psd_bytes_fini(&l->channels[i].data);
    }
    free(l->channels);
    mask_data_fini(&l->mask);
    pc_psd_bytes_fini(&l->blending_ranges);
    pc_psd_bytes_fini(&l->name);
    pc_psd_blocks_free(l->blocks, l->nblocks);
    pc_psd_bytes_fini(&l->extra_trailing);
    pc_psd_layer_init(l);
}

pc_status pc_psd_layer_clone(const pc_psd_layer *a, pc_psd_layer *out, pc_error *err)
{
    pc_psd_layer c;
    size_t i;
    pc_status st;
    pc_psd_layer_init(&c);
    c.rect = a->rect;
    memcpy(c.blend, a->blend, 4);
    c.opacity = a->opacity;
    c.clipping = a->clipping;
    c.flags = a->flags;
    c.filler = a->filler;
    if (a->nchannels > 0) {
        c.channels = pc_calloc_array(a->nchannels, sizeof *c.channels);
        if (c.channels == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
        for (i = 0; i < a->nchannels; i++) {
            c.channels[i] = a->channels[i];
            c.channels[i].data.data = NULL;
            c.channels[i].data.len = 0;
            c.nchannels++;
            CK(pc_psd_bytes_clone(&a->channels[i].data, &c.channels[i].data, err));
        }
    }
    CK(mask_data_clone(&a->mask, &c.mask, err));
    CK(pc_psd_bytes_clone(&a->blending_ranges, &c.blending_ranges, err));
    CK(pc_psd_bytes_clone(&a->name, &c.name, err));
    CK(pc_psd_blocks_clone(a->blocks, a->nblocks, &c.blocks, err));
    c.nblocks = a->nblocks;
    CK(pc_psd_bytes_clone(&a->extra_trailing, &c.extra_trailing, err));
    *out = c;
    return PC_OK;
fail:
    pc_psd_layer_fini(&c);
    return st;
}

static bool channel_eq(const pc_psd_channel *a, const pc_psd_channel *b)
{
    return a->id == b->id && a->has_compression == b->has_compression &&
           (!a->has_compression || a->compression == b->compression) && pc_psd_bytes_eq(&a->data, &b->data);
}

bool pc_psd_layer_eq(const pc_psd_layer *a, const pc_psd_layer *b)
{
    size_t i;
    if (!pc_psd_rect_eq(a->rect, b->rect) || a->nchannels != b->nchannels || memcmp(a->blend, b->blend, 4) != 0 ||
        a->opacity != b->opacity || a->clipping != b->clipping || a->flags != b->flags || a->filler != b->filler ||
        !mask_data_eq(&a->mask, &b->mask) || !pc_psd_bytes_eq(&a->blending_ranges, &b->blending_ranges) ||
        !pc_psd_bytes_eq(&a->name, &b->name) || !pc_psd_blocks_eq(a->blocks, a->nblocks, b->blocks, b->nblocks) ||
        !pc_psd_bytes_eq(&a->extra_trailing, &b->extra_trailing)) {
        return false;
    }
    for (i = 0; i < a->nchannels; i++) {
        if (!channel_eq(&a->channels[i], &b->channels[i])) {
            return false;
        }
    }
    return true;
}

pc_status pc_psd_layer_push_block(pc_psd_layer *l, pc_psd_block *b, pc_error *err)
{
    pc_psd_block *g = realloc(l->blocks, (l->nblocks + 1) * sizeof *g);
    if (g == NULL) {
        pc_psd_block_fini(b);
        return psd_nomem(err);
    }
    l->blocks = g;
    l->blocks[l->nblocks++] = *b;
    memset(b, 0, sizeof *b);
    return PC_OK;
}

pc_status pc_psd_layer_push_channel(pc_psd_layer *l, pc_psd_channel *c, pc_error *err)
{
    pc_psd_channel *g = realloc(l->channels, (l->nchannels + 1) * sizeof *g);
    if (g == NULL) {
        pc_psd_bytes_fini(&c->data);
        return psd_nomem(err);
    }
    l->channels = g;
    l->channels[l->nchannels++] = *c;
    memset(c, 0, sizeof *c);
    return PC_OK;
}

const pc_psd_block *pc_psd_layer_block(const pc_psd_layer *l, const char key[4])
{
    size_t i;
    for (i = 0; i < l->nblocks; i++) {
        if (pc_psd_block_is(&l->blocks[i], key)) {
            return &l->blocks[i];
        }
    }
    return NULL;
}

pc_psd_block *pc_psd_layer_block_mut(pc_psd_layer *l, const char key[4])
{
    size_t i;
    for (i = 0; i < l->nblocks; i++) {
        if (pc_psd_block_is(&l->blocks[i], key)) {
            return &l->blocks[i];
        }
    }
    return NULL;
}

pc_status pc_psd_layer_name(const pc_psd_layer *l, char **out, pc_error *err)
{
    size_t i;
    for (i = 0; i < l->nblocks; i++) {
        if (pc_psd_block_is(&l->blocks[i], "luni")) {
            char *n = NULL;
            pc_status st = pc_psd_block_unicode_name(&l->blocks[i], &n, NULL);
            if (st == PC_OK) {
                *out = n;
                return PC_OK;
            }
            if (st == PC_ERR_NOMEM) {
                return psd_nomem(err);
            }
        }
    }
    return psd_decode_legacy_name(l->name.data, l->name.len, out, err);
}

bool pc_psd_layer_section_divider(const pc_psd_layer *l, pc_psd_section_divider *out)
{
    static const char *const KEYS[2] = {"lsct", "lsdk"};
    int k;
    for (k = 0; k < 2; k++) {
        const pc_psd_block *b = pc_psd_layer_block(l, KEYS[k]);
        if (b != NULL && pc_psd_block_section(b, out, NULL) == PC_OK) {
            return true;
        }
    }
    return false;
}

uint32_t pc_psd_layer_section_type(const pc_psd_layer *l)
{
    pc_psd_section_divider s;
    return pc_psd_layer_section_divider(l, &s) ? s.kind : PC_PSD_SECTION_OTHER;
}

bool pc_psd_layer_is_visible(const pc_psd_layer *l)
{
    return (l->flags & PC_PSD_LF_HIDDEN) == 0;
}

bool pc_psd_layer_id(const pc_psd_layer *l, uint32_t *out)
{
    const pc_psd_block *b = pc_psd_layer_block(l, "lyid");
    return b != NULL && pc_psd_block_u32(b, out, NULL) == PC_OK;
}

uint8_t pc_psd_layer_fill_opacity(const pc_psd_layer *l)
{
    const pc_psd_block *b = pc_psd_layer_block(l, "iOpa");
    uint8_t v;
    return b != NULL && pc_psd_block_u8(b, &v, NULL) == PC_OK ? v : 255;
}

const pc_psd_layer_mask *pc_psd_layer_get_mask(const pc_psd_layer *l)
{
    return l->mask.kind == PC_PSD_MASK_PARSED ? &l->mask.mask : NULL;
}

const pc_psd_channel *pc_psd_layer_channel(const pc_psd_layer *l, int16_t id)
{
    size_t i;
    for (i = 0; i < l->nchannels; i++) {
        if (l->channels[i].id == id) {
            return &l->channels[i];
        }
    }
    return NULL;
}

pc_psd_rect pc_psd_layer_channel_rect(const pc_psd_layer *l, int16_t id)
{
    if (l->mask.kind == PC_PSD_MASK_PARSED) {
        if (id == PC_PSD_CH_USER_MASK) {
            return l->mask.mask.rect;
        }
        if (id == PC_PSD_CH_REAL_USER_MASK) {
            return l->mask.mask.has_real ? l->mask.mask.real.rect : l->mask.mask.rect;
        }
    }
    return l->rect;
}

pc_status pc_psd_layer_decode_channel(const pc_psd_layer *l, int16_t id, uint16_t depth, pc_psd_version v,
                                      pc_psd_bytes *out, pc_error *err)
{
    const pc_psd_channel *c = pc_psd_layer_channel(l, id);
    size_t w;
    size_t h;
    pc_status st;
    if (c == NULL) {
        return psd_invalid(err, "layer has no channel %d", (int)id);
    }
    st = pc_psd_rect_size(pc_psd_layer_channel_rect(l, id), &w, &h, err);
    if (st != PC_OK) {
        return st;
    }
    return pc_psd_channel_decode(c, w, h, depth, v, out, err);
}

/* Reads one record; channel lengths go to lens (nchannels entries, owned). */
static pc_status read_record(psd_rd *r, pc_psd_version v, pc_psd_layer *out, uint64_t **lens, pc_error *err)
{
    pc_psd_layer l;
    uint16_t nch;
    uint16_t i;
    uint8_t sig[4];
    uint32_t extra_len;
    psd_rd x;
    uint32_t mask_len;
    const uint8_t *mb;
    size_t mlen;
    uint32_t br_len;
    uint64_t *ln = NULL;
    pc_status st;
    pc_psd_layer_init(&l);
    CK(read_rect(r, &l.rect, err));
    CK(psd_u16(r, &nch, err));
    if (nch > PC_PSD_MAX_LAYER_CHANNELS) {
        st = psd_limit(err, "too many channels in layer");
        goto fail;
    }
    if (nch > 0) {
        l.channels = pc_calloc_array(nch, sizeof *l.channels);
        ln = pc_calloc_array(nch, sizeof *ln);
        if (l.channels == NULL || ln == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
    }
    for (i = 0; i < nch; i++) {
        CK(psd_i16(r, &l.channels[i].id, err));
        CK(psd_len_field(r, v == PC_PSD_V2, &ln[i], err));
        l.nchannels++;
    }
    CK(psd_arr4(r, sig, err));
    if (memcmp(sig, "8BIM", 4) != 0) {
        st = psd_bad_sig(err, "8BIM (layer blend mode)", sig);
        goto fail;
    }
    CK(psd_arr4(r, l.blend, err));
    CK(psd_u8(r, &l.opacity, err));
    CK(psd_u8(r, &l.clipping, err));
    CK(psd_u8(r, &l.flags, err));
    CK(psd_u8(r, &l.filler, err));
    CK(psd_u32(r, &extra_len, err));
    CK(psd_sub(r, extra_len, &x, err));
    CK(psd_u32(&x, &mask_len, err));
    CK(psd_bytes_u64(&x, mask_len, &mb, &mlen, err));
    if (mlen > 0) {
        if (pc_psd_layer_mask_parse(mb, mlen, &l.mask.mask, NULL) == PC_OK) {
            l.mask.kind = PC_PSD_MASK_PARSED;
        } else {
            CK(pc_psd_bytes_set(&l.mask.raw, mb, mlen, err));
            l.mask.kind = PC_PSD_MASK_RAW;
        }
    }
    CK(psd_u32(&x, &br_len, err));
    CK(psd_take_bytes(&x, br_len, &l.blending_ranges, err));
    CK(psd_read_pascal(&x, 4, &l.name, err));
    CK(psd_read_blocks(&x, v, &l.blocks, &l.nblocks, &l.extra_trailing, err));
    *out = l;
    *lens = ln;
    return PC_OK;
fail:
    free(ln);
    pc_psd_layer_fini(&l);
    return st;
}

static void write_record(psd_w *w, const pc_psd_layer *l, pc_psd_version v)
{
    size_t i;
    size_t at;
    size_t m;
    write_rect(w, l->rect);
    if (l->nchannels > 0xFFFF) {
        if (psd_w_ok(w)) {
            psd_w_fail(w, psd_limit(w->err, "too many channels"));
        }
        return;
    }
    psd_put_u16(w, (uint16_t)l->nchannels);
    for (i = 0; i < l->nchannels; i++) {
        psd_put_i16(w, l->channels[i].id);
        psd_put_len(w, stored_len(&l->channels[i]), v == PC_PSD_V2);
    }
    psd_put(w, "8BIM", 4);
    psd_put(w, l->blend, 4);
    psd_put_u8(w, l->opacity);
    psd_put_u8(w, l->clipping);
    psd_put_u8(w, l->flags);
    psd_put_u8(w, l->filler);
    at = psd_begin_len(w, false);
    m = psd_begin_len(w, false);
    if (l->mask.kind == PC_PSD_MASK_PARSED) {
        write_mask(w, &l->mask.mask);
    } else if (l->mask.kind == PC_PSD_MASK_RAW) {
        psd_put(w, l->mask.raw.data, l->mask.raw.len);
    }
    psd_end_len(w, m, false);
    psd_put_len(w, l->blending_ranges.len, false);
    psd_put(w, l->blending_ranges.data, l->blending_ranges.len);
    psd_write_pascal(w, l->name.data, l->name.len, 4);
    psd_write_blocks(w, l->blocks, l->nblocks, v);
    psd_put(w, l->extra_trailing.data, l->extra_trailing.len);
    psd_end_len(w, at, false);
}

/* ---- layer info ---- */

void pc_psd_layer_info_fini(pc_psd_layer_info *li)
{
    size_t i;
    if (li == NULL) {
        return;
    }
    for (i = 0; i < li->n; i++) {
        pc_psd_layer_fini(&li->layers[i]);
    }
    free(li->layers);
    pc_psd_opt_bytes_fini(&li->padding);
    memset(li, 0, sizeof *li);
}

pc_status pc_psd_layer_info_clone(const pc_psd_layer_info *li, pc_psd_layer_info *out, pc_error *err)
{
    pc_psd_layer_info c;
    size_t i;
    pc_status st;
    memset(&c, 0, sizeof c);
    c.merged_alpha = li->merged_alpha;
    if (li->n > 0) {
        c.layers = pc_calloc_array(li->n, sizeof *c.layers);
        if (c.layers == NULL) {
            return psd_nomem(err);
        }
        for (i = 0; i < li->n; i++) {
            CK(pc_psd_layer_clone(&li->layers[i], &c.layers[i], err));
            c.n++;
        }
    }
    CK(pc_psd_opt_bytes_clone(&li->padding, &c.padding, err));
    *out = c;
    return PC_OK;
fail:
    pc_psd_layer_info_fini(&c);
    return st;
}

bool pc_psd_layer_info_eq(const pc_psd_layer_info *a, const pc_psd_layer_info *b)
{
    size_t i;
    if (a->merged_alpha != b->merged_alpha || a->n != b->n || !pc_psd_opt_bytes_eq(&a->padding, &b->padding)) {
        return false;
    }
    for (i = 0; i < a->n; i++) {
        if (!pc_psd_layer_eq(&a->layers[i], &b->layers[i])) {
            return false;
        }
    }
    return true;
}

pc_status pc_psd_layer_info_push(pc_psd_layer_info *li, pc_psd_layer *l, pc_error *err)
{
    pc_psd_layer *g = realloc(li->layers, (li->n + 1) * sizeof *g);
    if (g == NULL) {
        pc_psd_layer_fini(l);
        return psd_nomem(err);
    }
    li->layers = g;
    li->layers[li->n++] = *l;
    pc_psd_layer_init(l);
    return PC_OK;
}

pc_status pc_psd_layer_info_parse(const uint8_t *data, size_t len, pc_psd_version v, pc_psd_layer_info *out,
                                  pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pc_psd_layer_info li;
    uint64_t **lens = NULL;
    int16_t count;
    uint16_t n;
    size_t i;
    size_t k;
    size_t body_len;
    const uint8_t *rest;
    size_t nrest;
    bool zeros = true;
    pc_status st;
    memset(&li, 0, sizeof li);
    CK(psd_i16(&r, &count, err));
    li.merged_alpha = count < 0;
    n = (uint16_t)(count < 0 ? -(int32_t)count : count);
    /* Minimum record size: 16 rect + 2 + 4 sig + 4 key + 4 + 4 extra len. */
    CK(psd_check_count(&r, n, 34, err));
    if (n > 0) {
        li.layers = pc_calloc_array(n, sizeof *li.layers);
        lens = pc_calloc_array(n, sizeof *lens);
        if (li.layers == NULL || lens == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
    }
    for (i = 0; i < n; i++) {
        CK(read_record(&r, v, &li.layers[i], &lens[i], err));
        li.n++;
    }
    for (i = 0; i < n; i++) {
        pc_psd_layer *l = &li.layers[i];
        const uint64_t *ln = lens[i]; /* NULL when the record has no channels */
        for (k = 0; ln != NULL && k < l->nchannels; k++) {
            uint64_t clen = ln[k];
            if (clen == 0) {
                continue;
            }
            if (clen == 1) {
                st = psd_invalid(err, "channel data length 1");
                goto fail;
            }
            CK(psd_u16(&r, &l->channels[k].compression, err));
            l->channels[k].has_compression = true;
            CK(psd_take_bytes(&r, clen - 2, &l->channels[k].data, err));
        }
    }
    body_len = r.pos;
    rest = psd_rest(&r);
    nrest = psd_remaining(&r);
    for (i = 0; i < nrest; i++) {
        zeros = zeros && rest[i] == 0;
    }
    if (!(nrest == body_len % 2 && zeros)) {
        li.padding.has = true;
        CK(pc_psd_bytes_set(&li.padding.v, rest, nrest, err));
    }
    for (i = 0; lens != NULL && i < n; i++) {
        free(lens[i]);
    }
    free(lens);
    *out = li;
    return PC_OK;
fail:
    for (i = 0; lens != NULL && i < n; i++) {
        free(lens[i]);
    }
    free(lens);
    pc_psd_layer_info_fini(&li);
    return st;
}

void psd_write_layer_info(psd_w *w, const pc_psd_layer_info *li, pc_psd_version v)
{
    size_t start = w->b.len;
    size_t i;
    size_t k;
    if (li->n > 32767) {
        if (psd_w_ok(w)) {
            psd_w_fail(w, psd_limit(w->err, "more than 32767 layers"));
        }
        return;
    }
    psd_put_i16(w, (int16_t)(li->merged_alpha ? -(int32_t)li->n : (int32_t)li->n));
    for (i = 0; i < li->n; i++) {
        write_record(w, &li->layers[i], v);
    }
    for (i = 0; i < li->n; i++) {
        for (k = 0; k < li->layers[i].nchannels; k++) {
            const pc_psd_channel *c = &li->layers[i].channels[k];
            if (c->has_compression) {
                psd_put_u16(w, c->compression);
                psd_put(w, c->data.data, c->data.len);
            }
        }
    }
    if (li->padding.has) {
        psd_put(w, li->padding.v.data, li->padding.v.len);
    } else if ((w->b.len - start) % 2 == 1) {
        psd_put_u8(w, 0);
    }
}

pc_status pc_psd_layer_info_to_bytes(const pc_psd_layer_info *li, pc_psd_version v, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    psd_w_init(&w, err);
    psd_write_layer_info(&w, li, v);
    return psd_w_finish(&w, out);
}

pc_status pc_psd_layer_info_unpadded_len(const pc_psd_layer_info *li, pc_psd_version v, uint64_t *out, pc_error *err)
{
    psd_w w;
    size_t i;
    size_t k;
    uint64_t n;
    psd_w_init(&w, err);
    psd_put_i16(&w, 0);
    for (i = 0; i < li->n; i++) {
        write_record(&w, &li->layers[i], v);
    }
    if (w.st != PC_OK) {
        pc_status st = w.st;
        psd_w_fini(&w);
        return st;
    }
    n = w.b.len;
    psd_w_fini(&w);
    for (i = 0; i < li->n; i++) {
        for (k = 0; k < li->layers[i].nchannels; k++) {
            const pc_psd_channel *c = &li->layers[i].channels[k];
            if (c->has_compression) {
                n += 2 + (uint64_t)c->data.len;
            }
        }
    }
    *out = n;
    return PC_OK;
}

pc_status pc_psd_layer_info_pad_to(pc_psd_layer_info *li, pc_psd_version v, uint64_t align, pc_error *err)
{
    uint64_t len;
    size_t pad;
    pc_status st;
    if (align == 0) {
        align = 1;
    }
    st = pc_psd_layer_info_unpadded_len(li, v, &len, err);
    if (st != PC_OK) {
        return st;
    }
    pad = (size_t)((align - len % align) % align);
    pc_psd_opt_bytes_fini(&li->padding);
    li->padding.has = true;
    li->padding.v.data = pad > 0 ? calloc(pad, 1) : NULL;
    if (pad > 0 && li->padding.v.data == NULL) {
        li->padding.has = false;
        return psd_nomem(err);
    }
    li->padding.v.len = pad;
    return PC_OK;
}
