/* Alpha / spot channel options <-> the PSD DisplayInfo image resources 1077 (and the older
 * 1007), and Multichannel documents <-> PSD colour mode 7 (ports of crates/io/src/channel_map.rs
 * and multichannel_map.rs). Internal to io: used by I2's psd_import and I3's psd_export.
 *
 * Resource 1077 starts with a 4-byte version (1), then one 14-byte record per extra channel:
 * colour space (u16), four u16 components, opacity (u16, 0..100), kind (u8: 0 colour indicates
 * selected areas, 1 masked areas, 2 spot channel) and a padding byte. 1007 has the same records
 * without the version and the spot kind. Thread-safe (no shared state). */
#ifndef PC_IO_CHANNEL_MAP_H
#define PC_IO_CHANNEL_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "io/core.h"
#include "psd/file.h"

enum {
    PC_IO_DISPLAY_INFO = 1077,
    PC_IO_DISPLAY_INFO_OLD = 1007,
    PC_IO_QUICK_MASK_INFO = 1022 /* channel id (u16) and an "initially empty" flag (u8) */
};

/* Resource 1077 for channels (in file order); *out replaced. */
pc_status pc_io_display_info(const pc_alpha_channel *const *channels, size_t n, pc_buf *out, pc_error *err);
/* Applies a DisplayInfo resource (versioned = 1077) to the channels in order; short or
 * truncated records are ignored. */
void pc_io_apply_display_info(const uint8_t *data, size_t len, bool versioned, pc_alpha_channel *channels,
                              size_t n);
/* A Photoshop colour structure (space + four u16 components) as an RGB colour. */
pc_color pc_io_decode_color(uint16_t space, const uint16_t c[4]);

/* Fills doc (already Multichannel at the file depth) from the merged planes `all` (planar,
 * big-endian, as decoded): one spot channel per plane, named from names[k] (n names; empty or
 * missing -> "Channel k+1"), ink colours from DisplayInfo. A missing plane adds a warning. */
pc_status pc_io_multichannel_import(const pc_psd_file *file, const uint8_t *all, size_t all_len,
                                    pc_document *doc, const pc_str *names, size_t nnames,
                                    pc_io_warnings *warnings, pc_error *err);
/* A Multichannel document as a mode-7 PSD (no layer section; every channel a plane, at most 56;
 * 32-bit written as 16). *out is initialised on success. */
pc_status pc_io_multichannel_to_psd(const pc_document *doc, bool force_psb, pc_psd_file *out,
                                    pc_io_warnings *warnings, pc_error *err);

#endif
