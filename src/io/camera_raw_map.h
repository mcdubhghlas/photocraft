/* Camera Raw Filter smart filters <-> PSD filterFX items (chunk I5:
 * crates/io/src/camera_raw_map.rs, a private submodule of smart_map.rs). Only descriptors
 * observed in real PSD files (filter version 18.4, processes 5 and 6) with nothing but verified
 * develop controls (and neutral values of unmodelled ones) become editable
 * PC_IO_CAMERA_RAW_COMMAND filters; anything else stays an opaque PC_IO_UNSUPPORTED_FILTER.
 * The params keep the source item (hex, "__cameraRawPsd") so an unedited filter writes back
 * byte for byte and an edit patches only the changed keys.
 *
 * smart_map.c (I3) calls these from pc_io_filter_from_item / pc_io_item_for_filter.
 * Thread-safe (no shared state). */
#ifndef PC_IO_CAMERA_RAW_MAP_H
#define PC_IO_CAMERA_RAW_MAP_H

#include <stdbool.h>

#include "base/json.h"
#include "base/status.h"
#include "psd/descriptor.h"

/* The Fltr class of a Camera Raw filter, and the smart-filter command it maps to. */
#define PC_IO_CAMERA_RAW_CLASS "Adobe Camera Raw Filter"
#define PC_IO_CAMERA_RAW_COMMAND "filter.cameraRaw"

/* camera_raw::import_params: the smart-filter params of a filterFX list item. *found is false
 * (the reference's None: keep the filter opaque) when the item is not a supported Camera Raw
 * filter. Fails only on out of memory. *out written only when *found. */
pc_status pc_io_camera_raw_import_params(const pc_psd_desc *item, bool *found, pc_json_value *out,
                                         pc_error *err);

/* camera_raw::export_item: the filterFX item for Camera Raw params (blendOptions and enab as in
 * the template or neutral: the caller sets them from the filter). A params set with no verified
 * mapping is PC_ERR_UNSUPPORTED with the reference's reason as the whole message (no prefix);
 * out of memory is PC_ERR_NOMEM. *out initialised on success. */
pc_status pc_io_camera_raw_export_item(const pc_json_value *params, pc_psd_desc *out, pc_error *err);

#endif
