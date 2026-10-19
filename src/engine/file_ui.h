/* Shell file-menu services. Paths come from an explicit UI/automation request. */
#ifndef PC_ENGINE_FILE_UI_H
#define PC_ENGINE_FILE_UI_H
#include "engine.h"
bool pc_engine_file_can_write_back(const char *path);
pc_status pc_engine_file_save_ui(pc_engine_session *session, yyjson_val *params, bool save_as,
                                 pc_json_value *out, pc_error *err);
#endif
