#include "preset_manager.h"

#include "base/mem.h"
#include <stdlib.h>
#include <string.h>

static const pc_ui_preset_kind KINDS[] = {
    {"brushes", "Brushes"},
    {"customShapes", "Custom Shapes"},
    {"patterns", "Patterns"},
};

size_t pc_ui_preset_kinds(const pc_ui_preset_kind **out)
{
    if (out != NULL) {
        *out = KINDS;
    }
    return sizeof KINDS / sizeof KINDS[0];
}

bool pc_ui_preset_kind_valid(const char *id)
{
    if (id == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++) {
        if (strcmp(id, KINDS[i].id) == 0) {
            return true;
        }
    }
    return false;
}

pc_status pc_ui_preset_list_parse(yyjson_mut_val *reply, const char *kind, char ***out, size_t *n,
                                  pc_error *err)
{
    if (out == NULL || n == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "preset list: missing output");
    }
    *out = NULL;
    *n = 0;
    if (reply == NULL || kind == NULL) {
        return PC_OK;
    }
    yyjson_mut_val *arr = yyjson_mut_obj_get(reply, kind);
    if (arr == NULL || !yyjson_mut_is_arr(arr)) {
        return PC_OK; /* a missing/odd field is an empty library, not an error */
    }
    size_t count = yyjson_mut_arr_size(arr);
    if (count == 0) {
        return PC_OK;
    }
    char **names = calloc(count, sizeof *names);
    if (names == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "preset list");
    }
    size_t filled = 0;
    for (size_t i = 0; i < count; i++) {
        const char *name = yyjson_mut_get_str(yyjson_mut_arr_get(arr, i));
        if (name == NULL) {
            name = "";
        }
        char *dup = pc_strndup(name, strlen(name));
        if (dup == NULL) {
            pc_ui_preset_names_free(names, filled);
            return pc_fail(err, PC_ERR_NOMEM, "preset name");
        }
        names[filled++] = dup;
    }
    *out = names;
    *n = filled;
    return PC_OK;
}

void pc_ui_preset_names_free(char **names, size_t n)
{
    if (names == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
}

size_t pc_ui_preset_clamp(size_t selected, size_t count)
{
    if (count == 0) {
        return 0;
    }
    if (selected >= count) {
        return count - 1;
    }
    return selected;
}

bool pc_ui_preset_can_move_up(size_t selected, size_t count)
{
    return selected > 0 && selected < count;
}

bool pc_ui_preset_can_move_down(size_t selected, size_t count)
{
    return count > 0 && selected + 1 < count;
}
