/* The plug-in manifest (pc_manifest) and its parameter schema (port of
 * crates/plugins/src/manifest.rs): validation, the dialog notation, parameter resolution and
 * serialisation. Parsing itself is manifest_de.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rfmt.h"
#include "base/utf8.h"
#include "plugins/internal.h"

/* ---- character classes for menu labels ---- */

/* The Unicode format characters (general category Cf, Unicode 15.1). Most draw nothing, and the
 * bidi controls among them reorder the text around them. */
static const uint32_t FORMAT[][2] = {
    {0xAD, 0xAD},       {0x600, 0x605},     {0x61C, 0x61C},     {0x6DD, 0x6DD},     {0x70F, 0x70F},
    {0x890, 0x891},     {0x8E2, 0x8E2},     {0x180E, 0x180E},   {0x200B, 0x200F},   {0x202A, 0x202E},
    {0x2060, 0x2064},   {0x2066, 0x206F},   {0xFEFF, 0xFEFF},   {0xFFF9, 0xFFFB},   {0x110BD, 0x110BD},
    {0x110CD, 0x110CD}, {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0001, 0xE0001},
    {0xE0020, 0xE007F},
};

/* Default-ignorable characters outside Cf (fillers, variation selectors): allowed in a name, but
 * they draw nothing on their own. */
static const uint32_t BLANK[][2] = {
    {0x34F, 0x34F},   {0x115F, 0x1160}, {0x17B4, 0x17B5}, {0x180B, 0x180D},   {0x180F, 0x180F},
    {0x3164, 0x3164}, {0xFE00, 0xFE0F}, {0xFFA0, 0xFFA0}, {0xE0100, 0xE01EF},
};

static bool in_ranges(uint32_t c, const uint32_t (*r)[2], size_t n)
{
    for (size_t k = 0; k < n; k++) {
        if (c >= r[k][0] && c <= r[k][1]) {
            return true;
        }
    }
    return false;
}

static bool is_control(uint32_t c)
{
    return c < 0x20 || (c >= 0x7F && c <= 0x9F);
}

/* A format character a menu label must not carry. ZWNJ and ZWJ stay allowed (Persian, Indic
 * scripts and emoji sequences need them; they cannot reorder text). */
static bool hidden_format(uint32_t c)
{
    return c != 0x200C && c != 0x200D && in_ranges(c, FORMAT, sizeof FORMAT / sizeof FORMAT[0]);
}

static bool visible(uint32_t c)
{
    return !pc_unicode_is_space(c) && !is_control(c) &&
           !in_ranges(c, FORMAT, sizeof FORMAT / sizeof FORMAT[0]) &&
           !in_ranges(c, BLANK, sizeof BLANK / sizeof BLANK[0]);
}

bool pc_plugin_valid_id(const char *id, size_t len)
{
    if (id == NULL || len < 1 || len > 64) {
        return false;
    }
    for (size_t k = 0; k < len; k++) {
        char b = id[k];
        bool alnum = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9');
        if (!alnum && b != '.' && b != '_' && b != '-') {
            return false;
        }
    }
    return true;
}

static bool valid_key(const pc_str *k)
{
    const char *s = pc_str_c(k);
    if (k->len < 1 || k->len > 64 || s[0] == '_') {
        return false;
    }
    for (size_t i = 0; i < k->len; i++) {
        char b = s[i];
        if (!((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_')) {
            return false;
        }
    }
    return !pc_str_eq_c(k, "id") && !pc_str_eq_c(k, "layer");
}

static bool valid_name(const pc_str *name)
{
    size_t start = 0, len = pc_utf8_trim(pc_str_c(name), name->len, &start), i = 0, count = 0;
    const uint8_t *u = (const uint8_t *)pc_str_c(name) + start;
    bool any_visible = false;
    while (i < len) {
        uint32_t cp = 0;
        if (!pc_utf8_next(u, len, &i, &cp) || is_control(cp) || hidden_format(cp)) {
            return false;
        }
        any_visible = any_visible || visible(cp);
        count++;
    }
    return any_visible && count <= 64;
}

/* ---- parse and validate ---- */

static pc_status validate(const pc_plugin_manifest *m, pc_error *err)
{
    if (!pc_plugin_valid_id(pc_str_c(&m->id), m->id.len)) {
        pc_buf q = PC_BUF_INIT;
        pc_plugin_debug_quote(pc_str_c(&m->id), m->id.len, &q);
        pc_plugin_fail_set(err, PC_PLUGIN_E_MANIFEST,
                           "invalid id %s: use 1-64 characters from A-Z a-z 0-9 . _ -", pc_buf_cstr(&q));
        pc_buf_fini(&q);
        return PC_ERR_FORMAT;
    }
    if (!valid_name(&m->name)) {
        return PC_PLUGIN_FAIL_MANIFEST(err, "`name` must be 1-64 printable characters");
    }
    if (m->version.len > 32 || m->description.len > 1024 || m->author.len > 128) {
        return PC_PLUGIN_FAIL_MANIFEST(err, "`version`, `description` or `author` is too long");
    }
    if (m->overlap > PC_PLUGIN_MAX_OVERLAP) {
        return PC_PLUGIN_FAIL_MANIFEST(err, "`overlap` %u exceeds %u", (unsigned)m->overlap,
                                       PC_PLUGIN_MAX_OVERLAP);
    }
    for (size_t i = 0; i < m->n_params; i++) {
        const pc_param_spec *ps = &m->params[i];
        const char *k = pc_str_c(&ps->name);
        if (!valid_key(&ps->name)) {
            pc_buf q = PC_BUF_INIT;
            pc_plugin_debug_quote(k, ps->name.len, &q);
            pc_plugin_fail_set(err, PC_PLUGIN_E_MANIFEST, "invalid parameter name %s", pc_buf_cstr(&q));
            pc_buf_fini(&q);
            return PC_ERR_FORMAT;
        }
        switch (ps->type) {
        case PC_PARAM_NUMBER:
            if (!(isfinite(ps->min) && isfinite(ps->max) && isfinite(ps->def)) || ps->min > ps->max) {
                return PC_PLUGIN_FAIL_MANIFEST(err, "parameter `%s`: bad range", k);
            }
            break;
        case PC_PARAM_INT:
            if (ps->imin > ps->imax) {
                return PC_PLUGIN_FAIL_MANIFEST(err, "parameter `%s`: bad range", k);
            }
            break;
        case PC_PARAM_CHOICE: {
            bool bad = ps->n_options == 0 || ps->n_options > 64, found = !ps->has_cdef;
            for (size_t o = 0; o < ps->n_options && !bad; o++) {
                const pc_str *opt = &ps->options[o];
                bad = opt->len == 0 || memchr(pc_str_c(opt), '|', opt->len) != NULL ||
                      memchr(pc_str_c(opt), '"', opt->len) != NULL;
                found = found || pc_str_eq(opt, &ps->cdef);
            }
            if (bad) {
                return PC_PLUGIN_FAIL_MANIFEST(
                    err, "parameter `%s`: choices must be 1-64 non-empty strings without | or \"", k);
            }
            if (!found) {
                return PC_PLUGIN_FAIL_MANIFEST(err, "parameter `%s`: default is not one of the options", k);
            }
            break;
        }
        case PC_PARAM_BOOL:
            break;
        }
    }
    return PC_OK;
}

pc_status pc_plugin_manifest_parse(const uint8_t *bytes, size_t len, pc_plugin_manifest *out, pc_error *err)
{
    pc_plugin_manifest m;
    pc_error de_err;
    pc_status st;
    size_t i = 0;
    if (len > PC_PLUGIN_MAX_MANIFEST_BYTES) {
        return PC_PLUGIN_FAIL_MANIFEST(err, "manifest is %zu bytes (limit %zu)", len,
                                       PC_PLUGIN_MAX_MANIFEST_BYTES);
    }
    if (bytes == NULL && len > 0) {
        return PC_PLUGIN_FAIL_MANIFEST(err, "manifest is not UTF-8");
    }
    while (i < len) {
        uint32_t cp;
        if (!pc_utf8_next(bytes, len, &i, &cp)) {
            return PC_PLUGIN_FAIL_MANIFEST(err, "manifest is not UTF-8");
        }
    }
    pc_error_clear(&de_err);
    st = pc_plugin_manifest_deserialize(len > 0 ? (const char *)bytes : "", len, &m, &de_err);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", de_err.msg);
    }
    if (st != PC_OK) {
        return PC_PLUGIN_FAIL_MANIFEST(err, "manifest: %s", de_err.msg);
    }
    st = validate(&m, err);
    if (st != PC_OK) {
        pc_plugin_manifest_fini(&m);
        return st;
    }
    *out = m;
    return PC_OK;
}

static void param_spec_fini(pc_param_spec *ps)
{
    pc_str_fini(&ps->name);
    for (size_t k = 0; k < ps->n_options; k++) {
        pc_str_fini(&ps->options[k]);
    }
    free(ps->options);
    pc_str_fini(&ps->cdef);
    memset(ps, 0, sizeof *ps);
}

void pc_plugin_manifest_fini(pc_plugin_manifest *m)
{
    if (m == NULL) {
        return;
    }
    pc_str_fini(&m->id);
    pc_str_fini(&m->name);
    pc_str_fini(&m->version);
    pc_str_fini(&m->description);
    pc_str_fini(&m->author);
    for (size_t i = 0; i < m->n_params; i++) {
        param_spec_fini(&m->params[i]);
    }
    free(m->params);
    memset(m, 0, sizeof *m);
}

static pc_status param_spec_clone(const pc_param_spec *ps, pc_param_spec *out, pc_error *err)
{
    pc_param_spec c = *ps;
    c.name = (pc_str)PC_STR_INIT;
    c.cdef = (pc_str)PC_STR_INIT;
    c.options = NULL;
    c.n_options = 0;
    if (pc_str_clone(&ps->name, &c.name, err) != PC_OK || pc_str_clone(&ps->cdef, &c.cdef, err) != PC_OK) {
        param_spec_fini(&c);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a plug-in manifest");
    }
    if (ps->n_options > 0) {
        c.options = pc_calloc_array(ps->n_options, sizeof *c.options);
        if (c.options == NULL) {
            param_spec_fini(&c);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a plug-in manifest");
        }
        for (size_t k = 0; k < ps->n_options; k++) {
            c.n_options = k + 1;
            if (pc_str_clone(&ps->options[k], &c.options[k], err) != PC_OK) {
                param_spec_fini(&c);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a plug-in manifest");
            }
        }
    }
    *out = c;
    return PC_OK;
}

pc_status pc_plugin_manifest_clone(const pc_plugin_manifest *m, pc_plugin_manifest *out, pc_error *err)
{
    pc_plugin_manifest c;
    memset(&c, 0, sizeof c);
    c.kind = m->kind;
    c.overlap = m->overlap;
    c.area = m->area;
    if (pc_str_clone(&m->id, &c.id, err) != PC_OK || pc_str_clone(&m->name, &c.name, err) != PC_OK ||
        pc_str_clone(&m->version, &c.version, err) != PC_OK ||
        pc_str_clone(&m->description, &c.description, err) != PC_OK ||
        pc_str_clone(&m->author, &c.author, err) != PC_OK) {
        pc_plugin_manifest_fini(&c);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a plug-in manifest");
    }
    if (m->n_params > 0) {
        c.params = pc_calloc_array(m->n_params, sizeof *c.params);
        if (c.params == NULL) {
            pc_plugin_manifest_fini(&c);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a plug-in manifest");
        }
        for (size_t i = 0; i < m->n_params; i++) {
            if (param_spec_clone(&m->params[i], &c.params[i], err) != PC_OK) {
                pc_plugin_manifest_fini(&c);
                return PC_ERR_NOMEM;
            }
            c.n_params = i + 1;
        }
    }
    *out = c;
    return PC_OK;
}

/* ---- the dialog notation ---- */

pc_status pc_plugin_manifest_params_notation(const pc_plugin_manifest *m, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status st = pc_buf_append_byte(&b, '{', err);
    for (size_t i = 0; i < m->n_params && st == PC_OK; i++) {
        const pc_param_spec *ps = &m->params[i];
        const char *k = pc_str_c(&ps->name);
        char t0[PC_RUST_F64_TEXT_MAX], t1[PC_RUST_F64_TEXT_MAX], t2[PC_RUST_F64_TEXT_MAX];
        if (i > 0) {
            st = pc_buf_append_byte(&b, ',', err);
        }
        if (st != PC_OK) {
            break;
        }
        switch (ps->type) {
        case PC_PARAM_NUMBER:
            (void)pc_rust_display_f64(ps->min, t0);
            (void)pc_rust_display_f64(ps->max, t1);
            (void)pc_rust_display_f64(ps->def, t2);
            st = pc_buf_appendf(&b, err, "\"%s\":%s..%s=%s", k, t0, t1, t2);
            break;
        case PC_PARAM_INT:
            st = pc_buf_appendf(&b, err, "\"%s\":int=%lld", k, (long long)ps->idef);
            break;
        case PC_PARAM_BOOL:
            st = pc_buf_appendf(&b, err, "\"%s\":bool=%s", k, ps->bdef ? "true" : "false");
            break;
        case PC_PARAM_CHOICE: {
            /* The dialog's default is the first option, so the default is listed first. */
            size_t first = SIZE_MAX;
            for (size_t o = 0; o < ps->n_options && ps->has_cdef; o++) {
                if (pc_str_eq(&ps->options[o], &ps->cdef)) {
                    first = o;
                    break;
                }
            }
            st = pc_buf_appendf(&b, err, "\"%s\":\"", k);
            /* Order: the default (if found), then the others in declaration order. */
            for (size_t n = 0; n < ps->n_options && st == PC_OK; n++) {
                size_t o = first == SIZE_MAX ? n : n == 0 ? first : n <= first ? n - 1 : n;
                if (n > 0) {
                    st = pc_buf_append_byte(&b, '|', err);
                }
                if (st == PC_OK) {
                    st = pc_buf_append(&b, pc_str_c(&ps->options[o]), ps->options[o].len, err);
                }
            }
            if (st == PC_OK) {
                st = pc_buf_append_byte(&b, '"', err);
            }
            break;
        }
        }
    }
    if (st == PC_OK) {
        st = pc_buf_append_byte(&b, '}', err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(out, b.data, b.len, err);
    }
    pc_buf_fini(&b);
    return st;
}

/* ---- parameter resolution ---- */

static pc_status params_err(pc_error *err, const char *k, const char *what)
{
    return PC_PLUGIN_FAIL_PARAMS(err, "`%s` must be %s", k, what);
}

pc_status pc_plugin_manifest_resolve_params(const pc_plugin_manifest *m, yyjson_val *params,
                                            pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *doc;
    yyjson_mut_val *obj;
    pc_status st = PC_OK;
    if (params != NULL && !yyjson_is_null(params) && !yyjson_is_obj(params)) {
        return PC_PLUGIN_FAIL_PARAMS(err, "plug-in parameters must be a JSON object");
    }
    doc = yyjson_mut_doc_new(NULL);
    obj = doc != NULL ? yyjson_mut_obj(doc) : NULL;
    if (obj == NULL) {
        yyjson_mut_doc_free(doc);
        return PC_PLUGIN_FAIL_NOMEM(err);
    }
    for (size_t i = 0; i < m->n_params && st == PC_OK; i++) {
        const pc_param_spec *ps = &m->params[i];
        const char *k = pc_str_c(&ps->name);
        yyjson_val *v = yyjson_is_obj(params) ? pc_json_get(params, k) : NULL;
        yyjson_mut_val *r = NULL;
        double f = 0.0;
        if (v != NULL && yyjson_is_null(v)) {
            v = NULL;
        }
        switch (ps->type) {
        case PC_PARAM_NUMBER:
            if (v == NULL) {
                f = ps->def;
            } else if (!pc_json_as_f64(v, &f) || !isfinite(f)) {
                st = params_err(err, k, "a number");
                break;
            }
            r = yyjson_mut_real(doc, pc_clampd(f, ps->min, ps->max));
            break;
        case PC_PARAM_INT:
            if (v == NULL) {
                r = yyjson_mut_sint(doc, pc_clamp_i64(ps->idef, ps->imin, ps->imax));
            } else if (!pc_json_as_f64(v, &f) || !isfinite(f)) {
                st = params_err(err, k, "a number");
            } else {
                r = yyjson_mut_sint(doc,
                                    pc_f64_to_i64(pc_clampd(round(f), (double)ps->imin, (double)ps->imax)));
            }
            break;
        case PC_PARAM_BOOL: {
            bool b = ps->bdef;
            if (v != NULL && !pc_json_as_bool(v, &b)) {
                st = params_err(err, k, "true or false");
                break;
            }
            r = yyjson_mut_bool(doc, b);
            break;
        }
        case PC_PARAM_CHOICE:
            if (v == NULL) {
                const pc_str *d = ps->has_cdef ? &ps->cdef : ps->n_options > 0 ? &ps->options[0] : NULL;
                r = yyjson_mut_strncpy(doc, d != NULL ? pc_str_c(d) : "", d != NULL ? d->len : 0);
            } else {
                size_t sl = 0;
                const char *s = pc_json_as_str(v, &sl);
                bool found = false;
                if (s == NULL) {
                    st = params_err(err, k, "a string");
                    break;
                }
                for (size_t o = 0; o < ps->n_options && !found; o++) {
                    found = pc_str_eq_mem(&ps->options[o], s, sl);
                }
                if (!found) {
                    pc_buf list = PC_BUF_INIT;
                    for (size_t o = 0; o < ps->n_options; o++) {
                        if (o > 0) {
                            (void)pc_buf_append_str(&list, ", ", NULL);
                        }
                        (void)pc_buf_append(&list, pc_str_c(&ps->options[o]), ps->options[o].len, NULL);
                    }
                    st = PC_PLUGIN_FAIL_PARAMS(err, "`%s` must be one of %s", k, pc_buf_cstr(&list));
                    pc_buf_fini(&list);
                    break;
                }
                r = yyjson_mut_strncpy(doc, s, sl);
            }
            break;
        }
        if (st == PC_OK &&
            (r == NULL || !yyjson_mut_obj_add(obj, yyjson_mut_strncpy(doc, k, ps->name.len), r))) {
            st = PC_PLUGIN_FAIL_NOMEM(err);
        }
    }
    if (st == PC_OK) {
        pc_json_value v = PC_JSON_VALUE_INIT;
        st = pc_json_value_from_mut(obj, &v, err);
        if (st == PC_OK) {
            *out = v;
        }
    }
    yyjson_mut_doc_free(doc);
    return st;
}

/* ---- serialisation (serde's derive(Serialize)) ---- */

static yyjson_mut_val *str_val(yyjson_mut_doc *doc, const pc_str *s)
{
    return yyjson_mut_strncpy(doc, pc_str_c(s), s->len);
}

static bool add(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, yyjson_mut_val *v)
{
    return v != NULL && yyjson_mut_obj_add(obj, yyjson_mut_str(doc, key), v);
}

static yyjson_mut_val *param_json(yyjson_mut_doc *doc, const pc_param_spec *ps)
{
    static const char *const TAGS[4] = {"number", "int", "bool", "choice"};
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    bool ok = o != NULL && add(doc, o, "type", yyjson_mut_str(doc, TAGS[ps->type]));
    switch (ps->type) {
    case PC_PARAM_NUMBER:
        ok = ok && add(doc, o, "min", yyjson_mut_real(doc, ps->min)) &&
             add(doc, o, "max", yyjson_mut_real(doc, ps->max)) &&
             add(doc, o, "default", yyjson_mut_real(doc, ps->def));
        break;
    case PC_PARAM_INT:
        ok = ok && add(doc, o, "min", yyjson_mut_sint(doc, ps->imin)) &&
             add(doc, o, "max", yyjson_mut_sint(doc, ps->imax)) &&
             add(doc, o, "default", yyjson_mut_sint(doc, ps->idef));
        break;
    case PC_PARAM_BOOL:
        ok = ok && add(doc, o, "default", yyjson_mut_bool(doc, ps->bdef));
        break;
    case PC_PARAM_CHOICE: {
        yyjson_mut_val *a = yyjson_mut_arr(doc);
        ok = ok && a != NULL;
        for (size_t k = 0; k < ps->n_options && ok; k++) {
            ok = yyjson_mut_arr_append(a, str_val(doc, &ps->options[k]));
        }
        ok = ok && add(doc, o, "options", a) &&
             add(doc, o, "default", ps->has_cdef ? str_val(doc, &ps->cdef) : yyjson_mut_null(doc));
        break;
    }
    }
    return ok ? o : NULL;
}

yyjson_mut_val *pc_plugin_manifest_to_json(const pc_plugin_manifest *m, yyjson_mut_doc *doc)
{
    yyjson_mut_val *o, *params;
    bool ok;
    if (m == NULL || doc == NULL) {
        return NULL;
    }
    o = yyjson_mut_obj(doc);
    params = yyjson_mut_obj(doc);
    ok = o != NULL && params != NULL;
    for (size_t i = 0; i < m->n_params && ok; i++) {
        ok = yyjson_mut_obj_add(params, str_val(doc, &m->params[i].name), param_json(doc, &m->params[i]));
    }
    ok = ok && add(doc, o, "id", str_val(doc, &m->id)) && add(doc, o, "name", str_val(doc, &m->name)) &&
         add(doc, o, "version", str_val(doc, &m->version)) &&
         add(doc, o, "kind", yyjson_mut_str(doc, "filter")) &&
         add(doc, o, "description", str_val(doc, &m->description)) &&
         add(doc, o, "author", str_val(doc, &m->author)) && add(doc, o, "params", params) &&
         add(doc, o, "overlap", yyjson_mut_uint(doc, m->overlap)) &&
         add(doc, o, "area", yyjson_mut_str(doc, m->area == PC_PLUGIN_AREA_CANVAS ? "canvas" : "content"));
    return ok ? o : NULL;
}
