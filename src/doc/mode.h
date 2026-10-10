/* Colour-mode side data: the Indexed Color table, Duotone inks and smart-object stack modes
 * (port of crates/doc/src/mode.rs). Conventions: doc/adjust.h. */
#ifndef PC_DOC_MODE_H
#define PC_DOC_MODE_H

#include "doc/adjust.h"

/* Image > Mode > Color Table (at most 256 sRGB 8-bit entries). */
typedef struct pc_color_table {
    uint8_t (*colors)[3];
    size_t ncolors;
    bool has_transparent;
    uint8_t transparent;
} pc_color_table;

void pc_color_table_fini(pc_color_table *t);
pc_status pc_color_table_clone(const pc_color_table *t, pc_color_table *out, pc_error *err);
bool pc_color_table_eq(const pc_color_table *a, const pc_color_table *b);
/* Nearest entry for an RGB colour in 0..=1 (squared distance, ties to the lower index; 0 when
 * the table is empty). */
size_t pc_color_table_nearest(const pc_color_table *t, const float c[3]);
/* Entry i as RGB 0..=1 (black past the end). */
void pc_color_table_rgb(const pc_color_table *t, size_t i, float out[3]);

/* One Duotone ink: display colour and gray-density -> ink-density curve. */
typedef struct pc_duotone_ink {
    pc_str name;
    float color[3];
    pc_curve curve;
} pc_duotone_ink;

/* An ink with the identity curve. */
pc_status pc_duotone_ink_new(const char *name, const float color[3], pc_duotone_ink *out, pc_error *err);
void pc_duotone_ink_fini(pc_duotone_ink *i);
pc_status pc_duotone_ink_clone(const pc_duotone_ink *i, pc_duotone_ink *out, pc_error *err);
bool pc_duotone_ink_eq(const pc_duotone_ink *a, const pc_duotone_ink *b);
float pc_duotone_ink_density(const pc_duotone_ink *i, float gray_density);

typedef struct pc_duotone {
    pc_duotone_ink *inks;
    size_t ninks;
    pc_blob *psd_raw; /* Option<Vec<u8>>: NULL = None; an empty blob is Some(empty) */
} pc_duotone;

void pc_duotone_fini(pc_duotone *d);
pc_status pc_duotone_clone(const pc_duotone *d, pc_duotone *out, pc_error *err);
bool pc_duotone_eq(const pc_duotone *a, const pc_duotone *b);
/* Simulated print colour of a gray value (0 black, 1 white). */
void pc_duotone_render(const pc_duotone *d, float gray, float out[3]);
/* The 33-stop Gradient Map showing the printed look. */
pc_status pc_duotone_display_adjustment(const pc_duotone *d, pc_adjustment *out, pc_error *err);
/* Piecewise-linear curve evaluation used by the inks (empty: identity; one point: constant). */
float pc_duotone_curve_eval(const pc_curve_point *pts, size_t n, float x);

typedef enum pc_stack_mode {
    PC_STACK_ENTROPY,
    PC_STACK_KURTOSIS,
    PC_STACK_MAXIMUM,
    PC_STACK_MEAN,
    PC_STACK_MEDIAN,
    PC_STACK_MINIMUM,
    PC_STACK_RANGE,
    PC_STACK_SKEWNESS,
    PC_STACK_STANDARD_DEVIATION,
    PC_STACK_SUMMATION,
    PC_STACK_VARIANCE
} pc_stack_mode;
#define PC_STACK_MODE_COUNT 11 /* enum order = the reference's ALL */

const char *pc_stack_mode_id(pc_stack_mode m);    /* "standardDeviation" */
const char *pc_stack_mode_label(pc_stack_mode m); /* "Standard Deviation" */
bool pc_stack_mode_from_id(const char *id, size_t len, pc_stack_mode *out);

#endif
