/* Save for Web (Legacy) — portable numeric/preview helpers (clean-room port of the logic in
 * crates/ui-egui/src/file_ui.rs: WEB_FORMATS, variants(), advance_preview() and the download-time
 * estimate). Kept free of SDL/Nuklear so the shell dialog's maths is unit-tested headless.
 * Thin; NULL-safe; never crashes on hostile input (REWRITE_PLAN.md rule 9). */
#ifndef PC_UI_WEB_EXPORT_H
#define PC_UI_WEB_EXPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The five optimisation formats, in the order shown in the Format menu. */
#define PC_UI_WEB_NFORMATS 5
extern const char *const PC_UI_WEB_FORMAT_IDS[PC_UI_WEB_NFORMATS];    /* gif png8 png24 jpeg wbmp */
extern const char *const PC_UI_WEB_FORMAT_LABELS[PC_UI_WEB_NFORMATS]; /* GIF PNG-8 PNG-24 JPEG WBMP */

/* Index of a format id, or -1 when unknown. */
int pc_ui_web_format_index(const char *id);

/* One 4-Up variant derived from the base settings scaled down by `k` (2 or 4), matching
 * file_ui.rs variants(). `fmt` is the base format id; `quality` (JPEG) and `colors` (GIF/PNG-8)
 * are the base values. Writes the variant's format id into out_fmt (cap bytes) and its quality and
 * colour count into *out_quality / *out_colors (whichever applies; the other is left at the base).
 * NULL out pointers are ignored; a NULL/short out_fmt is left untouched. */
void pc_ui_web_variant(const char *fmt, double quality, double colors, double k, char *out_fmt,
                       size_t cap, double *out_quality, double *out_colors);

/* Photoshop's download-time estimate: whole seconds to transfer `bytes` at 56.6 Kbps
 * (ceil(bytes * 8 / 56600)); 0 for a non-finite or non-positive size. */
double pc_ui_web_download_seconds(double bytes);

/* The output pixel size at `percent` of the document size (1..1000), each dimension rounded and
 * clamped to at least 1. A non-finite or out-of-range percent is treated as 100. */
void pc_ui_web_scaled_size(uint32_t width, uint32_t height, double percent, uint32_t *out_w,
                           uint32_t *out_h);

/* Advance a preview clock by whole elapsed frames, keeping fractional time (file_ui.rs
 * advance_preview). `duration` is clamped to >= 1 and `frame` to < duration; the caller resets
 * `last` to `now` on Play. Non-finite now keeps (frame, last); now < last (or non-finite last)
 * restarts from now. Writes the next frame and clock to *out_frame / *out_last (both NULL-safe). */
void pc_ui_web_advance_preview(size_t frame, size_t duration, double fps, double last, double now,
                               size_t *out_frame, double *out_last);

#endif /* PC_UI_WEB_EXPORT_H */
