/* Shared preset-gallery helpers (headless; no GUI): the thumbnail pixel size a picker asks the
 * engine for, bounded to the range the engine thumbnail primitives accept (8..256), so a hostile UI
 * scale or widget size can never make an out-of-range request (REWRITE_PLAN.md rule 9). Used by the
 * Styles preset gallery and the Pattern Overlay / pattern-fill picker. */
#ifndef PC_UI_PRESET_THUMB_H
#define PC_UI_PRESET_THUMB_H

#include <stdint.h>

/* Lower / upper bound on a preset thumbnail edge, matching pc_engine_*_thumbnail's own clamp. */
enum { PC_UI_PRESET_THUMB_MIN = 8, PC_UI_PRESET_THUMB_MAX = 256 };

/* round(logical_side * ui_scale) clamped to [PC_UI_PRESET_THUMB_MIN, PC_UI_PRESET_THUMB_MAX].
 * Non-finite or non-positive inputs yield PC_UI_PRESET_THUMB_MIN. */
uint32_t pc_ui_preset_thumb_px(double logical_side, double ui_scale);

#endif
