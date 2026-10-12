/* Shared state of a PSD import (psd_import.rs's Ctx), between psd_import.c (document level:
 * resources, merged image, channels) and psd_layers.c (layer records). Internal to src/io. */
#ifndef PC_IO_PSD_IMPORT_INTERNAL_H
#define PC_IO_PSD_IMPORT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "doc/doc.h"
#include "io/core.h"
#include "psd/file.h"
#include "psd/filter_effects.h"
#include "psd/misc.h"
#include "raster/interrupt.h"
#include "text/engine_data.h"

typedef struct pc_io_imp {
    const pc_psd_file *file;
    const pc_psd_layer *layers; /* the file's layer records */
    size_t nlayers;
    pc_pixel_format fmt;
    pc_pixel_format mask_fmt;
    size_t cc; /* colour channels */
    bool cmyk;
    pc_io_warnings *warnings;
    float dpi; /* document resolution (type sizes are converted to points with it) */
    bool has_txt2;
    pc_ed_value txt2;   /* the parsed Txt2 block (type settings EngineData lacks) */
    pc_psd_fx_item *fx; /* smart-filter caches from FEid / FXid (filter masks, by placed id) */
    size_t nfx;
    const pc_interrupt *ctl; /* may be NULL */
    size_t done;             /* layer records decoded so far, out of nlayers (progress) */
} pc_io_imp;

/* Imports the layer tree into *layers / *n (bottom to top). PC_ERR_CANCELLED when ctl cancels;
 * fails otherwise only on out of memory and on functions of later chunks that are not ported. */
pc_status pc_io_imp_build(pc_io_imp *cx, const pc_psd_node *nodes, size_t nnodes, size_t depth,
                          pc_layer **layers, size_t *n, pc_error *err);
/* Sets each layer's link_group from resource 1026's per-record ids. */
void pc_io_imp_link_groups(const pc_psd_node *nodes, size_t nnodes, pc_layer *layers, size_t nl,
                           const uint16_t *ids, size_t nids, size_t depth);

#endif
