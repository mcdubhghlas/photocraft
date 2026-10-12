/* Document slices <-> PSD resource 1050 (port of crates/io/src/slices_map.rs; the resource
 * itself is src/psd/slices.h).
 *
 * Import keeps user and layer-based slices (auto slices are regenerated from them) and links
 * layer-based slices to the layer whose lyid they name. Export writes the preserved resource
 * byte-for-byte while the slices are unchanged, else a fresh version 6 resource that also lists
 * the current auto slices, as Photoshop saves them. */
#include "io/slices_map.h"

#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "base/num.h"
#include "psd/slices.h"

static const pc_doc_psd_resource *raw_res(const pc_document *doc)
{
    for (size_t i = 0; i < doc->metadata.npsd_resources; i++) {
        if (doc->metadata.psd_resources[i].id == PC_IO_SLICES) {
            return &doc->metadata.psd_resources[i];
        }
    }
    return NULL;
}

static pc_status set_str(pc_str *dst, const char *s, pc_error *err)
{
    return pc_str_set(dst, s, s != NULL ? strlen(s) : 0, err);
}

/* The layer whose PSD id is psd (the last one in walk order, like collecting into a map). */
static bool by_psd_id(const pc_walk *w, uint32_t psd, pc_layer_id *out)
{
    for (size_t i = w->len; i-- > 0;) {
        const pc_layer *l = w->items[i].layer;
        if (l->has_psd_id && l->psd_id == psd) {
            *out = l->id;
            return true;
        }
    }
    return false;
}

static pc_status slice_from_record(const pc_psd_slice *s, const pc_walk *w, pc_slice *out, pc_error *err)
{
    pc_slice c;
    pc_layer_id layer = 0;
    bool has_layer = s->has_layer_id && s->origin == 1 && by_psd_id(w, s->layer_id, &layer);
    pc_status st;
    pc_slice_default(&c);
    c.id = s->id;
    c.group_id = s->group_id;
    /* A layer-based slice whose layer is gone keeps its rect as a user slice. */
    c.origin = s->origin == 1 && has_layer ? PC_SLICE_LAYER : PC_SLICE_USER;
    c.has_layer = has_layer;
    c.layer = layer;
    c.rect = pc_rect_make(s->rect[0], s->rect[1], s->rect[2], s->rect[3]);
    c.kind = pc_slice_kind_from_code(s->kind);
    c.cell_text_is_html = s->cell_text_is_html;
    c.horizontal_align = s->horizontal_align;
    c.vertical_align = s->vertical_align;
    c.has_background = s->color[0] != 0;
    memcpy(c.background, s->color, 4);
    memcpy(c.outsets, s->outsets, sizeof c.outsets);
    st = set_str(&c.name, s->name, err);
    if (st == PC_OK) {
        st = set_str(&c.url, s->url, err);
    }
    if (st == PC_OK) {
        st = set_str(&c.target, s->target, err);
    }
    if (st == PC_OK) {
        st = set_str(&c.message, s->message, err);
    }
    if (st == PC_OK) {
        st = set_str(&c.alt, s->alt, err);
    }
    if (st == PC_OK) {
        st = set_str(&c.cell_text, s->cell_text, err);
    }
    if (st != PC_OK) {
        pc_slice_fini(&c);
        return st;
    }
    *out = c;
    return PC_OK;
}

pc_status pc_io_slices_from_psd(const uint8_t *raw, size_t len, const pc_document *doc, pc_slices *out,
                                pc_error *err)
{
    pc_psd_slices res;
    pc_slices s;
    pc_walk w;
    pc_status st;
    memset(&s, 0, sizeof s);
    if (raw == NULL) {
        *out = s;
        return PC_OK;
    }
    st = pc_psd_slices_parse(raw, len, &res, err);
    if (st == PC_ERR_NOMEM) {
        return st;
    }
    if (st != PC_OK) {
        *out = s;
        return PC_OK;
    }
    st = pc_document_walk(doc, &w, err);
    if (st != PC_OK) {
        pc_psd_slices_fini(&res);
        return st;
    }
    if (res.n > 0) {
        s.list = pc_calloc_array(res.n, sizeof *s.list);
        if (s.list == NULL) {
            st = pc_io_nomem(err, "reading slices");
        }
    }
    for (size_t i = 0; i < res.n && st == PC_OK; i++) {
        if (res.slices[i].origin == 0) {
            continue;
        }
        st = slice_from_record(&res.slices[i], &w, &s.list[s.len], err);
        if (st == PC_OK) {
            s.len++;
        }
    }
    if (st == PC_OK) {
        st = set_str(&s.group_name, res.group_name, err);
    }
    pc_walk_fini(&w);
    pc_psd_slices_fini(&res);
    if (st != PC_OK) {
        pc_slices_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}

pc_status pc_io_slices_import(pc_document *doc, pc_error *err)
{
    const pc_doc_psd_resource *r = raw_res(doc);
    pc_slices s;
    pc_status st;
    const uint8_t *data = r != NULL ? pc_blob_data(r->data) : NULL;
    if (r != NULL && data == NULL) {
        data = (const uint8_t *)"";
    }
    st = pc_io_slices_from_psd(data, r != NULL ? pc_blob_len(r->data) : 0, doc, &s, err);
    if (st != PC_OK) {
        return st;
    }
    pc_slices_fini(&doc->slices);
    doc->slices = s;
    return PC_OK;
}

pc_status pc_io_slices_unchanged(const pc_document *doc, bool *out, pc_error *err)
{
    const pc_doc_psd_resource *r = raw_res(doc);
    pc_slices s;
    const uint8_t *data = r != NULL ? pc_blob_data(r->data) : NULL;
    pc_status st;
    if (r != NULL && data == NULL) {
        data = (const uint8_t *)"";
    }
    st = pc_io_slices_from_psd(data, r != NULL ? pc_blob_len(r->data) : 0, doc, &s, err);
    if (st != PC_OK) {
        return st;
    }
    *out = pc_slices_eq(&s, &doc->slices);
    pc_slices_fini(&s);
    return PC_OK;
}

static char *dup_str(const pc_str *s)
{
    return pc_strndup(pc_str_c(s), strlen(pc_str_c(s)));
}

static pc_status record_of(const pc_slice *s, const pc_io_layer_id_map *ids, pc_psd_slice *r, pc_error *err)
{
    pc_status st = pc_psd_slice_init(r, err);
    char **fields[6];
    const pc_str *src[6];
    if (st != PC_OK) {
        return st;
    }
    r->id = s->id;
    r->group_id = s->group_id;
    r->origin = pc_slice_origin_code(s->origin);
    if (s->has_layer && ids != NULL) {
        for (size_t i = 0; i < ids->n; i++) {
            if (ids->ids[i] == s->layer) {
                r->has_layer_id = true;
                r->layer_id = ids->psd[i];
                break;
            }
        }
    }
    /* A layer slice whose layer isn't written becomes a user slice. */
    if (r->origin == 1 && !r->has_layer_id) {
        r->origin = 2;
    }
    r->kind = pc_slice_kind_code(s->kind);
    r->rect[0] = s->rect.x0;
    r->rect[1] = s->rect.y0;
    r->rect[2] = s->rect.x1;
    r->rect[3] = s->rect.y1;
    r->cell_text_is_html = s->cell_text_is_html;
    r->horizontal_align = s->horizontal_align;
    r->vertical_align = s->vertical_align;
    if (s->has_background) {
        memcpy(r->color, s->background, 4);
    }
    memcpy(r->outsets, s->outsets, sizeof r->outsets);
    fields[0] = &r->name;
    fields[1] = &r->url;
    fields[2] = &r->target;
    fields[3] = &r->message;
    fields[4] = &r->alt;
    fields[5] = &r->cell_text;
    src[0] = &s->name;
    src[1] = &s->url;
    src[2] = &s->target;
    src[3] = &s->message;
    src[4] = &s->alt;
    src[5] = &s->cell_text;
    for (size_t i = 0; i < 6; i++) {
        char *p = dup_str(src[i]);
        if (p == NULL) {
            pc_psd_slice_fini(r);
            return pc_io_nomem(err, "writing slices");
        }
        free(*fields[i]);
        *fields[i] = p;
    }
    return PC_OK;
}

/* Sorted set of used slice ids. */
static bool used_has(const uint32_t *v, size_t n, uint32_t id, size_t *at)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *at = lo;
    return lo < n && v[lo] == id;
}

static void used_insert(uint32_t *v, size_t *n, uint32_t id)
{
    size_t at;
    if (!used_has(v, *n, id, &at)) {
        memmove(&v[at + 1], &v[at], (*n - at) * sizeof *v);
        v[at] = id;
        (*n)++;
    }
}

pc_status pc_io_slices_export_resource(const pc_document *doc, const pc_io_layer_id_map *ids,
                                       pc_io_slices_action *action, pc_buf *out, pc_io_warnings *warnings,
                                       pc_error *err)
{
    bool same = false;
    pc_psd_slices res;
    pc_rect *taken = NULL;
    pc_rect *autos = NULL;
    size_t nauto = 0;
    uint32_t *used = NULL;
    size_t nused = 0;
    bool has_next;
    uint32_t next_id = 1;
    bool omitted = false;
    pc_psd_bytes bytes;
    pc_buf b = PC_BUF_INIT;
    size_t n = doc->slices.len;
    pc_status st = pc_io_slices_unchanged(doc, &same, err);
    if (st != PC_OK) {
        return st;
    }
    if (same) {
        *action = PC_IO_SLICES_KEEP;
        return PC_OK;
    }
    if (pc_slices_is_empty(&doc->slices)) {
        *action = PC_IO_SLICES_REMOVE;
        return PC_OK;
    }
    memset(&res, 0, sizeof res);
    taken = pc_malloc_array(n > 0 ? n : 1, sizeof *taken);
    if (taken == NULL) {
        return pc_io_nomem(err, "writing slices");
    }
    for (size_t i = 0; i < n; i++) {
        taken[i] = doc->slices.list[i].rect;
    }
    st = pc_auto_slices(pc_document_bounds(doc), taken, n, &autos, &nauto, err);
    free(taken);
    if (st != PC_OK) {
        return st;
    }
    res.slices = pc_calloc_array(n + nauto > 0 ? n + nauto : 1, sizeof *res.slices);
    used = pc_malloc_array(n + nauto > 0 ? n + nauto : 1, sizeof *used);
    if (res.slices == NULL || used == NULL) {
        st = pc_io_nomem(err, "writing slices");
        goto done;
    }
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        st = record_of(&doc->slices.list[i], ids, &res.slices[res.n], err);
        if (st == PC_OK) {
            res.n++;
            used_insert(used, &nused, doc->slices.list[i].id);
        }
    }
    /* Keep the usual above-max numbering when possible; if stored ids include u32::MAX, use the
     * first available id instead of wrapping or colliding with a reserved one. */
    has_next = pc_slices_next_id(&doc->slices, &next_id);
    if (!has_next) {
        next_id = 1;
        has_next = true;
    }
    for (size_t k = 0; k < nauto && st == PC_OK; k++) {
        uint32_t id;
        size_t at;
        pc_psd_slice *r;
        if (!has_next) {
            omitted = true;
            break;
        }
        id = next_id;
        while (used_has(used, nused, id, &at)) {
            if (id == UINT32_MAX) {
                omitted = true;
                break;
            }
            id++;
        }
        if (omitted) {
            break;
        }
        used_insert(used, &nused, id);
        has_next = id != UINT32_MAX;
        next_id = has_next ? id + 1 : 0;
        r = &res.slices[res.n];
        st = pc_psd_slice_init(r, err);
        if (st != PC_OK) {
            break;
        }
        res.n++;
        r->id = id;
        r->origin = 0;
        r->kind = 1;
        r->rect[0] = autos[k].x0;
        r->rect[1] = autos[k].y0;
        r->rect[2] = autos[k].x1;
        r->rect[3] = autos[k].y1;
    }
    if (st == PC_OK) {
        if (doc->slices.group_name.len == 0) {
            pc_str base = PC_STR_INIT;
            st = pc_slices_base_name(doc, &base, err);
            if (st == PC_OK) {
                res.group_name = dup_str(&base);
                if (res.group_name == NULL) {
                    st = pc_io_nomem(err, "writing slices");
                }
            }
            pc_str_fini(&base);
        } else {
            res.group_name = dup_str(&doc->slices.group_name);
            if (res.group_name == NULL) {
                st = pc_io_nomem(err, "writing slices");
            }
        }
    }
    if (st == PC_OK) {
        res.version = 6;
        res.bounds[0] = 0;
        res.bounds[1] = 0;
        res.bounds[2] = pc_u32_as_i32(doc->size.height);
        res.bounds[3] = pc_u32_as_i32(doc->size.width);
        st = pc_psd_slices_to_bytes(&res, &bytes, err);
        if (st == PC_OK) {
            st = pc_buf_append(&b, bytes.data, bytes.len, err);
            pc_psd_bytes_fini(&bytes);
        }
    }
    if (st == PC_OK && omitted && warnings != NULL) {
        st = pc_io_warn(warnings, err, "auto slices omitted because the slice id space is exhausted");
    }
done:
    free(autos);
    free(used);
    pc_psd_slices_fini(&res);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    *action = PC_IO_SLICES_WRITE;
    return PC_OK;
}
