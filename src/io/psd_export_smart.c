/* Smart objects in PSD export (port of the SmartOut half of crates/io/src/psd_export.rs).
 *
 * An imported placed layer nothing changed is written verbatim; otherwise its PlLd and SoLd are
 * regenerated (from the imported descriptor where the source is the same). Sources become lnk2
 * items: a preserved embedded file, a .pcraft bundle exported as a PSB of the nested document,
 * an image file as it is, or a linked file read from disk and embedded. Smart objects with
 * filters get an FEid cache item holding the unfiltered placed pixels and the filter mask.
 * Without a source that can be embedded the layer is written as pixels, with a warning. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/warp.h"
#include "base/bytes.h"
#include "base/file.h"
#include "base/hash.h"
#include "base/mem.h"
#include "base/num.h"
#include "io/blocks.h"
#include "compose/compose.h"
#include "format/format.h"
#include "io/io.h"
#include "io/psd_export_internal.h"
#include "io/smart_internal.h"
#include "raster/pixel.h"

static const char LINK_KEYS[4][5] = {"lnk2", "lnk3", "lnkD", "lnkE"};

static bool is_link_key(const uint8_t k[4])
{
    for (size_t i = 0; i < 4; i++) {
        if (memcmp(k, LINK_KEYS[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_fx_key(const uint8_t k[4])
{
    return memcmp(k, "FEid", 4) == 0 || memcmp(k, "FXid", 4) == 0;
}

bool pc_io_ex_strs_has(const pc_io_strs *s, const char *p, size_t len)
{
    for (size_t i = 0; i < s->n; i++) {
        if (pc_str_eq_mem(&s->v[i], p, len)) {
            return true;
        }
    }
    return false;
}

static pc_status strs_insert(pc_io_strs *s, const char *p, size_t len, pc_error *err)
{
    return pc_io_ex_strs_has(s, p, len) ? PC_OK : pc_io_strs_push(s, p, len, err);
}

static pc_status push_fx(pc_psd_fx_item **v, size_t *n, size_t *cap, pc_psd_fx_item *it, pc_error *err)
{
    pc_psd_fx_item *g = pc_grow_array(*v, cap, *n + 1, sizeof *g);
    if (g == NULL) {
        pc_psd_fx_item_fini(it);
        return pc_io_nomem(err, "collecting filter caches");
    }
    *v = g;
    g[(*n)++] = *it;
    memset(it, 0, sizeof *it);
    return PC_OK;
}

pc_status pc_io_ex_smart_init(pc_io_ex_smart *s, const pc_document *doc, uint32_t depth, pc_error *err)
{
    pc_status st = PC_OK;
    memset(s, 0, sizeof *s);
    s->depth = depth;
    s->prune_ok = true;
    s->globals = doc->metadata.psd_global_blocks;
    s->nglobals = doc->metadata.npsd_global_blocks;
    for (size_t i = 0; i < s->nglobals && st == PC_OK; i++) {
        const pc_doc_psd_global_block *g = &s->globals[i];
        if (is_link_key(g->key)) {
            pc_io_strs u = {NULL, 0, 0};
            st = pc_io_linked_block_uuids(pc_blob_data(g->data), pc_blob_len(g->data), &u, err);
            for (size_t k = 0; k < u.n && st == PC_OK; k++) {
                st = pc_io_strs_push(&s->known, pc_str_c(&u.v[k]), u.v[k].len, err);
            }
            pc_io_strs_fini(&u);
        } else if (is_fx_key(g->key)) {
            pc_psd_filter_effects fx;
            pc_error pe;
            pc_status ps = pc_psd_filter_effects_parse(pc_blob_data(g->data), pc_blob_len(g->data), &fx, &pe);
            if (ps == PC_ERR_NOMEM) {
                st = pc_fail(err, PC_ERR_NOMEM, "%s", pe.msg);
            } else if (ps == PC_OK) {
                for (size_t k = 0; k < fx.n && st == PC_OK; k++) {
                    st = push_fx(&s->old_fx, &s->nold_fx, &s->capold_fx, &fx.items[k], err);
                }
                pc_psd_filter_effects_fini(&fx);
            }
        }
    }
    if (st != PC_OK) {
        pc_io_ex_smart_fini(s);
    }
    return st;
}

void pc_io_ex_smart_fini(pc_io_ex_smart *s)
{
    pc_io_strs_fini(&s->known);
    pc_io_strs_fini(&s->used);
    pc_io_linked_files_fini(&s->add);
    for (size_t i = 0; i < s->nconverted; i++) {
        pc_str_fini(&s->converted[i].src.uuid);
        free(s->converted[i].reason);
    }
    free(s->converted);
    for (size_t i = 0; i < s->ndocs; i++) {
        free(s->docs[i].uuid);
        pc_document_fini(&s->docs[i].doc);
    }
    free(s->docs);
    for (size_t i = 0; i < s->ncomposites; i++) {
        free(s->composites[i].uuid);
        if (s->composites[i].has) {
            pc_surface_fini(&s->composites[i].surface);
        }
    }
    free(s->composites);
    for (size_t i = 0; i < s->nold_fx; i++) {
        pc_psd_fx_item_fini(&s->old_fx[i]);
    }
    free(s->old_fx);
    pc_io_strs_fini(&s->keep_fx);
    for (size_t i = 0; i < s->nnew_fx; i++) {
        pc_psd_fx_item_fini(&s->new_fx[i]);
    }
    free(s->new_fx);
    memset(s, 0, sizeof *s);
}

/* FEid data padded to 4 bytes inside the block length: Photoshop counts the padding in the
 * length and can't open a file whose FEid is padded outside it. */
static pc_status padded_fx(const pc_psd_filter_effects *fx, pc_blob **out, pc_error *err)
{
    pc_psd_bytes b = {NULL, 0};
    pc_buf buf = PC_BUF_INIT;
    pc_status st = pc_psd_filter_effects_to_bytes(fx, &b, err);
    if (st == PC_OK) {
        st = pc_buf_append(&buf, b.data, b.len, err);
    }
    pc_psd_bytes_fini(&b);
    while (st == PC_OK && buf.len % 4 != 0) {
        st = pc_buf_append_byte(&buf, 0, err);
    }
    if (st == PC_OK) {
        st = pc_blob_new(buf.data, buf.len, out, err);
    }
    pc_buf_fini(&buf);
    return st;
}

static bool keep_uuid(void *ctx, const char *uuid, size_t len)
{
    const pc_io_ex_smart *s = ctx;
    return !s->prune_ok || pc_io_ex_strs_has(&s->used, uuid, len);
}

static pc_status out_push(pc_doc_psd_global_block **v, size_t *n, size_t *cap, const uint8_t sig[4],
                          const char key[4], pc_blob *data, pc_error *err)
{
    pc_doc_psd_global_block *g = pc_grow_array(*v, cap, *n + 1, sizeof *g);
    if (g == NULL) {
        pc_blob_release(data);
        return pc_io_nomem(err, "writing global blocks");
    }
    *v = g;
    memcpy(g[*n].signature, sig, 4);
    memcpy(g[*n].key, key, 4);
    g[*n].data = data;
    (*n)++;
    return PC_OK;
}

static pc_status fx_items_clone(const pc_psd_fx_item *src, size_t n, pc_psd_fx_item **out, size_t *nout,
                                size_t *cap, pc_error *err)
{
    pc_status st = PC_OK;
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        pc_psd_fx_item c;
        st = pc_io_fx_item_clone(&src[i], &c, err);
        if (st == PC_OK) {
            st = push_fx(out, nout, cap, &c, err);
        }
    }
    return st;
}

pc_status pc_io_ex_smart_finish(pc_io_ex_smart *s, pc_doc_psd_global_block *blocks, size_t n,
                                pc_doc_psd_global_block **out, size_t *nout, pc_error *err)
{
    static const uint8_t SIG[4] = {'8', 'B', 'I', 'M'};
    pc_doc_psd_global_block *o = NULL;
    size_t no = 0;
    size_t cap = 0;
    bool added = false;
    bool fx_placed = false;
    pc_status st = PC_OK;
    size_t i = 0;
    for (; i < n && st == PC_OK; i++) {
        pc_doc_psd_global_block *b = &blocks[i];
        if (is_link_key(b->key)) {
            bool lnk2 = memcmp(b->key, "lnk2", 4) == 0;
            bool give = !added && lnk2;
            bool changed = false;
            pc_buf nb = PC_BUF_INIT;
            added = added || lnk2;
            st = pc_io_rebuild_linked_block(pc_blob_data(b->data), pc_blob_len(b->data), keep_uuid, s,
                                            give ? s->add.v : NULL, give ? s->add.n : 0, &changed, &nb, err);
            if (st == PC_OK && !changed) {
                st = out_push(&o, &no, &cap, b->signature, (const char *)b->key, b->data, err);
                b->data = NULL;
            } else if (st == PC_OK && nb.len > 0) {
                pc_blob *d = NULL;
                st = pc_blob_new(nb.data, nb.len, &d, err);
                if (st == PC_OK) {
                    st = out_push(&o, &no, &cap, b->signature, (const char *)b->key, d, err);
                }
            }
            pc_buf_fini(&nb);
        } else if (is_fx_key(b->key)) {
            pc_psd_filter_effects fx;
            pc_error pe;
            pc_status ps = pc_psd_filter_effects_parse(pc_blob_data(b->data), pc_blob_len(b->data), &fx, &pe);
            size_t before;
            size_t k = 0;
            bool fresh;
            if (ps == PC_ERR_NOMEM) {
                st = pc_fail(err, PC_ERR_NOMEM, "%s", pe.msg);
                break;
            }
            if (ps != PC_OK) {
                st = out_push(&o, &no, &cap, b->signature, (const char *)b->key, b->data, err);
                b->data = NULL;
                continue;
            }
            before = fx.n;
            for (size_t j = 0; j < fx.n; j++) {
                const char *id = fx.items[j].id != NULL ? fx.items[j].id : "";
                if (pc_io_ex_strs_has(&s->keep_fx, id, strlen(id))) {
                    fx.items[k++] = fx.items[j];
                } else {
                    pc_psd_fx_item_fini(&fx.items[j]);
                }
            }
            fx.n = k;
            fresh = !fx_placed && s->nnew_fx > 0;
            if (fresh) {
                size_t cap_items = fx.n;
                st = fx_items_clone(s->new_fx, s->nnew_fx, &fx.items, &fx.n, &cap_items, err);
                fx_placed = true;
            }
            if (st == PC_OK && fx.n == before && !fresh) {
                st = out_push(&o, &no, &cap, b->signature, (const char *)b->key, b->data, err);
                b->data = NULL;
            } else if (st == PC_OK && fx.n > 0) {
                pc_blob *d = NULL;
                st = padded_fx(&fx, &d, err);
                if (st == PC_OK) {
                    st = out_push(&o, &no, &cap, b->signature, (const char *)b->key, d, err);
                }
            }
            pc_psd_filter_effects_fini(&fx);
        } else {
            st = out_push(&o, &no, &cap, b->signature, (const char *)b->key, b->data, err);
            b->data = NULL;
        }
    }
    if (st == PC_OK && !added && s->add.n > 0) {
        pc_buf nb = PC_BUF_INIT;
        pc_blob *d = NULL;
        for (size_t k = 0; k < s->add.n && st == PC_OK; k++) {
            st = pc_io_encode_linked_file(&s->add.v[k], &nb, err);
        }
        if (st == PC_OK) {
            st = pc_blob_new(nb.data, nb.len, &d, err);
        }
        if (st == PC_OK) {
            st = out_push(&o, &no, &cap, SIG, "lnk2", d, err);
        }
        pc_buf_fini(&nb);
    }
    if (st == PC_OK && !fx_placed && s->nnew_fx > 0) {
        pc_psd_filter_effects fx = {3, NULL, 0};
        size_t cap_items = 0;
        pc_blob *d = NULL;
        bool has_fmsk = false;
        st = fx_items_clone(s->new_fx, s->nnew_fx, &fx.items, &fx.n, &cap_items, err);
        if (st == PC_OK) {
            st = padded_fx(&fx, &d, err);
        }
        pc_psd_filter_effects_fini(&fx);
        if (st == PC_OK) {
            st = out_push(&o, &no, &cap, SIG, "FEid", d, err);
        }
        for (size_t k = 0; k < no; k++) {
            has_fmsk = has_fmsk || memcmp(o[k].key, "FMsk", 4) == 0;
        }
        if (st == PC_OK && !has_fmsk) {
            /* Filter mask overlay: RGB red at 50 %, Photoshop's default. */
            static const uint8_t FMSK[12] = {0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 50};
            st = pc_blob_new(FMSK, sizeof FMSK, &d, err);
            if (st == PC_OK) {
                st = out_push(&o, &no, &cap, SIG, "FMsk", d, err);
            }
        }
    }
    for (size_t k = 0; k < n; k++) {
        pc_blob_release(blocks[k].data);
    }
    free(blocks);
    if (st != PC_OK) {
        for (size_t k = 0; k < no; k++) {
            pc_blob_release(o[k].data);
        }
        free(o);
        return st;
    }
    *out = o;
    *nout = no;
    return PC_OK;
}

/* ---- sources ---- */

/* The document's global blocks as metadata (borrowed: never finalised). */
static pc_metadata globals_meta(const pc_io_ex_smart *s)
{
    pc_metadata m;
    memset(&m, 0, sizeof m);
    m.psd_global_blocks = (pc_doc_psd_global_block *)(uintptr_t)s->globals;
    m.npsd_global_blocks = s->nglobals;
    return m;
}

/* Size and resolution of a smart-object source file (PSD/PSB header, .pcraft, or any image).
 * *ok false when it can't be read (the reference's Err, whose text nobody shows). */
static pc_status source_geometry(const char *name, const uint8_t *bytes, size_t len, bool *ok, double size[2],
                                 double *dpi, pc_error *err)
{
    pc_io_import_result r;
    pc_error ie;
    pc_status st;
    *ok = false;
    if (pc_io_is_psd(bytes, len)) {
        if (len < 22) {
            return PC_OK;
        }
        size[0] = (double)pc_load_be32(bytes + 18);
        size[1] = (double)pc_load_be32(bytes + 14);
        *dpi = 72.0;
        *ok = true;
        return PC_OK;
    }
    st = pc_io_import(name, bytes, len, &r, &ie);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", ie.msg);
    }
    if (st != PC_OK) {
        return PC_OK;
    }
    size[0] = (double)r.document.size.width;
    size[1] = (double)r.document.size.height;
    *dpi = (double)r.document.resolution_dpi;
    *ok = true;
    pc_io_import_result_fini(&r);
    return PC_OK;
}

/* The source size of a smart object whose file can't be decoded: its rendered bounds taken back
 * through the inverse transform (the source's top-left is the origin). */
static void size_from_layer(const pc_smart_object *sm, double out[2])
{
    pc_rect r;
    pc_affine inv;
    double w = 1.0;
    double h = 1.0;
    out[0] = 1.0;
    out[1] = 1.0;
    if (!sm->has_cache) {
        return;
    }
    r = pc_surface_content_bounds(&sm->cache);
    if (pc_rect_is_empty(r) || !pc_affine_inverse(sm->transform, &inv)) {
        return;
    }
    {
        const double *m = inv.m;
        const int32_t cs[4][2] = {{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}};
        for (size_t i = 0; i < 4; i++) {
            double x = (double)cs[i][0];
            double y = (double)cs[i][1];
            w = fmax(w, round(m[0] * x + m[2] * y + m[4]));
            h = fmax(h, round(m[1] * x + m[3] * y + m[5]));
        }
    }
    out[0] = w;
    out[1] = h;
}

static pc_status source_set(pc_io_ex_source *s, const char *uuid, size_t len, const double size[2],
                            double dpi, pc_error *err)
{
    memset(s, 0, sizeof *s);
    s->size[0] = size[0];
    s->size[1] = size[1];
    s->dpi = dpi;
    return pc_str_set(&s->uuid, uuid, len, err);
}

/* convert_source: one new embedded file (a .pcraft bundle becomes a PSB of the nested document).
 * *reason (malloc'd) set and PC_OK returned when it can't be embedded. */
static pc_status convert_source(pc_io_ex *ex, const char *file_name, size_t fn_len, const uint8_t *bytes,
                                size_t len, pc_io_ex_source *out, char **reason, pc_error *err)
{
    pc_io_ex_smart *s = &ex->smart;
    char uuid[37];
    pc_str name = PC_STR_INIT;
    uint8_t *data = NULL;
    size_t dlen = 0;
    double size[2] = {0.0, 0.0};
    double dpi = (double)ex->dpi;
    bool nested = false;
    pc_document doc;
    pc_status st = PC_OK;
    char msg[PC_ERROR_MSG_MAX + 64];
    *reason = NULL;
    if (pc_format_is_pcraft(bytes, len)) {
        pc_io_psd_export_opts o = pc_io_psd_export_opts_default();
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        pc_psd_file file;
        pc_psd_bytes fb = {NULL, 0};
        pc_error fe;
        size_t stem_len = fn_len;
        if (s->depth >= PC_IO_MAX_NESTING) {
            (void)snprintf(msg, sizeof msg, "smart objects nest more than %u deep", PC_IO_MAX_NESTING);
            *reason = pc_strndup(msg, strlen(msg));
            return *reason != NULL ? PC_OK : pc_io_nomem(err, "describing a smart object");
        }
        st = pc_format_load_from_bytes(bytes, len, &doc, &fe);
        if (st == PC_ERR_NOMEM) {
            return pc_fail(err, PC_ERR_NOMEM, "%s", fe.msg);
        }
        if (st != PC_OK) {
            (void)snprintf(msg, sizeof msg, "its contents can't be read: %s", fe.msg);
            *reason = pc_strndup(msg, strlen(msg));
            return *reason != NULL ? PC_OK : pc_io_nomem(err, "describing a smart object");
        }
        o.force_psb = true;
        /* The reference's nested export can't fail; here a limit (or anything but memory) makes
         * the source one that can't be embedded, like an unwritable file. */
        st = pc_io_document_to_psd_nested(&doc, &o, s->depth + 1, &file, &w, &fe);
        if (st == PC_OK) {
            st = pc_psd_file_to_bytes(&file, &fb, &fe);
            pc_psd_file_fini(&file);
        }
        if (st == PC_ERR_NOMEM) {
            st = pc_fail(err, PC_ERR_NOMEM, "%s", fe.msg);
        } else if (st != PC_OK) {
            (void)snprintf(msg, sizeof msg, "its contents can't be written: %s", fe.msg);
            pc_io_warnings_fini(&w);
            pc_document_fini(&doc);
            *reason = pc_strndup(msg, strlen(msg));
            return *reason != NULL ? PC_OK : pc_io_nomem(err, "describing a smart object");
        }
        /* stem: the name before its last '.' */
        for (size_t i = fn_len; i > 0; i--) {
            if (file_name[i - 1] == '.') {
                stem_len = i - 1;
                break;
            }
        }
        for (size_t i = 0; i < w.n && st == PC_OK; i++) {
            st = pc_io_warn(&ex->warnings, err, "smart object %.*s: %s",
                            (int)(stem_len > 4096 ? 4096 : stem_len), file_name, w.v[i]);
        }
        pc_io_warnings_fini(&w);
        if (st == PC_OK) {
            st = pc_str_set(&name, file_name, stem_len, err);
        }
        if (st == PC_OK) {
            st = pc_str_append(&name, ".psb", 4, err);
        }
        if (st != PC_OK) {
            pc_psd_bytes_fini(&fb);
            pc_str_fini(&name);
            pc_document_fini(&doc);
            return st;
        }
        data = fb.data;
        dlen = fb.len;
        size[0] = (double)doc.size.width;
        size[1] = (double)doc.size.height;
        dpi = (double)doc.resolution_dpi;
        nested = true;
    } else {
        /* Formats we can't decode (vector PDF/AI...) are still embedded; their size comes from
         * the layer. */
        bool ok = false;
        char *cname = pc_strndup(file_name, fn_len);
        if (cname == NULL) {
            return pc_io_nomem(err, "embedding a smart object");
        }
        st = source_geometry(cname, bytes, len, &ok, size, &dpi, err);
        free(cname);
        if (st == PC_OK && !ok) {
            size[0] = 0.0;
            size[1] = 0.0;
            dpi = (double)ex->dpi;
        }
        if (st == PC_OK) {
            st = pc_str_set(&name, file_name, fn_len, err);
        }
        if (st == PC_OK) {
            data = pc_malloc(len);
            if (data == NULL) {
                st = pc_io_nomem(err, "embedding a smart object");
            } else {
                if (len > 0) {
                    memcpy(data, bytes, len);
                }
                dlen = len;
            }
        }
        if (st != PC_OK) {
            pc_str_fini(&name);
            return st;
        }
    }
    pc_io_uuid_from(data, dlen, uuid);
    if (nested) {
        pc_io_ex_doc *g = pc_grow_array(s->docs, &s->capdocs, s->ndocs + 1, sizeof *g);
        char *u = pc_strndup(uuid, strlen(uuid));
        if (g == NULL || u == NULL) {
            free(u);
            if (g != NULL) {
                s->docs = g;
            }
            pc_document_fini(&doc);
            st = pc_io_nomem(err, "keeping a nested document");
        } else {
            s->docs = g;
            g[s->ndocs].uuid = u;
            g[s->ndocs].doc = doc;
            s->ndocs++;
        }
    }
    if (st == PC_OK && !pc_io_ex_strs_has(&s->known, uuid, strlen(uuid))) {
        bool have = false;
        for (size_t i = 0; i < s->add.n && !have; i++) {
            have = pc_str_eq_c(&s->add.v[i].uuid, uuid);
        }
        if (!have) {
            pc_io_linked_file *g = pc_grow_array(s->add.v, &s->add.cap, s->add.n + 1, sizeof *g);
            if (g == NULL) {
                st = pc_io_nomem(err, "embedding a smart object");
            } else {
                s->add.v = g;
                memset(&g[s->add.n], 0, sizeof g[s->add.n]);
                st = pc_str_set_c(&g[s->add.n].uuid, uuid, err);
                if (st == PC_OK) {
                    g[s->add.n].file_name = name;
                    name.ptr = NULL;
                    name.len = 0;
                    g[s->add.n].bytes = data;
                    g[s->add.n].len = dlen;
                    data = NULL;
                    s->add.n++;
                } else {
                    pc_str_fini(&g[s->add.n].uuid);
                }
            }
        }
    }
    free(data);
    pc_str_fini(&name);
    if (st != PC_OK) {
        return st;
    }
    return source_set(out, uuid, strlen(uuid), size, dpi, err);
}

/* embed: adds bytes as an embedded file, once per distinct content. */
static pc_status embed(pc_io_ex *ex, const char *file_name, size_t fn_len, const uint8_t *bytes, size_t len,
                       pc_io_ex_source *out, char **reason, pc_error *err)
{
    pc_io_ex_smart *s = &ex->smart;
    uint8_t key[32];
    pc_io_ex_converted c;
    pc_io_ex_converted *g;
    pc_status st;
    pc_blake3(bytes, len, key);
    for (size_t i = 0; i < s->nconverted; i++) {
        if (memcmp(s->converted[i].key, key, 32) == 0) {
            if (!s->converted[i].ok) {
                *reason = pc_strndup(s->converted[i].reason, strlen(s->converted[i].reason));
                return *reason != NULL ? PC_OK : pc_io_nomem(err, "embedding a smart object");
            }
            *reason = NULL;
            return source_set(out, pc_str_c(&s->converted[i].src.uuid), s->converted[i].src.uuid.len,
                              s->converted[i].src.size, s->converted[i].src.dpi, err);
        }
    }
    memset(&c, 0, sizeof c);
    memcpy(c.key, key, 32);
    st = convert_source(ex, file_name, fn_len, bytes, len, &c.src, &c.reason, err);
    if (st != PC_OK) {
        return st;
    }
    c.ok = c.reason == NULL;
    g = pc_grow_array(s->converted, &s->capconverted, s->nconverted + 1, sizeof *g);
    if (g == NULL) {
        pc_str_fini(&c.src.uuid);
        free(c.reason);
        return pc_io_nomem(err, "embedding a smart object");
    }
    s->converted = g;
    g[s->nconverted++] = c;
    if (!c.ok) {
        *reason = pc_strndup(c.reason, strlen(c.reason));
        return *reason != NULL ? PC_OK : pc_io_nomem(err, "embedding a smart object");
    }
    *reason = NULL;
    return source_set(out, pc_str_c(&c.src.uuid), c.src.uuid.len, c.src.size, c.src.dpi, err);
}

/* The last path component (split on '/' and '\\'). */
static const char *base_name(const char *p, size_t len, size_t *out_len)
{
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '/' || p[i] == '\\') {
            start = i + 1;
        }
    }
    *out_len = len - start;
    return p + start;
}

/* smart_source: the lnk2 file a smart object's source is written as; *reason set when it can't. */
static pc_status smart_source(pc_io_ex *ex, const pc_smart_object *sm, const pc_io_placed *tmpl,
                              pc_io_ex_source *out, char **reason, pc_error *err)
{
    pc_io_ex_smart *s = &ex->smart;
    const pc_smart_source *src = &sm->source;
    *reason = NULL;
    if (src->linked) {
        const char *path = pc_str_c(&src->path);
        size_t plen = src->path.len;
        bool same = tmpl != NULL && pc_str_eq(&tmpl->idnt, &src->path);
        if (pc_io_ex_strs_has(&s->known, path, plen) || same) {
            /* A file of the imported document, or one its placed-layer data names (a missing
             * link stays a smart object, as in Photoshop). */
            double size[2];
            double dpi = (double)ex->dpi;
            bool ok = same && pc_io_stored_size(&tmpl->descriptor, size);
            if (!ok) {
                pc_metadata meta = globals_meta(s);
                pc_io_linked_file f;
                bool found = false;
                pc_status st = pc_io_find_linked_file(&meta, path, plen, &found, &f, err);
                if (st != PC_OK) {
                    return st;
                }
                if (found) {
                    char *nm = pc_strndup(pc_str_c(&f.file_name), f.file_name.len);
                    st = nm != NULL ? source_geometry(nm, f.bytes, f.len, &ok, size, &dpi, err)
                                    : pc_io_nomem(err, "reading a smart object");
                    free(nm);
                    pc_io_linked_file_fini(&f);
                    if (st != PC_OK) {
                        return st;
                    }
                }
            }
            if (!ok) {
                size_from_layer(sm, size);
                dpi = (double)ex->dpi;
            }
            return source_set(out, path, plen, size, dpi, err);
        }
        {
            pc_buf b = PC_BUF_INIT;
            pc_error fe;
            size_t nlen;
            const char *nm;
            char msg[PC_ERROR_MSG_MAX + 64];
            pc_status st = pc_file_read(path, PC_MAX_ALLOC, &b, &fe);
            if (st == PC_ERR_NOMEM) {
                return pc_fail(err, PC_ERR_NOMEM, "%s", fe.msg);
            }
            if (st != PC_OK) {
                /* As Rust's io::Error Display: "<strerror> (os error N)". */
                if (fe.sys_errno != 0) {
                    (void)snprintf(msg, sizeof msg, "the linked file %s can't be read: %s (os error %d)",
                                   path, strerror(fe.sys_errno), fe.sys_errno);
                } else {
                    (void)snprintf(msg, sizeof msg, "the linked file %s can't be read: %s", path, fe.msg);
                }
                *reason = pc_strndup(msg, strlen(msg));
                return *reason != NULL ? PC_OK : pc_io_nomem(err, "reading a smart object");
            }
            nm = base_name(path, plen, &nlen);
            st = pc_io_warn(&ex->warnings, err,
                            "the linked smart object %.*s was embedded (PSD export keeps no external links)",
                            (int)(nlen > 4096 ? 4096 : nlen), nm);
            if (st == PC_OK) {
                st = embed(ex, nm, nlen, b.data, b.len, out, reason, err);
            }
            pc_buf_fini(&b);
            return st;
        }
    }
    return embed(ex, pc_str_c(&src->file_name), src->file_name.len, pc_blob_data(src->bytes),
                 pc_blob_len(src->bytes), out, reason, err);
}

typedef struct comp_sink {
    pc_surface *s;
    pc_pixel_format fmt;
} comp_sink;

static pc_status composite_band(void *ctx, pc_compose_buffer *band, pc_error *err)
{
    comp_sink *c = ctx;
    size_t ch = pc_pixel_format_channels(c->fmt);
    float *vals = pc_malloc_array(band->len > 0 ? band->len : 1, ch * sizeof *vals);
    pc_status st;
    if (vals == NULL) {
        return pc_io_nomem(err, "rendering a smart object");
    }
    for (size_t i = 0; i < band->len; i++) {
        float v[PC_RASTER_MAX_CHANNELS];
        (void)pc_from_rgba(c->fmt, band->px[i], v);
        memcpy(vals + i * ch, v, ch * sizeof *v);
    }
    st = pc_surface_write_region(c->s, band->rect, vals, band->len * ch, err);
    free(vals);
    return st;
}

/* source_composite: the composite of source uuid in the document's pixel format, its top-left
 * at the origin; decoded once. *found false when it can't be read. Borrowed. */
static pc_status source_composite(pc_io_ex *ex, const char *uuid, size_t ulen, const pc_io_ex_composite **out,
                                  pc_error *err)
{
    pc_io_ex_smart *s = &ex->smart;
    pc_io_ex_composite c;
    pc_io_ex_composite *g;
    const pc_document *doc = NULL;
    pc_io_import_result r;
    bool imported = false;
    pc_status st = PC_OK;
    for (size_t i = 0; i < s->ncomposites; i++) {
        if (strlen(s->composites[i].uuid) == ulen && memcmp(s->composites[i].uuid, uuid, ulen) == 0) {
            *out = &s->composites[i];
            return PC_OK;
        }
    }
    for (size_t i = 0; i < s->ndocs && doc == NULL; i++) {
        if (strlen(s->docs[i].uuid) == ulen && memcmp(s->docs[i].uuid, uuid, ulen) == 0) {
            doc = &s->docs[i].doc;
        }
    }
    if (doc == NULL) {
        const pc_io_linked_file *f = NULL;
        pc_io_linked_file found_file;
        bool found = false;
        for (size_t i = 0; i < s->add.n && f == NULL; i++) {
            if (pc_str_eq_mem(&s->add.v[i].uuid, uuid, ulen)) {
                f = &s->add.v[i];
            }
        }
        if (f == NULL) {
            pc_metadata meta = globals_meta(s);
            st = pc_io_find_linked_file(&meta, uuid, ulen, &found, &found_file, err);
            if (st != PC_OK) {
                return st;
            }
            f = found ? &found_file : NULL;
        }
        if (f != NULL) {
            char *nm = pc_strndup(pc_str_c(&f->file_name), f->file_name.len);
            pc_error ie;
            if (nm == NULL) {
                st = pc_io_nomem(err, "reading a smart object");
            } else {
                st = pc_io_import(nm, f->bytes, f->len, &r, &ie);
                free(nm);
                if (st == PC_ERR_NOMEM) {
                    st = pc_fail(err, PC_ERR_NOMEM, "%s", ie.msg);
                } else if (st == PC_OK) {
                    imported = true;
                    doc = &r.document;
                } else {
                    st = PC_OK;
                }
            }
        }
        if (found) {
            pc_io_linked_file_fini(&found_file);
        }
        if (st != PC_OK) {
            return st;
        }
    }
    memset(&c, 0, sizeof c);
    c.uuid = pc_strndup(uuid, ulen);
    if (c.uuid == NULL) {
        st = pc_io_nomem(err, "rendering a smart object");
    }
    if (st == PC_OK && doc != NULL) {
        comp_sink sink;
        pc_error re;
        pc_surface_init(&c.surface, ex->fmt);
        sink.s = &c.surface;
        sink.fmt = ex->fmt;
        /* The reference ignores a failed render and keeps what was drawn; out of memory still
         * fails here. */
        st = pc_compose_render_bands(doc, pc_document_bounds(doc), 0, NULL, composite_band, &sink, &re);
        if (st == PC_ERR_NOMEM) {
            st = pc_fail(err, PC_ERR_NOMEM, "%s", re.msg);
            pc_surface_fini(&c.surface);
        } else {
            st = PC_OK;
            pc_surface_prune(&c.surface);
            c.has = true;
            c.bounds = pc_document_bounds(doc);
        }
    }
    if (imported) {
        pc_io_import_result_fini(&r);
    }
    if (st == PC_OK) {
        g = pc_grow_array(s->composites, &s->capcomposites, s->ncomposites + 1, sizeof *g);
        if (g == NULL) {
            st = pc_io_nomem(err, "rendering a smart object");
        } else {
            s->composites = g;
            g[s->ncomposites] = c;
            *out = &g[s->ncomposites];
            s->ncomposites++;
            return PC_OK;
        }
    }
    free(c.uuid);
    if (c.has) {
        pc_surface_fini(&c.surface);
    }
    return st;
}

/* ---- placed-layer blocks ---- */

pc_status pc_io_ex_upsert(pc_io_raw_blocks *raw, const char key[4], uint8_t **data, size_t len,
                          const char *before, pc_error *err)
{
    for (size_t i = 0; i < raw->n; i++) {
        if (memcmp(raw->v[i].key, key, 4) == 0) {
            free(raw->v[i].data);
            raw->v[i].data = *data;
            raw->v[i].len = len;
            *data = NULL;
            return PC_OK;
        }
    }
    {
        size_t at = raw->n;
        pc_status st;
        for (size_t i = 0; before != NULL && i < raw->n; i++) {
            if (memcmp(raw->v[i].key, before, 4) == 0) {
                at = i;
                break;
            }
        }
        st = pc_io_raw_blocks_push_take(raw, (const uint8_t *)key, data, len, err);
        if (st != PC_OK) {
            return st;
        }
        if (at < raw->n - 1) {
            pc_io_raw_block b = raw->v[raw->n - 1];
            memmove(&raw->v[at + 1], &raw->v[at], (raw->n - 1 - at) * sizeof b);
            raw->v[at] = b;
        }
        return PC_OK;
    }
}

static bool raw_has(const pc_io_raw_blocks *raw, const char key[4])
{
    return pc_io_raw_blocks_find(raw, (const uint8_t *)key) != NULL;
}

/* Whether an imported placed layer still says what the smart object is: transform, warp, filter
 * stack and filter mask as stored. */
static pc_status placed_unchanged(const pc_io_ex *ex, const pc_io_placed *t, const uint8_t *data, size_t len,
                                  const pc_smart_object *sm, const pc_io_filter_stack *stack, bool *out,
                                  pc_error *err)
{
    pc_warp w;
    bool has_w = false;
    pc_io_filter_stack dflt;
    const pc_io_filter_stack *ts;
    pc_status st;
    *out = false;
    if (memcmp(t->transform.m, sm->transform.m, sizeof t->transform.m) != 0) {
        /* Affine PartialEq: exact f64 equality per element. */
        for (size_t i = 0; i < 6; i++) {
            if (!(t->transform.m[i] == sm->transform.m[i])) {
                return PC_OK;
            }
        }
    }
    st = pc_io_parse_placed_warp((const uint8_t *)"SoLd", data, len, &has_w, &w, err);
    if (st != PC_OK) {
        return st;
    }
    if (has_w != sm->has_warp || (has_w && !pc_warp_eq(&w, &sm->warp))) {
        if (has_w) {
            pc_warp_fini(&w);
        }
        return PC_OK;
    }
    if (has_w) {
        pc_warp_fini(&w);
    }
    pc_io_filter_stack_init(&dflt);
    ts = t->has_stack ? &t->stack : &dflt;
    if (!pc_io_filter_stack_eq(ts, stack)) {
        return PC_OK;
    }
    if (stack->nfilters == 0) {
        *out = true;
        return PC_OK;
    }
    for (size_t i = 0; i < ex->smart.nold_fx; i++) {
        const pc_psd_fx_item *it = &ex->smart.old_fx[i];
        const char *id = it->id != NULL ? it->id : "";
        if (pc_str_eq_c(&t->placed, id)) {
            pc_layer_mask m;
            bool has = false;
            pc_error me;
            st = pc_io_mask_from_item(it, ex->mask_fmt.sample, stack, &has, &m, &me);
            if (st == PC_ERR_NOMEM) {
                return pc_fail(err, PC_ERR_NOMEM, "%s", me.msg);
            }
            if (st != PC_OK) {
                return PC_OK;
            }
            *out = has == sm->has_filter_mask && (!has || pc_layer_mask_eq(&m, &sm->filter_mask));
            if (has) {
                pc_layer_mask_fini(&m);
            }
            return PC_OK;
        }
    }
    *out = !sm->has_filter_mask;
    return PC_OK;
}

/* Appends each local warning to ex's, prefixed with the layer unless it names one already. */
static pc_status extend_layer_warnings(pc_io_ex *ex, const char *name, pc_io_warnings *w, pc_error *err)
{
    pc_status st = PC_OK;
    for (size_t i = 0; i < w->n && st == PC_OK; i++) {
        if (strncmp(w->v[i], "layer ", 6) == 0) {
            st = pc_io_warn(&ex->warnings, err, "%s", w->v[i]);
        } else {
            st = pc_io_warn(&ex->warnings, err, "layer \"%s\": %s", name, w->v[i]);
        }
    }
    return st;
}

/* Vec::dedup: drops consecutive duplicates. */
static void dedup(pc_io_warnings *w)
{
    size_t k = 0;
    for (size_t i = 0; i < w->n; i++) {
        if (k > 0 && strcmp(w->v[k - 1], w->v[i]) == 0) {
            free(w->v[i]);
        } else {
            w->v[k++] = w->v[i];
        }
    }
    w->n = k;
}

/* The unfiltered placed pixels and the FEid item of a smart object with filters. */
static pc_status filter_cache(pc_io_ex *ex, const pc_layer *l, const pc_smart_object *sm, const char *placed,
                              const pc_io_ex_source *src, pc_error *err)
{
    pc_rect bounds =
        sm->has_cache ? pc_rect_union(pc_surface_content_bounds(&sm->cache), ex->canvas) : ex->canvas;
    const pc_io_ex_composite *c = NULL;
    bool has = false;
    pc_psd_fx_item item;
    pc_status st = PC_OK;
    if (!sm->has_stack_mode) {
        st = source_composite(ex, pc_str_c(&src->uuid), src->uuid.len, &c, err);
    }
    if (st == PC_OK && c != NULL && c->has) {
        pc_surface unfiltered;
        const pc_warp *warp = sm->has_warp ? &sm->warp : NULL;
        if (sm->has_perspective) {
            pc_algo_homography h;
            memcpy(h.m, sm->perspective, sizeof h.m);
            st = pc_algo_place_source_projective(&c->surface, c->bounds, &h, warp, &unfiltered, err);
        } else {
            st = pc_algo_place_source(&c->surface, c->bounds, &sm->transform, warp, &unfiltered, err);
        }
        if (st == PC_OK) {
            st = pc_io_feid_item(placed, &unfiltered, sm->has_filter_mask ? &sm->filter_mask : NULL, bounds,
                                 ex->fmt, &has, &item, err);
            pc_surface_fini(&unfiltered);
        }
    }
    if (st != PC_OK) {
        return st;
    }
    if (has) {
        return push_fx(&ex->smart.new_fx, &ex->smart.nnew_fx, &ex->smart.capnew_fx, &item, err);
    }
    if (sm->has_filter_mask) {
        /* Without the unfiltered pixels Photoshop re-renders from the source itself; only the
         * filter mask has nowhere to go. */
        return pc_io_warn(&ex->warnings, err, "layer \"%s\": the smart filter mask was not written to PSD",
                          pc_str_c(&l->name));
    }
    return PC_OK;
}

pc_status pc_io_ex_smart_blocks(pc_io_ex *ex, const pc_layer *l, const pc_smart_object *sm,
                                const pc_io_placed *tmpl, pc_io_raw_blocks *raw, pc_error *err)
{
    pc_io_ex_source src;
    char *reason = NULL;
    pc_io_filter_stack stack;
    const pc_io_placed *same;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    char placed_buf[37];
    const char *placed;
    char *placed_own = NULL;
    double size[2];
    pc_psd_desc fx;
    bool has_fx = false;
    pc_io_placed_spec spec;
    pc_buf sold = PC_BUF_INIT;
    pc_buf plld = PC_BUF_INIT;
    char key[4];
    pc_status st = smart_source(ex, sm, tmpl, &src, &reason, err);
    if (st != PC_OK) {
        return st;
    }
    if (reason != NULL) {
        (void)pc_io_raw_blocks_remove_key(raw, (const uint8_t *)"SoLd");
        (void)pc_io_raw_blocks_remove_key(raw, (const uint8_t *)"PlLd");
        (void)pc_io_raw_blocks_remove_key(raw, (const uint8_t *)"SoLE");
        st = pc_io_warn(&ex->warnings, err, "layer \"%s\": smart object written as pixels (%s)",
                        pc_str_c(&l->name), reason);
        free(reason);
        return st;
    }
    st = strs_insert(&ex->smart.used, pc_str_c(&src.uuid), src.uuid.len, err);
    stack.enabled = sm->filters_enabled;
    stack.filters = sm->smart_filters; /* borrowed: never finalised here */
    stack.nfilters = sm->nsmart_filters;
    stack.mask_enabled = !sm->has_filter_mask || sm->filter_mask.enabled;
    stack.mask_linked = sm->has_filter_mask && sm->filter_mask.linked;
    same = tmpl != NULL && pc_str_eq(&tmpl->idnt, &src.uuid) ? tmpl : NULL;
    if (st == PC_OK && same != NULL && sm->psd_raw != NULL) {
        bool unchanged = false;
        st = placed_unchanged(ex, same, pc_blob_data(sm->psd_raw), pc_blob_len(sm->psd_raw), sm, &stack,
                              &unchanged, err);
        if (st == PC_OK && unchanged) {
            size_t len = pc_blob_len(sm->psd_raw);
            uint8_t *d = pc_malloc(len);
            size_t at = raw->n;
            if (d == NULL) {
                st = pc_io_nomem(err, "copying placed-layer data");
            } else {
                if (len > 0) {
                    memcpy(d, pc_blob_data(sm->psd_raw), len);
                }
                if (raw_has(raw, "SoLd")) {
                    for (size_t i = 0; i < raw->n && at == raw->n; i++) {
                        at = memcmp(raw->v[i].key, "SoLd", 4) == 0 ? i : at;
                    }
                } else {
                    static const char *const KEYS[2] = {"SoLE", "SoLd"};
                    for (size_t k = 0; k < 2 && at == raw->n; k++) {
                        for (size_t i = 0; i < raw->n && at == raw->n; i++) {
                            at = memcmp(raw->v[i].key, KEYS[k], 4) == 0 ? i : at;
                        }
                    }
                }
                if (at < raw->n) {
                    free(raw->v[at].data);
                    raw->v[at].data = d;
                    raw->v[at].len = len;
                } else {
                    st = pc_io_raw_blocks_push_take(raw, (const uint8_t *)"SoLd", &d, len, err);
                }
            }
            if (st == PC_OK && stack.nfilters > 0) {
                st = strs_insert(&ex->smart.keep_fx, pc_str_c(&same->placed), same->placed.len, err);
            }
            pc_str_fini(&src.uuid);
            return st;
        }
    }
    if (st != PC_OK) {
        pc_str_fini(&src.uuid);
        return st;
    }
    if (same != NULL && same->placed.len > 0) {
        placed_own = pc_strndup(pc_str_c(&same->placed), same->placed.len);
        placed = placed_own;
        if (placed == NULL) {
            pc_str_fini(&src.uuid);
            return pc_io_nomem(err, "writing placed-layer data");
        }
    } else {
        pc_buf seed = PC_BUF_INIT;
        st = pc_buf_append(&seed, pc_str_c(&src.uuid), src.uuid.len, err);
        if (st == PC_OK) {
            st = pc_buf_appendf(&seed, err, ":%llu", (unsigned long long)l->id);
        }
        if (st == PC_OK) {
            pc_io_uuid_from(seed.data, seed.len, placed_buf);
        }
        pc_buf_fini(&seed);
        if (st != PC_OK) {
            pc_str_fini(&src.uuid);
            return st;
        }
        placed = placed_buf;
    }
    if (same == NULL || !pc_io_stored_size(&same->descriptor, size)) {
        size[0] = src.size[0];
        size[1] = src.size[1];
    }
    if (!(size[0] > 0.0 && size[1] > 0.0)) {
        size_from_layer(sm, size);
    }
    if (stack.nfilters > 0) {
        st = pc_io_filter_fx(&stack, pc_str_c(&l->name), &w, &fx, err);
        has_fx = st == PC_OK;
    }
    memset(&spec, 0, sizeof spec);
    spec.idnt = pc_str_c(&src.uuid);
    spec.placed = placed;
    spec.transform = sm->transform;
    spec.has_perspective = sm->has_perspective;
    memcpy(spec.perspective, sm->perspective, sizeof spec.perspective);
    spec.size[0] = size[0];
    spec.size[1] = size[1];
    spec.dpi = src.dpi;
    spec.warp = sm->has_warp ? &sm->warp : NULL;
    spec.filter_fx = has_fx ? &fx : NULL;
    if (st == PC_OK) {
        st = pc_io_sold_bytes(same != NULL ? &same->descriptor : NULL, &spec, &w, &sold, err);
    }
    if (st == PC_OK) {
        st = pc_io_plld_bytes(&spec, &w, &plld, err);
    }
    if (has_fx) {
        pc_psd_desc_fini(&fx);
    }
    dedup(&w);
    memcpy(key, same != NULL && raw_has(raw, "SoLE") && !raw_has(raw, "SoLd") ? "SoLE" : "SoLd", 4);
    if (st == PC_OK) {
        size_t k = 0;
        for (size_t i = 0; i < raw->n; i++) {
            bool placed_key = memcmp(raw->v[i].key, "SoLd", 4) == 0 || memcmp(raw->v[i].key, "SoLE", 4) == 0;
            if (memcmp(raw->v[i].key, key, 4) == 0 || !placed_key) {
                raw->v[k++] = raw->v[i];
            } else {
                free(raw->v[i].data);
            }
        }
        raw->n = k;
        {
            size_t plen = 0;
            uint8_t *pd = pc_buf_take(&plld, &plen);
            st = pc_io_ex_upsert(raw, "PlLd", &pd, plen, key, err);
            free(pd);
        }
    }
    if (st == PC_OK) {
        size_t slen = 0;
        uint8_t *sd = pc_buf_take(&sold, &slen);
        st = pc_io_ex_upsert(raw, key, &sd, slen, NULL, err);
        free(sd);
    }
    pc_buf_fini(&sold);
    pc_buf_fini(&plld);
    if (st == PC_OK && stack.nfilters > 0) {
        st = filter_cache(ex, l, sm, placed, &src, err);
    }
    if (st == PC_OK) {
        st = extend_layer_warnings(ex, pc_str_c(&l->name), &w, err);
    }
    pc_io_warnings_fini(&w);
    free(placed_own);
    pc_str_fini(&src.uuid);
    return st;
}
