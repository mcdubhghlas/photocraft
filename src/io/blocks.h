/* Tagged-block and descriptor helpers (chunk I2: crates/io/src/blocks.rs). Declared here are the
 * functions other chunks call (I3's psd_export and smart_map, I4's vector_map and comps_map);
 * the rest follow. Implemented in blocks.c and blocks_fill.c; file data that can't be read is
 * "not found" (the reference's None), only out of memory fails. Descriptor values are src/psd's
 * (psd/descriptor.h). Thread-safe. */
#ifndef PC_IO_BLOCKS_H
#define PC_IO_BLOCKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "geom/warp.h"
#include "io/core.h"
#include "psd/descriptor.h"
#include "psd/types.h"

/* brst (channel restrictions) data for an excluded-channel mask; *has false when none needed. */
pc_status pc_io_brst_data(uint32_t mask, bool *has, pc_buf *out, pc_error *err);
/* Appends the Advanced Blending switches' blocks (knko, infx, clbl, tsly, lmgm, vmgm). */
pc_status pc_io_put_advanced(const pc_advanced_blending *a, pc_io_raw_blocks *raw, pc_error *err);
/* A layer record's blending-ranges bytes for Blend If over `channels` colour channels. */
pc_status pc_io_ranges_from_blend_if(const pc_blend_if *b, size_t channels, pc_psd_bytes *out, pc_error *err);

/* A number value (doub, long, UntF, ...) as f64; false otherwise (blocks::num). */
bool pc_io_desc_num(const pc_psd_value *v, double *out);
/* The descriptor stored under key (borrowed) or NULL (blocks::get_desc). */
const pc_psd_desc *pc_io_desc_get(const pc_psd_desc *d, const char *key);
/* An enum value's bytes (borrowed, *len set) or NULL (blocks::enum_of). */
const uint8_t *pc_io_desc_enum(const pc_psd_desc *d, const char *key, size_t *len);
/* A gradient's interpolation-method enum: "gradientsInterpolationMethod" (Photoshop 27.11),
 * falling back to the legacy "gs99" (blocks::gradient_method). Borrowed; *len set or NULL. */
const uint8_t *pc_io_desc_gradient_method(const pc_psd_desc *d, size_t *len);
/* A boolean value, false when absent (blocks::bool_of). */
bool pc_io_desc_bool(const pc_psd_desc *d, const char *key);
/* A colour descriptor (RGBC, CMYC, Grsc, LbCl, HSBC, ...); false when unreadable. */
bool pc_io_color_from_desc(const pc_psd_desc *d, pc_color *out);
pc_status pc_io_color_to_desc(const pc_color *c, pc_psd_desc *out, pc_error *err);
/* A versioned descriptor at the start of data (trailing bytes allowed); *found false when it
 * doesn't parse (blocks::parse_prefix_versioned). */
pc_status pc_io_parse_prefix_versioned(const uint8_t *data, size_t len, bool *found, pc_psd_desc *out,
                                       pc_error *err);
/* Fill layers: SoCo / GdFl / PtFl block data or descriptor <-> pc_fill; *found false when
 * unreadable. fill_to_desc also returns the block key. */
pc_status pc_io_parse_fill(const uint8_t key[4], const uint8_t *data, size_t len, bool *found, pc_fill *out,
                           pc_error *err);
pc_status pc_io_fill_from_desc(const uint8_t key[4], const pc_psd_desc *d, bool *found, pc_fill *out,
                               pc_error *err);
pc_status pc_io_fill_to_desc(const pc_fill *f, uint8_t key[4], pc_psd_desc *out, pc_error *err);
/* Smart-object blocks (PlLd / SoLd / SoLE): the Idnt uuid and source -> document transform. */
pc_status pc_io_parse_smart(const uint8_t key[4], const uint8_t *data, size_t len, pc_str *idnt,
                            pc_affine *transform, pc_error *err);
/* The full projective map of a distorted smart object (row-major 3x3); *found false otherwise. */
pc_status pc_io_parse_smart_perspective(const uint8_t key[4], const uint8_t *data, size_t len, bool *found,
                                        double out[9], pc_error *err);
/* The warp of a placed layer; *found false when it has none. */
pc_status pc_io_parse_placed_warp(const uint8_t key[4], const uint8_t *data, size_t len, bool *found,
                                  pc_warp *out, pc_error *err);
/* The source-space box Photoshop maps onto a placed layer's Trnf quad: the bounding box of a
 * single-patch custom warp's 16 control points (which may spill past the source), else the source
 * rect 0..w x 0..h. Mapping the source rect instead counts the overhang twice. A degenerate or
 * non-finite box falls back to the source rect. `warp` may be NULL. out = {x0, y0, x1, y1}. */
void pc_io_placed_frame(const pc_warp *warp, double w, double h, double out[4]);

/* ---- the rest of blocks.rs (I2; also used by I3's psd_export and I2's effects_map) ---- */

/* brst data -> bit mask (bit i = channel i; ids above 31 ignored). */
uint32_t pc_io_parse_brst(const uint8_t *data, size_t len);
/* Advanced Blending from a layer's blocks (block(ctx, key, &len) gives a block's data or NULL);
 * a missing or empty block keeps Photoshop's default. */
pc_advanced_blending
pc_io_advanced_from_blocks(const uint8_t *(*block)(void *ctx, const uint8_t key[4], size_t *len), void *ctx);
/* A layer record's blending ranges (8-byte entries) -> Blend If (trailing full entries dropped). */
pc_status pc_io_blend_if_from_ranges(const uint8_t *data, size_t len, pc_blend_if *out, pc_error *err);
/* lspf bits <-> locks; lclr index <-> label. */
pc_locks pc_io_locks_from_lspf(uint32_t v);
uint32_t pc_io_lspf_from_locks(const pc_locks *l);
pc_label_color pc_io_label_from_index(uint16_t v);
uint16_t pc_io_label_index(pc_label_color l);
/* A fill as its block: *key and the versioned descriptor bytes (*out replaced). */
pc_status pc_io_write_fill(const pc_fill *f, uint8_t key[4], pc_buf *out, pc_error *err);
/* Text and transform of a TySh block (best effort); *found false when too short. */
pc_status pc_io_parse_tysh(const uint8_t *data, size_t len, bool *found, pc_str *text, pc_affine *transform,
                           pc_error *err);
/* masterFXSwitch of lfx2 data (true when absent or unreadable). */
pc_status pc_io_effects_enabled(const uint8_t *data, size_t len, bool *out, pc_error *err);

/* Descriptor helpers shared by blocks.c and effects_map.c. */
pc_gradient_style pc_io_gradient_style(const pc_psd_desc *d);
pc_status pc_io_gradient_style_value(pc_gradient_style s, pc_psd_value *out, pc_error *err);
/* Colour and opacity stops of a Grdn descriptor baked to Photoshop's interpolation (method =
 * the parent's gs99 enum, NULL when absent). Both arrays malloc'd (NULL when empty). */
pc_status pc_io_gradient_stops_with(const pc_psd_desc *grad, const uint8_t *method, size_t method_len,
                                    pc_color_stop **stops, size_t *nstops, pc_opacity_stop **opacity,
                                    size_t *nopacity, pc_error *err);
/* A Grdn descriptor for the stops (opacity empty: each colour stop's alpha); mids may be NULL. */
pc_status pc_io_gradient_desc(const pc_color_stop *stops, size_t nstops, const pc_opacity_stop *opacity,
                              size_t nopacity, const float *mids, size_t nmids, pc_psd_desc *out,
                              pc_error *err);
/* Ptrn -> name and id (lossy UTF-8; "" when absent). */
pc_status pc_io_pattern_ref(const pc_psd_desc *d, pc_str *name, pc_str *id, pc_error *err);
pc_status pc_io_pattern_ref_desc(const char *name, size_t name_len, const char *id, size_t id_len,
                                 pc_psd_desc *out, pc_error *err);
/* Angl, Algn (default on) and the phase point of a pattern fill / overlay. */
void pc_io_pattern_placement(const pc_psd_desc *d, float *angle, bool *link, float phase[2]);
/* Adds the keys pattern_placement reads (angle only when non-zero). */
pc_status pc_io_with_pattern_placement(pc_psd_desc *d, float angle, bool link, const float phase[2],
                                       pc_error *err);

#endif
