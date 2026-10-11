/* Downsampled proxy documents (port of crates/compose/src/proxy.rs), multichannel ink display
 * (multichannel.rs) and cached content bounds (bounds.rs).
 *
 * A proxy scales every pixel surface down by an integer factor k (nearest neighbour), so a
 * composite of the proxy approximates the document's composite at 1/k resolution for ~1/k^2 of
 * the work (thumbnails, navigator, histograms of large documents). Thread-safe. */
#ifndef PC_COMPOSE_PROXY_H
#define PC_COMPOSE_PROXY_H

#include <stdbool.h>
#include <stdint.h>

#include "doc/doc.h"

/* Nearest-neighbour downsample by k (document coordinates / k); k <= 1 clones. */
pc_status pc_compose_downsample(const pc_surface *s, uint32_t k, pc_surface *out, pc_error *err);
/* A copy of doc scaled down by k (same layer ids; selection and channels scaled too). */
pc_status pc_compose_proxy_document(const pc_document *doc, uint32_t k, pc_document *out, pc_error *err);
/* A proxy composite is a faithful reduction: no layer effects and no enabled vector masks
 * (false also when the walk runs out of memory). */
bool pc_compose_proxy_faithful(const pc_document *doc);

/* pc_surface_content_bounds with the per-tile scan cached by tile identity. The cache keeps each
 * scanned tile alive (a reference) so its address can't be reused while cached, and drops
 * entries first-in first-out beyond 64 MiB of pinned tiles or 65536 entries (the reference holds
 * weak references instead, §7). Falls back to an uncached scan when memory runs out. */
pc_rect pc_compose_content_bounds(const pc_surface *s);
void pc_compose_purge_bounds_cache(void);

/* forward declaration: the buffer type lives in compose.h */
struct pc_compose_buffer;
/* The printed inks (spot channels over white paper) of doc over rect (opaque). */
pc_status pc_compose_inks(const pc_document *doc, pc_rect rect, struct pc_compose_buffer *out, pc_error *err);

#endif
