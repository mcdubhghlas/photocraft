/* Displace smart filters (Fltr class "Dspl") <-> PSD filterFX items (port of
 * crates/io/src/displace_map.rs, R10c; a private submodule of smart_map.rs). A Displace item that
 * carries an embedded displacement map (DspD, a `tdta` blob of two 8-bit planes) and known enum
 * values becomes an editable PC_IO_DISPLACE_COMMAND filter; anything else (another map mode, a
 * missing or truncated map, unknown enum values) stays an opaque PC_IO_UNSUPPORTED_FILTER.
 *
 * The whole filterFX item (with the map) is kept verbatim in the params as a hex template
 * (TEMPLATE_KEY "__displacePsd"), so the filter re-renders, survives .pcraft, and writes back to
 * PSD with only the editable settings (scales, fit, undefined areas) replaced.
 *
 * smart_filters.c calls import_params / export_item from pc_io_filter_from_item /
 * pc_io_item_for_filter; the compositor reads the map through pc_io_displace_embedded_map.
 * Thread-safe (no shared state). */
#ifndef PC_IO_DISPLACE_MAP_H
#define PC_IO_DISPLACE_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/image.h"
#include "base/json.h"
#include "base/status.h"
#include "psd/descriptor.h"

/* The Fltr class of a Displace filter, and the smart-filter command it maps to. */
#define PC_IO_DISPLACE_CLASS "Dspl"
#define PC_IO_DISPLACE_COMMAND "filter.distort.displace"

/* displace::import_params: the smart-filter params of a filterFX list item. *found is false (the
 * reference's None: keep the filter opaque) when the item is not a Displace filter with a readable
 * embedded map and known enum values. Fails only on out of memory. *out written only when *found. */
pc_status pc_io_displace_import_params(const pc_psd_desc *item, bool *found, pc_json_value *out,
                                       pc_error *err);

/* displace::export_item: the filterFX item for Displace params (blendOptions and enab as in the
 * template; the caller sets them from the filter). Params with no PSD equivalent are
 * PC_ERR_UNSUPPORTED with the reference's reason as the whole message (no prefix); out of memory is
 * PC_ERR_NOMEM. *out initialised on success. */
pc_status pc_io_displace_export_item(const pc_json_value *params, pc_psd_desc *out, pc_error *err);

/* displace::embedded_map: the displacement map embedded in Displace params imported from PSD.
 * *found is false when there is none or it is unreadable. The two planes become the first two
 * channels; identical planes become one channel (read for both directions). *out initialised on
 * success (and when *found); fails only on out of memory or a bad allocation. */
pc_status pc_io_displace_embedded_map(const pc_json_value *params, bool *found, pc_algo_image *out,
                                      pc_error *err);

#endif
