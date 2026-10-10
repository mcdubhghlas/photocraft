/* Image > Analysis data stored with a document (measurement scale, Count groups, the Ruler line)
 * and Note annotations (port of crates/doc/src/analysis.rs). Conventions: doc/adjust.h. */
#ifndef PC_DOC_ANALYSIS_H
#define PC_DOC_ANALYSIS_H

#include "color/color.h"
#include "doc/adjust.h"

/* pixel_length pixels = logical_length units. Default 1 pixel = 1 "pixels". */
typedef struct pc_measurement_scale {
    double pixel_length;
    double logical_length;
    pc_str units;
} pc_measurement_scale;

pc_status pc_measurement_scale_default(pc_measurement_scale *out, pc_error *err);
void pc_measurement_scale_fini(pc_measurement_scale *s);
pc_status pc_measurement_scale_clone(const pc_measurement_scale *s, pc_measurement_scale *out, pc_error *err);
bool pc_measurement_scale_eq(const pc_measurement_scale *a, const pc_measurement_scale *b);
double pc_measurement_scale_factor(const pc_measurement_scale *s); /* 1 when pixel_length <= 0 */
bool pc_measurement_scale_is_default(const pc_measurement_scale *s);
/* Photoshop's Scale text, e.g. "1 pixels = 1.0000 pixels" (Rust formatting). */
pc_status pc_measurement_scale_describe(const pc_measurement_scale *s, pc_str *out, pc_error *err);

/* One Count tool group. Default: "Count Group 1", red, marker 1, label 8, visible. */
typedef struct pc_count_group {
    pc_str name;
    pc_color color;
    uint32_t marker_size;
    uint32_t label_size;
    bool visible;
    double (*points)[2];
    size_t npoints;
} pc_count_group;

pc_status pc_count_group_default(pc_count_group *out, pc_error *err);
void pc_count_group_fini(pc_count_group *g);
pc_status pc_count_group_clone(const pc_count_group *g, pc_count_group *out, pc_error *err);
bool pc_count_group_eq(const pc_count_group *a, const pc_count_group *b);

typedef struct pc_ruler {
    double start[2];
    double end[2];
    bool has_protractor;
    double protractor[2];
} pc_ruler;

bool pc_ruler_eq(const pc_ruler *a, const pc_ruler *b);

typedef struct pc_measurement {
    pc_measurement_scale scale;
    pc_count_group *count_groups;
    size_t ncount_groups;
    size_t active_count_group;
    bool has_ruler;
    pc_ruler ruler;
} pc_measurement;

pc_status pc_measurement_default(pc_measurement *out, pc_error *err);
void pc_measurement_fini(pc_measurement *m);
pc_status pc_measurement_clone(const pc_measurement *m, pc_measurement *out, pc_error *err);
bool pc_measurement_eq(const pc_measurement *a, const pc_measurement *b);
bool pc_measurement_is_empty(const pc_measurement *m); /* == default */
size_t pc_measurement_count_total(const pc_measurement *m);

/* A Note tool annotation (PSD Anno). Default colour (1, 1, 0.51). */
typedef struct pc_note {
    pc_str author;
    pc_color color;
    pc_str text;
    double position[2];
    double popup[4];
    bool open;
    pc_str modified; /* PDF date text, empty when unknown */
} pc_note;

void pc_note_default(pc_note *out); /* no allocation */
void pc_note_fini(pc_note *n);
pc_status pc_note_clone(const pc_note *n, pc_note *out, pc_error *err);
bool pc_note_eq(const pc_note *a, const pc_note *b);

#endif
