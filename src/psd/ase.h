/* Adobe Swatch Exchange files (.ase), shared by Photoshop, Illustrator and InDesign (port of
 * crates/psd/src/ase.rs).
 *
 * Layout (big-endian): "ASEF", u16 major (1), u16 minor (0), u32 block count, then blocks of a
 * u16 type, a u32 body length and the body: 0xC001 group start (a name), 0xC002 group end
 * (empty), 0x0001 colour entry (a name, a four-character model "RGB ", "CMYK", "LAB ", "Gray",
 * its f32 components (3, 4, 3, 1) and a u16 colour type: 0 global, 1 spot, 2 process). A name
 * is a u16 UTF-16 unit count including the trailing NUL, then the units. Components are kept
 * as stored (non-finite ones read as 0); unknown models are kept verbatim, unknown block types
 * skipped. Converting colours is the caller's job. Thread-safe. */
#ifndef PC_PSD_ASE_H
#define PC_PSD_ASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/types.h"

#define PC_PSD_ASE_MAX_SWATCHES 100000u  /* most colour entries in one file */
#define PC_PSD_ASE_MAX_NAME_UNITS 1024u  /* longest name read or written, in UTF-16 units */

typedef enum pc_psd_ase_model {
    PC_PSD_ASE_RGB,  /* c[0..3] 0..1 */
    PC_PSD_ASE_CMYK, /* c[0..4] ink 0..1 */
    PC_PSD_ASE_LAB,  /* c[0] = L* / 100, c[1] = a*, c[2] = b* */
    PC_PSD_ASE_GRAY, /* c[0] 0..1, 1 = white */
    PC_PSD_ASE_OTHER /* tag + the component bytes as stored (up to the colour type) */
} pc_psd_ase_model;

typedef struct pc_psd_ase_color {
    pc_psd_ase_model model;
    float c[4];         /* unused components are 0 */
    uint8_t tag[4];     /* OTHER */
    pc_psd_bytes data;  /* OTHER */
} pc_psd_ase_color;

typedef enum pc_psd_ase_kind { PC_PSD_ASE_GLOBAL = 0, PC_PSD_ASE_SPOT = 1, PC_PSD_ASE_PROCESS = 2 } pc_psd_ase_kind;

typedef struct pc_psd_ase_swatch {
    pc_psd_str name;
    pc_psd_ase_color color;
    pc_psd_ase_kind kind;
} pc_psd_ase_swatch;

/* A top-level entry: a colour, or a named group of colours (one level of groups). */
typedef struct pc_psd_ase_entry {
    bool group;
    pc_psd_ase_swatch swatch;      /* !group */
    pc_psd_str name;               /* group */
    pc_psd_ase_swatch *swatches;   /* group */
    size_t n;
} pc_psd_ase_entry;

typedef struct pc_psd_ase {
    pc_psd_ase_entry *entries; /* file order */
    size_t n;
} pc_psd_ase;

void pc_psd_ase_swatch_fini(pc_psd_ase_swatch *s);
void pc_psd_ase_entry_fini(pc_psd_ase_entry *e);
void pc_psd_ase_fini(pc_psd_ase *f);
bool pc_psd_ase_swatch_eq(const pc_psd_ase_swatch *a, const pc_psd_ase_swatch *b); /* f32 == */
bool pc_psd_ase_eq(const pc_psd_ase *a, const pc_psd_ase *b);
size_t pc_psd_ase_swatch_count(const pc_psd_ase *f); /* in and outside groups */

/* A wrong signature or version, a truncated block, a block count beyond the data or more than
 * PC_PSD_ASE_MAX_SWATCHES colours is an error. Unknown block types are skipped, a group end
 * without a start ignored, and a group left open (or started inside another) is closed. */
pc_status pc_psd_ase_parse(const uint8_t *data, size_t len, pc_psd_ase *out, pc_error *err);
/* Version 1.0; fails only on 32-bit field overflow or too many colours (PC_ERR_LIMIT). */
pc_status pc_psd_ase_write(const pc_psd_ase *f, pc_psd_bytes *out, pc_error *err);

#endif
