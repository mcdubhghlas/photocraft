/* manifest.json decoding (crates/format/src/manifest.rs and store::read_manifest's decode).
 *
 * The model is described with doc's serde descriptors (doc/json_util.h): structs read their
 * fields in key order (serde_json::Value maps are sorted), report missing fields afterwards in
 * declaration order, honour #[serde(default)], and the internally tagged ContentM /
 * SmartSourceM deserialise their variants from serde's Content buffer (JR_CONTENT). Doc types
 * embedded in the manifest use their own descriptors through T_jtype().
 *
 * The reference decodes the layer tree one layer at a time so its stack does not grow with the
 * nesting: it takes `/document/layers` out of the JSON before decoding the manifest, and takes
 * each layer's `/content/children` out before decoding the layer, decoding the children first.
 * yyjson's immutable tree can't be edited, so a "taken" array is marked with the M_TAKEN mode
 * bit instead: the layer-list reader then returns an empty list for it, exactly what the
 * reference's decoder sees, and the list is decoded separately in the reference's order. */
#include "format/manifest.h"

#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"
#include "doc/json_util.h"
#include "format/bridge.h"

/* Reader mode bit: the layer list this value holds was taken out (decoded separately). Doc's
 * readers only look at JR_CONTENT, so the bit passes through them unnoticed. */
#define M_TAKEN 0x100u

/* ---- doc types, through their descriptors ---- */

#define FWD(T)                                                                                               \
    static pc_status fwd_rd_##T(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)      \
    {                                                                                                        \
        const jtype *r = T##_jtype();                                                                        \
        (void)t;                                                                                             \
        return r->read(r, v, out, mode, err);                                                                \
    }                                                                                                        \
    static yyjson_mut_val *fwd_wr_##T(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)      \
    {                                                                                                        \
        const jtype *r = T##_jtype();                                                                        \
        (void)t;                                                                                             \
        return r->write(r, d, v, flags);                                                                     \
    }                                                                                                        \
    static void fwd_fi_##T(const jtype *t, void *v)                                                          \
    {                                                                                                        \
        (void)t;                                                                                             \
        pc_jfini(T##_jtype(), v);                                                                            \
    }                                                                                                        \
    static const jtype jt_##T = {sizeof(T), fwd_rd_##T, fwd_wr_##T, fwd_fi_##T, NULL, 0, NULL};

FWD(pc_effect)
FWD(pc_locks)
FWD(pc_label_color)
FWD(pc_vector_mask)
FWD(pc_blend_if)
FWD(pc_advanced_blending)
FWD(pc_artboard)
FWD(pc_adjustment)
FWD(pc_text_run)
FWD(pc_paragraph_run)
FWD(pc_text_shape)
FWD(pc_orientation)
FWD(pc_antialias)
FWD(pc_text_warp)
FWD(pc_shape_stroke)
FWD(pc_live_shape)
FWD(pc_smart_filter)
FWD(pc_stack_mode)
FWD(pc_video_source)
FWD(pc_guides)
FWD(pc_global_light)
FWD(pc_clipping_path)
FWD(pc_color_table)
FWD(pc_duotone)
FWD(pc_variables)
FWD(pc_timeline)
FWD(pc_measurement)
FWD(pc_note)
FWD(pc_text_styles)
FWD(pc_slices)
FWD(pc_color_indicates)

/* ---- geom::warp::Warp (camelCase) ---- */

static const jvariant warp_style_v[] = {
    JVU("none"),  JVU("custom"),     JVU("arc"),        JVU("arcLower"), JVU("arcUpper"), JVU("arch"),
    JVU("bulge"), JVU("shellLower"), JVU("shellUpper"), JVU("flag"),     JVU("wave"),     JVU("fish"),
    JVU("rise"),  JVU("fisheye"),    JVU("inflate"),    JVU("squeeze"),  JVU("twist"),
};
static const jenum warp_style_e = JENUM_UNITS(pc_warp_style, warp_style_v);
static const jtype jt_warp_style = JT_UNIT_ENUM(pc_warp_style, warp_style_e);

static const jtype jt_f64x2 = JT_ARRAY(double, pc_jt_f64, 2);
static const jtype jt_f64x4 = JT_ARRAY(double, pc_jt_f64, 4);
static const jtype jt_f64x9 = JT_ARRAY(double, pc_jt_f64, 9);

static const jfield mesh_f[] = {
    JFVEC("us", pc_mesh, us, nus, pc_jt_f64),
    JFVEC("vs", pc_mesh, vs, nvs, pc_jt_f64),
    /* Vec<[f64; 2]> into pc_point (two doubles, the same layout) */
    JFVEC("points", pc_mesh, points, npoints, jt_f64x2),
};
static const jstruct mesh_s = {"struct BezierMesh", mesh_f, JCOUNT(mesh_f), NULL, sizeof(pc_mesh)};
static const jtype jt_mesh = JT_STRUCT(pc_mesh, mesh_s);

static const jfield warp_f[] = {
    JFV("style", pc_warp, style, jt_warp_style),
    JFVD("bend", pc_warp, bend, pc_jt_f64, JD_ZERO),
    JFVD("hDistort", pc_warp, h_distort, pc_jt_f64, JD_ZERO),
    JFVD("vDistort", pc_warp, v_distort, pc_jt_f64, JD_ZERO),
    JFVD("vertical", pc_warp, vertical, pc_jt_bool, JD_ZERO),
    JFV("bounds", pc_warp, bounds, jt_f64x4),
    JFO("mesh", pc_warp, mesh, has_mesh, jt_mesh, JD_ZERO),
};
static const jstruct warp_s = {"struct Warp", warp_f, JCOUNT(warp_f), NULL, sizeof(pc_warp)};
static const jtype jt_warp = JT_STRUCT(pc_warp, warp_s);

/* ---- surfaces, masks, effects ---- */

static const jfield tile_f[] = {
    JFV("tx", pc_mf_tile, tx, pc_jt_i32),
    JFV("ty", pc_mf_tile, ty, pc_jt_i32),
    JFV("hash", pc_mf_tile, hash, pc_jt_str),
};
static const jstruct tile_s = {"struct TileRef", tile_f, JCOUNT(tile_f), NULL, sizeof(pc_mf_tile)};
static const jtype jt_tile = JT_STRUCT(pc_mf_tile, tile_s);

static const jfield surface_f[] = {
    JFV("format", pc_mf_surface, format, pc_jt_pixel_format),
    JFV("default", pc_mf_surface, def, pc_jt_str),
    JFVEC("tiles", pc_mf_surface, tiles, ntiles, jt_tile),
};
static const jstruct surface_s = {"struct SurfaceM", surface_f, JCOUNT(surface_f), NULL,
                                  sizeof(pc_mf_surface)};
static const jtype jt_surface = JT_STRUCT(pc_mf_surface, surface_s);

static const jfield mask_f[] = {
    JFV("surface", pc_mf_mask, surface, jt_surface), JFV("enabled", pc_mf_mask, enabled, pc_jt_bool),
    JFV("linked", pc_mf_mask, linked, pc_jt_bool),   JFV("density", pc_mf_mask, density, pc_jt_f32),
    JFV("feather", pc_mf_mask, feather, pc_jt_f32),
};
static const jstruct mask_s = {"struct MaskM", mask_f, JCOUNT(mask_f), NULL, sizeof(pc_mf_mask)};
static const jtype jt_mask = JT_STRUCT(pc_mf_mask, mask_s);

static const jtuple_el f64x2t_el[] = {{&pc_jt_f64, 0}, {&pc_jt_f64, sizeof(double)}};
static const jtuple f64x2t_tu = {f64x2t_el, 2};
static const jtype jt_f64x2t = {
    2 * sizeof(double), pc_jt_tuple_read, pc_jt_tuple_write, pc_jt_tuple_fini, NULL, 0, &f64x2t_tu};

static const jfield effects_f[] = {
    JFV("enabled", pc_mf_effects, enabled, pc_jt_bool),
    JFVEC("items", pc_mf_effects, items, nitems, jt_pc_effect),
    JFO("psd_raw", pc_mf_effects, psd_raw, has_psd_raw, pc_jt_str, JD_REQ),
    JFO("reference", pc_mf_effects, reference, has_reference, jt_f64x2t, JD_ZERO),
};
static const jstruct effects_s = {"struct EffectsM", effects_f, JCOUNT(effects_f), NULL,
                                  sizeof(pc_mf_effects)};
static const jtype jt_effects = JT_STRUCT(pc_mf_effects, effects_s);

static const jfield fill_cache_f[] = {
    JFV("fill", pc_mf_fill_cache, fill, pc_jt_fill),
    JFV("surface", pc_mf_fill_cache, surface, jt_surface),
};
static const jstruct fill_cache_s = {"struct FillCacheM", fill_cache_f, JCOUNT(fill_cache_f), NULL,
                                     sizeof(pc_mf_fill_cache)};
static const jtype jt_fill_cache = JT_STRUCT(pc_mf_fill_cache, fill_cache_s);

static const jfield video_f[] = {
    JFVEC("frames", pc_mf_video, frames, nframes, jt_surface),
    JFV("source", pc_mf_video, source, jt_pc_video_source),
    JFV("fps", pc_mf_video, fps, pc_jt_f32),
    JFVD("show_altered", pc_mf_video, show_altered, pc_jt_bool, JD_ZERO),
};
static const jstruct video_s = {"struct VideoDataM", video_f, JCOUNT(video_f), NULL, sizeof(pc_mf_video)};
static const jtype jt_video = JT_STRUCT(pc_mf_video, video_s);

static const jtuple_el psd_block_el[] = {
    {&pc_jt_str, offsetof(pc_mf_psd_block, key)},
    {&pc_jt_str, offsetof(pc_mf_psd_block, hash)},
};
static const jtuple psd_block_tu = {psd_block_el, 2};
static const jtype jt_psd_block = JT_TUPLE(pc_mf_psd_block, psd_block_tu);

/* ---- SmartSourceM (internally tagged, "kind") ---- */

static const jfield source_embedded_f[] = {
    JFV("file_name", pc_mf_smart_source, file_name, pc_jt_str),
    JFV("blob", pc_mf_smart_source, blob, pc_jt_str),
};
static const jfield source_linked_f[] = {JFV("path", pc_mf_smart_source, path, pc_jt_str)};
static const jstruct source_embedded_s = {"struct variant SmartSourceM::Embedded", source_embedded_f,
                                          JCOUNT(source_embedded_f), NULL, sizeof(pc_mf_smart_source)};
static const jstruct source_linked_s = {"struct variant SmartSourceM::Linked", source_linked_f,
                                        JCOUNT(source_linked_f), NULL, sizeof(pc_mf_smart_source)};
static const jvariant source_v[] = {JVS("embedded", source_embedded_s), JVS("linked", source_linked_s)};
static const jenum source_e = JENUM_TAGGED(pc_mf_smart_source, source_v, kind);
static const jinternal source_in = {"kind", "internally tagged enum SmartSourceM", &source_e};
static const jtype jt_source = JT_INTERNAL(pc_mf_smart_source, source_in);

/* ---- LayerM and ContentM ---- */

static const jtype jt_layer;

static pc_status layer_list_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    pc_mf_layer_list *l = out;
    void *p = NULL;
    size_t n = 0;
    pc_status st;
    (void)t;
    if ((mode & M_TAKEN) != 0 && yyjson_is_arr(v)) {
        return PC_OK; /* taken out: decoded on its own (decode_layers) */
    }
    st = pc_jvec_read(&jt_layer, v, &p, &n, mode & ~M_TAKEN, err);
    if (st == PC_OK) {
        l->v = p;
        l->n = n;
    }
    return st;
}

static yyjson_mut_val *layer_list_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    const pc_mf_layer_list *l = v;
    (void)t;
    return pc_jvec_write(&jt_layer, d, l->v, l->n, flags);
}

static void layer_list_fini(const jtype *t, void *v)
{
    pc_mf_layer_list *l = v;
    void *p = l->v;
    (void)t;
    pc_jvec_fini(&jt_layer, &p, &l->n);
    l->v = NULL;
}

static const jtype jt_layer_list = {
    sizeof(pc_mf_layer_list), layer_list_read, layer_list_write, layer_list_fini, NULL, 0, NULL};

static void antialias_default(void *slot)
{
    pc_antialias a = PC_AA_SMOOTH;
    memcpy(slot, &a, sizeof a);
}

static void text_shape_default(void *slot)
{
    pc_text_layer t;
    pc_text_layer_default(&t);
    memcpy(slot, &t.shape, sizeof t.shape);
}

#define C pc_mf_content
static const jfield content_raster_f[] = {JFV("surface", C, surface, jt_surface)};
static const jfield content_group_f[] = {
    JFV("children", C, children, jt_layer_list),
    JFV("expanded", C, expanded, pc_jt_bool),
    JFO("artboard", C, artboard, has_artboard, jt_pc_artboard, JD_ZERO),
};
static const jfield content_adjustment_f[] = {JFV("adjustment", C, adjustment, jt_pc_adjustment)};
static const jfield content_fill_f[] = {JFV("fill", C, fill, pc_jt_fill)};
static const jfield content_text_f[] = {
    JFV("text", C, text, pc_jt_str),
    JFV("font_family", C, font_family, pc_jt_str),
    JFV("size_pt", C, size_pt, pc_jt_f32),
    JFV("color", C, color, pc_jt_color),
    JFV("transform", C, transform, pc_jt_affine),
    JFO("cache", C, cache, has_cache, jt_surface, JD_REQ),
    JFO("psd_raw", C, psd_raw, has_psd_raw, pc_jt_str, JD_REQ),
    JFVECD("runs", C, runs, nruns, jt_pc_text_run, JD_ZERO),
    JFVECD("paragraphs", C, paragraphs, nparagraphs, jt_pc_paragraph_run, JD_ZERO),
    JFFN("shape", C, shape, jt_pc_text_shape, text_shape_default),
    JFVD("orientation", C, orientation, jt_pc_orientation, JD_ZERO),
    JFFN("antialias", C, antialias, jt_pc_antialias, antialias_default),
    JFO("warp", C, text_warp, has_text_warp, jt_pc_text_warp, JD_ZERO),
};
static const jfield content_shape_f[] = {
    JFO("fill", C, fill, has_fill, pc_jt_fill, JD_REQ),
    JFO("cache", C, cache, has_cache, jt_surface, JD_REQ),
    JFO("psd_raw", C, psd_raw, has_psd_raw, pc_jt_str, JD_REQ),
    JFVD("path", C, path, pc_jt_path, JD_ZERO),
    JFO("stroke", C, stroke, has_stroke, jt_pc_shape_stroke, JD_ZERO),
    JFO("live", C, live, has_live, jt_pc_live_shape, JD_ZERO),
};
static const jfield content_smart_f[] = {
    JFV("source", C, source, jt_source),
    JFV("transform", C, transform, pc_jt_affine),
    JFVEC("smart_filters", C, smart_filters, nsmart_filters, jt_pc_smart_filter),
    JFO("cache", C, cache, has_cache, jt_surface, JD_REQ),
    JFO("psd_raw", C, psd_raw, has_psd_raw, pc_jt_str, JD_REQ),
    JFVD("filters_enabled", C, filters_enabled, pc_jt_bool, JD_TRUE),
    JFO("filter_mask", C, filter_mask, has_filter_mask, jt_mask, JD_ZERO),
    JFO("warp", C, warp, has_warp, jt_warp, JD_ZERO),
    JFO("stack_mode", C, stack_mode, has_stack_mode, jt_pc_stack_mode, JD_ZERO),
    JFOSKIP("perspective", C, perspective, has_perspective, jt_f64x9, JD_ZERO),
};
#define VARIANT(name, f) {"struct variant ContentM::" name, f, JCOUNT(f), NULL, sizeof(C)}
static const jstruct content_raster_s = VARIANT("Raster", content_raster_f);
static const jstruct content_group_s = VARIANT("Group", content_group_f);
static const jstruct content_adjustment_s = VARIANT("Adjustment", content_adjustment_f);
static const jstruct content_fill_s = VARIANT("Fill", content_fill_f);
static const jstruct content_text_s = VARIANT("Text", content_text_f);
static const jstruct content_shape_s = VARIANT("Shape", content_shape_f);
static const jstruct content_smart_s = VARIANT("Smart", content_smart_f);
#undef VARIANT
/* In pc_layer_kind order, which is the reference's variant order. */
static const jvariant content_v[] = {
    JVS("raster", content_raster_s), JVS("group", content_group_s), JVS("adjustment", content_adjustment_s),
    JVS("fill", content_fill_s),     JVS("text", content_text_s),   JVS("shape", content_shape_s),
    JVS("smart", content_smart_s),
};
static const jenum content_e = JENUM_TAGGED(C, content_v, kind);
static const jinternal content_in = {"kind", "internally tagged enum ContentM", &content_e};
#undef C

/* doc's pc_jt_internal_read, except that the M_TAKEN bit reaches the variant's fields (the
 * group's `children`). Every ContentM variant is a struct variant. */
static pc_status content_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    const jinternal *in = t->aux;
    yyjson_val *tag;
    size_t start = 0;
    unsigned idx = 0;
    pc_status st;
    if (yyjson_is_obj(v)) {
        tag = pc_json_get(v, in->tag);
        if (tag == NULL) {
            return pc_jfail_missing(in->tag, err);
        }
    } else if (yyjson_is_arr(v)) {
        tag = yyjson_arr_get_first(v);
        if (tag == NULL) {
            return pc_jfail_missing(in->tag, err);
        }
        start = 1;
    } else {
        return pc_jfail_type(v, in->expecting, err);
    }
    st = pc_jinternal_tag(in->e, tag, mode, &idx, err);
    if (st != PC_OK) {
        return st;
    }
    /* An object still holds the tag entry; the struct ignores it like any unknown key. */
    st = pc_jstruct_read(in->e->v[idx].st, v, out, JR_CONTENT | (mode & M_TAKEN), start, err);
    if (st == PC_OK) {
        pc_jenum_set(in->e, out, idx);
    }
    return st;
}

static const jtype jt_content = {
    sizeof(pc_mf_content), content_read, pc_jt_internal_write, pc_jt_internal_fini, NULL, 0, &content_in};

static void advanced_default(void *slot)
{
    pc_advanced_blending a = pc_advanced_blending_default();
    memcpy(slot, &a, sizeof a);
}

#define L pc_mf_layer
static const jfield layer_f[] = {
    JFV("id", L, id, pc_jt_u64),
    JFV("name", L, name, pc_jt_str),
    JFV("visible", L, visible, pc_jt_bool),
    JFV("locks", L, locks, jt_pc_locks),
    JFV("blend", L, blend, pc_jt_blend_mode),
    JFV("opacity", L, opacity, pc_jt_f32),
    JFV("fill_opacity", L, fill_opacity, pc_jt_f32),
    JFV("clipped", L, clipped, pc_jt_bool),
    JFO("mask", L, mask, has_mask, jt_mask, JD_REQ),
    JFO("vector_mask", L, vector_mask, has_vector_mask, jt_pc_vector_mask, JD_ZERO),
    JFV("effects", L, effects, jt_effects),
    JFV("label", L, label, jt_pc_label_color),
    JFV("content", L, content, jt_content),
    JFVEC("psd_blocks", L, psd_blocks, npsd_blocks, jt_psd_block),
    JFO("psd_id", L, psd_id, has_psd_id, pc_jt_u32, JD_REQ),
    JFO("fill_cache", L, fill_cache, has_fill_cache, jt_fill_cache, JD_REQ),
    JFO("link_group", L, link_group, has_link_group, pc_jt_u64, JD_ZERO),
    JFVD("excluded_channels", L, excluded_channels, pc_jt_u32, JD_ZERO),
    JFVD("blend_if", L, blend_if, jt_pc_blend_if, JD_ZERO),
    JFFN("advanced", L, advanced, jt_pc_advanced_blending, advanced_default),
    JFO("video", L, video, has_video, jt_video, JD_ZERO),
};
#undef L
static const jstruct layer_s = {"struct LayerM", layer_f, JCOUNT(layer_f), NULL, sizeof(pc_mf_layer)};
static const jtype jt_layer = JT_STRUCT(pc_mf_layer, layer_s);

/* ---- channels, patterns, paths, comps, metadata ---- */

static const jtuple_el spot_el[] = {
    {&pc_jt_color, offsetof(pc_mf_spot, color)},
    {&pc_jt_f32, offsetof(pc_mf_spot, solidity)},
};
static const jtuple spot_tu = {spot_el, 2};
static const jtype jt_spot = JT_TUPLE(pc_mf_spot, spot_tu);

static void channel_color_default(void *slot)
{
    pc_color c = PC_ALPHA_CHANNEL_DEFAULT_COLOR;
    memcpy(slot, &c, sizeof c);
}

static void channel_opacity_default(void *slot)
{
    float o = 0.5f;
    memcpy(slot, &o, sizeof o);
}

static const jfield channel_f[] = {
    JFV("name", pc_mf_channel, name, pc_jt_str),
    JFV("surface", pc_mf_channel, surface, jt_surface),
    JFO("spot", pc_mf_channel, spot, has_spot, jt_spot, JD_REQ),
    JFFN("color", pc_mf_channel, color, pc_jt_color, channel_color_default),
    JFFN("opacity", pc_mf_channel, opacity, pc_jt_f32, channel_opacity_default),
    JFVD("indicates", pc_mf_channel, indicates, jt_pc_color_indicates, JD_ZERO),
};
static const jstruct channel_s = {"struct ChannelM", channel_f, JCOUNT(channel_f), NULL,
                                  sizeof(pc_mf_channel)};
static const jtype jt_channel = JT_STRUCT(pc_mf_channel, channel_s);

static const jfield pattern_f[] = {
    JFV("id", pc_mf_pattern, id, pc_jt_str),
    JFV("name", pc_mf_pattern, name, pc_jt_str),
    JFV("width", pc_mf_pattern, width, pc_jt_u32),
    JFV("height", pc_mf_pattern, height, pc_jt_u32),
    JFV("surface", pc_mf_pattern, surface, jt_surface),
};
static const jstruct pattern_s = {"struct PatternM", pattern_f, JCOUNT(pattern_f), NULL,
                                  sizeof(pc_mf_pattern)};
static const jtype jt_pattern = JT_STRUCT(pc_mf_pattern, pattern_s);

static const jfield named_path_f[] = {
    JFV("name", pc_mf_named_path, name, pc_jt_str),
    JFV("path", pc_mf_named_path, path, pc_jt_path),
    JFO("psd_raw", pc_mf_named_path, psd_raw, has_psd_raw, pc_jt_str, JD_ZERO),
};
static const jstruct named_path_s = {"struct NamedPathM", named_path_f, JCOUNT(named_path_f), NULL,
                                     sizeof(pc_mf_named_path)};
static const jtype jt_named_path = JT_STRUCT(pc_mf_named_path, named_path_s);

static const jfield appearance_f[] = {
    JFV("blend", pc_mf_appearance, blend, pc_jt_blend_mode),
    JFV("opacity", pc_mf_appearance, opacity, pc_jt_f32),
    JFV("fill_opacity", pc_mf_appearance, fill_opacity, pc_jt_f32),
    JFV("effects", pc_mf_appearance, effects, jt_effects),
};
static const jstruct appearance_s = {"struct CompAppearanceM", appearance_f, JCOUNT(appearance_f), NULL,
                                     sizeof(pc_mf_appearance)};
static const jtype jt_appearance = JT_STRUCT(pc_mf_appearance, appearance_s);

static const jtuple_el i32x2_el[] = {{&pc_jt_i32, 0}, {&pc_jt_i32, sizeof(int32_t)}};
static const jtuple i32x2_tu = {i32x2_el, 2};
static const jtype jt_i32x2t = {
    2 * sizeof(int32_t), pc_jt_tuple_read, pc_jt_tuple_write, pc_jt_tuple_fini, NULL, 0, &i32x2_tu};

static const jfield comp_state_f[] = {
    JFV("layer", pc_mf_comp_state, layer, pc_jt_u64),
    JFO("visible", pc_mf_comp_state, visible, has_visible, pc_jt_bool, JD_ZERO),
    JFO("position", pc_mf_comp_state, position, has_position, jt_i32x2t, JD_ZERO),
    JFO("appearance", pc_mf_comp_state, appearance, has_appearance, jt_appearance, JD_ZERO),
};
static const jstruct comp_state_s = {"struct CompStateM", comp_state_f, JCOUNT(comp_state_f), NULL,
                                     sizeof(pc_mf_comp_state)};
static const jtype jt_comp_state = JT_STRUCT(pc_mf_comp_state, comp_state_s);

static const jfield comp_f[] = {
    JFV("id", pc_mf_comp, id, pc_jt_u32),
    JFV("name", pc_mf_comp, name, pc_jt_str),
    JFVD("comment", pc_mf_comp, comment, pc_jt_str, JD_ZERO),
    JFV("apply_visibility", pc_mf_comp, apply_visibility, pc_jt_bool),
    JFV("apply_position", pc_mf_comp, apply_position, pc_jt_bool),
    JFV("apply_appearance", pc_mf_comp, apply_appearance, pc_jt_bool),
    JFVECD("states", pc_mf_comp, states, nstates, jt_comp_state, JD_ZERO),
};
static const jstruct comp_s = {"struct LayerCompM", comp_f, JCOUNT(comp_f), NULL, sizeof(pc_mf_comp)};
static const jtype jt_comp = JT_STRUCT(pc_mf_comp, comp_s);

static const jtuple_el resource_el[] = {
    {&pc_jt_u16, offsetof(pc_mf_resource, id)},
    {&pc_jt_str, offsetof(pc_mf_resource, name)},
    {&pc_jt_str, offsetof(pc_mf_resource, hash)},
};
static const jtuple resource_tu = {resource_el, 3};
static const jtype jt_resource = JT_TUPLE(pc_mf_resource, resource_tu);

static const jtuple_el global_block_el[] = {
    {&pc_jt_str, offsetof(pc_mf_global_block, signature)},
    {&pc_jt_str, offsetof(pc_mf_global_block, key)},
    {&pc_jt_str, offsetof(pc_mf_global_block, hash)},
};
static const jtuple global_block_tu = {global_block_el, 3};
static const jtype jt_global_block = JT_TUPLE(pc_mf_global_block, global_block_tu);

static const jfield metadata_f[] = {
    JFO("xmp", pc_mf_metadata, xmp, has_xmp, pc_jt_str, JD_REQ),
    JFO("exif", pc_mf_metadata, exif, has_exif, pc_jt_str, JD_REQ),
    JFVEC("psd_resources", pc_mf_metadata, psd_resources, npsd_resources, jt_resource),
    JFVEC("psd_global_blocks", pc_mf_metadata, psd_global_blocks, npsd_global_blocks, jt_global_block),
};
static const jstruct metadata_s = {"struct MetadataM", metadata_f, JCOUNT(metadata_f), NULL,
                                   sizeof(pc_mf_metadata)};
static const jtype jt_metadata = JT_STRUCT(pc_mf_metadata, metadata_s);

/* ---- DocM and Manifest ---- */

/* The defaults of DocM's #[serde(default)] fields that are not all-zero (Measurement and
 * TextStyles allocate), applied through the container-default path (JD_CONT). */
static pc_status doc_defaults(void *out, pc_error *err)
{
    pc_mf_doc *d = out;
    pc_status st = pc_measurement_default(&d->measurement, err);
    if (st == PC_OK) {
        st = pc_text_styles_default(&d->text_styles, err);
    }
    return st;
}

#define D pc_mf_doc
static const jfield doc_f[] = {
    JFV("id", D, id, pc_jt_u64),
    JFV("name", D, name, pc_jt_str),
    JFV("size", D, size, pc_jt_size),
    JFV("resolution_dpi", D, resolution_dpi, pc_jt_f32),
    JFV("mode", D, mode, pc_jt_color_mode),
    JFV("depth", D, depth, pc_jt_sample_type),
    JFO("icc_profile", D, icc_profile, has_icc_profile, pc_jt_str, JD_REQ),
    JFV("layers", D, layers, jt_layer_list),
    JFVEC("channels", D, channels, nchannels, jt_channel),
    JFV("guides", D, guides, jt_pc_guides),
    JFO("selection", D, selection, has_selection, jt_surface, JD_REQ),
    JFV("metadata", D, metadata, jt_metadata),
    JFV("global_light", D, global_light, jt_pc_global_light),
    JFVECD("paths", D, paths, npaths, jt_named_path, JD_ZERO),
    JFO("work_path", D, work_path, has_work_path, pc_jt_path, JD_ZERO),
    JFO("clipping_path", D, clipping_path, has_clipping_path, jt_pc_clipping_path, JD_ZERO),
    JFO("quick_mask", D, quick_mask, has_quick_mask, jt_channel, JD_ZERO),
    JFVECD("patterns", D, patterns, npatterns, jt_pattern, JD_ZERO),
    JFO("color_table", D, color_table, has_color_table, jt_pc_color_table, JD_ZERO),
    JFO("duotone", D, duotone, has_duotone, jt_pc_duotone, JD_ZERO),
    JFVECD("layer_comps", D, layer_comps, nlayer_comps, jt_comp, JD_ZERO),
    JFO("last_applied_comp", D, last_applied_comp, has_last_applied_comp, pc_jt_u32, JD_ZERO),
    JFO("last_document_state", D, last_document_state, has_last_document_state, jt_comp, JD_ZERO),
    JFVD("variables", D, variables, jt_pc_variables, JD_ZERO),
    JFO("timeline", D, timeline, has_timeline, jt_pc_timeline, JD_ZERO),
    JFVD("measurement", D, measurement, jt_pc_measurement, JD_CONT),
    JFVECD("notes", D, notes, nnotes, jt_pc_note, JD_ZERO),
    JFVD("text_styles", D, text_styles, jt_pc_text_styles, JD_CONT),
    JFVD("slices", D, slices, jt_pc_slices, JD_ZERO),
};
#undef D
static const jstruct doc_s = {"struct DocM", doc_f, JCOUNT(doc_f), doc_defaults, sizeof(pc_mf_doc)};
static const jtype jt_doc = JT_STRUCT(pc_mf_doc, doc_s);

#define M pc_format_manifest
static const jfield manifest_f[] = {
    JFV("format_version", M, format_version, pc_jt_u32),
    JFV("generator", M, generator, pc_jt_str),
    JFV("document", M, document, jt_doc),
    JFO("thumbnail", M, thumbnail, has_thumbnail, pc_jt_str, JD_ZERO),
    JFO("composite", M, composite, has_composite, pc_jt_str, JD_ZERO),
};
#undef M
static const jstruct manifest_s = {"struct Manifest", manifest_f, JCOUNT(manifest_f), NULL,
                                   sizeof(pc_format_manifest)};

/* ---- decoding ---- */

void pc_mf_layer_fini(pc_mf_layer *l)
{
    if (l != NULL) {
        pc_jfini(&jt_layer, l);
    }
}

void pc_format_manifest_free(pc_format_manifest *m)
{
    if (m != NULL) {
        pc_jstruct_fini(&manifest_s, m);
        free(m);
    }
}

/* `v` is an object whose `key` holds an array: that array (the reference's take_array). */
static yyjson_val *takeable(yyjson_val *v, const char *key)
{
    yyjson_val *a = yyjson_is_obj(v) ? pc_json_get(v, key) : NULL;
    return yyjson_is_arr(a) ? a : NULL;
}

static yyjson_val *children_of(yyjson_val *layer)
{
    yyjson_val *content = yyjson_is_obj(layer) ? pc_json_get(layer, "content") : NULL;
    return takeable(content, "children");
}

/* store::layers_from_json: each layer's children first, then the layer without them. The
 * recursion follows the JSON nesting, which the store bounds (pc_format_check_manifest_depth);
 * the explicit limit only protects direct callers. */
static pc_status decode_layers(yyjson_val *arr, size_t depth, pc_mf_layer_list *out, pc_error *err)
{
    size_t n = yyjson_arr_size(arr);
    size_t i = 0;
    yyjson_arr_iter it;
    yyjson_val *v;
    pc_mf_layer *layers;
    if (depth > PC_FORMAT_MAX_MANIFEST_DEPTH) {
        return pc_format_limit(err, "manifest JSON nested deeper than %d levels",
                               PC_FORMAT_MAX_MANIFEST_DEPTH);
    }
    if (n == 0) {
        out->v = NULL;
        out->n = 0;
        return PC_OK;
    }
    layers = pc_calloc_array(n, sizeof *layers);
    if (layers == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    (void)yyjson_arr_iter_init(arr, &it);
    while (i < n && (v = yyjson_arr_iter_next(&it)) != NULL) {
        yyjson_val *kids_json = children_of(v);
        pc_mf_layer_list kids = {NULL, 0};
        pc_status st = PC_OK;
        if (kids_json != NULL) {
            st = decode_layers(kids_json, depth + 1, &kids, err);
        }
        if (st == PC_OK) {
            st = jt_layer.read(&jt_layer, v, &layers[i], kids_json != NULL ? M_TAKEN : 0, err);
        }
        if (st != PC_OK) {
            layer_list_fini(&jt_layer_list, &kids);
            while (i > 0) {
                pc_mf_layer_fini(&layers[--i]);
            }
            free(layers);
            return st;
        }
        if (layers[i].content.kind == PC_LAYER_GROUP) {
            /* As the reference: the taken list replaces whatever the group decoded, so a group
             * written as a sequence (its children not taken) ends up empty. */
            layer_list_fini(&jt_layer_list, &layers[i].content.children);
            layers[i].content.children = kids;
        } else {
            layer_list_fini(&jt_layer_list, &kids);
        }
        i++;
    }
    out->v = layers;
    out->n = i;
    return PC_OK;
}

pc_status pc_format_manifest_decode(yyjson_val *root, pc_format_manifest **out, pc_error *err)
{
    yyjson_val *doc = yyjson_is_obj(root) ? pc_json_get(root, "document") : NULL;
    yyjson_val *layers = takeable(doc, "layers");
    pc_format_manifest *m = calloc(1, sizeof *m);
    pc_status st;
    if (m == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    st = pc_jstruct_read(&manifest_s, root, m, layers != NULL ? M_TAKEN : 0, 0, err);
    if (st != PC_OK) {
        free(m); /* the reader left it zeroed */
        return st;
    }
    if (layers != NULL) {
        st = decode_layers(layers, 0, &m->document.layers, err);
        if (st != PC_OK) {
            pc_format_manifest_free(m);
            return st;
        }
    }
    *out = m;
    return PC_OK;
}

uint32_t pc_format_manifest_format_version(const pc_format_manifest *m)
{
    return m->format_version;
}

const char *pc_format_manifest_thumbnail(const pc_format_manifest *m)
{
    return m->has_thumbnail ? pc_str_c(&m->thumbnail) : NULL;
}

const char *pc_format_manifest_composite(const pc_format_manifest *m)
{
    return m->has_composite ? pc_str_c(&m->composite) : NULL;
}
