#include "lutfile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "base/utf8.h"

/* ---------------------------------------------------------------- text helpers */

typedef struct span {
    const char *p;
    size_t n;
} span;

static span sp(const char *p, size_t n)
{
    span s;
    s.p = p;
    s.n = n;
    return s;
}

/* str::trim */
static span trim(span s)
{
    size_t start;
    size_t n = pc_utf8_trim(s.p, s.n, &start);
    return sp(s.p + start, n);
}

/* Next line of text (str::lines: split on \n, drop a trailing \r). */
static bool next_line(span *rest, span *line)
{
    const char *nl;
    size_t n;
    if (rest->n == 0) {
        return false;
    }
    nl = memchr(rest->p, '\n', rest->n);
    n = nl != NULL ? (size_t)(nl - rest->p) : rest->n;
    *line = sp(rest->p, n);
    if (line->n > 0 && line->p[line->n - 1] == '\r') {
        line->n--;
    }
    rest->p += nl != NULL ? n + 1 : n;
    rest->n -= nl != NULL ? n + 1 : n;
    return true;
}

/* The part before the first c. */
static span before(span s, char c)
{
    const char *x = memchr(s.p, c, s.n);
    return x != NULL ? sp(s.p, (size_t)(x - s.p)) : s;
}

/* split_whitespace: next token, advancing *rest. */
static bool next_token(span *rest, span *tok)
{
    size_t i = 0;
    size_t start;
    uint32_t cp;
    const uint8_t *u = (const uint8_t *)rest->p;
    while (i < rest->n) {
        size_t j = i;
        if (pc_utf8_next(u, rest->n, &j, &cp) && pc_unicode_is_space(cp)) {
            i = j;
            continue;
        }
        break;
    }
    if (i >= rest->n) {
        rest->p += rest->n;
        rest->n = 0;
        return false;
    }
    start = i;
    while (i < rest->n) {
        size_t j = i;
        if (pc_utf8_next(u, rest->n, &j, &cp)) {
            if (pc_unicode_is_space(cp)) {
                break;
            }
            i = j;
        } else {
            i++;
        }
    }
    *tok = sp(rest->p + start, i - start);
    rest->p += i;
    rest->n -= i;
    return true;
}

static bool span_eq(span s, const char *lit)
{
    size_t n = strlen(lit);
    return s.n == n && memcmp(s.p, lit, n) == 0;
}

static bool ieq_word(const char *p, size_t n, const char *word)
{
    size_t i;
    if (strlen(word) != n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        if (c != word[i]) {
            return false;
        }
    }
    return true;
}

/* Rust's float grammar: [+-] (inf | infinity | nan | digits [. digits] [e [+-] digits]), with
 * at least one mantissa digit; no hex, no surrounding space. */
static bool float_syntax(span s)
{
    size_t i = 0;
    size_t digits = 0;
    if (s.n == 0) {
        return false;
    }
    if (s.p[0] == '+' || s.p[0] == '-') {
        i = 1;
    }
    if (ieq_word(s.p + i, s.n - i, "inf") || ieq_word(s.p + i, s.n - i, "infinity") ||
        ieq_word(s.p + i, s.n - i, "nan")) {
        return true;
    }
    while (i < s.n && s.p[i] >= '0' && s.p[i] <= '9') {
        i++;
        digits++;
    }
    if (i < s.n && s.p[i] == '.') {
        i++;
        while (i < s.n && s.p[i] >= '0' && s.p[i] <= '9') {
            i++;
            digits++;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (i < s.n && (s.p[i] == 'e' || s.p[i] == 'E')) {
        size_t ed = 0;
        i++;
        if (i < s.n && (s.p[i] == '+' || s.p[i] == '-')) {
            i++;
        }
        while (i < s.n && s.p[i] >= '0' && s.p[i] <= '9') {
            i++;
            ed++;
        }
        if (ed == 0) {
            return false;
        }
    }
    return i == s.n;
}

/* s.parse::<f32>() / ::<f64>() (correctly rounded, as strtof/strtod are on POSIX libcs). */
static bool parse_f64(span s, double *out)
{
    char buf[512];
    if (!float_syntax(s) || s.n >= sizeof buf) {
        return false;
    }
    memcpy(buf, s.p, s.n);
    buf[s.n] = '\0';
    *out = strtod(buf, NULL);
    return true;
}

static bool parse_f32(span s, float *out)
{
    char buf[512];
    if (!float_syntax(s) || s.n >= sizeof buf) {
        return false;
    }
    memcpy(buf, s.p, s.n);
    buf[s.n] = '\0';
    *out = strtof(buf, NULL);
    return true;
}

/* s.parse::<usize>(): optional '+', ASCII digits, no overflow. */
static bool parse_usize(span s, size_t *out)
{
    size_t i = 0;
    size_t v = 0;
    if (s.n > 0 && s.p[0] == '+') {
        i = 1;
    }
    if (i >= s.n) {
        return false;
    }
    for (; i < s.n; i++) {
        unsigned d;
        if (s.p[i] < '0' || s.p[i] > '9') {
            return false;
        }
        d = (unsigned)(s.p[i] - '0');
        if (!pc_size_mul(v, 10, &v) || !pc_size_add(v, d, &v)) {
            return false;
        }
    }
    *out = v;
    return true;
}

/* ---------------------------------------------------------------- model */

void pc_lutfile_fini(pc_lutfile *l)
{
    if (l != NULL) {
        free(l->title);
        free(l->data);
        memset(l, 0, sizeof *l);
    }
}

bool pc_lutfile_domain_is_default(const pc_lutfile *l)
{
    int k;
    for (k = 0; k < 3; k++) {
        if (l->domain_min[k] != 0.0f || l->domain_max[k] != 1.0f) {
            return false;
        }
    }
    return true;
}

static pc_status checked_data_len(size_t size, size_t *entries, size_t *data_len, pc_error *err)
{
    if (size < 2 || size > PC_LUTFILE_MAX_SIZE || !pc_size_mul3(size, size, size, entries) ||
        !pc_size_mul(*entries, 3, data_len)) {
        return pc_fail(err, PC_ERR_FORMAT, "unsupported LUT size %zu", size);
    }
    return PC_OK;
}

/* Validate and move the parts into *out (takes title and data in every case). */
static pc_status finish(char *title, size_t size, float *data, size_t ndata, const float dmin[3], const float dmax[3],
                        pc_lutfile *out, pc_error *err)
{
    size_t entries;
    size_t want;
    size_t i;
    pc_status s = checked_data_len(size, &entries, &want, err);
    if (s == PC_OK && ndata != want) {
        s = pc_fail(err, PC_ERR_FORMAT, "expected %zu entries, found %zu", entries, ndata / 3);
    }
    for (i = 0; s == PC_OK && i < ndata; i++) {
        if (!isfinite(data[i])) {
            s = pc_fail(err, PC_ERR_FORMAT, "non-finite LUT value");
        }
    }
    if (s != PC_OK) {
        free(title);
        free(data);
        return s;
    }
    out->title = title;
    out->size = size;
    out->data = data;
    out->ndata = ndata;
    memcpy(out->domain_min, dmin, sizeof out->domain_min);
    memcpy(out->domain_max, dmax, sizeof out->domain_max);
    return PC_OK;
}

typedef void (*look_fn)(const void *ctx, const float c[3], float o[3]);

/* Sample f on a size^3 grid (outputs clamped to 0..1). */
static pc_status from_fn(const char *title, size_t size, look_fn f, const void *ctx, pc_lutfile *out, pc_error *err)
{
    static const float dmin[3] = {0, 0, 0};
    static const float dmax[3] = {1, 1, 1};
    size_t n;
    size_t r;
    size_t g;
    size_t b;
    float m = (float)((size < 2 ? 2 : size) - 1);
    float *data;
    char *t;
    size_t k = 0;
    if (!pc_size_mul3(size, size, size, &n) || !pc_size_mul(n, 3, &n)) {
        return pc_fail(err, PC_ERR_LIMIT, "LUT too large");
    }
    data = pc_malloc_array(n, sizeof *data);
    t = pc_strndup(title, strlen(title));
    if (data == NULL || t == NULL) {
        free(data);
        free(t);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (b = 0; b < size; b++) {
        for (g = 0; g < size; g++) {
            for (r = 0; r < size; r++) {
                float c[3];
                float o[3];
                int i;
                c[0] = (float)r / m;
                c[1] = (float)g / m;
                c[2] = (float)b / m;
                f(ctx, c, o);
                for (i = 0; i < 3; i++) {
                    data[k++] = pc_clampf(o[i], 0.0f, 1.0f);
                }
            }
        }
    }
    return finish(t, size, data, n, dmin, dmax, out, err);
}

static void identity_fn(const void *ctx, const float c[3], float o[3])
{
    (void)ctx;
    memcpy(o, c, 3 * sizeof *o);
}

pc_status pc_lutfile_identity(size_t size, pc_lutfile *out, pc_error *err)
{
    return from_fn("Identity", size, identity_fn, NULL, out, err);
}

/* ---------------------------------------------------------------- .cube */

static void debug_f32x3(const float v[3], char *out, size_t cap)
{
    char s[3][PC_JSON_NUM_MAX];
    int i;
    for (i = 0; i < 3; i++) {
        if (isnan(v[i])) {
            (void)snprintf(s[i], sizeof s[i], "NaN");
        } else if (isinf(v[i])) {
            (void)snprintf(s[i], sizeof s[i], "%sinf", v[i] < 0 ? "-" : "");
        } else {
            (void)pc_json_format_f32(v[i], s[i]);
        }
    }
    (void)snprintf(out, cap, "[%s, %s, %s]", s[0], s[1], s[2]);
}

typedef struct cube1d {
    const float *rows;
    size_t size1;
    float dmin[3];
    float dmax[3];
} cube1d;

static float curve1d(const cube1d *c, int ch, float v)
{
    float t = (v - c->dmin[ch]) / (c->dmax[ch] - c->dmin[ch]);
    float x = (isfinite(t) ? pc_clampf(t, 0.0f, 1.0f) : 0.0f) * (float)(c->size1 - 1);
    size_t i = pc_f32_to_size(floorf(x));
    float f;
    if (i > c->size1 - 2) {
        i = c->size1 - 2;
    }
    f = x - (float)i;
    return c->rows[i * 3 + (size_t)ch] * (1.0f - f) + c->rows[(i + 1) * 3 + (size_t)ch] * f;
}

static void cube1d_fn(const void *ctx, const float c[3], float o[3])
{
    const cube1d *k = ctx;
    o[0] = curve1d(k, 0, c[0]);
    o[1] = curve1d(k, 1, c[1]);
    o[2] = curve1d(k, 2, c[2]);
}

/* Up to 3 parsed numbers from the rest of a line; *n gets how many parsed (all counted). */
static void collect_f32(span rest, float *v, size_t cap, size_t *n)
{
    span tok;
    *n = 0;
    while (next_token(&rest, &tok)) {
        float f;
        if (parse_f32(tok, &f)) {
            if (*n < cap) {
                v[*n] = f;
            }
            (*n)++;
        }
    }
}

pc_status pc_lutfile_parse_cube(const char *text, size_t len, pc_lutfile *out, pc_error *err)
{
    pc_buf title = PC_BUF_INIT;
    size_t size3 = 0;
    size_t size1 = 0;
    float dmin[3] = {0.0f, 0.0f, 0.0f};
    float dmax[3] = {1.0f, 1.0f, 1.0f};
    float *rows = NULL;
    size_t nrows = 0;
    size_t cap = 0;
    span rest = sp(text, len);
    span raw;
    pc_status s = PC_OK;
    bool domain_ok = true;
    int k;

    while (s == PC_OK && next_line(&rest, &raw)) {
        span line = trim(before(raw, '#'));
        span it = line;
        span key;
        if (line.n == 0 || !next_token(&it, &key)) {
            continue;
        }
        if (span_eq(key, "TITLE")) {
            span tv = trim(sp(line.p + 5, line.n - 5));
            while (tv.n > 0 && tv.p[0] == '"') { tv.p++; tv.n--; }
            while (tv.n > 0 && tv.p[tv.n - 1] == '"') { tv.n--; }
            pc_buf_truncate(&title, 0);
            s = pc_buf_append(&title, tv.p, tv.n, err);
        } else if (span_eq(key, "LUT_3D_SIZE") || span_eq(key, "LUT_1D_SIZE")) {
            span v;
            size_t sz;
            bool three = key.p[4] == '3';
            if (!next_token(&it, &v) || !parse_usize(v, &sz)) {
                s = pc_fail(err, PC_ERR_FORMAT, three ? "bad LUT_3D_SIZE" : "bad LUT_1D_SIZE");
            } else if (three) {
                size3 = sz;
            } else {
                size1 = sz;
            }
        } else if (span_eq(key, "DOMAIN_MIN") || span_eq(key, "DOMAIN_MAX")) {
            float v[3];
            size_t n;
            collect_f32(it, v, 3, &n);
            if (n != 3) {
                s = pc_fail(err, PC_ERR_FORMAT, "bad line `%.*s`", (int)line.n, line.p);
            } else {
                memcpy(key.p[8] == 'I' ? dmin : dmax, v, sizeof v); /* DOMAIN_M[I]N vs DOMAIN_M[A]X */
            }
        } else if (span_eq(key, "LUT_3D_INPUT_RANGE") || span_eq(key, "LUT_1D_INPUT_RANGE")) {
            float v[2];
            size_t n;
            collect_f32(it, v, 2, &n);
            if (n == 2) {
                for (k = 0; k < 3; k++) {
                    dmin[k] = v[0];
                    dmax[k] = v[1];
                }
            }
        } else if ((key.p[0] >= '0' && key.p[0] <= '9') || key.p[0] == '-' || key.p[0] == '+' || key.p[0] == '.') {
            float v[3];
            float first;
            size_t n;
            if (!parse_f32(key, &first)) {
                s = pc_fail(err, PC_ERR_FORMAT, "bad number in `%.*s`", (int)line.n, line.p);
                break;
            }
            collect_f32(it, v + 1, 2, &n);
            n += 1;
            v[0] = first;
            if (n != 3) {
                s = pc_fail(err, PC_ERR_FORMAT, "bad data line `%.*s`", (int)line.n, line.p);
                break;
            }
            if (nrows + 3 > cap) {
                float *nr = pc_grow_array(rows, &cap, nrows + 3, sizeof *rows);
                if (nr == NULL) {
                    s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                    break;
                }
                rows = nr;
            }
            memcpy(rows + nrows, v, sizeof v);
            nrows += 3;
        }
        /* Unknown keywords (e.g. LUT_IN_VIDEO_RANGE) are ignored, as the spec allows. */
    }
    for (k = 0; k < 3; k++) {
        domain_ok = domain_ok && dmin[k] < dmax[k] && isfinite(dmin[k]) && isfinite(dmax[k]);
    }
    if (s == PC_OK && (size3 > 0 || size1 >= 2) && !domain_ok) {
        char a[128];
        char b[128];
        debug_f32x3(dmin, a, sizeof a);
        debug_f32x3(dmax, b, sizeof b);
        s = pc_fail(err, PC_ERR_FORMAT, "DOMAIN_MIN %s must lie below DOMAIN_MAX %s", a, b);
    }
    if (s == PC_OK && size3 > 0) {
        char *t = pc_strndup(pc_buf_cstr(&title), title.len);
        pc_buf_fini(&title);
        if (t == NULL) {
            free(rows);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        if (rows == NULL) {
            rows = pc_malloc(0);
        }
        return finish(t, size3, rows, nrows, dmin, dmax, out, err);
    }
    if (s == PC_OK && size1 >= 2) {
        size_t expected;
        if (!pc_size_mul(size1, 3, &expected)) {
            s = pc_fail(err, PC_ERR_FORMAT, "unsupported 1D LUT size %zu", size1);
        } else if (nrows != expected) {
            s = pc_fail(err, PC_ERR_FORMAT, "no LUT_3D_SIZE in .cube file");
        } else {
            /* The input domain is baked into the resample onto a 0..1 cube. */
            cube1d c;
            c.rows = rows;
            c.size1 = size1;
            memcpy(c.dmin, dmin, sizeof dmin);
            memcpy(c.dmax, dmax, sizeof dmax);
            s = from_fn(pc_buf_cstr(&title), 33, cube1d_fn, &c, out, err);
        }
        free(rows);
        pc_buf_fini(&title);
        return s;
    }
    free(rows);
    pc_buf_fini(&title);
    return s != PC_OK ? s : pc_fail(err, PC_ERR_FORMAT, "no LUT_3D_SIZE in .cube file");
}

/* ---------------------------------------------------------------- .3dl */

pc_status pc_lutfile_parse_3dl(const char *text, size_t len, pc_lutfile *out, pc_error *err)
{
    static const float dmin[3] = {0, 0, 0};
    static const float dmax[3] = {1, 1, 1};
    double *rows = NULL;
    size_t nrows = 0; /* triples */
    size_t cap = 0;
    span rest = sp(text, len);
    span raw;
    size_t n;
    size_t i;
    double max = 0.0;
    double scale;
    float *data;
    size_t count;
    while (next_line(&rest, &raw)) {
        span line = trim(before(raw, '#'));
        span it = line;
        span tok;
        double v[3];
        size_t got = 0;
        unsigned char c0;
        if (line.n == 0) {
            continue;
        }
        c0 = (unsigned char)line.p[0];
        if ((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z')) {
            continue;
        }
        while (next_token(&it, &tok)) {
            double d;
            if (parse_f64(tok, &d)) {
                if (got < 3) {
                    v[got] = d;
                }
                got++;
            }
        }
        /* Lines with more than three numbers are the input shaper; it is assumed linear. */
        if (got == 3) {
            if ((nrows + 1) * 3 > cap) {
                double *nr = pc_grow_array(rows, &cap, (nrows + 1) * 3, sizeof *rows);
                if (nr == NULL) {
                    free(rows);
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory");
                }
                rows = nr;
            }
            memcpy(rows + nrows * 3, v, sizeof v);
            nrows++;
        }
    }
    n = pc_f64_to_size(round(pc_cbrt((double)nrows))); /* Rust std's cbrt */
    if (n < 2 || n > 4096 || n * n * n != nrows) {
        free(rows);
        return pc_fail(err, PC_ERR_FORMAT, "%zu rows is not a cube", nrows);
    }
    for (i = 0; i < nrows * 3; i++) {
        max = fmax(max, rows[i]);
    }
    if (max <= 1.0) {
        scale = 1.0;
    } else if (max <= 1023.0) {
        scale = 1023.0;
    } else if (max <= 4095.0) {
        scale = 4095.0;
    } else if (max <= 16383.0) {
        scale = 16383.0;
    } else if (max <= 65535.0) {
        scale = 65535.0;
    } else {
        scale = max;
    }
    count = nrows * 3;
    data = pc_calloc_array(count, sizeof *data);
    if (data == NULL) {
        free(rows);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < nrows; i++) {
        size_t r = i / (n * n);
        size_t g = (i / n) % n;
        size_t b = i % n;
        size_t at = ((b * n + g) * n + r) * 3;
        int k;
        for (k = 0; k < 3; k++) {
            data[at + (size_t)k] = pc_f64_to_f32(rows[i * 3 + (size_t)k] / scale);
        }
    }
    free(rows);
    {
        char *t = pc_strndup("", 0);
        if (t == NULL) {
            free(data);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        return finish(t, n, data, count, dmin, dmax, out, err);
    }
}

/* ---------------------------------------------------------------- .look */

/* The trimmed, quote-stripped, trimmed content of the first <name>...</name>. */
static bool look_tag(span text, const char *name, span *out)
{
    char open[32];
    char close[32];
    const char *start;
    const char *end;
    size_t i;
    size_t on;
    size_t cn;
    (void)snprintf(open, sizeof open, "<%s>", name);
    (void)snprintf(close, sizeof close, "</%s>", name);
    on = strlen(open);
    cn = strlen(close);
    start = NULL;
    for (i = 0; i + on <= text.n; i++) {
        if (memcmp(text.p + i, open, on) == 0) {
            start = text.p + i + on;
            break;
        }
    }
    if (start == NULL) {
        return false;
    }
    end = NULL;
    for (i = (size_t)(start - text.p); i + cn <= text.n; i++) {
        if (memcmp(text.p + i, close, cn) == 0) {
            end = text.p + i;
            break;
        }
    }
    if (end == NULL) {
        return false;
    }
    {
        span v = trim(sp(start, (size_t)(end - start)));
        while (v.n > 0 && v.p[0] == '"') { v.p++; v.n--; }
        while (v.n > 0 && v.p[v.n - 1] == '"') { v.n--; }
        *out = trim(v);
    }
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

pc_status pc_lutfile_parse_look(const char *text, size_t len, pc_lutfile *out, pc_error *err)
{
    static const float dmin[3] = {0, 0, 0};
    static const float dmax[3] = {1, 1, 1};
    span t = sp(text, len);
    span v;
    size_t size;
    size_t entries;
    size_t dl;
    size_t rgba;
    size_t nhex = 0;
    size_t i;
    size_t nbytes;
    size_t nfloats;
    uint8_t *bytes;
    float *floats;
    float *data;
    size_t ndata;
    char *title;
    pc_status s;
    if (!look_tag(t, "size", &v) || !parse_usize(v, &size)) {
        return pc_fail(err, PC_ERR_FORMAT, "no <size> in .look");
    }
    s = checked_data_len(size, &entries, &dl, err);
    if (s != PC_OK) {
        return s;
    }
    if (!look_tag(t, "data", &v)) {
        return pc_fail(err, PC_ERR_FORMAT, "no <data> in .look");
    }
    for (i = 0; i < v.n; i++) {
        nhex += hexval(v.p[i]) >= 0 ? 1 : 0;
    }
    nbytes = nhex / 2;
    bytes = pc_malloc(nbytes);
    if (bytes == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    {
        size_t k = 0;
        int hi = -1;
        for (i = 0; i < v.n && k < nbytes; i++) {
            int h = hexval(v.p[i]);
            if (h < 0) {
                continue;
            }
            if (hi < 0) {
                hi = h;
            } else {
                bytes[k++] = (uint8_t)(hi << 4 | h);
                hi = -1;
            }
        }
    }
    nfloats = nbytes / 4;
    floats = pc_malloc_array(nfloats, sizeof *floats);
    if (floats == NULL) {
        free(bytes);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < nfloats; i++) {
        floats[i] = pc_f32_from_bits(pc_load_le32(bytes + i * 4));
    }
    free(bytes);
    if (!pc_size_mul(entries, 4, &rgba)) {
        free(floats);
        return pc_fail(err, PC_ERR_FORMAT, "unsupported LUT size %zu", size);
    }
    if (nfloats == rgba) {
        ndata = entries * 3;
        data = pc_malloc_array(ndata, sizeof *data);
        if (data == NULL) {
            free(floats);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < entries; i++) {
            data[i * 3] = floats[i * 4];
            data[i * 3 + 1] = floats[i * 4 + 1];
            data[i * 3 + 2] = floats[i * 4 + 2];
        }
        free(floats);
    } else {
        data = floats;
        ndata = nfloats;
    }
    if (look_tag(t, "title", &v)) {
        title = pc_strndup(v.p, v.n);
    } else {
        title = pc_strndup("", 0);
    }
    if (title == NULL) {
        free(data);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (data == NULL) {
        data = pc_malloc(0);
    }
    return finish(title, size, data, ndata, dmin, dmax, out, err);
}

/* ---------------------------------------------------------------- dispatch, writing */

static bool ends_with_ci(const char *s, size_t n, const char *suffix)
{
    size_t m = strlen(suffix);
    return n >= m && ieq_word(s + n - m, m, suffix);
}

pc_status pc_lutfile_parse(const char *file_name, const uint8_t *bytes, size_t len, pc_lutfile *out, pc_error *err)
{
    pc_buf text = PC_BUF_INIT;
    size_t fl = file_name != NULL ? strlen(file_name) : 0;
    pc_status s = pc_utf8_lossy(bytes, bytes != NULL ? len : 0, &text, err);
    if (s == PC_OK) {
        const char *tp = pc_buf_cstr(&text);
        span head = trim(sp(tp, text.len));
        if (ends_with_ci(file_name, fl, ".3dl")) {
            s = pc_lutfile_parse_3dl(tp, text.len, out, err);
        } else if (ends_with_ci(file_name, fl, ".look") || (head.n > 0 && head.p[0] == '<')) {
            s = pc_lutfile_parse_look(tp, text.len, out, err);
        } else {
            s = pc_lutfile_parse_cube(tp, text.len, out, err);
        }
    }
    pc_buf_fini(&text);
    return s;
}

static pc_status put_f6(pc_buf *out, float v, const char *sep, pc_error *err)
{
    if (isnan(v)) {
        return pc_buf_appendf(out, err, "NaN%s", sep);
    }
    if (isinf(v)) {
        return pc_buf_appendf(out, err, "%sinf%s", v < 0 ? "-" : "", sep);
    }
    return pc_buf_appendf(out, err, "%.6f%s", (double)v, sep);
}

pc_status pc_lutfile_write_cube(const pc_lutfile *l, pc_buf *out, pc_error *err)
{
    size_t start = out->len;
    size_t i;
    pc_status s = PC_OK;
    if (l->title != NULL && l->title[0] != '\0') {
        size_t k;
        s = pc_buf_append_str(out, "TITLE \"", err);
        for (k = 0; s == PC_OK && l->title[k] != '\0'; k++) {
            s = pc_buf_append_byte(out, (uint8_t)(l->title[k] == '"' ? '\'' : l->title[k]), err);
        }
        if (s == PC_OK) {
            s = pc_buf_append_str(out, "\"\n", err);
        }
    }
    if (s == PC_OK) {
        s = pc_buf_appendf(out, err, "LUT_3D_SIZE %zu\n", l->size);
    }
    for (i = 0; s == PC_OK && i + 3 <= l->ndata; i += 3) {
        s = put_f6(out, l->data[i], " ", err);
        if (s == PC_OK) { s = put_f6(out, l->data[i + 1], " ", err); }
        if (s == PC_OK) { s = put_f6(out, l->data[i + 2], "\n", err); }
    }
    if (s != PC_OK) {
        pc_buf_truncate(out, start);
    }
    return s;
}

/* ---------------------------------------------------------------- built-in looks */

static const char *const LOOK_IDS[PC_LUTFILE_BUILTIN_COUNT] = {
    "warm", "cool", "tealOrange", "bleachBypass", "fadedFilm", "dayForNight", "monoContrast", "crossProcess",
};
static const char *const LOOK_LABELS[PC_LUTFILE_BUILTIN_COUNT] = {
    "Warm Filter", "Cool Filter", "Teal & Orange", "Bleach Bypass", "Faded Film", "Day for Night",
    "Mono High Contrast", "Cross Process",
};

const char *pc_lutfile_builtin_id(size_t i) { return i < PC_LUTFILE_BUILTIN_COUNT ? LOOK_IDS[i] : NULL; }
const char *pc_lutfile_builtin_label(size_t i) { return i < PC_LUTFILE_BUILTIN_COUNT ? LOOK_LABELS[i] : NULL; }

static float luma(const float c[3])
{
    return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

static float smooth(float x)
{
    x = pc_clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static void look_fn_impl(const void *ctx, const float c[3], float o[3])
{
    size_t which = *(const size_t *)ctx;
    int k;
    switch (which) {
    case 0: /* warm */
        o[0] = c[0] * 0.92f + 0.08f;
        o[1] = c[1] * 0.97f + 0.02f;
        o[2] = c[2] * 0.85f;
        break;
    case 1: /* cool */
        o[0] = c[0] * 0.86f;
        o[1] = c[1] * 0.97f + 0.01f;
        o[2] = c[2] * 0.9f + 0.1f;
        break;
    case 2: { /* tealOrange: shadows toward teal, highlights toward orange */
        static const float teal[3] = {0.0f, 0.5f, 0.55f};
        static const float orange[3] = {1.0f, 0.62f, 0.3f};
        float l = luma(c);
        float t = smooth(l);
        for (k = 0; k < 3; k++) {
            float tint = teal[k] * (1.0f - t) + orange[k] * t;
            o[k] = c[k] * 0.75f + fminf(tint * l * 1.1f, 1.0f) * 0.25f + (smooth(c[k]) - c[k]) * 0.3f;
        }
        break;
    }
    case 3: { /* bleachBypass */
        float l = luma(c);
        float s = smooth(l);
        for (k = 0; k < 3; k++) {
            float desat = c[k] * 0.45f + l * 0.55f;
            o[k] = desat * 0.5f + s * 0.5f;
        }
        break;
    }
    case 4: { /* fadedFilm */
        static const float tint[3] = {1.0f, 0.96f, 0.88f};
        float l = luma(c);
        for (k = 0; k < 3; k++) {
            o[k] = (0.08f + (c[k] * 0.8f + l * 0.2f) * 0.84f) * tint[k] + 0.02f;
        }
        break;
    }
    case 5: { /* dayForNight */
        float l = luma(c);
        o[0] = l * 0.35f;
        o[1] = l * 0.45f + c[1] * 0.05f;
        o[2] = l * 0.65f + c[2] * 0.15f;
        break;
    }
    case 6: /* monoContrast */
        o[0] = o[1] = o[2] = smooth(smooth(luma(c)));
        break;
    default: /* crossProcess */
        o[0] = smooth(c[0]) * 1.05f;
        o[1] = c[1] * 1.1f - 0.03f;
        o[2] = c[2] * 0.7f + 0.15f;
        break;
    }
}

pc_status pc_lutfile_builtin(const char *id, pc_lutfile *out, pc_error *err)
{
    size_t i;
    for (i = 0; id != NULL && i < PC_LUTFILE_BUILTIN_COUNT; i++) {
        if (strcmp(id, LOOK_IDS[i]) == 0) {
            return from_fn(LOOK_LABELS[i], 33, look_fn_impl, &i, out, err);
        }
    }
    return pc_fail(err, PC_ERR_NOT_FOUND, "no built-in look '%s'", id != NULL ? id : "");
}
