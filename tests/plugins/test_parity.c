/* Parity with the Rust reference: tests/golden/plugins-vectors.txt was written by
 * `rustref plugins-vectors` (tools/rustref/src/plugins_vectors.rs; its header lists the lines).
 *
 *   - manifests: serde's results and messages for valid and hostile JSON, the notation and the
 *     serialisation; resolve_params over the same manifests and parameters;
 *   - modules: every hand-built module loads (or fails) with the same Error variant, and with the
 *     same text wherever the text is the crate's own (see `kind_only` for the exceptions:
 *     validator wording, which comes from wasmi there and WAMR here);
 *   - applies: the same pixels (BLAKE3 of the samples) for the invert fixture and hand-built
 *     filters over formats, selections, overlaps and band sizes, and the same outcome for hostile
 *     filters (traps, budgets, ABI misuse);
 *   - output areas, format codes, the registry sequence and folder loading.
 * The C encoder (wasm_enc.h) rebuilds the ABI modules and must produce the reference's bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hash.h"
#include "base/json.h"
#include "common.h"
#include "harness.h"
#include "plugins/plugins.h"
#include "wasm_enc.h"

#define GOLDEN "tests/golden/plugins-vectors.txt"
#define MAXF 10

typedef struct line {
    char *f[MAXF];
    size_t n;
    char *rest[MAXF]; /* the line from field i on (tabs kept) */
} line;

static line *g_lines;
static size_t g_nlines;
static char *g_text, *g_copy;
static size_t g_kind_only; /* error lines compared by variant only */

static int load_golden(void)
{
    pc_buf b = PC_BUF_INIT;
    size_t cap = 0, len;
    char *p, *q;
    if (t_read_file(GOLDEN, &b) != 0) {
        return -1;
    }
    len = b.len;
    g_text = (char *)pc_buf_take(&b, NULL);
    g_copy = malloc(len + 1);
    if (g_text == NULL || g_copy == NULL) {
        return -1;
    }
    memcpy(g_copy, g_text, len + 1);
    p = g_text;
    q = g_copy;
    while (p != NULL && *p != '\0') {
        char *end = strchr(p, '\n'), *qend = strchr(q, '\n');
        line l;
        if (end != NULL) {
            *end = '\0';
            *qend = '\0';
        }
        memset(&l, 0, sizeof l);
        while (l.n < MAXF) {
            char *tab = strchr(p, '\t');
            l.rest[l.n] = q;
            l.f[l.n++] = p;
            if (tab == NULL) {
                break;
            }
            *tab = '\0';
            q += tab - p + 1;
            p = tab + 1;
        }
        if (g_nlines == cap) {
            line *grown = realloc(g_lines, (cap = cap ? cap * 2 : 1024) * sizeof *grown);
            if (grown == NULL) {
                return -1;
            }
            g_lines = grown;
        }
        g_lines[g_nlines++] = l;
        p = end != NULL ? end + 1 : NULL;
        q = qend != NULL ? qend + 1 : NULL;
    }
    return 0;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static pc_buf unhex(const char *h)
{
    pc_buf b = PC_BUF_INIT;
    size_t n = strlen(h);
    (void)pc_buf_reserve(&b, n / 2 + 1, NULL);
    for (size_t i = 0; i + 1 < n; i += 2) {
        (void)pc_buf_append_byte(&b, (uint8_t)(hexval(h[i]) * 16 + hexval(h[i + 1])), NULL);
    }
    return b;
}

static void esc(pc_buf *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\') {
            (void)pc_buf_append_str(o, "\\\\", NULL);
        } else if (s[i] == '\t') {
            (void)pc_buf_append_str(o, "\\t", NULL);
        } else if (s[i] == '\n') {
            (void)pc_buf_append_str(o, "\\n", NULL);
        } else {
            (void)pc_buf_append_byte(o, (uint8_t)s[i], NULL);
        }
    }
}

static void esc_c(pc_buf *o, const char *s)
{
    esc(o, s, strlen(s));
}

static const char *kind_name(pc_plugin_error_kind k)
{
    static const char *const N[] = {"None",  "Module", "Abi",    "Manifest", "Trap",
                                    "Limit", "Params", "Failed", "NotFound", "Io"};
    return (unsigned)k < sizeof N / sizeof N[0] ? N[k] : "?";
}

static void err_line(pc_buf *o, const pc_error *e)
{
    (void)pc_buf_appendf(o, NULL, "err\t%s\t", kind_name(pc_plugin_error_kind_of(e)));
    esc_c(o, e->msg);
}

/* serde_json's messages for errors inside a JSON string token; the B2 JSON wrapper words these
 * differently (REWRITE_PLAN.md §7, B2), so manifest lines carrying them compare up to the
 * "manifest: " prefix. */
static bool json_lexical(const char *want)
{
    static const char *const LEX[] = {"unexpected end of hex escape",
                                      "invalid escape",
                                      "control character",
                                      "lone leading surrogate",
                                      "invalid unicode code point",
                                      "EOF while parsing a string"};
    if (strncmp(want, "err\tplug-in manifest: manifest: ", 32) != 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof LEX / sizeof LEX[0]; i++) {
        if (strncmp(want + 32, LEX[i], strlen(LEX[i])) == 0) {
            return true;
        }
    }
    return false;
}

static size_t g_lexical;

/* Error texts the reference takes from wasmi's validator or linker: compared by variant only. */
static bool kind_only(const char *want)
{
    static const char *const OWN[] = {
        "not a valid plug-in module: module is ",
        "not a valid plug-in module: not a WebAssembly binary",
        "not a valid plug-in module: the Wasm module",
        "not a valid plug-in module: a function type exceeds",
    };
    if (strncmp(want, "err\tModule\t", 11) != 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof OWN / sizeof OWN[0]; i++) {
        if (strncmp(want + 11, OWN[i], strlen(OWN[i])) == 0) {
            return false;
        }
    }
    return true;
}

static void compare(const char *what, const char *got, const char *want)
{
    if (json_lexical(want)) {
        g_lexical++;
        CHECK(strncmp(got, want, 32) == 0);
        return;
    }
    if (kind_only(want)) {
        g_kind_only++;
        CHECK(strncmp(got, "err\tModule\t", 11) == 0);
        if (strncmp(got, "err\tModule\t", 11) != 0) {
            fprintf(stderr, "  %s\n    got  %s\n    want %s\n", what, got, want);
        }
        return;
    }
    CHECK(strcmp(got, want) == 0);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "  %s\n    got  %s\n    want %s\n", what, got, want);
    }
}

/* ---- manifests ---- */

static void test_manifests(void)
{
    size_t n = 0;
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        pc_buf in, o = PC_BUF_INIT;
        pc_plugin_manifest m;
        pc_error e;
        if (strcmp(l->f[0], "man") != 0 || l->n < 3) {
            continue;
        }
        in = unhex(l->f[1]);
        pc_error_clear(&e);
        if (pc_plugin_manifest_parse(in.data, in.len, &m, &e) == PC_OK) {
            pc_buf nt = PC_BUF_INIT, js = PC_BUF_INIT;
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            (void)pc_plugin_manifest_params_notation(&m, &nt, NULL);
            (void)pc_json_write(pc_plugin_manifest_to_json(&m, d), 0, &js, NULL);
            (void)pc_buf_append_str(&o, "ok\t", NULL);
            esc(&o, (const char *)nt.data, nt.len);
            (void)pc_buf_append_byte(&o, '\t', NULL);
            esc(&o, (const char *)js.data, js.len);
            pc_buf_fini(&nt);
            pc_buf_fini(&js);
            yyjson_mut_doc_free(d);
            pc_plugin_manifest_fini(&m);
        } else {
            (void)pc_buf_append_str(&o, "err\t", NULL);
            esc_c(&o, e.msg);
        }
        {
            char what[96];
            (void)snprintf(what, sizeof what, "man #%zu (%.40s)", n, l->f[1]);
            compare(what, pc_buf_cstr(&o), l->rest[2]);
        }
        pc_buf_fini(&o);
        pc_buf_fini(&in);
        n++;
    }
    CHECK_EQ_UINT(n, 200);
}

static void test_resolve_params(void)
{
    pc_plugin_manifest mans[8];
    bool have[8] = {false};
    size_t n = 0;
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        if (strcmp(l->f[0], "resman") == 0) {
            size_t k = (size_t)atoi(l->f[1]);
            pc_buf in = unhex(l->f[2]);
            if (k < 8) {
                have[k] = pc_plugin_manifest_parse(in.data, in.len, &mans[k], NULL) == PC_OK;
                CHECK(have[k]);
            }
            pc_buf_fini(&in);
        } else if (strcmp(l->f[0], "res") == 0 && l->n >= 4) {
            size_t k = (size_t)atoi(l->f[1]);
            pc_buf in = unhex(l->f[2]), o = PC_BUF_INIT;
            yyjson_doc *doc = NULL;
            pc_json_value v = PC_JSON_VALUE_INIT;
            pc_error e;
            if (k >= 8 || !have[k] || pc_json_parse((const char *)in.data, in.len, &doc, NULL) != PC_OK) {
                CHECK(false);
                pc_buf_fini(&in);
                continue;
            }
            if (pc_plugin_manifest_resolve_params(&mans[k], yyjson_doc_get_root(doc), &v, &e) == PC_OK) {
                pc_buf js = PC_BUF_INIT;
                (void)pc_json_write(pc_json_value_root(&v), PC_JSON_SORT_KEYS, &js, NULL);
                (void)pc_buf_append_str(&o, "ok\t", NULL);
                esc(&o, (const char *)js.data, js.len);
                pc_buf_fini(&js);
            } else {
                (void)pc_buf_append_str(&o, "err\t", NULL);
                esc_c(&o, e.msg);
            }
            compare(l->f[2], pc_buf_cstr(&o), l->rest[3]);
            pc_json_value_fini(&v);
            yyjson_doc_free(doc);
            pc_buf_fini(&o);
            pc_buf_fini(&in);
            n++;
        }
    }
    for (size_t k = 0; k < 8; k++) {
        if (have[k]) {
            pc_plugin_manifest_fini(&mans[k]);
        }
    }
    CHECK_EQ_UINT(n, 84);
}

static void test_format_codes(void)
{
    static const char *const MODES[] = {"Bitmap", "Grayscale", "Indexed",      "Rgb",
                                        "Cmyk",   "Lab",       "Multichannel", "Duotone"};
    static const pc_color_mode CM[] = {
        PC_COLOR_MODE_BITMAP, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_INDEXED,      PC_COLOR_MODE_RGB,
        PC_COLOR_MODE_CMYK,   PC_COLOR_MODE_LAB,       PC_COLOR_MODE_MULTICHANNEL, PC_COLOR_MODE_DUOTONE};
    static const char *const SAMPLES[] = {"U8", "U16", "F32"};
    size_t n = 0;
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        pc_pixel_format f;
        if (strcmp(l->f[0], "fmt") != 0 || l->n < 5) {
            continue;
        }
        memset(&f, 0, sizeof f);
        for (size_t k = 0; k < 8; k++) {
            if (strcmp(l->f[1], MODES[k]) == 0) {
                f.mode = CM[k];
            }
        }
        for (size_t k = 0; k < 3; k++) {
            if (strcmp(l->f[2], SAMPLES[k]) == 0) {
                f.sample = (pc_sample_type)k;
            }
        }
        f.alpha = strcmp(l->f[3], "true") == 0;
        CHECK_EQ_UINT(pc_plugin_format_code(f), strtoul(l->f[4], NULL, 10));
        n++;
    }
    CHECK_EQ_UINT(n, 48);
}

/* ---- modules ---- */

static pc_buf mod_bytes(const char *name)
{
    for (size_t i = 0; i < g_nlines; i++) {
        if (strcmp(g_lines[i].f[0], "mod") == 0 && strcmp(g_lines[i].f[1], name) == 0) {
            return unhex(g_lines[i].n > 2 ? g_lines[i].f[2] : "");
        }
    }
    return (pc_buf)PC_BUF_INIT;
}

static pc_plugin_limits limits(const char *tag)
{
    pc_plugin_limits d = pc_plugin_limits_default();
    if (strcmp(tag, "module100") == 0) {
        d.max_module_bytes = 100;
    } else if (strcmp(tag, "mem1024") == 0) {
        d.max_memory_bytes = 1024;
    } else if (strcmp(tag, "mem4m") == 0) {
        d.max_memory_bytes = (size_t)4 << 20;
        d.band_bytes = (size_t)64 << 20;
    } else if (strcmp(tag, "fuel1m") == 0) {
        d.fuel_base = 1000000;
        d.fuel_per_sample = 10;
    } else if (strcmp(tag, "wall300") == 0) {
        d.fuel_base = UINT64_MAX / 4;
        d.wall_time_ms = 300;
    } else if (strcmp(tag, "band1") == 0) {
        d.band_bytes = 1;
    } else if (strcmp(tag, "small") == 0) {
        d.band_bytes = 40 * 4 * 4 * 3;
    } else if (strcmp(tag, "depth16") == 0) {
        d.max_recursion_depth = 16;
    }
    return d;
}

static void plugin_line(pc_buf *o, const pc_plugin *p)
{
    const pc_plugin_manifest *m = pc_plugin_get_manifest(p);
    pc_buf nt = PC_BUF_INIT;
    (void)pc_plugin_manifest_params_notation(m, &nt, NULL);
    (void)pc_buf_append_str(o, "ok\t", NULL);
    esc_c(o, pc_plugin_id(p));
    (void)pc_buf_append_byte(o, '\t', NULL);
    esc(o, pc_str_c(&m->name), m->name.len);
    (void)pc_buf_append_byte(o, '\t', NULL);
    esc(o, (const char *)nt.data, nt.len);
    (void)pc_buf_appendf(o, NULL, "\t%zu", pc_plugin_size(p));
    pc_buf_fini(&nt);
}

static void test_loads(void)
{
    size_t n = 0;
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        pc_buf b, o = PC_BUF_INIT;
        pc_plugin_limits lim;
        pc_plugin *p = NULL;
        pc_error e;
        const char *want;
        if (strcmp(l->f[0], "load") != 0 || l->n < 4) {
            continue;
        }
        b = mod_bytes(l->f[1]);
        lim = limits(l->f[2]);
        pc_error_clear(&e);
        if (pc_plugin_load(b.data, b.len, &lim, &p, &e) == PC_OK) {
            plugin_line(&o, p);
            pc_plugin_release(p);
        } else {
            err_line(&o, &e);
        }
        want = l->rest[3];
        if (strcmp(want, "panic") == 0) {
            /* The reference divides by a zero function count in wasmi's parser (panics); here the
             * module is refused (REWRITE_PLAN.md §7, I6). */
            CHECK(strncmp(pc_buf_cstr(&o), "err\tModule\t", 11) == 0);
        } else {
            char what[96];
            (void)snprintf(what, sizeof what, "load %s %s", l->f[1], l->f[2]);
            compare(what, pc_buf_cstr(&o), want);
        }
        pc_buf_fini(&o);
        pc_buf_fini(&b);
        n++;
    }
    CHECK_EQ_UINT(n, 82);
}

static pc_pixel_format fmt_by_name(const char *n)
{
    pc_pixel_format f = PC_PIXEL_FORMAT_RGBA8;
    if (strcmp(n, "rgba16") == 0) {
        f = PC_PIXEL_FORMAT_RGBA16;
    } else if (strcmp(n, "rgba32f") == 0) {
        f = PC_PIXEL_FORMAT_RGBA32F;
    } else if (strcmp(n, "gray8") == 0) {
        f = PC_PIXEL_FORMAT_GRAY8;
    } else if (strcmp(n, "graya8") == 0) {
        f = PC_PIXEL_FORMAT_GRAYA8;
    } else if (strcmp(n, "cmyka8") == 0) {
        f = PC_PIXEL_FORMAT_CMYKA8;
    } else if (strcmp(n, "lab16") == 0) {
        f = (pc_pixel_format){PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U16, true};
    } else if (strcmp(n, "multi8") == 0) {
        f = (pc_pixel_format){PC_COLOR_MODE_MULTICHANNEL, PC_SAMPLE_TYPE_U8, false};
    } else if (strcmp(n, "duotone32f") == 0) {
        f = (pc_pixel_format){PC_COLOR_MODE_DUOTONE, PC_SAMPLE_TYPE_F32, true};
    }
    return f;
}

static void surface_result(pc_buf *o, const pc_surface *s, pc_rect canvas)
{
    float *data = NULL;
    size_t len = 0;
    char hex[PC_BLAKE3_HEX_LEN + 1];
    pc_rect b = pc_surface_content_bounds(s);
    if (pc_surface_read_region(s, canvas, &data, &len, NULL) != PC_OK) {
        (void)pc_buf_append_str(o, "read failed", NULL);
        return;
    }
    {
        uint8_t *bytes = malloc(len * 4 + 1);
        for (size_t i = 0; bytes != NULL && i < len; i++) {
            uint32_t u;
            memcpy(&u, &data[i], 4);
            bytes[4 * i] = (uint8_t)u;
            bytes[4 * i + 1] = (uint8_t)(u >> 8);
            bytes[4 * i + 2] = (uint8_t)(u >> 16);
            bytes[4 * i + 3] = (uint8_t)(u >> 24);
        }
        pc_blake3_hex(bytes, bytes != NULL ? len * 4 : 0, hex);
        free(bytes);
    }
    hex[32] = '\0';
    (void)pc_buf_appendf(o, NULL, "ok\t%zu\t%d,%d,%d,%d\t%s", pc_surface_tile_count(s), (int)b.x0, (int)b.y0,
                         (int)b.x1, (int)b.y1, hex);
    free(data);
}

/* CASE = FORMAT:W:H[:sel|:empty][:params=JSON] */
static void apply_case(pc_buf *o, const pc_plugin *p, const char *c)
{
    char head[64];
    const char *pj = strstr(c, ":params=");
    size_t hl = pj != NULL ? (size_t)(pj - c) : strlen(c);
    char *parts[6] = {NULL};
    size_t np = 0;
    yyjson_doc *doc = NULL;
    yyjson_val *params = NULL;
    pc_surface s, sel, out;
    bool has_sel = false, empty = false;
    int32_t w, h;
    pc_error e;
    if (hl >= sizeof head) {
        hl = sizeof head - 1;
    }
    memcpy(head, c, hl);
    head[hl] = '\0';
    for (char *t = strtok(head, ":"); t != NULL && np < 6; t = strtok(NULL, ":")) {
        parts[np++] = t;
    }
    if (np < 3) {
        (void)pc_buf_append_str(o, "bad case", NULL);
        return;
    }
    for (size_t k = 3; k < np; k++) {
        has_sel = has_sel || strcmp(parts[k], "sel") == 0;
        empty = empty || strcmp(parts[k], "empty") == 0;
    }
    w = atoi(parts[1]);
    h = atoi(parts[2]);
    if (pj != NULL) {
        if (pc_json_parse(pj + 8, strlen(pj + 8), &doc, NULL) == PC_OK) {
            params = yyjson_doc_get_root(doc);
        } /* else: null, as the reference's unwrap_or(Value::Null) */
    }
    if (empty) {
        pc_surface_init(&s, fmt_by_name(parts[0]));
    } else {
        t_surface(&s, fmt_by_name(parts[0]), w, h);
    }
    if (has_sel) {
        t_selection(&sel);
    }
    pc_error_clear(&e);
    if (pc_plugin_apply(p, &s, pc_rect_make(0, 0, w, h), has_sel ? &sel : NULL, pj != NULL ? params : NULL,
                        &out, &e) == PC_OK) {
        surface_result(o, &out, pc_rect_make(0, 0, w, h));
        pc_surface_fini(&out);
    } else {
        err_line(o, &e);
    }
    if (has_sel) {
        pc_surface_fini(&sel);
    }
    pc_surface_fini(&s);
    yyjson_doc_free(doc);
}

static void test_applies(void)
{
    size_t n = 0;
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        pc_buf b, o = PC_BUF_INIT;
        pc_plugin_limits lim;
        pc_plugin *p = NULL;
        pc_error e;
        char what[160];
        if (strcmp(l->f[0], "apply") != 0 || l->n < 5) {
            continue;
        }
        b = mod_bytes(l->f[1]);
        lim = limits(l->f[2]);
        if (pc_plugin_load(b.data, b.len, &lim, &p, &e) == PC_OK) {
            apply_case(&o, p, l->f[3]);
            pc_plugin_release(p);
        } else {
            (void)pc_buf_append_str(&o, "load-", NULL);
            err_line(&o, &e);
        }
        (void)snprintf(what, sizeof what, "apply %s %s %s", l->f[1], l->f[2], l->f[3]);
        compare(what, pc_buf_cstr(&o), l->rest[4]);
        pc_buf_fini(&o);
        pc_buf_fini(&b);
        n++;
    }
    CHECK_EQ_UINT(n, 81);
}

static void test_output_areas(void)
{
    const char *names[3] = {"invert", "shift", "canvas"};
    pc_plugin *ps[3] = {NULL, NULL, NULL};
    pc_surface s, e, sel, far;
    float one = 1.0f;
    size_t n = 0;
    for (int k = 0; k < 3; k++) {
        pc_buf b = mod_bytes(names[k]);
        CHECK_EQ_INT(pc_plugin_load(b.data, b.len, NULL, &ps[k], NULL), PC_OK);
        pc_buf_fini(&b);
    }
    t_surface(&s, PC_PIXEL_FORMAT_RGBA8, 40, 30);
    pc_surface_init(&e, PC_PIXEL_FORMAT_RGBA8);
    t_selection(&sel);
    pc_surface_init(&far, PC_PIXEL_FORMAT_GRAY8);
    (void)pc_surface_fill_rect(&far, pc_rect_make(100, 100, 120, 120), &one, 1, NULL);
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        pc_plugin *p = NULL;
        const pc_surface *surf = &s, *selection = NULL;
        pc_rect canvas = pc_rect_make(0, 0, 40, 30), r;
        if (strcmp(l->f[0], "area") != 0 || l->n < 7) {
            continue;
        }
        for (int k = 0; k < 3; k++) {
            if (strcmp(l->f[1], names[k]) == 0) {
                p = ps[k];
            }
        }
        if (p == NULL) {
            continue;
        }
        if (strcmp(l->f[2], "small-canvas") == 0) {
            canvas = pc_rect_make(0, 0, 20, 10);
        } else if (strcmp(l->f[2], "offset-canvas") == 0) {
            canvas = pc_rect_make(-5, -5, 10, 10);
        } else if (strcmp(l->f[2], "empty") == 0) {
            surf = &e;
        } else if (strcmp(l->f[2], "sel") == 0) {
            selection = &sel;
        } else if (strcmp(l->f[2], "far-sel") == 0) {
            selection = &far;
        }
        r = pc_plugin_output_area(p, surf, canvas, selection);
        CHECK_EQ_INT(r.x0, atoi(l->f[3]));
        CHECK_EQ_INT(r.y0, atoi(l->f[4]));
        CHECK_EQ_INT(r.x1, atoi(l->f[5]));
        CHECK_EQ_INT(r.y1, atoi(l->f[6]));
        n++;
    }
    CHECK_EQ_UINT(n, 18);
    for (int k = 0; k < 3; k++) {
        pc_plugin_release(ps[k]);
    }
    pc_surface_fini(&s);
    pc_surface_fini(&e);
    pc_surface_fini(&sel);
    pc_surface_fini(&far);
}

/* The C encoder must produce the reference's bytes for the ABI modules. */
static void test_encoder_matches(void)
{
    static const uint8_t NOOP[] = {0x41, 0x00, 0x0b};
    static const uint8_t SPIN[] = {0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b};
    static const uint8_t TRAP[] = {0x00, 0x0b};
    static const uint8_t CODE7[] = {0x41, 0x07, 0x0b};
    struct {
        const char *name, *manifest;
        int32_t version;
        const uint8_t *body;
        size_t n;
    } cases[] = {
        {"noop", WM_MANIFEST, 1, NOOP, sizeof NOOP},
        {"spin", WM_MANIFEST, 1, SPIN, sizeof SPIN},
        {"trap", WM_MANIFEST, 1, TRAP, sizeof TRAP},
        {"error-code", WM_MANIFEST, 1, CODE7, sizeof CODE7},
        {"abi-v2", WM_MANIFEST, 2, NOOP, sizeof NOOP},
        {"bad-manifest", "not json", 1, NOOP, sizeof NOOP},
        {"bad-id", "{\"id\":\"a b\",\"name\":\"x\",\"kind\":\"filter\"}", 1, NOOP, sizeof NOOP},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_buf c = wm_abi_bytes(cases[i].manifest, cases[i].version, 0, cases[i].body, cases[i].n);
        pc_buf r = mod_bytes(cases[i].name);
        CHECK_EQ_UINT(c.len, r.len);
        if (c.len == r.len && c.len > 0) {
            CHECK_EQ_MEM(c.data, r.data, c.len);
        }
        pc_buf_fini(&c);
        pc_buf_fini(&r);
    }
}

/* ---- registry and folders ---- */

static const char *find_rest(const char *kind, const char *step, size_t field)
{
    for (size_t i = 0; i < g_nlines; i++) {
        if (strcmp(g_lines[i].f[0], kind) == 0 && g_lines[i].n > field &&
            strcmp(g_lines[i].f[1], step) == 0) {
            return g_lines[i].rest[field];
        }
    }
    return "";
}

static void reg_install(const char *step, const char *manifest)
{
    static const uint8_t NOOP[] = {0x41, 0x00, 0x0b};
    pc_buf b = wm_abi_bytes(manifest, 1, 0, NOOP, sizeof NOOP);
    pc_plugin *p = NULL;
    pc_error e;
    char got[320];
    if (pc_plugins_install_bytes(b.data, b.len, &p, &e) == PC_OK) {
        (void)snprintf(got, sizeof got, "Ok(\"%s\")", pc_plugin_id(p));
        pc_plugin_release(p);
    } else {
        (void)snprintf(got, sizeof got, "Err(%s)", e.msg);
    }
    CHECK_EQ_STR(got, find_rest("reg", step, 2));
    pc_buf_fini(&b);
}

static void reg_show(const char *step, uint64_t rev0)
{
    pc_plugin **list = NULL;
    size_t n = 0;
    pc_buf o = PC_BUF_INIT;
    CHECK_EQ_INT(pc_plugins_list(&list, &n, NULL), PC_OK);
    for (size_t i = 0; i < n; i++) {
        const pc_plugin_manifest *m = pc_plugin_get_manifest(list[i]);
        (void)pc_buf_appendf(&o, NULL, "%s%s=%s", i > 0 ? "," : "", pc_plugin_id(list[i]),
                             pc_str_c(&m->name));
    }
    (void)pc_buf_appendf(&o, NULL, "\t%llu", (unsigned long long)(pc_plugins_revision() - rev0));
    CHECK_EQ_STR(pc_buf_cstr(&o), find_rest("reg", step, 2));
    pc_buf_fini(&o);
    pc_plugins_list_free(list, n);
}

static void test_registry(void)
{
    uint64_t rev0 = pc_plugins_revision();
    pc_plugin *p;
    pc_error e;
    static const uint8_t JUNK[] = "\0asm junk";
    reg_install("install-b", "{\"id\":\"test.b\",\"name\":\"R test.b\",\"kind\":\"filter\"}");
    reg_install("install-a", "{\"id\":\"test.a\",\"name\":\"R test.a\",\"kind\":\"filter\"}");
    reg_show("after-installs", rev0);
    reg_install("reinstall-a", "{\"id\":\"test.a\",\"name\":\"Again\",\"kind\":\"filter\"}");
    reg_show("after-reinstall", rev0);
    CHECK(pc_plugins_install_bytes(JUNK, sizeof JUNK - 1, NULL, &e) != PC_OK);
    CHECK_EQ_INT(pc_plugin_error_kind_of(&e), PC_PLUGIN_E_MODULE); /* text: WAMR's validator */
    reg_show("after-bad", rev0);
    p = pc_plugins_get("test.a");
    CHECK(p != NULL);
    if (p != NULL) {
        CHECK_EQ_STR(pc_str_c(&pc_plugin_get_manifest(p)->name), find_rest("reg", "get-a", 2));
        pc_plugin_release(p);
    }
    CHECK(pc_plugins_get("test.z") == NULL);
    CHECK_EQ_STR(pc_plugins_remove("test.a") ? "true" : "false", find_rest("reg", "remove-a", 2));
    CHECK_EQ_STR(pc_plugins_remove("test.a") ? "true" : "false", find_rest("reg", "remove-a-again", 2));
    reg_show("after-remove", rev0);
    CHECK(pc_plugins_remove("test.b"));
    reg_show("end", rev0);
}

static char *strip(const char *s, const char *prefix)
{
    pc_buf o = PC_BUF_INIT;
    size_t pl = strlen(prefix);
    while (*s != '\0') {
        if (pl > 0 && strncmp(s, prefix, pl) == 0) {
            s += pl;
            continue;
        }
        (void)pc_buf_append_byte(&o, (uint8_t)*s++, NULL);
    }
    return (char *)pc_buf_take(&o, NULL);
}

static void test_folders(void)
{
    char dir[512], path[600], prefix[600];
    pc_buf inv = PC_BUF_INIT, params = mod_bytes("params"), noop = mod_bytes("noop"),
           wasi = mod_bytes("wasi-import");
    pc_plugins_folder_report r;
    pc_plugin *p = NULL;
    pc_plugin_limits small = pc_plugin_limits_default();
    pc_error e;
    size_t failed = 0;
    if (t_mkdtemp("parity", dir, sizeof dir) != 0 || t_invert(&inv) != 0) {
        CHECK(false);
        return;
    }
    (void)snprintf(prefix, sizeof prefix, "%s/", dir);
#define PUT(name, data, len)                                                                                 \
    do {                                                                                                     \
        (void)snprintf(path, sizeof path, "%s/%s", dir, name);                                               \
        CHECK_EQ_INT(t_write_file(path, data, len), 0);                                                      \
    } while (0)
    PUT("b-invert.wasm", inv.data, inv.len);
    PUT("a-broken.wasm", "\0asm junk", 9);
    PUT("c-params.WASM", params.data, params.len);
    PUT("notes.txt", "ignored", 7);
    PUT(".wasm", noop.data, noop.len);
    PUT("d-empty.wasm", "", 0);
    PUT("e-import.Wasm", wasi.data, wasi.len);
#undef PUT
    (void)snprintf(path, sizeof path, "%s/f-dir.wasm", dir);
    CHECK_EQ_INT(mkdir(path, 0755), 0);
    CHECK_EQ_INT(pc_plugins_load_folder(dir, &r, NULL), PC_OK);
    {
        pc_buf ids = PC_BUF_INIT;
        for (size_t i = 0; i < r.n_loaded; i++) {
            (void)pc_buf_appendf(&ids, NULL, "%s%s", i > 0 ? "," : "", pc_str_c(&r.loaded[i]));
        }
        CHECK_EQ_STR(pc_buf_cstr(&ids), find_rest("folder", "loaded", 2));
        pc_buf_fini(&ids);
    }
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        if (strcmp(l->f[0], "folder") == 0 && strcmp(l->f[1], "failed") == 0 && l->n >= 4) {
            char *file = failed < r.n_failed ? strip(r.failed[failed].file, prefix) : NULL;
            char *msg = failed < r.n_failed ? strip(r.failed[failed].error, prefix) : NULL;
            pc_buf em = PC_BUF_INIT;
            CHECK(file != NULL && strcmp(file, l->f[2]) == 0);
            if (msg != NULL) {
                esc_c(&em, msg);
                /* WAMR words the broken module's error differently; the rest are the crate's. */
                if (strcmp(l->f[2], "a-broken.wasm") == 0) {
                    CHECK(strncmp(msg, "not a valid plug-in module: ", 28) == 0);
                } else {
                    CHECK_EQ_STR(pc_buf_cstr(&em), l->rest[3]);
                }
            }
            pc_buf_fini(&em);
            free(file);
            free(msg);
            failed++;
        }
    }
    CHECK_EQ_UINT(failed, r.n_failed);
    pc_plugins_folder_report_fini(&r);
    CHECK(pc_plugins_load_file(dir, NULL, &p, &e) != PC_OK);
    {
        char *m = strip(e.msg, dir), want[256];
        (void)snprintf(want, sizeof want, "DIR%s", m != NULL ? m : "");
        CHECK_EQ_STR(want, find_rest("folder", "file-dir", 2));
        free(m);
    }
    small.max_module_bytes = 100;
    (void)snprintf(path, sizeof path, "%s/b-invert.wasm", dir);
    CHECK(pc_plugins_load_file(path, &small, &p, &e) != PC_OK);
    {
        char *m = strip(e.msg, prefix);
        CHECK_EQ_STR(m, find_rest("folder", "file-big", 2));
        free(m);
    }
    CHECK_EQ_INT(pc_plugins_load_file(path, NULL, &p, NULL), PC_OK);
    if (p != NULL) {
        char *s = strip(pc_plugin_source(p), prefix), want[256];
        (void)snprintf(want, sizeof want, "Some(\"%s\")", s);
        CHECK_EQ_STR(want, find_rest("folder", "file-source", 2));
        free(s);
        pc_plugin_release(p);
    }
    {
        pc_plugin **list = NULL;
        size_t n = 0;
        if (pc_plugins_list(&list, &n, NULL) == PC_OK) {
            for (size_t i = 0; i < n; i++) {
                (void)pc_plugins_remove(pc_plugin_id(list[i]));
            }
            pc_plugins_list_free(list, n);
        }
    }
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        if (strcmp(l->f[0], "folder") == 0 && l->n >= 4 &&
            (strcmp(l->f[1], "missing-dir") == 0 || strcmp(l->f[1], "missing-file") == 0)) {
            pc_plugins_folder_report rr;
            pc_status st = strcmp(l->f[1], "missing-dir") == 0 ? pc_plugins_load_folder(l->f[2], &rr, &e)
                                                               : pc_plugins_load_file(l->f[2], NULL, &p, &e);
            CHECK_EQ_INT(st, PC_ERR_IO);
            CHECK_EQ_STR(e.msg, l->rest[3]);
        }
    }
    t_rm_rf(dir);
    pc_buf_fini(&inv);
    pc_buf_fini(&params);
    pc_buf_fini(&noop);
    pc_buf_fini(&wasi);
}

int main(int argc, char **argv)
{
    if (load_golden() != 0) {
        fprintf(stderr, "cannot read %s\n", GOLDEN);
        return 1;
    }
    RUN(test_manifests);
    RUN(test_resolve_params);
    RUN(test_format_codes);
    RUN(test_encoder_matches);
    if (pc_plugins_available()) {
        RUN(test_loads);
        RUN(test_applies);
        RUN(test_output_areas);
        RUN(test_registry);
        RUN(test_folders);
        fprintf(stderr, "  (%zu module errors compared by variant only: validator wording)\n", g_kind_only);
        fprintf(stderr, "  (%zu manifest errors inside JSON strings compared by prefix: B2 wording)\n",
                g_lexical);
    } else {
        fprintf(stderr, "  plug-ins unavailable in this build: module, apply and registry vectors skipped\n");
    }
    free(g_lines);
    free(g_text);
    free(g_copy);
    return pc_t_finish();
}
