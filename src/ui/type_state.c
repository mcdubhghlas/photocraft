#include "type_state.h"
#include <string.h>
void pc_ui_text_edit_fini(pc_ui_text_edit *edit)
{
    if (edit) {
        pc_str_fini(&edit->session);
        memset(edit, 0, sizeof *edit);
    }
}
pc_status pc_ui_text_edit_to_json(const pc_ui_text_edit *edit, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                  pc_error *err)
{
    yyjson_mut_val *value = yyjson_mut_obj(doc);
    if (!value || !yyjson_mut_obj_add_uint(doc, value, "layer", edit->layer) ||
        !yyjson_mut_obj_add_uint(doc, value, "caret", edit->caret) ||
        !yyjson_mut_obj_add_uint(doc, value, "anchor", edit->anchor) ||
        !yyjson_mut_obj_add_strcpy(doc, value, "session", pc_str_c(&edit->session)) ||
        !yyjson_mut_obj_add_bool(doc, value, "created", edit->created) ||
        !yyjson_mut_obj_add_bool(doc, value, "mask", edit->mask)) {
        return pc_fail(err, PC_ERR_NOMEM, "text edit state");
    }
    *out = value;
    return PC_OK;
}
pc_status pc_ui_text_edit_from_json(pc_ui_text_edit *out, yyjson_val *object, pc_error *err)
{
    pc_ui_text_edit next = {0};
    uint64_t caret = 0, anchor = 0;
    size_t len = 0;
    const char *session = pc_json_as_str(pc_json_get(object, "session"), &len);
    if (!yyjson_is_obj(object) || !pc_json_as_u64(pc_json_get(object, "layer"), &next.layer) || !next.layer ||
        !pc_json_as_u64(pc_json_get(object, "caret"), &caret) || caret > SIZE_MAX ||
        !pc_json_as_u64(pc_json_get(object, "anchor"), &anchor) || anchor > SIZE_MAX || !session || !len ||
        len > 4096) {
        return pc_fail(err, PC_ERR_INVALID, "invalid text edit state");
    }
    yyjson_val *created = pc_json_get(object, "created"), *mask = pc_json_get(object, "mask");
    if ((created && !pc_json_as_bool(created, &next.created)) ||
        (mask && !pc_json_as_bool(mask, &next.mask))) {
        return pc_fail(err, PC_ERR_INVALID, "invalid text edit flags");
    }
    pc_status status = pc_str_set(&next.session, session, len, err);
    if (status != PC_OK) {
        return status;
    }
    next.caret = (size_t)caret;
    next.anchor = (size_t)anchor;
    pc_ui_text_edit_fini(out);
    *out = next;
    return PC_OK;
}
