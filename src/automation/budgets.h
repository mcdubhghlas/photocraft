/* Fixed ceilings for automation previews and outbound JSON (port of budgets.rs). These bound
 * individual outputs, not total document memory or the duration of an engine command. */
#ifndef PC_AUTOMATION_BUDGETS_H
#define PC_AUTOMATION_BUDGETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "base/buf.h"
#include "base/status.h"

/* Maximum encoded JSON reply, including its newline for JSON-lines transports. */
#define PC_AUTO_MAX_RESPONSE_BYTES ((size_t)(8u << 20))
/* Maximum PNG bytes before base64 expansion (leaves room for JSON/MCP metadata). */
#define PC_AUTO_MAX_PNG_BYTES ((size_t)(5u << 20))
/* Maximum requested preview edge. Zero still requests full size, within this ceiling. */
#define PC_AUTO_MAX_PREVIEW_SIDE ((uint32_t)2048)
/* Maximum source pixels processed by a headless automation preview. */
#define PC_AUTO_MAX_RENDER_SOURCE_PIXELS ((uint64_t)(64u << 20))

/* Check before the compositor runs. Oversized requests fail rather than silently downscale. */
pc_status pc_auto_check_preview(uint32_t width, uint32_t height, uint32_t max_side, pc_error *err);
/* Check before base64 encoding or writing a rendered PNG. */
pc_status pc_auto_check_png(size_t bytes, pc_error *err);

/* Serialize `root` (compact) into *out when it fits in PC_AUTO_MAX_RESPONSE_BYTES-1 bytes; else
 * PC_ERR_INVALID "response encoding failed: response exceeds N bytes". *out is appended to. */
pc_status pc_auto_json_bytes(const yyjson_mut_val *root, pc_buf *out, pc_error *err);

/* Encode `reply` and append it plus a newline to *out. A reply that exceeds the response budget is
 * replaced by a small error carrying the same `id` (port of write_reply). */
pc_status pc_auto_write_reply(pc_buf *out, yyjson_mut_val *reply, pc_error *err);

/* Incremental batch-reply budget (port of BatchReplyBudget): reserves envelope/id space and bounds
 * retained batch results so many valid replies cannot accumulate into an enormous batch. */
typedef struct pc_auto_batch_budget {
    size_t remaining;
    bool escaped; /* charge each result as it costs inside a JSON string (MCP text content) */
} pc_auto_batch_budget;

pc_auto_batch_budget pc_auto_batch_budget_default(void);
pc_auto_batch_budget pc_auto_batch_budget_escaped(void);
/* Must be called before retaining a step result or dispatching the next step. */
pc_status pc_auto_batch_budget_charge(pc_auto_batch_budget *b, const yyjson_mut_val *result, pc_error *err);

#endif /* PC_AUTOMATION_BUDGETS_H */
