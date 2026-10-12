/* PSD export, the file (port of document_to_psd_nested and its helpers in
 * crates/io/src/psd_export.rs): layer ids, the merged composite (rendered and encoded in bands,
 * matted against white like Photoshop), alpha channels and Quick Mask, image resources (paths,
 * guides, link groups, metadata, preserved resources), global blocks and the layer-info
 * placement for the document's depth. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/utf8.h"
#include "codecs/orientation.h"
#include "compose/compose.h"
#include "io/annotations_map.h"
#include "io/channel_map.h"
#include "io/comps_map.h"
#include "io/pattern_map.h"
#include "io/pixels.h"
#include "io/psd_common.h"
#include "io/psd_export_internal.h"
#include "io/slices_map.h"
#include "io/vector_map.h"
#include "psd/misc.h"
#include "raster/pixel.h"

/* ---- the merged image ---- */

typedef struct merged_ctx {
    pc_pixel_format fmt;
    size_t cc;
    size_t bps;
    size_t plane; /* bytes per plane */
    size_t w;
    pc_rect canvas;
    bool cmyk;
    bool matte;
    bool lab16;
    float white[PC_RASTER_MAX_CHANNELS];
    pc_cmyk_space *space;
    uint8_t *planes;
    bool has_alpha;
    bool translucent;
    const pc_compose_buffer *band; /* the band being converted */
    size_t start;                  /* its first pixel index in the canvas */
} merged_ctx;

static pc_status merged_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    merged_ctx *m = ctx;
    /* The composite came through the document's CMYK profile: convert back through it too. */
    pc_cmyk_space *prev = pc_cmyk_space_enter(m->space);
    (void)chunk;
    for (size_t i = begin; i < end; i++) {
        float v[PC_RASTER_MAX_CHANNELS];
        (void)pc_from_rgba(m->fmt, m->band->px[i], v);
        for (size_t c = 0; c <= m->cc; c++) {
            /* Matte against white like Photoshop (see pc_io_matte). */
            float x = c < m->cc && m->matte ? pc_io_matte(v[c], v[m->cc], m->white[c]) : v[c];
            uint8_t *o = m->planes + c * m->plane + (m->start + i) * m->bps;
            x = m->cmyk && c < m->cc ? 1.0f - x : x;
            /* 16-bit Lab a and b use Photoshop's 0..65280 scale (pc_io_lab16_chroma). */
            if (m->lab16 && (c == 1 || c == 2)) {
                x = x * (PC_IO_LAB16_CHROMA_MAX / 65535.0f);
            }
            (void)pc_io_encode_be(x, m->fmt.sample, o);
        }
    }
    pc_cmyk_space_leave(prev);
    return PC_OK;
}

static pc_status merged_band(void *ctx, pc_compose_buffer *band, pc_error *err)
{
    merged_ctx *m = ctx;
    for (size_t i = 0; i < band->len; i++) {
        float a = band->px[i][3];
        float ca = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
        bool partial;
        /* Retain alpha whenever it differs from opaque at the stored precision. */
        switch (m->fmt.sample) {
        case PC_SAMPLE_TYPE_U8:
            partial = pc_io_q255(a) < 255;
            break;
        case PC_SAMPLE_TYPE_U16:
            partial = pc_f32_to_u16(ca * 65535.0f + 0.5f) != UINT16_MAX;
            break;
        default:
            partial = a != 1.0f;
            break;
        }
        m->has_alpha = m->has_alpha || partial;
        m->translucent = m->translucent || a < 1.0f;
    }
    m->band = band;
    m->start = (size_t)(band->rect.y0 - m->canvas.y0) * m->w;
    {
        pc_status st = pc_pool_for(pc_pool_global(), band->len, 16384, merged_chunk, m);
        if (st != PC_OK) {
            return pc_fail(err, st, "converting the merged image failed");
        }
    }
    return PC_OK;
}

/* The merged image's planes (colour channels, then alpha), big-endian, matted against white when
 * matte; with whether any pixel needs the alpha channel and whether any is below 1. */
static pc_status merged_planes(const pc_document *doc, pc_pixel_format fmt, bool matte, uint8_t **out,
                               size_t *len, bool *has_alpha, bool *translucent, pc_error *err)
{
    merged_ctx m;
    const float opaque_white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    size_t n;
    size_t total;
    pc_status st;
    memset(&m, 0, sizeof m);
    m.fmt = fmt;
    m.cc = pc_color_mode_channels(fmt.mode);
    m.bps = pc_sample_type_bytes(fmt.sample);
    m.canvas = pc_document_bounds(doc);
    m.w = pc_rect_width(m.canvas);
    m.cmyk = fmt.mode == PC_COLOR_MODE_CMYK;
    m.matte = matte;
    m.lab16 = fmt.mode == PC_COLOR_MODE_LAB && fmt.sample == PC_SAMPLE_TYPE_U16;
    if (!pc_size_mul(doc->size.width, doc->size.height, &n) || !pc_size_mul(n, m.bps, &m.plane) ||
        !pc_size_mul(m.plane, m.cc + 1, &total) || total > PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "limit exceeded: a %ux%u merged image", (unsigned)doc->size.width,
                       (unsigned)doc->size.height);
    }
    m.planes = pc_calloc_array(total > 0 ? total : 1, 1);
    if (m.planes == NULL) {
        return pc_io_nomem(err, "writing the merged image");
    }
    (void)pc_from_rgba(fmt, opaque_white, m.white);
    m.space = pc_compose_cmyk_space(doc);
    /* The reference ignores a failed render (its compositor can't fail); here the render's own
     * error (a group nesting limit, memory) is returned. */
    st = pc_compose_render_bands(doc, m.canvas, 0, NULL, merged_band, &m, err);
    pc_cmyk_space_release(m.space);
    if (st != PC_OK) {
        free(m.planes);
        return st;
    }
    *out = m.planes;
    *len = total;
    *has_alpha = m.has_alpha;
    *translucent = m.translucent;
    return PC_OK;
}

/* ---- resources ---- */

typedef struct res_list {
    pc_psd_resource *v;
    size_t n;
    size_t cap;
} res_list;

static pc_status add_res(res_list *r, uint16_t id, const void *data, size_t len, pc_error *err)
{
    pc_psd_resource *v = pc_grow_array(r->v, &r->cap, r->n + 1, sizeof *v);
    pc_status st;
    if (v == NULL) {
        return pc_io_nomem(err, "adding a resource");
    }
    r->v = v;
    st = pc_psd_resource_init(&v[r->n], id, data, len, err);
    if (st == PC_OK) {
        r->n++;
    }
    return st;
}

/* Adds a resource with a (legacy) name. */
static pc_status add_named(res_list *r, uint16_t id, const pc_str *name, const void *data, size_t len,
                           pc_error *err)
{
    pc_status st = add_res(r, id, data, len, err);
    if (st == PC_OK) {
        pc_psd_bytes_fini(&r->v[r->n - 1].name);
        st = pc_io_legacy_name(pc_str_c(name), name->len, &r->v[r->n - 1].name, err);
    }
    return st;
}

static pc_status guides_resource(const pc_document *doc, pc_buf *out, pc_error *err)
{
    uint8_t t[16];
    size_t n = doc->guides.nhorizontal + doc->guides.nvertical;
    pc_status st;
    pc_store_be32(t, 1);
    pc_store_be32(t + 4, 576);
    pc_store_be32(t + 8, 576);
    pc_store_be32(t + 12, (uint32_t)n);
    st = pc_buf_append(out, t, 16, err);
    for (size_t pass = 0; pass < 2 && st == PC_OK; pass++) {
        const float *list = pass == 0 ? doc->guides.vertical : doc->guides.horizontal;
        size_t cnt = pass == 0 ? doc->guides.nvertical : doc->guides.nhorizontal;
        for (size_t i = 0; i < cnt && st == PC_OK; i++) {
            pc_store_be32(t, (uint32_t)pc_f32_to_i32(roundf(list[i] * 32.0f)));
            t[4] = (uint8_t)pass;
            st = pc_buf_append(out, t, 5, err);
        }
    }
    return st;
}

/* Resource 1026 (Layer > Link Layers): one u16 group id per record, in emit order. Imported ids
 * that fit in a u16 are kept, so unchanged files write the same bytes; otherwise groups are
 * renumbered by first appearance. *has false when no layer is linked. */
static pc_status link_group_resource(const pc_io_ex *ex, bool *has, pc_buf *out, pc_error *err)
{
    bool any = false;
    bool fits = true;
    uint64_t *seen = NULL;
    size_t nseen = 0;
    pc_status st = PC_OK;
    *has = false;
    for (size_t i = 0; i < ex->nrecords; i++) {
        if (ex->link_has[i]) {
            any = true;
            fits = fits && ex->link_group[i] >= 1 && ex->link_group[i] <= UINT16_MAX;
        }
    }
    if (!any) {
        return PC_OK;
    }
    if (!fits) {
        seen = pc_malloc_array(ex->nrecords, sizeof *seen);
        if (seen == NULL) {
            return pc_io_nomem(err, "writing link groups");
        }
    }
    for (size_t i = 0; i < ex->nrecords && st == PC_OK; i++) {
        uint16_t id = 0;
        uint8_t b[2];
        if (ex->link_has[i] && fits) {
            id = (uint16_t)ex->link_group[i];
        } else if (ex->link_has[i]) {
            size_t k = 0;
            while (k < nseen && seen[k] != ex->link_group[i]) {
                k++;
            }
            if (k == nseen) {
                seen[nseen++] = ex->link_group[i];
            }
            id = k + 1 > UINT16_MAX ? UINT16_MAX : (uint16_t)(k + 1);
        }
        pc_store_be16(b, id);
        st = pc_buf_append(out, b, 2, err);
    }
    free(seen);
    *has = st == PC_OK;
    return st;
}

/* The Pascal name in a clipping-path resource as lossy UTF-8 ("" when it's cut short). */
static pc_status clip_raw_name(const pc_blob *data, pc_buf *out, pc_error *err)
{
    const uint8_t *d = pc_blob_data(data);
    size_t len = pc_blob_len(data);
    size_t n = len > 0 ? d[0] : 0;
    if (1 + n > len) {
        return PC_OK;
    }
    return pc_utf8_lossy(d + 1, n, out, err);
}

static const pc_doc_psd_resource *find_resource(const pc_document *doc, uint16_t id)
{
    for (size_t i = 0; i < doc->metadata.npsd_resources; i++) {
        if (doc->metadata.psd_resources[i].id == id) {
            return &doc->metadata.psd_resources[i];
        }
    }
    return NULL;
}

/* Saved paths (2000+), the work path (1025) and the clipping path name (2999). */
static pc_status path_resources(pc_io_ex *ex, const pc_document *doc, res_list *res, pc_error *err)
{
    uint32_t w = doc->size.width;
    uint32_t h = doc->size.height;
    size_t max = PC_IO_SAVED_PATHS_LAST - PC_IO_SAVED_PATHS_FIRST + 1;
    const pc_doc_psd_resource *raw_clip = find_resource(doc, PC_IO_CLIPPING_PATH);
    pc_status st = PC_OK;
    if (doc->npaths > max) {
        st = pc_io_warn(&ex->warnings, err, "only %zu saved paths fit in PSD; the rest were dropped", max);
    }
    for (size_t i = 0; i < doc->npaths && i < max && st == PC_OK; i++) {
        const pc_named_path *p = &doc->paths[i];
        bool reuse = false;
        pc_psd_bytes data = {NULL, 0};
        if (p->psd_raw != NULL) {
            pc_path back;
            bool found = false;
            st = pc_io_path_from_resource(pc_blob_data(p->psd_raw), pc_blob_len(p->psd_raw), w, h, &found,
                                          &back, err);
            if (st == PC_OK && found) {
                reuse = pc_path_eq(&back, &p->path);
                pc_path_fini(&back);
            }
        }
        if (st == PC_OK && reuse) {
            st = pc_psd_bytes_set(&data, pc_blob_data(p->psd_raw), pc_blob_len(p->psd_raw), err);
        } else if (st == PC_OK) {
            pc_psd_path rec;
            st = pc_io_path_to_records(&p->path, w, h, &rec, err);
            if (st == PC_OK) {
                st = pc_psd_path_to_bytes(&rec, &data, err);
                pc_psd_path_fini(&rec);
            }
        }
        if (st == PC_OK) {
            st = add_named(res, (uint16_t)(PC_IO_SAVED_PATHS_FIRST + i), &p->name, data.data, data.len, err);
        }
        pc_psd_bytes_fini(&data);
    }
    if (st == PC_OK && doc->has_work_path) {
        pc_psd_path rec;
        pc_psd_bytes data = {NULL, 0};
        st = pc_io_path_to_records(&doc->work_path, w, h, &rec, err);
        if (st == PC_OK) {
            st = pc_psd_path_to_bytes(&rec, &data, err);
            pc_psd_path_fini(&rec);
        }
        if (st == PC_OK) {
            st = add_res(res, PC_IO_WORK_PATH, data.data, data.len, err);
        }
        pc_psd_bytes_fini(&data);
    }
    if (st == PC_OK && doc->has_clipping_path) {
        const pc_clipping_path *c = &doc->clipping_path;
        bool same = false;
        if (raw_clip != NULL) {
            pc_buf nm = PC_BUF_INIT;
            st = clip_raw_name(raw_clip->data, &nm, err);
            same = st == PC_OK && pc_str_eq_mem(&c->name, (const char *)nm.data, nm.len);
            pc_buf_fini(&nm);
        }
        if (st == PC_OK && !same) {
            /* Pascal name, then flatness as 16.16 fixed. */
            pc_psd_bytes name = {NULL, 0};
            pc_buf d = PC_BUF_INIT;
            float fl = c->flatness >= 0.0f ? c->flatness : 0.0f; /* f32::max(0.0): NaN gives 0 */
            uint8_t t[4];
            st = pc_io_legacy_name(pc_str_c(&c->name), c->name.len, &name, err);
            if (st == PC_OK) {
                st = pc_buf_append_byte(&d, (uint8_t)name.len, err);
            }
            if (st == PC_OK) {
                st = pc_buf_append(&d, name.data, name.len, err);
            }
            pc_store_be32(t, pc_f32_to_u32(fl * 65536.0f));
            if (st == PC_OK) {
                st = pc_buf_append(&d, t, 4, err);
            }
            if (st == PC_OK) {
                st = add_res(res, PC_IO_CLIPPING_PATH, d.data, d.len, err);
            }
            pc_psd_bytes_fini(&name);
            pc_buf_fini(&d);
        }
    }
    return st;
}

/* The preserved resources, filtered: comps and slices when regenerated, measurement resources
 * that no longer describe the document, a clipping-path name that is no longer current. */
static pc_status preserved_resources(const pc_document *doc, bool comps_regen, bool slices_regen,
                                     res_list *res, pc_error *err)
{
    pc_status st = PC_OK;
    for (size_t i = 0; i < doc->metadata.npsd_resources && st == PC_OK; i++) {
        const pc_doc_psd_resource *r = &doc->metadata.psd_resources[i];
        if ((r->id == PC_IO_LAYER_COMPS && comps_regen) || (r->id == PC_IO_SLICES && slices_regen) ||
            !pc_io_keep_resource(doc, r->id)) {
            continue;
        }
        if (r->id == PC_IO_CLIPPING_PATH) {
            pc_buf nm = PC_BUF_INIT;
            bool same;
            st = clip_raw_name(r->data, &nm, err);
            same = st == PC_OK && doc->has_clipping_path &&
                   pc_str_eq_mem(&doc->clipping_path.name, (const char *)nm.data, nm.len);
            pc_buf_fini(&nm);
            if (st != PC_OK || !same) {
                continue;
            }
        }
        st = add_named(res, r->id, &r->name, pc_blob_data(r->data), pc_blob_len(r->data), err);
    }
    return st;
}

/* ---- the exporter ---- */

static void ex_fini(pc_io_ex *ex)
{
    for (size_t i = 0; i < ex->nrecords; i++) {
        pc_psd_layer_fini(&ex->records[i]);
    }
    free(ex->records);
    free(ex->link_has);
    free(ex->link_group);
    pc_io_ex_idmap_fini(&ex->layer_ids);
    pc_io_warnings_fini(&ex->warnings);
    pc_io_ex_smart_fini(&ex->smart);
}

/* Layer ids, assigned up front (stable across round trips): unique psd_ids are kept, the rest
 * numbered after them; dividers come after all. */
static pc_status assign_ids(pc_io_ex *ex, const pc_document *doc, pc_io_ex_idmap *used, pc_error *err)
{
    pc_walk walk;
    uint32_t next = 1;
    uint32_t max = 0;
    pc_status st = pc_document_walk(doc, &walk, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_io_ex_idmap_init(&ex->layer_ids, walk.len, err);
    if (st == PC_OK) {
        st = pc_io_ex_idmap_init(used, walk.len, err);
    }
    for (size_t i = 0; i < walk.len && st == PC_OK; i++) {
        const pc_layer *l = walk.items[i].layer;
        uint32_t dummy;
        if (l->has_psd_id && !pc_io_ex_idmap_get(used, l->psd_id, &dummy)) {
            (void)pc_io_ex_idmap_put(used, l->psd_id, 0);
            (void)pc_io_ex_idmap_put(&ex->layer_ids, l->id, l->psd_id);
            max = l->psd_id > max ? l->psd_id : max;
        }
    }
    for (size_t i = 0; i < walk.len && st == PC_OK; i++) {
        const pc_layer *l = walk.items[i].layer;
        uint32_t dummy;
        if (!pc_io_ex_idmap_get(&ex->layer_ids, l->id, &dummy)) {
            while (pc_io_ex_idmap_get(used, next, &dummy)) {
                next++;
            }
            (void)pc_io_ex_idmap_put(used, next, 0);
            (void)pc_io_ex_idmap_put(&ex->layer_ids, l->id, next);
            max = next > max ? next : max;
        }
    }
    pc_walk_fini(&walk);
    ex->next_id = max;
    return st;
}

/* The exporter's id map as the parallel arrays slices_map takes (walk order, first entries). */
static pc_status id_arrays(const pc_io_ex *ex, const pc_document *doc, pc_layer_id **ids, uint32_t **psd,
                           size_t *n, pc_error *err)
{
    pc_walk walk;
    pc_status st = pc_document_walk(doc, &walk, err);
    size_t k = 0;
    if (st != PC_OK) {
        return st;
    }
    *ids = pc_malloc_array(walk.len > 0 ? walk.len : 1, sizeof **ids);
    *psd = pc_malloc_array(walk.len > 0 ? walk.len : 1, sizeof **psd);
    if (*ids == NULL || *psd == NULL) {
        free(*ids);
        free(*psd);
        *ids = NULL;
        *psd = NULL;
        pc_walk_fini(&walk);
        return pc_io_nomem(err, "writing slices");
    }
    for (size_t i = 0; i < walk.len; i++) {
        uint32_t v;
        pc_layer_id id = walk.items[i].layer->id;
        bool dup = false;
        for (size_t j = 0; j < k && !dup; j++) {
            dup = (*ids)[j] == id;
        }
        if (!dup && pc_io_ex_idmap_get(&ex->layer_ids, id, &v)) {
            (*ids)[k] = id;
            (*psd)[k] = v;
            k++;
        }
    }
    pc_walk_fini(&walk);
    *n = k;
    return PC_OK;
}

/* The global blocks: patterns, then notes, then smart objects' files and filter caches; each
 * padded to 4 as Photoshop does (readers such as psd-tools step to the next block that way). */
static pc_status global_blocks(pc_io_ex *ex, const pc_document *doc, pc_psd_block **out, size_t *nout,
                               pc_error *err)
{
    pc_doc_psd_global_block *pat = NULL;
    size_t npat = 0;
    pc_doc_psd_global_block *ann = NULL;
    size_t nann = 0;
    pc_doc_psd_global_block *fin = NULL;
    size_t nfin = 0;
    pc_psd_block *blocks = NULL;
    size_t n = 0;
    pc_status st = pc_io_export_pattern_blocks(doc, &pat, &npat, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_io_annotation_export_blocks(doc, pat, npat, &ann, &nann, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_io_ex_smart_finish(&ex->smart, ann, nann, &fin, &nfin, err);
    if (st != PC_OK) {
        return st;
    }
    if (nfin > 0) {
        blocks = pc_calloc_array(nfin, sizeof *blocks);
        if (blocks == NULL) {
            st = pc_io_nomem(err, "writing global blocks");
        }
    }
    for (size_t i = 0; i < nfin && st == PC_OK; i++) {
        static const uint8_t ZERO[4] = {0, 0, 0, 0};
        size_t len = pc_blob_len(fin[i].data);
        char key[4];
        memcpy(key, fin[i].key, 4);
        st = pc_psd_block_init(&blocks[n], key, pc_blob_data(fin[i].data), len, err);
        if (st == PC_OK) {
            memcpy(blocks[n].signature, fin[i].signature, 4);
            blocks[n].padding.has = true;
            n++;
            st = pc_psd_bytes_set(&blocks[n - 1].padding.v, ZERO, (4 - len % 4) % 4, err);
        }
    }
    for (size_t i = 0; i < nfin; i++) {
        pc_blob_release(fin[i].data);
    }
    free(fin);
    if (st != PC_OK) {
        pc_psd_blocks_free(blocks, n);
        return st;
    }
    *out = blocks;
    *nout = n;
    return PC_OK;
}

static pc_status extra_planes(const pc_io_ex *ex, const pc_alpha_channel *const *extra, size_t nextra,
                              uint8_t **planes, size_t *len, pc_error *err)
{
    pc_status st = PC_OK;
    for (size_t i = 0; i < nextra && st == PC_OK; i++) {
        pc_surface conv;
        const pc_surface *s = &extra[i]->surface;
        bool owned = false;
        uint8_t *bytes = NULL;
        size_t blen = 0;
        uint8_t *plane = NULL;
        size_t plen = 0;
        bool inv = false;
        if (!pc_pixel_format_eq(pc_surface_format(s), ex->mask_fmt)) {
            st = pc_surface_convert(s, ex->mask_fmt, &conv, err);
            s = &conv;
            owned = st == PC_OK;
        }
        if (st == PC_OK) {
            st = pc_surface_to_interleaved(s, ex->canvas, &bytes, &blen, err);
        }
        if (owned) {
            pc_surface_fini(&conv);
        }
        if (st == PC_OK) {
            st = pc_io_deinterleave(bytes, blen, 1, ex->fmt.sample, &inv, &plane, &plen, err);
        }
        free(bytes);
        if (st == PC_OK && plen > 0) {
            size_t total;
            uint8_t *g;
            if (!pc_size_add(*len, plen, &total) || total > PC_MAX_ALLOC) {
                st = pc_fail(err, PC_ERR_LIMIT, "limit exceeded: the image data of %zu bytes", *len);
            } else if ((g = realloc(*planes, total)) == NULL) {
                st = pc_io_nomem(err, "writing alpha channels");
            } else {
                memcpy(g + *len, plane, plen);
                *planes = g;
                *len = total;
            }
        }
        free(plane);
    }
    return st;
}

pc_status pc_io_document_to_psd_nested(const pc_document *doc, const pc_io_psd_export_opts *opts,
                                       uint32_t depth, pc_psd_file *out, pc_io_warnings *warnings,
                                       pc_error *err)
{
    pc_pixel_format fmt;
    pc_sample_type sample;
    size_t cc;
    bool big;
    bool size_exceeds = false;
    pc_io_ex ex;
    pc_io_ex_idmap used;
    bool comps_unchanged = true;
    uint8_t *planes = NULL;
    size_t plen = 0;
    bool has_alpha = false;
    bool translucent = false;
    size_t n;
    size_t max_extra;
    const pc_alpha_channel **extra = NULL;
    size_t nextra = 0;
    bool has_qm_id = false;
    uint16_t qm_id = 0;
    pc_psd_file f;
    res_list res = {NULL, 0, 0};
    pc_buf tmp = PC_BUF_INIT;
    pc_psd_block *gblocks = NULL;
    size_t ngblocks = 0;
    bool comps_has = false;
    pc_buf comps_buf = PC_BUF_INIT;
    pc_io_slices_action slices_action = PC_IO_SLICES_KEEP;
    pc_buf slices_buf = PC_BUF_INIT;
    pc_status st;
    if (doc->mode == PC_COLOR_MODE_MULTICHANNEL) {
        return pc_io_multichannel_to_psd(doc, opts->force_psb, out, warnings, err);
    }
    fmt = pc_document_pixel_format(doc);
    sample = fmt.sample;
    cc = pc_color_mode_channels(fmt.mode);
    big = doc->size.width > 30000 || doc->size.height > 30000;
    memset(&ex, 0, sizeof ex);
    memset(&used, 0, sizeof used);
    pc_psd_file_init(&f);
    st = !opts->force_psb && !big ? pc_io_psd_size_exceeds_limit(doc, &size_exceeds, err) : PC_OK;
    if (st == PC_OK) {
        st = pc_io_comps_unchanged(doc, &comps_unchanged, err);
    }
    if (st == PC_OK) {
        st = pc_io_ex_smart_init(&ex.smart, doc, depth, err);
    }
    if (st != PC_OK) {
        return st;
    }
    ex.doc = doc;
    ex.fmt = fmt;
    ex.dpi = doc->resolution_dpi;
    ex.mask_fmt = (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, sample, false};
    ex.cc = cc;
    ex.cmyk = fmt.mode == PC_COLOR_MODE_CMYK;
    ex.version = pc_io_psd_version(doc, opts->force_psb, size_exceeds);
    ex.canvas = pc_document_bounds(doc);
    ex.comps = !comps_unchanged;
    if (big && !opts->force_psb) {
        st = pc_io_warn(&ex.warnings, err, "document exceeds 30000 px; written as PSB");
    } else if (size_exceeds && !opts->force_psb) {
        st = pc_io_warn(&ex.warnings, err, "estimated encoded size exceeds 2 GB; written as PSB");
    }
    if (st == PC_OK && fmt.mode != doc->mode) {
        st = pc_io_warn(&ex.warnings, err, "%s document written as %s", pc_color_mode_name(doc->mode),
                        pc_color_mode_name(fmt.mode));
    }
    if (st == PC_OK) {
        st = assign_ids(&ex, doc, &used, err);
    }
    pc_io_ex_idmap_fini(&used);
    if (st == PC_OK) {
        st = pc_io_ex_emit(&ex, doc->layers, doc->nlayers, 0, err);
    }
    /* Merged composite, rendered and encoded in bands. Matting against white only changes pixels
     * with alpha < 1; if some are slightly translucent but all round to opaque (so no alpha
     * channel is written), encode once more without the matte. */
    if (st == PC_OK) {
        st = merged_planes(doc, fmt, opts->merged_matte, &planes, &plen, &has_alpha, &translucent, err);
    }
    if (st == PC_OK && !has_alpha && translucent && opts->merged_matte) {
        bool a2;
        bool t2;
        free(planes);
        planes = NULL;
        st = merged_planes(doc, fmt, false, &planes, &plen, &a2, &t2, err);
    }
    n = (size_t)doc->size.width * doc->size.height;
    if (st == PC_OK && !has_alpha) {
        plen = cc * n * pc_sample_type_bytes(sample);
    }
    max_extra = 56 - cc - (has_alpha ? 1 : 0);
    if (st == PC_OK && doc->nchannels > max_extra) {
        st = pc_io_warn(&ex.warnings, err, "only %zu alpha channels fit in PSD; the rest were dropped",
                        max_extra);
    }
    if (st == PC_OK) {
        extra = pc_malloc_array(max_extra + 1, sizeof *extra);
        if (extra == NULL) {
            st = pc_io_nomem(err, "writing alpha channels");
        }
    }
    for (size_t i = 0; st == PC_OK && i < doc->nchannels && i < max_extra; i++) {
        extra[nextra++] = &doc->channels[i];
    }
    /* Saved in Quick Mask mode: the mask is written as the last extra channel and flagged by
     * resource 1022 (channel id, initially-empty flag), as Photoshop does. */
    if (st == PC_OK && doc->has_quick_mask) {
        if (nextra < max_extra) {
            extra[nextra++] = &doc->quick_mask;
            has_qm_id = true;
            qm_id = (uint16_t)(cc + (has_alpha ? 1 : 0) + nextra - 1);
        } else {
            st = pc_io_warn(&ex.warnings, err,
                            "Quick Mask was dropped because the PSD channel limit was reached");
        }
    }
    if (st == PC_OK) {
        st = extra_planes(&ex, extra, nextra, &planes, &plen, err);
    }
    if (st == PC_OK) {
        uint16_t mcomp = sample == PC_SAMPLE_TYPE_F32 ? PC_PSD_RAW : opts->merged_compression;
        pc_psd_bytes enc = {NULL, 0};
        f.header = pc_psd_header_make(ex.version, doc->size.width, doc->size.height,
                                      (uint16_t)(cc + (has_alpha ? 1 : 0) + nextra), pc_io_psd_depth(sample),
                                      fmt.mode == PC_COLOR_MODE_GRAYSCALE ? PC_PSD_MODE_GRAYSCALE
                                      : fmt.mode == PC_COLOR_MODE_CMYK    ? PC_PSD_MODE_CMYK
                                      : fmt.mode == PC_COLOR_MODE_LAB     ? PC_PSD_MODE_LAB
                                                                          : PC_PSD_MODE_RGB);
        st = pc_psd_image_encode(mcomp, planes, plen, &f.header, &enc, err);
        f.image_compression = mcomp;
        if (st != PC_OK && st != PC_ERR_NOMEM) {
            f.image_compression = PC_PSD_RAW;
            st = pc_psd_image_encode(PC_PSD_RAW, planes, plen, &f.header, &enc, err);
            if (st != PC_OK && st != PC_ERR_NOMEM) {
                enc.data = planes;
                enc.len = plen;
                planes = NULL;
                st = PC_OK;
            }
        }
        f.image_data = enc;
    }
    free(planes);
    /* Resources. */
    if (st == PC_OK) {
        uint8_t ri[16];
        uint8_t b4[4];
        pc_psd_resolution r = pc_psd_resolution_from_dpi((double)doc->resolution_dpi);
        pc_psd_resolution_write(&r, ri);
        st = add_res(&res, PC_PSD_RES_RESOLUTION_INFO, ri, sizeof ri, err);
        pc_store_be32(b4, (uint32_t)pc_f32_to_i32(roundf(doc->global_light.angle)));
        st = st == PC_OK ? add_res(&res, PC_PSD_RES_GLOBAL_ANGLE, b4, 4, err) : st;
        pc_store_be32(b4, (uint32_t)pc_f32_to_i32(roundf(doc->global_light.altitude)));
        st = st == PC_OK ? add_res(&res, PC_PSD_RES_GLOBAL_ALTITUDE, b4, 4, err) : st;
    }
    if (st == PC_OK && doc->icc_profile != NULL) {
        st = add_res(&res, PC_PSD_RES_ICC_PROFILE, pc_blob_data(doc->icc_profile),
                     pc_blob_len(doc->icc_profile), err);
    }
    if (st == PC_OK && (doc->guides.nhorizontal > 0 || doc->guides.nvertical > 0)) {
        pc_buf_truncate(&tmp, 0);
        st = guides_resource(doc, &tmp, err);
        st = st == PC_OK ? add_res(&res, 1032, tmp.data, tmp.len, err) : st;
    }
    if (st == PC_OK && nextra > 0) {
        pc_str *names = pc_malloc_array(nextra, sizeof *names);
        if (names == NULL) {
            st = pc_io_nomem(err, "writing channel names");
        }
        for (size_t i = 0; st == PC_OK && i < nextra; i++) {
            names[i] = extra[i]->name; /* borrowed */
        }
        if (st == PC_OK) {
            pc_buf_truncate(&tmp, 0);
            st = pc_io_pascal_names_resource(names, nextra, &tmp, err);
            st = st == PC_OK ? add_res(&res, 1006, tmp.data, tmp.len, err) : st;
        }
        if (st == PC_OK) {
            pc_buf_truncate(&tmp, 0);
            st = pc_io_unicode_names_resource(names, nextra, &tmp, err);
            st = st == PC_OK ? add_res(&res, 1045, tmp.data, tmp.len, err) : st;
        }
        if (st == PC_OK) {
            pc_buf_truncate(&tmp, 0);
            st = pc_io_display_info(extra, nextra, &tmp, err);
            st = st == PC_OK ? add_res(&res, PC_IO_DISPLAY_INFO, tmp.data, tmp.len, err) : st;
        }
        free(names);
    }
    if (st == PC_OK && has_qm_id) {
        uint8_t q[3];
        pc_store_be16(q, qm_id);
        q[2] = 0;
        st = add_res(&res, PC_IO_QUICK_MASK_INFO, q, 3, err);
    }
    if (st == PC_OK) {
        st = path_resources(&ex, doc, &res, err);
    }
    if (st == PC_OK) {
        bool has = false;
        pc_buf_truncate(&tmp, 0);
        st = link_group_resource(&ex, &has, &tmp, err);
        if (st == PC_OK && has) {
            st = add_res(&res, PC_PSD_RES_LAYER_GROUP_INFO, tmp.data, tmp.len, err);
        }
    }
    if (st == PC_OK && doc->metadata.has_xmp) {
        /* The pixels are saved as they are shown: never let a reader rotate them again. */
        char *x = NULL;
        size_t xl = 0;
        st = pc_upright_xmp(pc_str_c(&doc->metadata.xmp), doc->metadata.xmp.len, &x, &xl, err);
        if (st == PC_OK) {
            st = x != NULL ? add_res(&res, PC_PSD_RES_XMP, x, xl, err)
                           : add_res(&res, PC_PSD_RES_XMP, pc_str_c(&doc->metadata.xmp),
                                     doc->metadata.xmp.len, err);
        }
        free(x);
    }
    if (st == PC_OK && doc->metadata.exif != NULL) {
        uint8_t *x = NULL;
        size_t xl = 0;
        st = pc_upright_exif(pc_blob_data(doc->metadata.exif), pc_blob_len(doc->metadata.exif), &x, &xl, err);
        if (st == PC_OK) {
            st = x != NULL ? add_res(&res, PC_PSD_RES_EXIF, x, xl, err)
                           : add_res(&res, PC_PSD_RES_EXIF, pc_blob_data(doc->metadata.exif),
                                     pc_blob_len(doc->metadata.exif), err);
        }
        free(x);
    }
    if (st == PC_OK) {
        st = global_blocks(&ex, doc, &gblocks, &ngblocks, err);
    }
    if (st == PC_OK && ex.comps) {
        st = pc_io_write_comps_resource(doc, &comps_has, &comps_buf, err);
    }
    if (st == PC_OK) {
        pc_layer_id *ids = NULL;
        uint32_t *psd = NULL;
        size_t nids = 0;
        st = id_arrays(&ex, doc, &ids, &psd, &nids, err);
        if (st == PC_OK) {
            pc_io_layer_id_map m = {ids, psd, nids};
            st = pc_io_slices_export_resource(doc, &m, &slices_action, &slices_buf, &ex.warnings, err);
        }
        free(ids);
        free(psd);
    }
    if (st == PC_OK) {
        st = preserved_resources(doc, ex.comps, slices_action != PC_IO_SLICES_KEEP, &res, err);
    }
    if (st == PC_OK) {
        bool has = false;
        pc_buf_truncate(&tmp, 0);
        st = pc_io_fresh_scale_resource(doc, &has, &tmp, err);
        if (st == PC_OK && has) {
            st = add_res(&res, PC_IO_MEASUREMENT_SCALE, tmp.data, tmp.len, err);
        }
    }
    if (st == PC_OK && comps_has) {
        st = add_res(&res, PC_IO_LAYER_COMPS, comps_buf.data, comps_buf.len, err);
    }
    if (st == PC_OK && slices_action == PC_IO_SLICES_WRITE) {
        st = add_res(&res, PC_IO_SLICES, slices_buf.data, slices_buf.len, err);
    }
    if (st == PC_OK) {
        pc_psd_resource *v = pc_grow_array(res.v, &res.cap, res.n + 1, sizeof *v);
        if (v == NULL) {
            st = pc_io_nomem(err, "adding a resource");
        } else {
            res.v = v;
            st = pc_psd_version_info_resource(true, &res.v[res.n], err);
            res.n += st == PC_OK ? 1 : 0;
        }
    }
    free(extra);
    pc_buf_fini(&tmp);
    pc_buf_fini(&comps_buf);
    pc_buf_fini(&slices_buf);
    if (st == PC_OK) {
        bool has_layers = ex.nrecords > 0;
        if (has_layers && sample != PC_SAMPLE_TYPE_U8) {
            f.placement.where = PC_PSD_LI_GLOBAL_BLOCK;
            f.placement.index = 0;
            memcpy(f.placement.signature, "8BIM", 4);
            memcpy(f.placement.key, sample == PC_SAMPLE_TYPE_U16 ? "Lr16" : "Lr32", 4);
        } else {
            f.placement.where = PC_PSD_LI_SECTION;
        }
        f.has_layer_info = has_layers;
        if (has_layers) {
            pc_error pe;
            f.layer_info.merged_alpha = has_alpha;
            f.layer_info.layers = ex.records;
            f.layer_info.n = ex.nrecords;
            ex.records = NULL;
            ex.nrecords = 0;
            /* Photoshop pads the layer info to 4 bytes (an even pad misaligns readers after a
             * global Lr16 / Lr32 block, #200). A length that can't be computed keeps the even
             * default. */
            if (pc_psd_layer_info_pad_to(&f.layer_info, ex.version, 4, &pe) != PC_OK) {
                if (pe.code == PC_ERR_NOMEM) {
                    st = pc_fail(err, PC_ERR_NOMEM, "%s", pe.msg);
                }
                pc_psd_opt_bytes_fini(&f.layer_info.padding);
                f.layer_info.padding.has = false;
            }
        }
        /* 32-bit files carry Photoshop's HDR toning records as Color Mode Data; Photoshop will
         * not open a 32-bit file without them (#291). */
        if (st == PC_OK) {
            st = pc_psd_color_mode_data_for_depth(f.header.depth, &f.color_mode_data, err);
        }
        f.resources = res.v;
        f.nresources = res.n;
        res.v = NULL;
        res.n = 0;
        f.has_global_mask = has_layers || ngblocks > 0;
        f.global_blocks = gblocks;
        f.nglobal = ngblocks;
        gblocks = NULL;
        ngblocks = 0;
    }
    pc_psd_resources_free(res.v, res.n);
    pc_psd_blocks_free(gblocks, ngblocks);
    if (st == PC_OK) {
        st = pc_io_warnings_append(warnings, &ex.warnings, err);
    }
    ex_fini(&ex);
    if (st != PC_OK) {
        pc_psd_file_fini(&f);
        return st;
    }
    *out = f;
    return PC_OK;
}

pc_status pc_io_document_to_psd_with(const pc_document *doc, const pc_io_psd_export_opts *opts,
                                     pc_psd_file *out, pc_io_warnings *warnings, pc_error *err)
{
    pc_io_psd_export_opts dflt = pc_io_psd_export_opts_default();
    return pc_io_document_to_psd_nested(doc, opts != NULL ? opts : &dflt, 0, out, warnings, err);
}

pc_status pc_io_document_to_psd(const pc_document *doc, pc_psd_file *out, pc_error *err)
{
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_status st = pc_io_document_to_psd_with(doc, NULL, out, &w, err);
    pc_io_warnings_fini(&w);
    return st;
}
