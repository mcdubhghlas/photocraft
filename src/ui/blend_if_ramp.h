/* Blend If ramp core (headless; no GUI). Port of the handle logic in
 * crates/ui-egui/src/layer_style.rs blend_if_bar. A Blend If range is four levels
 * q = {blackLo, blackHi, whiteLo, whiteHi}; each pair splits in two under Alt-drag. */
#ifndef PC_UI_BLEND_IF_RAMP_H
#define PC_UI_BLEND_IF_RAMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Which handle (0 blackLo, 1 blackHi, 2 whiteLo, 3 whiteHi) a press at value `x` (0..255) grabs:
 * the nearest, resolving a coinciding pair by the side of `x`. */
size_t pc_ui_blend_if_nearest(const uint8_t q[4], double x);

/* Drag handle `h` to `target` (clamped 0..255). When !split the handle's own pair (blackLo/blackHi
 * or whiteLo/whiteHi) moves together keeping its spread; when split only `h` moves. Order is then
 * restored so blackLo <= blackHi <= whiteLo <= whiteHi. Returns whether q changed. NULL/out-of-range
 * h is a no-op returning false. */
bool pc_ui_blend_if_drag(uint8_t q[4], size_t h, int target, bool split);

#endif
