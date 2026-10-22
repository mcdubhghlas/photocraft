/* Camera Raw scope + preview-navigation model (port of crates/ui-egui/src/camera_raw_scope_ui.rs
 * and camera_raw_preview_ui.rs). Pure geometry and decision logic: no SDL / GL / Nuklear, no
 * engine or CMS calls, so every function here is headless-testable. The shell
 * (src/ui/shell/camera_raw_ui.c) maps these results onto Nuklear draw calls and input.
 *
 * View state lives in pc_ui_camera_raw_scope / pc_ui_camera_raw_preview (src/ui/state.h); these
 * helpers read and mutate it. Never crashes: hostile coordinates, NaN/inf, bad indices and absurd
 * sizes are validated or clamped, never trusted. */
#ifndef PC_UI_CAMERA_RAW_SCOPE_H
#define PC_UI_CAMERA_RAW_SCOPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/histogram.h"
#include "ui/state.h"
#include "ui/theme.h"

/* At most this many colour samplers (probes) on the preview. */
#define PC_UI_CR_MAX_SAMPLERS 9
/* The vectorscope analyses at most this many proxy pixels. */
#define PC_UI_CR_SCOPE_SAMPLES 8192

/* ---- Tone zones (histogram drag targets, shared with the Light sliders) ---- */
typedef enum pc_ui_cr_tone_zone {
    PC_UI_CR_ZONE_BLACKS,
    PC_UI_CR_ZONE_SHADOWS,
    PC_UI_CR_ZONE_EXPOSURE,
    PC_UI_CR_ZONE_HIGHLIGHTS,
    PC_UI_CR_ZONE_WHITES,
} pc_ui_cr_tone_zone;

/* The zone under normalised histogram x in 0..1 (non-finite x clamps to Blacks). */
pc_ui_cr_tone_zone pc_ui_cr_tone_zone_at(float x);
/* English source label (feed through i18n at the call site). */
const char *pc_ui_cr_tone_zone_name(pc_ui_cr_tone_zone zone);
/* The camera-raw JSON field key this zone edits ("blacks", ... "whites"). */
const char *pc_ui_cr_tone_zone_key(pc_ui_cr_tone_zone zone);
/* [lo, hi] normalised x-range this zone covers on the histogram. */
void pc_ui_cr_tone_zone_range(pc_ui_cr_tone_zone zone, float out[2]);
/* The symmetric value limit (Exposure 5, everything else 100). */
float pc_ui_cr_tone_zone_limit(pc_ui_cr_tone_zone zone);
/* value clamped to [-limit, limit]; non-finite becomes 0. */
float pc_ui_cr_tone_zone_clamp(pc_ui_cr_tone_zone zone, float value);

/* ---- Clipping ---- */
typedef enum pc_ui_cr_clip_mode {
    PC_UI_CR_CLIP_NONE,
    PC_UI_CR_CLIP_WARNINGS,          /* mask bit0 = shadows on, bit1 = highlights on */
    PC_UI_CR_CLIP_SHADOW_CHANNELS,   /* Alt diagnostic over Blacks/Shadows */
    PC_UI_CR_CLIP_HIGHLIGHT_CHANNELS,/* Alt diagnostic over Highlights/Whites */
} pc_ui_cr_clip_mode;

/* Per-channel clip bit masks of the histogram endpoints (bit0=R,1=G,2=B). A channel is "clipped"
 * when its exact endpoint count is non-zero. */
uint8_t pc_ui_cr_shadow_mask(const pc_algo_rgb_histogram *h);
uint8_t pc_ui_cr_highlight_mask(const pc_algo_rgb_histogram *h);

/* The overlay colour for one straight RGBA pixel under `mode` (warn = the Warnings mask). Returns
 * false (and leaves *out untouched) when the pixel contributes no overlay (transparent / clear). */
bool pc_ui_cr_clip_color(const float p[4], pc_ui_cr_clip_mode mode, uint8_t warn, const pc_ui_tokens *t,
                         pc_ui_color *out);

/* ---- Vectorscope geometry ---- */
/* Thin RGB scope colour for hue h (turns, 0..1) and saturation s (0..1), scaled by the theme
 * histogram level. */
pc_ui_color pc_ui_cr_scope_hue(const pc_ui_tokens *t, float h, float s);
/* Screen point for (hue, saturation) inside rect {x,y,w,h}; red_right puts R at 3 o'clock, else
 * the default -100 degree offset. out is always finite. */
void pc_ui_cr_scope_point(const float rect[4], bool red_right, float hue, float saturation,
                          float out[2]);
/* The scope plot radius for rect {x,y,w,h}. */
float pc_ui_cr_scope_radius(const float rect[4]);

/* ---- Readouts ---- */
/* 0..1 straight RGB -> 0..255 readout values. */
void pc_ui_cr_readout_rgb(const float rgb[3], double out[3]);
/* An evaluated Lab triple (L,a,b each in the transform's 0..1 encoding) -> display L/a/b. */
void pc_ui_cr_readout_lab(const float lab[3], double out[3]);
/* Formats one readout as "R n G n B n" or "L n a n b n" into out (NUL-terminated, truncated to n). */
void pc_ui_cr_readout_text(const double values[3], bool lab, char *out, size_t n);

/* ---- Colour samplers ---- */
/* A normalised sample point is valid when both coordinates are finite and within 0..1. */
bool pc_ui_cr_point_valid(const float p[2]);
/* Clamp the sampler list to the cap and drop invalid points; clear an invalid floating rect. Call
 * once per frame before reading the view. */
void pc_ui_cr_scope_sanitize(pc_ui_camera_raw_scope *view);
/* Append a sampler (validated, capped). Returns false if invalid or full. */
bool pc_ui_cr_scope_add_sampler(pc_ui_camera_raw_scope *view, const float p[2]);
/* Remove sampler i. Returns false if out of range. */
bool pc_ui_cr_scope_remove_sampler(pc_ui_camera_raw_scope *view, size_t i);
/* Drop every sampler. */
void pc_ui_cr_scope_clear_samplers(pc_ui_camera_raw_scope *view);
/* Move sampler i to a new (validated) position. Returns false if out of range or invalid. */
bool pc_ui_cr_scope_move_sampler(pc_ui_camera_raw_scope *view, size_t i, const float p[2]);

/* A floating-scope rectangle [left,top,right,bottom] is valid when finite with width in 200..4096
 * and height in 150..4096. */
bool pc_ui_cr_floating_rect_valid(const float r[4]);

/* ---- Preview navigation (source-pixel coordinates; no document edits) ---- */
#define PC_UI_CR_MIN_ZOOM 0.0001f
#define PC_UI_CR_MAX_ZOOM 16.0f

/* Effective scale (image px -> screen px) for viewport {x,y,w,h} and image size {w,h}. Fits when
 * no explicit zoom is set. */
float pc_ui_cr_preview_scale(const pc_ui_camera_raw_preview *p, const float viewport[4],
                             const float size[2]);
/* The on-screen image rectangle {x,y,w,h}; also clamps the stored centre so the view stays put.
 * Always finite. */
void pc_ui_cr_preview_image_rect(pc_ui_camera_raw_preview *p, const float viewport[4],
                                 const float size[2], float out[4]);
/* Zoom to `zoom`, keeping the image point under `at` (screen coords) fixed. No-op for non-finite
 * inputs. */
void pc_ui_cr_preview_zoom_at(pc_ui_camera_raw_preview *p, float zoom, const float at[2],
                              const float viewport[4], const float size[2]);
/* Pan by a screen-space delta. No-op for non-finite deltas. */
void pc_ui_cr_preview_pan(pc_ui_camera_raw_preview *p, const float delta[2], const float viewport[4],
                          const float size[2]);
/* Return to fit-in-view, centred. */
void pc_ui_cr_preview_fit(pc_ui_camera_raw_preview *p);

#endif
