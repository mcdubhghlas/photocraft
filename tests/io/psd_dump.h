/* The canonical document dump of rustref io-psd-vectors (doc_dump in io_psd_vectors.rs): every
 * field a PSD import sets, serde JSON where the type has it, surfaces as format / bounds /
 * default pixel / BLAKE3, layer ids replaced by their walk index. Test code only. */
#ifndef PC_TESTS_IO_PSD_DUMP_H
#define PC_TESTS_IO_PSD_DUMP_H

#include "common.h"
#include "doc/json.h"

/* Appends a value built in a fresh doc (NULL builder result = "<json error>"). */
static inline void dj_put(pc_buf *b, yyjson_mut_doc *d, yyjson_mut_val *v)
{
    pc_buf out = PC_BUF_INIT;
    if (v == NULL || pc_json_write(v, 0, &out, NULL) != PC_OK) {
        sb(b, "<json error>");
    } else {
        must(pc_buf_append(b, out.data, out.len, NULL));
    }
    pc_buf_fini(&out);
    yyjson_mut_doc_free(d);
}

/* A string as serde_json writes it (exact bytes: names from files may hold any character). */
static inline void sjs(pc_buf *b, const char *p, size_t len)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    dj_put(b, d, d != NULL ? yyjson_mut_strncpy(d, p != NULL ? p : "", len) : NULL);
}

static inline void dd_chan(pc_buf *b, const pc_alpha_channel *c)
{
    sjs(b, pc_str_c(&c->name), c->name.len);
    sb(b, "|");
    if (c->has_spot) {
        s_color(b, &c->spot_color);
        sb(b, "@%08x", fb(c->spot_solidity));
    } else {
        sb(b, "-");
    }
    sb(b, "|");
    s_color(b, &c->color);
    sb(b, "|%08x|%s|", fb(c->opacity), c->indicates == PC_INDICATES_SELECTED_AREAS ? "sel" : "mask");
    s_surf(b, &c->surface, false);
}

/* Warnings as a JSON array of strings. */
static inline void dd_warn(pc_buf *b, char *const *v, size_t n)
{
    sb(b, "W[");
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            sb(b, ",");
        }
        sjs(b, v[i], strlen(v[i]));
    }
    sb(b, "]");
}

#define DJ(b, T, ptr)                                                                                        \
    do {                                                                                                     \
        yyjson_mut_doc *dj_d_ = yyjson_mut_doc_new(NULL);                                                    \
        dj_put((b), dj_d_, T##_to_json(dj_d_, (ptr), 0));                                                    \
    } while (0)

/* A JSON array of n items of type T. */
#define DJ_ARR(b, T, arr, n)                                                                                 \
    do {                                                                                                     \
        yyjson_mut_doc *dj_d_ = yyjson_mut_doc_new(NULL);                                                    \
        yyjson_mut_val *dj_a_ = yyjson_mut_arr(dj_d_);                                                       \
        for (size_t dj_i_ = 0; dj_i_ < (n) && dj_a_ != NULL; dj_i_++) {                                      \
            yyjson_mut_val *dj_v_ = T##_to_json(dj_d_, &(arr)[dj_i_], 0);                                    \
            if (dj_v_ == NULL || !yyjson_mut_arr_append(dj_a_, dj_v_)) {                                     \
                dj_a_ = NULL;                                                                                \
            }                                                                                                \
        }                                                                                                    \
        dj_put((b), dj_d_, dj_a_);                                                                           \
    } while (0)

static inline void dd_hex(pc_buf *b, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sb(b, "%02x", p[i]);
    }
}

static inline uint64_t dd_db(double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    return u;
}

static inline void dd_blob(pc_buf *b, const pc_blob *blob)
{
    sopt(b, pc_blob_data(blob), pc_blob_len(blob), blob != NULL);
}

static inline void dd_mask(pc_buf *b, bool has, const pc_layer_mask *m)
{
    if (!has) {
        sb(b, "-");
        return;
    }
    sb(b, "%d%d:%08x:%08x:", m->enabled, m->linked, fb(m->density), fb(m->feather));
    s_surf(b, &m->surface, false);
}

static inline void dd_effects(pc_buf *b, const pc_effects *e)
{
    sb(b, "%d:", e->enabled);
    DJ_ARR(b, pc_effect, e->items, e->nitems);
    sb(b, ":");
    dd_blob(b, e->psd_raw);
    if (e->has_reference) {
        sb(b, ":%016llx,%016llx", (unsigned long long)dd_db(e->reference[0]),
           (unsigned long long)dd_db(e->reference[1]));
    } else {
        sb(b, ":-");
    }
}

static inline void dd_osurf(pc_buf *b, bool has, const pc_surface *s)
{
    if (has) {
        s_surf(b, s, false);
    } else {
        sb(b, "-");
    }
}

static inline void dd_warp(pc_buf *b, bool has, const pc_warp *w)
{
    if (!has) {
        sb(b, "-");
        return;
    }
    sb(b, "%s,%016llx,%016llx,%016llx,%d", pc_warp_style_id(w->style), (unsigned long long)dd_db(w->bend),
       (unsigned long long)dd_db(w->h_distort), (unsigned long long)dd_db(w->v_distort), w->vertical);
    for (size_t i = 0; i < 4; i++) {
        sb(b, ",%016llx", (unsigned long long)dd_db(w->bounds[i]));
    }
    if (w->has_mesh) {
        sb(b, ":m");
        for (size_t i = 0; i < w->mesh.nus; i++) {
            sb(b, ",%016llx", (unsigned long long)dd_db(w->mesh.us[i]));
        }
        for (size_t i = 0; i < w->mesh.nvs; i++) {
            sb(b, ",%016llx", (unsigned long long)dd_db(w->mesh.vs[i]));
        }
        for (size_t i = 0; i < w->mesh.npoints; i++) {
            sb(b, ",%016llx/%016llx", (unsigned long long)dd_db(w->mesh.points[i].x),
               (unsigned long long)dd_db(w->mesh.points[i].y));
        }
    }
}

static inline uint64_t dd_idx(const pc_walk *w, pc_layer_id id)
{
    for (size_t i = 0; i < w->len; i++) {
        if (w->items[i].layer->id == id) {
            return (uint64_t)i + 1;
        }
    }
    return 0;
}

static inline void dd_layer(pc_buf *b, const pc_layer *l);

static inline void dd_content(pc_buf *b, const pc_layer_content *c)
{
    switch (c->kind) {
    case PC_LAYER_RASTER:
        sb(b, "R:");
        s_surf(b, &c->u.raster, false);
        break;
    case PC_LAYER_GROUP:
        sb(b, "G:%d:", c->u.group.expanded);
        if (c->u.group.has_artboard) {
            DJ(b, pc_artboard, &c->u.group.artboard);
        } else {
            sb(b, "-");
        }
        sb(b, "{");
        for (size_t i = 0; i < c->u.group.nchildren; i++) {
            if (i > 0) {
                sb(b, ";");
            }
            dd_layer(b, &c->u.group.children[i]);
        }
        sb(b, "}");
        break;
    case PC_LAYER_ADJUSTMENT:
        sb(b, "A:");
        DJ(b, pc_adjustment, &c->u.adjustment);
        break;
    case PC_LAYER_FILL:
        sb(b, "F:");
        DJ(b, pc_fill, &c->u.fill);
        break;
    case PC_LAYER_TEXT: {
        const pc_text_layer *t = &c->u.text;
        sb(b, "T:");
        sjs(b, pc_str_c(&t->text), t->text.len);
        sb(b, "|");
        sjs(b, pc_str_c(&t->font_family), t->font_family.len);
        sb(b, "|%08x|", fb(t->size_pt));
        s_color(b, &t->color);
        sb(b, "|");
        DJ(b, pc_affine, &t->transform);
        sb(b, "|");
        dd_osurf(b, t->has_cache, &t->cache);
        sb(b, "|");
        dd_blob(b, t->psd_raw);
        sb(b, "|");
        DJ_ARR(b, pc_text_run, t->runs, t->nruns);
        sb(b, "|");
        DJ_ARR(b, pc_paragraph_run, t->paragraphs, t->nparagraphs);
        sb(b, "|");
        DJ(b, pc_text_shape, &t->shape);
        sb(b, "|");
        DJ(b, pc_orientation, &t->orientation);
        sb(b, "|");
        DJ(b, pc_antialias, &t->antialias);
        sb(b, "|");
        if (t->has_warp) {
            DJ(b, pc_text_warp, &t->warp);
        } else {
            sb(b, "-");
        }
        break;
    }
    case PC_LAYER_SHAPE: {
        const pc_shape_layer *s = &c->u.shape;
        sb(b, "S:");
        DJ(b, pc_path, &s->path);
        sb(b, "|");
        if (s->has_fill) {
            DJ(b, pc_fill, &s->fill);
        } else {
            sb(b, "-");
        }
        sb(b, "|");
        if (s->has_stroke) {
            DJ(b, pc_shape_stroke, &s->stroke);
        } else {
            sb(b, "-");
        }
        sb(b, "|");
        if (s->has_live) {
            DJ(b, pc_live_shape, &s->live);
        } else {
            sb(b, "-");
        }
        sb(b, "|");
        dd_osurf(b, s->has_cache, &s->cache);
        sb(b, "|");
        dd_blob(b, s->psd_raw);
        break;
    }
    case PC_LAYER_SMART:
    default: {
        const pc_smart_object *o = &c->u.smart;
        sb(b, "O:");
        if (o->source.linked) {
            sb(b, "link:");
            sjs(b, pc_str_c(&o->source.path), o->source.path.len);
        } else {
            sb(b, "emb:");
            sjs(b, pc_str_c(&o->source.file_name), o->source.file_name.len);
            sb(b, ":");
            sopt(b, pc_blob_data(o->source.bytes), pc_blob_len(o->source.bytes), true);
        }
        sb(b, "|");
        DJ(b, pc_affine, &o->transform);
        sb(b, "|");
        DJ_ARR(b, pc_smart_filter, o->smart_filters, o->nsmart_filters);
        sb(b, "|");
        dd_osurf(b, o->has_cache, &o->cache);
        sb(b, "|");
        dd_blob(b, o->psd_raw);
        sb(b, "|%d|", o->filters_enabled);
        dd_mask(b, o->has_filter_mask, &o->filter_mask);
        sb(b, "|");
        dd_warp(b, o->has_warp, &o->warp);
        sb(b, "|%s|", o->has_stack_mode ? pc_stack_mode_id(o->stack_mode) : "-");
        if (o->has_perspective) {
            for (size_t i = 0; i < 9; i++) {
                sb(b, "%s%016llx", i > 0 ? "," : "", (unsigned long long)dd_db(o->perspective[i]));
            }
        } else {
            sb(b, "-");
        }
        break;
    }
    }
}

static inline void dd_layer(pc_buf *b, const pc_layer *l)
{
    sjs(b, pc_str_c(&l->name), l->name.len);
    sb(b, "|%d|", l->visible);
    DJ(b, pc_locks, &l->locks);
    sb(b, "|");
    DJ(b, pc_blend_mode, &l->blend);
    sb(b, "|%08x|%08x|%d|", fb(l->opacity), fb(l->fill_opacity), l->clipped);
    DJ(b, pc_label_color, &l->label);
    if (l->has_psd_id) {
        sb(b, "|%u", l->psd_id);
    } else {
        sb(b, "|-");
    }
    if (l->has_link_group) {
        sb(b, "|%llu", (unsigned long long)l->link_group);
    } else {
        sb(b, "|-");
    }
    sb(b, "|%u|", l->excluded_channels);
    DJ(b, pc_blend_if, &l->blend_if);
    sb(b, "|");
    DJ(b, pc_advanced_blending, &l->advanced);
    sb(b, "|M:");
    dd_mask(b, l->has_mask, &l->mask);
    sb(b, "|V:");
    if (l->has_vector_mask) {
        DJ(b, pc_vector_mask, &l->vector_mask);
    } else {
        sb(b, "-");
    }
    sb(b, "|X:");
    dd_effects(b, &l->effects);
    sb(b, "|B:[");
    for (size_t i = 0; i < l->npsd_blocks; i++) {
        if (i > 0) {
            sb(b, ",");
        }
        dd_hex(b, l->psd_blocks[i].key, 4);
        sb(b, ":");
        dd_blob(b, l->psd_blocks[i].data);
    }
    sb(b, "]|C:");
    if (l->has_fill_cache) {
        DJ(b, pc_fill, &l->fill_cache.fill);
        sb(b, ":");
        s_surf(b, &l->fill_cache.surface, false);
    } else {
        sb(b, "-");
    }
    sb(b, "|");
    dd_content(b, &l->content);
}

static inline void dd_comp(pc_buf *b, const pc_layer_comp *c, const pc_walk *w)
{
    sb(b, "%u|", c->id);
    sjs(b, pc_str_c(&c->name), c->name.len);
    sb(b, "|");
    sjs(b, pc_str_c(&c->comment), c->comment.len);
    sb(b, "|%d%d%d|[", c->apply_visibility, c->apply_position, c->apply_appearance);
    for (size_t i = 0; i < c->nstates; i++) {
        const pc_comp_layer_state *s = &c->states[i];
        if (i > 0) {
            sb(b, ",");
        }
        sb(b, "%llu:", (unsigned long long)dd_idx(w, s->layer));
        if (s->has_visible) {
            sb(b, "%d:", s->visible);
        } else {
            sb(b, "-:");
        }
        if (s->has_position) {
            sb(b, "%d,%d:", s->position[0], s->position[1]);
        } else {
            sb(b, "-:");
        }
        if (s->has_appearance) {
            DJ(b, pc_blend_mode, &s->appearance.blend);
            sb(b, "/%08x/%08x/", fb(s->appearance.opacity), fb(s->appearance.fill_opacity));
            dd_effects(b, &s->appearance.effects);
        } else {
            sb(b, "-");
        }
    }
    sb(b, "]");
}

static inline void dd_doc(pc_buf *b, const pc_document *d)
{
    pc_walk w;
    pc_slices sl;
    must(pc_document_walk(d, &w, NULL));
    sb(b, "%ux%u ", d->size.width, d->size.height);
    DJ(b, pc_color_mode, &d->mode);
    sb(b, " ");
    DJ(b, pc_sample_type, &d->depth);
    sb(b, " dpi=%08x icc=", fb(d->resolution_dpi));
    dd_blob(b, d->icc_profile);
    sb(b, " exif=");
    dd_blob(b, d->metadata.exif);
    sb(b, " xmp=");
    sopt(b, d->metadata.xmp.ptr, d->metadata.xmp.len, d->metadata.has_xmp);
    sb(b, " guides=");
    DJ(b, pc_guides, &d->guides);
    sb(b, " light=");
    DJ(b, pc_global_light, &d->global_light);
    sb(b, " res=[");
    for (size_t i = 0; i < d->metadata.npsd_resources; i++) {
        const pc_doc_psd_resource *r = &d->metadata.psd_resources[i];
        sb(b, "%s%u:", i > 0 ? "," : "", r->id);
        sjs(b, pc_str_c(&r->name), r->name.len);
        sb(b, ":");
        dd_blob(b, r->data);
    }
    sb(b, "] gb=[");
    for (size_t i = 0; i < d->metadata.npsd_global_blocks; i++) {
        const pc_doc_psd_global_block *g = &d->metadata.psd_global_blocks[i];
        if (i > 0) {
            sb(b, ",");
        }
        dd_hex(b, g->signature, 4);
        sb(b, "/");
        dd_hex(b, g->key, 4);
        sb(b, ":");
        dd_blob(b, g->data);
    }
    sb(b, "] paths=[");
    for (size_t i = 0; i < d->npaths; i++) {
        if (i > 0) {
            sb(b, ",");
        }
        sjs(b, pc_str_c(&d->paths[i].name), d->paths[i].name.len);
        sb(b, ":");
        DJ(b, pc_path, &d->paths[i].path);
        sb(b, ":");
        dd_blob(b, d->paths[i].psd_raw);
    }
    sb(b, "] work=");
    if (d->has_work_path) {
        DJ(b, pc_path, &d->work_path);
    } else {
        sb(b, "-");
    }
    sb(b, " clip=");
    if (d->has_clipping_path) {
        DJ(b, pc_clipping_path, &d->clipping_path);
    } else {
        sb(b, "-");
    }
    sb(b, " pat=[");
    for (size_t i = 0; i < d->npatterns; i++) {
        const pc_pattern *p = &d->patterns[i];
        if (i > 0) {
            sb(b, ";");
        }
        sjs(b, pc_str_c(&p->id), p->id.len);
        sb(b, "|");
        sjs(b, pc_str_c(&p->name), p->name.len);
        sb(b, "|%ux%u|", p->width, p->height);
        s_surf(b, &p->surface, false);
    }
    sb(b, "] ct=");
    if (d->has_color_table) {
        DJ(b, pc_color_table, &d->color_table);
    } else {
        sb(b, "-");
    }
    sb(b, " duo=");
    if (d->has_duotone) {
        DJ(b, pc_duotone, &d->duotone);
    } else {
        sb(b, "-");
    }
    sb(b, " comps=[");
    for (size_t i = 0; i < d->nlayer_comps; i++) {
        if (i > 0) {
            sb(b, ";");
        }
        dd_comp(b, &d->layer_comps[i], &w);
    }
    sb(b, "] la=");
    if (d->has_last_applied_comp) {
        sb(b, "%u", d->last_applied_comp);
    } else {
        sb(b, "-");
    }
    sb(b, " ls=");
    if (d->has_last_document_state) {
        dd_comp(b, &d->last_document_state, &w);
    } else {
        sb(b, "-");
    }
    sb(b, " notes=");
    DJ_ARR(b, pc_note, d->notes, d->nnotes);
    sb(b, " meas=");
    DJ(b, pc_measurement, &d->measurement);
    sb(b, " ts=");
    DJ(b, pc_text_styles, &d->text_styles);
    sb(b, " sl=");
    must(pc_slices_clone(&d->slices, &sl, NULL));
    for (size_t i = 0; i < sl.len; i++) {
        if (sl.list[i].has_layer) {
            sl.list[i].layer = dd_idx(&w, sl.list[i].layer);
        }
    }
    DJ(b, pc_slices, &sl);
    pc_slices_fini(&sl);
    sb(b, " L[");
    for (size_t i = 0; i < d->nlayers; i++) {
        if (i > 0) {
            sb(b, ";");
        }
        dd_layer(b, &d->layers[i]);
    }
    sb(b, "] C[");
    for (size_t i = 0; i < d->nchannels; i++) {
        if (i > 0) {
            sb(b, ";");
        }
        dd_chan(b, &d->channels[i]);
    }
    sb(b, "] Q=");
    if (d->has_quick_mask) {
        dd_chan(b, &d->quick_mask);
    } else {
        sb(b, "-");
    }
    pc_walk_fini(&w);
}

#endif
