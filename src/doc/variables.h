/* Image > Variables and Data Sets (port of crates/doc/src/variables.rs). Pure data: loading a
 * replacement image is the engine's job. Conventions: doc/adjust.h. */
#ifndef PC_DOC_VARIABLES_H
#define PC_DOC_VARIABLES_H

#include "doc/adjust.h"

typedef uint64_t pc_layer_id;

typedef enum pc_var_type { PC_VAR_VISIBILITY, PC_VAR_TEXT_REPLACEMENT, PC_VAR_PIXEL_REPLACEMENT } pc_var_type;
typedef enum pc_pixel_method { PC_PIXEL_FIT, PC_PIXEL_FILL, PC_PIXEL_AS_IS, PC_PIXEL_CONFORM } pc_pixel_method;
typedef enum pc_pixel_align {
    PC_ALIGN_TOP_LEFT,
    PC_ALIGN_TOP_CENTER,
    PC_ALIGN_TOP_RIGHT,
    PC_ALIGN_CENTER_LEFT,
    PC_ALIGN_CENTER, /* default */
    PC_ALIGN_CENTER_RIGHT,
    PC_ALIGN_BOTTOM_LEFT,
    PC_ALIGN_BOTTOM_CENTER,
    PC_ALIGN_BOTTOM_RIGHT
} pc_pixel_align;

/* method/align/clip are meaningful for PIXEL_REPLACEMENT only. */
typedef struct pc_var_kind {
    pc_var_type type;
    pc_pixel_method method;
    pc_pixel_align align;
    bool clip;
} pc_var_kind;

typedef struct pc_variable_def {
    pc_str name;
    pc_layer_id layer;
    pc_var_kind kind;
} pc_variable_def;

typedef enum pc_data_value_kind { PC_DATA_VISIBILITY, PC_DATA_TEXT, PC_DATA_PIXELS } pc_data_value_kind;

typedef struct pc_data_value {
    pc_str variable;
    pc_data_value_kind kind;
    bool visibility; /* VISIBILITY */
    pc_str text;     /* TEXT: the string; PIXELS: the image path */
} pc_data_value;

typedef struct pc_data_set {
    pc_str name;
    pc_data_value *values;
    size_t nvalues;
} pc_data_set;

typedef struct pc_variables {
    pc_variable_def *defs;
    size_t ndefs;
    pc_data_set *data_sets;
    size_t ndata_sets;
    bool has_active;
    size_t active;
} pc_variables;

void pc_variable_def_fini(pc_variable_def *d);
pc_status pc_variable_def_clone(const pc_variable_def *d, pc_variable_def *out, pc_error *err);
bool pc_variable_def_eq(const pc_variable_def *a, const pc_variable_def *b);
void pc_data_value_fini(pc_data_value *v);
pc_status pc_data_value_clone(const pc_data_value *v, pc_data_value *out, pc_error *err);
bool pc_data_value_eq(const pc_data_value *a, const pc_data_value *b);
void pc_data_set_fini(pc_data_set *s);
pc_status pc_data_set_clone(const pc_data_set *s, pc_data_set *out, pc_error *err);
bool pc_data_set_eq(const pc_data_set *a, const pc_data_set *b);
void pc_variables_fini(pc_variables *v);
pc_status pc_variables_clone(const pc_variables *v, pc_variables *out, pc_error *err);
bool pc_variables_eq(const pc_variables *a, const pc_variables *b);

bool pc_variables_is_empty(const pc_variables *v);
const pc_variable_def *pc_variables_def(const pc_variables *v, const char *name, size_t len);
const pc_data_set *pc_variables_data_set(const pc_variables *v, const char *name, size_t len);
bool pc_variables_data_set_index(const pc_variables *v, const char *name, size_t len, size_t *out);
const pc_data_value *pc_data_set_get(const pc_data_set *s, const char *variable, size_t len);

#endif
