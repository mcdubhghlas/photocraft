/* Per-monitor ICC profile resolution for the colour-managed canvas (Edit > Color Settings >
 * Monitor Profile = auto, #569).
 *
 * Behavioural port of the Linux-relevant, pure parts of apps/photocraft/src/monitor_profile.rs:
 * the bounded RGB-only ICC validation, reading an ICC file from disk, the colord/XDG on-disk ICC
 * store search path, and the unambiguous clone-profile selection. These are tested headlessly on
 * every platform and feed the engine's display colour model (src/engine/display_color.h).
 *
 * The live mapping from a monitor to its assigned profile touches the session:
 *   - X11: the colord D-Bus service (GetDevicesByKind "display" -> XRANDR_name metadata ->
 *     GetProfileForQualifiers -> Filename) and/or the RandR _ICC_PROFILE output atom. Both need a
 *     link dependency this build does not yet carry (libdbus / libxcb); SDL3 provides neither.
 *     Enumeration is therefore left to a follow-up (REWRITE_PLAN P2) and only the pure building
 *     blocks are ported here.
 *   - Native Wayland (including XWayland windows from a Wayland session) reports no display
 *     profiles, exactly as the reference does, to avoid double-transforming a compositor-managed
 *     output (decision in the reference module docs).
 *   - macOS ColorSync is deferred (decision D2) and not ported.
 *
 * Nothing crashes: every entry validates its input; hostile ICC bytes are rejected, not trusted. */
#ifndef PC_APP_MONITOR_PROFILE_H
#define PC_APP_MONITOR_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_dirs.h"
#include "base/buf.h"
#include "base/status.h"

/* Largest display ICC profile accepted (matches the reference 64 MiB cap). */
#define PC_MONITOR_MAX_ICC_BYTES (64u * 1024u * 1024u)

/* Validate `len` bytes as a display ICC profile: within bounds (>= 132, <= cap), the 'acsp'
 * signature at offset 36, a declared size (big-endian u32 at offset 0) in [132, len], and, once
 * parsed over the declared bytes, an RGB colour space. On PC_OK *out_valid_len is the declared
 * length to keep (<= len); otherwise an error (PC_ERR_FORMAT / PC_ERR_LIMIT / PC_ERR_UNSUPPORTED).
 * out_valid_len may be NULL. */
pc_status pc_monitor_icc_validate(const uint8_t *bytes, size_t len, size_t *out_valid_len,
                                  pc_error *err);

/* Read an ICC file at `path` (bounded by the cap), validate it, and leave the kept bytes in *out
 * (truncated to the declared length). *out is replaced on success and must be pc_buf_fini'd by the
 * caller. Any read or validation failure returns an error and leaves *out untouched. */
pc_status pc_monitor_read_icc(const char *path, pc_buf *out, pc_error *err);

/* The directories colord and the XDG spec keep display ICC profiles in, most specific first:
 * $XDG_DATA_HOME/icc (or $HOME/.local/share/icc), each $XDG_DATA_DIRS entry's /icc, then the system
 * stores (/var/lib/colord/icc, /usr/local/share/color/icc, /usr/share/color/icc). Writes up to
 * `max` NUL-terminated paths into out[][PC_APP_PATH_MAX] and returns how many. `env` may be NULL. */
size_t pc_monitor_icc_search_dirs(pc_app_env_fn env, void *ctx, char out[][PC_APP_PATH_MAX],
                                  size_t max);

/* Clone selection: the one profile shared by every output of a cloned CRTC, by content hash. Each
 * output i has counts[i] candidate hashes at candidates[i]. Returns true (and *out_hash) only when
 * every output has at least one candidate, every candidate of each output is identical, and all
 * outputs agree on that one hash; false (ambiguous: a missing, conflicting or differing profile)
 * otherwise. Mirrors the reference's unambiguous_clone_profile. */
bool pc_monitor_clone_unambiguous(const uint64_t *const candidates[], const size_t counts[],
                                  size_t n_outputs, uint64_t *out_hash);

/* The display ICC profile colord has assigned to the X11 output named `output_name` (e.g. "DP-1"):
 * the device whose XRANDR_name metadata matches, its default ("*") profile's Filename, read and
 * RGB-validated with pc_monitor_read_icc. On PC_OK *out holds the profile bytes (caller pc_buf_fini's
 * it) when colord has one, or is left empty when it has none or output_name is NULL/empty. A build
 * without libdbus, or an unreachable colord/system-bus, returns PC_ERR_UNSUPPORTED (the caller then
 * uses the sRGB fallback); a NULL `out` returns PC_ERR_INVALID. Never crashes. */
pc_status pc_monitor_colord_profile(const char *output_name, pc_buf *out, pc_error *err);

/* The display ICC profile published on the X11 root window as the _ICC_PROFILE (atom_index 0) or
 * _ICC_PROFILE_<atom_index> atom (the "ICC Profiles in X" convention), read over xcb and bounded,
 * RGB-validated with pc_monitor_icc_validate. On PC_OK *out holds the kept bytes (caller pc_buf_fini's
 * it) when an atom is present and valid, or is left empty otherwise. A build without libxcb, or no
 * X11 display, returns PC_ERR_UNSUPPORTED; a NULL `out` returns PC_ERR_INVALID. Never crashes. */
pc_status pc_monitor_randr_atom_profile(uint32_t atom_index, pc_buf *out, pc_error *err);

/* Resolve the display ICC profile for an X11 output: colord's default profile for `output_name`
 * first (skipped when output_name is NULL), then the RandR _ICC_PROFILE[_atom_index] root atom.
 * Always degrades gracefully: on PC_OK *out holds the first profile found (caller pc_buf_fini's it)
 * or is left empty when none is available (the caller then uses sRGB). Returns PC_ERR_INVALID only
 * for a NULL `out`. Never crashes. */
pc_status pc_monitor_profile_for_output(const char *output_name, uint32_t atom_index, pc_buf *out,
                                        pc_error *err);

#endif /* PC_APP_MONITOR_PROFILE_H */
