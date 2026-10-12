/* Private helpers shared by the src/plugins files. Not part of the public API. */
#ifndef PC_PLUGINS_INTERNAL_H
#define PC_PLUGINS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "plugins/plugins.h"

/* serde_json::from_str::<Manifest>: the streaming deserializer (manifest_de.c). On failure err
 * holds serde's message (with " at line L column C" where serde adds one), not yet prefixed. */
pc_status pc_plugin_manifest_deserialize(const char *text, size_t len, pc_plugin_manifest *out,
                                         pc_error *err);

/* Rust's `{:?}` of a str: quoted, escapes for \" \\ \0 \t \r \n and \u{..} for controls and the
 * other characters Rust's escape_debug escapes (format and separator characters). Appends to
 * out; best effort (a NOMEM truncates). */
void pc_plugin_debug_quote(const char *s, size_t len, pc_buf *out);

/* Fills err with the reference Error variant's Display text: "<variant prefix><detail>". */
void pc_plugin_fail_set(pc_error *err, pc_plugin_error_kind kind, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* pc_fail-style macros (they evaluate to the constant code, so analysers follow them). */
#define PC_PLUGIN_FAIL_MODULE(err, ...)                                                                      \
    (pc_plugin_fail_set((err), PC_PLUGIN_E_MODULE, __VA_ARGS__), PC_ERR_FORMAT)
#define PC_PLUGIN_FAIL_ABI(err, ...) (pc_plugin_fail_set((err), PC_PLUGIN_E_ABI, __VA_ARGS__), PC_ERR_FORMAT)
#define PC_PLUGIN_FAIL_MANIFEST(err, ...)                                                                    \
    (pc_plugin_fail_set((err), PC_PLUGIN_E_MANIFEST, __VA_ARGS__), PC_ERR_FORMAT)
#define PC_PLUGIN_FAIL_TRAP(err, ...)                                                                        \
    (pc_plugin_fail_set((err), PC_PLUGIN_E_TRAP, __VA_ARGS__), PC_ERR_FORMAT)
#define PC_PLUGIN_FAIL_LIMIT(err, ...)                                                                       \
    (pc_plugin_fail_set((err), PC_PLUGIN_E_LIMIT, __VA_ARGS__), PC_ERR_LIMIT)
#define PC_PLUGIN_FAIL_PARAMS(err, ...)                                                                      \
    (pc_plugin_fail_set((err), PC_PLUGIN_E_PARAMS, __VA_ARGS__), PC_ERR_INVALID)
#define PC_PLUGIN_FAIL_FAILED(err, ...)                                                                      \
    (pc_plugin_fail_set((err), PC_PLUGIN_E_FAILED, __VA_ARGS__), PC_ERR_FORMAT)
#define PC_PLUGIN_FAIL_NOMEM(err) pc_fail((err), PC_ERR_NOMEM, "out of memory running a plug-in")

/* ---- runtime (runtime.c) used by apply.c ---- */

/* When a run must stop: a wall-clock deadline and the first error of any band, shared by the
 * parallel bands of one load or apply. Running instances register so that an abort or the
 * deadline can terminate them. */
typedef struct pc_plugin_deadline pc_plugin_deadline;

pc_status pc_plugin_deadline_new(const pc_plugin_limits *limits, pc_plugin_deadline **out, pc_error *err);
/* Stops the watchdog and frees. NULL-safe. Every run using it must have finished. */
void pc_plugin_deadline_free(pc_plugin_deadline *d);
/* Publishes err as the run's outcome (first one wins) and stops every registered instance. */
void pc_plugin_deadline_abort(pc_plugin_deadline *d, const pc_error *err);

/* Runs pc_filter on data (interleaved f32, w * h * channels) in a fresh instance, in place. */
pc_status pc_plugin_filter_buffer(const pc_plugin *p, float *data, size_t n, uint32_t w, uint32_t h,
                                  uint32_t channels, uint32_t format, const uint8_t *params,
                                  size_t params_len, pc_plugin_deadline *d, pc_error *err);

#endif
