/* A doc-space textured overlay primitive for the GL canvas.
 *
 * Draws an RGBA8 texture positioned in DOCUMENT coordinates and transformed by the canvas camera
 * (zoom/pan/rotate/flip) exactly like the document layer, so an overlay lines up pixel-correct
 * under every view transform. It mirrors the canvas camera shader (canvas_view.c render_camera):
 * a full-screen triangle whose fragment shader maps each viewport point back to document space via
 * the same origin/axisX/axisY the camera uses, then samples the overlay texture at that document
 * point. It renders into whatever framebuffer + viewport are currently bound, blending over the
 * existing content with premultiplied alpha, so the caller composites it on top of the camera pass.
 *
 * The texture holds PREMULTIPLIED RGBA8 (row-major, w * h). This is the primitive the Liquify live
 * warped-pixel proxy preview uses; it is deliberately generic so later tools (perspective / puppet
 * live previews) can draw their own doc-space image previews through the same path.
 *
 * Never crashes: GL object creation is checked and reported as an error; all draws validate the
 * camera transform and reject non-finite / degenerate ones; nothing here allocates host memory.
 * Not thread-safe (a GL context is per-thread). Finalise before destroying the GL context. */
#ifndef PC_UI_SHELL_CANVAS_OVERLAY_H
#define PC_UI_SHELL_CANVAS_OVERLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gl33.h"
#include "base/status.h"
#include "geom/geom.h"
#include "ui/canvas.h"

typedef struct pc_ui_canvas_overlay {
    GLuint program; /* 0 until the first prepare() compiles it */
    GLuint vao;     /* empty VAO for the full-screen triangle */
    GLuint texture; /* RGBA8 overlay texture */
    int tex_w, tex_h;
} pc_ui_canvas_overlay;

/* Ensures the program, VAO and an RGBA8 texture of exactly w x h exist. *needs_full is set true
 * when the texture was (re)allocated and must be fully uploaded before the next draw; false when
 * the existing texture of the same size was kept (a partial upload suffices). w and h must be
 * >= 1 and within the GPU texture budget. */
pc_status pc_ui_canvas_overlay_prepare(pc_ui_canvas_overlay *o, const gl33 *gl, size_t w, size_t h,
                                       bool *needs_full, pc_error *err);

/* Uploads the pixels in the proxy-pixel rectangle [x0,y0,x1,y1) (clamped to the texture) from a
 * premultiplied RGBA8 image whose row stride is `src_w` pixels. No-op for an empty rectangle. */
void pc_ui_canvas_overlay_upload(const pc_ui_canvas_overlay *o, const gl33 *gl, const uint8_t (*px)[4],
                                 size_t src_w, size_t x0, size_t y0, size_t x1, size_t y1);

/* Draws the overlay into the currently bound framebuffer and viewport. `xf` is the canvas camera
 * (as in render_camera), `doc_rect` the document-space rectangle the texture maps onto, and
 * opacity in 0..1 scales the premultiplied pixels. Returns PC_ERR_INVALID on a degenerate camera
 * or empty rect (nothing drawn); the caller treats that as "no overlay this frame". */
pc_status pc_ui_canvas_overlay_draw(const pc_ui_canvas_overlay *o, const gl33 *gl,
                                    const pc_ui_canvas_xform *xf, pc_rect doc_rect, float opacity,
                                    pc_error *err);

void pc_ui_canvas_overlay_fini(pc_ui_canvas_overlay *o, const gl33 *gl); /* NULL-safe */

#endif /* PC_UI_SHELL_CANVAS_OVERLAY_H */
