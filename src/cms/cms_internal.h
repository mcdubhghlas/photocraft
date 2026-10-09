/* Private helpers shared by the cms sources. Not part of the module's API. */
#ifndef PC_CMS_INTERNAL_H
#define PC_CMS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "profile.h"

/* The reference's CmsError messages. The first variadic argument must be a string literal. */
#define cms_truncated(err) pc_fail((err), PC_ERR_FORMAT, "ICC profile is truncated")
#define cms_bad_signature(err) pc_fail((err), PC_ERR_FORMAT, "not an ICC profile (missing 'acsp' signature)")
#define cms_invalid(err, ...) pc_fail((err), PC_ERR_FORMAT, "invalid ICC profile: " __VA_ARGS__)
#define cms_unsupported(err, ...) pc_fail((err), PC_ERR_UNSUPPORTED, "unsupported ICC feature: " __VA_ARGS__)

/* ICC v4.3 encoding of a profile model (write.c). */
pc_status pc_icc_encode(const pc_profile *p, pc_buf *out, pc_error *err);

/* Content hash of ICC bytes: the first 8 bytes of their BLAKE3, little-endian. */
uint64_t pc_cms_hash_bytes(const uint8_t *b, size_t len);

/* The `{:?}` rendering of a byte string decoded lossily ("mft1" with quotes, controls escaped). */
void pc_cms_debug_str(const uint8_t *b, size_t len, char *out, size_t cap);

#endif
