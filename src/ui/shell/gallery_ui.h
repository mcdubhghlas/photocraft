#ifndef PC_UI_SHELL_GALLERY_UI_H
#define PC_UI_SHELL_GALLERY_UI_H
#include "dialogs.h"
pc_status pc_ui_shell_gallery_init(pc_ui_shell_app *app, yyjson_mut_doc *fields, pc_error *err);
void pc_ui_shell_gallery_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
#endif
