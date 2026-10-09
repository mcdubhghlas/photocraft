/* The 26-byte PSD/PSB file header (port of crates/psd/src/header.rs). Pure data; thread-safe. */
#ifndef PC_PSD_HEADER_H
#define PC_PSD_HEADER_H

#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/types.h"

typedef struct pc_psd_header {
    pc_psd_version version;
    uint8_t reserved[6]; /* spec: zero; kept for exact round trips */
    uint16_t channels;   /* merged image channels incl. alpha (1..=56) */
    uint32_t height;
    uint32_t width;
    uint16_t depth;      /* 1, 8, 16 or 32 */
    uint16_t color_mode; /* PC_PSD_MODE_* or any other value */
} pc_psd_header;

pc_psd_header pc_psd_header_make(pc_psd_version v, uint32_t width, uint32_t height, uint16_t channels, uint16_t depth,
                                 uint16_t color_mode);
/* Spec constraints and safety limits (PC_ERR_FORMAT / PC_ERR_LIMIT). */
pc_status pc_psd_header_validate(const pc_psd_header *h, pc_error *err);
/* Parses and validates 26 bytes at the start of data. */
pc_status pc_psd_header_parse(const uint8_t *data, size_t len, pc_psd_header *out, pc_error *err);
void pc_psd_header_write(const pc_psd_header *h, uint8_t out[26]);
bool pc_psd_header_eq(const pc_psd_header *a, const pc_psd_header *b);

/* Bytes per row of `width` samples at `depth` bits (depth 1 packs 8 per byte; saturating). */
size_t pc_psd_row_bytes(size_t width, uint16_t depth);
static inline size_t pc_psd_header_row_bytes(const pc_psd_header *h) { return pc_psd_row_bytes(h->width, h->depth); }

#endif
