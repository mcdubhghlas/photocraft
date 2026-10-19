#include "type_panels.h"
#include <math.h>
#include <stdlib.h>
#include "base/mem.h"
#include "base/utf8.h"
#include <string.h>
void pc_ui_type_panels_fini(pc_ui_type_panels *s)
{
    if (!s) {
        return;
    }
    pc_str_fini(&s->glyph_family);
    pc_str_fini(&s->glyph_style);
    pc_str_fini(&s->glyph_category);
    pc_strlist_fini(&s->glyph_recent);
    pc_str_fini(&s->glyph_selected);
    pc_json_value_fini(&s->spell.items);
    pc_strlist_fini(&s->spell.ignore);
    pc_str_fini(&s->spell.change_to);
    pc_str_fini(&s->spell.message);
    memset(s, 0, sizeof *s);
}
static yyjson_mut_val *list_json(yyjson_mut_doc *doc, const pc_strlist *list)
{
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    if (!array) {
        return NULL;
    }
    for (size_t i = 0; i < list->n; i++) {
        if (!yyjson_mut_arr_add_strcpy(doc, array, list->v[i])) {
            return NULL;
        }
    }
    return array;
}
pc_status pc_ui_type_panels_to_json(const pc_ui_type_panels *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                    pc_error *err)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc), *options = yyjson_mut_null(doc), *spell = yyjson_mut_null(doc);
    if (s->has_options) {
        options = yyjson_mut_arr(doc);
        if (!options || !yyjson_mut_arr_add_bool(doc, options, s->options_paragraph) ||
            !yyjson_mut_arr_add_uint(doc, options, s->options_id)) {
            return pc_fail(err, PC_ERR_NOMEM, "type panel options");
        }
    }
    if (s->has_spell) {
        spell = yyjson_mut_obj(doc);
        yyjson_mut_val *items = s->spell.items.doc
                                    ? yyjson_mut_val_mut_copy(doc, pc_json_value_root(&s->spell.items))
                                    : yyjson_mut_arr(doc);
        if (!spell || !items || !yyjson_mut_obj_add_val(doc, spell, "items", items) ||
            !yyjson_mut_obj_add_uint(doc, spell, "index", s->spell.index) ||
            !yyjson_mut_obj_add_val(doc, spell, "ignore", list_json(doc, &s->spell.ignore)) ||
            !yyjson_mut_obj_add_strcpy(doc, spell, "change_to", pc_str_c(&s->spell.change_to)) ||
            !yyjson_mut_obj_add_strcpy(doc, spell, "message", pc_str_c(&s->spell.message)) ||
            !yyjson_mut_obj_add_bool(doc, spell, "all_layers", s->spell.all_layers)) {
            return pc_fail(err, PC_ERR_NOMEM, "spelling state");
        }
    }
    if (!o || !yyjson_mut_obj_add_bool(doc, o, "styles", s->styles) ||
        !yyjson_mut_obj_add_uint(doc, o, "styles_tab", s->styles_tab) ||
        !yyjson_mut_obj_add_bool(doc, o, "glyphs", s->glyphs) ||
        !yyjson_mut_obj_add_val(doc, o, "options", options) ||
        !yyjson_mut_obj_add_strcpy(doc, o, "glyph_family", pc_str_c(&s->glyph_family)) ||
        !yyjson_mut_obj_add_strcpy(doc, o, "glyph_style", pc_str_c(&s->glyph_style)) ||
        !yyjson_mut_obj_add_strcpy(doc, o, "glyph_category", pc_str_c(&s->glyph_category)) ||
        !yyjson_mut_obj_add_real(doc, o, "glyph_size", s->glyph_size) ||
        !yyjson_mut_obj_add_val(doc, o, "glyph_recent", list_json(doc, &s->glyph_recent)) ||
        !yyjson_mut_obj_add_val(doc, o, "glyph_selected",
                                s->has_glyph_selected ? yyjson_mut_strcpy(doc, pc_str_c(&s->glyph_selected))
                                                      : yyjson_mut_null(doc)) ||
        !yyjson_mut_obj_add_val(doc, o, "spell", spell)) {
        return pc_fail(err, PC_ERR_NOMEM, "type panel state");
    }
    *out = o;
    return PC_OK;
}
static pc_status boolean(yyjson_val *o, const char *key, bool *value, pc_error *err)
{
    yyjson_val *v = pc_json_get(o, key);
    return !v || pc_json_as_bool(v, value) ? PC_OK
                                           : pc_fail(err, PC_ERR_INVALID, "invalid type panel boolean");
}
static pc_status string(yyjson_val *o, const char *key, pc_str *value, pc_error *err)
{
    yyjson_val *v = pc_json_get(o, key);
    if (!v) {
        return PC_OK;
    }
    size_t length = 0;
    const char *text = pc_json_as_str(v, &length);
    return text && length <= 1048576 && !memchr(text, 0, length)
               ? pc_str_set(value, text, length, err)
               : pc_fail(err, PC_ERR_INVALID, "invalid type panel string");
}
static pc_status list_read(yyjson_val *o, const char *key, pc_strlist *list, size_t limit, pc_error *err)
{
    yyjson_val *v = pc_json_get(o, key);
    if (!v) {
        return PC_OK;
    }
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) > limit) {
        return pc_fail(err, PC_ERR_INVALID, "invalid type panel list");
    }
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(v, i, n, item)
    {
        size_t length = 0;
        const char *text = pc_json_as_str(item, &length);
        if (!text || length > 4096 || memchr(text, 0, length)) {
            return pc_fail(err, PC_ERR_INVALID, "invalid type panel list item");
        }
        pc_status status = pc_strlist_push(list, text, length, err);
        if (status != PC_OK) {
            return status;
        }
    }
    return PC_OK;
}
pc_status pc_ui_type_panels_from_json(pc_ui_type_panels *s, yyjson_val *o, pc_error *err)
{
    if (!yyjson_is_obj(o)) {
        return pc_fail(err, PC_ERR_INVALID, "type panels must be an object");
    }
    pc_ui_type_panels next = {0};
    pc_status status = boolean(o, "styles", &next.styles, err);
    if (status == PC_OK) {
        status = boolean(o, "glyphs", &next.glyphs, err);
    }
    uint64_t tab = 0;
    yyjson_val *v = pc_json_get(o, "styles_tab");
    if (v && (!pc_json_as_u64(v, &tab) || tab > 1)) {
        status = pc_fail(err, PC_ERR_INVALID, "invalid style tab");
    }
    next.styles_tab = (size_t)tab;
    v = pc_json_get(o, "options");
    if (v && !yyjson_is_null(v)) {
        uint64_t id = 0;
        if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 2 ||
            !pc_json_as_bool(yyjson_arr_get(v, 0), &next.options_paragraph) ||
            !pc_json_as_u64(yyjson_arr_get(v, 1), &id) || id > UINT32_MAX) {
            status = pc_fail(err, PC_ERR_INVALID, "invalid style options target");
        }
        next.has_options = true;
        next.options_id = (uint32_t)id;
    }
    const char *keys[] = {"glyph_family", "glyph_style", "glyph_category", "glyph_selected"};
    pc_str *strings[] = {&next.glyph_family, &next.glyph_style, &next.glyph_category, &next.glyph_selected};
    for (size_t i = 0; i < 4 && status == PC_OK; i++) {
        v = pc_json_get(o, keys[i]);
        if (i == 3 && (!v || yyjson_is_null(v))) {
            continue;
        }
        status = string(o, keys[i], strings[i], err);
        if (i == 3) {
            next.has_glyph_selected = true;
        }
    }
    double size = 0;
    v = pc_json_get(o, "glyph_size");
    if (v && (!pc_json_as_f64(v, &size) || !isfinite(size) || size < 0 || size > 512)) {
        status = pc_fail(err, PC_ERR_INVALID, "invalid glyph size");
    }
    next.glyph_size = status == PC_OK ? (float)size : 0;
    if (status == PC_OK) {
        status = list_read(o, "glyph_recent", &next.glyph_recent, 256, err);
    }
    yyjson_val *spell = pc_json_get(o, "spell");
    if (status == PC_OK && spell && !yyjson_is_null(spell)) {
        next.has_spell = true;
        if (!yyjson_is_obj(spell)) {
            status = pc_fail(err, PC_ERR_INVALID, "invalid spelling state");
        }
        if (status == PC_OK) {
            status = boolean(spell, "all_layers", &next.spell.all_layers, err);
        }
        if (status == PC_OK) {
            status = string(spell, "change_to", &next.spell.change_to, err);
        }
        if (status == PC_OK) {
            status = string(spell, "message", &next.spell.message, err);
        }
        if (status == PC_OK) {
            status = list_read(spell, "ignore", &next.spell.ignore, 4096, err);
        }
        uint64_t index = 0;
        v = pc_json_get(spell, "index");
        if (v && (!pc_json_as_u64(v, &index) || index > 100000)) {
            status = pc_fail(err, PC_ERR_INVALID, "invalid spelling index");
        }
        next.spell.index = (size_t)index;
        yyjson_val *items = pc_json_get(spell, "items");
        if (status == PC_OK && items) {
            if (!yyjson_is_arr(items) || yyjson_arr_size(items) > 100000) {
                status = pc_fail(err, PC_ERR_INVALID, "invalid spelling items");
            } else {
                status = pc_json_value_from_val(items, &next.spell.items, err);
            }
        }
    }
    if (status == PC_OK) {
        pc_ui_type_panels_fini(s);
        *s = next;
    } else {
        pc_ui_type_panels_fini(&next);
    }
    return status;
}
pc_status pc_ui_type_glyph_used(pc_ui_type_panels *state, const char *text, size_t length, pc_error *err)
{
    if (!state || state->glyph_recent.n > 256 || !text || !length || length > 4096 ||
        memchr(text, 0, length)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid glyph");
    }
    for (size_t offset = 0; offset < length;) {
        uint32_t scalar;
        if (!pc_utf8_next((const uint8_t *)text, length, &offset, &scalar)) {
            return pc_fail(err, PC_ERR_INVALID, "glyph is not UTF-8");
        }
    }
    pc_str selected = PC_STR_INIT;
    pc_status status = pc_str_set(&selected, text, length, err);
    if (status != PC_OK) {
        return status;
    }
    pc_strlist *recent = &state->glyph_recent;
    size_t existing = recent->n;
    for (size_t i = 0; i < recent->n; i++) {
        if (strlen(recent->v[i]) == length && !memcmp(recent->v[i], text, length)) {
            existing = i;
            break;
        }
    }
    char *copy = NULL;
    if (existing < recent->n) {
        copy = recent->v[existing];
        memmove(recent->v + 1, recent->v, existing * sizeof *recent->v);
    } else {
        copy = pc_strndup(text, length);
        if (!copy) {
            pc_str_fini(&selected);
            return pc_fail(err, PC_ERR_NOMEM, "recent glyph");
        }
        if (recent->n < 256) {
            size_t capacity = recent->n;
            char **items = pc_grow_array(recent->v, &capacity, recent->n + 1, sizeof *items);
            if (!items) {
                free(copy);
                pc_str_fini(&selected);
                return pc_fail(err, PC_ERR_NOMEM, "recent glyphs");
            }
            recent->v = items;
            recent->n++;
        } else {
            free(recent->v[255]);
        }
        memmove(recent->v + 1, recent->v, (recent->n - 1) * sizeof *recent->v);
    }
    recent->v[0] = copy;
    pc_str_fini(&state->glyph_selected);
    state->glyph_selected = selected;
    state->has_glyph_selected = true;
    return PC_OK;
}
