/* Port of crates/doc/src/comps.rs: layer comps and artboards. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "doc/doc.h"
#include "doc/tree.h"

/* ---- comp states -------------------------------------------------------------------------- */

void pc_comp_layer_state_fini(pc_comp_layer_state *s)
{
    if (s == NULL) {
        return;
    }
    if (s->has_appearance) {
        pc_effects_fini(&s->appearance.effects);
    }
    memset(s, 0, sizeof *s);
}

pc_status pc_comp_layer_state_clone(const pc_comp_layer_state *s, pc_comp_layer_state *out, pc_error *err)
{
    pc_comp_layer_state c = *s;
    if (s->has_appearance) {
        pc_status st = pc_effects_clone(&s->appearance.effects, &c.appearance.effects, err);
        if (st != PC_OK) {
            return st;
        }
    } else {
        memset(&c.appearance, 0, sizeof c.appearance);
    }
    *out = c;
    return PC_OK;
}

bool pc_comp_layer_state_eq(const pc_comp_layer_state *a, const pc_comp_layer_state *b)
{
    if (a->layer != b->layer || a->has_visible != b->has_visible ||
        (a->has_visible && a->visible != b->visible) || a->has_position != b->has_position ||
        (a->has_position && (a->position[0] != b->position[0] || a->position[1] != b->position[1])) ||
        a->has_appearance != b->has_appearance) {
        return false;
    }
    return !a->has_appearance ||
           (a->appearance.blend == b->appearance.blend && a->appearance.opacity == b->appearance.opacity &&
            a->appearance.fill_opacity == b->appearance.fill_opacity &&
            pc_effects_eq(&a->appearance.effects, &b->appearance.effects));
}

void pc_comp_layer_states_free(pc_comp_layer_state *s, size_t n)
{
    PC_DOC_FINI_VEC(s, n, pc_comp_layer_state_fini);
}

/* ---- comps -------------------------------------------------------------------------------- */

void pc_layer_comp_fini(pc_layer_comp *c)
{
    if (c == NULL) {
        return;
    }
    pc_str_fini(&c->name);
    pc_str_fini(&c->comment);
    PC_DOC_FINI_VEC(c->states, c->nstates, pc_comp_layer_state_fini);
    memset(c, 0, sizeof *c);
}

pc_status pc_layer_comp_clone(const pc_layer_comp *c, pc_layer_comp *out, pc_error *err)
{
    pc_layer_comp r = *c;
    pc_status s;
    memset(&r.name, 0, sizeof r.name);
    memset(&r.comment, 0, sizeof r.comment);
    r.states = NULL;
    r.nstates = 0;
    s = pc_str_clone(&c->name, &r.name, err);
    if (s == PC_OK) {
        s = pc_str_clone(&c->comment, &r.comment, err);
    }
    if (s == PC_OK) {
        PC_DOC_CLONE_VEC(s, c->states, c->nstates, r.states, pc_comp_layer_state_clone,
                         pc_comp_layer_state_fini, err);
        if (s == PC_OK) {
            r.nstates = c->nstates;
        }
    }
    if (s != PC_OK) {
        pc_layer_comp_fini(&r);
        return s;
    }
    *out = r;
    return PC_OK;
}

bool pc_layer_comp_eq(const pc_layer_comp *a, const pc_layer_comp *b)
{
    bool same;
    if (a->id != b->id || !pc_str_eq(&a->name, &b->name) || !pc_str_eq(&a->comment, &b->comment) ||
        a->apply_visibility != b->apply_visibility || a->apply_position != b->apply_position ||
        a->apply_appearance != b->apply_appearance) {
        return false;
    }
    PC_DOC_EQ_VEC(same, a->states, a->nstates, b->states, b->nstates, pc_comp_layer_state_eq);
    return same;
}

int32_t pc_layer_comp_captured_info(const pc_layer_comp *c)
{
    return (c->apply_visibility ? 1 : 0) | (c->apply_position ? 2 : 0) | (c->apply_appearance ? 4 : 0);
}

void pc_layer_comp_set_captured_info(pc_layer_comp *c, int32_t v)
{
    c->apply_visibility = (v & 1) != 0;
    c->apply_position = (v & 2) != 0;
    c->apply_appearance = (v & 4) != 0;
}

const pc_comp_layer_state *pc_layer_comp_state(const pc_layer_comp *c, pc_layer_id id)
{
    size_t i;
    for (i = 0; c->states != NULL && i < c->nstates; i++) {
        if (c->states[i].layer == id) {
            return &c->states[i];
        }
    }
    return NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

pc_status pc_layer_comp_missing_layers(const pc_layer_comp *c, const pc_document *doc, pc_layer_id **out,
                                       size_t *n, pc_error *err)
{
    uint64_t *live;
    size_t nlive;
    pc_layer_id *miss = NULL;
    size_t len = 0;
    size_t i;
    pc_status s = pc_doc_sorted_ids(doc->layers, doc->nlayers, &live, &nlive, err);
    if (s != PC_OK) {
        return s;
    }
    if (c->nstates > 0 && c->states != NULL) {
        miss = pc_malloc_array(c->nstates, sizeof *miss);
        if (miss == NULL) {
            free(live);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory listing missing layers");
        }
        for (i = 0; i < c->nstates; i++) {
            uint64_t id = c->states[i].layer;
            if (nlive == 0 || bsearch(&id, live, nlive, sizeof *live, cmp_u64) == NULL) {
                miss[len++] = id;
            }
        }
    }
    free(live);
    if (len == 0) {
        free(miss);
        miss = NULL;
    }
    *out = miss;
    *n = len;
    return PC_OK;
}

/* ---- positions and capture ---------------------------------------------------------------- */

pc_rect pc_comp_position_bounds(const pc_layer *l)
{
    const pc_surface *s;
    switch (l->content.kind) {
    case PC_LAYER_GROUP:
        return l->content.u.group.has_artboard ? l->content.u.group.artboard.rect : PC_RECT_EMPTY;
    case PC_LAYER_ADJUSTMENT:
    case PC_LAYER_FILL:
        return PC_RECT_EMPTY;
    default:
        s = pc_layer_surface(l);
        return s != NULL ? pc_surface_content_bounds(s) : PC_RECT_EMPTY;
    }
}

bool pc_comp_layer_position(const pc_layer *l, int32_t out[2])
{
    pc_rect r = pc_comp_position_bounds(l);
    if (pc_rect_is_empty(r)) {
        return false;
    }
    out[0] = r.x0;
    out[1] = r.y0;
    return true;
}

pc_status pc_comp_capture_states(const pc_document *doc, pc_comp_layer_state **out, size_t *n, pc_error *err)
{
    pc_doc_iter it;
    const pc_layer *l;
    pc_comp_layer_state *v = NULL;
    size_t len = 0;
    size_t cap = 0;
    pc_status s = PC_OK;
    int r;
    pc_doc_iter_init(&it, doc->layers, doc->nlayers);
    while ((r = pc_doc_iter_next(&it, &l)) == 1) {
        pc_comp_layer_state st;
        if (len == cap) {
            pc_comp_layer_state *g = pc_grow_array(v, &cap, len + 1, sizeof *v);
            if (g == NULL) {
                s = pc_fail(err, PC_ERR_NOMEM, "out of memory capturing layer states");
                break;
            }
            v = g;
        }
        memset(&st, 0, sizeof st);
        st.layer = l->id;
        st.has_visible = true;
        st.visible = l->visible;
        st.has_position = pc_comp_layer_position(l, st.position);
        st.has_appearance = true;
        st.appearance.blend = l->blend;
        st.appearance.opacity = l->opacity;
        st.appearance.fill_opacity = l->fill_opacity;
        s = pc_effects_clone(&l->effects, &st.appearance.effects, err);
        if (s != PC_OK) {
            break;
        }
        v[len++] = st;
    }
    pc_doc_iter_fini(&it);
    if (s == PC_OK && r < 0) {
        s = pc_fail(err, PC_ERR_NOMEM, "out of memory walking the layer tree");
    }
    if (s != PC_OK) {
        pc_comp_layer_states_free(v, len);
        return s;
    }
    *out = v;
    *n = len;
    return PC_OK;
}

uint32_t pc_next_comp_id(const pc_document *doc)
{
    uint32_t max = 0;
    uint32_t id;
    size_t i;
    size_t n = doc->layer_comps != NULL ? doc->nlayer_comps : 0;
    bool taken;
    for (i = 0; i < n; i++) {
        if (doc->layer_comps[i].id > max) {
            max = doc->layer_comps[i].id;
        }
    }
    /* wrapping_add(1).max(1): never 0, the Last Document State's id. */
    id = max + 1u;
    if (id == 0) {
        id = 1;
    }
    do {
        taken = false;
        for (i = 0; i < n; i++) {
            if (doc->layer_comps[i].id == id) {
                taken = true;
                id = id + 1u;
                if (id == 0) {
                    id = 1;
                }
                break;
            }
        }
    } while (taken);
    return id;
}

/* ---- artboards ---------------------------------------------------------------------------- */

bool pc_artboard_background_eq(const pc_artboard_background *a, const pc_artboard_background *b)
{
    return a->kind == b->kind && (a->kind != PC_ARTBOARD_CUSTOM || pc_doc_color_eq(&a->custom, &b->custom));
}

bool pc_artboard_background_rgba(const pc_artboard_background *b, float out[4])
{
    switch (b->kind) {
    case PC_ARTBOARD_WHITE:
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return true;
    case PC_ARTBOARD_BLACK:
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return true;
    case PC_ARTBOARD_CUSTOM: {
        float rgb[3];
        if (!(b->custom.alpha > 0.0f)) {
            return false;
        }
        pc_color_to_rgb(&b->custom, rgb);
        out[0] = rgb[0];
        out[1] = rgb[1];
        out[2] = rgb[2];
        out[3] = b->custom.alpha;
        return true;
    }
    default:
        return false;
    }
}

int32_t pc_artboard_background_psd_type(const pc_artboard_background *b)
{
    switch (b->kind) {
    case PC_ARTBOARD_WHITE:
        return 1;
    case PC_ARTBOARD_BLACK:
        return 2;
    case PC_ARTBOARD_TRANSPARENT:
        return 3;
    default:
        return 4;
    }
}

const char *pc_artboard_background_name(const pc_artboard_background *b)
{
    switch (b->kind) {
    case PC_ARTBOARD_WHITE:
        return "white";
    case PC_ARTBOARD_BLACK:
        return "black";
    case PC_ARTBOARD_TRANSPARENT:
        return "transparent";
    default:
        return "custom";
    }
}

void pc_artboard_init(pc_artboard *out, pc_rect rect)
{
    memset(out, 0, sizeof *out);
    out->rect = rect;
    out->background.kind = PC_ARTBOARD_WHITE;
}

void pc_artboard_fini(pc_artboard *a)
{
    if (a == NULL) {
        return;
    }
    pc_str_fini(&a->preset);
    memset(a, 0, sizeof *a);
}

pc_status pc_artboard_clone(const pc_artboard *a, pc_artboard *out, pc_error *err)
{
    pc_artboard c = *a;
    pc_status s = pc_str_clone(&a->preset, &c.preset, err);
    if (s != PC_OK) {
        return s;
    }
    *out = c;
    return PC_OK;
}

bool pc_artboard_eq(const pc_artboard *a, const pc_artboard *b)
{
    return pc_rect_eq(a->rect, b->rect) && pc_artboard_background_eq(&a->background, &b->background) &&
           pc_str_eq(&a->preset, &b->preset);
}

pc_status pc_document_artboards(const pc_document *doc, pc_artboard_ref **out, size_t *n, pc_error *err)
{
    pc_artboard_ref *v = NULL;
    size_t len = 0;
    size_t i;
    size_t nl = doc->layers != NULL ? doc->nlayers : 0;
    if (pc_document_has_artboards(doc)) {
        v = pc_malloc_array(nl, sizeof *v);
        if (v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory listing artboards");
        }
        for (i = 0; i < nl; i++) {
            const pc_artboard *a = pc_layer_artboard(&doc->layers[i]);
            if (a != NULL) {
                v[len].layer = doc->layers[i].id;
                v[len].name = &doc->layers[i].name;
                v[len].artboard = a;
                len++;
            }
        }
    }
    *out = v;
    *n = len;
    return PC_OK;
}

bool pc_document_has_artboards(const pc_document *doc)
{
    size_t i;
    for (i = 0; doc->layers != NULL && i < doc->nlayers; i++) {
        if (pc_layer_artboard(&doc->layers[i]) != NULL) {
            return true;
        }
    }
    return false;
}

const pc_layer_comp *pc_document_comp(const pc_document *doc, uint32_t id)
{
    size_t i;
    for (i = 0; doc->layer_comps != NULL && i < doc->nlayer_comps; i++) {
        if (doc->layer_comps[i].id == id) {
            return &doc->layer_comps[i];
        }
    }
    return NULL;
}

bool pc_document_artboard_of(const pc_document *doc, pc_layer_id id, pc_layer_id *out)
{
    pc_doc_iter it;
    const pc_layer *l;
    bool found = false;
    if (pc_doc_iter_find(&it, doc->layers, doc->nlayers, id, &l) == 1) {
        const pc_layer *top = &doc->layers[pc_doc_iter_index(&it, 0)];
        if (pc_layer_artboard(top) != NULL) {
            *out = top->id;
            found = true;
        }
    }
    pc_doc_iter_fini(&it);
    return found;
}

const pc_artboard *pc_layer_artboard(const pc_layer *l)
{
    if (l->content.kind == PC_LAYER_GROUP && l->content.u.group.has_artboard) {
        return &l->content.u.group.artboard;
    }
    return NULL;
}

pc_artboard *pc_layer_artboard_mut(pc_layer *l)
{
    if (l->content.kind == PC_LAYER_GROUP && l->content.u.group.has_artboard) {
        return &l->content.u.group.artboard;
    }
    return NULL;
}
