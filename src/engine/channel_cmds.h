/* Channels: alpha/spot channels, Save/Load Selection, Quick Mask, channel targeting and
 * visibility (the Channels panel), Split/Merge Channels, Merge Spot Channel (port of
 * crates/engine/src/channel_cmds.rs, E7).
 *
 * This header exports the channel-target PRIMITIVES the pixel chunks need (E4 Edit/fill/clear,
 * E8 adjustments, E10 painting): where a "target" param writes, whether it is a channel target,
 * and the Channels panel's current target. It also exports the Channels-panel view-state
 * accessors (the ChannelView type is private to channel_cmds.c) for the selection tools.
 *
 * The view state lives on DocState.channel_view (NULL = Photoshop's default). The layer-mask view
 * lives on the DocState mask-view fields (E5, mask_view_cmds), so the C ChannelView has no
 * layer_mask member. */
#ifndef PC_ENGINE_CHANNEL_CMDS_H
#define PC_ENGINE_CHANNEL_CMDS_H

#include "engine_internal.h"

/* The Channels-panel registry (channel.*, select.saveSelection/loadSelection, quick mask). */
const pc_engine_command *pc_engine_channel_specs(size_t *n);

/* ---- channel-target primitives (pixel commands: E4/E8/E10) -------------------------------- */

/* Where a pixel command writes, from its "target" param (channel_cmds::Target). */
typedef enum pc_engine_pixel_target {
    PC_ENGINE_PIXTGT_PIXELS,     /* the active layer's pixels (default) */
    PC_ENGINE_PIXTGT_MASK,       /* the active layer's layer mask */
    PC_ENGINE_PIXTGT_ALPHA,      /* an alpha / spot channel (index in *alpha_index) */
    PC_ENGINE_PIXTGT_QUICK_MASK  /* the Quick Mask channel */
} pc_engine_pixel_target;

/* Read the "target" param. For PC_ENGINE_PIXTGT_ALPHA, *alpha_index (may be NULL) gets the index. */
pc_engine_pixel_target pc_engine_target_of(yyjson_val *p, size_t *alpha_index);
/* The target is an alpha channel or the Quick Mask (no layer involved). */
bool pc_engine_is_channel_target(yyjson_val *p);
/* The surface a pixel command reads (channel_cmds::read_target_surface). Borrowed; valid until
 * `doc` is mutated. has_layer/layer give the active layer for the Pixels/Mask targets. */
pc_status pc_engine_read_target_surface(const pc_document *doc, bool has_layer, pc_layer_id layer,
                                        yyjson_val *p, const pc_surface **out, pc_error *err);

/* The grayscale surface filters, adjustments and plug-ins edit instead of the layer's pixels when
 * the params target a channel / Quick Mask / the active layer's mask (channel_cmds::
 * channel_surface_for_filter). *out is set to the MUTABLE target surface, made explicit over the
 * canvas (so the caller sees the whole area), or to NULL when the target is the layer's pixels (so
 * the caller falls back to its own pixel/smart-object path). Borrowed; valid until `doc` is
 * mutated. has_layer/layer give the active layer for the Mask target. */
pc_status pc_engine_channel_surface_for_filter(pc_document *doc, bool has_layer, pc_layer_id layer,
                                               yyjson_val *p, pc_surface **out, pc_error *err);

/* The colour channel index the Channels panel targets (ChannelTarget::Color), or SIZE_MAX. Clear
 * (Delete) is limited to that channel (channel_cmds::targeted_color). */
size_t pc_engine_targeted_color(const pc_engine_session *s);
/* An alpha / Quick Mask channel targeted by the Channels panel (channel_cmds::targeted_channel):
 * true, with *is_quick set and (for an alpha) *alpha_index set; false when the composite/colour/
 * mask is targeted. Clear fills that channel instead of the layer. */
bool pc_engine_targeted_channel(const pc_engine_session *s, bool *is_quick, size_t *alpha_index);
/* The active document targets an alpha channel or is in Quick Mask mode, so pixel commands work
 * without a pixel layer (channel_cmds::edits_channel). */
bool pc_engine_edits_channel(const pc_engine_session *s);

/* ---- Channels-panel view-state accessors (for selection tools) ---------------------------- */

/* Colour (non-alpha) channels of the document's edit mode (channel_cmds::color_count). */
size_t pc_engine_color_count(const pc_document *doc);
/* How many of the `colors` colour channels are visible (ChannelView::visible_colors). */
size_t pc_engine_channel_visible_colors(const pc_engine_doc_state *d, size_t colors);
/* Whether alpha channel i has its eye on (ChannelView::alpha_shown). */
bool pc_engine_channel_alpha_shown(const pc_engine_doc_state *d, size_t i);
/* Whether the Quick Mask eye is hidden (ChannelView::quick_mask_hidden). */
bool pc_engine_channel_quick_mask_hidden(const pc_engine_doc_state *d);

#endif /* PC_ENGINE_CHANNEL_CMDS_H */
