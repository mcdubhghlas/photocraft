#include "ui/parameter_schema.h"
#include "base/mem.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
void pc_ui_parameter_schema_fini(pc_ui_parameter_schema *s)
{
    for (size_t i = 0; i < s->count; i++) {
        pc_ui_param *p = &s->items[i];
        free(p->key);
        free(p->text);
        for (size_t j = 0; j < p->nchoices; j++)
            free(p->choices[j]);
        free(p->choices);
    }
    free(s->items);
    memset(s, 0, sizeof *s);
}
static const char *space(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p))
        p++;
    return p;
}
static bool numeric(const char *p, const char *end, double *value, const char **after)
{
    char *stop = NULL;
    errno = 0;
    double n = strtod(p, &stop);
    if (stop == p || stop > end || errno == ERANGE || !isfinite(n))
        return false;
    if (stop > p && stop < end && stop[-1] == '.' && *stop == '.')
        stop--;
    *value = n;
    if (after)
        *after = stop;
    return true;
}
static pc_status choice(pc_ui_param *p, const char *start, const char *end, pc_error *err)
{
    if (p->nchoices >= 64)
        return pc_fail(err, PC_ERR_LIMIT, "too many parameter choices");
    char *value = pc_strndup(start, (size_t)(end - start));
    char **choices = realloc(p->choices, (p->nchoices + 1) * sizeof *choices);
    if (!value || !choices) {
        free(value);
        if (choices)
            p->choices = choices;
        return pc_fail(err, PC_ERR_NOMEM, "parameter choices");
    }
    p->choices = choices;
    p->choices[p->nchoices++] = value;
    return PC_OK;
}
static pc_status parse_param(pc_ui_param *p, const char *start, const char *end, pc_error *err)
{
    const char *v = space(start, end);
    p->kind = PC_UI_PARAM_STRUCTURED;
    const char *equals = NULL;
    /* Only a leading choice's closing quote belongs to the notation; suffix comments are ignored. */
    if (v < end && *v == '"') {
        const char *close = memchr(v + 1, '"', (size_t)(end - v - 1));
        if (!close)
            return PC_OK;
        bool color = (size_t)(close - v - 1) == 7 && !memcmp(v + 1, "#rrggbb", 7);
        if (color) {
            p->kind = PC_UI_PARAM_COLOR;
            return PC_OK;
        }
        p->kind = PC_UI_PARAM_CHOICE;
        const char *part = v + 1;
        for (const char *q = part; q <= close; q++)
            if (q == close || *q == '|') {
                pc_status status = choice(p, part, q, err);
                if (status != PC_OK)
                    return status;
                part = q + 1;
            }
        equals = space(close + 1, end);
        if (equals < end && *equals == '=') {
            const char *d = space(equals + 1, end);
            char quote = d < end && (*d == '"' || *d == '\'') ? *d++ : 0;
            const char *finish = d;
            while (finish < end &&
                   (quote ? *finish != quote
                          : !isspace((unsigned char)*finish) && *finish != ')' && *finish != '('))
                finish++;
            for (size_t i = 0; i < p->nchoices; i++)
                if (strlen(p->choices[i]) == (size_t)(finish - d) &&
                    !memcmp(p->choices[i], d, (size_t)(finish - d))) {
                    p->text = pc_strndup(d, (size_t)(finish - d));
                    if (!p->text)
                        return pc_fail(err, PC_ERR_NOMEM, "parameter default");
                    p->has_default = true;
                    break;
                }
        }
        if (!p->has_default && p->nchoices) {
            p->text = pc_strndup(p->choices[0], strlen(p->choices[0]));
            if (!p->text)
                return pc_fail(err, PC_ERR_NOMEM, "parameter default");
            p->has_default = true;
        }
        return PC_OK;
    }
    if (end - v >= 4 && !memcmp(v, "bool", 4)) {
        p->kind = PC_UI_PARAM_BOOL;
        p->has_default = true;
        const char *d = space(v + 4, end);
        if (d < end && *d == '=') {
            d = space(d + 1, end);
            p->boolean = end - d >= 4 && !memcmp(d, "true", 4);
        }
        return PC_OK;
    }
    if (end - v >= 5 && !memcmp(v, "color", 5)) {
        p->kind = PC_UI_PARAM_COLOR;
        const char *d = space(v + 5, end);
        if (d < end && *d == '=') {
            d = space(d + 1, end);
            if (end - d >= 7 && *d == '#') {
                p->text = pc_strndup(d, 7);
                if (!p->text)
                    return pc_fail(err, PC_ERR_NOMEM, "color default");
                p->has_default = true;
            }
        }
        return PC_OK;
    }
    if (end - v >= 3 && !memcmp(v, "str", 3)) {
        p->kind = PC_UI_PARAM_TEXT;
        const char *d = space(v + 3, end);
        if (d < end && *d == '=') {
            d = space(d + 1, end);
            char quote = d < end && (*d == '"' || *d == '\'') ? *d++ : 0;
            const char *finish = d;
            while (finish < end &&
                   (quote ? *finish != quote : !isspace((unsigned char)*finish) && *finish != '('))
                finish++;
            p->text = pc_strndup(d, (size_t)(finish - d));
            if (!p->text)
                return pc_fail(err, PC_ERR_NOMEM, "text default");
            p->has_default = true;
        }
        return PC_OK;
    }
    if ((end - v >= 5 && !memcmp(v, "float", 5)) ||
        (end - v >= 3 && (!memcmp(v, "f32", 3) || !memcmp(v, "f64", 3) || !memcmp(v, "deg", 3)))) {
        p->kind = PC_UI_PARAM_RANGE;
        p->min = end - v >= 3 && !memcmp(v, "deg", 3) ? -360 : -1e9;
        p->max = -p->min;
        p->has_default = true;
        const char *d = memchr(v, '=', (size_t)(end - v));
        if (d)
            (void)numeric(space(d + 1, end), end, &p->number, NULL);
        return PC_OK;
    }
    if (end - v >= 4 && !memcmp(v, "text", 4)) {
        p->kind = PC_UI_PARAM_TEXT;
        return PC_OK;
    }
    if (end - v >= 3 && !memcmp(v, "doc", 3)) {
        p->kind = PC_UI_PARAM_DOCUMENT;
        return PC_OK;
    }
    if (end - v >= 4 && !memcmp(v, "int[", 4)) {
        double count = 0;
        const char *after;
        if (numeric(v + 4, end, &count, &after) && after < end && *after == ']' && count >= 1 &&
            count <= 4096 && floor(count) == count) {
            p->kind = PC_UI_PARAM_GRID;
            p->grid_size = (size_t)count;
        }
        return PC_OK;
    }
    const char *after = NULL;
    double lower = 0, upper = 0;
    if (numeric(v, end, &lower, &after) && end - after >= 2 && !memcmp(after, "..", 2) &&
        numeric(after + 2, end, &upper, &after) && lower <= upper && fabs(lower) <= 1e12 &&
        fabs(upper) <= 1e12) {
        p->kind = PC_UI_PARAM_RANGE;
        p->min = lower;
        p->max = upper;
        p->number = lower;
        p->has_default = true;
        after = space(after, end);
        if (after < end && *after == '=') {
            double def;
            if (numeric(space(after + 1, end), end, &def, NULL))
                p->number = fmin(upper, fmax(lower, def));
        }
        return PC_OK;
    }
    if ((end - v >= 3 &&
         (!memcmp(v, "u32", 3) || !memcmp(v, "i32", 3) || !memcmp(v, "u64", 3) || !memcmp(v, "i64", 3))) ||
        (end - v >= 2 && !memcmp(v, "px", 2)) || (end - v >= 3 && !memcmp(v, "int", 3))) {
        p->kind = PC_UI_PARAM_INTEGER;
        p->min = *v == 'u' ? 0 : -2147483647;
        p->max = *v == 'u' ? 4294967295.0 : 2147483647;
        p->has_default = true;
        equals = memchr(v, '=', (size_t)(end - v));
        if (equals)
            (void)numeric(space(equals + 1, end), end, &p->number, NULL);
    }
    return PC_OK;
}
pc_status pc_ui_parameter_schema_parse(pc_ui_parameter_schema *out, const char *spec, pc_error *err)
{
    pc_ui_parameter_schema next = {0};
    if (!spec)
        spec = "{}";
    size_t length = strnlen(spec, 65537);
    if (length > 65536)
        return pc_fail(err, PC_ERR_LIMIT, "parameter specification too long");
    const char *end = spec + length, *p = space(spec, end);
    if (p == end || *p != '{')
        return pc_fail(err, PC_ERR_INVALID, "parameter specification needs an object");
    p++;
    pc_status status = PC_OK;
    while (p < end) {
        p = space(p, end);
        if (p == end || *p == '}')
            break;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') {
            status = pc_fail(err, PC_ERR_INVALID, "invalid parameter key");
            break;
        }
        const char *key = ++p, *close = memchr(p, '"', (size_t)(end - p));
        if (!close) {
            status = pc_fail(err, PC_ERR_INVALID, "unterminated parameter key");
            break;
        }
        p = space(close + 1, end);
        if (p == end || *p != ':') {
            status = pc_fail(err, PC_ERR_INVALID, "missing parameter colon");
            break;
        }
        const char *value = ++p;
        size_t depth = 0;
        bool quoted = false, escaped = false;
        for (; p < end; p++) {
            char c = *p;
            if (quoted) {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    quoted = false;
                continue;
            }
            if (c == '"') {
                quoted = true;
                continue;
            }
            if (c == '[' || c == '{') {
                if (++depth > 64) {
                    status = pc_fail(err, PC_ERR_LIMIT, "parameter nesting too deep");
                    break;
                }
            } else if (c == ']' || c == '}') {
                if (!depth)
                    break;
                depth--;
            } else if (c == ',' && !depth)
                break;
        }
        if (status != PC_OK)
            break;
        if ((size_t)(close - key) == 5 && !memcmp(key, "layer", 5))
            continue;
        if (next.count >= 128) {
            status = pc_fail(err, PC_ERR_LIMIT, "too many parameters");
            break;
        }
        pc_ui_param *items = realloc(next.items, (next.count + 1) * sizeof *items);
        if (!items) {
            status = pc_fail(err, PC_ERR_NOMEM, "parameter schema");
            break;
        }
        next.items = items;
        pc_ui_param *item = &items[next.count++];
        memset(item, 0, sizeof *item);
        item->key = pc_strndup(key, (size_t)(close - key));
        if (!item->key) {
            status = pc_fail(err, PC_ERR_NOMEM, "parameter key");
            break;
        }
        status = parse_param(item, value, p, err);
        if (status != PC_OK)
            break;
    }
    if (status != PC_OK) {
        pc_ui_parameter_schema_fini(&next);
        return status;
    }
    pc_ui_parameter_schema_fini(out);
    *out = next;
    return PC_OK;
}
pc_status pc_ui_parameter_schema_defaults(const pc_ui_parameter_schema *s, yyjson_mut_doc *d,
                                          yyjson_mut_val *o, pc_error *err)
{
    if (!yyjson_mut_is_obj(o))
        return pc_fail(err, PC_ERR_INVALID, "parameter defaults need an object");
    for (size_t i = 0; i < s->count; i++) {
        const pc_ui_param *p = &s->items[i];
        if (!p->has_default || yyjson_mut_obj_get(o, p->key))
            continue;
        yyjson_mut_val *value =
            p->kind == PC_UI_PARAM_BOOL ? yyjson_mut_bool(d, p->boolean)
            : p->text                   ? yyjson_mut_strcpy(d, p->text)
            : p->kind == PC_UI_PARAM_INTEGER
                ? yyjson_mut_sint(d, (int64_t)fmin(p->max, fmax(p->min, round(p->number))))
                : yyjson_mut_real(d, p->number);
        yyjson_mut_val *key = yyjson_mut_strcpy(d, p->key);
        if (!value || !key || !yyjson_mut_obj_add(o, key, value))
            return pc_fail(err, PC_ERR_NOMEM, "parameter defaults");
    }
    return PC_OK;
}
