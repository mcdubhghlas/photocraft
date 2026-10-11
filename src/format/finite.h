/* Save-time check that a manifest holds only finite floats (port of crates/format/src/finite.rs,
 * issue #1101).
 *
 * JSON cannot spell NaN or the infinities: serde_json (and pc_json_write) writes them as null,
 * which the manifest's float fields refuse on load. Such a document would save with a success
 * report and then never open again, so the save is refused instead, naming the value.
 *
 * The reference walks the manifest's Serialize impl; here the walk runs over the yyjson tree the
 * manifest is serialised from (C5 builds it with NaN/inf kept as reals). Both visit values in
 * serialisation order and name them the same way: `.field` for struct fields, map keys and
 * externally tagged enum variants (`layers[1].Exposure.exposure`), `[i]` for sequence and
 * tuple elements, leading dots trimmed. Map keys are written as serde_json::to_string spells them
 * with the surrounding quotes trimmed (so `"` and `\` inside a key appear escaped).
 * The walk is iterative (no recursion on deep trees). Thread-safe. */
#ifndef PC_FORMAT_FINITE_H
#define PC_FORMAT_FINITE_H

#include <yyjson.h>

#include "base/buf.h"
#include "base/status.h"

/* PC_OK when every real number under root is finite. Otherwise PC_ERR_INVALID with
 * FormatError::NonFinite's message ("`PATH` is NaN or infinite, which a .pcraft manifest cannot
 * hold; reset that value and save again"), and *path (if non-NULL) replaced with the full path of
 * the first non-finite value (the message is cut at 255 bytes, the path is not). NULL root is
 * fine (JSON null). */
pc_status pc_format_check_finite(const yyjson_mut_val *root, pc_buf *path, pc_error *err);

#endif
