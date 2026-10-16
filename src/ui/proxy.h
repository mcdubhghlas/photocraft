/* Downsampled proxy documents for fast interactive previews (port of the pure math from
 * crates/ui-egui/src/proxy.rs). The proxy build/composite lives in src/compose; U1 owns the
 * factor math the UI uses to choose a preview resolution. No GUI dependency. */
#ifndef PC_UI_PROXY_H
#define PC_UI_PROXY_H

#include <stdbool.h>
#include <stdint.h>

/* Target pixel count for previews (fast enough to composite every frame on the CPU). */
#define PC_UI_PREVIEW_PIXELS 2500000ULL

/* Downscale factor for a document of `area` pixels, or 1 when it is already small. */
uint32_t pc_ui_proxy_factor(uint64_t area);

/* The factor of a live preview: pc_ui_proxy_factor when `reduced`, else 1 (full resolution). */
uint32_t pc_ui_proxy_preview_factor(uint64_t area, bool reduced);

#endif
