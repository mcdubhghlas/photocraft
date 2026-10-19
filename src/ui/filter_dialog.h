#ifndef PC_UI_FILTER_DIALOG_H
#define PC_UI_FILTER_DIALOG_H
#include "engine/engine.h"
#include "ui/parameter_schema.h"
bool pc_ui_filter_dialog_command(const char *command);
pc_status pc_ui_filter_dialog_fields_schema(const pc_engine_session *session, const char *command,
                                            const char *schema_text, yyjson_mut_doc **out, pc_error *err);
pc_status pc_ui_filter_dialog_fields(const pc_engine_session *session, const char *command,
                                     yyjson_mut_doc **out, pc_error *err);
pc_status pc_ui_filter_dialog_guard(const pc_engine_session *session, const yyjson_mut_val *fields,
                                    pc_error *err);
pc_status pc_ui_filter_dialog_params(const yyjson_mut_val *fields, pc_buf *out, pc_error *err);
#endif
