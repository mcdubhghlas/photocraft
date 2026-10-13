/* Multi-part OpenEXR and channel groups as layers (port of crates/io/src/exr_parts.rs, R10c): a
 * Maya/Arnold render writes one part per AOV, or a single part with prefixed channel sets like
 * `diffuse.R`. pc_io_exr_entries lists the selectable entries for a chooser (whole parts, prefixed
 * channel groups, and Cryptomatte layers as information); pc_io_exr_import imports the chosen ones
 * as the layers of one document, with an optional additive beauty precomp from the AOVs.
 *
 * All the EXR decoding is done by the codecs (pc_cexr_info / pc_cdecode_exr_part /
 * pc_cdecode_exr_channels / pc_ccryptomatte_layers); this file only turns their results into the
 * document model. Thread-safe. */
#ifndef PC_IO_EXR_PARTS_H
#define PC_IO_EXR_PARTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io/core.h"

typedef enum pc_io_exr_entry_kind {
    PC_IO_EXR_PART,        /* a whole flat part */
    PC_IO_EXR_GROUP,       /* a prefixed channel group inside a part */
    PC_IO_EXR_CRYPTOMATTE  /* a Cryptomatte layer (information only; not an image layer) */
} pc_io_exr_entry_kind;

/* One selectable line of the chooser. Strings are owned (free with pc_io_exr_entries_free). */
typedef struct pc_io_exr_entry {
    pc_io_exr_entry_kind kind;
    char *name;      /* the part name, the channel-group prefix, or the Cryptomatte layer name */
    char *view;      /* the part's `view` attribute (stereo), or NULL */
    uint32_t width;  /* 0 for groups and Cryptomatte entries */
    uint32_t height;
    size_t channels; /* image channels that would import (Cryptomatte: stream count) */
    bool has_objects;
    size_t objects;  /* Cryptomatte only: objects in the parsed manifest */
} pc_io_exr_entry;

void pc_io_exr_entries_free(pc_io_exr_entry *v, size_t n);

/* Lists the entries of a multi-part or channel-grouped EXR: flat parts in file order, then the
 * channel groups, then the Cryptomatte layers. A file whose parts are all deep is refused
 * (PC_ERR_UNSUPPORTED; the deep decoder composites those). *out is a new array on success. */
pc_status pc_io_exr_entries(const uint8_t *bytes, size_t len, pc_io_exr_entry **out, size_t *n,
                            pc_error *err);

/* What pc_io_exr_import turns into layers. Borrowed string arrays. */
typedef struct pc_io_exr_selection {
    const char *const *parts; /* part names to import as whole layers */
    size_t nparts;
    const char *const *groups; /* channel-group prefixes to import as layers */
    size_t ngroups;
    const char *view; /* "both", "left" or "right": which stereo views to consider */
    bool precomp;     /* add a `beauty (precomp)` layer summed from the file's AOV groups */
} pc_io_exr_selection;

/* Imports the chosen entries as the layers of one document: parts whole, groups from their own
 * channels, F16 kept losslessly as F32, the linear-light EXR values tagged linear sRGB. `precomp`
 * adds the additive sum of the AOV groups as a top layer and fails, naming the missing AOVs, when
 * the light-path set is incomplete. *out initialised on success. */
pc_status pc_io_exr_import(const char *name, const uint8_t *bytes, size_t len,
                           const pc_io_exr_selection *sel, pc_io_import_result *out, pc_error *err);

#endif
