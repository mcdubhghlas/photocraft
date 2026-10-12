/* Corpus composite goldens (REWRITE_PLAN.md section 8, "Corpus composite goldens"): every corpus
 * file's flattened composite against tests/golden/composite/<corpus>/<file>.json, written by
 * `RUSTREF_COMPOSITE_OUT=tests/golden/composite rustref io-extra-vectors` (size, mode and depth
 * or the import error; SHA-256 of the f32 RGBA composite, little-endian with NaNs canonical; of
 * its RGBA8 form; a box-averaged thumbnail of at most 64 x 64 as PNG; per-channel min / max /
 * mean).
 *
 * Each file is graded exact (f32 hash), rgba8 (8-bit hash), near (thumbnail within 2/255 and
 * statistics within 1e-3), error (both sides refuse it) or fail. tests/golden/composite/grades.txt
 * records every file's grade, with a reason for each that is only "near"; a file whose grade
 * drops below its recorded one fails (a ratchet: raise grades when they improve). Without
 * corpus/ the test skips; files whose codec this build lacks (EXR, HEIF) are skipped.
 * PC_COMPOSITE_WRITE_GRADES=1 prints a grades file for the current results instead of checking. */
#include <dirent.h>

#include "harness.h"
#include "io/common.h"

#define GOLDEN_DIR "tests/golden/composite"

/* ---------------------------------------------------------------- SHA-256 (test-only) */

static void sha256_hex(const uint8_t *data, size_t len, char out[65])
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t total = ((len + 9 + 63) / 64) * 64;
    uint64_t bits = (uint64_t)len * 8;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        uint32_t w[64];
        uint32_t v[8];
        for (size_t i = 0; i < 64; i++) {
            size_t at = off + i;
            blk[i] = at < len ? data[at] : at == len ? 0x80 : 0;
        }
        if (off + 64 == total) {
            for (int i = 0; i < 8; i++) {
                blk[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
            }
        }
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 |
                   (uint32_t)blk[4 * i + 2] << 8 | blk[4 * i + 3];
        }
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        memcpy(v, h, sizeof v);
        for (int i = 0; i < 64; i++) {
            uint32_t s1 = ROR(v[4], 6) ^ ROR(v[4], 11) ^ ROR(v[4], 25);
            uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            uint32_t t1 = v[7] + s1 + ch + K[i] + w[i];
            uint32_t s0 = ROR(v[0], 2) ^ ROR(v[0], 13) ^ ROR(v[0], 22);
            uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            memmove(v + 1, v, 7 * sizeof v[0]);
            v[4] += t1;
            v[0] = t1 + s0 + mj;
        }
#undef ROR
        for (int i = 0; i < 8; i++) {
            h[i] += v[i];
        }
    }
    for (int i = 0; i < 8; i++) {
        (void)snprintf(out + 8 * i, 9, "%08x", h[i]);
    }
}

/* ---------------------------------------------------------------- the summary of one file */

static void span(size_t d, size_t s, size_t n, size_t *a, size_t *b)
{
    size_t x = d * s / n;
    size_t y;
    x = x < s - 1 ? x : s - 1;
    y = ((d + 1) * s + n - 1) / n;
    y = y < x + 1 ? x + 1 : y > s ? s : y;
    *a = x;
    *b = y;
}

static uint8_t *thumbnail(const uint8_t *px, size_t w, size_t h, size_t *tw, size_t *th)
{
    uint8_t *out;
    *tw = w < 64 ? w : 64;
    *th = h < 64 ? h : 64;
    out = xmalloc(*tw * *th * 4);
    for (size_t ty = 0; ty < *th; ty++) {
        size_t y0;
        size_t y1;
        span(ty, h, *th, &y0, &y1);
        for (size_t tx = 0; tx < *tw; tx++) {
            size_t x0;
            size_t x1;
            uint64_t sum[4] = {0, 0, 0, 0};
            uint64_t n;
            span(tx, w, *tw, &x0, &x1);
            for (size_t y = y0; y < y1; y++) {
                for (size_t x = x0; x < x1; x++) {
                    for (int c = 0; c < 4; c++) {
                        sum[c] += px[(y * w + x) * 4 + (size_t)c];
                    }
                }
            }
            n = (uint64_t)((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 4; c++) {
                out[(ty * *tw + tx) * 4 + (size_t)c] = (uint8_t)((sum[c] + n / 2) / n);
            }
        }
    }
    return out;
}

typedef enum { G_EXACT, G_RGBA8, G_NEAR, G_ERROR, G_FAIL, G_SKIP } grade;

static const char *grade_name(grade g)
{
    static const char *const N[] = {"exact", "rgba8", "near", "error", "fail", "skip"};
    return N[g];
}

static int rank(grade g)
{
    return g == G_EXACT || g == G_ERROR ? 0 : g == G_RGBA8 ? 1 : g == G_NEAR ? 2 : 3;
}

static bool is_optional_codec(const char *rel)
{
    return strncmp(rel, "exr/", 4) == 0 || strncmp(rel, "heif/", 5) == 0;
}

static grade grade_file(const char *rel, yyjson_val *g, char *why, size_t cap)
{
    char path[1100];
    pc_buf bytes = PC_BUF_INIT;
    pc_io_import_result r;
    pc_error e;
    pc_status st;
    const char *name;
    pc_compose_buffer b;
    pc_rgba8_image img;
    uint8_t *raw;
    char sha[65];
    grade gr = G_FAIL;
    why[0] = '\0';
    (void)snprintf(path, sizeof path, "corpus/%s", rel);
    if (pc_file_read(path, (size_t)1 << 31, &bytes, NULL) != PC_OK) {
        return G_SKIP;
    }
    name = strrchr(rel, '/') != NULL ? strrchr(rel, '/') + 1 : rel;
    st = pc_io_import(name, bytes.data, bytes.len, &r, &e);
    pc_buf_fini(&bytes);
    if (pc_json_get(g, "error") != NULL) {
        const char *ge = pc_json_as_str(pc_json_get(g, "error"), NULL);
        if (ge != NULL && strstr(ge, "isn't included in this build") != NULL) {
            /* The reference was built without that codec: nothing to compare. */
            if (st == PC_OK) {
                pc_io_import_result_fini(&r);
            }
            return G_SKIP;
        }
        if (st == PC_OK) {
            pc_io_import_result_fini(&r);
            (void)snprintf(why, cap, "opened; the reference refuses it: %s",
                           pc_json_as_str(pc_json_get(g, "error"), NULL));
            return G_FAIL;
        }
        return G_ERROR;
    }
    if (st != PC_OK) {
        if (is_optional_codec(rel)) {
            return G_SKIP;
        }
        (void)snprintf(why, cap, "import failed: %s", e.msg);
        return G_FAIL;
    }
    {
        uint64_t w = 0;
        uint64_t h = 0;
        (void)pc_json_as_u64(pc_json_get(g, "width"), &w);
        (void)pc_json_as_u64(pc_json_get(g, "height"), &h);
        if (w != r.document.size.width || h != r.document.size.height ||
            strcmp(pc_json_as_str(pc_json_get(g, "mode"), NULL), pc_color_mode_name(r.document.mode)) != 0 ||
            strcmp(pc_json_as_str(pc_json_get(g, "depth"), NULL), pc_sample_type_name(r.document.depth)) !=
                0) {
            (void)snprintf(why, cap, "size/mode/depth %ux%u %s %s", r.document.size.width,
                           r.document.size.height, pc_color_mode_name(r.document.mode),
                           pc_sample_type_name(r.document.depth));
            pc_io_import_result_fini(&r);
            return G_FAIL;
        }
    }
    must(pc_compose_flatten(&r.document, &b, NULL));
    pc_io_import_result_fini(&r);
    raw = xmalloc(b.len * 16 + 1);
    for (size_t i = 0; i < b.len; i++) {
        for (int c = 0; c < 4; c++) {
            float v = b.px[i][c];
            uint32_t u = isnan(v) ? 0x7fc00000u : fb(v);
            for (int k = 0; k < 4; k++) {
                raw[i * 16 + (size_t)c * 4 + (size_t)k] = (uint8_t)(u >> (8 * k));
            }
        }
    }
    sha256_hex(raw, b.len * 16, sha);
    free(raw);
    must(pc_compose_buffer_to_rgba8(&b, &img, NULL));
    if (strcmp(sha, pc_json_as_str(pc_json_get(g, "f32_sha256"), NULL)) == 0) {
        gr = G_EXACT;
    } else {
        sha256_hex(img.pixels, (size_t)img.width * img.height * 4, sha);
        if (strcmp(sha, pc_json_as_str(pc_json_get(g, "rgba8_sha256"), NULL)) == 0) {
            gr = G_RGBA8;
        } else {
            /* near: the thumbnail within 2/255, statistics within 1e-3 */
            size_t tw;
            size_t th;
            uint8_t *t = thumbnail(img.pixels, img.width, img.height, &tw, &th);
            const char *b64 = pc_json_as_str(pc_json_get(g, "thumb_png"), NULL);
            pc_buf png = PC_BUF_INIT;
            pc_cimage want;
            unsigned mx = 0;
            double sdiff = 0.0;
            must(pc_base64_decode(b64, strlen(b64), &png, NULL));
            must(pc_cdecode(png.data, png.len, &want, NULL));
            if (want.width != tw || want.height != th) {
                mx = 255;
            } else {
                for (size_t k = 0; k < tw * th * 4; k++) {
                    unsigned d = (unsigned)abs((int)want.data[k] - (int)t[k]);
                    mx = d > mx ? d : mx;
                }
            }
            {
                yyjson_val *stats = pc_json_get(g, "stats");
                for (size_t c = 0; c < 4; c++) {
                    yyjson_val *s = yyjson_arr_get(stats, c);
                    double mn = INFINITY;
                    double mxv = -INFINITY;
                    double sum = 0.0;
                    size_t cnt = 0;
                    for (size_t i = 0; i < b.len; i++) {
                        double v = b.px[i][c];
                        if (isfinite(v)) {
                            mn = v < mn ? v : mn;
                            mxv = v > mxv ? v : mxv;
                            sum += v;
                            cnt++;
                        }
                    }
                    if (yyjson_is_arr(s) && cnt > 0) {
                        double gv[3];
                        double cv[3] = {mn, mxv, sum / (double)cnt};
                        for (size_t k = 0; k < 3; k++) {
                            (void)pc_json_as_f64(yyjson_arr_get(s, k), &gv[k]);
                            sdiff = fmax(sdiff, fabs(gv[k] - cv[k]));
                        }
                    } else if (yyjson_is_arr(s) != (cnt > 0)) {
                        sdiff = 1.0;
                    }
                }
            }
            gr = mx <= 2 && sdiff <= 1e-3 ? G_NEAR : G_FAIL;
            (void)snprintf(why, cap, "thumbnail max %u/255, statistics %.2g", mx, sdiff);
            pc_cimage_fini(&want);
            pc_buf_fini(&png);
            free(t);
        }
    }
    free(img.pixels);
    free(b.px);
    return gr;
}

/* ---------------------------------------------------------------- walking the goldens */

typedef struct entry {
    char *rel;
    grade want;
} entry;

static entry *g_grades;
static size_t g_ngrades;

static void load_grades(void)
{
    pc_buf b = PC_BUF_INIT;
    char *p;
    if (pc_file_read(GOLDEN_DIR "/grades.txt", (size_t)1 << 24, &b, NULL) != PC_OK) {
        return;
    }
    p = (char *)b.data;
    while (p != NULL && *p != '\0') {
        char *nl = strchr(p, '\n');
        char *tab;
        if (nl != NULL) {
            *nl = '\0';
        }
        tab = strchr(p, '\t');
        if (p[0] != '#' && tab != NULL) {
            char *g2 = tab + 1;
            char *tab2 = strchr(g2, '\t');
            grade gr = G_FAIL;
            if (tab2 != NULL) {
                *tab2 = '\0';
            }
            for (int k = 0; k <= G_FAIL; k++) {
                if (strcmp(g2, grade_name((grade)k)) == 0) {
                    gr = (grade)k;
                }
            }
            *tab = '\0';
            g_grades = realloc(g_grades, (g_ngrades + 1) * sizeof *g_grades);
            if (g_grades == NULL) {
                abort();
            }
            g_grades[g_ngrades].rel = strdup(p);
            g_grades[g_ngrades].want = gr;
            g_ngrades++;
        }
        p = nl != NULL ? nl + 1 : NULL;
    }
    pc_buf_fini(&b);
}

static bool recorded(const char *rel, grade *out)
{
    for (size_t i = 0; i < g_ngrades; i++) {
        if (strcmp(g_grades[i].rel, rel) == 0) {
            *out = g_grades[i].want;
            return true;
        }
    }
    return false;
}

typedef struct counts {
    size_t n[6];
} counts;

static void walk(const char *dir, const char *rel, counts *c, bool write)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char **names = NULL;
    size_t n = 0;
    if (d == NULL) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        names = realloc(names, (n + 1) * sizeof *names);
        if (names == NULL) {
            abort();
        }
        names[n++] = strdup(de->d_name);
    }
    (void)closedir(d);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (strcmp(names[j], names[i]) < 0) {
                char *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        char path[2100];
        char sub[1024];
        struct stat sb2;
        size_t nl = strlen(names[i]);
        (void)snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        (void)snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] != '\0' ? "/" : "", names[i]);
        if (stat(path, &sb2) == 0 && S_ISDIR(sb2.st_mode)) {
            walk(path, sub, c, write);
        } else if (nl > 5 && strcmp(names[i] + nl - 5, ".json") == 0) {
            pc_buf text = PC_BUF_INIT;
            yyjson_doc *jd;
            char why[512];
            grade gr;
            grade want;
            sub[strlen(sub) - 5] = '\0';
            must(pc_file_read(path, (size_t)1 << 24, &text, NULL));
            must(pc_json_parse((const char *)text.data, text.len, &jd, NULL));
            gr = grade_file(sub, yyjson_doc_get_root(jd), why, sizeof why);
            yyjson_doc_free(jd);
            pc_buf_fini(&text);
            c->n[gr]++;
            if (gr == G_SKIP) {
                free(names[i]);
                continue;
            }
            if (write) {
                (void)printf("%s\t%s\t%s\n", sub, grade_name(gr), gr == G_NEAR || gr == G_FAIL ? why : "");
            } else {
                pc_t_checks++;
                if (!recorded(sub, &want)) {
                    want = G_EXACT;
                }
                if (rank(gr) > rank(want) || (gr == G_ERROR) != (want == G_ERROR)) {
                    PC_T_FAIL("%s: %s, recorded %s (%s)", sub, grade_name(gr), grade_name(want), why);
                } else if (rank(gr) < rank(want)) {
                    (void)printf("  %s: improved to %s (recorded %s): raise grades.txt\n", sub,
                                 grade_name(gr), grade_name(want));
                }
            }
        }
        free(names[i]);
    }
    free(names);
}

static void test_corpus_composites(void)
{
    counts c;
    bool write = getenv("PC_COMPOSITE_WRITE_GRADES") != NULL;
    memset(&c, 0, sizeof c);
    if (access("corpus/psd", R_OK) != 0) {
        (void)printf("  corpus/ absent: skipped (run `make corpus`)\n");
        return;
    }
    load_grades();
    walk(GOLDEN_DIR, "", &c, write);
    (void)fprintf(
        write ? stderr : stdout,
        "composites: %zu exact, %zu 8-bit exact, %zu near, %zu refused by both, %zu fail, %zu skipped\n",
        c.n[G_EXACT], c.n[G_RGBA8], c.n[G_NEAR], c.n[G_ERROR], c.n[G_FAIL], c.n[G_SKIP]);
}

int main(int argc, char **argv)
{
    RUN(test_corpus_composites);
    return pc_t_finish();
}
