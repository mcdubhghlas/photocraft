/* Internal IPP helpers exposed for tests (the reference's `super::*` surface): the message
 * builder and parser, the chunked-transfer walk, and the request-as-a-server-sees-it parse used
 * by the in-process fake CUPS server in the tests. */
#ifndef PC_IPP_INTERNAL_H
#define PC_IPP_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "ipp/ipp.h"

/* Largest reply read, and the most attributes taken from a reply. */
#define PC_IPP_MAX_REPLY (16u << 20)
#define PC_IPP_MAX_ATTRIBUTES 100000u

enum {
    PC_IPP_OP_PRINT_JOB = 0x0002,
    PC_IPP_OP_CUPS_GET_DEFAULT = 0x4001,
    PC_IPP_OP_CUPS_GET_PRINTERS = 0x4002,
    PC_IPP_OP_CUPS_GET_CLASSES = 0x4005
};

enum {
    PC_IPP_TAG_OPERATION_GROUP = 0x01,
    PC_IPP_TAG_JOB_GROUP = 0x02,
    PC_IPP_TAG_END = 0x03,
    PC_IPP_TAG_PRINTER_GROUP = 0x04,
    PC_IPP_TAG_INTEGER = 0x21,
    PC_IPP_TAG_BOOLEAN = 0x22,
    PC_IPP_TAG_ENUM = 0x23,
    PC_IPP_TAG_TEXT = 0x41,
    PC_IPP_TAG_NAME = 0x42,
    PC_IPP_TAG_KEYWORD = 0x44,
    PC_IPP_TAG_URI = 0x45,
    PC_IPP_TAG_CHARSET = 0x47,
    PC_IPP_TAG_LANGUAGE = 0x48,
    PC_IPP_TAG_MIME = 0x49
};

/* One value of a reply attribute: raw bytes (data may be NULL when len is 0). */
typedef struct pc_ipp_value {
    uint8_t *data;
    size_t len;
} pc_ipp_value;

/* One attribute of a reply: the group it sits in, its tag, name and values. */
typedef struct pc_ipp_attr {
    uint8_t group;
    uint8_t tag;
    char *name; /* owned, NUL-terminated */
    pc_ipp_value *values;
    size_t nvalues;
    size_t cap_values;
} pc_ipp_attr;

/* A decoded reply. */
typedef struct pc_ipp_reply {
    uint16_t status;
    uint32_t request_id;
    pc_ipp_attr *attrs;
    size_t nattrs;
    size_t cap_attrs;
} pc_ipp_reply;

void pc_ipp_reply_fini(pc_ipp_reply *r);

/* Parse an IPP message (RFC 8010 §3). On OK, *out is filled (caller pc_ipp_reply_fini) and, when
 * out_end is non-NULL, *out_end is where the attributes end (document data may follow). */
pc_status pc_ipp_reply_parse(const uint8_t *b, size_t len, pc_ipp_reply *out, size_t *out_end,
                             pc_error *err);

/* The first value of `a` as a NUL-terminated C string appended to `out` (empty when none). */
pc_status pc_ipp_attr_text(const pc_ipp_attr *a, pc_buf *out, pc_error *err);
/* The first value of `a` as a big-endian i32; true when it has a 4-byte value. */
bool pc_ipp_attr_int(const pc_ipp_attr *a, int32_t *out);
/* The attribute named `name`, or NULL. */
const pc_ipp_attr *pc_ipp_reply_find(const pc_ipp_reply *r, const char *name);

/* A request builder on a pc_buf. pc_ipp_request_begin writes the header and the two mandatory
 * attributes; append more with the attr helpers; pc_ipp_request_end adds the end tag. */
pc_status pc_ipp_request_begin(pc_buf *b, uint16_t op, uint32_t id, pc_error *err);
pc_status pc_ipp_request_attr(pc_buf *b, uint8_t tag, const char *name, const void *value,
                              size_t value_len, pc_error *err);
pc_status pc_ipp_request_integer(pc_buf *b, const char *name, int32_t v, pc_error *err);
pc_status pc_ipp_request_group(pc_buf *b, uint8_t tag, pc_error *err);
pc_status pc_ipp_request_end(pc_buf *b, pc_error *err);

/* Whether a chunked body has reached its last (zero-size) chunk, walking the chunk sizes from
 * *resume (updated) so a last-chunk pattern inside a payload does not count. */
bool pc_ipp_chunked_complete(const uint8_t *body, size_t len, size_t *resume);
/* Join the chunks of a chunked transfer encoding into *out (caller pc_buf_fini). */
pc_status pc_ipp_dechunk(const uint8_t *b, size_t len, pc_buf *out, pc_error *err);

/* A request as a server sees it: the operation, its attributes and the document after the end
 * tag. For the in-process fake server in the tests. On OK *out_reply holds op in its `status`
 * field and the attributes (caller pc_ipp_reply_fini); the document is (*out_doc, *out_doc_len)
 * borrowed from `b`. */
pc_status pc_ipp_parse_request(const uint8_t *b, size_t len, pc_ipp_reply *out_reply,
                               const uint8_t **out_doc, size_t *out_doc_len, pc_error *err);

/* Where `/path` lands on the server: `/printers/<name>` or `/classes/<name>`, appended to `out`
 * with the name percent-encoded. Rejects a name with control characters. */
pc_status pc_ipp_queue_path(const char *name, bool is_class, pc_buf *out, pc_error *err);

#endif
