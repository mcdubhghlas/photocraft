/* Adjustment layers <-> PSD adjustment blocks (chunk I2: crates/io/src/adjust_map.rs;
 * adjust_map.c parses, adjust_write.c writes). Thread-safe. */
#ifndef PC_IO_ADJUST_MAP_H
#define PC_IO_ADJUST_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "doc/doc.h"
#include "io/core.h"

/* The adjustment block keys (levl curv hue2 brit nvrt thrs post expA vibA blnc mixr grdm phfl
 * selc blwh clrL). */
extern const char PC_IO_ADJUSTMENT_KEYS[16][5];

/* Appends the blocks that store adj (its main block and any companions). */
pc_status pc_io_adjust_write(const pc_adjustment *adj, pc_io_raw_blocks *out, pc_error *err);

/* How the per-channel records of Levels / Curves map to display channels (adjust_map::Channels). */
typedef enum pc_io_adj_channels {
    PC_IO_ADJ_RGB,  /* records 1..=3 are R, G, B */
    PC_IO_ADJ_GRAY, /* record 1 is gray (applied to all three) */
    PC_IO_ADJ_CMYK, /* records 1..=4 are C, M, Y, K */
    PC_IO_ADJ_LAB,  /* records 1..=3 are L, a, b */
    PC_IO_ADJ_OTHER /* records ignored */
} pc_io_adj_channels;

/* Parses an adjustment block (cged: the optional CgEd block data, NULL when absent). Blocks that
 * can't be read become PC_ADJ_UNSUPPORTED with the raw bytes. Fails only on out of memory. */
pc_status pc_io_adjust_parse(const uint8_t key[4], const uint8_t *data, size_t len, const uint8_t *cged,
                             size_t cged_len, pc_io_adj_channels channels, pc_adjustment *out, pc_error *err);

#endif
