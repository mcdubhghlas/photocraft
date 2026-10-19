#include "ui/credits.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "text/unicode.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
static const char *string(yyjson_val *o, const char *key)
{
    const char *s = yyjson_get_str(yyjson_obj_get(o, key));
    return s ? s : "";
}
void pc_ui_credits_fini(pc_ui_credits *c)
{
    if (!c)
        return;
    for (size_t i = 0; i < c->count; i++) {
        free(c->contributors[i].display_name);
        free(c->contributors[i].real_name);
    }
    free(c->contributors);
    free(c->models);
    yyjson_doc_free(c->source);
    *c = (pc_ui_credits){0};
}
/* people.toml uses tables of scalar strings/booleans. Parse that bounded subset rather than
 * treating names as code or depending on a host-language tool at build/run time. */
static char *toml_string(const char *text, size_t length)
{
    if (!length || (*text != '"' && *text != '\''))
        return NULL;
    char quote = *text++;
    length--;
    pc_buf value = PC_BUF_INIT;
    bool closed = false, ok = true;
    for (size_t i = 0; ok && i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == (unsigned char)quote) {
            closed = true;
            break;
        }
        if (c == '\\' && quote == '"') {
            if (++i >= length) {
                ok = false;
                break;
            }
            c = (unsigned char)text[i];
            if (c == 'n')
                c = '\n';
            else if (c == 'r')
                c = '\r';
            else if (c == 't')
                c = '\t';
            else if (c == 'b')
                c = '\b';
            else if (c == 'f')
                c = '\f';
            else if (c == 'u' || c == 'U') {
                size_t digits = c == 'u' ? 4 : 8;
                uint32_t cp = 0;
                if (length - i - 1 < digits) {
                    ok = false;
                    break;
                }
                for (size_t j = 0; j < digits; j++) {
                    char h = text[++i];
                    unsigned n = h >= '0' && h <= '9'   ? (unsigned)(h - '0')
                                 : h >= 'a' && h <= 'f' ? (unsigned)(h - 'a') + 10
                                 : h >= 'A' && h <= 'F' ? (unsigned)(h - 'A') + 10
                                                        : 16;
                    if (n > 15) {
                        ok = false;
                        break;
                    }
                    cp = cp * 16 + n;
                }
                if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                    ok = false;
                if (ok)
                    ok = pc_utf8_put(&value, cp, NULL) == PC_OK;
                continue;
            } else if (c != '"' && c != '\\') {
                ok = false;
                break;
            }
        }
        if (!c || c < 0x20) {
            ok = false;
            break;
        }
        ok = pc_buf_append(&value, &c, 1, NULL) == PC_OK;
    }
    size_t start = 0, n = pc_utf8_trim(pc_buf_cstr(&value), value.len, &start);
    char *result = ok && closed && n ? pc_strndup(pc_buf_cstr(&value) + start, n) : NULL;
    pc_buf_fini(&value);
    return result;
}
static void consent_names(pc_ui_contributor *row, yyjson_val *json, const char *people, size_t length)
{
    if (!yyjson_is_true(yyjson_obj_get(json, "people_entry")))
        return;
    bool active = false, sync = false;
    for (size_t offset = 0; offset < length;) {
        const char *line = people + offset, *end = memchr(line, '\n', length - offset);
        size_t n = end ? (size_t)(end - line) : length - offset;
        offset += n + (end ? 1u : 0u);
        while (n && isspace((unsigned char)*line)) {
            line++;
            n--;
        }
        if (!n || *line == '#')
            continue;
        if (*line == '[') {
            const char prefix[] = "[people.";
            const char *close = memchr(line, ']', n);
            size_t name = close && (size_t)(close - line) >= sizeof prefix - 1
                              ? (size_t)(close - line) - (sizeof prefix - 1)
                              : 0;
            active = n >= sizeof prefix && !memcmp(line, prefix, sizeof prefix - 1) &&
                     name == strlen(row->login) && !memcmp(line + sizeof prefix - 1, row->login, name);
            continue;
        }
        if (!active)
            continue;
        const char *equals = memchr(line, '=', n);
        if (!equals)
            continue;
        size_t keylen = (size_t)(equals - line);
        while (keylen && isspace((unsigned char)line[keylen - 1]))
            keylen--;
        const char *value = equals + 1;
        size_t left = n - (size_t)(value - line);
        while (left && isspace((unsigned char)*value)) {
            value++;
            left--;
        }
        if (keylen == 16 && !memcmp(line, "sync_github_name", 16))
            sync = left >= 4 && !memcmp(value, "true", 4) &&
                   (left == 4 || isspace((unsigned char)value[4]) || value[4] == '#');
        else if (keylen == 12 && !memcmp(line, "display_name", 12)) {
            free(row->display_name);
            row->display_name = toml_string(value, left);
        } else if (keylen == 9 && !memcmp(line, "real_name", 9)) {
            free(row->real_name);
            row->real_name = toml_string(value, left);
        }
    }
    if (!row->display_name && sync) {
        const char *name = string(json, "github_name");
        size_t start = 0, n = pc_utf8_trim(name, strlen(name), &start);
        if (n)
            row->display_name = pc_strndup(name + start, n);
    }
}
pc_status pc_ui_credits_parse(const char *json, size_t length, const char *people, size_t people_length,
                              pc_ui_credits *out, pc_error *err)
{
    if (!json || !out || length > 4 * 1024 * 1024 || people_length > 1024 * 1024 ||
        (!people && people_length))
        return pc_fail(err, PC_ERR_INVALID, "invalid contributor data");
    pc_ui_credits next = {0};
    pc_status status = pc_json_parse(json, length, &next.source, err);
    yyjson_val *root = next.source ? yyjson_doc_get_root(next.source) : NULL,
               *rows = yyjson_obj_get(root, "contributors"), *models = yyjson_obj_get(root, "models");
    if (status == PC_OK && (!yyjson_is_obj(root) || !yyjson_is_arr(rows) || yyjson_arr_size(rows) > 4096 ||
                            (models && (!yyjson_is_arr(models) || yyjson_arr_size(models) > 4096))))
        status = pc_fail(err, PC_ERR_INVALID, "invalid contributor tables");
    if (status == PC_OK) {
        next.contributors = pc_calloc_array(yyjson_arr_size(rows) + 1, sizeof *next.contributors);
        next.models = pc_calloc_array(yyjson_arr_size(models) + 1, sizeof *next.models);
        if (!next.contributors || !next.models)
            status = pc_fail(err, PC_ERR_NOMEM, "contributors");
    }
    for (size_t i = 0; status == PC_OK && i < yyjson_arr_size(rows); i++) {
        yyjson_val *v = yyjson_arr_get(rows, i);
        const char *login = string(v, "login");
        if (!*login || strlen(login) != yyjson_get_len(yyjson_obj_get(v, "login")))
            continue;
        pc_ui_contributor *c = &next.contributors[next.count++];
        c->login = login;
        c->first = string(v, "first_commit");
        c->last = string(v, "last_commit");
        c->prs = yyjson_get_uint(yyjson_obj_get(v, "prs"));
        c->commits = yyjson_get_uint(yyjson_obj_get(v, "commits"));
        c->added = yyjson_get_uint(yyjson_obj_get(v, "lines_added"));
        c->deleted = yyjson_get_uint(yyjson_obj_get(v, "lines_deleted"));
        c->binary_added = yyjson_get_uint(yyjson_obj_get(v, "binary_added"));
        c->binary_deleted = yyjson_get_uint(yyjson_obj_get(v, "binary_deleted"));
        consent_names(c, v, people, people_length);
    }
    for (size_t i = 0; status == PC_OK && i < yyjson_arr_size(models); i++) {
        yyjson_val *v = yyjson_arr_get(models, i);
        pc_ui_credit_model *m = &next.models[next.nmodels++];
        m->company = string(v, "company");
        m->model = string(v, "model");
        m->version = string(v, "version");
        m->commits = yyjson_get_uint(yyjson_obj_get(v, "commits"));
        m->added = yyjson_get_uint(yyjson_obj_get(v, "lines_added"));
        m->deleted = yyjson_get_uint(yyjson_obj_get(v, "lines_deleted"));
    }
    if (status == PC_OK) {
        next.total_commits = yyjson_get_uint(yyjson_obj_get(root, "commits"));
        *out = next;
    } else
        pc_ui_credits_fini(&next);
    return status;
}
pc_status pc_ui_credits_embedded(pc_ui_credits *out, pc_error *err)
{
    return pc_ui_credits_parse((const char *)pc_ui_contributors_json, pc_ui_contributors_json_len,
                               (const char *)pc_ui_people_toml, pc_ui_people_toml_len, out, err);
}
const char *pc_ui_credit_chosen_name(const pc_ui_contributor *c, pc_ui_credit_name_mode mode)
{
    if (!c)
        return NULL;
    return mode == PC_CREDIT_DISPLAY_NAME && c->display_name ? c->display_name
           : mode == PC_CREDIT_REAL_NAME && c->real_name     ? c->real_name
                                                             : NULL;
}
static int number(uint64_t a, uint64_t b)
{
    return a < b ? -1 : a > b ? 1 : 0;
}
static int name_compare(const pc_ui_contributor *a, const pc_ui_contributor *b, pc_ui_credit_name_mode mode)
{
    const char *x = pc_ui_credit_chosen_name(a, mode), *y = pc_ui_credit_chosen_name(b, mode);
    if (!x)
        x = a->login;
    if (!y)
        y = b->login;
    while (*x == '@')
        x++;
    while (*y == '@')
        y++;
    return pc_text_cmp_lower(x, strlen(x), y, strlen(y));
}
int pc_ui_credit_compare(const pc_ui_contributor *a, const pc_ui_contributor *b, pc_ui_credit_name_mode mode,
                         pc_ui_credit_sort key, bool ascending)
{
    int result = 0;
    switch (key) {
    case PC_CREDIT_NAME:
        result = name_compare(a, b, mode);
        break;
    case PC_CREDIT_PRS:
        result = number(a->prs, b->prs);
        break;
    case PC_CREDIT_COMMITS:
        result = number(a->commits, b->commits);
        break;
    case PC_CREDIT_ADDED:
        result = number(a->added, b->added);
        break;
    case PC_CREDIT_DELETED:
        result = number(a->deleted, b->deleted);
        break;
    case PC_CREDIT_BINARY_ADDED:
        result = number(a->binary_added, b->binary_added);
        break;
    case PC_CREDIT_BINARY_DELETED:
        result = number(a->binary_deleted, b->binary_deleted);
        break;
    case PC_CREDIT_DELTA: {
        bool aneg = a->added < a->deleted, bneg = b->added < b->deleted;
        uint64_t x = aneg ? a->deleted - a->added : a->added - a->deleted,
                 y = bneg ? b->deleted - b->added : b->added - b->deleted;
        result = aneg != bneg ? (aneg ? -1 : 1) : number(x, y) * (aneg ? -1 : 1);
        break;
    }
    case PC_CREDIT_FIRST:
    case PC_CREDIT_LAST: {
        const char *x = key == PC_CREDIT_FIRST ? a->first : a->last,
                   *y = key == PC_CREDIT_FIRST ? b->first : b->last;
        result = !*x != !*y ? (!*x ? 1 : -1) : strcmp(x, y);
        break;
    }
    default:
        break;
    }
    result = result < 0 ? -1 : result > 0 ? 1 : 0;
    if (!ascending)
        result = -result;
    if (!result)
        result = name_compare(a, b, mode);
    if (!result)
        result = strcmp(a->login, b->login);
    return result;
}
pc_status pc_ui_credits_order(const pc_ui_credits *c, pc_ui_credit_name_mode names, pc_ui_credit_sort key,
                              bool ascending, size_t **out, pc_error *err)
{
    if (!c || !out || c->count > 4096 || (c->count && !c->contributors))
        return pc_fail(err, PC_ERR_INVALID, "invalid credits order");
    size_t *indices = pc_malloc_array(c->count + 1, sizeof *indices);
    if (!indices)
        return pc_fail(err, PC_ERR_NOMEM, "credits order");
    for (size_t i = 0; i < c->count; i++) {
        size_t j = i;
        while (j && pc_ui_credit_compare(&c->contributors[i], &c->contributors[indices[j - 1]], names, key,
                                         ascending) < 0) {
            indices[j] = indices[j - 1];
            j--;
        }
        indices[j] = i;
    }
    *out = indices;
    return PC_OK;
}
