#include "json.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

/* ---- parsing ---- */

/* Byte offset of the first '[' or '{' nested deeper than PC_JSON_MAX_DEPTH, or SIZE_MAX. Run
 * before parsing so yyjson never sees (and our recursive walkers never get) deeper input. */
static size_t too_deep_at(const char *t, size_t len)
{
    size_t depth = 0;
    bool in_str = false;
    bool esc = false;
    size_t i;
    for (i = 0; i < len; i++) {
        char c = t[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '[' || c == '{') {
            if (++depth > PC_JSON_MAX_DEPTH) {
                return i;
            }
        } else if ((c == ']' || c == '}') && depth > 0) {
            depth--;
        }
    }
    return SIZE_MAX;
}

/* serde_json-style 1-based line and column of byte offset pos. */
static void line_col(const char *t, size_t len, size_t pos, size_t *line, size_t *col)
{
    size_t i;
    *line = 1;
    *col = 1;
    if (pos > len) {
        pos = len;
    }
    for (i = 0; i < pos; i++) {
        if (t[i] == '\n') {
            (*line)++;
            *col = 1;
        } else {
            (*col)++;
        }
    }
}

static const char *serde_phrase(yyjson_read_code code, const char *fallback)
{
    /* yyjson's codes are `static const` objects, not constant expressions: no switch. */
    if (code == YYJSON_READ_ERROR_EMPTY_CONTENT || code == YYJSON_READ_ERROR_UNEXPECTED_END) {
        return "EOF while parsing a value";
    }
    if (code == YYJSON_READ_ERROR_UNEXPECTED_CHARACTER) {
        return "expected value";
    }
    if (code == YYJSON_READ_ERROR_UNEXPECTED_CONTENT) {
        return "trailing characters";
    }
    if (code == YYJSON_READ_ERROR_INVALID_NUMBER) {
        return "invalid number";
    }
    if (code == YYJSON_READ_ERROR_LITERAL) {
        return "expected ident";
    }
    return fallback;
}

pc_status pc_json_parse(const char *text, size_t len, yyjson_doc **out, pc_error *err)
{
    yyjson_read_err rerr;
    yyjson_doc *doc;
    size_t line;
    size_t col;
    size_t deep;

    if (text == NULL && len > 0) {
        return pc_fail(err, PC_ERR_INVALID, "no JSON text");
    }
    if (len == 0) { /* yyjson calls this a bad parameter; serde_json an empty document */
        return pc_fail(err, PC_ERR_FORMAT, "EOF while parsing a value at line 1 column 0");
    }
    deep = too_deep_at(text, len);
    if (deep != SIZE_MAX) {
        line_col(text, len, deep, &line, &col);
        return pc_fail(err, PC_ERR_FORMAT, "recursion limit exceeded at line %zu column %zu", line, col);
    }
    /* Without YYJSON_READ_INSITU, yyjson only reads the buffer. */
    doc = yyjson_read_opts((char *)(uintptr_t)(text != NULL ? text : ""), len, YYJSON_READ_NOFLAG, NULL, &rerr);
    if (doc == NULL) {
        if (rerr.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory parsing JSON");
        }
        /* serde reports the column of the last byte read; for running out of input that is the
         * last byte of the text ("tru" -> column 3, "  \n " -> line 2 column 1). */
        if (rerr.code == YYJSON_READ_ERROR_EMPTY_CONTENT || rerr.code == YYJSON_READ_ERROR_UNEXPECTED_END) {
            line_col(text, len, len, &line, &col);
            col--;
        } else {
            line_col(text, len, rerr.pos, &line, &col);
        }
        return pc_fail(err, PC_ERR_FORMAT, "%s at line %zu column %zu", serde_phrase(rerr.code, rerr.msg), line,
                       col);
    }
    *out = doc;
    return PC_OK;
}

/* ---- serde_json::Value accessors ---- */

yyjson_val *pc_json_get(yyjson_val *obj, const char *key)
{
    yyjson_val *k;
    yyjson_val *v;
    yyjson_val *found = NULL;
    size_t idx;
    size_t max;
    size_t klen;
    if (!yyjson_is_obj(obj) || key == NULL) {
        return NULL;
    }
    klen = strlen(key);
    yyjson_obj_foreach(obj, idx, max, k, v) {
        if (yyjson_get_len(k) == klen && memcmp(yyjson_get_str(k), key, klen) == 0) {
            found = v; /* keep scanning: the last duplicate wins, as in serde_json::Value */
        }
    }
    return found;
}

bool pc_json_as_u64(yyjson_val *v, uint64_t *out)
{
    if (yyjson_is_uint(v)) {
        *out = yyjson_get_uint(v);
        return true;
    }
    if (yyjson_is_sint(v) && yyjson_get_sint(v) >= 0) {
        *out = (uint64_t)yyjson_get_sint(v);
        return true;
    }
    return false;
}

bool pc_json_as_i64(yyjson_val *v, int64_t *out)
{
    if (yyjson_is_sint(v)) {
        *out = yyjson_get_sint(v);
        return true;
    }
    if (yyjson_is_uint(v) && yyjson_get_uint(v) <= (uint64_t)INT64_MAX) {
        *out = (int64_t)yyjson_get_uint(v);
        return true;
    }
    return false;
}

bool pc_json_as_f64(yyjson_val *v, double *out)
{
    if (!yyjson_is_num(v)) {
        return false;
    }
    *out = yyjson_get_num(v);
    return true;
}

bool pc_json_as_bool(yyjson_val *v, bool *out)
{
    if (!yyjson_is_bool(v)) {
        return false;
    }
    *out = yyjson_get_bool(v);
    return true;
}

const char *pc_json_as_str(yyjson_val *v, size_t *len)
{
    if (!yyjson_is_str(v)) {
        return NULL;
    }
    if (len != NULL) {
        *len = yyjson_get_len(v);
    }
    return yyjson_get_str(v);
}

/* ---- numbers in serde_json's format (zmij 1.0 layout) ---- */

/* Lay out shortest digits the way zmij's write() does. digits: no leading/trailing zeros,
 * value = d.ddd x 10^e. fixed_lo..fixed_hi is the exponent range written without "e". */
static size_t layout(bool neg, const char *digits, size_t n, int e, int fixed_lo, int fixed_hi,
                     char out[PC_JSON_NUM_MAX])
{
    size_t o = 0;
    size_t i;
    if (neg) {
        out[o++] = '-';
    }
    if (e >= fixed_lo && e <= fixed_hi) {
        if ((int)n - 1 <= e) { /* 1234e7 -> 12340000000.0 */
            for (i = 0; i < n; i++) {
                out[o++] = digits[i];
            }
            for (i = n; i < (size_t)e + 1; i++) {
                out[o++] = '0';
            }
            out[o++] = '.';
            out[o++] = '0';
        } else if (e >= 0) { /* 1234e-2 -> 12.34 */
            for (i = 0; i < n; i++) {
                if (i == (size_t)e + 1) {
                    out[o++] = '.';
                }
                out[o++] = digits[i];
            }
        } else { /* 1234e-6 -> 0.001234 */
            out[o++] = '0';
            out[o++] = '.';
            for (i = 1; i < (size_t)(-e); i++) {
                out[o++] = '0';
            }
            for (i = 0; i < n; i++) {
                out[o++] = digits[i];
            }
        }
    } else { /* 1234e30 -> 1.234e+33 */
        int a = e < 0 ? -e : e;
        out[o++] = digits[0];
        if (n > 1) {
            out[o++] = '.';
            for (i = 1; i < n; i++) {
                out[o++] = digits[i];
            }
        }
        out[o++] = 'e';
        out[o++] = e < 0 ? '-' : '+';
        if (a >= 100) {
            out[o++] = (char)('0' + a / 100);
        }
        if (a >= 10) {
            out[o++] = (char)('0' + a / 10 % 10);
        }
        out[o++] = (char)('0' + a % 10);
    }
    out[o] = '\0';
    return o;
}

/* Shortest round-trip digits come from yyjson (an exact shortest algorithm, like zmij); this
 * re-lays them out. */
static size_t format_real(const yyjson_mut_val *val, bool is_f32, char out[PC_JSON_NUM_MAX])
{
    char pool[512];
    yyjson_alc alc;
    const char *s;
    size_t slen;
    char digits[32];
    size_t n = 0;
    bool neg = false;
    bool seen_point = false;
    bool nonzero = false;
    long before_point = 0; /* significant digits before '.' (after leading zeros) */
    long leading_frac_zeros = 0;
    long exp10 = 0;
    size_t i;
    double d = yyjson_mut_get_real(val);

    if (!isfinite(d)) {
        memcpy(out, "null", 5);
        return 4;
    }
    if (!yyjson_alc_pool_init(&alc, pool, sizeof pool) ||
        (s = yyjson_mut_val_write_opts(val, YYJSON_WRITE_NOFLAG, &alc, &slen, NULL)) == NULL) {
        memcpy(out, "null", 5); /* unreachable with a 512-byte pool */
        return 4;
    }
    for (i = 0; i < slen; i++) {
        char c = s[i];
        if (c == '-') {
            neg = true;
        } else if (c == '.') {
            seen_point = true;
        } else if (c == 'e' || c == 'E') {
            exp10 = strtol(s + i + 1, NULL, 10);
            break;
        } else if (c >= '0' && c <= '9') {
            if (!nonzero && c == '0') {
                if (seen_point) {
                    leading_frac_zeros++;
                }
                continue;
            }
            nonzero = true;
            if (n < sizeof digits) {
                digits[n++] = c;
            }
            if (!seen_point) {
                before_point++;
            }
        }
    }
    while (n > 0 && digits[n - 1] == '0') {
        n--;
    }
    if (n == 0) {
        return layout(neg, "0", 1, 0, -5, 15, out); /* "0.0" / "-0.0" */
    }
    {
        long e = (before_point > 0 ? before_point - 1 : -(leading_frac_zeros + 1)) + exp10;
        return is_f32 ? layout(neg, digits, n, (int)e, -6, 12, out) : layout(neg, digits, n, (int)e, -5, 15, out);
    }
}

static bool mut_is_f32(const yyjson_mut_val *v)
{
    return (v->tag & ((uint64_t)YYJSON_WRITE_FP_TO_FLOAT << 32)) != 0;
}

size_t pc_json_format_f64(double v, char out[PC_JSON_NUM_MAX])
{
    yyjson_mut_val tmp;
    memset(&tmp, 0, sizeof tmp);
    (void)yyjson_mut_set_real(&tmp, v);
    return format_real(&tmp, false, out);
}

size_t pc_json_format_f32(float v, char out[PC_JSON_NUM_MAX])
{
    yyjson_mut_val tmp;
    memset(&tmp, 0, sizeof tmp);
    (void)yyjson_mut_set_float(&tmp, v);
    return format_real(&tmp, true, out);
}

/* ---- writing ---- */

enum { MAX_WRITE_DEPTH = 1024 };

typedef struct writer {
    pc_buf *out;
    unsigned flags;
    pc_error *err;
} writer;

static pc_status put(writer *w, const char *s, size_t n)
{
    return pc_buf_append(w->out, s, n, w->err);
}

static pc_status newline_indent(writer *w, size_t depth)
{
    pc_status s = put(w, "\n", 1);
    size_t i;
    for (i = 0; s == PC_OK && i < depth; i++) {
        s = put(w, "  ", 2);
    }
    return s;
}

/* serde_json's escaping: \" \\ \b \f \n \r \t, other C0 controls as \u00xx (lower-case hex). */
static pc_status put_string(writer *w, const char *str, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    size_t run = 0; /* start of the pending unescaped run */
    pc_status s = put(w, "\"", 1);
    for (i = 0; s == PC_OK && i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        char esc[7];
        size_t elen = 2;
        if (c >= 0x20 && c != '"' && c != '\\') {
            continue;
        }
        s = put(w, str + run, i - run);
        run = i + 1;
        esc[0] = '\\';
        switch (c) {
        case '"': esc[1] = '"'; break;
        case '\\': esc[1] = '\\'; break;
        case '\b': esc[1] = 'b'; break;
        case '\f': esc[1] = 'f'; break;
        case '\n': esc[1] = 'n'; break;
        case '\r': esc[1] = 'r'; break;
        case '\t': esc[1] = 't'; break;
        default:
            memcpy(esc, "\\u00", 4);
            esc[4] = hex[c >> 4];
            esc[5] = hex[c & 15];
            elen = 6;
            break;
        }
        if (s == PC_OK) {
            s = put(w, esc, elen);
        }
    }
    if (s == PC_OK) {
        s = put(w, str + run, len - run);
    }
    return s == PC_OK ? put(w, "\"", 1) : s;
}

typedef struct member {
    const char *key;
    size_t klen;
    yyjson_mut_val *val;
    size_t index;
} member;

static int member_cmp(const void *a, const void *b)
{
    const member *x = a;
    const member *y = b;
    size_t n = x->klen < y->klen ? x->klen : y->klen;
    int c = memcmp(x->key, y->key, n);
    if (c != 0) {
        return c;
    }
    if (x->klen != y->klen) {
        return x->klen < y->klen ? -1 : 1;
    }
    return x->index < y->index ? -1 : (x->index > y->index ? 1 : 0);
}

static pc_status write_val(writer *w, yyjson_mut_val *v, size_t depth);

static pc_status write_obj(writer *w, yyjson_mut_val *obj, size_t depth)
{
    size_t count = yyjson_mut_obj_size(obj);
    member *m;
    size_t n = 0;
    size_t i;
    yyjson_mut_obj_iter it;
    yyjson_mut_val *k;
    pc_status s;
    bool pretty = (w->flags & PC_JSON_PRETTY) != 0;
    bool first = true;

    if (count == 0) {
        return put(w, "{}", 2);
    }
    m = pc_malloc_array(count, sizeof *m);
    if (m == NULL) {
        return pc_fail(w->err, PC_ERR_NOMEM, "out of memory writing JSON");
    }
    if (!yyjson_mut_obj_iter_init(obj, &it)) {
        free(m);
        return pc_fail(w->err, PC_ERR_INVALID, "invalid JSON object");
    }
    while (n < count && (k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        m[n].key = yyjson_mut_get_str(k);
        m[n].klen = yyjson_mut_get_len(k);
        m[n].val = yyjson_mut_obj_iter_get_val(k);
        m[n].index = n;
        n++;
    }
    if ((w->flags & PC_JSON_SORT_KEYS) != 0) {
        qsort(m, n, sizeof *m, member_cmp);
    }
    s = put(w, "{", 1);
    for (i = 0; s == PC_OK && i < n; i++) {
        /* Sorted: a key equal to the next one is a duplicate the later entry replaces. */
        if ((w->flags & PC_JSON_SORT_KEYS) != 0 && i + 1 < n && m[i].klen == m[i + 1].klen &&
            memcmp(m[i].key, m[i + 1].key, m[i].klen) == 0) {
            continue;
        }
        if (!first) {
            s = put(w, ",", 1);
        }
        first = false;
        if (s == PC_OK && pretty) {
            s = newline_indent(w, depth + 1);
        }
        if (s == PC_OK) {
            s = put_string(w, m[i].key, m[i].klen);
        }
        if (s == PC_OK) {
            s = pretty ? put(w, ": ", 2) : put(w, ":", 1);
        }
        if (s == PC_OK) {
            s = write_val(w, m[i].val, depth + 1);
        }
    }
    if (s == PC_OK && pretty) {
        s = newline_indent(w, depth);
    }
    if (s == PC_OK) {
        s = put(w, "}", 1);
    }
    free(m);
    return s;
}

static pc_status write_arr(writer *w, yyjson_mut_val *arr, size_t depth)
{
    size_t idx;
    yyjson_mut_arr_iter it;
    yyjson_mut_val *v;
    pc_status s;
    bool pretty = (w->flags & PC_JSON_PRETTY) != 0;

    if (yyjson_mut_arr_size(arr) == 0) {
        return put(w, "[]", 2);
    }
    s = put(w, "[", 1);
    if (!yyjson_mut_arr_iter_init(arr, &it)) {
        return pc_fail(w->err, PC_ERR_INVALID, "invalid JSON array");
    }
    for (idx = 0; s == PC_OK && (v = yyjson_mut_arr_iter_next(&it)) != NULL; idx++) {
        if (idx > 0) {
            s = put(w, ",", 1);
        }
        if (s == PC_OK && pretty) {
            s = newline_indent(w, depth + 1);
        }
        if (s == PC_OK) {
            s = write_val(w, v, depth + 1);
        }
    }
    if (s == PC_OK && pretty) {
        s = newline_indent(w, depth);
    }
    return s == PC_OK ? put(w, "]", 1) : s;
}

static pc_status write_val(writer *w, yyjson_mut_val *v, size_t depth)
{
    char num[PC_JSON_NUM_MAX];
    int n;
    if (depth > MAX_WRITE_DEPTH) {
        return pc_fail(w->err, PC_ERR_LIMIT, "JSON nested deeper than %d levels", MAX_WRITE_DEPTH);
    }
    switch (yyjson_mut_get_type(v)) {
    case YYJSON_TYPE_NULL: return put(w, "null", 4);
    case YYJSON_TYPE_BOOL: return yyjson_mut_get_bool(v) ? put(w, "true", 4) : put(w, "false", 5);
    case YYJSON_TYPE_STR: return put_string(w, yyjson_mut_get_str(v), yyjson_mut_get_len(v));
    case YYJSON_TYPE_RAW: return put(w, yyjson_mut_get_raw(v), yyjson_mut_get_len(v));
    case YYJSON_TYPE_ARR: return write_arr(w, v, depth);
    case YYJSON_TYPE_OBJ: return write_obj(w, v, depth);
    case YYJSON_TYPE_NUM:
        if (yyjson_mut_is_uint(v)) {
            n = snprintf(num, sizeof num, "%" PRIu64, yyjson_mut_get_uint(v));
        } else if (yyjson_mut_is_sint(v)) {
            n = snprintf(num, sizeof num, "%" PRId64, yyjson_mut_get_sint(v));
        } else {
            n = (int)format_real(v, mut_is_f32(v), num);
        }
        return n < 0 ? pc_fail(w->err, PC_ERR_INVALID, "bad number") : put(w, num, (size_t)n);
    case YYJSON_TYPE_NONE:
    default: break;
    }
    return pc_fail(w->err, PC_ERR_INVALID, "invalid JSON value");
}

pc_status pc_json_write(const yyjson_mut_val *root, unsigned flags, pc_buf *out, pc_error *err)
{
    writer w;
    size_t start = out->len;
    pc_status s;
    if (root == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no JSON value");
    }
    w.out = out;
    w.flags = flags;
    w.err = err;
    /* yyjson's read-only accessors take non-const pointers. */
    s = write_val(&w, (yyjson_mut_val *)(uintptr_t)root, 0);
    if (s != PC_OK) {
        pc_buf_truncate(out, start);
    }
    return s;
}

pc_status pc_json_write_val(yyjson_val *root, unsigned flags, pc_buf *out, pc_error *err)
{
    yyjson_mut_doc *md;
    yyjson_mut_val *copy;
    pc_status s;
    if (root == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no JSON value");
    }
    md = yyjson_mut_doc_new(NULL);
    copy = md != NULL ? yyjson_val_mut_copy(md, root) : NULL;
    if (copy == NULL) {
        yyjson_mut_doc_free(md);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory writing JSON");
    }
    s = pc_json_write(copy, flags, out, err);
    yyjson_mut_doc_free(md);
    return s;
}
