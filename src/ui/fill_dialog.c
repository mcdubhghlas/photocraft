#include "ui/fill_dialog.h"
#include "ui/color_picker.h"

const char *const PC_UI_FILL_CONTENTS[9] = {"foreground", "background", "color", "contentAware", "pattern",
                                            "history",    "black",      "gray",  "white"};
const char *const PC_UI_FILL_CONTENT_LABELS[9] = {
    "Foreground Color", "Background Color", "Color…",    "Content-Aware", "Pattern",
    "History",          "Black",            "50% Gray", "White"};

yyjson_mut_val *pc_ui_fill_default_fields(yyjson_mut_doc *doc, const float foreground[4])
{
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) {
        return NULL;
    }
    char hex[8] = "#000000";
    if (foreground) {
        float rgb[3] = {foreground[0], foreground[1], foreground[2]};
        pc_ui_picker_hex(rgb, hex);
    }
    bool ok = yyjson_mut_obj_add_strcpy(doc, root, "contents", "foreground") &&
              yyjson_mut_obj_add_strcpy(doc, root, "color", hex) &&
              yyjson_mut_obj_add_bool(doc, root, "colorAdaptation", true) &&
              yyjson_mut_obj_add_strcpy(doc, root, "mode", "Normal") &&
              yyjson_mut_obj_add_real(doc, root, "opacity", 100.0) &&
              yyjson_mut_obj_add_bool(doc, root, "preserveTransparency", false);
    return ok ? root : NULL;
}
