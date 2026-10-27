/* pc_ipp: a small IPP (RFC 8010/8011) client for the local CUPS server, in pure C with no
 * dependencies beyond src/base and POSIX sockets (port of crates/ipp).
 *
 * It does in-process what `lp` does: list the printers, read the default, and submit a PDF as a
 * job, over the CUPS domain socket (`/run/cups/cups.sock`, or `CUPS_SERVER` when it names a path)
 * or TCP (`localhost:631`, or `CUPS_SERVER` as `host[:port]`). PhotoCraft can then print where
 * there is no `lp` binary (a Flatpak with `--socket=cups`, a minimal container).
 *
 * Never crashes: every read is bounded and every malformed reply is an error (REWRITE_PLAN rule
 * 9). Errors map onto pc_status: an unreachable server is PC_ERR_IO, a broken HTTP/IPP reply is
 * PC_ERR_FORMAT, and an IPP error status is PC_ERR_INVALID (its code and message are in the
 * pc_error message, and pc_ipp_* that need the numeric code read it through an out-param). */
#ifndef PC_IPP_IPP_H
#define PC_IPP_IPP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "base/str.h"

/* ---- endpoint ----------------------------------------------------------------------------- */

typedef enum pc_ipp_kind {
    PC_IPP_UNIX, /* a Unix domain socket (CUPS's default, also `--socket=cups`) */
    PC_IPP_TCP   /* a TCP host and port */
} pc_ipp_kind;

/* Where the print server listens. `host` is owned: for PC_IPP_UNIX it is the socket path, for
 * PC_IPP_TCP the host (which may carry an IPv6 zone id, `fe80::1%eth0`). Clear with
 * pc_ipp_endpoint_fini. A zeroed pc_ipp_endpoint is valid and empty. */
typedef struct pc_ipp_endpoint {
    pc_ipp_kind kind;
    pc_str host;
    uint16_t port;
} pc_ipp_endpoint;

void pc_ipp_endpoint_fini(pc_ipp_endpoint *ep);

/* Parse `server` as a socket path (starts with `/`) or `host[:port]` (port 631 by default). An
 * IPv6 server is `[address]` or `[address]:port`; written bare (more than one `:`) it keeps the
 * default port. A mistyped bracketed endpoint (`[::1]:abc`) keeps the whole string as the host so
 * it stays unresolvable rather than quietly becoming the default one. Never fails but for OOM. */
pc_status pc_ipp_endpoint_parse(const char *server, pc_ipp_endpoint *out, pc_error *err);

/* The server this machine uses: `CUPS_SERVER`, else the CUPS socket, else localhost:631. Returns
 * true and fills *out when something answers, false (out untouched) when nothing does. */
bool pc_ipp_endpoint_detect(pc_ipp_endpoint *out);

/* Whether a connection opens. */
bool pc_ipp_endpoint_reachable(const pc_ipp_endpoint *ep);

/* The endpoint as an HTTP authority (an IPv6 host bracketed, a zone id "%25"-escaped), appended
 * to `out`. Used for the Host: header and the ipp:// printer-uri. */
pc_status pc_ipp_endpoint_host(const pc_ipp_endpoint *ep, pc_buf *out, pc_error *err);

/* The endpoint written for a human (an IPv6 host bracketed, the zone kept as typed), appended to
 * `out`. */
pc_status pc_ipp_endpoint_display(const pc_ipp_endpoint *ep, pc_buf *out, pc_error *err);

/* ---- printers ----------------------------------------------------------------------------- */

/* A printer the server knows. */
typedef struct pc_ipp_printer {
    pc_str name;
    pc_str info;          /* printer-info, the human description */
    uint32_t state;       /* printer-state: 3 idle, 4 printing, 5 stopped */
    bool accepting_jobs;
    bool is_default;
    bool is_class;        /* a CUPS class (a pool of printers) rather than one printer */
} pc_ipp_printer;

typedef struct pc_ipp_printer_list {
    pc_ipp_printer *items;
    size_t count;
    size_t cap;
} pc_ipp_printer_list;

void pc_ipp_printer_list_fini(pc_ipp_printer_list *list);

/* The printers the server knows (CUPS-Get-Printers), then its classes (CUPS-Get-Classes), the
 * default one flagged. A server with no printers answers "not found": that is an empty list, not
 * an error. */
pc_status pc_ipp_printers(const pc_ipp_endpoint *ep, pc_ipp_printer_list *out, pc_error *err);

/* The server's default printer (CUPS-Get-Default). On success *present says whether there is one;
 * when true, *out holds its name (owned; caller pc_str_fini). Absence is not an error. */
pc_status pc_ipp_default_printer(const pc_ipp_endpoint *ep, pc_str *out, bool *present, pc_error *err);

/* ---- printing ----------------------------------------------------------------------------- */

/* What to print. Borrowed fields; `document` is `format` (e.g. "application/pdf"). */
typedef struct pc_ipp_job {
    const char *printer;
    bool is_class;             /* `printer` names a class */
    const char *title;         /* shown in the queue */
    const char *user;
    uint32_t copies;
    const uint8_t *document;
    size_t document_len;
    const char *format;
} pc_ipp_job;

/* Submit `job` (Print-Job) and return its id in *out_job_id. */
pc_status pc_ipp_print_job(const pc_ipp_endpoint *ep, const pc_ipp_job *job, uint32_t *out_job_id,
                           pc_error *err);

#endif
