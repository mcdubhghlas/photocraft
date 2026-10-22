/* On-screen colour picking (eyedropper from the desktop) for the desktop app.
 *
 * Behavioural port of the Linux-relevant, pure parts of apps/photocraft/src/screen_color.rs: the
 * pixel-budget guard, the monitor-geometry match, the 16-bit->8-bit channel conversion and the
 * RGB-only ICC validation. These are tested headlessly on every platform.
 *
 * The actual pixel grab touches the display server:
 *   - X11: reading screen pixels needs libxcb/libX11 (XGetImage / RandR). SDL3 has no screen-grab
 *     API, so this is a new link dependency the build does not yet carry; pc_screen_pick therefore
 *     reports PC_ERR_UNSUPPORTED with an actionable message until that dependency is added
 *     (REWRITE_PLAN P2).
 *   - Wayland: capture goes through the xdg-desktop-portal PickColor permission dialog, which is
 *     DEFERRED for the first pass (decision D19). pc_screen_pick reports it as unavailable.
 *   - macOS ColorSync/CoreGraphics capture is deferred (decision D2) and not ported here.
 *
 * Nothing crashes: every entry validates its input and every unavailable path returns an error. */
#ifndef PC_APP_SCREEN_COLOR_H
#define PC_APP_SCREEN_COLOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"

/* The screen-capture memory budget: at most 64 megapixels across all monitors. */
#define PC_SCREEN_MAX_PIXELS (64u * 1024u * 1024u)

/* w*h pixels, written to *out_count: PC_OK when it is non-zero and within the budget, else
 * PC_ERR_LIMIT (overflow or over budget). */
pc_status pc_screen_pixel_budget(size_t w, size_t h, size_t *out_count, pc_error *err);

/* One monitor's placement in the virtual desktop (physical pixels). */
typedef struct pc_screen_placement {
    size_t index;
    int32_t x, y;
    uint32_t width, height;
} pc_screen_placement;

/* The index of the placement whose geometry exactly equals (x, y, w, h), via *out_index; false
 * when none matches (handles negative and HiDPI monitor origins). */
bool pc_screen_find_monitor(const pc_screen_placement *placements, size_t n, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, size_t *out_index);

/* A 16-bit colour channel (as X11's PixelLayout::decode yields) rounded to 8 bits: (v + 128) / 257
 * maps 0 -> 0 and 65535 -> 255. */
uint8_t pc_screen_chan8(uint16_t v);

/* Pack `pixels` RGB triplets of 16-bit channels (rgb16: r,g,b,r,g,b,...) into RGBA8 with opaque
 * alpha, mirroring the X11 capture inner loop. PC_ERR_LIMIT when pixels exceeds the budget;
 * PC_ERR_INVALID on a NULL buffer. out_rgba must hold pixels*4 bytes. */
pc_status pc_screen_pack_rgba(const uint16_t *rgb16, size_t pixels, uint8_t *out_rgba, pc_error *err);

/* Validate ICC bytes intended for a captured screen: non-empty, at most 1 MiB, parseable and an
 * RGB colour space. PC_OK when usable, else an error (the caller then reports no profile). */
pc_status pc_screen_icc_validate(const uint8_t *bytes, size_t len, pc_error *err);

/* Probe whether screen colour picking is available in this session. `wayland` selects the session:
 * Wayland is deferred (D19), so it always reports unavailable. On an X11 session built with libxcb
 * this connects and checks for a usable TrueColor visual, returning PC_OK when the eyedropper can
 * run, or PC_ERR_UNSUPPORTED (no DISPLAY, no TrueColor visual) otherwise. A build without libxcb
 * reports unavailable, naming the missing library. Never crashes. */
pc_status pc_screen_pick(bool wayland, pc_error *err);

/* Grab the on-screen colour at the global X11 pixel coordinate (x, y), as an opaque RGBA8 pixel in
 * out_rgba[4] (required). `wayland` selects the session (Wayland is deferred, D19). When
 * placements/n_placements describe the monitors, *out_monitor (optional) receives the index of the
 * placement whose geometry equals the monitor containing the point (SIZE_MAX when unknown or no
 * match). *out_profile (optional) receives the containing monitor's display ICC bytes when one is
 * published (per-output _ICC_PROFILE, else the root _ICC_PROFILE); the caller pc_buf_fini's it, and
 * it is left empty otherwise. Any failure (no DISPLAY, connection/request error, off-screen point,
 * non-TrueColor visual, or a build without libxcb) returns an error; never crashes. */
pc_status pc_screen_pick_at(bool wayland, int32_t x, int32_t y,
                            const pc_screen_placement *placements, size_t n_placements,
                            uint8_t out_rgba[4], size_t *out_monitor, pc_buf *out_profile,
                            pc_error *err);

#endif /* PC_APP_SCREEN_COLOR_H */
