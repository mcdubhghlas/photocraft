/* Document -> manifest (crates/format/src/convert.rs, the save half: check_nesting, doc_m and
 * the Manifest wrapper the writer builds in store::prepare).
 *
 * The manifest is written straight from the document as a yyjson tree, field by field in the
 * order of the reference's serde structs (manifest.h mirrors them), with doc types written by
 * their own serde writers. Floats keep NaN and the infinities as real numbers
 * (PC_DOC_JSON_NONFINITE) so the store's finite check can name them; nothing here prints JSON.
 * Every tile and blob goes through the store's sink, which hashes it and keeps it for the save.
 *
 * Group nesting is bounded by PC_MAX_GROUP_DEPTH (check_nesting runs first), so the recursion
 * over the layer tree is bounded as in the reference. */
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "doc/json_util.h"
#include "format/bridge.h"
#include "format/convert.h"
#include "format/manifest.h"
#include "raster/pixel.h"

#define NF PC_DOC_JSON_NONFINITE

/* ---- keeping the native format complete (AGENTS.md §8) ----
 *
 * The reference destructures the doc structs in convert.rs, so a new field fails to compile
 * until the manifest saves it. C can't enumerate a struct's fields; instead these assertions
 * pin the size of every doc struct the conversion copies field by field. Adding a field breaks
 * the build here until manifest.h/.c (the model and its decoding), convert_save.c and
 * convert_load.c carry it (with a serde default, so older bundles still load) and the size is
 * updated. The sizes were measured on LP64 hosts (Linux/macOS/BSD on x86-64 and aarch64); other
 * ABIs skip the check. A field that fits into existing padding keeps the size, so
 * tests/format/test_manifest.c also round-trips a document with every field set. */
#if UINTPTR_MAX == UINT64_MAX && UINT_MAX == 0xFFFFFFFFu && SIZE_MAX == UINT64_MAX
#define PC_DOC_FIELDS(T, size) _Static_assert(sizeof(T) == (size), #T " changed: update the .pcraft manifest")
PC_DOC_FIELDS(pc_document, 1016);
PC_DOC_FIELDS(pc_layer, 1040);
PC_DOC_FIELDS(pc_layer_content, 512);
PC_DOC_FIELDS(pc_group, 88);
PC_DOC_FIELDS(pc_text_layer, 288);
PC_DOC_FIELDS(pc_shape_layer, 488);
PC_DOC_FIELDS(pc_smart_object, 504);
PC_DOC_FIELDS(pc_smart_source, 48);
PC_DOC_FIELDS(pc_layer_mask, 80);
PC_DOC_FIELDS(pc_fill_cache, 152);
PC_DOC_FIELDS(pc_video_data, 48);
PC_DOC_FIELDS(pc_effects, 56);
PC_DOC_FIELDS(pc_alpha_channel, 144);
PC_DOC_FIELDS(pc_layer_comp, 64);
PC_DOC_FIELDS(pc_comp_layer_state, 96);
PC_DOC_FIELDS(pc_comp_appearance, 72);
PC_DOC_FIELDS(pc_metadata, 64);
PC_DOC_FIELDS(pc_doc_psd_block, 16);
PC_DOC_FIELDS(pc_doc_psd_resource, 32);
PC_DOC_FIELDS(pc_doc_psd_global_block, 16);
PC_DOC_FIELDS(pc_named_path, 48);
PC_DOC_FIELDS(pc_pattern, 104);
#undef PC_DOC_FIELDS
#endif

/* ---- check_nesting ---- */

static pc_status too_deep(pc_error *err)
{
    return pc_format_limit(err, "layer groups nested deeper than %d", PC_MAX_GROUP_DEPTH);
}

/* The reference's too_deep_at: true at the first non-empty layer list past the limit. The
 * recursion stops there, so it is at most PC_MAX_GROUP_DEPTH + 2 frames deep. */
static bool too_deep_at(const pc_layer *layers, size_t n, size_t depth)
{
    if (depth > PC_MAX_GROUP_DEPTH && n > 0) {
        return true;
    }
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = &layers[i];
        if (l->content.kind == PC_LAYER_GROUP &&
            too_deep_at(l->content.u.group.children, l->content.u.group.nchildren, depth + 1)) {
            return true;
        }
    }
    return false;
}

pc_status pc_format_check_nesting(const pc_document *doc, pc_error *err)
{
    return too_deep_at(doc->layers, doc->nlayers, 0) ? too_deep(err) : PC_OK;
}

/* ---- hex ---- */

void pc_format_hex(const uint8_t *bytes, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[bytes[i] >> 4];
        out[2 * i + 1] = H[bytes[i] & 15u];
    }
    out[2 * n] = '\0';
}

/* ---- the builder: a sticky error so the field lists read like the reference ---- */

typedef struct wb {
    yyjson_mut_doc *d;
    pc_format_sink *sink;
    pc_status st;
    pc_error *err;
} wb;

static bool ok(const wb *w)
{
    return w->st == PC_OK;
}

static yyjson_mut_val *oom(wb *w)
{
    if (w->st == PC_OK) {
        w->st = pc_fail(w->err, PC_ERR_NOMEM, "out of memory");
    }
    return NULL;
}

/* Adds key: v to obj (key is a string literal; yyjson keeps the pointer). */
static void put(wb *w, yyjson_mut_val *obj, const char *key, yyjson_mut_val *v)
{
    if (!ok(w)) {
        return;
    }
    if (obj == NULL || v == NULL || !yyjson_mut_obj_add(obj, yyjson_mut_str(w->d, key), v)) {
        (void)oom(w);
    }
}

static void push(wb *w, yyjson_mut_val *arr, yyjson_mut_val *v)
{
    if (!ok(w)) {
        return;
    }
    if (arr == NULL || v == NULL || !yyjson_mut_arr_append(arr, v)) {
        (void)oom(w);
    }
}

static yyjson_mut_val *obj(wb *w)
{
    yyjson_mut_val *o = ok(w) ? yyjson_mut_obj(w->d) : NULL;
    return o != NULL ? o : oom(w);
}

static yyjson_mut_val *arr(wb *w)
{
    yyjson_mut_val *a = ok(w) ? yyjson_mut_arr(w->d) : NULL;
    return a != NULL ? a : oom(w);
}

static yyjson_mut_val *jnull(wb *w)
{
    return yyjson_mut_null(w->d);
}

static yyjson_mut_val *jbool(wb *w, bool b)
{
    return yyjson_mut_bool(w->d, b);
}

static yyjson_mut_val *ju64(wb *w, uint64_t v)
{
    return yyjson_mut_uint(w->d, v);
}

static yyjson_mut_val *ji64(wb *w, int64_t v)
{
    return yyjson_mut_sint(w->d, v);
}

static yyjson_mut_val *jf32(wb *w, float v)
{
    return pc_jw_f32(w->d, v, NF);
}

static yyjson_mut_val *jf64(wb *w, double v)
{
    return pc_jw_f64(w->d, v, NF);
}

static yyjson_mut_val *jstr(wb *w, const pc_str *s)
{
    return pc_jw_str(w->d, s);
}

static yyjson_mut_val *jcstr(wb *w, const char *s)
{
    return yyjson_mut_strcpy(w->d, s);
}

/* An Option: the value (only built when present), or null. Uses the builder `w` in scope. */
#define OPT(has, v) ((has) ? (v) : jnull(w))

static yyjson_mut_val *blob(wb *w, pc_blob *b)
{
    char h[PC_FORMAT_HASH_HEX + 1];
    if (!ok(w)) {
        return NULL;
    }
    w->st = pc_format_sink_blob(w->sink, b, h, w->err);
    return ok(w) ? jcstr(w, h) : NULL;
}

/* Option<Arc<Vec<u8>>> as Option<Hash> (NULL = None). */
static yyjson_mut_val *opt_blob(wb *w, pc_blob *b)
{
    return b != NULL ? blob(w, b) : jnull(w);
}

static yyjson_mut_val *hexv(wb *w, const uint8_t *bytes, size_t n)
{
    char buf[2 * 8 + 1];
    if (n > 8) {
        return oom(w);
    }
    pc_format_hex(bytes, n, buf);
    return jcstr(w, buf);
}

static yyjson_mut_val *f64_array(wb *w, const double *v, size_t n)
{
    yyjson_mut_val *a = arr(w);
    for (size_t i = 0; i < n && ok(w); i++) {
        push(w, a, jf64(w, v[i]));
    }
    return a;
}

/* ---- surfaces ---- */

static yyjson_mut_val *surface_m(wb *w, const pc_surface *s)
{
    pc_pixel_format f = pc_surface_format(s);
    size_t bpp = pc_pixel_format_bytes_per_pixel(f);
    float px[PC_RASTER_MAX_CHANNELS];
    uint8_t dp[PC_RASTER_MAX_PIXEL_BYTES];
    char hex[2 * PC_RASTER_MAX_PIXEL_BYTES + 1];
    pc_tile_entry *tiles = NULL;
    size_t ntiles = 0;
    yyjson_mut_val *o = obj(w);
    yyjson_mut_val *ts;
    if (!ok(w)) {
        return NULL;
    }
    /* The default pixel re-encoded from its decoded value, as encode_pixel(default_pixel()). */
    memset(dp, 0, sizeof dp);
    pc_surface_default_pixel(s, px);
    pc_encode_pixel(f, px, pc_surface_channels(s), dp);
    pc_format_swap_to_le(dp, bpp, f.sample);
    pc_format_hex(dp, bpp, hex);
    put(w, o, "format", pc_pixel_format_to_json(w->d, &f, NF));
    put(w, o, "default", jcstr(w, hex));
    if (ok(w)) {
        w->st = pc_surface_tiles_sorted(s, &tiles, &ntiles, w->err);
    }
    ts = arr(w);
    for (size_t i = 0; i < ntiles && ok(w); i++) {
        char h[PC_FORMAT_HASH_HEX + 1];
        yyjson_mut_val *t = obj(w);
        w->st = pc_format_sink_tile(w->sink, f, tiles[i].tile, h, w->err);
        put(w, t, "tx", ji64(w, tiles[i].coord.tx));
        put(w, t, "ty", ji64(w, tiles[i].coord.ty));
        put(w, t, "hash", ok(w) ? jcstr(w, h) : NULL);
        push(w, ts, t);
    }
    pc_tile_entries_free(tiles, ntiles);
    put(w, o, "tiles", ts);
    return ok(w) ? o : NULL;
}

static yyjson_mut_val *mask_m(wb *w, const pc_layer_mask *m)
{
    yyjson_mut_val *o = obj(w);
    put(w, o, "surface", surface_m(w, &m->surface));
    put(w, o, "enabled", jbool(w, m->enabled));
    put(w, o, "linked", jbool(w, m->linked));
    put(w, o, "density", jf32(w, m->density));
    put(w, o, "feather", jf32(w, m->feather));
    return o;
}

static yyjson_mut_val *effects_m(wb *w, const pc_effects *e)
{
    yyjson_mut_val *o = obj(w);
    yyjson_mut_val *items = arr(w);
    for (size_t i = 0; i < e->nitems && ok(w); i++) {
        push(w, items, pc_effect_to_json(w->d, &e->items[i], NF));
    }
    put(w, o, "enabled", jbool(w, e->enabled));
    put(w, o, "items", items);
    put(w, o, "psd_raw", opt_blob(w, e->psd_raw));
    put(w, o, "reference", OPT(e->has_reference, f64_array(w, e->reference, 2)));
    return o;
}

/* ---- warp (geom::warp::Warp, camelCase) ---- */

static const char *const WARP_STYLES[PC_WARP_STYLE_COUNT] = {
    "none", "custom", "arc",  "arcLower", "arcUpper", "arch",    "bulge",   "shellLower", "shellUpper",
    "flag", "wave",   "fish", "rise",     "fisheye",  "inflate", "squeeze", "twist",
};

static yyjson_mut_val *warp_m(wb *w, const pc_warp *wp)
{
    yyjson_mut_val *o = obj(w);
    unsigned st = (unsigned)wp->style;
    put(w, o, "style", st < PC_WARP_STYLE_COUNT ? jcstr(w, WARP_STYLES[st]) : jnull(w));
    put(w, o, "bend", jf64(w, wp->bend));
    put(w, o, "hDistort", jf64(w, wp->h_distort));
    put(w, o, "vDistort", jf64(w, wp->v_distort));
    put(w, o, "vertical", jbool(w, wp->vertical));
    put(w, o, "bounds", f64_array(w, wp->bounds, 4));
    if (wp->has_mesh) {
        const pc_mesh *m = &wp->mesh;
        yyjson_mut_val *mo = obj(w);
        yyjson_mut_val *pts = arr(w);
        for (size_t i = 0; i < m->npoints && ok(w); i++) {
            double xy[2] = {m->points[i].x, m->points[i].y};
            push(w, pts, f64_array(w, xy, 2));
        }
        put(w, mo, "us", f64_array(w, m->us, m->nus));
        put(w, mo, "vs", f64_array(w, m->vs, m->nvs));
        put(w, mo, "points", pts);
        put(w, o, "mesh", mo);
    } else {
        put(w, o, "mesh", jnull(w));
    }
    return o;
}

/* ---- layers ---- */

static yyjson_mut_val *layer_m(wb *w, const pc_layer *l, size_t depth);

static const char *const CONTENT_KINDS[] = {"raster", "group", "adjustment", "fill",
                                            "text",   "shape", "smart"};

static yyjson_mut_val *content_m(wb *w, const pc_layer_content *c, size_t depth)
{
    yyjson_mut_val *o = obj(w);
    unsigned k = (unsigned)c->kind;
    if (k >= sizeof CONTENT_KINDS / sizeof CONTENT_KINDS[0]) {
        if (ok(w)) {
            w->st = pc_fail(w->err, PC_ERR_INVALID, "unknown layer kind %u", k);
        }
        return NULL;
    }
    put(w, o, "kind", jcstr(w, CONTENT_KINDS[k]));
    switch (c->kind) {
    case PC_LAYER_RASTER:
        put(w, o, "surface", surface_m(w, &c->u.raster));
        break;
    case PC_LAYER_GROUP: {
        const pc_group *g = &c->u.group;
        yyjson_mut_val *kids = arr(w);
        for (size_t i = 0; i < g->nchildren && ok(w); i++) {
            push(w, kids, layer_m(w, &g->children[i], depth + 1));
        }
        put(w, o, "children", kids);
        put(w, o, "expanded", jbool(w, g->expanded));
        put(w, o, "artboard", OPT(g->has_artboard, pc_artboard_to_json(w->d, &g->artboard, NF)));
        break;
    }
    case PC_LAYER_ADJUSTMENT:
        put(w, o, "adjustment", pc_adjustment_to_json(w->d, &c->u.adjustment, NF));
        break;
    case PC_LAYER_FILL:
        put(w, o, "fill", pc_fill_to_json(w->d, &c->u.fill, NF));
        break;
    case PC_LAYER_TEXT: {
        const pc_text_layer *t = &c->u.text;
        yyjson_mut_val *runs = arr(w);
        yyjson_mut_val *paras = arr(w);
        put(w, o, "text", jstr(w, &t->text));
        put(w, o, "font_family", jstr(w, &t->font_family));
        put(w, o, "size_pt", jf32(w, t->size_pt));
        put(w, o, "color", pc_color_to_json(w->d, &t->color, NF));
        put(w, o, "transform", pc_affine_to_json(w->d, &t->transform, NF));
        put(w, o, "cache", t->has_cache ? surface_m(w, &t->cache) : jnull(w));
        put(w, o, "psd_raw", opt_blob(w, t->psd_raw));
        for (size_t i = 0; i < t->nruns && ok(w); i++) {
            push(w, runs, pc_text_run_to_json(w->d, &t->runs[i], NF));
        }
        for (size_t i = 0; i < t->nparagraphs && ok(w); i++) {
            push(w, paras, pc_paragraph_run_to_json(w->d, &t->paragraphs[i], NF));
        }
        put(w, o, "runs", runs);
        put(w, o, "paragraphs", paras);
        put(w, o, "shape", pc_text_shape_to_json(w->d, &t->shape, NF));
        put(w, o, "orientation", pc_orientation_to_json(w->d, &t->orientation, NF));
        put(w, o, "antialias", pc_antialias_to_json(w->d, &t->antialias, NF));
        put(w, o, "warp", OPT(t->has_warp, pc_text_warp_to_json(w->d, &t->warp, NF)));
        break;
    }
    case PC_LAYER_SHAPE: {
        const pc_shape_layer *s = &c->u.shape;
        put(w, o, "fill", OPT(s->has_fill, pc_fill_to_json(w->d, &s->fill, NF)));
        put(w, o, "cache", s->has_cache ? surface_m(w, &s->cache) : jnull(w));
        put(w, o, "psd_raw", opt_blob(w, s->psd_raw));
        put(w, o, "path", pc_path_to_json(w->d, &s->path, NF));
        put(w, o, "stroke", OPT(s->has_stroke, pc_shape_stroke_to_json(w->d, &s->stroke, NF)));
        put(w, o, "live", OPT(s->has_live, pc_live_shape_to_json(w->d, &s->live, NF)));
        break;
    }
    case PC_LAYER_SMART: {
        const pc_smart_object *s = &c->u.smart;
        yyjson_mut_val *src = obj(w);
        yyjson_mut_val *filters = arr(w);
        if (s->source.linked) {
            put(w, src, "kind", jcstr(w, "linked"));
            put(w, src, "path", jstr(w, &s->source.path));
        } else {
            put(w, src, "kind", jcstr(w, "embedded"));
            put(w, src, "file_name", jstr(w, &s->source.file_name));
            put(w, src, "blob", blob(w, s->source.bytes));
        }
        put(w, o, "source", src);
        put(w, o, "transform", pc_affine_to_json(w->d, &s->transform, NF));
        for (size_t i = 0; i < s->nsmart_filters && ok(w); i++) {
            push(w, filters, pc_smart_filter_to_json(w->d, &s->smart_filters[i], NF));
        }
        put(w, o, "smart_filters", filters);
        put(w, o, "cache", s->has_cache ? surface_m(w, &s->cache) : jnull(w));
        put(w, o, "psd_raw", opt_blob(w, s->psd_raw));
        put(w, o, "filters_enabled", jbool(w, s->filters_enabled));
        put(w, o, "filter_mask", s->has_filter_mask ? mask_m(w, &s->filter_mask) : jnull(w));
        put(w, o, "warp", s->has_warp ? warp_m(w, &s->warp) : jnull(w));
        put(w, o, "stack_mode", OPT(s->has_stack_mode, pc_stack_mode_to_json(w->d, &s->stack_mode, NF)));
        if (s->has_perspective) {
            put(w, o, "perspective", f64_array(w, s->perspective, 9));
        }
        break;
    }
    }
    return o;
}

static yyjson_mut_val *surface_list(wb *w, const pc_surface *s, size_t n)
{
    yyjson_mut_val *a = arr(w);
    for (size_t i = 0; i < n && ok(w); i++) {
        push(w, a, surface_m(w, &s[i]));
    }
    return a;
}

static yyjson_mut_val *layer_m(wb *w, const pc_layer *l, size_t depth)
{
    yyjson_mut_val *o;
    yyjson_mut_val *blocks;
    if (!ok(w)) {
        return NULL;
    }
    if (depth > PC_MAX_GROUP_DEPTH) {
        w->st = too_deep(w->err); /* check_nesting runs first; this guards direct callers */
        return NULL;
    }
    o = obj(w);
    put(w, o, "id", ju64(w, l->id));
    put(w, o, "name", jstr(w, &l->name));
    put(w, o, "visible", jbool(w, l->visible));
    put(w, o, "locks", pc_locks_to_json(w->d, &l->locks, NF));
    put(w, o, "blend", pc_blend_mode_to_json(w->d, &l->blend, NF));
    put(w, o, "opacity", jf32(w, l->opacity));
    put(w, o, "fill_opacity", jf32(w, l->fill_opacity));
    put(w, o, "clipped", jbool(w, l->clipped));
    put(w, o, "mask", l->has_mask ? mask_m(w, &l->mask) : jnull(w));
    put(w, o, "vector_mask", OPT(l->has_vector_mask, pc_vector_mask_to_json(w->d, &l->vector_mask, NF)));
    put(w, o, "effects", effects_m(w, &l->effects));
    put(w, o, "label", pc_label_color_to_json(w->d, &l->label, NF));
    put(w, o, "content", content_m(w, &l->content, depth));
    blocks = arr(w);
    for (size_t i = 0; i < l->npsd_blocks && ok(w); i++) {
        yyjson_mut_val *t = arr(w);
        push(w, t, hexv(w, l->psd_blocks[i].key, 4));
        push(w, t, blob(w, l->psd_blocks[i].data));
        push(w, blocks, t);
    }
    put(w, o, "psd_blocks", blocks);
    put(w, o, "psd_id", OPT(l->has_psd_id, ju64(w, l->psd_id)));
    if (l->has_fill_cache) {
        yyjson_mut_val *fc = obj(w);
        put(w, fc, "fill", pc_fill_to_json(w->d, &l->fill_cache.fill, NF));
        put(w, fc, "surface", surface_m(w, &l->fill_cache.surface));
        put(w, o, "fill_cache", fc);
    } else {
        put(w, o, "fill_cache", jnull(w));
    }
    put(w, o, "link_group", OPT(l->has_link_group, ju64(w, l->link_group)));
    if (l->excluded_channels != 0) {
        put(w, o, "excluded_channels", ju64(w, l->excluded_channels));
    }
    if (!pc_blend_if_is_default(&l->blend_if)) {
        put(w, o, "blend_if", pc_blend_if_to_json(w->d, &l->blend_if, NF));
    }
    if (!pc_advanced_blending_is_default(&l->advanced)) {
        put(w, o, "advanced", pc_advanced_blending_to_json(w->d, &l->advanced, NF));
    }
    if (l->has_video) {
        const pc_video_data *v = &l->video;
        yyjson_mut_val *vo = obj(w);
        put(w, vo, "frames", surface_list(w, v->frames, v->nframes));
        put(w, vo, "source", pc_video_source_to_json(w->d, &v->source, NF));
        put(w, vo, "fps", jf32(w, v->fps));
        put(w, vo, "show_altered", jbool(w, v->show_altered));
        put(w, o, "video", vo);
    } else {
        put(w, o, "video", jnull(w));
    }
    return ok(w) ? o : NULL;
}

/* ---- document parts ---- */

static yyjson_mut_val *channel_m(wb *w, const pc_alpha_channel *c)
{
    yyjson_mut_val *o = obj(w);
    put(w, o, "name", jstr(w, &c->name));
    put(w, o, "surface", surface_m(w, &c->surface));
    if (c->has_spot) {
        yyjson_mut_val *t = arr(w);
        push(w, t, pc_color_to_json(w->d, &c->spot_color, NF));
        push(w, t, jf32(w, c->spot_solidity));
        put(w, o, "spot", t);
    } else {
        put(w, o, "spot", jnull(w));
    }
    put(w, o, "color", pc_color_to_json(w->d, &c->color, NF));
    put(w, o, "opacity", jf32(w, c->opacity));
    put(w, o, "indicates", pc_color_indicates_to_json(w->d, &c->indicates, NF));
    return o;
}

static yyjson_mut_val *comp_m(wb *w, const pc_layer_comp *c)
{
    yyjson_mut_val *o = obj(w);
    yyjson_mut_val *states = arr(w);
    put(w, o, "id", ju64(w, c->id));
    put(w, o, "name", jstr(w, &c->name));
    put(w, o, "comment", jstr(w, &c->comment));
    put(w, o, "apply_visibility", jbool(w, c->apply_visibility));
    put(w, o, "apply_position", jbool(w, c->apply_position));
    put(w, o, "apply_appearance", jbool(w, c->apply_appearance));
    for (size_t i = 0; i < c->nstates && ok(w); i++) {
        const pc_comp_layer_state *s = &c->states[i];
        yyjson_mut_val *so = obj(w);
        put(w, so, "layer", ju64(w, s->layer));
        put(w, so, "visible", OPT(s->has_visible, jbool(w, s->visible)));
        if (s->has_position) {
            yyjson_mut_val *p = arr(w);
            push(w, p, ji64(w, s->position[0]));
            push(w, p, ji64(w, s->position[1]));
            put(w, so, "position", p);
        } else {
            put(w, so, "position", jnull(w));
        }
        if (s->has_appearance) {
            const pc_comp_appearance *a = &s->appearance;
            yyjson_mut_val *ao = obj(w);
            put(w, ao, "blend", pc_blend_mode_to_json(w->d, &a->blend, NF));
            put(w, ao, "opacity", jf32(w, a->opacity));
            put(w, ao, "fill_opacity", jf32(w, a->fill_opacity));
            put(w, ao, "effects", effects_m(w, &a->effects));
            put(w, so, "appearance", ao);
        } else {
            put(w, so, "appearance", jnull(w));
        }
        push(w, states, so);
    }
    put(w, o, "states", states);
    return o;
}

static yyjson_mut_val *metadata_m(wb *w, const pc_metadata *m)
{
    yyjson_mut_val *o = obj(w);
    yyjson_mut_val *res = arr(w);
    yyjson_mut_val *glob = arr(w);
    put(w, o, "xmp", OPT(m->has_xmp, jstr(w, &m->xmp)));
    put(w, o, "exif", opt_blob(w, m->exif));
    for (size_t i = 0; i < m->npsd_resources && ok(w); i++) {
        yyjson_mut_val *t = arr(w);
        push(w, t, ju64(w, m->psd_resources[i].id));
        push(w, t, jstr(w, &m->psd_resources[i].name));
        push(w, t, blob(w, m->psd_resources[i].data));
        push(w, res, t);
    }
    for (size_t i = 0; i < m->npsd_global_blocks && ok(w); i++) {
        yyjson_mut_val *t = arr(w);
        push(w, t, hexv(w, m->psd_global_blocks[i].signature, 4));
        push(w, t, hexv(w, m->psd_global_blocks[i].key, 4));
        push(w, t, blob(w, m->psd_global_blocks[i].data));
        push(w, glob, t);
    }
    put(w, o, "psd_resources", res);
    put(w, o, "psd_global_blocks", glob);
    return o;
}

/* doc_m */
static yyjson_mut_val *doc_m(wb *w, const pc_document *d)
{
    yyjson_mut_val *o = obj(w);
    yyjson_mut_val *a;
    put(w, o, "id", ju64(w, d->id));
    put(w, o, "name", jstr(w, &d->name));
    put(w, o, "size", pc_size_to_json(w->d, &d->size, NF));
    put(w, o, "resolution_dpi", jf32(w, d->resolution_dpi));
    put(w, o, "mode", pc_color_mode_to_json(w->d, &d->mode, NF));
    put(w, o, "depth", pc_sample_type_to_json(w->d, &d->depth, NF));
    put(w, o, "icc_profile", opt_blob(w, d->icc_profile));
    a = arr(w);
    for (size_t i = 0; i < d->nlayers && ok(w); i++) {
        push(w, a, layer_m(w, &d->layers[i], 0));
    }
    put(w, o, "layers", a);
    a = arr(w);
    for (size_t i = 0; i < d->nchannels && ok(w); i++) {
        push(w, a, channel_m(w, &d->channels[i]));
    }
    put(w, o, "channels", a);
    put(w, o, "guides", pc_guides_to_json(w->d, &d->guides, NF));
    put(w, o, "selection", d->has_selection ? surface_m(w, &d->selection) : jnull(w));
    put(w, o, "metadata", metadata_m(w, &d->metadata));
    put(w, o, "global_light", pc_global_light_to_json(w->d, &d->global_light, NF));
    a = arr(w);
    for (size_t i = 0; i < d->npaths && ok(w); i++) {
        yyjson_mut_val *p = obj(w);
        put(w, p, "name", jstr(w, &d->paths[i].name));
        put(w, p, "path", pc_path_to_json(w->d, &d->paths[i].path, NF));
        put(w, p, "psd_raw", opt_blob(w, d->paths[i].psd_raw));
        push(w, a, p);
    }
    put(w, o, "paths", a);
    put(w, o, "work_path", OPT(d->has_work_path, pc_path_to_json(w->d, &d->work_path, NF)));
    put(w, o, "clipping_path",
        OPT(d->has_clipping_path, pc_clipping_path_to_json(w->d, &d->clipping_path, NF)));
    put(w, o, "quick_mask", d->has_quick_mask ? channel_m(w, &d->quick_mask) : jnull(w));
    a = arr(w);
    for (size_t i = 0; i < d->npatterns && ok(w); i++) {
        const pc_pattern *pt = &d->patterns[i];
        yyjson_mut_val *p = obj(w);
        put(w, p, "id", jstr(w, &pt->id));
        put(w, p, "name", jstr(w, &pt->name));
        put(w, p, "width", ju64(w, pt->width));
        put(w, p, "height", ju64(w, pt->height));
        put(w, p, "surface", surface_m(w, &pt->surface));
        push(w, a, p);
    }
    put(w, o, "patterns", a);
    put(w, o, "color_table", OPT(d->has_color_table, pc_color_table_to_json(w->d, &d->color_table, NF)));
    put(w, o, "duotone", OPT(d->has_duotone, pc_duotone_to_json(w->d, &d->duotone, NF)));
    a = arr(w);
    for (size_t i = 0; i < d->nlayer_comps && ok(w); i++) {
        push(w, a, comp_m(w, &d->layer_comps[i]));
    }
    put(w, o, "layer_comps", a);
    put(w, o, "last_applied_comp", OPT(d->has_last_applied_comp, ju64(w, d->last_applied_comp)));
    put(w, o, "last_document_state",
        d->has_last_document_state ? comp_m(w, &d->last_document_state) : jnull(w));
    put(w, o, "variables", pc_variables_to_json(w->d, &d->variables, NF));
    put(w, o, "timeline", OPT(d->has_timeline, pc_timeline_to_json(w->d, &d->timeline, NF)));
    put(w, o, "measurement", pc_measurement_to_json(w->d, &d->measurement, NF));
    a = arr(w);
    for (size_t i = 0; i < d->nnotes && ok(w); i++) {
        push(w, a, pc_note_to_json(w->d, &d->notes[i], NF));
    }
    put(w, o, "notes", a);
    put(w, o, "text_styles", pc_text_styles_to_json(w->d, &d->text_styles, NF));
    if (!pc_slices_is_empty(&d->slices)) {
        put(w, o, "slices", pc_slices_to_json(w->d, &d->slices, NF));
    }
    return ok(w) ? o : NULL;
}

pc_status pc_format_manifest_build(const pc_document *doc, const char *thumbnail, const char *composite,
                                   pc_format_sink *sink, yyjson_mut_doc *jdoc, yyjson_mut_val **out,
                                   pc_error *err)
{
    wb w = {jdoc, sink, PC_OK, err};
    yyjson_mut_val *m = obj(&w);
    put(&w, m, "format_version", ju64(&w, PC_FORMAT_VERSION));
    put(&w, m, "generator", jcstr(&w, PC_FORMAT_GENERATOR));
    put(&w, m, "document", doc_m(&w, doc));
    put(&w, m, "thumbnail", thumbnail != NULL ? jcstr(&w, thumbnail) : jnull(&w));
    put(&w, m, "composite", composite != NULL ? jcstr(&w, composite) : jnull(&w));
    if (w.st != PC_OK) {
        return w.st;
    }
    yyjson_mut_doc_set_root(jdoc, m);
    *out = m;
    return PC_OK;
}
