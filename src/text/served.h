/* Served fonts (crates/text/src/served.rs): NOT PORTED, no-op stubs.
 *
 * Upstream's served.rs lets the web build fetch fonts a host lists in `fonts/manifest.txt` on
 * demand and use them as script fallbacks. The C build has no web target (REWRITE_PLAN.md D2), so
 * the catalog is always empty: requests are ignored and the fallback stack gains nothing. The
 * functions exist so fonts.c and layout keep the reference's call structure. Thread-safe (no
 * state). */
#ifndef PC_TEXT_SERVED_H
#define PC_TEXT_SERVED_H

#include <stddef.h>

#include "base/status.h"
#include "text/fonts.h"

/* The served families that are some script's fallback: always none (out is emptied). */
pc_status pc_text_served_script_fallback_families(pc_strlist *out, pc_error *err);
/* Asks for a family to be fetched: ignored (nothing is served). */
void pc_text_served_request(const char *family, size_t len);
/* Asks for the served fallback families text needs: ignored. */
void pc_text_served_request_for_text(const char *text, size_t len);

#endif
