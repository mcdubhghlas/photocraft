/* Headless JSON-lines server (port of rpc.rs): the control protocol's envelope
 * ({"id","method","params"} -> {"id","ok","result"|"error"}) over a pc_headless session, on stdio
 * or a loopback TCP port. poll()-based (REWRITE_PLAN.md D14). */
#ifndef PC_AUTOMATION_RPC_H
#define PC_AUTOMATION_RPC_H

#include <yyjson.h>

#include "automation/headless.h"
#include "base/status.h"

/* Serve JSON lines from in_fd to out_fd until EOF. Blank lines are ignored; an over-long or
 * non-UTF-8 line gets an error reply and is skipped. Single session, single thread. */
pc_status pc_auto_serve_lines(pc_headless *h, int in_fd, int out_fd, pc_error *err);

/* Serve authenticated JSON lines on a loopback TCP address with one shared session. `ready` (may
 * be NULL) is called with the bound port once listening. Caps connections and request bytes. */
pc_status pc_auto_serve_tcp(const char *addr, pc_headless *h, const char *token,
                            void (*ready)(int port, void *ud), void *ud, pc_error *err);

#endif /* PC_AUTOMATION_RPC_H */
