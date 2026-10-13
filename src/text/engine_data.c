/* EngineData parsing and writing (port of crates/text/src/engine_data.rs). See engine_data.h. */
#include "text/engine_data.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"

/* ---- construction and ownership ---- */

pc_ed_value pc_ed_dict(void)
{
    pc_ed_value v;
    memset(&v, 0, sizeof v);
    v.kind = PC_ED_DICT;
    return v;
}

pc_ed_value pc_ed_array(void)
{
    pc_ed_value v = pc_ed_dict();
    v.kind = PC_ED_ARRAY;
    return v;
}

pc_ed_value pc_ed_int(int64_t i)
{
    pc_ed_value v = pc_ed_dict();
    v.kind = PC_ED_INT;
    v.i = i;
    return v;
}

pc_ed_value pc_ed_real(double r)
{
    pc_ed_value v = pc_ed_dict();
    v.kind = PC_ED_REAL;
    v.r = r;
    return v;
}

pc_ed_value pc_ed_bool(bool b)
{
    pc_ed_value v = pc_ed_dict();
    v.kind = PC_ED_BOOL;
    v.b = b;
    return v;
}

static pc_status ed_text(pc_ed_kind kind, const char *s, size_t len, pc_ed_value *out, pc_error *err)
{
    pc_ed_value v = pc_ed_dict();
    pc_status st = pc_str_set(&v.s, s, len, err);
    if (st != PC_OK) {
        return st;
    }
    v.kind = kind;
    *out = v;
    return PC_OK;
}

pc_status pc_ed_string(const char *s, size_t len, pc_ed_value *out, pc_error *err)
{
    return ed_text(PC_ED_STRING, s, len, out, err);
}

pc_status pc_ed_name(const char *s, size_t len, pc_ed_value *out, pc_error *err)
{
    return ed_text(PC_ED_NAME, s, len, out, err);
}

void pc_ed_fini(pc_ed_value *v)
{
    size_t k;
    if (v == NULL) {
        return;
    }
    pc_str_fini(&v->s);
    if (v->items != NULL) {
        for (k = 0; k < v->n; k++) {
            pc_ed_fini(&v->items[k]);
        }
        free(v->items);
    }
    if (v->entries != NULL) {
        for (k = 0; k < v->n; k++) {
            pc_str_fini(&v->entries[k].key);
            pc_ed_fini(&v->entries[k].value);
        }
        free(v->entries);
    }
    *v = pc_ed_dict();
}

static pc_status clone_at(const pc_ed_value *v, size_t depth, pc_ed_value *out, pc_error *err)
{
    pc_ed_value c = pc_ed_dict();
    pc_status st = PC_OK;
    size_t k;
    if (depth > PC_ED_MAX_TREE_DEPTH) {
        return pc_fail(err, PC_ERR_LIMIT, "EngineData: nesting too deep");
    }
    c.kind = v->kind;
    c.i = v->i;
    c.r = v->r;
    c.b = v->b;
    if (v->kind == PC_ED_STRING || v->kind == PC_ED_NAME) {
        st = pc_str_clone(&v->s, &c.s, err);
    } else if (v->kind == PC_ED_ARRAY && v->n > 0) {
        c.items = pc_calloc_array(v->n, sizeof *c.items);
        if (c.items == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying EngineData");
        }
        c.cap = v->n;
        for (k = 0; k < v->n && st == PC_OK; k++) {
            st = clone_at(&v->items[k], depth + 1, &c.items[k], err);
            c.n = k + 1;
        }
    } else if (v->kind == PC_ED_DICT && v->n > 0) {
        c.entries = pc_calloc_array(v->n, sizeof *c.entries);
        if (c.entries == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying EngineData");
        }
        c.cap = v->n;
        for (k = 0; k < v->n && st == PC_OK; k++) {
            c.n = k + 1;
            st = pc_str_clone(&v->entries[k].key, &c.entries[k].key, err);
            if (st == PC_OK) {
                st = clone_at(&v->entries[k].value, depth + 1, &c.entries[k].value, err);
            }
        }
    }
    if (st != PC_OK) {
        pc_ed_fini(&c);
        return st;
    }
    *out = c;
    return PC_OK;
}

pc_status pc_ed_clone(const pc_ed_value *v, pc_ed_value *out, pc_error *err)
{
    return clone_at(v, 0, out, err);
}

static bool eq_at(const pc_ed_value *a, const pc_ed_value *b, size_t depth)
{
    size_t k;
    if (a->kind != b->kind || depth > PC_ED_MAX_TREE_DEPTH) {
        return false;
    }
    switch (a->kind) {
    case PC_ED_STRING:
    case PC_ED_NAME:
        return pc_str_eq(&a->s, &b->s);
    case PC_ED_INT:
        return a->i == b->i;
    case PC_ED_REAL:
        return a->r == b->r;
    case PC_ED_BOOL:
        return a->b == b->b;
    case PC_ED_ARRAY:
        if (a->n != b->n) {
            return false;
        }
        for (k = 0; k < a->n; k++) {
            if (!eq_at(&a->items[k], &b->items[k], depth + 1)) {
                return false;
            }
        }
        return true;
    case PC_ED_DICT:
    default:
        if (a->n != b->n) {
            return false;
        }
        for (k = 0; k < a->n; k++) {
            if (!pc_str_eq(&a->entries[k].key, &b->entries[k].key) ||
                !eq_at(&a->entries[k].value, &b->entries[k].value, depth + 1)) {
                return false;
            }
        }
        return true;
    }
}

bool pc_ed_eq(const pc_ed_value *a, const pc_ed_value *b)
{
    return eq_at(a, b, 0);
}

/* ---- access ---- */

const pc_ed_value *pc_ed_get(const pc_ed_value *v, const char *key)
{
    size_t k;
    if (v == NULL || v->kind != PC_ED_DICT) {
        return NULL;
    }
    for (k = 0; k < v->n; k++) {
        if (pc_str_eq_c(&v->entries[k].key, key)) {
            return &v->entries[k].value;
        }
    }
    return NULL;
}

pc_ed_value *pc_ed_get_mut(pc_ed_value *v, const char *key)
{
    size_t k;
    if (v == NULL || v->kind != PC_ED_DICT) {
        return NULL;
    }
    for (k = 0; k < v->n; k++) {
        if (pc_str_eq_c(&v->entries[k].key, key)) {
            return &v->entries[k].value;
        }
    }
    return NULL;
}

const pc_ed_value *pc_ed_path(const pc_ed_value *v, ...)
{
    va_list ap;
    const char *key;
    va_start(ap, v);
    while (v != NULL && (key = va_arg(ap, const char *)) != NULL) {
        v = pc_ed_get(v, key);
    }
    va_end(ap);
    return v;
}

pc_status pc_ed_set(pc_ed_value *dict, const char *key, pc_ed_value *val, pc_error *err)
{
    pc_ed_value *slot;
    pc_ed_entry *grown;
    pc_str k = PC_STR_INIT;
    pc_status st;
    if (dict->kind != PC_ED_DICT) {
        pc_ed_fini(val);
        return PC_OK;
    }
    slot = pc_ed_get_mut(dict, key);
    if (slot != NULL) {
        pc_ed_fini(slot);
        *slot = *val;
        *val = pc_ed_dict();
        return PC_OK;
    }
    st = pc_str_set_c(&k, key, err);
    if (st != PC_OK) {
        return st;
    }
    if (dict->n >= dict->cap || dict->entries == NULL) {
        size_t cap = dict->cap;
        grown = pc_grow_array(dict->entries, &cap, dict->n + 1, sizeof *grown);
        if (grown == NULL) {
            pc_str_fini(&k);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory building EngineData");
        }
        dict->entries = grown;
        dict->cap = cap;
    }
    dict->entries[dict->n].key = k;
    dict->entries[dict->n].value = *val;
    dict->n++;
    *val = pc_ed_dict();
    return PC_OK;
}

pc_status pc_ed_push(pc_ed_value *array, pc_ed_value *val, pc_error *err)
{
    pc_ed_value *grown;
    if (array->kind != PC_ED_ARRAY) {
        pc_ed_fini(val);
        return PC_OK;
    }
    if (array->n >= array->cap || array->items == NULL) {
        size_t cap = array->cap;
        grown = pc_grow_array(array->items, &cap, array->n + 1, sizeof *grown);
        if (grown == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory building EngineData");
        }
        array->items = grown;
        array->cap = cap;
    }
    array->items[array->n++] = *val;
    *val = pc_ed_dict();
    return PC_OK;
}

void pc_ed_remove(pc_ed_value *dict, const char *key)
{
    size_t r, w = 0;
    if (dict == NULL || dict->kind != PC_ED_DICT) {
        return;
    }
    for (r = 0; r < dict->n; r++) {
        if (pc_str_eq_c(&dict->entries[r].key, key)) {
            pc_str_fini(&dict->entries[r].key);
            pc_ed_fini(&dict->entries[r].value);
        } else {
            dict->entries[w++] = dict->entries[r];
        }
    }
    dict->n = w;
}

bool pc_ed_as_f64(const pc_ed_value *v, double *out)
{
    if (v != NULL && v->kind == PC_ED_INT) {
        *out = (double)v->i;
        return true;
    }
    if (v != NULL && v->kind == PC_ED_REAL) {
        *out = v->r;
        return true;
    }
    return false;
}

bool pc_ed_as_i64(const pc_ed_value *v, int64_t *out)
{
    if (v != NULL && v->kind == PC_ED_INT) {
        *out = v->i;
        return true;
    }
    if (v != NULL && v->kind == PC_ED_REAL) {
        *out = pc_f64_to_i64(v->r);
        return true;
    }
    return false;
}

bool pc_ed_as_bool(const pc_ed_value *v, bool *out)
{
    if (v != NULL && v->kind == PC_ED_BOOL) {
        *out = v->b;
        return true;
    }
    if (v != NULL && v->kind == PC_ED_INT) {
        *out = v->i != 0;
        return true;
    }
    return false;
}

const pc_str *pc_ed_as_str(const pc_ed_value *v)
{
    return v != NULL && (v->kind == PC_ED_STRING || v->kind == PC_ED_NAME) ? &v->s : NULL;
}

const pc_ed_value *pc_ed_as_array(const pc_ed_value *v, size_t *n)
{
    if (v == NULL || v->kind != PC_ED_ARRAY) {
        return NULL;
    }
    *n = v->n;
    /* An empty array still answers (non-NULL) so callers can tell it from "not an array". */
    return v->items != NULL ? v->items : v;
}

/* ---- parsing ---- */

typedef struct parser {
    const uint8_t *b;
    size_t len;
    size_t i; /* may run one past len after a trailing backslash, as in the reference */
} parser;

static bool is_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0;
}

static bool is_delim(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '[' || c == ']' || c == '<' || c == '>' ||
           c == '(' || c == '/';
}

static int peek(const parser *p, size_t at)
{
    return at < p->len ? p->b[at] : -1;
}

static void skip_ws(parser *p)
{
    while (p->i < p->len && is_ws(p->b[p->i])) {
        p->i++;
    }
}

static pc_status perr(const parser *p, const char *m, pc_error *err)
{
    return pc_fail(err, PC_ERR_FORMAT, "EngineData: %s at byte %zu", m, p->i);
}

/* `/Name`: the bytes up to the next delimiter, lossy UTF-8. */
static pc_status parse_name(parser *p, pc_str *out, pc_error *err)
{
    size_t start;
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    size_t n;
    uint8_t *data;
    p->i++;
    start = p->i;
    while (p->i < p->len && !is_delim(p->b[p->i])) {
        p->i++;
    }
    st = pc_utf8_lossy(p->b + start, p->i - start, &b, err);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    data = pc_buf_take(&b, &n);
    pc_str_fini(out);
    *out = data != NULL ? pc_str_adopt((char *)data, n) : (pc_str)PC_STR_INIT;
    return PC_OK;
}

/* decode_string: UTF-16BE after a FE FF mark (a trailing odd byte is dropped), else Latin-1. */
static pc_status decode_string(const uint8_t *raw, size_t n, pc_str *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status st = PC_OK;
    size_t k, len;
    uint8_t *data;
    if (n >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        st = pc_utf16be_lossy(raw + 2, (n - 2) / 2, &b, err);
    } else {
        for (k = 0; k < n && st == PC_OK; k++) {
            st = pc_utf8_put(&b, raw[k], err);
        }
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    data = pc_buf_take(&b, &len);
    *out = data != NULL ? pc_str_adopt((char *)data, len) : (pc_str)PC_STR_INIT;
    return PC_OK;
}

static bool ieq_word(const uint8_t *s, size_t n, const char *w)
{
    size_t k, wl = strlen(w);
    if (n != wl) {
        return false;
    }
    for (k = 0; k < n; k++) {
        uint8_t c = s[k];
        if (c >= 'A' && c <= 'Z') {
            c = (uint8_t)(c - 'A' + 'a');
        }
        if (c != (uint8_t)w[k]) {
            return false;
        }
    }
    return true;
}

/* Rust's f64 grammar: [+-] (inf | infinity | nan | digits [. digits] [e [+-] digits]), at least
 * one mantissa digit; no hex, no spaces. */
static bool float_syntax(const uint8_t *s, size_t n)
{
    size_t i = 0, digits = 0;
    if (n == 0) {
        return false;
    }
    if (s[0] == '+' || s[0] == '-') {
        i = 1;
    }
    if (ieq_word(s + i, n - i, "inf") || ieq_word(s + i, n - i, "infinity") ||
        ieq_word(s + i, n - i, "nan")) {
        return true;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        i++;
        digits++;
    }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
            digits++;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t ed = 0;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
            ed++;
        }
        if (ed == 0) {
            return false;
        }
    }
    return i == n;
}

/* str::parse::<f64>: 1 ok, 0 syntax error, -1 out of memory. strtod is correctly rounded on
 * the POSIX libcs we support, like Rust's parser. */
static int parse_f64(const uint8_t *s, size_t n, double *out)
{
    char *z;
    if (!float_syntax(s, n)) {
        return 0;
    }
    z = pc_strndup((const char *)s, n);
    if (z == NULL) {
        return -1;
    }
    *out = strtod(z, NULL);
    free(z);
    return 1;
}

/* str::parse::<i64>: [+-] digits, no overflow. */
static bool parse_i64(const uint8_t *s, size_t n, int64_t *out)
{
    size_t i = 0;
    bool neg = false;
    uint64_t mag = 0, lim;
    if (n > 0 && (s[0] == '+' || s[0] == '-')) {
        neg = s[0] == '-';
        i = 1;
    }
    if (i == n) {
        return false;
    }
    lim = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    for (; i < n; i++) {
        unsigned d;
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        d = (unsigned)(s[i] - '0');
        if (mag > (lim - d) / 10u) {
            return false;
        }
        mag = mag * 10u + d;
    }
    if (neg) {
        *out = mag == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)mag;
    } else {
        *out = (int64_t)mag;
    }
    return true;
}

static bool valid_utf8(const uint8_t *s, size_t n)
{
    size_t i = 0;
    uint32_t cp;
    while (i < n) {
        if (!pc_utf8_next(s, n, &i, &cp)) {
            return false;
        }
    }
    return true;
}

static pc_status parse_value(parser *p, size_t depth, pc_ed_value *out, pc_error *err);

static pc_status parse_dict(parser *p, size_t depth, pc_ed_value *out, pc_error *err)
{
    pc_ed_value d = pc_ed_dict();
    pc_status st = PC_OK;
    p->i += 2;
    for (;;) {
        int c;
        skip_ws(p);
        c = peek(p, p->i);
        if (c == '>' && peek(p, p->i + 1) == '>') {
            p->i += 2;
            *out = d;
            return PC_OK;
        }
        if (c == '/') {
            pc_str k = PC_STR_INIT;
            pc_ed_value v = pc_ed_dict();
            pc_ed_entry *grown;
            st = parse_name(p, &k, err);
            if (st == PC_OK) {
                st = parse_value(p, depth + 1, &v, err);
            }
            if (st == PC_OK && (d.n >= d.cap || d.entries == NULL)) {
                size_t cap = d.cap;
                grown = pc_grow_array(d.entries, &cap, d.n + 1, sizeof *grown);
                if (grown == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory parsing EngineData");
                } else {
                    d.entries = grown;
                    d.cap = cap;
                }
            }
            if (st != PC_OK) {
                pc_str_fini(&k);
                pc_ed_fini(&v);
                break;
            }
            d.entries[d.n].key = k;
            d.entries[d.n].value = v;
            d.n++;
            continue;
        }
        st = perr(p, c < 0 ? "unterminated dictionary" : "expected key", err);
        break;
    }
    pc_ed_fini(&d);
    return st;
}

static pc_status parse_array(parser *p, size_t depth, pc_ed_value *out, pc_error *err)
{
    pc_ed_value a = pc_ed_array();
    pc_status st;
    p->i += 1;
    for (;;) {
        pc_ed_value v = pc_ed_dict();
        int c;
        skip_ws(p);
        c = peek(p, p->i);
        if (c == ']') {
            p->i += 1;
            *out = a;
            return PC_OK;
        }
        if (c < 0) {
            st = perr(p, "unterminated array", err);
            break;
        }
        st = parse_value(p, depth + 1, &v, err);
        if (st == PC_OK) {
            st = pc_ed_push(&a, &v, err);
        }
        if (st != PC_OK) {
            pc_ed_fini(&v);
            break;
        }
    }
    pc_ed_fini(&a);
    return st;
}

static pc_status parse_string(parser *p, pc_ed_value *out, pc_error *err)
{
    pc_buf raw = PC_BUF_INIT;
    pc_status st = PC_OK;
    pc_ed_value v = pc_ed_dict();
    p->i += 1;
    for (;;) {
        int c = peek(p, p->i);
        if (c < 0) {
            st = perr(p, "unterminated string", err);
            break;
        }
        if (c == '\\') {
            int e = peek(p, p->i + 1);
            if (e >= 0) {
                st = pc_buf_append_byte(&raw, (uint8_t)e, err);
            }
            p->i += 2;
        } else if (c == ')') {
            p->i += 1;
            break;
        } else {
            st = pc_buf_append_byte(&raw, (uint8_t)c, err);
            p->i += 1;
        }
        if (st != PC_OK) {
            break;
        }
    }
    if (st == PC_OK) {
        st = decode_string(raw.data, raw.len, &v.s, err);
    }
    pc_buf_fini(&raw);
    if (st != PC_OK) {
        return st;
    }
    v.kind = PC_ED_STRING;
    *out = v;
    return PC_OK;
}

static bool has_byte(const uint8_t *s, size_t n, uint8_t c)
{
    return n > 0 && memchr(s, c, n) != NULL;
}

static pc_status parse_token(parser *p, pc_ed_value *out, pc_error *err)
{
    size_t start = p->i, n;
    const uint8_t *t;
    int64_t iv;
    double rv;
    int r;
    while (p->i < p->len && !is_delim(p->b[p->i])) {
        p->i++;
    }
    t = p->b + start;
    n = p->i - start;
    if (!valid_utf8(t, n)) {
        n = 0; /* the reference reads a non-UTF-8 token as "" */
    }
    if (n == 4 && memcmp(t, "true", 4) == 0) {
        *out = pc_ed_bool(true);
        return PC_OK;
    }
    if (n == 5 && memcmp(t, "false", 5) == 0) {
        *out = pc_ed_bool(false);
        return PC_OK;
    }
    if (n == 0) {
        p->i += 1;
        return perr(p, "unexpected byte", err);
    }
    if (has_byte(t, n, '.') || has_byte(t, n, 'e') || has_byte(t, n, 'E')) {
        r = parse_f64(t, n, &rv);
        if (r < 0) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory parsing EngineData");
        }
        if (r == 0) {
            return perr(p, "bad number", err);
        }
        *out = pc_ed_real(rv);
        return PC_OK;
    }
    if (parse_i64(t, n, &iv)) {
        *out = pc_ed_int(iv);
        return PC_OK;
    }
    r = parse_f64(t, n, &rv);
    if (r < 0) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory parsing EngineData");
    }
    if (r == 0) {
        return perr(p, "bad token", err);
    }
    *out = pc_ed_real(rv);
    return PC_OK;
}

static pc_status parse_value(parser *p, size_t depth, pc_ed_value *out, pc_error *err)
{
    int c;
    if (depth > PC_ED_MAX_DEPTH) {
        return perr(p, "nesting too deep", err);
    }
    skip_ws(p);
    c = peek(p, p->i);
    if (c < 0) {
        return perr(p, "unexpected end", err);
    }
    if (c == '<' && peek(p, p->i + 1) == '<') {
        return parse_dict(p, depth, out, err);
    }
    if (c == '[') {
        return parse_array(p, depth, out, err);
    }
    if (c == '(') {
        return parse_string(p, out, err);
    }
    if (c == '/') {
        pc_ed_value v = pc_ed_dict();
        pc_status st = parse_name(p, &v.s, err);
        if (st != PC_OK) {
            return st;
        }
        v.kind = PC_ED_NAME;
        *out = v;
        return PC_OK;
    }
    return parse_token(p, out, err);
}

pc_status pc_ed_parse(const uint8_t *data, size_t len, pc_ed_value *out, pc_error *err)
{
    parser p;
    pc_ed_value v = pc_ed_dict();
    pc_status st;
    p.b = data;
    p.len = data != NULL ? len : 0;
    p.i = 0;
    st = parse_value(&p, 0, &v, err);
    if (st != PC_OK) {
        return st;
    }
    *out = v;
    return PC_OK;
}

/* ---- writing ---- */

size_t pc_ed_format_real(double r, char out[PC_ED_REAL_MAX])
{
    char s[PC_ED_REAL_MAX];
    size_t n;
    int w;
    if (!isfinite(r)) {
        memcpy(out, "0.0", 4);
        return 3;
    }
    if (r == trunc(r) && fabs(r) < 1e15) {
        w = snprintf(out, PC_ED_REAL_MAX, "%.1f", r);
        return w > 0 ? (size_t)w : 0;
    }
    /* `{:.5}` is the exact decimal value rounded half to even, as glibc/BSD printf do. */
    w = snprintf(s, sizeof s, "%.5f", r);
    if (w <= 0 || (size_t)w >= sizeof s) {
        memcpy(out, "0.0", 4);
        return 3;
    }
    n = (size_t)w;
    while (n > 0 && s[n - 1] == '0') {
        n--;
    }
    s[n] = '\0';
    if (strcmp(s, "0.") == 0 || strcmp(s, "-0.") == 0) {
        memcpy(out, "0.0", 4);
        return 3;
    }
    if (n >= 2 && s[0] == '0' && s[1] == '.') {
        memcpy(out, s + 1, n); /* ".xxx" + NUL */
        return n - 1;
    }
    if (n >= 3 && s[0] == '-' && s[1] == '0' && s[2] == '.') {
        out[0] = '-';
        memcpy(out + 1, s + 2, n - 1);
        return n - 1;
    }
    memcpy(out, s, n + 1);
    return n;
}

/* Sticky writer: the first failure is kept and later appends do nothing. */
typedef struct wr {
    pc_buf *b;
    pc_status st;
    pc_error *err;
} wr;

static void put(wr *w, const void *p, size_t n)
{
    if (w->st == PC_OK) {
        w->st = pc_buf_append(w->b, p, n, w->err);
    }
}

static void putc_(wr *w, uint8_t c)
{
    put(w, &c, 1);
}

static void indent(wr *w, size_t n)
{
    size_t k;
    for (k = 0; k < n && w->st == PC_OK; k++) {
        putc_(w, '\t');
    }
}

static bool is_container(const pc_ed_value *v)
{
    size_t k;
    if (v->kind == PC_ED_DICT) {
        return true;
    }
    if (v->kind != PC_ED_ARRAY) {
        return false;
    }
    for (k = 0; k < v->n; k++) {
        if (v->items[k].kind == PC_ED_DICT || v->items[k].kind == PC_ED_ARRAY) {
            return true;
        }
    }
    return false;
}

/* One UTF-16 unit as escaped big-endian bytes. */
static void put_unit(wr *w, uint16_t u)
{
    uint8_t bytes[2] = {(uint8_t)(u >> 8), (uint8_t)u};
    int k;
    for (k = 0; k < 2; k++) {
        if (bytes[k] == '(' || bytes[k] == ')' || bytes[k] == '\\') {
            putc_(w, '\\');
        }
        putc_(w, bytes[k]);
    }
}

/* `( FE FF utf16be )` with ( ) \ escaped. Invalid UTF-8 in s is read as U+FFFD. */
static void put_string(wr *w, const pc_str *s)
{
    const uint8_t *p = (const uint8_t *)pc_str_c(s);
    size_t i = 0;
    putc_(w, '(');
    put_unit(w, 0xFEFF);
    while (i < s->len && w->st == PC_OK) {
        uint32_t cp;
        if (!pc_utf8_next(p, s->len, &i, &cp)) {
            cp = 0xFFFD;
            i++;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            put_unit(w, (uint16_t)(0xD800 + (cp >> 10)));
            put_unit(w, (uint16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            put_unit(w, (uint16_t)cp);
        }
    }
    putc_(w, ')');
}

/* `depth` is the indentation level (arrays don't indent their items); `level` counts recursion. */
static void write_value(wr *w, const pc_ed_value *v, size_t depth, size_t level)
{
    size_t k;
    char num[PC_ED_REAL_MAX];
    if (level > PC_ED_MAX_TREE_DEPTH && w->st == PC_OK) {
        w->st = pc_fail(w->err, PC_ERR_LIMIT, "EngineData: nesting too deep");
    }
    if (w->st != PC_OK) {
        return;
    }
    switch (v->kind) {
    case PC_ED_DICT:
        put(w, "<<\n", 3);
        for (k = 0; k < v->n; k++) {
            const pc_ed_entry *e = &v->entries[k];
            indent(w, depth + 1);
            putc_(w, '/');
            put(w, pc_str_c(&e->key), e->key.len);
            if (is_container(&e->value)) {
                putc_(w, '\n');
                indent(w, depth + 1);
            } else {
                putc_(w, ' ');
            }
            write_value(w, &e->value, depth + 1, level + 1);
            putc_(w, '\n');
        }
        indent(w, depth);
        put(w, ">>", 2);
        break;
    case PC_ED_ARRAY:
        if (is_container(v)) {
            put(w, "[\n", 2);
            for (k = 0; k < v->n; k++) {
                indent(w, depth);
                write_value(w, &v->items[k], depth, level + 1);
                putc_(w, '\n');
            }
            indent(w, depth);
            putc_(w, ']');
        } else {
            putc_(w, '[');
            for (k = 0; k < v->n; k++) {
                putc_(w, ' ');
                write_value(w, &v->items[k], depth, level + 1);
            }
            put(w, " ]", 2);
        }
        break;
    case PC_ED_STRING:
        put_string(w, &v->s);
        break;
    case PC_ED_INT: {
        int n = snprintf(num, sizeof num, "%" PRId64, v->i);
        put(w, num, n > 0 ? (size_t)n : 0);
        break;
    }
    case PC_ED_REAL:
        put(w, num, pc_ed_format_real(v->r, num));
        break;
    case PC_ED_BOOL:
        if (v->b) {
            put(w, "true", 4);
        } else {
            put(w, "false", 5);
        }
        break;
    case PC_ED_NAME:
    default:
        putc_(w, '/');
        put(w, pc_str_c(&v->s), v->s.len);
        break;
    }
}

pc_status pc_ed_write(const pc_ed_value *v, pc_buf *out, pc_error *err)
{
    size_t start = out->len;
    wr w;
    w.b = out;
    w.st = PC_OK;
    w.err = err;
    put(&w, "\n\n", 2);
    write_value(&w, v, 0, 0);
    putc_(&w, '\n');
    if (w.st != PC_OK) {
        pc_buf_truncate(out, start);
    }
    return w.st;
}

pc_status pc_ed_write_bare(const pc_ed_value *dict, pc_buf *out, pc_error *err)
{
    size_t start = out->len, k;
    wr w;
    w.b = out;
    w.st = PC_OK;
    w.err = err;
    put(&w, "\n\n", 2);
    for (k = 0; dict != NULL && dict->kind == PC_ED_DICT && k < dict->n; k++) {
        const pc_ed_entry *e = &dict->entries[k];
        putc_(&w, '/');
        put(&w, pc_str_c(&e->key), e->key.len);
        if (is_container(&e->value)) {
            putc_(&w, '\n');
            indent(&w, 1);
        } else {
            putc_(&w, ' ');
        }
        write_value(&w, &e->value, 1, 1);
        putc_(&w, '\n');
    }
    if (w.st != PC_OK) {
        pc_buf_truncate(out, start);
    }
    return w.st;
}
