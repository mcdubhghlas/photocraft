/* Port of crates/automation/src/workspace.rs. Capability-based filesystem policy (D16):
 * openat / O_NOFOLLOW / O_DIRECTORY relative to a held directory fd. */
#define _XOPEN_SOURCE 700 /* realpath */
#include "automation/workspace.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "automation/automation.h"
#include "automation/security.h"
#include "base/json.h"

#define DENIED "automation filesystem access is not granted"

/* ---- path validation ---- */

static pc_status path_error(const char *path, const char *reason, pc_error *err)
{
    return pc_auto_bad(err, "automation path rejected: %s: `%s`", reason, path);
}

static bool is_windows_device_name(const char *comp, size_t len)
{
    char stem[16];
    size_t n = 0;
    for (size_t i = 0; i < len && comp[i] != '.' && n + 1 < sizeof stem; i++) {
        char c = comp[i];
        stem[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    stem[n] = '\0';
    static const char *const names[] = {"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$", NULL};
    for (size_t i = 0; names[i]; i++) {
        if (strcmp(stem, names[i]) == 0) {
            return true;
        }
    }
    if ((strncmp(stem, "COM", 3) == 0 || strncmp(stem, "LPT", 3) == 0) && stem[3] >= '1' && stem[3] <= '9' &&
        stem[4] == '\0') {
        return true;
    }
    return false;
}

/* Validate a forward-slash relative request path (port of relative_path). */
static pc_status relative_path(const char *raw, pc_error *err)
{
    if (raw[0] == '\0') {
        return path_error(raw, "path is empty", err);
    }
    if (strchr(raw, '\\')) {
        return path_error(raw, "alternate separators are not allowed; use `/`", err);
    }
    if (raw[0] == '/') {
        return path_error(raw, "absolute paths are not allowed", err);
    }
    if (strchr(raw, ':')) {
        return path_error(raw, "drive, device and stream prefixes are not allowed", err);
    }
    const char *p = raw;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) {
            return path_error(raw, "empty path components are not allowed", err);
        }
        if (len == 1 && p[0] == '.') {
            return path_error(raw, "`.` path components are not allowed", err);
        }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            return path_error(raw, "parent traversal is not allowed", err);
        }
        if (p[len - 1] == '.' || p[len - 1] == ' ') {
            return path_error(raw, "path components ending in a dot or space are not allowed", err);
        }
        if (is_windows_device_name(p, len)) {
            return path_error(raw, "reserved device names are not allowed", err);
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    return PC_OK;
}

/* Resolve a request path to a relative one (out, caller frees). Absolute paths are accepted only
 * when their text names a file beneath `root` (port of request_path). */
static pc_status request_path(const char *raw, const pc_auto_root *root, char **out, pc_error *err)
{
    bool absolute = raw[0] == '/';
    if (!absolute || !root || !root->path) {
        if (relative_path(raw, err) != PC_OK) {
            return err ? err->code : PC_ERR_INVALID;
        }
        char *dup = strdup(raw);
        if (!dup) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        *out = dup;
        return PC_OK;
    }
    /* raw and root->path are both absolute. Strip root's components from raw textually. */
    const char *r = raw;
    const char *rp = root->path;
    /* Skip the leading '/' on both. */
    while (*rp == '/') {
        rp++;
    }
    while (*r == '/') {
        r++;
    }
    /* Compare component by component: every root component must prefix-match raw. */
    while (*rp) {
        const char *rpe = strchr(rp, '/');
        size_t rlen = rpe ? (size_t)(rpe - rp) : strlen(rp);
        const char *re = strchr(r, '/');
        size_t clen = re ? (size_t)(re - r) : strlen(r);
        if (clen != rlen || strncmp(r, rp, rlen) != 0) {
            return path_error(raw, "absolute paths must be inside the automation root", err);
        }
        r += clen;
        while (*r == '/') {
            r++;
        }
        rp += rlen;
        while (*rp == '/') {
            rp++;
        }
    }
    if (*r == '\0') {
        return path_error(raw, "absolute paths must be inside the automation root", err);
    }
    if (relative_path(r, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    char *dup = strdup(r);
    if (!dup) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *out = dup;
    return PC_OK;
}

/* ---- capability walk ---- */

/* Open the parent directory of `rel` beneath root_fd, following no symlinks in any component.
 * *leaf points into `rel` at the final component. Returns the parent fd (caller closes) or -1. */
static int open_parent(int root_fd, char *rel, const char **leaf, pc_error *err)
{
    if (!rel) {
        (void)pc_fail(err, PC_ERR_INVALID, "empty path");
        return -1;
    }
    int cur = dup(root_fd);
    if (cur < 0) {
        (void)pc_fail_errno(err, PC_ERR_IO, errno, "dup root");
        return -1;
    }
    char *p = rel;
    for (;;) {
        char *slash = strchr(p, '/');
        if (!slash) {
            *leaf = p;
            return cur;
        }
        *slash = '\0';
        int next = openat(cur, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        *slash = '/';
        (void)close(cur);
        if (next < 0) {
            (void)pc_fail_errno(err, PC_ERR_IO, errno, "open directory `%.*s`", (int)(slash - rel), rel);
            return -1;
        }
        cur = next;
        p = slash + 1;
    }
}

/* ---- roots ---- */

static pc_status open_root(const char *path, const char *authority, pc_auto_root *out, pc_error *err)
{
    out->fd = -1;
    out->path = NULL;
    if (!path) {
        return PC_OK;
    }
    if (path[0] == '\0') {
        return pc_auto_bad(err, "automation %s root is empty", authority);
    }
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return pc_auto_io(err, "cannot open automation %s root `%s`: %s", authority, path, strerror(errno));
    }
    /* Absolute path (textual). */
    char abs[2 * PATH_MAX + 2];
    char *absolute;
    if (path[0] == '/') {
        absolute = strdup(path);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) {
            (void)close(fd);
            return pc_auto_io(err, "cannot resolve automation %s root `%s`: %s", authority, path,
                              strerror(errno));
        }
        (void)snprintf(abs, sizeof abs, "%s/%s", cwd, path);
        absolute = strdup(abs);
    }
    if (!absolute) {
        (void)close(fd);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    out->fd = fd;
    out->path = absolute;
    return PC_OK;
}

static void close_root(pc_auto_root *r)
{
    if (r->fd >= 0) {
        (void)close(r->fd);
        r->fd = -1;
    }
    free(r->path);
    r->path = NULL;
}

pc_status pc_auto_workspace_init(pc_auto_workspace *ws, const char *read_root, const char *write_root,
                                 pc_error *err)
{
    ws->read.fd = -1;
    ws->read.path = NULL;
    ws->write.fd = -1;
    ws->write.path = NULL;
    if (open_root(read_root, "read", &ws->read, err) != PC_OK) {
        return err ? err->code : PC_ERR_IO;
    }
    if (open_root(write_root, "write", &ws->write, err) != PC_OK) {
        close_root(&ws->read);
        return err ? err->code : PC_ERR_IO;
    }
    return PC_OK;
}

void pc_auto_workspace_fini(pc_auto_workspace *ws)
{
    if (!ws) {
        return;
    }
    close_root(&ws->read);
    close_root(&ws->write);
}

pc_status pc_auto_workspace_read(const pc_auto_workspace *ws, const char *path, pc_buf *out, pc_error *err)
{
    char *rel = NULL;
    if (request_path(path, ws->read.fd >= 0 ? &ws->read : NULL, &rel, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    if (ws->read.fd < 0) {
        free(rel);
        return pc_auto_bad(err, DENIED ": read authority is absent");
    }
    const char *leaf;
    int parent = open_parent(ws->read.fd, rel, &leaf, err);
    if (parent < 0) {
        char *why = rel;
        pc_auto_io_set(err, "automation read `%s` failed: %s", path, strerror(errno));
        (void)why;
        free(rel);
        return PC_ERR_IO;
    }
    int fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int oerr = errno;
    (void)close(parent);
    free(rel);
    if (fd < 0) {
        return pc_auto_io(err, "automation read `%s` failed: %s", path, strerror(oerr));
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        (void)close(fd);
        return pc_auto_io(err, "automation read `%s` failed: %s", path, strerror(e));
    }
    if (!S_ISREG(st.st_mode)) {
        (void)close(fd);
        return pc_auto_bad(err, "automation read path is not a regular file: `%s`", path);
    }
    /* Read in chunks; reject growth beyond the observed length. */
    uint64_t observed = (uint64_t)st.st_size;
    uint8_t chunk[65536];
    uint64_t total = 0;
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            int e = errno;
            (void)close(fd);
            return pc_auto_io(err, "automation read failed: %s", strerror(e));
        }
        if (n == 0) {
            break;
        }
        total += (uint64_t)n;
        if (total > observed) {
            (void)close(fd);
            return pc_auto_bad(err, "automation file grew beyond its observed length of %llu bytes",
                               (unsigned long long)observed);
        }
        if (pc_buf_append(out, chunk, (size_t)n, err) != PC_OK) {
            (void)close(fd);
            return err ? err->code : PC_ERR_NOMEM;
        }
    }
    (void)close(fd);
    return PC_OK;
}

pc_status pc_auto_workspace_write(const pc_auto_workspace *ws, const char *path, const void *bytes,
                                  size_t len, pc_error *err)
{
    char *rel = NULL;
    if (request_path(path, ws->write.fd >= 0 ? &ws->write : NULL, &rel, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    if (ws->write.fd < 0) {
        free(rel);
        return pc_auto_bad(err, DENIED ": write authority is absent");
    }
    const char *leaf;
    int parent = open_parent(ws->write.fd, rel, &leaf, err);
    if (parent < 0) {
        pc_auto_io_set(err, "automation write `%s` failed: %s", path, strerror(errno));
        free(rel);
        return PC_ERR_IO;
    }
    char tmp[NAME_MAX + 1];
    (void)snprintf(tmp, sizeof tmp, ".%s.%ld.tmp", leaf, (long)getpid());
    int fd = openat(parent, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        int e = errno;
        (void)close(parent);
        free(rel);
        return pc_auto_io(err, "automation write `%s` failed: %s", path, strerror(e));
    }
    pc_status rc = pc_auto_write_all(fd, false, bytes, len, err);
    if (rc == PC_OK && fsync(fd) != 0) {
        rc = pc_auto_io(err, "automation write `%s` failed: %s", path, strerror(errno));
    }
    (void)close(fd);
    if (rc == PC_OK && renameat(parent, tmp, parent, leaf) != 0) {
        rc = pc_auto_io(err, "automation write `%s` failed: %s", path, strerror(errno));
    }
    if (rc != PC_OK) {
        (void)unlinkat(parent, tmp, 0);
    } else {
        (void)fsync(parent); /* best-effort directory-entry flush */
    }
    (void)close(parent);
    free(rel);
    return rc;
}

/* ---- command authorization ---- */

static pc_status command_error(const char *id, pc_error *err)
{
    return pc_auto_bad(err,
                       "automation command `%s` uses ambient filesystem paths and is disabled; use "
                       "capability-scoped document methods",
                       id);
}

static bool matches_any(const char *id, const char *const *list)
{
    for (size_t i = 0; list[i]; i++) {
        if (strcmp(id, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool str_contains(const char *s, char c)
{
    return strchr(s, c) != NULL;
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls) {
        return false;
    }
    for (size_t i = 0; i < lx; i++) {
        char a = s[ls - lx + i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != suffix[i]) {
            return false;
        }
    }
    return true;
}

static bool looks_like_path(const char *v)
{
    return str_contains(v, '/') || str_contains(v, '\\') || str_contains(v, ':') || ends_with_ci(v, ".icc") ||
           ends_with_ci(v, ".icm");
}

static const char *param_str(yyjson_val *params, const char *key)
{
    return pc_json_as_str(pc_json_get(params, key), NULL);
}

static bool params_contain_ambient_path(const char *id, yyjson_val *params)
{
    const char *const *keys = NULL;
    static const char *const file_key[] = {"file", NULL};
    static const char *const mappath_key[] = {"mapPath", NULL};
    static const char *const path_key[] = {"path", NULL};
    static const char *const profile_key[] = {"profile", NULL};
    static const char *const working_keys[] = {"workingRgb", "workingCmyk", "workingGray", NULL};
    if (strcmp(id, "image.adjustments.colorLookup") == 0 || strcmp(id, "layer.newAdjustmentLayer.colorLookup") == 0 ||
        strcmp(id, "layer.setAdjustment") == 0) {
        keys = file_key;
    } else if (strcmp(id, "filter.distort.displace") == 0) {
        keys = mappath_key;
    } else if (strcmp(id, "layer.quickExportAsPng") == 0 || strcmp(id, "layer.exportAs") == 0 ||
               strcmp(id, "image.applyDataSet") == 0 || strcmp(id, "layer.smartObjects.editContents") == 0 ||
               strcmp(id, "layer.smartObjects.convertToLayers") == 0) {
        keys = path_key;
    } else if (strcmp(id, "image.mode.rgb") == 0 || strcmp(id, "image.mode.grayscale") == 0 ||
               strcmp(id, "image.mode.cmyk") == 0 || strcmp(id, "image.mode.lab") == 0 ||
               strcmp(id, "edit.assignProfile") == 0 || strcmp(id, "edit.convertToProfile") == 0 ||
               strcmp(id, "edit.profileInfo") == 0 || strcmp(id, "view.proofSetup") == 0 ||
               strcmp(id, "view.gamutWarning") == 0) {
        keys = profile_key;
    } else if (strcmp(id, "edit.colorSettings") == 0) {
        keys = working_keys;
    } else {
        return false;
    }
    for (size_t i = 0; keys[i]; i++) {
        const char *v = param_str(params, keys[i]);
        if (!v) {
            continue;
        }
        bool raw_key = strcmp(keys[i], "file") == 0 || strcmp(keys[i], "mapPath") == 0 ||
                       strcmp(keys[i], "path") == 0;
        if (raw_key ? v[0] != '\0' : looks_like_path(v)) {
            return true;
        }
    }
    return false;
}

static bool profile_command_may_read_ambient(const char *id, yyjson_val *params)
{
    if (strcmp(id, "color.profileMismatch") == 0 || strcmp(id, "edit.colorSettings") == 0 ||
        strcmp(id, "edit.installedProfiles") == 0 || strcmp(id, "view.proofSetup") == 0) {
        return true;
    }
    if (strcmp(id, "edit.assignProfile") == 0 || strcmp(id, "edit.convertToProfile") == 0 ||
        strcmp(id, "edit.profileInfo") == 0) {
        const char *p = param_str(params, "profile");
        if (p) {
            static const char *const w[] = {"working", "default",     "working-cmyk", "workingCmyk",
                                            "working-rgb", "workingRgb", "working-gray", "workingGray", NULL};
            return matches_any(p, w);
        }
    }
    return false;
}

static bool preference_uses_ambient_filesystem(const char *path)
{
    const char *seg = path;
    while (*seg == '.') {
        seg++;
    }
    if (*seg == '\0') {
        /* no non-empty segment at all: whole-preferences update */
        /* but a path that is only dots: treat as whole update */
        return true;
    }
    size_t len = 0;
    while (seg[len] && seg[len] != '.') {
        len++;
    }
    char section[64];
    size_t n = len < sizeof section - 1 ? len : sizeof section - 1;
    memcpy(section, seg, n);
    section[n] = '\0';
    static const char *const s[] = {"colorSettings", "scriptEvents", "historyLog", "plugIns", "scratchDisks", NULL};
    return matches_any(section, s);
}

static bool preferences_may_grant_ambient_paths(const char *id, yyjson_val *params)
{
    if (strcmp(id, "prefs.set") != 0) {
        return false;
    }
    const char *direct = param_str(params, "path");
    if (direct && preference_uses_ambient_filesystem(direct)) {
        return true;
    }
    yyjson_val *values = pc_json_get(params, "values");
    if (values && yyjson_is_obj(values)) {
        yyjson_val *key, *val;
        yyjson_obj_iter it;
        yyjson_obj_iter_init(values, &it);
        while ((key = yyjson_obj_iter_next(&it))) {
            val = yyjson_obj_iter_get_val(key);
            (void)val;
            const char *k = yyjson_get_str(key);
            if (k && preference_uses_ambient_filesystem(k)) {
                return true;
            }
        }
    }
    return false;
}

static bool command_uses_ambient_path(const char *id, yyjson_val *params)
{
    if (strcmp(id, "actions.load") == 0 || strcmp(id, "brush.presets.importAbr") == 0 ||
        strcmp(id, "gradient.presets.importGrd") == 0 || strcmp(id, "plugin.install") == 0 ||
        strcmp(id, "swatches.import") == 0) {
        return pc_json_get(params, "data") == NULL && pc_json_get(params, "path") != NULL;
    }
    if (strcmp(id, "actions.save") == 0 || strcmp(id, "swatches.export") == 0) {
        return pc_json_get(params, "path") != NULL;
    }
    if (strcmp(id, "plugin.reload") == 0) {
        return true;
    }
    return false;
}

pc_status pc_auto_authorize_engine_command(const char *id, yyjson_val *params, pc_error *err)
{
    static const char *const safe_file[] = {
        "file.new", "file.newFromClipboard", "file.close", "file.closeAll", "file.closeOthers",
        "file.fileInfo", "file.automate.fitImage", "file.automate.conditionalModeChange",
        "file.automate.cropAndStraightenPhotos", "file.scripts.flattenAllLayerEffects",
        "file.scripts.flattenAllMasks", "file.scripts.deleteAllEmptyLayers", "file.export.exportPreferences",
        NULL};
    if (strncmp(id, "file.", 5) == 0 && !matches_any(id, safe_file)) {
        return command_error(id, err);
    }
    static const char *const in_memory_smart[] = {
        "layer.smartObjects.convertToSmartObject", "layer.smartObjects.editContents",
        "layer.smartObjects.convertToLayers", "layer.smartObjects.saveContents", NULL};
    static const char *const denied_misc[] = {
        "pattern.import", "pattern.export", "edit.presets.migratePresets", "measurementLog.export",
        "layer.videoLayers.newVideoLayerFromFile", "layer.videoLayers.replaceFootage",
        "layer.videoLayers.reloadFrame", NULL};
    bool smart_denied = strncmp(id, "layer.smartObjects.", 19) == 0 && !matches_any(id, in_memory_smart);
    if (smart_denied || matches_any(id, denied_misc) || command_uses_ambient_path(id, params) ||
        profile_command_may_read_ambient(id, params) || preferences_may_grant_ambient_paths(id, params) ||
        params_contain_ambient_path(id, params)) {
        return command_error(id, err);
    }
    return PC_OK;
}

pc_status pc_auto_authorize_desktop_engine_command(const char *id, yyjson_val *params, pc_error *err)
{
    if (pc_auto_authorize_engine_command(id, params, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    if (strncmp(id, "image.mode.", 11) == 0) {
        return command_error(id, err);
    }
    return PC_OK;
}
