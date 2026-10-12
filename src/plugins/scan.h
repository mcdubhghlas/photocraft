/* A light walk over a WebAssembly module's sections (no validation; WAMR validates), for what the
 * reference's wasmi host enforced that WAMR doesn't: wasmi's "strict" EnforcedLimits (checked by
 * wasmi while it parses each section) and the StoreLimits applied at instantiation (tables,
 * table elements, the linear-memory cap), plus the module's first import and its start function.
 *
 * pc_wasm_rewrite then makes the module safe to hand to WAMR: the start section becomes an export
 * the host calls itself (under the instruction and time budgets; WAMR would run it unmetered
 * inside instantiation), and the exports WAMR calls on its own after instantiation
 * ("__wasm_call_ctors", "__post_instantiate") are renamed so they are not run (wasmi never runs
 * them). Pure functions; thread-safe. */
#ifndef PC_PLUGINS_SCAN_H
#define PC_PLUGINS_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

/* The export name the start function is re-exported under (contains a byte no toolchain emits). */
#define PC_WASM_START_EXPORT "\x01photocraft:start"

typedef struct pc_wasm_scan {
    bool walked;        /* every section header was in bounds */
    char limit_msg[96]; /* non-empty: the first wasmi EnforcedLimits error, in section order */
    /* First import (any kind): offsets into the module bytes. */
    bool has_import;
    size_t imp_module_off, imp_module_len, imp_name_off, imp_name_len;
    /* Linear memory 0 and tables, for the store limits. */
    uint32_t memory_count;
    uint64_t memory_min_pages;
    bool memory64;
    uint32_t table_count;
    uint64_t table_max_min; /* largest declared minimum size of a defined table */
    /* Start function and the sections the rewrite touches. */
    bool has_start;
    uint32_t start_func;
    size_t start_off, start_end; /* whole start section (id byte to end) */
    bool has_export;
    size_t export_off, export_end;
    size_t export_payload; /* offset of the export section's payload */
    bool needs_rewrite;    /* a start section or an export WAMR would call */
} pc_wasm_scan;

void pc_wasm_scan_module(const uint8_t *b, size_t len, pc_wasm_scan *out);

/* The module with the start section turned into the PC_WASM_START_EXPORT export and WAMR's
 * automatic exports renamed. *out (malloc'd) is a copy even when nothing changes. */
pc_status pc_wasm_rewrite(const uint8_t *b, size_t len, const pc_wasm_scan *s, uint8_t **out, size_t *out_len,
                          pc_error *err);

#endif
