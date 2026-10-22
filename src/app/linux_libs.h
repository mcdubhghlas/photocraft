/* Linux start-up check for the shared libraries the windowing/GPU stack loads at runtime.
 *
 * The windowing (X11/Wayland, xkbcommon) and GPU (Vulkan, EGL) libraries are dlopen'd when the
 * window opens, not linked, so a missing one surfaces as a runtime failure. Before opening the
 * window we look for the libraries the current session type needs and, if one is conclusively
 * absent, print the package to install and exit non-zero instead of failing obscurely.
 *
 * No dlopen here: we read the dynamic linker's cache (ldconfig -p) and look in LD_LIBRARY_PATH and
 * the usual directories. Only the cache is conclusive; if it cannot be read and a library is not
 * found in the directories, that proves nothing, so we start silently. PHOTOCRAFT_SKIP_LIB_CHECK=1
 * skips the check.
 *
 * Behavioural port of apps/photocraft/src/linux_libs.rs. The parsing and decision logic is pure and
 * tested on every platform; only the probe helpers touch the system. The library set follows the
 * winit/wgpu reference; SDL3 may load a slightly different set (REWRITE_PLAN.md §7). */
#ifndef PC_APP_LINUX_LIBS_H
#define PC_APP_LINUX_LIBS_H

#include <stdbool.h>
#include <stddef.h>

#include "app_dirs.h"
#include "base/status.h"

typedef enum pc_display_session {
    PC_DISPLAY_WAYLAND,
    PC_DISPLAY_X11,
    PC_DISPLAY_NONE
} pc_display_session;

/* Chosen the way winit 0.30 does: Wayland when WAYLAND_DISPLAY/WAYLAND_SOCKET is set non-empty,
 * otherwise X11 when DISPLAY is. */
pc_display_session pc_linux_session_from_env(pc_app_env_fn env, void *ctx);

/* A runtime-loaded library and the package that provides it on each distro family. */
typedef struct pc_lib {
    const char *soname;
    const char *debian;
    const char *fedora;
    const char *arch;
    const char *suse;
} pc_lib;

/* One requirement: at least one of these libraries must be present. */
typedef struct pc_lib_anyof {
    const pc_lib *libs;
    size_t n;
} pc_lib_anyof;

/* The requirements for `session`: *out points to a static array of `*count` entries. */
void pc_linux_requirements(pc_display_session session, const pc_lib_anyof **out, size_t *count);

typedef enum pc_distro {
    PC_DISTRO_DEBIAN,
    PC_DISTRO_FEDORA,
    PC_DISTRO_ARCH,
    PC_DISTRO_SUSE,
    PC_DISTRO_UNKNOWN
} pc_distro;

pc_distro pc_linux_distro_from_os_release(const char *text);

/* The ldconfig -p architecture tag for this build (NULL when indistinguishable from 32-bit). */
const char *pc_linux_arch_tag(void);
/* Does ldconfig -p output list `soname` for `arch_tag` (NULL = any architecture)? */
bool pc_linux_ldconfig_has(const char *output, const char *soname, const char *arch_tag);
/* A real ldconfig -p listing starts with "N libs found in cache". */
bool pc_linux_ldconfig_valid(const char *output);

/* Where we look for libraries, and what we found there. */
typedef struct pc_linux_probe {
    const char *ldconfig;       /* ldconfig -p output, or NULL when it could not be read */
    const char *const *dirs;    /* directories to check for the soname */
    size_t n_dirs;
} pc_linux_probe;

/* Whether `path` exists (so tests can fake the file system). */
typedef bool (*pc_path_exists_fn)(void *ctx, const char *path);

typedef struct pc_linux_verdict {
    bool ok;
    const pc_lib_anyof *missing[16];
    size_t n_missing;
    bool conclusive; /* true when the linker cache was read, so the libraries really are absent */
} pc_linux_verdict;

void pc_linux_check(pc_display_session session, const pc_linux_probe *probe,
                    pc_path_exists_fn exists, void *ctx, pc_linux_verdict *out);

/* The message for missing libraries: what is missing and the command that installs it. */
void pc_linux_missing_message(pc_display_session session, const pc_lib_anyof *const *missing,
                              size_t n_missing, pc_distro distro, char *out, size_t cap);

/* Run the check against this system. Returns PC_OK to start (out_msg empty), or PC_ERR_UNSUPPORTED
 * with the message to print before exiting non-zero. Honours PHOTOCRAFT_SKIP_LIB_CHECK. */
pc_status pc_linux_preflight(pc_display_session session, char *out_msg, size_t cap);

/* Are all the libraries `session` needs found on this system? */
bool pc_linux_available(pc_display_session session);

#endif /* PC_APP_LINUX_LIBS_H */
