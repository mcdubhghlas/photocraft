#ifndef PC_UI_PARAMETER_SCHEMA_H
#define PC_UI_PARAMETER_SCHEMA_H
#include "base/json.h"
typedef enum pc_ui_param_kind {
    PC_UI_PARAM_RANGE,
    PC_UI_PARAM_INTEGER,
    PC_UI_PARAM_BOOL,
    PC_UI_PARAM_CHOICE,
    PC_UI_PARAM_TEXT,
    PC_UI_PARAM_COLOR,
    PC_UI_PARAM_DOCUMENT,
    PC_UI_PARAM_GRID,
    PC_UI_PARAM_STRUCTURED
} pc_ui_param_kind;
typedef struct pc_ui_param {
    char *key;
    pc_ui_param_kind kind;
    double min, max, number;
    bool boolean, has_default;
    char **choices;
    size_t nchoices, grid_size;
    char *text;
} pc_ui_param;
typedef struct pc_ui_parameter_schema {
    pc_ui_param *items;
    size_t count;
} pc_ui_parameter_schema;
void pc_ui_parameter_schema_fini(pc_ui_parameter_schema *schema);
pc_status pc_ui_parameter_schema_parse(pc_ui_parameter_schema *schema, const char *spec, pc_error *err);
pc_status pc_ui_parameter_schema_defaults(const pc_ui_parameter_schema *schema, yyjson_mut_doc *doc,
                                          yyjson_mut_val *object, pc_error *err);
#endif
