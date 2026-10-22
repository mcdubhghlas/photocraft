/* See linux_libs.h. Behavioural port of apps/photocraft/src/linux_libs.rs. */
#include "linux_libs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/buf.h"
#include "base/file.h"

#define LIB(soname, debian, fedora, arch, suse) {soname, debian, fedora, arch, suse}

static const pc_lib XKBCOMMON = LIB("libxkbcommon.so.0", "libxkbcommon0", "libxkbcommon", "libxkbcommon", "libxkbcommon0");
static const pc_lib XKBCOMMON_X11 = LIB("libxkbcommon-x11.so.0", "libxkbcommon-x11-0", "libxkbcommon-x11", "libxkbcommon-x11", "libxkbcommon-x11-0");
static const pc_lib X11 = LIB("libX11.so.6", "libx11-6", "libX11", "libx11", "libX11-6");
static const pc_lib X11_XCB = LIB("libX11-xcb.so.1", "libx11-xcb1", "libX11-xcb", "libx11", "libX11-xcb1");
static const pc_lib XCB = LIB("libxcb.so.1", "libxcb1", "libxcb", "libxcb", "libxcb1");
static const pc_lib XCURSOR = LIB("libXcursor.so.1", "libxcursor1", "libXcursor", "libxcursor", "libXcursor1");
static const pc_lib XI = LIB("libXi.so.6", "libxi6", "libXi", "libxi", "libXi6");
static const pc_lib WAYLAND_CLIENT = LIB("libwayland-client.so.0", "libwayland-client0", "libwayland-client", "wayland", "libwayland-client0");
static const pc_lib VULKAN = LIB("libvulkan.so.1", "libvulkan1", "vulkan-loader", "vulkan-icd-loader", "libvulkan1");
static const pc_lib EGL = LIB("libEGL.so.1", "libegl1", "libglvnd-egl", "libglvnd", "libEGL1");

/* GPU: Vulkan OR EGL. */
static const pc_lib GPU_LIBS[] = {VULKAN, EGL};

static const pc_lib WAYLAND_CLIENT_ONE[] = {WAYLAND_CLIENT};
static const pc_lib XKBCOMMON_ONE[] = {XKBCOMMON};
static const pc_lib_anyof WAYLAND_REQS[] = {
    {WAYLAND_CLIENT_ONE, 1}, {XKBCOMMON_ONE, 1}, {GPU_LIBS, 2}};

static const pc_lib X11_ONE[] = {X11};
static const pc_lib X11_XCB_ONE[] = {X11_XCB};
static const pc_lib XCB_ONE[] = {XCB};
static const pc_lib XCURSOR_ONE[] = {XCURSOR};
static const pc_lib XI_ONE[] = {XI};
static const pc_lib XKBCOMMON_X11_ONE[] = {XKBCOMMON_X11};
static const pc_lib_anyof X11_REQS[] = {{X11_ONE, 1},      {X11_XCB_ONE, 1}, {XCB_ONE, 1},
                                        {XCURSOR_ONE, 1},  {XI_ONE, 1},      {XKBCOMMON_ONE, 1},
                                        {XKBCOMMON_X11_ONE, 1}, {GPU_LIBS, 2}};

void pc_linux_requirements(pc_display_session session, const pc_lib_anyof **out, size_t *count)
{
    switch (session) {
    case PC_DISPLAY_WAYLAND:
        *out = WAYLAND_REQS;
        *count = sizeof WAYLAND_REQS / sizeof WAYLAND_REQS[0];
        return;
    case PC_DISPLAY_X11:
        *out = X11_REQS;
        *count = sizeof X11_REQS / sizeof X11_REQS[0];
        return;
    case PC_DISPLAY_NONE:
        *out = NULL;
        *count = 0;
        return;
    }
    *out = NULL;
    *count = 0;
}

static bool env_set(pc_app_env_fn env, void *ctx, const char *name)
{
    const char *v = env ? env(ctx, name) : NULL;
    return v && v[0] != '\0';
}

pc_display_session pc_linux_session_from_env(pc_app_env_fn env, void *ctx)
{
    if (env_set(env, ctx, "WAYLAND_DISPLAY") || env_set(env, ctx, "WAYLAND_SOCKET")) {
        return PC_DISPLAY_WAYLAND;
    }
    if (env_set(env, ctx, "DISPLAY")) {
        return PC_DISPLAY_X11;
    }
    return PC_DISPLAY_NONE;
}

const char *pc_linux_arch_tag(void)
{
#if defined(__x86_64__)
    return "x86-64";
#elif defined(__aarch64__)
    return "AArch64";
#else
    return NULL;
#endif
}

bool pc_linux_ldconfig_has(const char *output, const char *soname, const char *arch_tag)
{
    if (!output || !soname) {
        return false;
    }
    const char *line = output;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        /* trim leading whitespace */
        const char *s = line;
        size_t rem = len;
        while (rem > 0 && (*s == ' ' || *s == '\t')) {
            s++;
            rem--;
        }
        const char *sp = memchr(s, ' ', rem);
        if (sp) {
            size_t name_len = (size_t)(sp - s);
            if (name_len == strlen(soname) && memcmp(s, soname, name_len) == 0) {
                if (!arch_tag) {
                    return true;
                }
                /* flags are inside the first "(...)" of the rest */
                const char *rest = sp;
                size_t rest_len = rem - name_len;
                const char *op = memchr(rest, '(', rest_len);
                if (op) {
                    const char *cp = memchr(op, ')', rest_len - (size_t)(op - rest));
                    if (cp) {
                        const char *f = op + 1;
                        while (f < cp) {
                            const char *comma = memchr(f, ',', (size_t)(cp - f));
                            const char *fend = comma ? comma : cp;
                            size_t flen = (size_t)(fend - f);
                            /* trim */
                            const char *ft = f;
                            size_t ftl = flen;
                            while (ftl > 0 && (*ft == ' ' || *ft == '\t')) { ft++; ftl--; }
                            while (ftl > 0 && (ft[ftl - 1] == ' ' || ft[ftl - 1] == '\t')) { ftl--; }
                            if (ftl == strlen(arch_tag) && memcmp(ft, arch_tag, ftl) == 0) {
                                return true;
                            }
                            if (!comma) {
                                break;
                            }
                            f = comma + 1;
                        }
                    }
                }
            }
        }
        if (!eol) {
            break;
        }
        line = eol + 1;
    }
    return false;
}

bool pc_linux_ldconfig_valid(const char *output)
{
    if (!output) {
        return false;
    }
    /* First token of the first line must parse as an unsigned integer. */
    const char *s = output;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s < '0' || *s > '9') {
        return false;
    }
    return true;
}

static bool probe_has(const pc_linux_probe *probe, const char *soname, pc_path_exists_fn exists,
                      void *ctx)
{
    if (probe->ldconfig && pc_linux_ldconfig_has(probe->ldconfig, soname, pc_linux_arch_tag())) {
        return true;
    }
    for (size_t i = 0; i < probe->n_dirs; i++) {
        char path[PC_APP_PATH_MAX];
        int n = snprintf(path, sizeof path, "%s/%s", probe->dirs[i], soname);
        if (n > 0 && (size_t)n < sizeof path && exists && exists(ctx, path)) {
            return true;
        }
    }
    return false;
}

static bool anyof_present(const pc_lib_anyof *req, const pc_linux_probe *probe,
                          pc_path_exists_fn exists, void *ctx)
{
    for (size_t i = 0; i < req->n; i++) {
        if (probe_has(probe, req->libs[i].soname, exists, ctx)) {
            return true;
        }
    }
    return false;
}

void pc_linux_check(pc_display_session session, const pc_linux_probe *probe,
                    pc_path_exists_fn exists, void *ctx, pc_linux_verdict *out)
{
    memset(out, 0, sizeof *out);
    const pc_lib_anyof *reqs = NULL;
    size_t n = 0;
    pc_linux_requirements(session, &reqs, &n);
    for (size_t i = 0; i < n && out->n_missing < 16; i++) {
        if (!anyof_present(&reqs[i], probe, exists, ctx)) {
            out->missing[out->n_missing++] = &reqs[i];
        }
    }
    out->ok = out->n_missing == 0;
    out->conclusive = probe->ldconfig != NULL;
}

pc_distro pc_linux_distro_from_os_release(const char *text)
{
    if (!text) {
        return PC_DISTRO_UNKNOWN;
    }
    bool debian = false, fedora = false, arch = false, suse = false;
    const char *line = text;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        const char *eq = memchr(line, '=', len);
        if (eq) {
            size_t klen = (size_t)(eq - line);
            /* trim key */
            const char *k = line;
            while (klen > 0 && (*k == ' ' || *k == '\t')) { k++; klen--; }
            while (klen > 0 && (k[klen - 1] == ' ' || k[klen - 1] == '\t')) { klen--; }
            bool is_id = (klen == 2 && memcmp(k, "ID", 2) == 0);
            bool is_like = (klen == 7 && memcmp(k, "ID_LIKE", 7) == 0);
            if (is_id || is_like) {
                /* value: strip quotes, split on whitespace, lower-case compare */
                const char *v = eq + 1;
                size_t vlen = len - (size_t)(eq + 1 - line);
                while (vlen > 0 && (*v == ' ' || *v == '\t' || *v == '"' || *v == '\'')) { v++; vlen--; }
                while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t' || v[vlen - 1] == '"' ||
                                    v[vlen - 1] == '\'' || v[vlen - 1] == '\r')) { vlen--; }
                const char *p = v;
                size_t rem = vlen;
                while (rem > 0) {
                    while (rem > 0 && (*p == ' ' || *p == '\t')) { p++; rem--; }
                    const char *tok = p;
                    size_t tl = 0;
                    while (tl < rem && p[tl] != ' ' && p[tl] != '\t') { tl++; }
                    char id[64];
                    size_t copy = tl < sizeof id - 1 ? tl : sizeof id - 1;
                    for (size_t i = 0; i < copy; i++) {
                        char c = tok[i];
                        id[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
                    }
                    id[copy] = '\0';
                    if (strcmp(id, "debian") == 0 || strcmp(id, "ubuntu") == 0) { debian = true; }
                    else if (strcmp(id, "fedora") == 0 || strcmp(id, "rhel") == 0 || strcmp(id, "centos") == 0) { fedora = true; }
                    else if (strcmp(id, "arch") == 0) { arch = true; }
                    else if (strcmp(id, "suse") == 0 || strcmp(id, "opensuse") == 0 || strncmp(id, "opensuse", 8) == 0) { suse = true; }
                    p += tl;
                    rem -= tl;
                }
            }
        }
        if (!eol) {
            break;
        }
        line = eol + 1;
    }
    if (debian) { return PC_DISTRO_DEBIAN; }
    if (fedora) { return PC_DISTRO_FEDORA; }
    if (arch) { return PC_DISTRO_ARCH; }
    if (suse) { return PC_DISTRO_SUSE; }
    return PC_DISTRO_UNKNOWN;
}

static const char *package(const pc_lib *lib, pc_distro distro)
{
    switch (distro) {
    case PC_DISTRO_FEDORA: return lib->fedora;
    case PC_DISTRO_ARCH: return lib->arch;
    case PC_DISTRO_SUSE: return lib->suse;
    case PC_DISTRO_DEBIAN:
    case PC_DISTRO_UNKNOWN: return lib->debian;
    }
    return lib->debian;
}

/* Append to out[cap] at *off (bounded; always keeps the string NUL-terminated). */
static void append(char *out, size_t cap, size_t *off, const char *s)
{
    if (!s || cap == 0 || *off + 1 >= cap) {
        return;
    }
    size_t avail = cap - *off - 1; /* leave room for the NUL */
    size_t sl = strlen(s);
    size_t n = sl < avail ? sl : avail;
    memcpy(out + *off, s, n);
    *off += n;
    out[*off] = '\0';
}

void pc_linux_missing_message(pc_display_session session, const pc_lib_anyof *const *missing,
                              size_t n_missing, pc_distro distro, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    size_t off = 0;
    const char *session_name = session == PC_DISPLAY_WAYLAND ? "Wayland"
                               : session == PC_DISPLAY_X11    ? "X11"
                                                              : "display";
    append(out, cap, &off, "photocraft: cannot start: the ");
    append(out, cap, &off, session_name);
    append(out, cap, &off, " session needs libraries that are not installed: ");
    for (size_t i = 0; i < n_missing; i++) {
        if (i > 0) {
            append(out, cap, &off, ", ");
        }
        for (size_t j = 0; j < missing[i]->n; j++) {
            if (j > 0) {
                append(out, cap, &off, " or ");
            }
            append(out, cap, &off, missing[i]->libs[j].soname);
        }
    }
    append(out, cap, &off, ".\nInstall them with:\n  ");
    /* First alternative of each requirement, de-duplicated, as packages. */
    const char *pkgs[32];
    size_t n_pkgs = 0;
    for (size_t i = 0; i < n_missing && n_pkgs < 32; i++) {
        if (missing[i]->n == 0) {
            continue;
        }
        const char *p = package(&missing[i]->libs[0], distro);
        bool dup = false;
        for (size_t k = 0; k < n_pkgs; k++) {
            if (strcmp(pkgs[k], p) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            pkgs[n_pkgs++] = p;
        }
    }
    const char *prefix = distro == PC_DISTRO_FEDORA ? "sudo dnf install "
                         : distro == PC_DISTRO_ARCH ? "sudo pacman -S "
                         : distro == PC_DISTRO_SUSE ? "sudo zypper install "
                         : distro == PC_DISTRO_DEBIAN ? "sudo apt install "
                                                      : "Debian/Ubuntu: sudo apt install ";
    append(out, cap, &off, prefix);
    for (size_t i = 0; i < n_pkgs; i++) {
        if (i > 0) {
            append(out, cap, &off, " ");
        }
        append(out, cap, &off, pkgs[i]);
    }
    append(out, cap, &off, "\n");
    if (distro == PC_DISTRO_UNKNOWN) {
        append(out, cap, &off, "  Fedora: sudo dnf install ");
        for (size_t i = 0; i < n_missing; i++) {
            if (missing[i]->n == 0) {
                continue;
            }
            if (i > 0) {
                append(out, cap, &off, " ");
            }
            append(out, cap, &off, missing[i]->libs[0].fedora);
        }
        append(out, cap, &off, "\n");
    }
    append(out, cap, &off, "(Set PHOTOCRAFT_SKIP_LIB_CHECK=1 to start anyway.)\n");
}

/* ---- system probes (touch the real system) ---- */

static bool path_exists(void *ctx, const char *path)
{
    (void)ctx;
    return access(path, F_OK) == 0;
}

/* Read ldconfig -p from the usual locations; appended into *out. Returns true on a valid listing. */
static bool read_ldconfig(pc_buf *out)
{
    static const char *const cmds[] = {"/sbin/ldconfig -p", "/usr/sbin/ldconfig -p", "ldconfig -p"};
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        char full[128];
        (void)snprintf(full, sizeof full, "LC_ALL=C %s 2>/dev/null", cmds[i]);
        FILE *p = popen(full, "r");
        if (!p) {
            continue;
        }
        out->len = 0;
        char chunk[4096];
        size_t got;
        pc_error err;
        bool ok = true;
        while ((got = fread(chunk, 1, sizeof chunk, p)) > 0) {
            if (pc_buf_append(out, chunk, got, &err) != PC_OK) {
                ok = false;
                break;
            }
            if (out->len > (size_t)(8u << 20)) {
                break; /* bound the listing */
            }
        }
        int rc = pclose(p);
        if (ok && rc == 0) {
            char nul = '\0';
            if (pc_buf_append(out, &nul, 1, &err) == PC_OK &&
                pc_linux_ldconfig_valid((const char *)out->data)) {
                out->len--; /* drop the NUL from the length; data stays terminated */
                return true;
            }
        }
    }
    return false;
}

/* The usual library directories plus LD_LIBRARY_PATH. Owns the strings via *storage. */
static size_t search_dirs(const char ***out_dirs, char ***storage)
{
    static const char *const triplet =
#if defined(__aarch64__)
        "aarch64-linux-gnu";
#elif defined(__riscv) && (__riscv_xlen == 64)
        "riscv64-linux-gnu";
#else
        "x86_64-linux-gnu";
#endif
    char **owned = calloc(32, sizeof *owned);
    const char **dirs = calloc(32, sizeof *dirs);
    if (!owned || !dirs) {
        free(owned);
        free(dirs);
        *out_dirs = NULL;
        *storage = NULL;
        return 0;
    }
    size_t n = 0;
    const char *ld = getenv("LD_LIBRARY_PATH");
    if (ld) {
        char *copy = strdup(ld);
        if (copy) {
            char *save = NULL;
            for (char *tok = strtok_r(copy, ":", &save); tok && n < 24; tok = strtok_r(NULL, ":", &save)) {
                if (tok[0] != '\0') {
                    char *d = strdup(tok);
                    if (d) {
                        owned[n] = d;
                        dirs[n] = d;
                        n++;
                    }
                }
            }
            free(copy);
        }
    }
    static const char *const fixed[] = {"/app/lib", "/usr/local/lib", "/usr/lib64", "/lib64",
                                        "/usr/lib", "/lib"};
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0] && n < 30; i++) {
        dirs[n++] = fixed[i];
    }
    char tbuf[128];
    (void)snprintf(tbuf, sizeof tbuf, "/usr/lib/%s", triplet);
    char *td1 = strdup(tbuf);
    if (td1 && n < 31) { owned[n] = td1; dirs[n] = td1; n++; }
    (void)snprintf(tbuf, sizeof tbuf, "/lib/%s", triplet);
    char *td2 = strdup(tbuf);
    if (td2 && n < 32) { owned[n] = td2; dirs[n] = td2; n++; }
    *out_dirs = dirs;
    *storage = owned;
    return n;
}

static void free_dirs(const char **dirs, char **storage)
{
    if (storage) {
        for (size_t i = 0; i < 32; i++) {
            free(storage[i]);
        }
        free(storage);
    }
    free((void *)dirs);
}

static void system_check(pc_display_session session, pc_linux_verdict *out)
{
    pc_buf ld = PC_BUF_INIT;
    bool have_ld = read_ldconfig(&ld);
    const char **dirs = NULL;
    char **storage = NULL;
    size_t n_dirs = search_dirs(&dirs, &storage);
    pc_linux_probe probe = {have_ld ? (const char *)ld.data : NULL, dirs, n_dirs};
    pc_linux_check(session, &probe, path_exists, NULL, out);
    free_dirs(dirs, storage);
    pc_buf_fini(&ld);
}

bool pc_linux_available(pc_display_session session)
{
    pc_linux_verdict v;
    system_check(session, &v);
    return v.ok;
}

static pc_distro read_distro(void)
{
    pc_buf b = PC_BUF_INIT;
    pc_error err;
    pc_distro d = PC_DISTRO_UNKNOWN;
    if (pc_file_read("/etc/os-release", (size_t)(1u << 20), &b, &err) == PC_OK) {
        char nul = '\0';
        if (pc_buf_append(&b, &nul, 1, &err) == PC_OK) {
            d = pc_linux_distro_from_os_release((const char *)b.data);
        }
    }
    pc_buf_fini(&b);
    return d;
}

pc_status pc_linux_preflight(pc_display_session session, char *out_msg, size_t cap)
{
    if (out_msg && cap > 0) {
        out_msg[0] = '\0';
    }
    const char *skip = getenv("PHOTOCRAFT_SKIP_LIB_CHECK");
    if (skip && skip[0] != '\0' && strcmp(skip, "0") != 0) {
        return PC_OK;
    }
    if (session == PC_DISPLAY_NONE) {
        return PC_OK;
    }
    pc_linux_verdict v;
    system_check(session, &v);
    if (v.ok || !v.conclusive) {
        return PC_OK;
    }
    pc_linux_missing_message(session, v.missing, v.n_missing, read_distro(), out_msg, cap);
    return PC_ERR_UNSUPPORTED;
}
