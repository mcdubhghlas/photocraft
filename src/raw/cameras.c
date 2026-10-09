/* Camera profiles measured by PhotoCraft, for raw formats that carry no colour description of
 * their own (port of crates/raw/src/cameras.rs; see it for how the Nikon D4 profile was
 * measured). A profile gives the image area, the saturation level, a ForwardMatrix and a default
 * tone curve. */
#include <stdlib.h>
#include <string.h>

#include "base/utf8.h"
#include "raw/internal.h"

static const pc_raw_mat3 D4_FORWARD = {{{0.644598, 0.0703598, 0.249262},
                                         {0.342249, 0.518929, 0.138822},
                                         {0.0568265, -0.269568, 1.03795}}};
static const pc_raw_mat3 D4_COLOR = {{{0.895936, -0.218092, -0.185988},
                                       {-1.09736, 2.06898, -0.0131889},
                                       {-0.26977, 0.393884, 0.687829}}};
static const float D4_TONE[31][2] = {
    {0.005524272f, 0.00285028f}, {0.006569503f, 0.00357083f}, {0.0078125f, 0.00447912f},
    {0.009290681f, 0.00605813f}, {0.01104854f, 0.00841445f},  {0.01313901f, 0.011536f},
    {0.015625f, 0.0165399f},     {0.01858136f, 0.0230234f},   {0.02209709f, 0.0319205f},
    {0.02627801f, 0.0434858f},   {0.03125f, 0.0582138f},      {0.03716272f, 0.0771963f},
    {0.04419417f, 0.0993894f},   {0.05255603f, 0.125058f},    {0.0625f, 0.157207f},
    {0.07432544f, 0.194929f},    {0.08838835f, 0.236109f},    {0.1051121f, 0.279155f},
    {0.125f, 0.329365f},         {0.1486509f, 0.388405f},     {0.1767767f, 0.457944f},
    {0.2102241f, 0.52456f},      {0.25f, 0.598051f},          {0.2973018f, 0.668794f},
    {0.3535534f, 0.749954f},     {0.4204482f, 0.815443f},     {0.5f, 0.870167f},
    {0.5946036f, 0.907869f},     {0.70710677f, 0.939351f}, /* FRAC_1_SQRT_2 as f32 */
    {0.8408964f, 0.970567f},     {1.0f, 1.0f}};

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
} profile;

static const profile PROFILES[] = {
    {"NIKON CORPORATION", "NIKON D4", 4992, 3292, 8, 6, 4928, 3280, 15520.0f, true, &D4_FORWARD, &D4_COLOR, D4_TONE,
     31},
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
        return PC_OK;
    }
    return PC_OK;
}
