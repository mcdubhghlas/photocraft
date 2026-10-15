/* Persistent preferences (Edit > Preferences), keyboard-shortcut, menu and toolbar customisation
 * (port of crates/engine/src/prefs.rs, layer L5).
 *
 * The whole set is plain JSON data owned by the session (pc_engine_prefs), kept as one yyjson
 * document so every frontend reads the same values and agents change them through prefs.get /
 * prefs.set / prefs.reset. The engine never touches the filesystem for preferences; frontends
 * persist pc_engine_session_prefs_to_json and restore it with pc_engine_session_load_prefs_json.
 *
 * Preferences are user/disk data: a corrupt settings file must produce an error or be ignored,
 * never a crash (AGENTS.md "Never crash"). Unknown keys are dropped and missing keys default, so
 * files from older and newer versions load. */
#ifndef PC_ENGINE_PREFS_H
#define PC_ENGINE_PREFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/json.h"
#include "base/status.h"

/* ---- length units (port of prefs::Unit) --------------------------------------------------- */

typedef enum pc_pref_unit {
    PC_UNIT_PIXELS,
    PC_UNIT_INCHES,
    PC_UNIT_CENTIMETERS,
    PC_UNIT_MILLIMETERS,
    PC_UNIT_POINTS,
    PC_UNIT_PICAS,
    PC_UNIT_PERCENT
} pc_pref_unit;

/* Document pixels <-> this unit. dpi: document resolution; extent: 100% length (percent);
 * ppi: points per inch (72 PostScript, 72.27 traditional). */
double pc_pref_unit_from_px(pc_pref_unit u, double px, double dpi, double extent, double ppi);
double pc_pref_unit_to_px(pc_pref_unit u, double v, double dpi, double extent, double ppi);
const char *pc_pref_unit_suffix(pc_pref_unit u);
size_t pc_pref_unit_decimals(pc_pref_unit u);
/* The unit a typed suffix names (any case); false when unknown. */
bool pc_pref_unit_from_suffix(const char *suffix, size_t len, pc_pref_unit *out);

/* Round v to `decimals` places (half away from zero) and drop trailing zeros, into out[cap].
 * Returns the string length (never exceeds cap-1; always NUL-terminated). */
size_t pc_pref_fmt_decimals(double v, size_t decimals, char *out, size_t cap);

/* "#rrggbb" -> three bytes; false when malformed. */
bool pc_pref_parse_hex(const char *s, size_t len, uint8_t out[3]);

/* ---- pen pressure curve (port of prefs::PressureCurve) ------------------------------------- */

enum { PC_PRESSURE_CURVE_MAX_POINTS = 16 };

/* A monotone cubic through sanitised control points. Build once, evaluate many times. */
typedef struct pc_pressure_curve {
    size_t n;
    float xs[PC_PRESSURE_CURVE_MAX_POINTS];
    float ys[PC_PRESSURE_CURVE_MAX_POINTS];
    float ms[PC_PRESSURE_CURVE_MAX_POINTS]; /* tangents */
} pc_pressure_curve;

/* Build from `npts` [input, output] points (points[i][0], points[i][1]); hostile input is
 * sanitised (non-finite dropped, clamped to 0..1, sorted, deduped, capped, outputs monotone). */
void pc_pressure_curve_init(pc_pressure_curve *c, const float (*points)[2], size_t npts);
/* The curve's output for pen pressure x (0..1; clamped, NaN -> 0). */
float pc_pressure_curve_eval(const pc_pressure_curve *c, float x);
/* The identity (default) curve: pressure passes through unchanged. */
bool pc_pressure_curve_is_linear(const pc_pressure_curve *c);

/* ---- keyboard shortcuts (port of prefs::normalize_shortcut / conflicts) -------------------- */

/* Canonical spelling of a shortcut (modifiers in Cmd+Ctrl+Alt+Shift order, key last). Writes
 * into out[cap] and returns true, or returns false when it has no key or an unknown modifier. */
bool pc_pref_normalize_shortcut(const char *s, size_t len, char *out, size_t cap);

/* ---- enumerated/ranged metadata (port of prefs::choices / range / is_color) ---------------- */

/* The valid names of an enumerated preference at `path`, or NULL with *n untouched. Borrowed. */
const char *const *pc_pref_choices(const char *path, size_t *n);
/* The [lo,hi] range of a numeric preference at `path`; false when it has none. */
bool pc_pref_range(const char *path, double *lo, double *hi);
/* Is the preference at `path` a "#rrggbb" colour? */
bool pc_pref_is_color(const char *path);
/* Is the preference at `path` hidden from the Preferences dialog (unimplemented or Linux-only)? */
bool pc_pref_is_hidden(const char *path);

/* ---- session-level access (used by other engine chunks and apply_prefs) -------------------- */

typedef struct pc_engine_session pc_engine_session;

/* Read a preference by dotted path ("" = everything) into *out (owned). PC_ERR_NOT_FOUND when the
 * path is unknown. */
pc_status pc_engine_prefs_get(const pc_engine_session *s, const char *path, pc_json_value *out,
                              pc_error *err);
/* History limits derived from the performance section (apply to every open and new document). */
void pc_engine_prefs_history_limits(const pc_engine_session *s, size_t *max_states, size_t *max_bytes);
/* Push engine-relevant preferences into live state (history limits, effect-cache budget). */
void pc_engine_apply_prefs(pc_engine_session *s);
/* Everything persisted as one pretty JSON document (caller frees the returned string). NULL on
 * OOM. */
char *pc_engine_session_prefs_to_json(const pc_engine_session *s);
/* Restore preferences saved by pc_engine_session_prefs_to_json. Missing keys keep their defaults,
 * unknown keys are ignored. PC_ERR_INVALID (not a crash) on malformed JSON. */
pc_status pc_engine_session_load_prefs_json(pc_engine_session *s, const char *text, size_t len,
                                            pc_error *err);

#endif /* PC_ENGINE_PREFS_H */
