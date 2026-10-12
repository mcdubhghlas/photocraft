/* serde_json::from_str::<Manifest> (crates/plugins/src/manifest.rs), reproduced as a streaming
 * reader so that errors come out as the reference reports them: the first problem in reading
 * order (a type error before a later syntax error), with serde's message and serde_json's
 * position rules ("at line L column C", where the column counts bytes consumed on that line).
 *
 * What is modelled, from serde_json 1.0.149 and serde 1.0.229:
 *   - deserialize_struct/any/str/number/option/enum and their "fix_position" points: errors made
 *     by visitors (invalid type, missing field, ...) get the position where the innermost such
 *     point finishes; deserialize_enum has none, so its errors take an outer position;
 *   - SeqAccess/MapAccess/end_seq/end_map/ignore_value structural errors and side effects (an
 *     error inside a map still lets end_map consume what follows);
 *   - the recursion limit (128 levels, not counted by ignore_value);
 *   - the derived visitors: Manifest from a map or a sequence (defaults for #[serde(default)]
 *     fields), the internally tagged ParamSpec (buffered as serde's Content, then deserialized
 *     from it), the unit enums Kind and Area, and the `params` object reader.
 * Lexical errors inside a string, number or literal are reported by pc_json_parse over the whole
 * text, which stops at the same place (everything before it is valid JSON), so they carry the
 * B2 wrapper's message (REWRITE_PLAN.md §7, B2). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "plugins/internal.h"

/* ---- serde's buffered values (Content) ---- */

typedef enum ctype { CT_UNIT, CT_BOOL, CT_U64, CT_I64, CT_F64, CT_STR, CT_SEQ, CT_MAP } ctype;

typedef struct content content;
struct content {
    ctype t;
    bool b;
    uint64_t u;
    int64_t i;
    double f;
    char *s; /* CT_STR: slen bytes + NUL */
    size_t slen;
    content *items; /* CT_SEQ: n items; CT_MAP: n key/value pairs as 2n items */
    size_t n, cap;
};

static void content_fini(content *c)
{
    if (c == NULL) {
        return;
    }
    free(c->s);
    if (c->items != NULL) {
        size_t m = c->t == CT_MAP ? 2 * c->n : c->n;
        for (size_t k = 0; k < m; k++) {
            content_fini(&c->items[k]);
        }
        free(c->items);
    }
    memset(c, 0, sizeof *c);
}

/* ---- reader state and errors ---- */

typedef struct de {
    const char *s;
    size_t len, idx;
    int depth; /* serde_json's remaining_depth */
    bool failed, positioned, lexical;
    pc_status code;
    char msg[PC_ERROR_MSG_MAX];
} de;

static void set_msgv(de *d, const char *fmt, va_list ap)
{
    if (vsnprintf(d->msg, sizeof d->msg, fmt, ap) < 0) {
        d->msg[0] = '\0';
    }
}

static void append_pos(de *d, size_t i)
{
    size_t line = 1, start = 0, used = strlen(d->msg);
    if (i > d->len) {
        i = d->len;
    }
    for (size_t k = 0; k < i; k++) {
        if (d->s[k] == '\n') {
            line++;
            start = k + 1;
        }
    }
    if (used < sizeof d->msg) {
        (void)snprintf(d->msg + used, sizeof d->msg - used, " at line %zu column %zu", line, i - start);
    }
}

/* A syntax error at byte offset i (serde_json's Error::syntax). */
static bool e_at(de *d, size_t i, const char *what)
{
    if (!d->failed) {
        d->failed = true;
        d->positioned = true;
        d->code = PC_ERR_FORMAT;
        (void)snprintf(d->msg, sizeof d->msg, "%s", what);
        append_pos(d, i);
    }
    return false;
}

static bool e_here(de *d, const char *what)
{
    return e_at(d, d->idx, what);
}

static bool e_peek(de *d, const char *what)
{
    return e_at(d, d->idx + 1 < d->len ? d->idx + 1 : d->len, what);
}

/* A de::Error::custom (no position yet). */
static bool e_custom(de *d, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static bool e_custom(de *d, const char *fmt, ...)
{
    if (!d->failed) {
        va_list ap;
        va_start(ap, fmt);
        set_msgv(d, fmt, ap);
        va_end(ap);
        d->failed = true;
        d->positioned = false;
        d->code = PC_ERR_FORMAT;
    }
    return false;
}

static bool e_lex(de *d)
{
    if (!d->failed) {
        d->failed = true;
        d->lexical = true;
        d->code = PC_ERR_FORMAT;
    }
    return false;
}

static bool e_nomem(de *d)
{
    if (!d->failed) {
        d->failed = true;
        d->positioned = true;
        d->code = PC_ERR_NOMEM;
        (void)snprintf(d->msg, sizeof d->msg, "out of memory reading a plug-in manifest");
    }
    return false;
}

/* serde_json's fix_position: a custom error gets the current position. */
static void fix(de *d)
{
    if (d->failed && !d->positioned && !d->lexical) {
        d->positioned = true;
        append_pos(d, d->idx);
    }
}

/* ---- lexical layer ---- */

static int peek(const de *d)
{
    return d->idx < d->len ? (unsigned char)d->s[d->idx] : -1;
}

static int ws(de *d)
{
    while (d->idx < d->len) {
        char c = d->s[d->idx];
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r') {
            return (unsigned char)c;
        }
        d->idx++;
    }
    return -1;
}

static bool ident(de *d, const char *rest)
{
    for (; *rest != '\0'; rest++) {
        if (d->idx >= d->len) {
            return e_here(d, "EOF while parsing a value");
        }
        if (d->s[d->idx++] != *rest) {
            return e_here(d, "expected ident");
        }
    }
    return true;
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* A number token at idx ('-' or a digit): its value as serde_json classifies it. */
static bool number(de *d, content *out)
{
    size_t start = d->idx;
    yyjson_doc *doc = NULL;
    yyjson_val *v;
    bool neg = false;
    if (peek(d) == '-') {
        neg = true;
        d->idx++;
    }
    if (peek(d) == '0') {
        d->idx++;
        if (is_digit(peek(d))) {
            return e_lex(d);
        }
    } else if (is_digit(peek(d))) {
        while (is_digit(peek(d))) {
            d->idx++;
        }
    } else {
        return e_lex(d);
    }
    if (peek(d) == '.') {
        d->idx++;
        if (!is_digit(peek(d))) {
            return e_lex(d);
        }
        while (is_digit(peek(d))) {
            d->idx++;
        }
    }
    if (peek(d) == 'e' || peek(d) == 'E') {
        d->idx++;
        if (peek(d) == '+' || peek(d) == '-') {
            d->idx++;
        }
        if (!is_digit(peek(d))) {
            return e_lex(d);
        }
        while (is_digit(peek(d))) {
            d->idx++;
        }
    }
    if (pc_json_parse(d->s + start, d->idx - start, &doc, NULL) != PC_OK) {
        return e_lex(d); /* out of range */
    }
    v = yyjson_doc_get_root(doc);
    memset(out, 0, sizeof *out);
    if (yyjson_is_uint(v)) {
        out->t = CT_U64;
        out->u = yyjson_get_uint(v);
    } else if (yyjson_is_sint(v)) {
        out->t = CT_I64;
        out->i = yyjson_get_sint(v);
        if (neg && out->i == 0) { /* serde_json: "-0" is the float -0.0 */
            out->t = CT_F64;
            out->f = -0.0;
        }
    } else {
        out->t = CT_F64;
        out->f = yyjson_get_real(v);
    }
    yyjson_doc_free(doc);
    return true;
}

/* A string token at idx (the opening quote), decoded. */
static bool string(de *d, content *out)
{
    size_t i = d->idx + 1;
    yyjson_doc *doc = NULL;
    yyjson_val *v;
    for (;;) {
        unsigned char c;
        if (i >= d->len) {
            return e_lex(d);
        }
        c = (unsigned char)d->s[i];
        if (c == '"') {
            break;
        }
        if (c < 0x20) {
            return e_lex(d);
        }
        i += c == '\\' ? 2 : 1;
    }
    if (pc_json_parse(d->s + d->idx, i + 1 - d->idx, &doc, NULL) != PC_OK) {
        return e_lex(d);
    }
    v = yyjson_doc_get_root(doc);
    memset(out, 0, sizeof *out);
    out->t = CT_STR;
    out->slen = yyjson_get_len(v);
    out->s = pc_strndup(yyjson_get_str(v), out->slen);
    yyjson_doc_free(doc);
    if (out->s == NULL) {
        return e_nomem(d);
    }
    d->idx = i + 1;
    return true;
}

/* ---- serde's Unexpected / Expected wording ---- */

static void unexpected(const content *c, char *out, size_t cap)
{
    char num[PC_JSON_NUM_MAX];
    switch (c->t) {
    case CT_UNIT:
        (void)snprintf(out, cap, "null");
        break;
    case CT_BOOL:
        (void)snprintf(out, cap, "boolean `%s`", c->b ? "true" : "false");
        break;
    case CT_U64:
        (void)snprintf(out, cap, "integer `%llu`", (unsigned long long)c->u);
        break;
    case CT_I64:
        (void)snprintf(out, cap, "integer `%lld`", (long long)c->i);
        break;
    case CT_F64:
        (void)pc_json_format_f64(c->f, num);
        (void)snprintf(out, cap, "floating point `%s`", num);
        break;
    case CT_STR: {
        pc_buf q = PC_BUF_INIT;
        pc_plugin_debug_quote(c->s, c->slen, &q);
        (void)snprintf(out, cap, "string %s", pc_buf_cstr(&q));
        pc_buf_fini(&q);
        break;
    }
    case CT_SEQ:
        (void)snprintf(out, cap, "sequence");
        break;
    case CT_MAP:
        (void)snprintf(out, cap, "map");
        break;
    }
}

static bool e_type(de *d, const content *c, const char *exp)
{
    char u[PC_ERROR_MSG_MAX];
    unexpected(c, u, sizeof u);
    return e_custom(d, "invalid type: %s, expected %s", u, exp);
}

/* serde_json's peek_invalid_type: consumes a scalar to describe it, then fixes the position. */
static bool peek_invalid_type(de *d, const char *exp)
{
    content c;
    memset(&c, 0, sizeof c);
    switch (peek(d)) {
    case 'n':
        d->idx++;
        if (!ident(d, "ull")) {
            return false;
        }
        c.t = CT_UNIT;
        break;
    case 't':
        d->idx++;
        if (!ident(d, "rue")) {
            return false;
        }
        c.t = CT_BOOL;
        c.b = true;
        break;
    case 'f':
        d->idx++;
        if (!ident(d, "alse")) {
            return false;
        }
        c.t = CT_BOOL;
        break;
    case '"':
        if (!string(d, &c)) {
            return false;
        }
        break;
    case '[':
        c.t = CT_SEQ;
        break;
    case '{':
        c.t = CT_MAP;
        break;
    default:
        if (peek(d) == '-' || is_digit(peek(d))) {
            if (!number(d, &c)) {
                return false;
            }
            break;
        }
        return e_peek(d, "expected value");
    }
    (void)e_type(d, &c, exp);
    content_fini(&c);
    fix(d);
    return false;
}

/* ---- structural layer (SeqAccess / MapAccess) ---- */

/* has_next_element: 1 = an element follows, 0 = the end (']' not consumed), -1 = error. */
static int seq_next(de *d, bool *first)
{
    int c = ws(d);
    if (c < 0) {
        return e_peek(d, "EOF while parsing a list") ? 0 : -1;
    }
    if (c == ']') {
        return 0;
    }
    if (*first) {
        *first = false;
        return 1;
    }
    if (c == ',') {
        d->idx++;
        c = ws(d);
        if (c == ']') {
            return e_peek(d, "trailing comma") ? 0 : -1;
        }
        if (c < 0) {
            return e_peek(d, "EOF while parsing a value") ? 0 : -1;
        }
        return 1;
    }
    return e_peek(d, "expected `,` or `]`") ? 0 : -1;
}

/* has_next_key: 1 = a key string follows (at idx), 0 = the end, -1 = error. */
static int map_next(de *d, bool *first)
{
    int c = ws(d);
    if (c < 0) {
        return e_peek(d, "EOF while parsing an object") ? 0 : -1;
    }
    if (c == '}') {
        return 0;
    }
    if (*first) {
        *first = false;
        if (c == '"') {
            return 1;
        }
        return e_peek(d, "key must be a string") ? 0 : -1;
    }
    if (c == ',') {
        d->idx++;
        c = ws(d);
        if (c == '"') {
            return 1;
        }
        if (c == '}') {
            return e_peek(d, "trailing comma") ? 0 : -1;
        }
        if (c < 0) {
            return e_peek(d, "EOF while parsing a value") ? 0 : -1;
        }
        return e_peek(d, "key must be a string") ? 0 : -1;
    }
    return e_peek(d, "expected `,` or `}`") ? 0 : -1;
}

static bool colon(de *d)
{
    int c = ws(d);
    if (c == ':') {
        d->idx++;
        return true;
    }
    return e_peek(d, c < 0 ? "EOF while parsing an object" : "expected `:`");
}

/* end_seq / end_map: run after the visitor even when it failed (their effect on idx decides
 * the position a pending custom error gets); their own errors count only if nothing failed. */
static void end_seq(de *d)
{
    int c = ws(d);
    if (c == ']') {
        d->idx++;
    } else if (c == ',') {
        d->idx++;
        c = ws(d);
        (void)e_peek(d, c == ']' ? "trailing comma" : "trailing characters");
    } else if (c < 0) {
        (void)e_peek(d, "EOF while parsing a list");
    } else {
        (void)e_peek(d, "trailing characters");
    }
}

static void end_map(de *d)
{
    int c = ws(d);
    if (c == '}') {
        d->idx++;
    } else if (c == ',') {
        (void)e_peek(d, "trailing comma");
    } else if (c < 0) {
        (void)e_peek(d, "EOF while parsing an object");
    } else {
        (void)e_peek(d, "trailing characters");
    }
}

static bool enter(de *d)
{
    if (--d->depth == 0) {
        return e_peek(d, "recursion limit exceeded");
    }
    d->idx++;
    return true;
}

/* serde_json's ignore_value (IgnoredAny): structural checks only, no recursion limit. */
static bool ignore_value(de *d)
{
    char *stack = NULL;
    size_t n = 0, cap = 0;
    bool ok = false;
    for (;;) {
        int c = ws(d);
        bool accept_comma;
        char frame;
        content tmp;
        if (c < 0) {
            (void)e_peek(d, "EOF while parsing a value");
            goto done;
        }
        switch (c) {
        case 'n':
            d->idx++;
            if (!ident(d, "ull")) {
                goto done;
            }
            frame = 0;
            break;
        case 't':
            d->idx++;
            if (!ident(d, "rue")) {
                goto done;
            }
            frame = 0;
            break;
        case 'f':
            d->idx++;
            if (!ident(d, "alse")) {
                goto done;
            }
            frame = 0;
            break;
        case '"':
            if (!string(d, &tmp)) {
                goto done;
            }
            content_fini(&tmp);
            frame = 0;
            break;
        case '[':
        case '{': {
            char *grown = pc_grow_array(stack, &cap, n + 1, 1);
            if (grown == NULL) {
                (void)e_nomem(d);
                goto done;
            }
            stack = grown;
            stack[n++] = (char)c;
            d->idx++;
            frame = (char)c;
            break;
        }
        default:
            if (c == '-' || is_digit(c)) {
                if (!number(d, &tmp)) {
                    goto done;
                }
                frame = 0;
                break;
            }
            (void)e_peek(d, "expected value");
            goto done;
        }
        if (n == 0) {
            ok = true;
            goto done;
        }
        accept_comma = frame == 0;
        frame = stack[n - 1];
        for (;;) {
            c = ws(d);
            if (c == ',' && accept_comma) {
                d->idx++;
                break;
            }
            if (!((c == ']' && frame == '[') || (c == '}' && frame == '{'))) {
                if (c < 0) {
                    (void)e_peek(d,
                                 frame == '[' ? "EOF while parsing a list" : "EOF while parsing an object");
                    goto done;
                }
                if (accept_comma) {
                    (void)e_peek(d, frame == '[' ? "expected `,` or `]`" : "expected `,` or `}`");
                    goto done;
                }
                break;
            }
            d->idx++;
            if (--n == 0) {
                ok = true;
                goto done;
            }
            frame = stack[n - 1];
            accept_comma = true;
        }
        if (frame == '{') {
            c = ws(d);
            if (c != '"') {
                (void)e_peek(d, c < 0 ? "EOF while parsing an object" : "key must be a string");
                goto done;
            }
            if (!string(d, &tmp)) {
                goto done;
            }
            content_fini(&tmp);
            c = ws(d);
            if (c != ':') {
                (void)e_peek(d, c < 0 ? "EOF while parsing an object" : "expected `:`");
                goto done;
            }
            d->idx++;
        }
    }
done:
    free(stack);
    return ok;
}

/* ---- deserialize_any into Content (ContentVisitor) ---- */

static bool push_item(de *d, content *c, size_t extra)
{
    size_t need = (c->t == CT_MAP ? 2 * c->n : c->n) + extra;
    content *grown = pc_grow_array(c->items, &c->cap, need, sizeof *grown);
    if (grown == NULL) {
        return e_nomem(d);
    }
    c->items = grown;
    return true;
}

static bool read_any(de *d, content *out);

static bool read_seq_rest(de *d, content *out, bool first)
{
    for (;;) {
        int has = seq_next(d, &first);
        if (has <= 0) {
            return has == 0 && !d->failed;
        }
        if (!push_item(d, out, 1)) {
            return false;
        }
        memset(&out->items[out->n], 0, sizeof out->items[0]);
        if (!read_any(d, &out->items[out->n])) {
            content_fini(&out->items[out->n]);
            return false;
        }
        out->n++;
    }
}

static bool read_any(de *d, content *out)
{
    int c = ws(d);
    bool ok;
    memset(out, 0, sizeof *out);
    if (c < 0) {
        return e_peek(d, "EOF while parsing a value");
    }
    switch (c) {
    case 'n':
        d->idx++;
        ok = ident(d, "ull");
        out->t = CT_UNIT;
        break;
    case 't':
        d->idx++;
        ok = ident(d, "rue");
        out->t = CT_BOOL;
        out->b = true;
        break;
    case 'f':
        d->idx++;
        ok = ident(d, "alse");
        out->t = CT_BOOL;
        break;
    case '"':
        ok = string(d, out);
        break;
    case '[':
        if (!enter(d)) {
            return false;
        }
        out->t = CT_SEQ;
        ok = read_seq_rest(d, out, true);
        d->depth++;
        end_seq(d);
        ok = ok && !d->failed;
        break;
    case '{': {
        bool first = true;
        if (!enter(d)) {
            return false;
        }
        out->t = CT_MAP;
        for (;;) {
            int has = map_next(d, &first);
            content *kv;
            if (has <= 0) {
                ok = has == 0 && !d->failed;
                break;
            }
            if (!push_item(d, out, 2)) {
                ok = false;
                break;
            }
            kv = &out->items[2 * out->n];
            memset(kv, 0, 2 * sizeof *kv);
            if (!string(d, &kv[0])) {
                content_fini(&kv[0]);
                ok = false;
                break;
            }
            if (!colon(d) || !read_any(d, &kv[1])) {
                content_fini(&kv[0]);
                content_fini(&kv[1]);
                ok = false;
                break;
            }
            out->n++;
        }
        d->depth++;
        end_map(d);
        ok = ok && !d->failed;
        break;
    }
    default:
        if (c == '-' || is_digit(c)) {
            ok = number(d, out);
            break;
        }
        return e_peek(d, "expected value");
    }
    fix(d);
    return ok;
}

/* ---- typed fields ---- */

/* deserialize_string into a String. */
static bool read_string(de *d, pc_str *out)
{
    content c;
    int p = ws(d);
    memset(&c, 0, sizeof c);
    if (p < 0) {
        return e_peek(d, "EOF while parsing a value");
    }
    if (p != '"') {
        return peek_invalid_type(d, "a string");
    }
    if (!string(d, &c)) {
        return false;
    }
    pc_str_fini(out);
    *out = pc_str_adopt(c.s, c.slen);
    return true;
}

/* deserialize_identifier for an enum's variants: a string naming one of `names`. */
static bool read_variant(de *d, const char *const *names, size_t n, const char *one_of, size_t *which)
{
    content c;
    int p = ws(d);
    if (p < 0) {
        return e_peek(d, "EOF while parsing a value");
    }
    if (p != '"') {
        return peek_invalid_type(d, "variant identifier");
    }
    if (!string(d, &c)) {
        return false;
    }
    for (size_t k = 0; k < n; k++) {
        if (c.slen == strlen(names[k]) && memcmp(c.s, names[k], c.slen) == 0) {
            *which = k;
            content_fini(&c);
            return true;
        }
    }
    (void)e_custom(d, "unknown variant `%s`, expected %s", c.s, one_of);
    content_fini(&c);
    fix(d);
    return false;
}

/* A unit-only enum (Kind, Area) through deserialize_enum: "name" or {"name": null}. */
static bool read_unit_enum(de *d, const char *const *names, size_t n, const char *one_of, size_t *which)
{
    int c = ws(d);
    if (c == '"') {
        return read_variant(d, names, n, one_of, which);
    }
    if (c == '{') {
        int p;
        if (!enter(d)) {
            return false;
        }
        if (!read_variant(d, names, n, one_of, which) || !colon(d)) {
            d->depth++;
            return false;
        }
        p = ws(d);
        if (p < 0) {
            d->depth++;
            return e_peek(d, "EOF while parsing a value");
        }
        if (p != 'n') {
            d->depth++;
            return peek_invalid_type(d, "unit");
        }
        d->idx++;
        if (!ident(d, "ull")) {
            d->depth++;
            return false;
        }
        d->depth++;
        c = ws(d);
        if (c == '}') {
            d->idx++;
            return true;
        }
        return e_here(d, c < 0 ? "EOF while parsing an object" : "expected value");
    }
    return e_peek(d, c < 0 ? "EOF while parsing a value" : "expected value");
}

/* deserialize_u32 (via deserialize_number). */
static bool read_u32(de *d, uint32_t *out)
{
    content c;
    int p = ws(d);
    char u[PC_ERROR_MSG_MAX];
    if (p < 0) {
        return e_peek(d, "EOF while parsing a value");
    }
    if (p != '-' && !is_digit(p)) {
        return peek_invalid_type(d, "u32");
    }
    if (!number(d, &c)) {
        return false;
    }
    if (c.t == CT_U64 && c.u <= UINT32_MAX) {
        *out = (uint32_t)c.u;
        return true;
    }
    unexpected(&c, u, sizeof u);
    if (c.t == CT_F64) {
        (void)e_custom(d, "invalid type: %s, expected u32", u);
    } else {
        (void)e_custom(d, "invalid value: %s, expected u32", u);
    }
    fix(d);
    return false;
}

/* ---- ParamSpec from Content (serde's ContentDeserializer; errors keep no position) ---- */

static bool c_f64(de *d, const content *c, double *out)
{
    switch (c->t) {
    case CT_U64:
        *out = (double)c->u;
        return true;
    case CT_I64:
        *out = (double)c->i;
        return true;
    case CT_F64:
        *out = c->f;
        return true;
    default:
        return e_type(d, c, "f64");
    }
}

static bool c_i64(de *d, const content *c, int64_t *out)
{
    char u[PC_ERROR_MSG_MAX];
    switch (c->t) {
    case CT_U64:
        if (c->u <= (uint64_t)INT64_MAX) {
            *out = (int64_t)c->u;
            return true;
        }
        unexpected(c, u, sizeof u);
        return e_custom(d, "invalid value: %s, expected i64", u);
    case CT_I64:
        *out = c->i;
        return true;
    default:
        return e_type(d, c, "i64");
    }
}

static bool c_bool(de *d, const content *c, bool *out)
{
    if (c->t != CT_BOOL) {
        return e_type(d, c, "a boolean");
    }
    *out = c->b;
    return true;
}

static bool c_string(de *d, const content *c, pc_str *out)
{
    if (c->t != CT_STR) {
        return e_type(d, c, "a string");
    }
    pc_str_fini(out);
    if (pc_str_set(out, c->s, c->slen, NULL) != PC_OK) {
        return e_nomem(d);
    }
    return true;
}

static bool c_options(de *d, const content *c, pc_param_spec *ps)
{
    pc_str *opts;
    if (c->t != CT_SEQ) {
        return e_type(d, c, "a sequence");
    }
    opts = pc_calloc_array(c->n > 0 ? c->n : 1, sizeof *opts);
    if (opts == NULL) {
        return e_nomem(d);
    }
    for (size_t k = 0; k < c->n; k++) {
        if (!c_string(d, &c->items[k], &opts[k])) {
            for (size_t j = 0; j <= k; j++) {
                pc_str_fini(&opts[j]);
            }
            free(opts);
            return false;
        }
    }
    for (size_t j = 0; j < ps->n_options; j++) {
        pc_str_fini(&ps->options[j]);
    }
    free(ps->options);
    ps->options = opts;
    ps->n_options = c->n;
    return true;
}

static bool c_option_string(de *d, const content *c, pc_param_spec *ps)
{
    if (c->t == CT_UNIT) {
        pc_str_fini(&ps->cdef);
        ps->has_cdef = false;
        return true;
    }
    if (!c_string(d, c, &ps->cdef)) {
        return false;
    }
    ps->has_cdef = true;
    return true;
}

typedef struct variant_def {
    const char *name;      /* struct variant ParamSpec::<name> */
    const char *fields[3]; /* declaration order */
    size_t nfields;
    unsigned required; /* bit k: field k has no default */
} variant_def;

static const variant_def VARIANTS[4] = {
    {"Number", {"min", "max", "default"}, 3, 7u},
    {"Int", {"min", "max", "default"}, 3, 0u},
    {"Bool", {"default", NULL, NULL}, 1, 0u},
    {"Choice", {"options", "default", NULL}, 2, 1u},
};

/* One field value of variant v (field k) into ps. */
static bool c_field(de *d, pc_param_type v, size_t k, const content *c, pc_param_spec *ps)
{
    switch (v) {
    case PC_PARAM_NUMBER:
        return c_f64(d, c, k == 0 ? &ps->min : k == 1 ? &ps->max : &ps->def);
    case PC_PARAM_INT:
        return c_i64(d, c, k == 0 ? &ps->imin : k == 1 ? &ps->imax : &ps->idef);
    case PC_PARAM_BOOL:
        return c_bool(d, c, &ps->bdef);
    case PC_PARAM_CHOICE:
        return k == 0 ? c_options(d, c, ps) : c_option_string(d, c, ps);
    }
    return false;
}

static bool param_from_content(de *d, pc_param_type v, const content *body, pc_param_spec *ps)
{
    const variant_def *vd = &VARIANTS[v];
    unsigned seen = 0;
    ps->type = v;
    ps->imin = -1000000;
    ps->imax = 1000000;
    if (body->t == CT_SEQ) {
        size_t k;
        for (k = 0; k < vd->nfields; k++) {
            if (k >= body->n) {
                if ((vd->required >> k) & 1u) {
                    return e_custom(
                        d, "invalid length %zu, expected struct variant ParamSpec::%s with %zu element%s", k,
                        vd->name, vd->nfields, vd->nfields == 1 ? "" : "s");
                }
                continue;
            }
            if (!c_field(d, v, k, &body->items[k], ps)) {
                return false;
            }
        }
        if (body->n > vd->nfields) {
            return e_custom(d, "invalid length %zu, expected %zu element%s in sequence", body->n, vd->nfields,
                            vd->nfields == 1 ? "" : "s");
        }
        return true;
    }
    if (body->t != CT_MAP) {
        char exp[64];
        (void)snprintf(exp, sizeof exp, "struct variant ParamSpec::%s", vd->name);
        return e_type(d, body, exp);
    }
    for (size_t e = 0; e < body->n; e++) {
        const content *key = &body->items[2 * e];
        size_t k;
        for (k = 0; k < vd->nfields; k++) {
            if (key->t == CT_STR && key->slen == strlen(vd->fields[k]) &&
                memcmp(key->s, vd->fields[k], key->slen) == 0) {
                break;
            }
        }
        if (k == vd->nfields) {
            continue; /* unknown fields are ignored */
        }
        if ((seen >> k) & 1u) {
            return e_custom(d, "duplicate field `%s`", vd->fields[k]);
        }
        seen |= 1u << k;
        if (!c_field(d, v, k, &body->items[2 * e + 1], ps)) {
            return false;
        }
    }
    for (size_t k = 0; k < vd->nfields; k++) {
        if (((vd->required >> k) & 1u) && !((seen >> k) & 1u)) {
            return e_custom(d, "missing field `%s`", vd->fields[k]);
        }
    }
    return true;
}

static const char *const PARAM_TAGS[4] = {"number", "int", "bool", "choice"};
#define PARAM_ONE_OF "one of `number`, `int`, `bool`, `choice`"

/* ParamSpec::deserialize: TaggedContentVisitor through deserialize_any, then the variant. */
static bool read_param_spec(de *d, pc_param_spec *ps)
{
    int c = ws(d);
    size_t tag = 0;
    bool have_tag = false, ok = true;
    content body;
    memset(&body, 0, sizeof body);
    if (c < 0) {
        return e_peek(d, "EOF while parsing a value");
    }
    if (c == '{') {
        bool first = true;
        if (!enter(d)) {
            return false;
        }
        body.t = CT_MAP;
        for (;;) {
            int has = map_next(d, &first);
            content key;
            if (has <= 0) {
                ok = has == 0 && !d->failed;
                break;
            }
            if (!string(d, &key)) {
                ok = false;
                break;
            }
            if (key.slen == 4 && memcmp(key.s, "type", 4) == 0) {
                content_fini(&key);
                if (have_tag) {
                    ok = e_custom(d, "duplicate field `type`");
                    break;
                }
                if (!colon(d) || !read_variant(d, PARAM_TAGS, 4, PARAM_ONE_OF, &tag)) {
                    ok = false;
                    break;
                }
                have_tag = true;
                continue;
            }
            if (!push_item(d, &body, 2)) {
                content_fini(&key);
                ok = false;
                break;
            }
            body.items[2 * body.n] = key;
            memset(&body.items[2 * body.n + 1], 0, sizeof body.items[0]);
            if (!colon(d) || !read_any(d, &body.items[2 * body.n + 1])) {
                content_fini(&body.items[2 * body.n]);
                content_fini(&body.items[2 * body.n + 1]);
                ok = false;
                break;
            }
            body.n++;
        }
        if (ok && !have_tag) {
            ok = e_custom(d, "missing field `type`");
        }
        d->depth++;
        end_map(d);
        ok = ok && !d->failed;
    } else if (c == '[') {
        bool first = true;
        int has;
        if (!enter(d)) {
            return false;
        }
        has = seq_next(d, &first);
        if (has == 1) {
            if (read_variant(d, PARAM_TAGS, 4, PARAM_ONE_OF, &tag)) {
                body.t = CT_SEQ;
                ok = read_seq_rest(d, &body, false);
            } else {
                ok = false;
            }
        } else if (has == 0 && !d->failed) {
            ok = e_custom(d, "missing field `type`");
        } else {
            ok = false;
        }
        d->depth++;
        end_seq(d);
        ok = ok && !d->failed;
    } else {
        content s;
        memset(&s, 0, sizeof s);
        switch (c) {
        case 'n':
            d->idx++;
            ok = ident(d, "ull");
            s.t = CT_UNIT;
            break;
        case 't':
            d->idx++;
            ok = ident(d, "rue");
            s.t = CT_BOOL;
            s.b = true;
            break;
        case 'f':
            d->idx++;
            ok = ident(d, "alse");
            s.t = CT_BOOL;
            break;
        case '"':
            ok = string(d, &s);
            break;
        default:
            if (c == '-' || is_digit(c)) {
                ok = number(d, &s);
                break;
            }
            return e_peek(d, "expected value");
        }
        if (ok) {
            ok = e_type(d, &s, "internally tagged enum ParamSpec");
        }
        content_fini(&s);
    }
    fix(d);
    if (ok) {
        ok = param_from_content(d, (pc_param_type)tag, &body, ps);
    }
    content_fini(&body);
    return ok;
}

static void param_fini(pc_param_spec *ps)
{
    pc_str_fini(&ps->name);
    for (size_t k = 0; k < ps->n_options; k++) {
        pc_str_fini(&ps->options[k]);
    }
    free(ps->options);
    pc_str_fini(&ps->cdef);
    memset(ps, 0, sizeof *ps);
}

/* The `params` field: ordered_params through deserialize_any. */
static bool read_params(de *d, pc_plugin_manifest *m)
{
    int c = ws(d);
    bool ok = true, first = true;
    if (c < 0) {
        return e_peek(d, "EOF while parsing a value");
    }
    if (c == 'n') {
        d->idx++;
        if (!ident(d, "ull")) {
            return false;
        }
        return true; /* visit_unit: no parameters */
    }
    if (c == '[') {
        if (!enter(d)) {
            return false;
        }
        (void)e_custom(d, "invalid type: sequence, expected an object of parameter specs");
        d->depth++;
        end_seq(d);
        fix(d);
        return false;
    }
    if (c != '{') {
        content s;
        memset(&s, 0, sizeof s);
        if (c == 't' || c == 'f') {
            d->idx++;
            ok = ident(d, c == 't' ? "rue" : "alse");
            s.t = CT_BOOL;
            s.b = c == 't';
        } else if (c == '"') {
            ok = string(d, &s);
        } else if (c == '-' || is_digit(c)) {
            ok = number(d, &s);
        } else {
            return e_peek(d, "expected value");
        }
        if (ok) {
            (void)e_type(d, &s, "an object of parameter specs");
        }
        content_fini(&s);
        fix(d);
        return false;
    }
    if (!enter(d)) {
        return false;
    }
    for (;;) {
        int has = map_next(d, &first);
        size_t cap = m->n_params;
        pc_param_spec ps;
        pc_param_spec *grown;
        if (has <= 0) {
            ok = has == 0 && !d->failed;
            break;
        }
        memset(&ps, 0, sizeof ps);
        if (!read_string(d, &ps.name) || !colon(d) || !read_param_spec(d, &ps)) {
            param_fini(&ps);
            ok = false;
            break;
        }
        if (m->n_params >= PC_PLUGIN_MAX_PARAMS) {
            param_fini(&ps);
            ok = e_custom(d, "too many parameters (limit %d)", PC_PLUGIN_MAX_PARAMS);
            break;
        }
        grown = pc_grow_array(m->params, &cap, m->n_params + 1, sizeof *grown);
        if (grown == NULL) {
            param_fini(&ps);
            ok = e_nomem(d);
            break;
        }
        m->params = grown;
        m->params[m->n_params++] = ps;
    }
    d->depth++;
    end_map(d);
    fix(d);
    return ok && !d->failed;
}

/* ---- the Manifest struct ---- */

enum { F_ID, F_NAME, F_VERSION, F_KIND, F_DESCRIPTION, F_AUTHOR, F_PARAMS, F_OVERLAP, F_AREA, F_COUNT };
static const char *const FIELDS[F_COUNT] = {"id",     "name",   "version", "kind", "description",
                                            "author", "params", "overlap", "area"};
static const char *const KINDS[1] = {"filter"};
static const char *const AREAS[2] = {"content", "canvas"};

static bool read_field(de *d, int f, pc_plugin_manifest *m)
{
    size_t which = 0;
    switch (f) {
    case F_ID:
        return read_string(d, &m->id);
    case F_NAME:
        return read_string(d, &m->name);
    case F_VERSION:
        return read_string(d, &m->version);
    case F_DESCRIPTION:
        return read_string(d, &m->description);
    case F_AUTHOR:
        return read_string(d, &m->author);
    case F_KIND:
        if (!read_unit_enum(d, KINDS, 1, "`filter`", &which)) {
            return false;
        }
        m->kind = PC_PLUGIN_KIND_FILTER;
        return true;
    case F_AREA:
        if (!read_unit_enum(d, AREAS, 2, "`content` or `canvas`", &which)) {
            return false;
        }
        m->area = which == 1 ? PC_PLUGIN_AREA_CANVAS : PC_PLUGIN_AREA_CONTENT;
        return true;
    case F_PARAMS:
        return read_params(d, m);
    case F_OVERLAP:
        return read_u32(d, &m->overlap);
    default:
        return false;
    }
}

static bool read_manifest_map(de *d, pc_plugin_manifest *m)
{
    bool seen[F_COUNT] = {false};
    bool first = true;
    for (;;) {
        int has = map_next(d, &first);
        content key;
        int f;
        if (has <= 0) {
            if (has < 0 || d->failed) {
                return false;
            }
            break;
        }
        if (!string(d, &key)) {
            return false;
        }
        for (f = 0; f < F_COUNT; f++) {
            if (key.slen == strlen(FIELDS[f]) && memcmp(key.s, FIELDS[f], key.slen) == 0) {
                break;
            }
        }
        content_fini(&key);
        if (f == F_COUNT) {
            if (!colon(d) || !ignore_value(d)) {
                return false;
            }
            continue;
        }
        if (seen[f]) {
            return e_custom(d, "duplicate field `%s`", FIELDS[f]);
        }
        seen[f] = true;
        if (!colon(d) || !read_field(d, f, m)) {
            return false;
        }
    }
    if (!seen[F_ID]) {
        return e_custom(d, "missing field `id`");
    }
    if (!seen[F_NAME]) {
        return e_custom(d, "missing field `name`");
    }
    if (!seen[F_KIND]) {
        return e_custom(d, "missing field `kind`");
    }
    return true;
}

static bool read_manifest_seq(de *d, pc_plugin_manifest *m)
{
    bool first = true;
    for (int f = 0; f < F_COUNT; f++) {
        int has = seq_next(d, &first);
        if (has < 0 || d->failed) {
            return false;
        }
        if (has == 0) {
            if (f == F_ID || f == F_NAME || f == F_KIND) {
                return e_custom(d, "invalid length %d, expected struct Manifest with 9 elements", f);
            }
            continue;
        }
        if (!read_field(d, f, m)) {
            return false;
        }
    }
    return true;
}

pc_status pc_plugin_manifest_deserialize(const char *text, size_t len, pc_plugin_manifest *out, pc_error *err)
{
    de d;
    pc_plugin_manifest m;
    int c;
    bool ok;
    memset(&d, 0, sizeof d);
    memset(&m, 0, sizeof m);
    d.s = text;
    d.len = len;
    d.depth = 128;
    c = ws(&d);
    if (c < 0) {
        ok = e_peek(&d, "EOF while parsing a value");
    } else if (c == '{' || c == '[') {
        if (!enter(&d)) {
            ok = false;
        } else {
            ok = c == '{' ? read_manifest_map(&d, &m) : read_manifest_seq(&d, &m);
            d.depth++;
            if (c == '{') {
                end_map(&d);
            } else {
                end_seq(&d);
            }
            ok = ok && !d.failed;
        }
        fix(&d);
    } else {
        ok = peek_invalid_type(&d, "struct Manifest");
    }
    if (ok && ws(&d) >= 0) {
        ok = e_peek(&d, "trailing characters");
    }
    if (!ok || d.failed) {
        pc_plugin_manifest_fini(&m);
        if (d.code == PC_ERR_NOMEM) {
            return pc_fail(err, PC_ERR_NOMEM, "%s", d.msg);
        }
        if (d.lexical) {
            /* Everything before this point is valid JSON, so the full parse stops here too. */
            pc_error je;
            yyjson_doc *doc = NULL;
            pc_error_clear(&je);
            if (pc_json_parse(text, len, &doc, &je) == PC_OK) {
                yyjson_doc_free(doc);
                return pc_fail(err, PC_ERR_FORMAT, "invalid JSON");
            }
            return pc_fail(err, PC_ERR_FORMAT, "%s", je.msg);
        }
        if (!d.positioned) {
            append_pos(&d, d.idx);
        }
        return pc_fail(err, PC_ERR_FORMAT, "%s", d.msg);
    }
    *out = m;
    return PC_OK;
}
