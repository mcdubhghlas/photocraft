/* Where the desktop app keeps its per-user data: preferences, UI state, crash-recovery markers and
 * the log directory all live under the one directory pc_app_config_dir returns.
 *
 * Behavioural port of apps/photocraft/src/app_dirs.rs (clean-room). Resolution order:
 *   1. PHOTOCRAFT_CONFIG_DIR, when set and non-empty (tests, agents, custom setups).
 *   2. Portable mode: a marker file (PC_APP_PORTABLE_MARKERS) beside the executable puts everything
 *      in <exe dir>/PhotoCraftData. If that folder cannot be written, the app warns and falls back.
 *   3. The platform convention: Linux $XDG_CONFIG_HOME/photocraft or ~/.config/photocraft; macOS
 *      ~/Library/Application Support/Photocraft; Windows %APPDATA%\Photocraft.
 *
 * Nothing here crashes: a missing HOME/APPDATA simply yields no directory (has_dir == false). */
#ifndef PC_APP_DIRS_H
#define PC_APP_DIRS_H

#include <stdbool.h>
#include <stddef.h>

/* The product id, matching the .desktop file and the hicolor icon name. */
#define PC_APP_ID "ai.storyteller.photocraft"
/* The folder created beside the executable in portable mode. */
#define PC_APP_PORTABLE_DATA_DIR "PhotoCraftData"

/* Longest resolved directory path kept. Paths longer than this are treated as "no directory". */
#ifndef PC_APP_PATH_MAX
#define PC_APP_PATH_MAX 4096
#endif

/* How the directory was chosen. */
typedef enum pc_app_dir_mode {
    PC_APP_DIR_OVERRIDE,  /* PHOTOCRAFT_CONFIG_DIR */
    PC_APP_DIR_PORTABLE,  /* <exe dir>/PhotoCraftData */
    PC_APP_DIR_PLATFORM   /* the platform's per-user config directory */
} pc_app_dir_mode;

/* The resolved data directory. */
typedef struct pc_app_dirs {
    bool has_dir;                    /* false when no platform directory is known either */
    char dir[PC_APP_PATH_MAX];
    pc_app_dir_mode mode;
    bool has_warning;                /* set when a portable marker was found but unusable */
    char warning[512];
} pc_app_dirs;

/* An environment lookup, so tests can fake HOME/APPDATA/XDG_CONFIG_HOME. Returns NULL when unset. */
typedef const char *(*pc_app_env_fn)(void *ctx, const char *name);

/* Resolve the data directory from `env` and the executable's folder (`exe_dir` may be NULL). Pure:
 * touches the disk only for the portable write-probe. Never crashes. */
void pc_app_dirs_resolve(pc_app_dirs *out, pc_app_env_fn env, void *ctx, const char *exe_dir);

/* The platform convention from `env`; writes into out[cap] and returns true, or false when no user
 * folder is known (no HOME/APPDATA). */
bool pc_app_platform_dir(pc_app_env_fn env, void *ctx, char *out, size_t cap);

/* The executable's directory via /proc/self/exe (Linux). Writes into out[cap]; false on failure. */
bool pc_app_exe_dir(char *out, size_t cap);

/* Resolve once for this process (reads getenv + the real executable path). Never NULL. */
const pc_app_dirs *pc_app_dirs_current(void);

/* The per-user settings directory, or NULL when none is known. */
const char *pc_app_config_dir(void);

#endif /* PC_APP_DIRS_H */
