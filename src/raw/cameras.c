/* Camera profiles measured by PhotoCraft, for raw formats that carry no colour description of
 * their own (port of crates/raw/src/cameras.rs; see it for how the Nikon D4 profile was
 * measured). A profile gives the image area, the saturation level, a ForwardMatrix, a default
 * tone curve and Camera Raw's black-point factor. Bodies whose full profile is not measured yet can
 * still carry a measured image area (IMAGE_AREAS), so their frames crop like the camera's JPEG. */
#include <stdlib.h>
#include <string.h>

#include "base/utf8.h"
#include "raw/internal.h"

static const pc_raw_mat3 D4_FORWARD = {
    {{0.639454, 0.260359, 0.0643991}, {0.273094, 0.796492, -0.0695779}, {0.00965321, -0.131427, 0.94699}}};
/* A third of the 0.1th percentile of the luminance, as Camera Raw renders D4 NEFs by default. */
#define D4_BLACK_POINT 0.33f
static const pc_raw_mat3 D4_COLOR = {{{0.895936, -0.218092, -0.185988},
                                       {-1.09736, 2.06898, -0.0131889},
                                       {-0.26977, 0.393884, 0.687829}}};
/* FRAC_1_SQRT_2 as f32 is 0.70710677. */
static const float D4_TONE[53][2] = {{0.00012207031f, 7.44377e-05f},
                                     {0.00014516688f, 8.44049e-05f},
                                     {0.00017263349f, 9.57067e-05f},
                                     {0.00020529698f, 0.000108523f},
                                     {0.00024414062f, 0.000123055f},
                                     {0.00029033376f, 0.000123202f},
                                     {0.00034526698f, 0.000123348f},
                                     {0.00041059396f, 0.000142387f},
                                     {0.00048828125f, 0.000164366f},
                                     {0.0005806675f, 0.00020246f},
                                     {0.00069053395f, 0.000249382f},
                                     {0.0008211879f, 0.000310083f},
                                     {0.0009765625f, 0.000385559f},
                                     {0.001161335f, 0.000474115f},
                                     {0.0013810679f, 0.00058301f},
                                     {0.0016423758f, 0.000717142f},
                                     {0.001953125f, 0.000882133f},
                                     {0.00232267f, 0.00109054f},
                                     {0.0027621358f, 0.00134818f},
                                     {0.0032847517f, 0.00163655f},
                                     {0.00390625f, 0.0019866f},
                                     {0.00464534f, 0.00235945f},
                                     {0.0055242716f, 0.00280228f},
                                     {0.0065695033f, 0.00342616f},
                                     {0.0078125f, 0.00418894f},
                                     {0.00929068f, 0.00540942f},
                                     {0.011048543f, 0.0069855f},
                                     {0.013139007f, 0.00959286f},
                                     {0.015625f, 0.0131734f},
                                     {0.01858136f, 0.0181471f},
                                     {0.022097087f, 0.0249987f},
                                     {0.026278013f, 0.0336416f},
                                     {0.03125f, 0.0452726f},
                                     {0.03716272f, 0.0591889f},
                                     {0.044194173f, 0.077383f},
                                     {0.052556027f, 0.0990869f},
                                     {0.0625f, 0.126878f},
                                     {0.07432544f, 0.161688f},
                                     {0.088388346f, 0.206049f},
                                     {0.10511205f, 0.256563f},
                                     {0.125f, 0.31946f},
                                     {0.14865088f, 0.382637f},
                                     {0.17677669f, 0.458308f},
                                     {0.2102241f, 0.531942f},
                                     {0.25f, 0.617405f},
                                     {0.29730177f, 0.683279f},
                                     {0.35355338f, 0.756182f},
                                     {0.4204482f, 0.809073f},
                                     {0.5f, 0.865664f},
                                     {0.59460354f, 0.900869f},
                                     {0.70710677f, 0.937507f},
                                     {0.8408964f, 0.956434f},
                                     {1.0f, 1.0f}};

typedef struct profile {
    const char *make;
    const char *model;
    size_t width, height;              /* sensor data size the profile was measured on */
    size_t cx, cy, cw, ch;             /* the image area */
    float white;
    bool black_zero;                   /* the data is black-subtracted in camera */
    const pc_raw_mat3 *forward;
    const pc_raw_mat3 *color;
    const float (*tone)[2];
    size_t ntone;
    float black_point; /* Camera Raw's adaptive black-point factor (pc_raw_sensor.black_point) */
} profile;

static const profile PROFILES[] = {
    {"NIKON CORPORATION", "NIKON D4", 4992, 3292, 8, 6, 4928, 3280, 15520.0f, true, &D4_FORWARD, &D4_COLOR,
     D4_TONE, 53, D4_BLACK_POINT},
};

/* A measured image area of a body whose remaining profile is not measured yet (aligned with the
 * camera's own JPEG; both origins even, so the Bayer phase is unchanged). */
typedef struct image_area {
    const char *make;
    const char *model;
    size_t width, height; /* sensor data size the measurement was taken on */
    size_t cx, cy, cw, ch;
} image_area;

static const image_area IMAGE_AREAS[] = {
    {"NIKON CORPORATION", "NIKON D5100", 4992, 3280, 8, 8, 4928, 3264},
    {"NIKON CORPORATION", "NIKON D800E", 7424, 4924, 8, 6, 7360, 4912},
    {"SONY", "ILCE-7", 6048, 4024, 12, 12, 6000, 4000},
};

const pc_raw_mat3 *rx_d4_forward(void) { return &D4_FORWARD; }

const float (*rx_d4_tone(size_t *n))[2]
{
    *n = sizeof D4_TONE / sizeof D4_TONE[0];
    return D4_TONE;
}

static bool trimmed_eq(const char *s, const char *want)
{
    size_t start;
    size_t n = pc_utf8_trim(s, strlen(s), &start);
    return n == strlen(want) && memcmp(s + start, want, n) == 0;
}

pc_status rx_cameras_apply(pc_raw_sensor *s, pc_error *err)
{
    size_t i;
    if (s->make == NULL || s->model == NULL) {
        return PC_OK;
    }
    for (i = 0; i < sizeof IMAGE_AREAS / sizeof IMAGE_AREAS[0]; i++) {
        const image_area *a = &IMAGE_AREAS[i];
        if (trimmed_eq(s->make, a->make) && trimmed_eq(s->model, a->model) && s->width == a->width &&
            s->height == a->height && pc_raw_rect_eq(s->crop, s->active)) {
            s->crop = pc_raw_rect_make(a->cx, a->cy, a->cw, a->ch);
            break;
        }
    }
    for (i = 0; i < sizeof PROFILES / sizeof PROFILES[0]; i++) {
        const profile *p = &PROFILES[i];
        float(*tone)[2];
        if (!trimmed_eq(s->make, p->make) || !trimmed_eq(s->model, p->model) || s->width != p->width ||
            s->height != p->height) {
            continue;
        }
        tone = malloc(p->ntone * sizeof *tone);
        if (tone == NULL) {
            return rx_nomem(err);
        }
        memcpy(tone, p->tone, p->ntone * sizeof *tone);
        s->crop = pc_raw_rect_make(p->cx, p->cy, p->cw, p->ch);
        s->white[0] = s->white[1] = s->white[2] = p->white;
        if (p->black_zero) {
            rx_strings_remove_containing(&s->warnings, "black level not recorded");
        }
        memset(&s->color, 0, sizeof s->color);
        s->color.ncalibrations = 1;
        /* D50 daylight: the matrices are consistent at the camera's daylight neutral. */
        s->color.calibrations[0].temperature = 5003.0;
        s->color.calibrations[0].color_matrix = *p->color;
        s->color.calibrations[0].has_forward_matrix = true;
        s->color.calibrations[0].forward_matrix = *p->forward;
        s->color.calibrations[0].camera_calibration = RX_IDENTITY;
        free(s->tone_curve);
        s->tone_curve = tone;
        s->ntone_curve = p->ntone;
        s->black_point = p->black_point;
        return PC_OK;
    }
    return PC_OK;
}
