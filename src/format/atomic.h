/* Crash-safe file replacement for the format module (crates/format/src/atomic.rs).
 *
 * The implementation lives in base (src/base/file.h: pc_file_write_atomic / pc_atomic_write,
 * the failure-injection seam pc_atomic_write_with over a pc_atomic_fs, pc_atomic_retry_rename,
 * pc_atomic_temp_name), so every save in the tree shares one algorithm and one set of messages.
 * This header only keeps the module's historical include path working. */
#ifndef PC_FORMAT_ATOMIC_H
#define PC_FORMAT_ATOMIC_H

#include "base/file.h"

#endif
