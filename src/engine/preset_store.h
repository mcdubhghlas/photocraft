/* Persistent brush-preset store (port of crates/engine/src/preset_store.rs, layer L5).
 *
 * User and imported (.abr) brush presets, and their sampled tips, survive a restart in a directory
 * next to the preferences:
 *   - <group>-<hash>.pcbrushes : one JSON file per group holding the presets' settings, every
 *     bitmap replaced by a reference to a tip file plus its size and a small preview's tip file;
 *   - tips/<hash>.pctip        : content-addressed bitmaps, deflated (8 bits per sample when the
 *     tip came from 8-bit data, 16 otherwise);
 *   - index.json               : group order, library (panel) order, and deleted built-ins.
 *
 * Built-in presets are never written (regenerated on start). Every write is atomic and bounded;
 * a corrupt or oversized file is skipped with a warning and left on disk, never a crash.
 *
 * A session has no store by default (headless CLI/MCP and tests stay hermetic). The desktop app
 * loads one (pc_engine_preset_store_open_dir) and attaches it (pc_engine_session_attach_preset_store).
 *
 * The Actions list (actions.json) shares the directory in the reference; it needs E12 actions,
 * which the C session does not have yet, so it is a documented stub here (see preset_store.c). */
#ifndef PC_ENGINE_PRESET_STORE_H
#define PC_ENGINE_PRESET_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "paint/presets.h"
#include "paint/tile.h"

struct pc_engine_session;
struct pc_engine_preset_store; /* opaque; defined in preset_store.c (Session field type) */

/* ---- limits (mirror the reference) -------------------------------------------------------- */
#define PC_PRESET_GROUP_EXT    "pcbrushes"
#define PC_PRESET_TIP_EXT      "pctip"
#define PC_PRESET_TIPS_DIR     "tips"
#define PC_PRESET_INDEX_FILE   "index.json"
#define PC_PRESET_MAX_GROUP_BYTES   (16ull << 20)
#define PC_PRESET_MAX_TIP_BYTES     (64ull << 20)
#define PC_PRESET_MAX_INDEX_BYTES   (4ull << 20)
#define PC_PRESET_MAX_STORE_BYTES   (2ull << 30)
#define PC_PRESET_MAX_TIP_SIDE      16384u
#define PC_PRESET_MAX_TIP_SAMPLES   (1ull << 26)
#define PC_PRESET_MAX_GROUPS        4096u
#define PC_PRESET_MAX_PRESETS_PER_GROUP 50000u
/* Default byte budget of decoded full-size tips kept for reuse. */
#define PC_PRESET_TIP_CACHE_BYTES   (256ull << 20)

/* ---- owned string vector (Rust Vec<String>) ----------------------------------------------- */
typedef struct pc_preset_strs {
    char **v; /* each NUL-terminated, owned */
    size_t len;
    size_t cap;
} pc_preset_strs;

void pc_preset_strs_fini(pc_preset_strs *s); /* NULL-safe; frees each string and the array */

/* ---- backend ------------------------------------------------------------------------------ */
typedef struct pc_preset_file {
    char *name;    /* owned ("name.pcbrushes" or "tips/x.pctip") */
    uint64_t size;
} pc_preset_file;

typedef struct pc_preset_backend pc_preset_backend;
struct pc_preset_backend {
    /* Every file with its byte size (top level plus tips/). A missing store is empty, not error. */
    pc_status (*list)(pc_preset_backend *b, pc_preset_file **out, size_t *n, pc_error *err);
    /* Read a file into *out (replacing it), failing when larger than max bytes. */
    pc_status (*read)(pc_preset_backend *b, const char *name, uint64_t max, pc_buf *out, pc_error *err);
    /* Replace a file atomically. */
    pc_status (*write)(pc_preset_backend *b, const char *name, const uint8_t *data, size_t len, pc_error *err);
    /* Delete a file; deleting a missing file succeeds. */
    pc_status (*remove)(pc_preset_backend *b, const char *name, pc_error *err);
    void (*destroy)(pc_preset_backend *b); /* frees the backend */
};

/* An in-memory backend (tests, and frontends without a file system). Owns its files. */
pc_preset_backend *pc_preset_mem_backend_new(void);
/* A store directory on disk (desktop). Copies root. */
pc_preset_backend *pc_preset_dir_backend_new(const char *root);

/* ---- a loaded store ----------------------------------------------------------------------- */
typedef struct pc_engine_preset_opened {
    struct pc_engine_preset_store *store; /* owned until attached (then owned by the session) */
    pc_paint_preset_list presets;         /* user and imported, in group order */
    pc_preset_strs hidden_builtins;       /* built-in presets the user deleted (by name) */
    pc_preset_strs order;                 /* saved library order (preset names), empty = default */
    pc_preset_strs warnings;              /* files skipped (corrupt, oversized, missing tips) */
} pc_engine_preset_opened;

/* Load a store (takes ownership of `backend`; destroys it on its own failure paths). Never fails:
 * problems become warnings. *out is filled (zero it first is not required). */
void pc_engine_preset_store_open(pc_preset_backend *backend, pc_engine_preset_opened *out);
/* Load a store directory (desktop). */
void pc_engine_preset_store_open_dir(const char *root, pc_engine_preset_opened *out);
/* Release an Opened whose store/presets were NOT attached (frees everything it holds). NULL-safe. */
void pc_engine_preset_opened_fini(pc_engine_preset_opened *o);

/* Free a store (the Session field type). NULL-safe. */
void pc_engine_preset_store_free(struct pc_engine_preset_store *st);
/* Total bytes of the files the store owns. */
uint64_t pc_engine_preset_store_bytes(const struct pc_engine_preset_store *st);
/* Warnings since the last call: *out gets owned strings (free each, then it); returns the count. */
size_t pc_engine_preset_store_take_warnings(struct pc_engine_preset_store *st, char ***out);

/* ---- session glue ------------------------------------------------------------------------- */
/* Attach a loaded store: merge its presets into the library (a stored preset replaces the built-in
 * of the same name; presets created before loading win), hide deleted built-ins, apply the saved
 * order, then sync. Consumes `opened` (store moves into the session; the rest is freed). Load
 * warnings plus sync warnings are returned in *warnings_out (owned). */
pc_status pc_engine_session_attach_preset_store(struct pc_engine_session *s,
                                                pc_engine_preset_opened *opened,
                                                pc_preset_strs *warnings_out, pc_error *err);
/* Load the stored tips of `b` so it paints with its full bitmaps (a picked preset's settings, or an
 * export). Tips already loaded or embedded are left alone. Fails when a tip can't be loaded. */
pc_status pc_engine_session_load_brush_tips(const struct pc_engine_session *s, pc_paint_brush *b,
                                            pc_error *err);

/* ---- primitives (exposed for tests and the accessors) ------------------------------------- */
/* "PCTIP1", bits (8|16), width, height (u32 LE), then zlib-deflated samples; appends to *out. */
pc_status pc_preset_encode_tip(const pc_paint_gray_tile *t, pc_buf *out, pc_error *err);
/* Inverse of pc_preset_encode_tip; malformed/truncated/oversized input is an error. Writes *out. */
pc_status pc_preset_decode_tip(const uint8_t *data, size_t len, pc_paint_gray_tile *out, pc_error *err);
/* Content hash of a tip: 32 lower-case hex digits plus NUL into out[33]. */
void pc_preset_tip_hash(const pc_paint_gray_tile *t, char out[33]);
/* A group's file name (sanitised prefix + hash of the exact name); appends to *out. */
pc_status pc_preset_group_file_name(const char *group, pc_buf *out, pc_error *err);
/* Whether a generated store file name is safe (no path separators or ..). */
bool pc_preset_valid_name(const char *name);
/* "tips/<hash>.pctip" appended to *out. */
pc_status pc_preset_tip_file(const char *hash, pc_buf *out, pc_error *err);

#endif /* PC_ENGINE_PRESET_STORE_H */
