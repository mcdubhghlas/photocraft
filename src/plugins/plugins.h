/* Sandboxed WebAssembly filter plug-ins (port of crates/plugins).
 *
 * A plug-in is a WebAssembly module implementing the PhotoCraft plug-in ABI v1 (docs/plugins.md):
 *
 *   pc_abi_version() -> i32                      must return PC_PLUGIN_ABI_VERSION
 *   pc_manifest() -> i64                         (len << 32) | ptr of the UTF-8 JSON manifest
 *   pc_alloc(n: i32) -> i32                      a block of n bytes in linear memory (0 = failure)
 *   pc_filter(buf, len, w, h, channels, format, params_ptr, params_len) -> i32   0 = success
 *   memory                                       the exported linear memory
 *
 * It runs in WAMR's interpreter (REWRITE_PLAN.md D13; the reference used wasmi) with the same
 * sandbox: no host imports at all (a module importing anything is refused), an instruction budget
 * per call, a wall-clock budget per run, a linear-memory cap, a call-stack cap, wasmi's "strict"
 * module limits, and a fresh instance per band so no state leaks between calls. Every failure
 * (malformed module, trap, exhausted budget, bad output) is an error; nothing a module does can
 * crash or hang the host.
 *
 * Pixels are handed over as interleaved, normalised little-endian f32 samples in the document's
 * own colour model, in horizontal tile-aligned bands so the copy stays bounded.
 *
 * Errors. Messages are the reference's `Error` Display texts ("plug-in trapped: ...", "plug-in
 * exceeded its instruction budget", ...); pc_plugin_error_kind() recovers the variant. Codes:
 * Module/Abi/Manifest/Trap/Failed -> PC_ERR_FORMAT, Limit -> PC_ERR_LIMIT, Params ->
 * PC_ERR_INVALID, NotFound -> PC_ERR_NOT_FOUND, Io -> PC_ERR_IO; PC_ERR_NOMEM when the host runs
 * out of memory; PC_ERR_UNSUPPORTED ("plug-ins unavailable in this build") when PhotoCraft was
 * built with WITH_PLUGINS=0. Manifests, parameters and the registry work in every build.
 *
 * Thread safety: a loaded pc_plugin is immutable (apart from pc_plugin_set_source before it is
 * shared) and reference counted; any number of threads may apply it at once (each run gets its
 * own instances). The registry is process-wide and synchronised. */
#ifndef PC_PLUGINS_PLUGINS_H
#define PC_PLUGINS_PLUGINS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "base/buf.h"
#include "base/json.h"
#include "base/status.h"
#include "base/str.h"
#include "color/color.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* The plug-in ABI version this host implements. */
#define PC_PLUGIN_ABI_VERSION 1
/* Longest manifest the host reads, most parameters, largest overlap (pixels). */
#define PC_PLUGIN_MAX_MANIFEST_BYTES ((size_t)64 * 1024)
#define PC_PLUGIN_MAX_PARAMS 64
#define PC_PLUGIN_MAX_OVERLAP 256u

/* ---- errors ---- */

typedef enum pc_plugin_error_kind {
    PC_PLUGIN_E_NONE,      /* not a plug-in error (PC_OK, NOMEM, UNSUPPORTED, ...) */
    PC_PLUGIN_E_MODULE,    /* "not a valid plug-in module: ..." */
    PC_PLUGIN_E_ABI,       /* "plug-in ABI: ..." */
    PC_PLUGIN_E_MANIFEST,  /* "plug-in manifest: ..." */
    PC_PLUGIN_E_TRAP,      /* "plug-in trapped: ..." */
    PC_PLUGIN_E_LIMIT,     /* "plug-in exceeded its ..." */
    PC_PLUGIN_E_PARAMS,    /* "plug-in parameters: ..." */
    PC_PLUGIN_E_FAILED,    /* "plug-in failed: ..." */
    PC_PLUGIN_E_NOT_FOUND, /* "no plug-in \"id\" is installed" */
    PC_PLUGIN_E_IO         /* file-system errors (code PC_ERR_IO) */
} pc_plugin_error_kind;

/* Which reference Error variant err holds (from its code and message prefix). */
pc_plugin_error_kind pc_plugin_error_kind_of(const pc_error *err);
/* Fills err with "no plug-in {id:?} is installed" (PC_ERR_NOT_FOUND) and returns that code. */
pc_status pc_plugin_fail_not_found(pc_error *err, const char *id, size_t id_len);

/* ---- manifest ---- */

typedef enum pc_plugin_kind { PC_PLUGIN_KIND_FILTER } pc_plugin_kind;
/* Which pixels a filter is given: the layer's non-empty pixels, or the whole canvas. */
typedef enum pc_plugin_area { PC_PLUGIN_AREA_CONTENT, PC_PLUGIN_AREA_CANVAS } pc_plugin_area;
typedef enum pc_param_type { PC_PARAM_NUMBER, PC_PARAM_INT, PC_PARAM_BOOL, PC_PARAM_CHOICE } pc_param_type;

/* One declared parameter; the fields of its type are meaningful. */
typedef struct pc_param_spec {
    pc_str name;
    pc_param_type type;
    double min, max, def;     /* PC_PARAM_NUMBER */
    int64_t imin, imax, idef; /* PC_PARAM_INT (defaults -1e6, 1e6, 0) */
    bool bdef;                /* PC_PARAM_BOOL */
    pc_str *options;          /* PC_PARAM_CHOICE: n_options strings */
    size_t n_options;
    bool has_cdef; /* PC_PARAM_CHOICE: a default was given (cdef) */
    pc_str cdef;
} pc_param_spec;

typedef struct pc_plugin_manifest {
    pc_str id, name, version, description, author;
    pc_plugin_kind kind;
    pc_param_spec *params; /* declaration (dialog) order */
    size_t n_params;
    uint32_t overlap; /* pixels of context around each output pixel */
    pc_plugin_area area;
} pc_plugin_manifest;

/* Parses and validates manifest JSON (serde semantics and messages, prefixed "manifest: " for
 * JSON/type errors). `out` is written on success only; finalise it with _fini. Failures are
 * PC_PLUGIN_E_MANIFEST (PC_ERR_FORMAT) or PC_ERR_NOMEM. */
pc_status pc_plugin_manifest_parse(const uint8_t *bytes, size_t len, pc_plugin_manifest *out, pc_error *err);
void pc_plugin_manifest_fini(pc_plugin_manifest *m); /* NULL-safe */
pc_status pc_plugin_manifest_clone(const pc_plugin_manifest *m, pc_plugin_manifest *out, pc_error *err);
/* Appends the parameter notation used by the command registry and the generated dialogs:
 * {"amount":0..100=50,"mode":"a|b","mono":bool=false,"seed":int=0}. */
pc_status pc_plugin_manifest_params_notation(const pc_plugin_manifest *m, pc_buf *out, pc_error *err);
/* Validates user params (NULL or JSON null = {}) against the schema: missing keys take their
 * defaults, numbers are clamped into range, unknown keys are dropped; a wrong type is a
 * PC_PLUGIN_E_PARAMS error. *out receives a serde_json Value object (keys sorted). */
pc_status pc_plugin_manifest_resolve_params(const pc_plugin_manifest *m, yyjson_val *params,
                                            pc_json_value *out, pc_error *err);
/* The manifest as serde serialises it (field order, params as an object), owned by doc. NULL on
 * NOMEM. */
yyjson_mut_val *pc_plugin_manifest_to_json(const pc_plugin_manifest *m, yyjson_mut_doc *doc);
/* [A-Za-z0-9._-]{1,64}. */
bool pc_plugin_valid_id(const char *id, size_t len);

/* ---- loading and running ---- */

typedef struct pc_plugin_limits {
    size_t max_module_bytes;    /* largest module accepted (32 MiB) */
    size_t max_memory_bytes;    /* cap on the plug-in's linear memory (512 MiB) */
    size_t band_bytes;          /* target size of one band of f32 pixels (4 MiB) */
    uint64_t fuel_base;         /* instructions any call may run (50M) ... */
    uint64_t fuel_per_sample;   /* ... plus this many per sample for pc_filter (4000) */
    size_t max_recursion_depth; /* deepest call nesting (1024; at least 16) */
    uint64_t wall_time_ms;      /* wall-clock budget for one load or apply (60 s) */
} pc_plugin_limits;

pc_plugin_limits pc_plugin_limits_default(void);

/* true when this build can load and run plug-ins (WITH_PLUGINS=1). */
bool pc_plugins_available(void);

typedef struct pc_plugin pc_plugin;

/* Validates and loads a module: checks the ABI version and reads the manifest. limits NULL =
 * the defaults. *out holds one reference (release with pc_plugin_release). */
pc_status pc_plugin_load(const uint8_t *bytes, size_t len, const pc_plugin_limits *limits, pc_plugin **out,
                         pc_error *err);
pc_plugin *pc_plugin_retain(pc_plugin *p); /* NULL-safe; returns p */
void pc_plugin_release(pc_plugin *p);      /* NULL-safe */

/* Borrowed for the plug-in's lifetime. */
const pc_plugin_manifest *pc_plugin_get_manifest(const pc_plugin *p);
const char *pc_plugin_id(const pc_plugin *p);
const pc_plugin_limits *pc_plugin_get_limits(const pc_plugin *p);
size_t pc_plugin_size(const pc_plugin *p); /* module size in bytes */
/* Where the module was loaded from (NULL if not from a file). */
const char *pc_plugin_source(const pc_plugin *p);
/* Records the source path. Only before the plug-in is shared with other threads. */
pc_status pc_plugin_set_source(pc_plugin *p, const char *source, pc_error *err);

/* The pc_filter `format` argument: bits 0-7 the bit depth (8, 16, 32), bit 8 alpha, bits 16-23
 * the PSD colour-mode number (0 Bitmap, 1 Gray, 2 Indexed, 3 RGB, 4 CMYK, 7 Multichannel,
 * 8 Duotone, 9 Lab). */
uint32_t pc_plugin_format_code(pc_pixel_format f);

/* The rectangle a run writes: the layer's content grown by the overlap (or the canvas, for
 * area "canvas"), limited to the selection's bounds. */
pc_rect pc_plugin_output_area(const pc_plugin *p, const pc_surface *surface, pc_rect canvas,
                              const pc_surface *selection);

/* Runs the filter on `surface` with user params (validated against the manifest; NULL = {}) and
 * writes the new surface to *out (initialised on success only). `canvas` is the document bounds;
 * `selection` (coverage in channel 0, may be NULL) limits and feathers the result. Bands run in
 * parallel on the global pool; the first failing band (in band order) gives the error, and a
 * failure stops the sibling bands. The input is never modified. */
pc_status pc_plugin_apply(const pc_plugin *p, const pc_surface *surface, pc_rect canvas,
                          const pc_surface *selection, yyjson_val *params, pc_surface *out, pc_error *err);

/* ---- the process-wide registry ---- */

/* Installs p (the registry takes its own reference), replacing one with the same id. */
pc_status pc_plugins_install(pc_plugin *p, pc_error *err);
/* Loads bytes with the default limits and installs the plug-in; *out (optional) gets a
 * reference to it. */
pc_status pc_plugins_install_bytes(const uint8_t *bytes, size_t len, pc_plugin **out, pc_error *err);
/* The plug-in installed as id (a new reference), or NULL. */
pc_plugin *pc_plugins_get(const char *id);
/* Installed plug-ins sorted by id, each retained; free with pc_plugins_list_free. */
pc_status pc_plugins_list(pc_plugin ***out, size_t *n, pc_error *err);
void pc_plugins_list_free(pc_plugin **list, size_t n);
/* Uninstalls id; false if it wasn't installed. */
bool pc_plugins_remove(const char *id);
/* Changes whenever a plug-in is installed or removed (for caches such as menus). */
uint64_t pc_plugins_revision(void);

/* Reads a .wasm file (refusing anything that isn't a regular file or exceeds the module limit)
 * and loads it, recording the path as its source. */
pc_status pc_plugins_load_file(const char *path, const pc_plugin_limits *limits, pc_plugin **out,
                               pc_error *err);

typedef struct pc_plugins_failure {
    char *file;  /* path as displayed */
    char *error; /* the error's message */
} pc_plugins_failure;

typedef struct pc_plugins_folder_report {
    pc_str *loaded; /* ids installed */
    size_t n_loaded;
    pc_plugins_failure *failed; /* each .wasm that failed to load */
    size_t n_failed;
} pc_plugins_folder_report;

/* Installs every *.wasm (case-insensitive extension) directly in dir, in file-name order, at
 * most 1000. A bad module is reported and skipped. PC_ERR_IO when dir can't be read. */
pc_status pc_plugins_load_folder(const char *dir, pc_plugins_folder_report *out, pc_error *err);
void pc_plugins_folder_report_fini(pc_plugins_folder_report *r); /* NULL-safe */

#endif
