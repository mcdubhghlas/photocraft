/* Crash-safe GPU startup.
 *
 * A graphics driver that faults while the GL context / compositor comes up cannot be caught
 * in-process. So before touching the driver the app writes a marker (gpu-starting.json in the config
 * directory) and holds an advisory lock on it; it clears the marker once the first frames rendered.
 * If the next launch finds a marker that no running process holds, the previous start died in the
 * driver, and this one retries with the next safer backend (ending at the CPU/software path).
 *
 * Behavioural port of apps/photocraft/src/gpu_startup.rs. The wgpu-specific parts (backend bitsets,
 * adapter ranking, the DX12 shader-compiler choice, indirect-validation skips) are NOT ported: the
 * C desktop app uses SDL3 + OpenGL only (REWRITE_PLAN.md §7, D2/D19). What remains is the planning
 * and the crash-detecting sentinel, which stay faithful and testable. For the SDL/GL stack a plan
 * of PC_APP_GPU_CPU means "software / safe rendering"; any other backend means "use the GPU". */
#ifndef PC_APP_GPU_STARTUP_H
#define PC_APP_GPU_STARTUP_H

#include <stdbool.h>
#include <stddef.h>

#include "app_dirs.h"
#include "base/status.h"

/* The marker's file name in the config directory. */
#define PC_APP_GPU_MARKER_FILE "gpu-starting.json"

typedef enum pc_app_gpu_backend {
    PC_APP_GPU_AUTO,
    PC_APP_GPU_VULKAN,
    PC_APP_GPU_DX12,
    PC_APP_GPU_METAL,
    PC_APP_GPU_GL,
    PC_APP_GPU_CPU
} pc_app_gpu_backend;

typedef enum pc_app_render_mode { PC_APP_RENDER_AUTO, PC_APP_RENDER_GPU, PC_APP_RENDER_CPU } pc_app_render_mode;

typedef enum pc_app_gpu_os { PC_APP_GPU_OS_WINDOWS, PC_APP_GPU_OS_MAC, PC_APP_GPU_OS_OTHER } pc_app_gpu_os;

bool pc_app_gpu_backend_parse(const char *s, pc_app_gpu_backend *out);
const char *pc_app_gpu_backend_name(pc_app_gpu_backend b);
bool pc_app_render_mode_parse(const char *s, pc_app_render_mode *out);
pc_app_gpu_os pc_app_gpu_os_current(void);

/* A backend the platform has (others fall back to PC_APP_GPU_AUTO). */
pc_app_gpu_backend pc_app_gpu_sanitize(pc_app_gpu_backend pref, pc_app_gpu_os os);
/* The next safer backend after a start on `tried` crashed. */
pc_app_gpu_backend pc_app_gpu_next_safer(pc_app_gpu_backend tried, pc_app_gpu_os os);

/* What a start records before touching the driver. */
typedef struct pc_app_gpu_marker {
    char backend[64];         /* "auto","gl",...; or "env:<value>" */
    char adapter[256];
    char adapter_backend[64];
    char driver[256];
    char version[64];
} pc_app_gpu_marker;

/* Compact JSON of `m` into out[cap] (truncated to fit; always NUL-terminated). */
void pc_app_gpu_marker_to_json(const pc_app_gpu_marker *m, char *out, size_t cap);
/* Parse leniently: anything unreadable yields a marker of an unknown backend. */
void pc_app_gpu_marker_parse(const char *text, size_t len, pc_app_gpu_marker *out);
/* The backend the crashed start was using. false for a start under an env override. */
bool pc_app_gpu_marker_tried(const pc_app_gpu_marker *m, pc_app_gpu_backend *out);

/* The backend this launch uses and why. */
typedef struct pc_app_gpu_plan {
    pc_app_gpu_backend backend;
    bool has_env;
    char env[128];
    bool has_reason;
    char reason[256];
    bool remember; /* store `backend` in the preferences once the start succeeded */
} pc_app_gpu_plan;

/* `crashed` may be NULL; `env` may be NULL (the WGPU_BACKEND-style override value). */
void pc_app_gpu_compute_plan(pc_app_gpu_backend pref, const pc_app_gpu_marker *crashed, const char *env,
                         bool safe_gpu, pc_app_gpu_os os, pc_app_gpu_plan *out);
void pc_app_gpu_plan_with_mode(pc_app_gpu_backend pref, pc_app_render_mode mode, const pc_app_gpu_marker *crashed,
                           const char *env, bool safe_gpu, pc_app_gpu_os os, pc_app_gpu_plan *out);
bool pc_app_gpu_keep_marker_after_error(const pc_app_gpu_plan *plan, pc_app_gpu_os os);

/* Read the rendering policy leniently from a preferences.json file (NULL/missing => auto). */
void pc_app_gpu_read_rendering_prefs(const char *prefs_path, pc_app_gpu_backend *backend, pc_app_render_mode *mode);

/* What a previous start left behind. */
typedef enum pc_app_gpu_prev { PC_APP_GPU_PREV_CLEAN, PC_APP_GPU_PREV_CRASHED, PC_APP_GPU_PREV_BUSY } pc_app_gpu_prev;

/* The locked marker file of this start. */
typedef struct pc_app_gpu_sentinel {
    int fd;
    bool open;
    char path[PC_APP_PATH_MAX];
    pc_app_gpu_marker marker;
} pc_app_gpu_sentinel;

/* Open (creating) and lock the marker in `dir`. Returns the previous outcome; when it is
 * PC_APP_GPU_PREV_CRASHED, *prev_marker is filled. *created is set true and *sent initialised when this
 * start owns the sentinel (not when another instance holds the lock or the dir is unwritable). */
pc_app_gpu_prev pc_app_gpu_sentinel_begin(const char *dir, pc_app_gpu_sentinel *sent, bool *created,
                                  pc_app_gpu_marker *prev_marker);
/* Replace the marker's contents with `m`. */
pc_status pc_app_gpu_sentinel_write(pc_app_gpu_sentinel *s, const pc_app_gpu_marker *m, pc_error *err);
/* The start succeeded: clear the marker and release the lock. */
void pc_app_gpu_sentinel_finish(pc_app_gpu_sentinel *s);
/* Release the lock but keep the (non-empty) marker, so the next start tries a safer backend. */
void pc_app_gpu_sentinel_release(pc_app_gpu_sentinel *s);

#endif /* PC_APP_GPU_STARTUP_H */
