/* Additional layer information ("tagged blocks") (port of crates/psd/src/tagged.rs).
 *
 * Every block is kept raw (signature, key, data and the exact padding that followed it) so
 * unknown and partly understood blocks round-trip byte for byte; typed views read the simple,
 * well-known keys. Thread-safe. */
#ifndef PC_PSD_TAGGED_H
#define PC_PSD_TAGGED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/types.h"

typedef struct pc_psd_block {
    uint8_t signature[4];     /* 8BIM or 8B64 (MIB8 / 46B8: little-endian data kept from a TIFF) */
    uint8_t key[4];
    pc_psd_bytes data;        /* exactly as covered by the length field */
    pc_psd_opt_bytes padding; /* bytes before the next block; none = zero-pad to even */
} pc_psd_block;

/* Keys whose length field is 8 bytes in PSB files (the spec's 13 plus 8 observed ones). */
#define PC_PSD_LONG_KEY_COUNT 21
extern const char PC_PSD_LONG_KEYS[PC_PSD_LONG_KEY_COUNT][5];
bool pc_psd_uses_long_length(pc_psd_version v, const uint8_t key[4]);
/* MIB8 / 46B8 (see pc_psd_block.signature). */
bool pc_psd_is_foreign_order_signature(const uint8_t *b, size_t len);

/* A new 8BIM block with default padding and a copy of data. */
pc_status pc_psd_block_init(pc_psd_block *b, const char key[4], const void *data, size_t len, pc_error *err);
void pc_psd_block_fini(pc_psd_block *b);
pc_status pc_psd_block_clone(const pc_psd_block *b, pc_psd_block *out, pc_error *err);
bool pc_psd_block_eq(const pc_psd_block *a, const pc_psd_block *b);
bool pc_psd_block_is(const pc_psd_block *b, const char key[4]);
/* Serialized size in bytes. */
size_t pc_psd_block_encoded_len(const pc_psd_block *b, pc_psd_version v);

/* Section divider (lsct / lsdk). */
typedef struct pc_psd_section_divider {
    uint32_t kind; /* PC_PSD_SECTION_* or other */
    bool has_blend;
    uint8_t blend[4];
    bool has_sub_type;
    uint32_t sub_type; /* 0 normal, 1 scene group */
} pc_psd_section_divider;

/* ---- typed views (BlockData); each fails like the reference for malformed data ---- */

/* Whether the key has a typed parser (luni lsct lsdk lyid lnsr clbl infx knko tsly lmgm vmgm lspf
 * lclr iOpa shmd). */
bool pc_psd_block_has_typed(const pc_psd_block *b);
/* For a key with a typed parser: PC_OK when its data parses, else the parse error. */
pc_status pc_psd_block_check_typed(const pc_psd_block *b, pc_error *err);
pc_status pc_psd_block_unicode_name(const pc_psd_block *b, char **out, pc_error *err); /* luni; owned */
pc_status pc_psd_block_section(const pc_psd_block *b, pc_psd_section_divider *out, pc_error *err);
pc_status pc_psd_block_u32(const pc_psd_block *b, uint32_t *out, pc_error *err); /* lyid, lspf */
pc_status pc_psd_block_u16(const pc_psd_block *b, uint16_t *out, pc_error *err); /* lclr */
pc_status pc_psd_block_u8(const pc_psd_block *b, uint8_t *out, pc_error *err);   /* flags, knko, iOpa */
pc_status pc_psd_block_name_source(const pc_psd_block *b, uint8_t out[4], pc_error *err); /* lnsr */

/* ---- constructors ---- */

/* luni: UTF-16 name padded to a multiple of 4, as Photoshop writes it. */
pc_status pc_psd_block_make_unicode_name(const char *utf8, pc_psd_block *out, pc_error *err);
/* lsct with the type and, when given, the blend key and sub type (blend may be NULL; sub_type is
 * written only with a blend key). */
pc_status pc_psd_block_make_section(uint32_t kind, const uint8_t *blend, const uint32_t *sub_type, pc_psd_block *out,
                                    pc_error *err);
pc_status pc_psd_block_make_u32(const char key[4], uint32_t v, pc_psd_block *out, pc_error *err); /* lyid, lspf */
/* One byte then 3 zeros (clbl infx knko tsly lmgm vmgm iOpa). */
pc_status pc_psd_block_make_flag(const char key[4], uint8_t v, pc_psd_block *out, pc_error *err);
pc_status pc_psd_block_make_name_source(const uint8_t src[4], pc_psd_block *out, pc_error *err);
pc_status pc_psd_block_make_sheet_color(uint16_t color, pc_psd_block *out, pc_error *err);

/* Strictly re-parses blocks whose data carries its own sizes (PlLd, SoLd/SoLE, lfx2,
 * lnk2/lnk3/lnkD, FEid/FXid); other keys pass. */
pc_status pc_psd_block_check_structure(const pc_psd_block *b, pc_error *err);

/* Reads blocks until the reader is exhausted; bytes that don't form a block become trailing. */
struct psd_rd;
struct psd_w;
/* `alignment` is the default (canonical) padding: 4 for global blocks, 2 for layer blocks. Padding
 * equal to zeros up to it reads as "default" (padding.has == false). */
pc_status psd_read_blocks(struct psd_rd *r, pc_psd_version v, size_t alignment, pc_psd_block **blocks,
                          size_t *n, pc_psd_bytes *trailing, pc_error *err);
void psd_write_block(struct psd_w *w, const pc_psd_block *b, pc_psd_version v, size_t alignment);
void psd_write_blocks(struct psd_w *w, const pc_psd_block *blocks, size_t n, pc_psd_version v,
                      size_t alignment);
void pc_psd_blocks_free(pc_psd_block *blocks, size_t n);
pc_status pc_psd_blocks_clone(const pc_psd_block *blocks, size_t n, pc_psd_block **out, pc_error *err);
bool pc_psd_blocks_eq(const pc_psd_block *a, size_t na, const pc_psd_block *b, size_t nb);

#endif
