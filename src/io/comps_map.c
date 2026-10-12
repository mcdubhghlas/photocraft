/* Layer comps and artboards <-> PSD (port of crates/io/src/comps_map.rs).
 *
 *  - Layer comps: image resource 1065 holds a CompList descriptor (list of Comp objects with
 *    "Nm  ", compID, capturedInfo bit flags and an optional comment, plus lastAppliedComp). Each
 *    layer's state per comp lives in its shmd block under the cmls key: layerSettings, one object
 *    per comp id list with enab (visibility) and Ofst (offset from the layer's saved position).
 *    Comp id 0 is the Last Document State.
 *  - Artboards: the per-layer artb block (older files: artd / abdd) is an artboard descriptor
 *    with artboardRect, guideIndeces, artboardPresetName, "Clr " and artboardBackgroundType
 *    (1 white, 2 black, 3 transparent, 4 other).
 * Both are re-emitted verbatim while they still decode to the document's state and regenerated
 * otherwise (comp appearance is not written to PSD; .pcraft keeps it). */
#include "io/comps_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "base/num.h"
#include "io/blocks.h"
#include "io/maps_desc.h"
#include "psd/misc.h"

const char PC_IO_ARTBOARD_KEYS[3][5] = {"artb", "artd", "abdd"};

static bool is_artboard_key(const uint8_t k[4])
{
    for (size_t i = 0; i < 3; i++) {
        if (memcmp(k, PC_IO_ARTBOARD_KEYS[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

/* comps_map's own `int`: Integer, LargeInteger, or a rounded Double / UnitFloat (saturating). */
static bool int_of(const pc_psd_desc *d, const char *key, int64_t *out)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    if (v == NULL) {
        return false;
    }
    switch (v->kind) {
    case PC_PSD_V_INTEGER:
        *out = v->i;
        return true;
    case PC_PSD_V_LARGE_INTEGER:
        *out = v->l;
        return true;
    case PC_PSD_V_DOUBLE:
    case PC_PSD_V_UNIT_FLOAT:
        *out = pc_f64_to_i64(round(v->d));
        return true;
    default:
        return false;
    }
}

/* Text value (lossy, trailing NULs dropped) into *out; false (out untouched) otherwise. */
static pc_status text_of(const pc_psd_desc *d, const char *key, pc_str *out, pc_error *err)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    if (v == NULL || v->kind != PC_PSD_V_TEXT) {
        return PC_OK;
    }
    return pc_io4_ustr_lossy(&v->text, out, err);
}

/* A descriptor named with a lone NUL unit (the reference's `obj`). */
static void obj(pc_psd_desc *d, const char *cls, pc_status *st, pc_error *err)
{
    pc_io4_desc(d, cls, st, err);
    if (*st == PC_OK) {
        d->name.u = malloc(sizeof *d->name.u);
        if (d->name.u == NULL) {
            *st = pc_io_nomem(err, "writing layer comps");
            return;
        }
        d->name.u[0] = 0;
        d->name.n = 1;
    }
}

/* VersionedDescriptor bytes, padded to an even length when pad. */
static pc_status vdesc_out(pc_psd_desc *d, bool pad, pc_status st, pc_buf *out, pc_error *err)
{
    pc_psd_bytes bytes;
    pc_buf b = PC_BUF_INIT;
    if (st == PC_OK) {
        st = pc_psd_vdesc_to_bytes(d, &bytes, err);
        if (st == PC_OK) {
            st = pc_buf_append(&b, bytes.data, bytes.len, err);
            pc_psd_bytes_fini(&bytes);
        }
        if (st == PC_OK && pad && b.len % 2 == 1) {
            st = pc_buf_append_byte(&b, 0, err);
        }
    }
    pc_psd_desc_fini(d);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

/* ---------------------------------------------------------------- artboards */

pc_status pc_io_parse_artboard(const uint8_t *data, size_t len, bool *found, pc_artboard *out, pc_error *err)
{
    pc_psd_desc d;
    bool ok = false;
    const pc_psd_desc *r;
    double v[4];
    static const char *const keys[4] = {"Left", "Top ", "Rght", "Btom"};
    pc_artboard a;
    int64_t ty = 1;
    pc_status st = pc_io4_vdesc_prefix(data, len, &ok, &d, err);
    if (st != PC_OK || !ok) {
        if (st == PC_OK) {
            *found = false;
        }
        return st;
    }
    r = pc_io4_obj(&d, "artboardRect");
    for (size_t i = 0; i < 4; i++) {
        if (r == NULL || !pc_io4_num_of(r, keys[i], &v[i])) {
            pc_psd_desc_fini(&d);
            *found = false;
            return PC_OK;
        }
    }
    pc_artboard_init(&a, pc_rect_make(pc_f64_to_i32(round(v[0])), pc_f64_to_i32(round(v[1])),
                                      pc_f64_to_i32(round(v[2])), pc_f64_to_i32(round(v[3]))));
    (void)int_of(&d, "artboardBackgroundType", &ty);
    if (ty == 2) {
        a.background.kind = PC_ARTBOARD_BLACK;
    } else if (ty == 3) {
        a.background.kind = PC_ARTBOARD_TRANSPARENT;
    } else if (ty == 4) {
        const pc_psd_desc *c = pc_io4_obj(&d, "Clr ");
        a.background.kind = PC_ARTBOARD_CUSTOM;
        if (c == NULL || !pc_io_color_from_desc(c, &a.background.custom)) {
            a.background.custom = PC_COLOR_WHITE;
        }
    }
    st = text_of(&d, "artboardPresetName", &a.preset, err);
    pc_psd_desc_fini(&d);
    if (st != PC_OK) {
        pc_artboard_fini(&a);
        return st;
    }
    *out = a;
    *found = true;
    return PC_OK;
}

pc_status pc_io_write_artboard(const pc_artboard *a, const int32_t *guides, size_t n, pc_buf *out,
                               pc_error *err)
{
    pc_status st = PC_OK;
    pc_psd_desc rect;
    pc_psd_desc d;
    pc_psd_desc cd;
    pc_psd_value list = pc_io4_list();
    pc_color color = a->background.kind == PC_ARTBOARD_CUSTOM  ? a->background.custom
                     : a->background.kind == PC_ARTBOARD_BLACK ? PC_COLOR_BLACK
                                                               : PC_COLOR_WHITE;
    pc_io4_desc(&rect, "classFloatRect", &st, err);
    pc_io4_put(&rect, "Top ", pc_psd_value_double((double)a->rect.y0), &st, err);
    pc_io4_put(&rect, "Left", pc_psd_value_double((double)a->rect.x0), &st, err);
    pc_io4_put(&rect, "Btom", pc_psd_value_double((double)a->rect.y1), &st, err);
    pc_io4_put(&rect, "Rght", pc_psd_value_double((double)a->rect.x1), &st, err);
    for (size_t i = 0; i < n; i++) {
        pc_io4_list_push(&list, pc_psd_value_int(guides[i]), &st, err);
    }
    pc_io4_desc(&d, "artboard", &st, err);
    pc_io4_put_obj(&d, "artboardRect", &rect, &st, err);
    pc_io4_put(&d, "guideIndeces", list, &st, err);
    pc_io4_put_text(&d, "artboardPresetName", a->preset.ptr, a->preset.len, true, &st, err);
    memset(&cd, 0, sizeof cd);
    if (st == PC_OK) {
        st = pc_io_color_to_desc(&color, &cd, err);
    }
    pc_io4_put_obj(&d, "Clr ", &cd, &st, err);
    pc_io4_put(&d, "artboardBackgroundType",
               pc_psd_value_int(pc_artboard_background_psd_type(&a->background)), &st, err);
    return vdesc_out(&d, true, st, out, err);
}

pc_status pc_io_guides_in(const pc_guides *g, pc_rect r, int32_t **out, size_t *n, pc_error *err)
{
    size_t total = g->nvertical + g->nhorizontal;
    int32_t *v = pc_malloc_array(total > 0 ? total : 1, sizeof *v);
    size_t k = 0;
    if (v == NULL) {
        return pc_io_nomem(err, "listing artboard guides");
    }
    for (size_t i = 0; i < total; i++) {
        bool vert = i < g->nvertical;
        double p = vert ? (double)g->vertical[i] : (double)g->horizontal[i - g->nvertical];
        double lo = (double)(vert ? r.x0 : r.y0);
        double hi = (double)(vert ? r.x1 : r.y1);
        if (p > lo && p < hi) {
            v[k++] = (int32_t)i;
        }
    }
    *out = v;
    *n = k;
    return PC_OK;
}

static void remove_at(pc_io_raw_blocks *raw, size_t i)
{
    free(raw->v[i].data);
    memmove(&raw->v[i], &raw->v[i + 1], (raw->n - i - 1) * sizeof *raw->v);
    raw->n--;
}

pc_status pc_io_artboard_block(const pc_guides *guides, const pc_layer *l, pc_io_raw_blocks *raw,
                               pc_error *err)
{
    const pc_artboard *a = pc_layer_artboard(l);
    bool pos = false;
    size_t at = 0;
    int32_t *gi = NULL;
    size_t ng = 0;
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    for (size_t i = 0; i < raw->n && !pos; i++) {
        if (is_artboard_key(raw->v[i].key)) {
            pos = true;
            at = i;
        }
    }
    if (a == NULL) {
        for (size_t i = raw->n; i-- > 0;) {
            if (is_artboard_key(raw->v[i].key)) {
                remove_at(raw, i);
            }
        }
        return PC_OK;
    }
    if (pos) {
        pc_artboard cur;
        bool ok = false;
        st = pc_io_parse_artboard(raw->v[at].data, raw->v[at].len, &ok, &cur, err);
        if (st != PC_OK) {
            return st;
        }
        if (ok) {
            bool same = pc_artboard_eq(&cur, a);
            pc_artboard_fini(&cur);
            if (same) {
                return PC_OK;
            }
        }
    }
    st = pc_io_guides_in(guides, a->rect, &gi, &ng, err);
    if (st == PC_OK) {
        st = pc_io_write_artboard(a, gi, ng, &b, err);
    }
    free(gi);
    if (st != PC_OK) {
        return st;
    }
    if (pos) {
        size_t n = 0;
        free(raw->v[at].data);
        raw->v[at].data = pc_buf_take(&b, &n);
        raw->v[at].len = n;
        memcpy(raw->v[at].key, "artb", 4);
        return PC_OK;
    }
    {
        size_t n = 0;
        uint8_t *p = pc_buf_take(&b, &n);
        return pc_io_raw_blocks_push_take(raw, (const uint8_t *)"artb", &p, n, err);
    }
}

/* ---------------------------------------------------------------- layer comps */

/* One layerSettings entry of a cmls descriptor. The offset is kept at the file's width: whether
 * origin + offset fits an i32 is decided per layer (#1017). */
typedef struct setting {
    uint32_t *comps;
    size_t ncomps;
    bool has_visible;
    bool visible;
    bool has_offset;
    int64_t offset[2];
} setting;

static void settings_free(setting *s, size_t n)
{
    for (size_t i = 0; s != NULL && i < n; i++) {
        free(s[i].comps);
    }
    free(s);
}

/* parse_cmls; *found false when it doesn't parse. */
static pc_status parse_cmls(const uint8_t *data, size_t len, bool *found, setting **out, size_t *n,
                            pc_error *err)
{
    pc_psd_desc d;
    bool ok = false;
    const pc_psd_value *items;
    setting *v = NULL;
    size_t k = 0;
    pc_status st = pc_io4_vdesc_prefix(data, len, &ok, &d, err);
    if (st != PC_OK || !ok) {
        if (st == PC_OK) {
            *found = false;
        }
        return st;
    }
    items = pc_psd_desc_get(&d, "layerSettings");
    if (items == NULL || items->kind != PC_PSD_V_LIST) {
        pc_psd_desc_fini(&d);
        *found = false;
        return PC_OK;
    }
    if (items->nlist > 0) {
        v = pc_calloc_array(items->nlist, sizeof *v);
        if (v == NULL) {
            pc_psd_desc_fini(&d);
            return pc_io_nomem(err, "reading layer comps");
        }
    }
    for (size_t i = 0; i < items->nlist; i++) {
        const pc_psd_desc *s;
        const pc_psd_value *ids;
        const pc_psd_value *en;
        const pc_psd_desc *o;
        setting *t = &v[k];
        if (items->list[i].kind != PC_PSD_V_DESCRIPTOR) {
            continue;
        }
        s = items->list[i].desc;
        ids = pc_psd_desc_get(s, "compList");
        if (ids != NULL && ids->kind == PC_PSD_V_LIST && ids->nlist > 0) {
            t->comps = pc_malloc_array(ids->nlist, sizeof *t->comps);
            if (t->comps == NULL) {
                settings_free(v, k);
                pc_psd_desc_fini(&d);
                return pc_io_nomem(err, "reading layer comps");
            }
            for (size_t j = 0; j < ids->nlist; j++) {
                if (ids->list[j].kind == PC_PSD_V_INTEGER) {
                    t->comps[t->ncomps++] = (uint32_t)ids->list[j].i;
                }
            }
        }
        en = pc_psd_desc_get(s, "enab");
        if (en != NULL && en->kind == PC_PSD_V_BOOLEAN) {
            t->has_visible = true;
            t->visible = en->b;
        }
        o = pc_io4_obj(s, "Ofst");
        if (o != NULL && int_of(o, "Hrzn", &t->offset[0]) && int_of(o, "Vrtc", &t->offset[1])) {
            t->has_offset = true;
        }
        k++;
    }
    pc_psd_desc_fini(&d);
    *out = v;
    *n = k;
    *found = true;
    return PC_OK;
}

/* The cmls item of a layer's preserved shmd block: *data malloc'd (NULL when absent). */
static pc_status layer_cmls(const pc_layer *l, uint8_t **data, size_t *len, pc_error *err)
{
    *data = NULL;
    *len = 0;
    for (size_t i = 0; i < l->npsd_blocks; i++) {
        pc_psd_meta_item *items = NULL;
        size_t n = 0;
        pc_status st;
        if (memcmp(l->psd_blocks[i].key, "shmd", 4) != 0) {
            continue;
        }
        st = pc_psd_parse_shmd(pc_blob_data(l->psd_blocks[i].data), pc_blob_len(l->psd_blocks[i].data),
                               &items, &n, err);
        if (st == PC_ERR_NOMEM) {
            return st;
        }
        if (st != PC_OK) {
            return PC_OK;
        }
        for (size_t j = 0; j < n; j++) {
            if (memcmp(items[j].key, "cmls", 4) == 0) {
                *data = items[j].data.data;
                *len = items[j].data.len;
                items[j].data.data = NULL;
                if (*data == NULL) {
                    *data = malloc(1);
                    if (*data == NULL) {
                        pc_psd_meta_items_free(items, n);
                        return pc_io_nomem(err, "reading layer comps");
                    }
                }
                break;
            }
        }
        pc_psd_meta_items_free(items, n);
        return PC_OK;
    }
    return PC_OK;
}

/* Absolute comp position of a layer at here moved by a file offset; false when it doesn't fit
 * an i32 (the offsets come from the file, #1017). */
static bool offset_position(const int32_t here[2], const int64_t off[2], int32_t out[2])
{
    for (size_t i = 0; i < 2; i++) {
        int64_t v;
        if ((off[i] > 0 && (int64_t)here[i] > INT64_MAX - off[i]) ||
            (off[i] < 0 && (int64_t)here[i] < INT64_MIN - off[i])) {
            return false;
        }
        v = (int64_t)here[i] + off[i];
        if (v < INT32_MIN || v > INT32_MAX) {
            return false;
        }
        out[i] = (int32_t)v;
    }
    return true;
}

void pc_io_comps_fini(pc_io_comps *c)
{
    if (c == NULL) {
        return;
    }
    for (size_t i = 0; i < c->n; i++) {
        pc_layer_comp_fini(&c->comps[i]);
    }
    free(c->comps);
    if (c->has_last_state) {
        pc_layer_comp_fini(&c->last_state);
    }
    memset(c, 0, sizeof *c);
}

static pc_status push_state(pc_layer_comp *c, const pc_comp_layer_state *s, pc_error *err)
{
    pc_comp_layer_state *v = realloc(c->states, (c->nstates + 1) * sizeof *v);
    if (v == NULL) {
        return pc_io_nomem(err, "reading layer comps");
    }
    c->states = v;
    v[c->nstates++] = *s; /* plain data: no appearance recorded */
    return PC_OK;
}

/* Reads the comp list of resource 1065 into r (comps and last applied comp). */
static pc_status read_list(const pc_psd_desc *d, pc_io_comps *r, pc_error *err)
{
    const pc_psd_value *items = pc_psd_desc_get(d, "list");
    int64_t la;
    if (items != NULL && items->kind == PC_PSD_V_LIST && items->nlist > 0) {
        r->comps = pc_calloc_array(items->nlist, sizeof *r->comps);
        if (r->comps == NULL) {
            return pc_io_nomem(err, "reading layer comps");
        }
        for (size_t i = 0; i < items->nlist; i++) {
            const pc_psd_desc *c;
            pc_layer_comp *comp = &r->comps[r->n];
            int64_t id;
            int64_t ci = 0;
            pc_status st;
            if (items->list[i].kind != PC_PSD_V_DESCRIPTOR) {
                continue;
            }
            c = items->list[i].desc;
            if (!int_of(c, "compID", &id)) {
                continue;
            }
            memset(comp, 0, sizeof *comp);
            comp->id = (uint32_t)id;
            r->n++;
            st = text_of(c, "Nm  ", &comp->name, err);
            if (st == PC_OK) {
                st = text_of(c, "comment", &comp->comment, err);
            }
            if (st != PC_OK) {
                return st;
            }
            (void)int_of(c, "capturedInfo", &ci);
            pc_layer_comp_set_captured_info(comp, pc_u32_as_i32((uint32_t)(uint64_t)ci));
        }
    }
    if (int_of(d, "lastAppliedComp", &la)) {
        uint32_t id = (uint32_t)la;
        for (size_t i = 0; i < r->n; i++) {
            if (r->comps[i].id == id) {
                r->has_last_applied = true;
                r->last_applied = id;
                break;
            }
        }
    }
    return PC_OK;
}

/* Adds one layer's cmls settings to the comps. */
static pc_status apply_layer(const pc_layer *l, pc_io_comps *r, pc_io_warnings *warnings, pc_error *err)
{
    uint8_t *data = NULL;
    size_t len = 0;
    setting *ss = NULL;
    size_t ns = 0;
    bool ok = false;
    int32_t here[2];
    bool has_here;
    pc_status st = layer_cmls(l, &data, &len, err);
    if (st != PC_OK || data == NULL) {
        return st;
    }
    st = parse_cmls(data, len, &ok, &ss, &ns, err);
    free(data);
    if (st != PC_OK || !ok) {
        return st;
    }
    has_here = pc_comp_layer_position(l, here);
    for (size_t i = 0; i < ns && st == PC_OK; i++) {
        const setting *s = &ss[i];
        pc_comp_layer_state state;
        if (!s->has_visible && !s->has_offset) {
            continue;
        }
        memset(&state, 0, sizeof state);
        state.layer = l->id;
        state.has_visible = s->has_visible;
        state.visible = s->visible;
        if (has_here && s->has_offset) {
            state.has_position = offset_position(here, s->offset, state.position);
            if (!state.has_position && warnings != NULL) {
                st = pc_io_warn(warnings, err,
                                "layer \"%s\": layer comp position offset (%lld, %lld) is out of range; that "
                                "position was not imported",
                                pc_str_c(&l->name), (long long)s->offset[0], (long long)s->offset[1]);
            }
        }
        for (size_t j = 0; j < s->ncomps && st == PC_OK; j++) {
            pc_layer_comp *target = NULL;
            if (s->comps[j] == 0) {
                target = &r->last_state;
            } else {
                for (size_t c = 0; c < r->n && target == NULL; c++) {
                    if (r->comps[c].id == s->comps[j]) {
                        target = &r->comps[c];
                    }
                }
            }
            if (target != NULL) {
                st = push_state(target, &state, err);
            }
        }
    }
    settings_free(ss, ns);
    return st;
}

pc_status pc_io_decode_comps(const uint8_t *res, size_t len, const pc_document *doc, pc_io_comps *out,
                             pc_io_warnings *warnings, pc_error *err)
{
    pc_io_comps r;
    pc_psd_desc d;
    bool ok = false;
    pc_walk w;
    pc_status st;
    memset(&r, 0, sizeof r);
    if (res == NULL) {
        *out = r;
        return PC_OK;
    }
    st = pc_io4_vdesc_prefix(res, len, &ok, &d, err);
    if (st != PC_OK) {
        return st;
    }
    if (!ok) {
        *out = r;
        return PC_OK;
    }
    st = read_list(&d, &r, err);
    pc_psd_desc_fini(&d);
    if (st == PC_OK) {
        r.last_state.id = 0;
        r.last_state.apply_visibility = true;
        r.last_state.apply_position = true;
        st = pc_str_set_c(&r.last_state.name, "Last Document State", err);
        r.has_last_state = true; /* owned while building; cleared below when empty */
    }
    if (st == PC_OK) {
        st = pc_document_walk(doc, &w, err);
        if (st == PC_OK) {
            for (size_t i = 0; i < w.len && st == PC_OK; i++) {
                st = apply_layer(w.items[i].layer, &r, warnings, err);
            }
            pc_walk_fini(&w);
        }
    }
    if (st != PC_OK) {
        pc_io_comps_fini(&r);
        return st;
    }
    if (r.last_state.nstates == 0) {
        pc_layer_comp_fini(&r.last_state);
        memset(&r.last_state, 0, sizeof r.last_state);
        r.has_last_state = false;
    }
    *out = r;
    return PC_OK;
}

pc_status pc_io_write_comps_resource(const pc_document *doc, bool *has, pc_buf *out, pc_error *err)
{
    pc_status st = PC_OK;
    pc_psd_desc d;
    pc_psd_value list = pc_io4_list();
    if (doc->nlayer_comps == 0) {
        *has = false;
        return PC_OK;
    }
    for (size_t i = 0; i < doc->nlayer_comps; i++) {
        const pc_layer_comp *c = &doc->layer_comps[i];
        pc_psd_desc cd;
        pc_psd_value v;
        pc_io4_desc(&cd, "Comp", &st, err);
        pc_io4_put_text(&cd, "Nm  ", c->name.ptr, c->name.len, true, &st, err);
        if (c->comment.len > 0) {
            pc_io4_put_text(&cd, "comment", c->comment.ptr, c->comment.len, true, &st, err);
        }
        pc_io4_put(&cd, "compID", pc_psd_value_int(pc_u32_as_i32(c->id)), &st, err);
        pc_io4_put(&cd, "capturedInfo", pc_psd_value_int(pc_layer_comp_captured_info(c)), &st, err);
        if (st == PC_OK) {
            st = pc_psd_value_object(&cd, &v, err);
            pc_io4_list_push(&list, v, &st, err);
        } else {
            pc_psd_desc_fini(&cd);
        }
    }
    pc_io4_desc(&d, "CompList", &st, err);
    pc_io4_put(&d, "list", list, &st, err);
    if (doc->has_last_applied_comp) {
        pc_io4_put(&d, "lastAppliedComp", pc_psd_value_int(pc_u32_as_i32(doc->last_applied_comp)), &st, err);
    }
    st = vdesc_out(&d, false, st, out, err);
    if (st == PC_OK) {
        *has = true;
    }
    return st;
}

static pc_psd_value setting_value(uint32_t id, const pc_comp_layer_state *s, bool has_here,
                                  const int32_t here[2], pc_status *st, pc_error *err)
{
    pc_psd_desc o;
    pc_psd_value ids = pc_io4_list();
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    obj(&o, "null", st, err);
    if (s != NULL) {
        if (s->has_visible) {
            pc_io4_put(&o, "enab", pc_psd_value_bool(s->visible), st, err);
        }
        /* A position whose offset from the layer does not fit an i32 (e.g. from a .pcraft file)
         * is left out rather than wrapped: the comp then records no position (#1017). */
        if (s->has_position && has_here) {
            int64_t dx = (int64_t)s->position[0] - here[0];
            int64_t dy = (int64_t)s->position[1] - here[1];
            if (dx >= INT32_MIN && dx <= INT32_MAX && dy >= INT32_MIN && dy <= INT32_MAX) {
                pc_psd_desc off;
                obj(&off, "null", st, err);
                pc_io4_put(&off, "Hrzn", pc_psd_value_int((int32_t)dx), st, err);
                pc_io4_put(&off, "Vrtc", pc_psd_value_int((int32_t)dy), st, err);
                pc_io4_put_obj(&o, "Ofst", &off, st, err);
            }
        }
    }
    pc_io4_list_push(&ids, pc_psd_value_int(pc_u32_as_i32(id)), st, err);
    pc_io4_put(&o, "compList", ids, st, err);
    if (*st == PC_OK) {
        *st = pc_psd_value_object(&o, &v, err);
    } else {
        pc_psd_desc_fini(&o);
    }
    return v;
}

pc_status pc_io_write_cmls(const pc_layer_comp *comps, size_t n, const pc_layer_comp *last, const pc_layer *l,
                           uint32_t psd_id, bool *has, pc_buf *out, pc_error *err)
{
    pc_status st = PC_OK;
    int32_t here[2] = {0, 0};
    bool has_here;
    pc_psd_value settings = pc_io4_list();
    pc_psd_desc origin;
    pc_psd_desc d;
    if (n == 0) {
        *has = false;
        return PC_OK;
    }
    has_here = pc_comp_layer_position(l, here);
    for (size_t i = 0; i < n; i++) {
        pc_psd_value v =
            setting_value(comps[i].id, pc_layer_comp_state(&comps[i], l->id), has_here, here, &st, err);
        pc_io4_list_push(&settings, v, &st, err);
    }
    {
        pc_psd_value v = setting_value(0, last != NULL ? pc_layer_comp_state(last, l->id) : NULL, has_here,
                                       here, &st, err);
        pc_io4_list_push(&settings, v, &st, err);
    }
    obj(&origin, "null", &st, err);
    pc_io4_put(&origin, "Hrzn", pc_psd_value_double(0.0), &st, err);
    pc_io4_put(&origin, "Vrtc", pc_psd_value_double(0.0), &st, err);
    pc_io4_desc(&d, "null", &st, err);
    pc_io4_put_obj(&d, "origFXRefPoint", &origin, &st, err);
    pc_io4_put(&d, "LyrI", pc_psd_value_int(pc_u32_as_i32(psd_id)), &st, err);
    pc_io4_put(&d, "layerSettings", settings, &st, err);
    st = vdesc_out(&d, false, st, out, err);
    if (st == PC_OK) {
        *has = true;
    }
    return st;
}

static bool comps_eq(const pc_layer_comp *a, size_t na, const pc_layer_comp *b, size_t nb)
{
    if (na != nb) {
        return false;
    }
    for (size_t i = 0; i < na; i++) {
        if (!pc_layer_comp_eq(&a[i], &b[i])) {
            return false;
        }
    }
    return true;
}

pc_status pc_io_comps_unchanged(const pc_document *doc, bool *out, pc_error *err)
{
    const uint8_t *raw = NULL;
    size_t len = 0;
    pc_io_comps c;
    pc_status st;
    for (size_t i = 0; i < doc->metadata.npsd_resources; i++) {
        if (doc->metadata.psd_resources[i].id == PC_IO_LAYER_COMPS) {
            raw = pc_blob_data(doc->metadata.psd_resources[i].data);
            len = pc_blob_len(doc->metadata.psd_resources[i].data);
            if (raw == NULL) {
                raw = (const uint8_t *)"";
            }
            break;
        }
    }
    st = pc_io_decode_comps(raw, len, doc, &c, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    *out = comps_eq(c.comps, c.n, doc->layer_comps, doc->nlayer_comps) &&
           c.has_last_applied == doc->has_last_applied_comp &&
           (!c.has_last_applied || c.last_applied == doc->last_applied_comp) &&
           c.has_last_state == doc->has_last_document_state &&
           (!c.has_last_state || pc_layer_comp_eq(&c.last_state, &doc->last_document_state));
    pc_io_comps_fini(&c);
    return PC_OK;
}

pc_status pc_io_set_cmls(pc_io_raw_blocks *raw, uint8_t **cmls, size_t len, pc_error *err)
{
    bool some = cmls != NULL && *cmls != NULL;
    uint8_t *data = some ? *cmls : NULL;
    pc_psd_meta_item *items = NULL;
    size_t n = 0;
    size_t pos = 0;
    bool has_pos = false;
    bool has_item = false;
    size_t at = 0;
    pc_psd_bytes shmd;
    pc_status st = PC_OK;
    if (cmls != NULL) {
        *cmls = NULL;
    }
    for (size_t i = 0; i < raw->n && !has_pos; i++) {
        if (memcmp(raw->v[i].key, "shmd", 4) == 0) {
            has_pos = true;
            pos = i;
        }
    }
    if (has_pos) {
        st = pc_psd_parse_shmd(raw->v[pos].data, raw->v[pos].len, &items, &n, err);
        if (st == PC_ERR_NOMEM) {
            free(data);
            return st;
        }
        if (st != PC_OK) {
            items = NULL;
            n = 0;
            st = PC_OK;
        }
    }
    for (size_t i = 0; i < n && !has_item; i++) {
        if (memcmp(items[i].key, "cmls", 4) == 0) {
            has_item = true;
            at = i;
        }
    }
    if (has_item && some) {
        uint8_t *p = malloc(len + 1);
        if (p == NULL) {
            st = pc_io_nomem(err, "writing layer comps");
        } else {
            if (len > 0) {
                memcpy(p, data, len);
            }
            p[len] = 0;
            pc_psd_bytes_fini(&items[at].data);
            items[at].data.data = p;
            items[at].data.len = len + (len % 2);
        }
    } else if (has_item) {
        pc_psd_bytes_fini(&items[at].data);
        memmove(&items[at], &items[at + 1], (n - at - 1) * sizeof *items);
        n--;
    } else if (some) {
        pc_psd_meta_item it;
        pc_psd_meta_item *v = realloc(items, (n + 1) * sizeof *v);
        if (v == NULL) {
            st = pc_io_nomem(err, "writing layer comps");
        } else {
            items = v;
            st = pc_psd_meta_item_make("cmls", data, len, &it, err);
            if (st == PC_OK) {
                memmove(&items[1], &items[0], n * sizeof *items);
                items[0] = it;
                n++;
            }
        }
    } else {
        pc_psd_meta_items_free(items, n);
        return PC_OK;
    }
    free(data);
    if (st != PC_OK) {
        pc_psd_meta_items_free(items, n);
        return st;
    }
    if (n == 0) {
        pc_psd_meta_items_free(items, n);
        if (has_pos) {
            remove_at(raw, pos);
        }
        return PC_OK;
    }
    st = pc_psd_write_shmd(items, n, &shmd, err);
    pc_psd_meta_items_free(items, n);
    if (st != PC_OK) {
        return st;
    }
    if (has_pos) {
        free(raw->v[pos].data);
        raw->v[pos].data = shmd.data;
        raw->v[pos].len = shmd.len;
        return PC_OK;
    }
    return pc_io_raw_blocks_push_take(raw, (const uint8_t *)"shmd", &shmd.data, shmd.len, err);
}
