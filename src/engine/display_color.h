/* Colour-managed canvas display (display_color.rs / proof_sim.rs). Single-owner immutable
 * LUT after build; fini releases it. No document edits. The monitor resolver shares Color Settings'
 * explicit-profile validation and its reported sRGB fallback when auto detection is unavailable. */
#ifndef PC_ENGINE_DISPLAY_COLOR_H
#define PC_ENGINE_DISPLAY_COLOR_H
#include "engine.h"
#include "color_cmds.h"
#include "cms/lut3d.h"
typedef struct pc_engine_display {
    pc_lut3d lut;
    pc_hdr_preview hdr;
    bool encode_linear, gamut_warning;
    float warning[3], warning_opacity;
} pc_engine_display;
pc_status pc_engine_color_diagnostics(const pc_engine_session *session, pc_json_value *out, pc_error *err);
void pc_engine_display_fini(pc_engine_display *display);
uint64_t pc_engine_display_key(const pc_engine_session *session);
pc_status pc_engine_display_monitor(const pc_engine_session *session, pc_profile *out, pc_error *err);
pc_status pc_engine_display_build(const pc_engine_session *session, size_t size, pc_engine_display *out,
                                  pc_error *err);
/* Straight composite RGB -> monitor RGB; mirrors the GL display shader. Alpha is handled outside. */
void pc_engine_display_apply(const pc_engine_display *display, const float rgb[3], float out[3]);
void pc_engine_display_dichromat(const float linear_rgb[3], bool protan, float out[3]);
#endif
