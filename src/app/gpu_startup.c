/* See gpu_startup.h. Behavioural port of apps/photocraft/src/gpu_startup.rs (planning + sentinel). */
#include "gpu_startup.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <yyjson.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "base/log.h"
#include "base/mem.h"

/* Largest marker read (a marker is a few hundred bytes). */
#define MARKER_MAX ((size_t)(64u << 10))

bool pc_app_gpu_backend_parse(const char *s, pc_app_gpu_backend *out)
{
    if (!s) {
        return false;
    }
    static const struct {
        const char *name;
        pc_app_gpu_backend b;
    } tbl[] = {{"auto", PC_APP_GPU_AUTO}, {"vulkan", PC_APP_GPU_VULKAN}, {"dx12", PC_APP_GPU_DX12},
              {"metal", PC_APP_GPU_METAL}, {"gl", PC_APP_GPU_GL},        {"cpu", PC_APP_GPU_CPU}};
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
        if (strcmp(s, tbl[i].name) == 0) {
            if (out) {
                *out = tbl[i].b;
            }
            return true;
        }
    }
    return false;
}

const char *pc_app_gpu_backend_name(pc_app_gpu_backend b)
{
    switch (b) {
    case PC_APP_GPU_AUTO: return "auto";
    case PC_APP_GPU_VULKAN: return "vulkan";
    case PC_APP_GPU_DX12: return "dx12";
    case PC_APP_GPU_METAL: return "metal";
    case PC_APP_GPU_GL: return "gl";
    case PC_APP_GPU_CPU: return "cpu";
    }
    return "auto";
}

bool pc_app_render_mode_parse(const char *s, pc_app_render_mode *out)
{
    if (!s) {
        return false;
    }
    if (strcmp(s, "auto") == 0) {
        if (out) { *out = PC_APP_RENDER_AUTO; }
        return true;
    }
    if (strcmp(s, "gpu") == 0) {
        if (out) { *out = PC_APP_RENDER_GPU; }
        return true;
    }
    if (strcmp(s, "cpu") == 0) {
        if (out) { *out = PC_APP_RENDER_CPU; }
        return true;
    }
    return false;
}

pc_app_gpu_os pc_app_gpu_os_current(void)
{
#if defined(_WIN32)
    return PC_APP_GPU_OS_WINDOWS;
#elif defined(__APPLE__)
    return PC_APP_GPU_OS_MAC;
#else
    return PC_APP_GPU_OS_OTHER;
#endif
}

pc_app_gpu_backend pc_app_gpu_sanitize(pc_app_gpu_backend pref, pc_app_gpu_os os)
{
    switch (os) {
    case PC_APP_GPU_OS_WINDOWS:
        return pref == PC_APP_GPU_METAL ? PC_APP_GPU_AUTO : pref;
    case PC_APP_GPU_OS_MAC:
        return (pref == PC_APP_GPU_VULKAN || pref == PC_APP_GPU_DX12 || pref == PC_APP_GPU_GL) ? PC_APP_GPU_AUTO : pref;
    case PC_APP_GPU_OS_OTHER:
        return (pref == PC_APP_GPU_DX12 || pref == PC_APP_GPU_METAL) ? PC_APP_GPU_AUTO : pref;
    }
    return pref;
}

pc_app_gpu_backend pc_app_gpu_next_safer(pc_app_gpu_backend tried, pc_app_gpu_os os)
{
    switch (os) {
    case PC_APP_GPU_OS_WINDOWS:
        return (tried == PC_APP_GPU_DX12 || tried == PC_APP_GPU_CPU) ? PC_APP_GPU_CPU : PC_APP_GPU_DX12;
    case PC_APP_GPU_OS_MAC:
        return PC_APP_GPU_CPU;
    case PC_APP_GPU_OS_OTHER:
        return (tried == PC_APP_GPU_GL || tried == PC_APP_GPU_CPU) ? PC_APP_GPU_CPU : PC_APP_GPU_GL;
    }
    return PC_APP_GPU_CPU;
}

void pc_app_gpu_marker_to_json(const pc_app_gpu_marker *m, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return;
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "backend", m->backend);
    yyjson_mut_obj_add_strcpy(doc, o, "adapter", m->adapter);
    yyjson_mut_obj_add_strcpy(doc, o, "adapterBackend", m->adapter_backend);
    yyjson_mut_obj_add_strcpy(doc, o, "driver", m->driver);
    yyjson_mut_obj_add_strcpy(doc, o, "version", m->version);
    yyjson_mut_doc_set_root(doc, o);
    size_t len = 0;
    char *text = yyjson_mut_write(doc, 0, &len);
    if (text) {
        int n = snprintf(out, cap, "%s", text);
        if (n < 0) {
            out[0] = '\0';
        }
        free(text);
    }
    yyjson_mut_doc_free(doc);
}

/* Copy up to cap-1 chars of a JSON string field into dst[cap]. */
static void field_str(yyjson_val *obj, const char *key, char *dst, size_t cap)
{
    dst[0] = '\0';
    size_t len = 0;
    const char *s = pc_json_as_str(pc_json_get(obj, key), &len);
    if (s) {
        (void)snprintf(dst, cap, "%.*s", (int)(len < cap - 1 ? len : cap - 1), s);
    }
}

void pc_app_gpu_marker_parse(const char *text, size_t len, pc_app_gpu_marker *out)
{
    memset(out, 0, sizeof *out);
    if (!text || len == 0) {
        return;
    }
    yyjson_doc *doc = NULL;
    pc_error err;
    if (pc_json_parse(text, len, &doc, &err) != PC_OK || !doc) {
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    field_str(root, "backend", out->backend, sizeof out->backend);
    field_str(root, "adapter", out->adapter, sizeof out->adapter);
    field_str(root, "adapterBackend", out->adapter_backend, sizeof out->adapter_backend);
    field_str(root, "driver", out->driver, sizeof out->driver);
    field_str(root, "version", out->version, sizeof out->version);
    yyjson_doc_free(doc);
}

bool pc_app_gpu_marker_tried(const pc_app_gpu_marker *m, pc_app_gpu_backend *out)
{
    pc_app_gpu_backend planned;
    if (m->backend[0] == '\0') {
        planned = PC_APP_GPU_AUTO;
    } else if (!pc_app_gpu_backend_parse(m->backend, &planned)) {
        return false; /* "env:..." or unknown: not ours to count */
    }
    pc_app_gpu_backend actual;
    if (planned == PC_APP_GPU_AUTO && pc_app_gpu_backend_parse(m->adapter_backend, &actual)) {
        if (out) { *out = actual; }
    } else if (out) {
        *out = planned;
    }
    return true;
}

static void set_reason(pc_app_gpu_plan *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(p->reason, sizeof p->reason, fmt, ap);
    va_end(ap);
    p->has_reason = n > 0;
}

void pc_app_gpu_compute_plan(pc_app_gpu_backend pref, const pc_app_gpu_marker *crashed, const char *env,
                         bool safe_gpu, pc_app_gpu_os os, pc_app_gpu_plan *out)
{
    memset(out, 0, sizeof *out);
    const char *e = env;
    while (e && (*e == ' ' || *e == '\t')) {
        e++;
    }
    if (e && *e != '\0') {
        out->backend = safe_gpu ? PC_APP_GPU_CPU : PC_APP_GPU_AUTO;
        out->has_env = true;
        (void)snprintf(out->env, sizeof out->env, "%s", env);
        set_reason(out, "WGPU_BACKEND=%s overrides the backend", env);
        return;
    }
    if (safe_gpu) {
        out->backend = PC_APP_GPU_CPU;
        set_reason(out, "--safe-gpu: CPU renderer for this launch");
        return;
    }
    pref = pc_app_gpu_sanitize(pref, os);
    pc_app_gpu_backend tried;
    if (crashed && pc_app_gpu_marker_tried(crashed, &tried)) {
        pc_app_gpu_backend next = pc_app_gpu_next_safer(tried, os);
        out->backend = next;
        out->remember = next != pref;
        if (crashed->adapter[0] != '\0') {
            set_reason(out, "the previous start didn't finish on %s (%s); using %s",
                       pc_app_gpu_backend_name(tried), crashed->adapter, pc_app_gpu_backend_name(next));
        } else {
            set_reason(out, "the previous start didn't finish on %s; using %s",
                       pc_app_gpu_backend_name(tried), pc_app_gpu_backend_name(next));
        }
        return;
    }
    out->backend = pref;
}

void pc_app_gpu_plan_with_mode(pc_app_gpu_backend pref, pc_app_render_mode mode, const pc_app_gpu_marker *crashed,
                           const char *env, bool safe_gpu, pc_app_gpu_os os, pc_app_gpu_plan *out)
{
    pc_app_gpu_backend backend;
    if (mode == PC_APP_RENDER_CPU) {
        backend = PC_APP_GPU_CPU;
    } else if ((mode == PC_APP_RENDER_AUTO || mode == PC_APP_RENDER_GPU) && pref == PC_APP_GPU_CPU) {
        backend = PC_APP_GPU_AUTO;
    } else {
        backend = pref;
    }
    const char *use_env = (safe_gpu || mode == PC_APP_RENDER_CPU) ? NULL : env;
    const pc_app_gpu_marker *use_crashed = (mode == PC_APP_RENDER_CPU) ? NULL : crashed;
    pc_app_gpu_compute_plan(backend, use_crashed, use_env, safe_gpu, os, out);
}

bool pc_app_gpu_keep_marker_after_error(const pc_app_gpu_plan *plan, pc_app_gpu_os os)
{
    return !plan->has_env && pc_app_gpu_next_safer(plan->backend, os) != plan->backend;
}

void pc_app_gpu_read_rendering_prefs(const char *prefs_path, pc_app_gpu_backend *backend, pc_app_render_mode *mode)
{
    pc_app_gpu_backend b = PC_APP_GPU_AUTO;
    pc_app_render_mode m = PC_APP_RENDER_AUTO;
    bool have_backend = false;
    pc_buf buf = PC_BUF_INIT;
    pc_error err;
    if (prefs_path && pc_file_read(prefs_path, PC_MAX_ALLOC, &buf, &err) == PC_OK) {
        yyjson_doc *doc = NULL;
        if (pc_json_parse((const char *)buf.data, buf.len, &doc, &err) == PC_OK && doc) {
            yyjson_val *perf = pc_json_get(yyjson_doc_get_root(doc), "performance");
            if (perf) {
                size_t len = 0;
                const char *bs = pc_json_as_str(pc_json_get(perf, "gpuBackend"), &len);
                have_backend = bs && pc_app_gpu_backend_parse(bs, &b);
                bool use_gpu = true;
                (void)pc_json_as_bool(pc_json_get(perf, "useGpu"), &use_gpu);
                const char *ms = pc_json_as_str(pc_json_get(perf, "renderingMode"), &len);
                pc_app_render_mode explicit;
                if (ms && pc_app_render_mode_parse(ms, &explicit)) {
                    m = explicit;
                } else {
                    m = (!use_gpu || b == PC_APP_GPU_CPU) ? PC_APP_RENDER_CPU : PC_APP_RENDER_AUTO;
                }
            }
        }
        yyjson_doc_free(doc);
    }
    pc_buf_fini(&buf);
    if (backend) {
        *backend = have_backend ? b : PC_APP_GPU_AUTO;
    }
    if (mode) {
        *mode = m;
    }
}

/* ---- sentinel ---- */

static bool try_lock(int fd)
{
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0; /* whole file */
    if (fcntl(fd, F_SETLK, &fl) == 0) {
        return true;
    }
    return false; /* EACCES/EAGAIN = busy; other errors treated as not acquired */
}

pc_app_gpu_prev pc_app_gpu_sentinel_begin(const char *dir, pc_app_gpu_sentinel *sent, bool *created,
                                  pc_app_gpu_marker *prev_marker)
{
    if (created) {
        *created = false;
    }
    if (sent) {
        memset(sent, 0, sizeof *sent);
        sent->fd = -1;
    }
    pc_error err;
    if (pc_mkdir_p(dir, &err) != PC_OK) {
        pc_log(PC_LOG_WARN, "GPU startup marker: %s", err.msg);
        return PC_APP_GPU_PREV_CLEAN;
    }
    char path[PC_APP_PATH_MAX];
    (void)snprintf(path, sizeof path, "%s/%s", dir, PC_APP_GPU_MARKER_FILE);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        pc_log(PC_LOG_WARN, "GPU startup marker %s: %s", path, strerror(errno));
        return PC_APP_GPU_PREV_CLEAN;
    }
    if (!try_lock(fd)) {
        (void)close(fd);
        return PC_APP_GPU_PREV_BUSY;
    }
    char text[MARKER_MAX + 1];
    ssize_t n = read(fd, text, MARKER_MAX);
    size_t tlen = n > 0 ? (size_t)n : 0;
    text[tlen] = '\0';
    bool blank = true;
    for (size_t i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != '\v') {
            blank = false;
            break;
        }
    }
    pc_app_gpu_prev prev = PC_APP_GPU_PREV_CLEAN;
    if (!blank) {
        prev = PC_APP_GPU_PREV_CRASHED;
        if (prev_marker) {
            pc_app_gpu_marker_parse(text, tlen, prev_marker);
        }
    }
    if (sent) {
        sent->fd = fd;
        sent->open = true;
        (void)snprintf(sent->path, sizeof sent->path, "%s", path);
        if (created) {
            *created = true;
        }
    } else {
        (void)close(fd);
    }
    return prev;
}

pc_status pc_app_gpu_sentinel_write(pc_app_gpu_sentinel *s, const pc_app_gpu_marker *m, pc_error *err)
{
    if (!s || !s->open || s->fd < 0) {
        return pc_fail(err, PC_ERR_INVALID, "marker closed");
    }
    char text[1024];
    pc_app_gpu_marker_to_json(m, text, sizeof text);
    size_t len = strlen(text);
    if (ftruncate(s->fd, 0) != 0 || lseek(s->fd, 0, SEEK_SET) != 0) {
        return pc_fail_errno(err, PC_ERR_IO, errno, "GPU startup marker");
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(s->fd, text + off, len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return pc_fail_errno(err, PC_ERR_IO, errno, "GPU startup marker");
        }
        off += (size_t)w;
    }
    s->marker = *m;
    return PC_OK;
}

void pc_app_gpu_sentinel_finish(pc_app_gpu_sentinel *s)
{
    if (!s || !s->open) {
        return;
    }
    if (s->fd >= 0) {
        if (ftruncate(s->fd, 0) != 0) {
            /* empty means "no crash"; if truncation fails we still unlink below */
        }
        (void)close(s->fd);
        s->fd = -1;
    }
    (void)unlink(s->path);
    s->open = false;
}

void pc_app_gpu_sentinel_release(pc_app_gpu_sentinel *s)
{
    if (!s || !s->open) {
        return;
    }
    if (s->fd >= 0) {
        (void)close(s->fd); /* releases the lock; leaves the marker on disk */
        s->fd = -1;
    }
    s->open = false;
}
