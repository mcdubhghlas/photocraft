/* Smart-filter caches: the global FEid / FXid ("Filter Effects") block (port of
 * crates/psd/src/filter_effects.rs).
 *
 *   u32 version (1..=3), then sections (u64 length; zero-padded to 4 outside the length), each
 *   holding items: Pascal id (unpadded), u32 item version, u64 item length, then inside the
 *   item rect, u32 depth, u32 max channels, max channels + 2 slots (u32 written flag; if set
 *   u64 length, u16 compression, data); after the item a u8 flag and, if set, the filter mask
 *   (rect, u64 length, u16 compression, data).
 *
 * RLE row counts are 4 bytes wide in every file version. Thread-safe. */
#ifndef PC_PSD_FILTER_EFFECTS_H
#define PC_PSD_FILTER_EFFECTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/types.h"

#define PC_PSD_FX_MAX_SLOTS 64u

typedef struct pc_psd_fx_plane {
    uint16_t compression;
    pc_psd_bytes data; /* without the compression field */
} pc_psd_fx_plane;

typedef struct pc_psd_fx_item {
    char *id; /* the smart object's placed id (lossy UTF-8, owned) */
    uint32_t version;
    pc_psd_rect rect;
    uint32_t depth;
    uint32_t max_channels;
    pc_psd_fx_plane *slots; /* max_channels + 2 entries */
    bool *slot_written;     /* same count; false = not written */
    size_t nslots;
    bool has_mask;
    pc_psd_rect mask_rect;
    pc_psd_fx_plane mask;
} pc_psd_fx_item;

typedef struct pc_psd_filter_effects {
    uint32_t version;
    pc_psd_fx_item *items;
    size_t n;
} pc_psd_filter_effects;

/* Encodes / decodes one plane of w × h big-endian samples. decode refuses sizes the data can't
 * hold before allocating ("filter effects: plane larger than its data can hold"). */
pc_status pc_psd_fx_plane_encode(uint16_t compression, const uint8_t *decoded, size_t len, size_t w, size_t h,
                                 uint16_t depth, pc_psd_fx_plane *out, pc_error *err);
pc_status pc_psd_fx_plane_decode(const pc_psd_fx_plane *p, size_t w, size_t h, uint16_t depth, pc_psd_bytes *out,
                                 pc_error *err);

pc_status pc_psd_filter_effects_parse(const uint8_t *data, size_t len, pc_psd_filter_effects *out, pc_error *err);
/* Photoshop's layout: one section per item, padded to 4 (an empty section when there are none). */
pc_status pc_psd_filter_effects_to_bytes(const pc_psd_filter_effects *fx, pc_psd_bytes *out, pc_error *err);
void pc_psd_filter_effects_fini(pc_psd_filter_effects *fx);
void pc_psd_fx_item_fini(pc_psd_fx_item *it);
bool pc_psd_fx_item_eq(const pc_psd_fx_item *a, const pc_psd_fx_item *b);

#endif
