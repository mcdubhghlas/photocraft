/* Port of crates/automation/src/budgets.rs. */
#include "automation/budgets.h"

#include <string.h>

#include "automation/automation.h"
#include "automation/security.h"
#include "base/json.h"

pc_status pc_auto_check_preview(uint32_t width, uint32_t height, uint32_t max_side, pc_error *err)
{
    if (width == 0 || height == 0) {
        return pc_auto_bad(err, "automation preview requires nonzero document dimensions");
    }
    if ((uint64_t)width * (uint64_t)height > PC_AUTO_MAX_RENDER_SOURCE_PIXELS) {
        return pc_auto_bad(err, "automation preview source exceeds %llu pixels",
                           (unsigned long long)PC_AUTO_MAX_RENDER_SOURCE_PIXELS);
    }
    uint32_t side = max_side == 0 ? (width > height ? width : height) : max_side;
    if (side > PC_AUTO_MAX_PREVIEW_SIDE) {
        return pc_auto_bad(err, "automation preview side exceeds %u pixels", (unsigned)PC_AUTO_MAX_PREVIEW_SIDE);
    }
    return PC_OK;
}

pc_status pc_auto_check_png(size_t bytes, pc_error *err)
{
    if (bytes > PC_AUTO_MAX_PNG_BYTES) {
        return pc_auto_bad(err, "automation PNG exceeds %zu bytes", PC_AUTO_MAX_PNG_BYTES);
    }
    return PC_OK;
}

/* Compact serialisation of `root`, appended to *tmp; returns its length. On NOMEM returns an
 * error. Does not enforce a limit (callers do). */
static pc_status encode_compact(const yyjson_mut_val *root, pc_buf *tmp, pc_error *err)
{
    return pc_json_write(root, 0, tmp, err);
}

/* Serialise `root` with a byte ceiling, appending to *out only when it fits. */
static pc_status encode_with_limit(const yyjson_mut_val *root, size_t maximum, pc_buf *out, pc_error *err)
{
    pc_buf tmp = PC_BUF_INIT;
    pc_status rc = encode_compact(root, &tmp, err);
    if (rc != PC_OK) {
        pc_buf_fini(&tmp);
        return rc;
    }
    if (tmp.len > maximum) {
        pc_buf_fini(&tmp);
        return pc_auto_bad(err, "response encoding failed: response exceeds %zu bytes", maximum);
    }
    rc = pc_buf_append(out, tmp.data, tmp.len, err);
    pc_buf_fini(&tmp);
    return rc;
}

pc_status pc_auto_json_bytes(const yyjson_mut_val *root, pc_buf *out, pc_error *err)
{
    return encode_with_limit(root, PC_AUTO_MAX_RESPONSE_BYTES - 1, out, err);
}

pc_status pc_auto_write_reply(pc_buf *out, yyjson_mut_val *reply, pc_error *err)
{
    pc_buf tmp = PC_BUF_INIT;
    pc_status rc = encode_with_limit(reply, PC_AUTO_MAX_RESPONSE_BYTES - 1, &tmp, NULL);
    if (rc == PC_OK) {
        rc = pc_buf_append(out, tmp.data, tmp.len, err);
        if (rc == PC_OK) {
            rc = pc_buf_append_byte(out, '\n', err);
        }
        pc_buf_fini(&tmp);
        return rc;
    }
    pc_buf_fini(&tmp);
    /* Build a small error carrying the original id. */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        /* Last resort: the fixed error literal (matches the Rust final fallback). */
        rc = pc_buf_append_str(out, "{\"id\":null,\"ok\":false,\"error\":\"response budget exceeded\"}\n", err);
        return rc == PC_OK ? PC_OK : rc;
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_val *id = yyjson_mut_obj_get(reply, "id");
    yyjson_mut_val *idcopy = id ? yyjson_mut_val_mut_copy(doc, id) : yyjson_mut_null(doc);
    char msg[128];
    (void)snprintf(msg, sizeof msg, "response exceeds %zu bytes; operation may have completed",
                   PC_AUTO_MAX_RESPONSE_BYTES);
    yyjson_mut_obj_add_val(doc, o, "id", idcopy ? idcopy : yyjson_mut_null(doc));
    yyjson_mut_obj_add_bool(doc, o, "ok", false);
    yyjson_mut_obj_add_strcpy(doc, o, "error", msg);
    yyjson_mut_doc_set_root(doc, o);
    pc_buf tmp2 = PC_BUF_INIT;
    rc = encode_with_limit(o, PC_AUTO_MAX_RESPONSE_BYTES - 1, &tmp2, NULL);
    if (rc == PC_OK) {
        rc = pc_buf_append(out, tmp2.data, tmp2.len, err);
        if (rc == PC_OK) {
            rc = pc_buf_append_byte(out, '\n', err);
        }
    } else {
        rc = pc_buf_append_str(out, "{\"id\":null,\"ok\":false,\"error\":\"response budget exceeded\"}\n", err);
    }
    pc_buf_fini(&tmp2);
    yyjson_mut_doc_free(doc);
    return rc;
}

pc_auto_batch_budget pc_auto_batch_budget_default(void)
{
    pc_auto_batch_budget b;
    b.remaining = PC_AUTO_MAX_RESPONSE_BYTES - PC_AUTO_MAX_REQUEST_BYTES - 4096;
    b.escaped = false;
    return b;
}

pc_auto_batch_budget pc_auto_batch_budget_escaped(void)
{
    pc_auto_batch_budget b = pc_auto_batch_budget_default();
    b.escaped = true;
    return b;
}

static size_t sat_sub(size_t a, size_t b)
{
    return a > b ? a - b : 0;
}

pc_status pc_auto_batch_budget_charge(pc_auto_batch_budget *b, const yyjson_mut_val *result, pc_error *err)
{
    size_t limit = sat_sub(b->remaining, 1);
    pc_buf tmp = PC_BUF_INIT;
    pc_status rc = encode_with_limit(result, limit, &tmp, err);
    if (rc != PC_OK) {
        pc_buf_fini(&tmp);
        return rc;
    }
    size_t size = tmp.len;
    if (b->escaped) {
        /* Compact JSON has no raw control characters, so only `"` and `\` grow when escaped. */
        for (size_t i = 0; i < tmp.len; i++) {
            if (tmp.data[i] == '"' || tmp.data[i] == '\\') {
                size++;
            }
        }
    }
    pc_buf_fini(&tmp);
    if (size > limit) {
        return pc_auto_bad(err, "response exceeds %zu bytes once escaped", limit);
    }
    b->remaining = sat_sub(b->remaining, size + 1);
    return PC_OK;
}
