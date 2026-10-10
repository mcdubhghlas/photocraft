/* Named type styles (port of crates/doc/src/text_styles.rs). Attribute sets are serde_json
 * Values; applying one goes through the styles' JSON forms exactly as the reference does
 * (to_value, insert, from_value), so type errors carry serde's messages. */
#include "doc/text_styles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "doc/json.h"

#define REF_KEY "style_sheet"

/* ---- attribute sets ---- */

static yyjson_mut_val *attrs_obj(const pc_style_attrs *a)
{
    yyjson_mut_val *r = a != NULL ? pc_json_value_root(a) : NULL;
    return r != NULL && yyjson_mut_is_obj(r) ? r : NULL;
}

bool pc_style_attrs_is_empty(const pc_style_attrs *a)
{
    return pc_style_attrs_len(a) == 0;
}

size_t pc_style_attrs_len(const pc_style_attrs *a)
{
    yyjson_mut_val *o = attrs_obj(a);
    return o != NULL ? yyjson_mut_obj_size(o) : 0;
}

static bool key_is(yyjson_mut_val *k, const char *key, size_t len)
{
    return yyjson_mut_get_len(k) == len && (len == 0 || memcmp(yyjson_mut_get_str(k), key, len) == 0);
}

static yyjson_mut_val *obj_get(yyjson_mut_val *o, const char *key, size_t len)
{
    yyjson_mut_obj_iter it;
    yyjson_mut_val *k;
    yyjson_mut_val *found = NULL;
    if (o == NULL || !yyjson_mut_obj_iter_init(o, &it)) {
        return NULL;
    }
    while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        if (key_is(k, key, len)) {
            found = yyjson_mut_obj_iter_get_val(k);
        }
    }
    return found;
}

yyjson_mut_val *pc_style_attrs_get(const pc_style_attrs *a, const char *key)
{
    return key != NULL ? obj_get(attrs_obj(a), key, strlen(key)) : NULL;
}

/* Rust's String ordering: bytewise, a prefix first. */
static int key_cmp(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    int c = n == 0 ? 0 : memcmp(a, b, n);
    if (c != 0) {
        return c;
    }
    return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

pc_status pc_style_attrs_insert(pc_style_attrs *a, const char *key, const yyjson_mut_val *v, pc_error *err)
{
    yyjson_mut_doc *d;
    yyjson_mut_val *obj;
    yyjson_mut_val *old = attrs_obj(a);
    size_t klen;
    bool placed = false;
    if (a == NULL || key == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no attribute set or key");
    }
    klen = strlen(key);
    d = yyjson_mut_doc_new(NULL);
    obj = d != NULL ? yyjson_mut_obj(d) : NULL;
    if (obj == NULL) {
        goto nomem;
    }
    yyjson_mut_doc_set_root(d, obj);
    /* Rebuild in key order with the new entry in its place (the set stays a sorted map). */
    if (old != NULL) {
        yyjson_mut_obj_iter it;
        yyjson_mut_val *k;
        (void)yyjson_mut_obj_iter_init(old, &it);
        while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
            int c = key_cmp(yyjson_mut_get_str(k), yyjson_mut_get_len(k), key, klen);
            if (c == 0) {
                continue;
            }
            if (c > 0 && !placed) {
                if (!yyjson_mut_obj_add(obj, yyjson_mut_strncpy(d, key, klen),
                                        pc_json_normalized_copy(d, v))) {
                    goto nomem;
                }
                placed = true;
            }
            if (!yyjson_mut_obj_add(obj, yyjson_mut_strncpy(d, yyjson_mut_get_str(k), yyjson_mut_get_len(k)),
                                    pc_json_normalized_copy(d, yyjson_mut_obj_iter_get_val(k)))) {
                goto nomem;
            }
        }
    }
    if (!placed &&
        !yyjson_mut_obj_add(obj, yyjson_mut_strncpy(d, key, klen), pc_json_normalized_copy(d, v))) {
        goto nomem;
    }
    pc_json_value_fini(a);
    a->doc = d;
    return PC_OK;
nomem:
    yyjson_mut_doc_free(d);
    return pc_fail(err, PC_ERR_NOMEM, "out of memory inserting a style attribute");
}

static bool attrs_eq(const pc_style_attrs *a, const pc_style_attrs *b)
{
    yyjson_mut_val *x = attrs_obj(a);
    yyjson_mut_val *y = attrs_obj(b);
    if (pc_style_attrs_len(a) == 0 || pc_style_attrs_len(b) == 0) {
        return pc_style_attrs_len(a) == pc_style_attrs_len(b);
    }
    return pc_json_mut_equal(x, y);
}

static pc_status attrs_clone(const pc_style_attrs *a, pc_style_attrs *out, pc_error *err)
{
    return pc_json_value_clone(a, out, err);
}

/* ---- style definitions ---- */

void pc_character_style_def_fini(pc_character_style_def *d)
{
    if (d == NULL) {
        return;
    }
    pc_str_fini(&d->name);
    pc_json_value_fini(&d->attrs);
    memset(d, 0, sizeof *d);
}

pc_status pc_character_style_def_clone(const pc_character_style_def *d, pc_character_style_def *out,
                                       pc_error *err)
{
    pc_character_style_def c;
    memset(&c, 0, sizeof c);
    c.id = d->id;
    if (pc_str_clone(&d->name, &c.name, err) != PC_OK || attrs_clone(&d->attrs, &c.attrs, err) != PC_OK) {
        pc_character_style_def_fini(&c);
        return PC_ERR_NOMEM;
    }
    *out = c;
    return PC_OK;
}

bool pc_character_style_def_eq(const pc_character_style_def *a, const pc_character_style_def *b)
{
    return a->id == b->id && pc_str_eq(&a->name, &b->name) && attrs_eq(&a->attrs, &b->attrs);
}

void pc_paragraph_style_def_fini(pc_paragraph_style_def *d)
{
    if (d == NULL) {
        return;
    }
    pc_str_fini(&d->name);
    pc_json_value_fini(&d->para_attrs);
    pc_json_value_fini(&d->char_attrs);
    memset(d, 0, sizeof *d);
}

pc_status pc_paragraph_style_def_clone(const pc_paragraph_style_def *d, pc_paragraph_style_def *out,
                                       pc_error *err)
{
    pc_paragraph_style_def c;
    memset(&c, 0, sizeof c);
    c.id = d->id;
    if (pc_str_clone(&d->name, &c.name, err) != PC_OK ||
        attrs_clone(&d->para_attrs, &c.para_attrs, err) != PC_OK ||
        attrs_clone(&d->char_attrs, &c.char_attrs, err) != PC_OK) {
        pc_paragraph_style_def_fini(&c);
        return PC_ERR_NOMEM;
    }
    *out = c;
    return PC_OK;
}

bool pc_paragraph_style_def_eq(const pc_paragraph_style_def *a, const pc_paragraph_style_def *b)
{
    return a->id == b->id && pc_str_eq(&a->name, &b->name) && attrs_eq(&a->para_attrs, &b->para_attrs) &&
           attrs_eq(&a->char_attrs, &b->char_attrs);
}

/* ---- the style list ---- */

pc_status pc_text_styles_default(pc_text_styles *out, pc_error *err)
{
    pc_text_styles s;
    memset(&s, 0, sizeof s);
    if (pc_str_set_c(&s.basic.name, PC_BASIC_PARAGRAPH, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    *out = s;
    return PC_OK;
}

void pc_text_styles_fini(pc_text_styles *s)
{
    size_t i;
    if (s == NULL) {
        return;
    }
    for (i = 0; i < s->ncharacter && s->character != NULL; i++) {
        pc_character_style_def_fini(&s->character[i]);
    }
    for (i = 0; i < s->nparagraph && s->paragraph != NULL; i++) {
        pc_paragraph_style_def_fini(&s->paragraph[i]);
    }
    free(s->character);
    free(s->paragraph);
    pc_paragraph_style_def_fini(&s->basic);
    pc_json_value_fini(&s->psd_raw);
    memset(s, 0, sizeof *s);
}

pc_status pc_text_styles_clone(const pc_text_styles *s, pc_text_styles *out, pc_error *err)
{
    pc_text_styles c;
    size_t i;
    memset(&c, 0, sizeof c);
    if (s->ncharacter > 0) {
        c.character = pc_calloc_array(s->ncharacter, sizeof *c.character);
        if (c.character == NULL) {
            goto nomem;
        }
        for (i = 0; i < s->ncharacter; i++) {
            if (pc_character_style_def_clone(&s->character[i], &c.character[i], err) != PC_OK) {
                goto nomem;
            }
            c.ncharacter = i + 1;
        }
    }
    if (s->nparagraph > 0) {
        c.paragraph = pc_calloc_array(s->nparagraph, sizeof *c.paragraph);
        if (c.paragraph == NULL) {
            goto nomem;
        }
        for (i = 0; i < s->nparagraph; i++) {
            if (pc_paragraph_style_def_clone(&s->paragraph[i], &c.paragraph[i], err) != PC_OK) {
                goto nomem;
            }
            c.nparagraph = i + 1;
        }
    }
    if (pc_paragraph_style_def_clone(&s->basic, &c.basic, err) != PC_OK ||
        pc_json_value_clone(&s->psd_raw, &c.psd_raw, err) != PC_OK) {
        goto nomem;
    }
    *out = c;
    return PC_OK;
nomem:
    pc_text_styles_fini(&c);
    return pc_fail(err, PC_ERR_NOMEM, "out of memory copying text styles");
}

bool pc_text_styles_eq(const pc_text_styles *a, const pc_text_styles *b)
{
    size_t i;
    if (a->ncharacter != b->ncharacter || a->nparagraph != b->nparagraph) {
        return false;
    }
    for (i = 0; i < a->ncharacter; i++) {
        if (!pc_character_style_def_eq(&a->character[i], &b->character[i])) {
            return false;
        }
    }
    for (i = 0; i < a->nparagraph; i++) {
        if (!pc_paragraph_style_def_eq(&a->paragraph[i], &b->paragraph[i])) {
            return false;
        }
    }
    return pc_paragraph_style_def_eq(&a->basic, &b->basic) && pc_json_value_eq(&a->psd_raw, &b->psd_raw);
}

bool pc_text_styles_is_empty(const pc_text_styles *s)
{
    return s->ncharacter == 0 && s->nparagraph == 0 && pc_style_attrs_is_empty(&s->basic.char_attrs) &&
           pc_style_attrs_is_empty(&s->basic.para_attrs);
}

const pc_character_style_def *pc_text_styles_char_style(const pc_text_styles *s, uint32_t id)
{
    size_t i;
    for (i = 0; i < s->ncharacter; i++) {
        if (s->character[i].id == id) {
            return &s->character[i];
        }
    }
    return NULL;
}

pc_character_style_def *pc_text_styles_char_style_mut(pc_text_styles *s, uint32_t id)
{
    size_t i;
    for (i = 0; i < s->ncharacter; i++) {
        if (s->character[i].id == id) {
            return &s->character[i];
        }
    }
    return NULL;
}

const pc_paragraph_style_def *pc_text_styles_para_style(const pc_text_styles *s, uint32_t id)
{
    size_t i;
    if (id == 0) {
        return &s->basic;
    }
    for (i = 0; i < s->nparagraph; i++) {
        if (s->paragraph[i].id == id) {
            return &s->paragraph[i];
        }
    }
    return NULL;
}

pc_paragraph_style_def *pc_text_styles_para_style_mut(pc_text_styles *s, uint32_t id)
{
    size_t i;
    if (id == 0) {
        return &s->basic;
    }
    for (i = 0; i < s->nparagraph; i++) {
        if (s->paragraph[i].id == id) {
            return &s->paragraph[i];
        }
    }
    return NULL;
}

bool pc_text_styles_next_char_id(const pc_text_styles *s, uint32_t *out)
{
    uint32_t max = 0;
    size_t i;
    for (i = 0; i < s->ncharacter; i++) {
        max = s->character[i].id > max ? s->character[i].id : max;
    }
    if (max == UINT32_MAX) {
        return false;
    }
    *out = max + 1;
    return true;
}

bool pc_text_styles_next_para_id(const pc_text_styles *s, uint32_t *out)
{
    uint32_t max = 0;
    size_t i;
    for (i = 0; i < s->nparagraph; i++) {
        max = s->paragraph[i].id > max ? s->paragraph[i].id : max;
    }
    if (max == UINT32_MAX) {
        return false;
    }
    *out = max + 1;
    return true;
}

static bool name_taken(const pc_text_styles *s, bool paragraph, const char *n, size_t len)
{
    size_t i;
    if (paragraph) {
        if (len == strlen(PC_BASIC_PARAGRAPH) && memcmp(n, PC_BASIC_PARAGRAPH, len) == 0) {
            return true;
        }
        for (i = 0; i < s->nparagraph; i++) {
            if (pc_str_eq_mem(&s->paragraph[i].name, n, len)) {
                return true;
            }
        }
        return false;
    }
    if (len == strlen(PC_NO_CHARACTER_STYLE) && memcmp(n, PC_NO_CHARACTER_STYLE, len) == 0) {
        return true;
    }
    for (i = 0; i < s->ncharacter; i++) {
        if (pc_str_eq_mem(&s->character[i].name, n, len)) {
            return true;
        }
    }
    return false;
}

pc_status pc_text_styles_unique_name(const pc_text_styles *s, bool paragraph, const char *base, pc_str *out,
                                     pc_error *err)
{
    size_t blen = base != NULL ? strlen(base) : 0;
    size_t cap;
    char *buf;
    uint64_t i;
    pc_status st;
    if (!pc_size_add(blen, 24, &cap) || (buf = pc_malloc(cap)) == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory naming a style");
    }
    /* Each style takes at most one name, so a free number turns up within count + 2 tries. */
    for (i = 1;; i++) {
        int n = snprintf(buf, cap, "%s %llu", base != NULL ? base : "", (unsigned long long)i);
        if (n < 0 || (size_t)n >= cap) {
            free(buf);
            return pc_fail(err, PC_ERR_INVALID, "style name too long");
        }
        if (!name_taken(s, paragraph, buf, (size_t)n)) {
            st = pc_str_set(out, buf, (size_t)n, err);
            free(buf);
            return st;
        }
    }
}

/* ---- applying and diffing attribute sets (through the serde forms) ---- */

typedef struct style_ops {
    yyjson_mut_val *(*to_json)(yyjson_mut_doc *d, const void *v, unsigned flags);
    pc_status (*from_json)(yyjson_val *v, void *out, pc_error *err);
} style_ops;

static yyjson_mut_val *char_to_json(yyjson_mut_doc *d, const void *v, unsigned flags)
{
    return pc_char_style_to_json(d, v, flags);
}

static pc_status char_from_json(yyjson_val *v, void *out, pc_error *err)
{
    return pc_char_style_from_json(v, out, err);
}

static yyjson_mut_val *para_to_json(yyjson_mut_doc *d, const void *v, unsigned flags)
{
    return pc_paragraph_style_to_json(d, v, flags);
}

static pc_status para_from_json(yyjson_val *v, void *out, pc_error *err)
{
    return pc_paragraph_style_from_json(v, out, err);
}

static const style_ops CHAR_OPS = {char_to_json, char_from_json};
static const style_ops PARA_OPS = {para_to_json, para_from_json};

/* serde_json::to_value(style) as a normalised object in a new document. */
static pc_status style_value(const style_ops *ops, const void *style, yyjson_mut_doc **out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc *n = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *raw = d != NULL ? ops->to_json(d, style, PC_DOC_JSON_VALUE) : NULL;
    yyjson_mut_val *norm = raw != NULL && n != NULL ? pc_json_normalized_copy(n, raw) : NULL;
    yyjson_mut_doc_free(d);
    if (norm == NULL) {
        yyjson_mut_doc_free(n);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory serialising a style");
    }
    yyjson_mut_doc_set_root(n, norm);
    *out = n;
    return PC_OK;
}

/* apply_attrs: to_value(base), reject keys the style doesn't have, insert, from_value. */
static pc_status apply(const style_ops *ops, const void *base, const pc_style_attrs *attrs, void *out,
                       pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    yyjson_mut_val *root;
    yyjson_mut_val *a = attrs_obj(attrs);
    yyjson_doc *imut;
    pc_status st = style_value(ops, base, &d, err);
    if (st != PC_OK) {
        return st;
    }
    root = yyjson_mut_doc_get_root(d);
    if (a != NULL) {
        yyjson_mut_obj_iter it;
        yyjson_mut_val *k;
        (void)yyjson_mut_obj_iter_init(a, &it);
        while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
            const char *key = yyjson_mut_get_str(k);
            size_t klen = yyjson_mut_get_len(k);
            yyjson_mut_obj_iter rit;
            yyjson_mut_val *rk;
            bool found = false;
            if (key_is(k, REF_KEY, strlen(REF_KEY))) {
                continue;
            }
            (void)yyjson_mut_obj_iter_init(root, &rit);
            while ((rk = yyjson_mut_obj_iter_next(&rit)) != NULL) {
                if (key_is(rk, key, klen)) {
                    yyjson_mut_val *copy = pc_json_normalized_copy(d, yyjson_mut_obj_iter_get_val(k));
                    if (copy == NULL) {
                        yyjson_mut_doc_free(d);
                        return pc_fail(err, PC_ERR_NOMEM, "out of memory applying a style");
                    }
                    /* Replace the value in place; the map keeps its order. */
                    (void)yyjson_mut_obj_replace(root, rk, copy);
                    found = true;
                    break;
                }
            }
            if (!found) {
                char q[PC_ERROR_MSG_MAX];
                pc_utf8_debug_quote((const uint8_t *)key, klen, q, sizeof q);
                yyjson_mut_doc_free(d);
                return pc_fail(err, PC_ERR_INVALID, "unknown style attribute %s", q);
            }
        }
    }
    imut = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    if (imut == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory applying a style");
    }
    st = ops->from_json(yyjson_doc_get_root(imut), out, err);
    yyjson_doc_free(imut);
    return st;
}

/* diff_attrs: entries of style's object that differ from base's (style reference ignored). */
static pc_status diff(const style_ops *ops, const void *style, const void *base, pc_style_attrs *out,
                      pc_error *err)
{
    yyjson_mut_doc *a = NULL;
    yyjson_mut_doc *b = NULL;
    yyjson_mut_doc *o = NULL;
    yyjson_mut_val *obj = NULL;
    yyjson_mut_obj_iter it;
    yyjson_mut_val *k;
    pc_status st = style_value(ops, style, &a, err);
    if (st != PC_OK || (st = style_value(ops, base, &b, err)) != PC_OK) {
        yyjson_mut_doc_free(a);
        return st;
    }
    (void)yyjson_mut_obj_iter_init(yyjson_mut_doc_get_root(a), &it);
    while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        yyjson_mut_val *av = yyjson_mut_obj_iter_get_val(k);
        yyjson_mut_val *bv;
        if (key_is(k, REF_KEY, strlen(REF_KEY))) {
            continue;
        }
        bv = obj_get(yyjson_mut_doc_get_root(b), yyjson_mut_get_str(k), yyjson_mut_get_len(k));
        if (bv != NULL && pc_json_mut_equal(av, bv)) {
            continue;
        }
        if (o == NULL) {
            o = yyjson_mut_doc_new(NULL);
            obj = o != NULL ? yyjson_mut_obj(o) : NULL;
            if (obj == NULL) {
                goto nomem;
            }
            yyjson_mut_doc_set_root(o, obj);
        }
        if (!yyjson_mut_obj_add(obj, yyjson_mut_strncpy(o, yyjson_mut_get_str(k), yyjson_mut_get_len(k)),
                                pc_json_normalized_copy(o, av))) {
            goto nomem;
        }
    }
    yyjson_mut_doc_free(a);
    yyjson_mut_doc_free(b);
    out->doc = o; /* NULL: nothing differs */
    return PC_OK;
nomem:
    yyjson_mut_doc_free(a);
    yyjson_mut_doc_free(b);
    yyjson_mut_doc_free(o);
    return pc_fail(err, PC_ERR_NOMEM, "out of memory comparing styles");
}

/* object(): every attribute without the style reference. */
static pc_status all_attrs(const style_ops *ops, const void *style, pc_style_attrs *out, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    yyjson_mut_val *root;
    pc_status st = style_value(ops, style, &d, err);
    if (st != PC_OK) {
        return st;
    }
    root = yyjson_mut_doc_get_root(d);
    (void)yyjson_mut_obj_remove_key(root, REF_KEY);
    out->doc = d;
    return PC_OK;
}

pc_status pc_char_style_apply_attrs(const pc_char_style *base, const pc_style_attrs *attrs,
                                    pc_char_style *out, pc_error *err)
{
    return apply(&CHAR_OPS, base, attrs, out, err);
}

pc_status pc_paragraph_style_apply_attrs(const pc_paragraph_style *base, const pc_style_attrs *attrs,
                                         pc_paragraph_style *out, pc_error *err)
{
    return apply(&PARA_OPS, base, attrs, out, err);
}

pc_status pc_char_style_diff_attrs(const pc_char_style *style, const pc_char_style *base, pc_style_attrs *out,
                                   pc_error *err)
{
    return diff(&CHAR_OPS, style, base, out, err);
}

pc_status pc_paragraph_style_diff_attrs(const pc_paragraph_style *style, const pc_paragraph_style *base,
                                        pc_style_attrs *out, pc_error *err)
{
    return diff(&PARA_OPS, style, base, out, err);
}

pc_status pc_char_style_all_attrs(const pc_char_style *s, pc_style_attrs *out, pc_error *err)
{
    return all_attrs(&CHAR_OPS, s, out, err);
}

pc_status pc_paragraph_style_all_attrs(const pc_paragraph_style *s, pc_style_attrs *out, pc_error *err)
{
    return all_attrs(&PARA_OPS, s, out, err);
}

pc_status pc_char_style_validate_attrs(const pc_style_attrs *attrs, pc_error *err)
{
    pc_char_style def;
    pc_char_style r;
    pc_status st;
    pc_char_style_default(&def);
    st = pc_char_style_apply_attrs(&def, attrs, &r, err);
    if (st == PC_OK) {
        pc_char_style_fini(&r);
    }
    pc_char_style_fini(&def);
    return st;
}

pc_status pc_paragraph_style_validate_attrs(const pc_style_attrs *attrs, pc_error *err)
{
    pc_paragraph_style def = pc_paragraph_style_default();
    pc_paragraph_style r;
    return pc_paragraph_style_apply_attrs(&def, attrs, &r, err);
}

/* ---- resolution (a failing apply keeps what it had, as the reference's unwrap_or does) ---- */

/* *s = s with attrs applied, unless they don't apply. Only out-of-memory is an error. */
static pc_status char_apply_or_keep(pc_char_style *s, const pc_style_attrs *attrs, pc_error *err)
{
    pc_char_style r;
    pc_error e;
    pc_status st = pc_char_style_apply_attrs(s, attrs, &r, &e);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", e.msg);
    }
    if (st == PC_OK) {
        pc_char_style_fini(s);
        *s = r;
    }
    return PC_OK;
}

static pc_status para_apply_or_keep(pc_paragraph_style *s, const pc_style_attrs *attrs, pc_error *err)
{
    pc_paragraph_style r;
    pc_error e;
    pc_status st = pc_paragraph_style_apply_attrs(s, attrs, &r, &e);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", e.msg);
    }
    if (st == PC_OK) {
        *s = r;
    }
    return PC_OK;
}

pc_status pc_text_styles_resolve_para(const pc_text_styles *s, const uint32_t *para, pc_paragraph_style *out,
                                      pc_error *err)
{
    pc_paragraph_style r = pc_paragraph_style_default();
    const pc_paragraph_style_def *def;
    /* apply_attrs(default, basic).unwrap_or_default() == unwrap_or(default) here. */
    if (para_apply_or_keep(&r, &s->basic.para_attrs, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    def = para != NULL ? pc_text_styles_para_style(s, *para) : NULL;
    if (def != NULL && para_apply_or_keep(&r, &def->para_attrs, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    r.has_style_sheet = para != NULL && *para != 0 && def != NULL;
    r.style_sheet = r.has_style_sheet ? *para : 0;
    *out = r;
    return PC_OK;
}

pc_status pc_text_styles_resolve_char(const pc_text_styles *s, const uint32_t *para, const uint32_t *chr,
                                      const char *default_family, pc_char_style *out, pc_error *err)
{
    pc_char_style r;
    const pc_paragraph_style_def *pdef;
    const pc_character_style_def *cdef;
    pc_char_style_default(&r);
    if (pc_str_set_c(&r.font_family, default_family != NULL ? default_family : "", err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (char_apply_or_keep(&r, &s->basic.char_attrs, err) != PC_OK) {
        goto fail;
    }
    pdef = para != NULL ? pc_text_styles_para_style(s, *para) : NULL;
    if (pdef != NULL && pdef->id != 0 && char_apply_or_keep(&r, &pdef->char_attrs, err) != PC_OK) {
        goto fail;
    }
    cdef = chr != NULL ? pc_text_styles_char_style(s, *chr) : NULL;
    if (cdef != NULL && char_apply_or_keep(&r, &cdef->attrs, err) != PC_OK) {
        goto fail;
    }
    r.has_style_sheet = cdef != NULL;
    r.style_sheet = cdef != NULL ? *chr : 0;
    *out = r;
    return PC_OK;
fail:
    pc_char_style_fini(&r);
    return PC_ERR_NOMEM;
}

pc_status pc_text_styles_char_overrides(const pc_text_styles *s, const pc_char_style *run,
                                        const uint32_t *para, const char *default_family, pc_style_attrs *out,
                                        pc_error *err)
{
    pc_char_style base;
    pc_status st = pc_text_styles_resolve_char(s, para, run->has_style_sheet ? &run->style_sheet : NULL,
                                               default_family, &base, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_char_style_diff_attrs(run, &base, out, err);
    pc_char_style_fini(&base);
    return st;
}

pc_status pc_text_styles_para_overrides(const pc_text_styles *s, const pc_paragraph_style *p,
                                        pc_style_attrs *out, pc_error *err)
{
    pc_paragraph_style base;
    pc_status st = pc_text_styles_resolve_para(s, p->has_style_sheet ? &p->style_sheet : NULL, &base, err);
    if (st != PC_OK) {
        return st;
    }
    return pc_paragraph_style_diff_attrs(p, &base, out, err);
}

pc_status pc_text_styles_restyle_char(const pc_text_styles *s, const pc_text_styles *old,
                                      const pc_char_style *run, const uint32_t *old_para,
                                      const uint32_t *new_para, const uint32_t *new_chr, const char *family,
                                      pc_char_style *out, pc_error *err)
{
    pc_style_attrs overrides = PC_JSON_VALUE_INIT;
    pc_char_style base;
    pc_status st = pc_text_styles_char_overrides(old, run, old_para, family, &overrides, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_text_styles_resolve_char(s, new_para, new_chr, family, &base, err);
    if (st == PC_OK) {
        st = char_apply_or_keep(&base, &overrides, err);
        if (st == PC_OK) {
            *out = base;
        } else {
            pc_char_style_fini(&base);
        }
    }
    pc_json_value_fini(&overrides);
    return st;
}

pc_status pc_text_styles_restyle_para(const pc_text_styles *s, const pc_text_styles *old,
                                      const pc_paragraph_style *p, const uint32_t *new_para,
                                      pc_paragraph_style *out, pc_error *err)
{
    pc_style_attrs overrides = PC_JSON_VALUE_INIT;
    pc_paragraph_style base;
    pc_status st = pc_text_styles_para_overrides(old, p, &overrides, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_text_styles_resolve_para(s, new_para, &base, err);
    if (st == PC_OK) {
        st = para_apply_or_keep(&base, &overrides, err);
    }
    pc_json_value_fini(&overrides);
    if (st != PC_OK) {
        return st;
    }
    base.has_style_sheet =
        new_para != NULL && *new_para != 0 && pc_text_styles_para_style(s, *new_para) != NULL;
    base.style_sheet = base.has_style_sheet ? *new_para : 0;
    *out = base;
    return PC_OK;
}
