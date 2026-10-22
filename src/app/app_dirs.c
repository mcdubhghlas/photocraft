/* See app_dirs.h. Behavioural port of apps/photocraft/src/app_dirs.rs. */
#include "app_dirs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/file.h"
#include "base/log.h"
#include "base/status.h"

/* Files beside the executable that switch on portable mode (either one; contents ignored). */
static const char *const PORTABLE_MARKERS[] = {"portable.txt", "PhotoCraft.portable"};

/* Join `a` and `b` with a '/' into out[cap]. Returns false (and leaves out empty) if it would not
 * fit; never writes past the buffer. */
static bool join(char *out, size_t cap, const char *a, const char *b)
{
    int n = snprintf(out, cap, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= cap) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return false;
    }
    return true;
}

/* Non-empty env value, or NULL. */
static const char *env_nonempty(pc_app_env_fn env, void *ctx, const char *name)
{
    const char *v = env ? env(ctx, name) : NULL;
    return (v && v[0] != '\0') ? v : NULL;
}

/* True if `path` names a regular file. */
static bool is_file(const char *path)
{
    pc_buf b = PC_BUF_INIT;
    pc_error err;
    /* Read zero bytes is enough to confirm it opens as a file; cap tiny to avoid large reads. */
    pc_status rc = pc_file_read(path, 0, &b, &err);
    pc_buf_fini(&b);
    return rc == PC_OK;
}

bool pc_app_platform_dir(pc_app_env_fn env, void *ctx, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    const char *home = env_nonempty(env, ctx, "HOME");
#if defined(__APPLE__)
    if (!home) {
        return false;
    }
    return join(out, cap, home, "Library/Application Support/Photocraft");
#elif defined(_WIN32)
    {
        const char *appdata = env_nonempty(env, ctx, "APPDATA");
        if (!appdata) {
            return false;
        }
        return join(out, cap, appdata, "Photocraft");
    }
#else
    {
        const char *xdg = env_nonempty(env, ctx, "XDG_CONFIG_HOME");
        char base[PC_APP_PATH_MAX];
        if (xdg) {
            int n = snprintf(base, sizeof base, "%s", xdg);
            if (n < 0 || (size_t)n >= sizeof base) {
                return false;
            }
        } else if (home) {
            if (!join(base, sizeof base, home, ".config")) {
                return false;
            }
        } else {
            return false;
        }
        return join(out, cap, base, "photocraft");
    }
#endif
}

/* The first portable marker file in exe_dir, into out[cap]; false when none. */
static bool portable_marker(const char *exe_dir, char *out, size_t cap)
{
    for (size_t i = 0; i < sizeof PORTABLE_MARKERS / sizeof PORTABLE_MARKERS[0]; i++) {
        char p[PC_APP_PATH_MAX];
        if (join(p, sizeof p, exe_dir, PORTABLE_MARKERS[i]) && is_file(p)) {
            int n = snprintf(out, cap, "%s", p);
            return n >= 0 && (size_t)n < cap;
        }
    }
    return false;
}

/* Create `dir` and prove a file can be written in it (a folder can exist yet be read-only). */
static bool ensure_writable(const char *dir)
{
    pc_error err;
    if (pc_mkdir_p(dir, &err) != PC_OK) {
        return false;
    }
    char probe[PC_APP_PATH_MAX];
    char name[64];
    (void)snprintf(name, sizeof name, ".write-test-%ld", (long)getpid());
    if (!join(probe, sizeof probe, dir, name)) {
        return false;
    }
    if (pc_file_write_atomic(probe, "", 0, &err) != PC_OK) {
        return false;
    }
    (void)unlink(probe); /* a leftover probe is harmless */
    return true;
}

static void set_dir(pc_app_dirs *out, pc_app_dir_mode mode, const char *dir)
{
    out->mode = mode;
    if (dir && dir[0] != '\0') {
        int n = snprintf(out->dir, sizeof out->dir, "%s", dir);
        out->has_dir = n >= 0 && (size_t)n < sizeof out->dir;
    } else {
        out->has_dir = false;
        out->dir[0] = '\0';
    }
}

void pc_app_dirs_resolve(pc_app_dirs *out, pc_app_env_fn env, void *ctx, const char *exe_dir)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->mode = PC_APP_DIR_PLATFORM;

    const char *override = env_nonempty(env, ctx, "PHOTOCRAFT_CONFIG_DIR");
    if (override) {
        set_dir(out, PC_APP_DIR_OVERRIDE, override);
        return;
    }

    char platform[PC_APP_PATH_MAX];
    bool has_platform = pc_app_platform_dir(env, ctx, platform, sizeof platform);

    char marker[PC_APP_PATH_MAX];
    if (!exe_dir || exe_dir[0] == '\0' || !portable_marker(exe_dir, marker, sizeof marker)) {
        set_dir(out, PC_APP_DIR_PLATFORM, has_platform ? platform : NULL);
        return;
    }

    char data[PC_APP_PATH_MAX];
    if (!join(data, sizeof data, exe_dir, PC_APP_PORTABLE_DATA_DIR)) {
        set_dir(out, PC_APP_DIR_PLATFORM, has_platform ? platform : NULL);
        return;
    }
    if (ensure_writable(data)) {
        set_dir(out, PC_APP_DIR_PORTABLE, data);
        return;
    }
    /* Portable marker found but the data folder is not writable: fall back with a warning. */
    set_dir(out, PC_APP_DIR_PLATFORM, has_platform ? platform : NULL);
    out->has_warning = true;
    (void)snprintf(out->warning, sizeof out->warning,
                   "portable mode (%.120s found) but %.120s isn't writable; settings are kept in "
                   "%.120s instead",
                   marker, data, has_platform ? platform : "nowhere (no user folder found)");
}

bool pc_app_exe_dir(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return false;
    }
    char path[PC_APP_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0 || (size_t)n >= sizeof path) {
        return false;
    }
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) {
        return false;
    }
    if (slash == path) {
        slash[1] = '\0'; /* the root directory */
    } else {
        *slash = '\0';
    }
    int m = snprintf(out, cap, "%s", path);
    return m >= 0 && (size_t)m < cap;
}

static const char *getenv_fn(void *ctx, const char *name)
{
    (void)ctx;
    return getenv(name);
}

const pc_app_dirs *pc_app_dirs_current(void)
{
    static pc_app_dirs cached;
    static bool done = false;
    if (!done) {
        char exe_dir[PC_APP_PATH_MAX];
        bool has_exe = pc_app_exe_dir(exe_dir, sizeof exe_dir);
        pc_app_dirs_resolve(&cached, getenv_fn, NULL, has_exe ? exe_dir : NULL);
        if (cached.has_warning) {
            pc_log(PC_LOG_WARN, "%s", cached.warning);
        }
        pc_log(PC_LOG_INFO, "settings directory (mode %d): %s", (int)cached.mode,
               cached.has_dir ? cached.dir : "(none)");
        done = true;
    }
    return &cached;
}

const char *pc_app_config_dir(void)
{
    const pc_app_dirs *d = pc_app_dirs_current();
    return d->has_dir ? d->dir : NULL;
}
