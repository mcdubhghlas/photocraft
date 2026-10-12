/* Port of crates/io/tests/corpus.rs and tiff_corpus.rs (the synthetic composite.rs and
 * corpus_regressions.rs are in test_io_composite.c).
 *
 * corpus.rs: every corpus/{psd,psd-tools,photoshop} file is imported and flattened against its
 * own merged composite (or its embedded thumbnail), exported and re-imported, with the same
 * pass / round-trip floors as the reference; plus the psd-tools truncation/mutation sweep.
 * tiff_corpus.rs: every importable corpus PSD round-trips through a layered TIFF like it does
 * through PSD.
 *
 * The corpus tests read corpus/ relative to the cwd (the repo root under `make check`) and skip
 * with a message when it is absent (`make corpus` fetches it). The reference catches panics
 * per file; C has none to catch, so a crash ends the run (a failure all the same). The per-file
 * table corpus.rs prints goes to stderr only with PHOTOCRAFT_CORPUS_VERBOSE=1; summaries always
 * print. PHOTOCRAFT_CORPUS_STRICT and CORPUS_THUMB_CALIBRATE work as in the reference. */
#include "harness.h"

#include <dirent.h>
#include <strings.h>
#include <time.h>

#include "codecs/codecs.h"
#include "codecs/image.h"
#include "psd/builder.h"
#include "psd_rt.h"
#include "vector/vector.h"

/* ================================================================ shared helpers */

typedef float px4[4];

/* common::max_diff on plain arrays (pixels where either shows; premultiplied colour, alpha). */
static float max_diff(const px4 *a, size_t na, const px4 *b, size_t nb)
{
    float m = 0.0f;
    for (size_t i = 0; i < na && i < nb; i++) {
        const float *p = a[i];
        const float *q = b[i];
        if (p[3] <= 0.0f && q[3] <= 0.0f) {
            continue;
        }
        for (int c = 0; c < 4; c++) {
            float x = c < 3 ? p[c] * p[3] : p[c];
            float y = c < 3 ? q[c] * q[3] : q[c];
            m = fmaxf(m, fabsf(x - y));
        }
    }
    return m;
}

static pc_compose_buffer flatten(const pc_document *d)
{
    pc_compose_buffer b;
    pc_error e;
    if (pc_compose_flatten(d, &b, &e) != PC_OK) {
        (void)fprintf(stderr, "flatten: %s\n", e.msg);
        abort();
    }
    return b;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool env_set(const char *name)
{
    return getenv(name) != NULL;
}

static double now_s(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ================================================================ corpus file lists */

typedef struct paths {
    char **v;
    size_t n;
    size_t cap;
} paths;

static void paths_push(paths *out, const char *p)
{
    if (out->n == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 64;
        out->v = realloc(out->v, out->cap * sizeof *out->v);
        if (out->v == NULL) {
            abort();
        }
    }
    out->v[out->n++] = xstrdup(p);
}

static void paths_fini(paths *p)
{
    for (size_t i = 0; i < p->n; i++) {
        free(p->v[i]);
    }
    free(p->v);
    memset(p, 0, sizeof *p);
}

/* Every *.psd / *.psb under dir (recursively). */
static void collect(const char *dir, paths *out)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char p[2048];
        size_t n = strlen(e->d_name);
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        (void)snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_dir(p)) {
            collect(p, out);
        } else if (n > 4 && (strcasecmp(e->d_name + n - 4, ".psd") == 0 ||
                             strcasecmp(e->d_name + n - 4, ".psb") == 0)) {
            paths_push(out, p);
        }
    }
    (void)closedir(d);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void sort_paths(paths *p)
{
    if (p->n > 1) {
        qsort(p->v, p->n, sizeof *p->v, cmp_str);
    }
}

static bool read_file(const char *path, pc_buf *out)
{
    return pc_file_read(path, SIZE_MAX / 2, out, NULL) == PC_OK;
}

/* ================================================================ corpus.rs */

#define PASS_TOL (2.0f / 255.0f)
/* Export -> re-import must render within one 8-bit step of the imported document. */
#define ROUNDTRIP_TOL (1.0f / 255.0f + 1e-5f)
#define DISSOLVE_BLOCK 16
#define DISSOLVE_TOL 0.1f
#define THUMB_MEAN_TOL (3.0f / 255.0f)
#define THUMB_BAD (24.0f / 255.0f)
#define THUMB_BAD_FRAC 0.02f

static bool g_verbose;

#define VLOG(...)                                                                                            \
    do {                                                                                                     \
        if (g_verbose) {                                                                                     \
            (void)fprintf(stderr, __VA_ARGS__);                                                              \
        }                                                                                                    \
    } while (0)

static float pm(const float *p, int c)
{
    return c < 3 ? p[c] * p[3] : p[3];
}

/* Oracle for documents with Dissolve layers (see corpus.rs dissolve_matches): outside the
 * dissolve layers' bounds pixels match within 2/255; inside them the premultiplied colour
 * averaged over 16 x 16 blocks agrees within 0.1. */
static bool dissolve_matches(const pc_document *doc, const px4 *ours, size_t no, const px4 *ps, size_t np)
{
    pc_rect canvas = pc_document_bounds(doc);
    pc_walk w;
    pc_rect *regions;
    size_t nr = 0;
    int32_t width = (int32_t)pc_rect_width(canvas);
    int32_t height = (int32_t)pc_rect_height(canvas);
    bool ok = true;
    must(pc_document_walk(doc, &w, NULL));
    regions = xmalloc((w.len + 1) * sizeof *regions);
    for (size_t i = 0; i < w.len; i++) {
        const pc_layer *l = w.items[i].layer;
        if (l->visible && l->blend == PC_BLEND_DISSOLVE) {
            pc_rect r = pc_rect_intersect(pc_compose_layer_bounds(l, canvas), canvas);
            if (!pc_rect_is_empty(r)) {
                regions[nr++] = r;
            }
        }
    }
    pc_walk_fini(&w);
    if (nr == 0 || no != np || no != (size_t)width * (size_t)height) {
        free(regions);
        return false;
    }
    for (int32_t y = 0; y < height && ok; y++) {
        for (int32_t x = 0; x < width && ok; x++) {
            size_t i = (size_t)y * (size_t)width + (size_t)x;
            bool inside = false;
            for (size_t k = 0; k < nr && !inside; k++) {
                inside = pc_rect_contains(regions[k], x + canvas.x0, y + canvas.y0);
            }
            if (!inside) {
                for (int c = 0; c < 4; c++) {
                    if (fabsf(pm(ours[i], c) - pm(ps[i], c)) > PASS_TOL) {
                        ok = false;
                    }
                }
            }
        }
    }
    free(regions);
    if (!ok) {
        return false;
    }
    for (int32_t by = 0; by < height; by += DISSOLVE_BLOCK) {
        for (int32_t bx = 0; bx < width; bx += DISSOLVE_BLOCK) {
            int32_t x1 = bx + DISSOLVE_BLOCK < width ? bx + DISSOLVE_BLOCK : width;
            int32_t y1 = by + DISSOLVE_BLOCK < height ? by + DISSOLVE_BLOCK : height;
            float n = (float)((x1 - bx) * (y1 - by));
            for (int c = 0; c < 4; c++) {
                float a = 0.0f;
                float b = 0.0f;
                for (int32_t y = by; y < y1; y++) {
                    for (int32_t x = bx; x < x1; x++) {
                        size_t i = (size_t)y * (size_t)width + (size_t)x;
                        a += pm(ours[i], c);
                        b += pm(ps[i], c);
                    }
                }
                if (fabsf((a - b) / n) > DISSOLVE_TOL) {
                    return false;
                }
            }
        }
    }
    return true;
}

typedef float px3[3];

/* Photoshop's embedded thumbnail (resource 1036: a 28-byte header, then JFIF) as RGB floats. */
static px3 *thumbnail(const pc_psd_file *file, size_t *tw, size_t *th)
{
    const pc_psd_resource *r = pc_psd_file_resource(file, PC_PSD_RES_THUMBNAIL);
    pc_cimage img;
    uint8_t *rgba = NULL;
    size_t n = 0;
    px3 *out;
    size_t count;
    if (r == NULL || r->data.len < 4) {
        return NULL;
    }
    if (((uint32_t)r->data.data[0] << 24 | (uint32_t)r->data.data[1] << 16 | (uint32_t)r->data.data[2] << 8 |
         r->data.data[3]) != 1) {
        return NULL;
    }
    if (r->data.len < 28 || pc_cdecode(r->data.data + 28, r->data.len - 28, &img, NULL) != PC_OK) {
        return NULL;
    }
    if (pc_cimage_to_rgba8(&img, &rgba, &n, NULL) != PC_OK) {
        pc_cimage_fini(&img);
        return NULL;
    }
    count = (size_t)img.width * img.height;
    if (n < count * 4) {
        count = n / 4;
    }
    out = xmalloc((count + 1) * sizeof *out);
    for (size_t i = 0; i < count; i++) {
        for (int c = 0; c < 3; c++) {
            out[i][c] = (float)rgba[i * 4 + (size_t)c] / 255.0f;
        }
    }
    *tw = img.width;
    *th = img.height;
    if (count != (size_t)img.width * img.height) {
        *th = 0; /* rejected below (thumb.len() != tw * th) */
    }
    free(rgba);
    pc_cimage_fini(&img);
    return out;
}

/* 3x3 box blur of an RGB plane (edges clamp). */
static px3 *blur3(px3 *v, size_t w, size_t h)
{
    px3 *out = xmalloc((w * h + 1) * sizeof *out);
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            float acc[3] = {0.0f, 0.0f, 0.0f};
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int64_t sx = (int64_t)x + dx;
                    int64_t sy = (int64_t)y + dy;
                    sx = sx<0 ? 0 : sx>(int64_t) w - 1 ? (int64_t)w - 1 : sx;
                    sy = sy<0 ? 0 : sy>(int64_t) h - 1 ? (int64_t)h - 1 : sy;
                    for (int c = 0; c < 3; c++) {
                        acc[c] += v[(size_t)sy * w + (size_t)sx][c];
                    }
                }
            }
            for (int c = 0; c < 3; c++) {
                out[y * w + x][c] = acc[c] / 9.0f;
            }
        }
    }
    return out;
}

/* Our composite over white box-downsampled to the thumbnail's size, both lightly blurred: the
 * mean absolute error and the share of pixels off by more than THUMB_BAD. false without a
 * usable thumbnail. */
static bool thumbnail_oracle(const pc_psd_file *file, const px4 *ours, size_t nours, float *mean, float *frac)
{
    size_t tw = 0;
    size_t th = 0;
    px3 *thumb = thumbnail(file, &tw, &th);
    size_t w = file->header.width;
    size_t h = file->header.height;
    px3 *small;
    px3 *a;
    px3 *b;
    float sum = 0.0f;
    size_t bad = 0;
    if (thumb == NULL) {
        return false;
    }
    if (tw < 8 || th < 8 || tw > w || th > h || nours != w * h) {
        free(thumb);
        return false;
    }
    small = xmalloc(tw * th * sizeof *small);
    for (size_t ty = 0; ty < th; ty++) {
        for (size_t tx = 0; tx < tw; tx++) {
            size_t x0 = tx * w / tw;
            size_t x1 = (tx + 1) * w / tw;
            size_t y0 = ty * h / th;
            size_t y1 = (ty + 1) * h / th;
            float acc[3] = {0.0f, 0.0f, 0.0f};
            float n;
            x1 = x1 > x0 + 1 ? x1 : x0 + 1;
            x1 = x1 < w ? x1 : w;
            y1 = y1 > y0 + 1 ? y1 : y0 + 1;
            y1 = y1 < h ? y1 : h;
            for (size_t y = y0; y < y1; y++) {
                for (size_t x = x0; x < x1; x++) {
                    const float *p = ours[y * w + x];
                    for (int c = 0; c < 3; c++) {
                        acc[c] += p[c] * p[3] + 1.0f - p[3];
                    }
                }
            }
            n = (float)((x1 - x0) * (y1 - y0));
            for (int c = 0; c < 3; c++) {
                small[ty * tw + tx][c] = acc[c] / n;
            }
        }
    }
    a = blur3(small, tw, th);
    b = blur3(thumb, tw, th);
    for (size_t i = 0; i < tw * th; i++) {
        float d[3];
        bool any = false;
        for (int c = 0; c < 3; c++) {
            d[c] = fabsf(a[i][c] - b[i][c]);
            any = any || d[c] > THUMB_BAD;
        }
        sum += (d[0] + d[1] + d[2]) / 3.0f;
        bad += any ? 1u : 0u;
    }
    *mean = sum / (float)(tw * th);
    *frac = (float)bad / (float)(tw * th);
    free(a);
    free(b);
    free(small);
    free(thumb);
    return true;
}

typedef enum outcome { OUT_PASS, OUT_THUMB_PASS, OUT_DIFF, OUT_SKIP, OUT_ERROR } outcome;

typedef struct source {
    const char *label;
    const char *dir; /* relative to the repo root (the cwd) */
    size_t group_depth;
    size_t pass_floor;
    size_t roundtrip_floor;
} source;

/* Hand-picked mix in corpus/psd (170 files: 134 vs the merged image + 12 vs the thumbnail). */
static const source MIXED = {"io corpus", "corpus/psd", 0, 146, 169};
/* The full psd-tools test set (309 files): 226 vs the merged image + 10 vs the thumbnail. */
static const source PSD_TOOLS = {"psd-tools corpus", "corpus/psd-tools", 0, 236, 307};
/* Our Photoshop-authored oracles (256 files), grouped by feature. */
static const source PHOTOSHOP = {"photoshop oracles", "corpus/photoshop", 2, 133, 258};

/* Files known to be invalid upstream: their parse error is expected. */
static const char *const KNOWN_BAD[] = {"group-divider-blend-mode.psd"};

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s);
    size_t b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

static size_t g_block_errors;
static pc_buf g_block_error_names;

typedef struct check_result {
    outcome out;
    bool has_rt;
    float rt;
} check_result;

static check_result check_file(const char *name, const uint8_t *bytes, size_t len)
{
    check_result res = {OUT_ERROR, false, 0.0f};
    bool known_bad = false;
    pc_psd_file file;
    pc_io_import_result imp;
    pc_io_export_result ex;
    pc_io_import_result again_imp;
    pc_compose_buffer ours;
    pc_compose_buffer again;
    pc_error e;
    size_t layers = 0;
    bool real = true;
    bool use_thumb;
    float (*merged)[4] = NULL;
    size_t nm = 0;
    char notes[512];
    for (size_t i = 0; i < sizeof KNOWN_BAD / sizeof *KNOWN_BAD; i++) {
        known_bad = known_bad || ends_with(name, KNOWN_BAD[i]);
    }
    if (pc_psd_file_parse(bytes, len, &file, &e) != PC_OK) {
        VLOG("%-60s %6s %9s %8s  %s %s\n", name, "-", "-", "-", known_bad ? "KNOWN-BAD" : "PARSE-ERROR",
             e.msg);
        res.out = known_bad ? OUT_SKIP : OUT_ERROR;
        return res;
    }
    if (pc_io_import(name, bytes, len, &imp, &e) != PC_OK) {
        VLOG("%-60s IMPORT-ERROR %s\n", name, e.msg);
        pc_psd_file_fini(&file);
        return res;
    }
    /* Export must always succeed, re-parse and re-import. */
    if (pc_io_export(&imp.document, "x.psd", NULL, &ex, &e) != PC_OK) {
        VLOG("%-60s EXPORT-ERROR %s\n", name, e.msg);
        pc_io_import_result_fini(&imp);
        pc_psd_file_fini(&file);
        return res;
    }
    {
        pc_psd_file re;
        size_t errs;
        if (pc_psd_file_parse(ex.bytes, ex.len, &re, &e) != PC_OK) {
            VLOG("%-60s REEXPORT-PARSE-ERROR %s\n", name, e.msg);
            pc_io_export_result_fini(&ex);
            pc_io_import_result_fini(&imp);
            pc_psd_file_fini(&file);
            return res;
        }
        pc_psd_file_fini(&re);
        /* Every exported tagged block must re-parse strictly (#200). */
        errs = rt_strict_block_errors(ex.bytes, ex.len);
        if (errs > 0) {
            (void)fprintf(stderr, "%-60s EXPORT-BLOCK-ERROR (%zu, see above)\n", name, errs);
            g_block_errors++;
            sb(&g_block_error_names, "%s%s", g_block_error_names.len > 0 ? ", " : "", name);
        }
    }
    if (pc_io_import(name, ex.bytes, ex.len, &again_imp, &e) != PC_OK) {
        VLOG("%-60s REIMPORT-ERROR %s\n", name, e.msg);
        pc_io_export_result_fini(&ex);
        pc_io_import_result_fini(&imp);
        pc_psd_file_fini(&file);
        return res;
    }
    pc_io_export_result_fini(&ex);
    ours = flatten(&imp.document);
    /* Our own export must not change how the document renders. */
    again = flatten(&again_imp.document);
    res.has_rt = true;
    res.rt = again.len == ours.len
                 ? max_diff((const px4 *)ours.px, ours.len, (const px4 *)again.px, again.len)
                 : INFINITY;
    pc_compose_buffer_fini(&again);
    pc_io_import_result_fini(&again_imp);
    must(pc_document_layer_count(&imp.document, &layers, NULL));
    notes[0] = '\0';
    for (size_t i = 0; i < imp.warnings.n && i < 2; i++) {
        size_t at = strlen(notes);
        (void)snprintf(notes + at, sizeof notes - at, "%s%s", i > 0 ? " | " : "", imp.warnings.v[i]);
    }
    use_thumb = (pc_psd_file_has_real_merged_data(&file, &real) && !real) ||
                file.header.color_mode == PC_PSD_MODE_MULTICHANNEL;
    if (use_thumb || pc_io_merged_composite(&file, &merged, &nm, NULL) != PC_OK) {
        float mean = 0.0f;
        float frac = 0.0f;
        merged = NULL;
        if (thumbnail_oracle(&file, (const px4 *)ours.px, ours.len, &mean, &frac)) {
            bool ok = mean <= THUMB_MEAN_TOL && frac <= THUMB_BAD_FRAC;
            VLOG("%-60s %6zu %9.4f %7.2f%%  %s %s\n", name, layers, (double)mean, 100.0 * (double)frac,
                 ok ? "PASS (thumbnail)" : "DIFF (thumbnail)", notes);
            res.out = ok ? OUT_THUMB_PASS : OUT_DIFF;
        } else {
            VLOG("%-60s %6zu %9s %8s  SKIP (no real composite and no thumbnail)\n", name, layers, "-", "-");
            res.out = OUT_SKIP;
        }
    } else {
        /* Without an alpha channel in the merged image, Photoshop flattened the document over
         * white. */
        float m;
        size_t bad = 0;
        const char *status;
        if (!pc_psd_file_merged_has_alpha(&file)) {
            for (size_t i = 0; i < ours.len; i++) {
                float *p = ours.px[i];
                float wt = 1.0f - p[3];
                p[0] = p[0] * p[3] + wt;
                p[1] = p[1] * p[3] + wt;
                p[2] = p[2] * p[3] + wt;
                p[3] = 1.0f;
            }
        }
        m = max_diff((const px4 *)ours.px, ours.len, (const px4 *)merged, nm);
        for (size_t i = 0; i < ours.len && i < nm; i++) {
            bool any = false;
            for (int c = 0; c < 4; c++) {
                any = any || fabsf(ours.px[i][c] * ours.px[i][3] - merged[i][c] * merged[i][3]) > PASS_TOL;
            }
            bad += any ? 1u : 0u;
        }
        if (m <= PASS_TOL) {
            res.out = OUT_PASS;
            status = "PASS";
        } else if (dissolve_matches(&imp.document, (const px4 *)ours.px, ours.len, (const px4 *)merged, nm)) {
            res.out = OUT_PASS;
            status = "PASS (dissolve metric)";
        } else {
            res.out = OUT_DIFF;
            status = "DIFF";
        }
        VLOG("%-60s %6zu %9.4f %7.2f%%  %s %s\n", name, layers, (double)m,
             100.0 * (double)bad / (double)(ours.len > 0 ? ours.len : 1), status, notes);
        if (env_set("CORPUS_THUMB_CALIBRATE")) {
            float mean;
            float frac;
            if (thumbnail_oracle(&file, (const px4 *)ours.px, ours.len, &mean, &frac)) {
                (void)fprintf(stderr, "THUMBCAL %s %.4f %.4f %s\n", status, (double)mean, (double)frac, name);
            }
        }
    }
    free(merged);
    pc_compose_buffer_fini(&ours);
    pc_io_import_result_fini(&imp);
    pc_psd_file_fini(&file);
    return res;
}

/* Feature group of a corpus-relative path: its first `depth` directories. */
static void group_of(const char *name, size_t depth, char *out, size_t cap)
{
    size_t parts = 1;
    size_t n;
    size_t seen = 0;
    size_t i = 0;
    for (const char *p = name; *p; p++) {
        parts += (*p == '/' || *p == '\\') ? 1u : 0u;
    }
    n = depth < parts - 1 ? depth : parts - 1;
    if (n == 0) {
        (void)snprintf(out, cap, ".");
        return;
    }
    for (; name[i] != '\0'; i++) {
        if (name[i] == '/' || name[i] == '\\') {
            if (++seen == n) {
                break;
            }
        }
    }
    (void)snprintf(out, cap, "%.*s", (int)i, name);
    for (size_t k = 0; out[k] != '\0'; k++) {
        out[k] = out[k] == '\\' ? '/' : out[k];
    }
}

typedef struct group_count {
    char name[256];
    size_t pass;
    size_t total;
} group_count;

static int cmp_group(const void *a, const void *b)
{
    return strcmp(((const group_count *)a)->name, ((const group_count *)b)->name);
}

static void run_oracle(const source *src)
{
    paths files = {NULL, 0, 0};
    group_count *groups = NULL;
    size_t ngroups = 0;
    size_t pass = 0, thumb_pass = 0, diff = 0, skipped = 0, errors = 0, rt_same = 0, rt_diff = 0;
    pc_buf rt_list = PC_BUF_INIT;
    size_t root_len = strlen(src->dir) + 1;
    double t0 = now_s();
    if (!is_dir(src->dir)) {
        (void)printf("  %s: %s absent: skipped (run `make corpus`)\n", src->label, src->dir);
        return;
    }
    g_verbose = env_set("PHOTOCRAFT_CORPUS_VERBOSE");
    g_block_errors = 0;
    pc_buf_truncate(&g_block_error_names, 0);
    collect(src->dir, &files);
    sort_paths(&files);
    groups = xmalloc((files.n + 1) * sizeof *groups);
    VLOG("%-60s %6s %9s %8s  status\n", "file", "layers", "max_err", "bad_px%");
    for (size_t i = 0; i < files.n; i++) {
        const char *name = files.v[i] + root_len;
        pc_buf bytes = PC_BUF_INIT;
        check_result r;
        char g[256];
        size_t gi = 0;
        if (!read_file(files.v[i], &bytes)) {
            pc_buf_truncate(&bytes, 0); /* the reference reads unwrap_or_default */
        }
        r = check_file(name, bytes.data, bytes.len);
        pc_buf_fini(&bytes);
        group_of(name, src->group_depth, g, sizeof g);
        while (gi < ngroups && strcmp(groups[gi].name, g) != 0) {
            gi++;
        }
        if (gi == ngroups) {
            (void)snprintf(groups[gi].name, sizeof groups[gi].name, "%s", g);
            groups[gi].pass = 0;
            groups[gi].total = 0;
            ngroups++;
        }
        groups[gi].total++;
        if (r.out == OUT_PASS || r.out == OUT_THUMB_PASS) {
            groups[gi].pass++;
        }
        switch (r.out) {
        case OUT_PASS:
            pass++;
            break;
        case OUT_THUMB_PASS:
            thumb_pass++;
            break;
        case OUT_DIFF:
            diff++;
            break;
        case OUT_SKIP:
            skipped++;
            break;
        case OUT_ERROR:
            errors++;
            break;
        }
        if (r.has_rt) {
            if (r.rt <= ROUNDTRIP_TOL) {
                rt_same++;
            } else {
                rt_diff++;
                sb(&rt_list, "%s%s (%.4f)", rt_list.len > 0 ? ", " : "", name, (double)r.rt);
            }
        }
    }
    (void)fprintf(
        stderr,
        "%s: %zu files: %zu pass (%zu vs the merged image <= 2/255, %zu vs the thumbnail), %zu differ, "
        "%zu skipped, %zu errors, 0 crashes (%.1f s)\n",
        src->label, files.n, pass + thumb_pass, pass, thumb_pass, diff, skipped, errors, now_s() - t0);
    pass += thumb_pass;
    if (src->group_depth > 0) {
        qsort(groups, ngroups, sizeof *groups, cmp_group);
        for (size_t i = 0; i < ngroups; i++) {
            (void)fprintf(stderr, "%s:   %-28s %4zu / %-4zu pass\n", src->label, groups[i].name,
                          groups[i].pass, groups[i].total);
        }
    }
    (void)fprintf(stderr, "%s: export -> re-import renders the same for %zu files; differs for %zu: %.*s\n",
                  src->label, rt_same, rt_diff, (int)rt_list.len,
                  rt_list.data != NULL ? (const char *)rt_list.data : "");
    (void)fprintf(stderr, "%s: exported files whose tagged blocks fail the strict re-parse: %zu\n",
                  src->label, g_block_errors);
    if (env_set("PHOTOCRAFT_CORPUS_STRICT")) {
        CHECK_EQ_UINT(errors, 0);
    }
    pc_t_checks++;
    if (g_block_errors != 0) {
        PC_T_FAIL("%s: exported tagged blocks fail the strict re-parse (#200): %.*s", src->label,
                  (int)g_block_error_names.len, (const char *)g_block_error_names.data);
    }
    pc_t_checks++;
    if (pass < src->pass_floor) {
        PC_T_FAIL("%s: oracle pass count %zu fell below the floor %zu", src->label, pass, src->pass_floor);
    }
    pc_t_checks++;
    if (rt_same < src->roundtrip_floor) {
        PC_T_FAIL("%s: export round trip %zu fell below the floor %zu", src->label, rt_same,
                  src->roundtrip_floor);
    }
    pc_buf_fini(&rt_list);
    free(groups);
    paths_fini(&files);
}

/* Deterministic xorshift for the mutation sweep. */
static uint64_t next_rand(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static void mutate_run(const char *name, const uint8_t *data, size_t len, const char *what, size_t *slow)
{
    double t = now_s();
    pc_io_import_result imp;
    double secs;
    if (pc_io_import(name, data, len, &imp, NULL) == PC_OK) {
        pc_rect b = pc_document_bounds(&imp.document);
        if ((int64_t)pc_rect_width(b) * (int64_t)pc_rect_height(b) <= 4000000) {
            pc_compose_buffer f;
            if (pc_compose_flatten(&imp.document, &f, NULL) == PC_OK) {
                pc_compose_buffer_fini(&f);
            }
        }
        pc_io_import_result_fini(&imp);
    }
    secs = now_s() - t;
    if (secs > 20.0) {
        (void)fprintf(stderr, "SLOW %s %s: %.1fs\n", name, what, secs);
        (*slow)++;
    }
}

/* Truncations and byte corruptions of every corpus file: import (and flatten when the document
 * is small) must return. Files are mutated in their first 64 KB. */
static void run_mutations(const source *src)
{
    paths files = {NULL, 0, 0};
    size_t runs = 0;
    size_t slow = 0;
    size_t root_len = strlen(src->dir) + 1;
    double t0 = now_s();
    if (!is_dir(src->dir)) {
        (void)printf("  %s: %s absent: skipped (run `make corpus`)\n", src->label, src->dir);
        return;
    }
    collect(src->dir, &files);
    sort_paths(&files);
    for (size_t i = 0; i < files.n; i++) {
        const char *name = files.v[i] + root_len;
        pc_buf bytes = PC_BUF_INIT;
        uint64_t seed;
        size_t span;
        if (!read_file(files.v[i], &bytes)) {
            continue;
        }
        seed = 0x9E3779B97F4A7C15ull ^ (uint64_t)bytes.len;
        span = bytes.len < 1 ? 1 : bytes.len > 64 * 1024 ? 64 * 1024 : bytes.len;
        for (size_t k = 1; k < 8; k++) {
            size_t cut = bytes.len * k / 8;
            char what[64];
            (void)snprintf(what, sizeof what, "truncate@%zu", cut);
            runs++;
            mutate_run(name, bytes.data, cut, what, &slow);
        }
        for (int v = 0; v < 24; v++) {
            uint8_t *m = xmalloc(bytes.len);
            size_t flips;
            if (bytes.len > 0) {
                memcpy(m, bytes.data, bytes.len);
            }
            flips = 1 + (size_t)(next_rand(&seed) % 4);
            for (size_t f = 0; f < flips; f++) {
                size_t at = (size_t)(next_rand(&seed) % (uint64_t)span);
                if (at < bytes.len) {
                    switch (next_rand(&seed) % 3) {
                    case 0:
                        m[at] = 0xFF;
                        break;
                    case 1:
                        m[at] = 0x00;
                        break;
                    default:
                        m[at] = (uint8_t)(m[at] ^ (1u << (next_rand(&seed) % 8)));
                        break;
                    }
                }
            }
            runs++;
            mutate_run(name, m, bytes.len, "mutate", &slow);
            free(m);
        }
        pc_buf_fini(&bytes);
    }
    (void)fprintf(stderr, "%s: %zu mutated imports, 0 panics, %zu slow (> 20 s) (%.1f s)\n", src->label, runs,
                  slow, now_s() - t0);
    CHECK(runs > 0);
    paths_fini(&files);
}

static void test_corpus_import_flatten_oracle(void)
{
    run_oracle(&MIXED);
}
static void test_psd_tools_import_flatten_oracle(void)
{
    run_oracle(&PSD_TOOLS);
}
static void test_photoshop_oracle_corpus(void)
{
    run_oracle(&PHOTOSHOP);
}
static void test_psd_tools_mutations_never_panic(void)
{
    run_mutations(&PSD_TOOLS);
}

/* ================================================================ tiff_corpus.rs */

static void structure(const pc_layer *layers, size_t n, pc_buf *out, size_t depth)
{
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = &layers[i];
        for (size_t k = 0; k < depth; k++) {
            sb(out, " ");
        }
        sb(out, "%s [%s] %s %.3f mask=%s fx=%zu\n", pc_str_c(&l->name),
           pc_layer_content_kind_name(&l->content), pc_blend_name(l->blend), (double)l->opacity,
           l->has_mask ? "true" : "false", l->effects.nitems);
        if (l->content.kind == PC_LAYER_GROUP) {
            structure(l->content.u.group.children, l->content.u.group.nchildren, out, depth + 1);
        }
    }
}

static float tiff_max_diff(const pc_compose_buffer *a, const pc_compose_buffer *b)
{
    float m = 0.0f;
    for (size_t i = 0; i < a->len && i < b->len; i++) {
        const float *p = a->px[i];
        const float *q = b->px[i];
        for (int c = 0; c < 4; c++) {
            m = fmaxf(m, fabsf(p[c] * p[3] - q[c] * q[3]));
        }
    }
    return m;
}

/* Keys the io crate reads from layer records or global blocks. */
static const char MAPPED[][5] = {
    "lfx2", "lmfx", "lrFX", "TySh", "SoLd", "SoLE", "PlLd", "vmsk", "vsms", "vogk", "vscg", "vstk", "SoCo",
    "GdFl", "PtFl", "levl", "curv", "hue2", "brit", "thrs", "post", "expA", "vibA", "blnc", "mixr", "grdm",
    "phfl", "selc", "blwh", "clrL", "luni", "lsct", "lyid", "lclr", "iOpa", "clbl", "infx", "knko", "lspf",
    "fxrp", "brst", "shmd", "Patt", "FEid", "FXid", "FMsk", "artb", "artd", "cinf"};

typedef struct dropped_key {
    char key[64];
    size_t n;
} dropped_key;

static int cmp_dropped(const void *a, const void *b)
{
    return strcmp(((const dropped_key *)a)->key, ((const dropped_key *)b)->key);
}

/* One file: NULL on success, else an owned failure message. */
static char *tiff_round_trip(const pc_document *doc, dropped_key *dropped, size_t *ndropped)
{
    pc_io_export_result psd;
    pc_io_export_result out;
    pc_io_import_result oracle;
    pc_io_import_result back;
    pc_io_export_opts opts = pc_io_export_opts_default();
    pc_error e;
    pc_buf msg = PC_BUF_INIT;
    char *ret = NULL;
    bool flat;
    if (pc_io_export(doc, "x.psd", NULL, &psd, &e) != PC_OK) {
        sb(&msg, "psd export: %s", e.msg);
        goto done0;
    }
    if (pc_io_import("x.psd", psd.bytes, psd.len, &oracle, &e) != PC_OK) {
        sb(&msg, "psd re-import: %s", e.msg);
        pc_io_export_result_fini(&psd);
        goto done0;
    }
    pc_io_export_result_fini(&psd);
    opts.tiff_layers = true;
    if (pc_io_export(doc, "x.tif", &opts, &out, &e) != PC_OK) {
        sb(&msg, "export: %s", e.msg);
        goto done1;
    }
    for (size_t i = 0; i < out.warnings.n; i++) {
        const char *w = out.warnings.v[i];
        const char *rest;
        const char *sep;
        size_t klen;
        char key[64];
        size_t k = 0;
        if (strncmp(w, "layer data: ", 12) != 0) {
            continue;
        }
        rest = w + 12;
        sep = strstr(rest, " block (");
        if (sep == NULL) {
            continue;
        }
        klen = (size_t)(sep - rest);
        (void)snprintf(key, sizeof key, "%.*s", (int)klen, rest);
        while (k < *ndropped && strcmp(dropped[k].key, key) != 0) {
            k++;
        }
        if (k == *ndropped && k < 256) {
            (void)snprintf(dropped[k].key, sizeof dropped[k].key, "%s", key);
            dropped[k].n = 0;
            (*ndropped)++;
        }
        if (k < *ndropped) {
            dropped[k].n++;
        }
        for (size_t m = 0; m < sizeof MAPPED / sizeof *MAPPED; m++) {
            if (klen == 4 && memcmp(rest, MAPPED[m], 4) == 0) {
                sb(&msg, "mapped block dropped: %s", w);
                goto done2;
            }
        }
    }
    if (pc_io_import("x.tif", out.bytes, out.len, &back, &e) != PC_OK) {
        sb(&msg, "re-import: %s", e.msg);
        goto done2;
    }
    flat = !pc_io_would_write_layers(doc);
    {
        pc_buf a = PC_BUF_INIT;
        pc_buf b = PC_BUF_INIT;
        structure(oracle.document.layers, oracle.document.nlayers, &a, 0);
        structure(back.document.layers, back.document.nlayers, &b, 0);
        if (!flat && (a.len != b.len || (a.len > 0 && memcmp(a.data, b.data, a.len) != 0))) {
            sb(&msg, "layer structure changed:\n%.*s  --\n%.*s", (int)a.len, (const char *)a.data, (int)b.len,
               (const char *)b.data);
        }
        pc_buf_fini(&a);
        pc_buf_fini(&b);
    }
    if (msg.len == 0) {
        pc_document expected;
        pc_compose_buffer x;
        pc_compose_buffer y;
        float diff;
        must(pc_document_clone(&oracle.document, &expected, NULL));
        for (size_t i = 0; i < expected.nchannels; i++) {
            pc_alpha_channel_fini(&expected.channels[i]);
        }
        expected.nchannels = 0;
        x = flatten(&expected);
        y = flatten(&back.document);
        diff = tiff_max_diff(&x, &y);
        if (diff > 1.0f / 255.0f + 1e-5f) {
            sb(&msg, "render differs by %g", (double)diff);
        }
        pc_compose_buffer_fini(&x);
        pc_compose_buffer_fini(&y);
        pc_document_fini(&expected);
    }
    pc_io_import_result_fini(&back);
done2:
    pc_io_export_result_fini(&out);
done1:
    pc_io_import_result_fini(&oracle);
done0:
    if (msg.len > 0) {
        ret = xmalloc(msg.len + 1);
        memcpy(ret, msg.data, msg.len);
        ret[msg.len] = '\0';
    }
    pc_buf_fini(&msg);
    return ret;
}

static void test_corpus_round_trips_through_layered_tiff(void)
{
    static const char *const SUBS[3] = {"psd", "psd-tools", "photoshop"};
    paths files = {NULL, 0, 0};
    size_t tested = 0;
    size_t failures = 0;
    dropped_key *dropped = xmalloc(256 * sizeof *dropped);
    size_t ndropped = 0;
    double t0 = now_s();
    if (!is_dir("corpus/psd")) {
        (void)printf("  corpus/psd absent: skipped (run `make corpus`)\n");
        free(dropped);
        return;
    }
    for (int s = 0; s < 3; s++) {
        char dir[64];
        (void)snprintf(dir, sizeof dir, "corpus/%s", SUBS[s]);
        collect(dir, &files);
    }
    sort_paths(&files);
    CHECK(files.n > 0);
    for (size_t i = 0; i < files.n; i++) {
        const char *name = files.v[i] + strlen("corpus/");
        pc_buf bytes = PC_BUF_INIT;
        pc_io_import_result imp;
        pc_pixel_format fmt;
        char *err;
        if (!read_file(files.v[i], &bytes)) {
            continue;
        }
        if (pc_io_import(name, bytes.data, bytes.len, &imp, NULL) != PC_OK) {
            pc_buf_fini(&bytes);
            continue;
        }
        pc_buf_fini(&bytes);
        /* Only modes a TIFF holds; Multichannel documents have no layers at all. */
        fmt = pc_document_pixel_format(&imp.document);
        if (!(fmt.mode == PC_COLOR_MODE_GRAYSCALE || fmt.mode == PC_COLOR_MODE_RGB ||
              fmt.mode == PC_COLOR_MODE_CMYK) ||
            imp.document.mode == PC_COLOR_MODE_MULTICHANNEL ||
            (imp.document.size.width > imp.document.size.height ? imp.document.size.width
                                                                : imp.document.size.height) > 4096) {
            pc_io_import_result_fini(&imp);
            continue;
        }
        tested++;
        err = tiff_round_trip(&imp.document, dropped, &ndropped);
        if (err != NULL) {
            (void)printf("FAIL %s: %s\n", name, err);
            failures++;
            free(err);
        }
        pc_io_import_result_fini(&imp);
    }
    (void)printf("layered TIFF round trips: %zu tested, %zu failed, 0 crashed (%.1f s)\n", tested, failures,
                 now_s() - t0);
    if (ndropped > 1) {
        qsort(dropped, ndropped, sizeof *dropped, cmp_dropped);
    }
    (void)printf("blocks dropped on the little-endian path: {");
    for (size_t k = 0; k < ndropped; k++) {
        (void)printf("%s\"%s\": %zu", k > 0 ? ", " : "", dropped[k].key, dropped[k].n);
    }
    (void)printf("}\n");
    CHECK_EQ_UINT(failures, 0);
    pc_t_checks++;
    if (!(tested > 100)) {
        PC_T_FAIL("only %zu files tested", tested);
    }
    free(dropped);
    paths_fini(&files);
}

/* ================================================================ main */

int main(int argc, char **argv)
{
    RUN(test_corpus_import_flatten_oracle);
    RUN(test_psd_tools_import_flatten_oracle);
    RUN(test_photoshop_oracle_corpus);
    RUN(test_psd_tools_mutations_never_panic);
    RUN(test_corpus_round_trips_through_layered_tiff);
    pc_buf_fini(&g_block_error_names);
    return pc_t_finish();
}
