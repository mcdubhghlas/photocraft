/* The hashes of tools/rustref/src/raw_vectors.rs (FNV-1a over little-endian fields), shared by
 * the raw parity tests. */
#ifndef PC_TESTS_RAW_RAWHASH_H
#define PC_TESTS_RAW_RAWHASH_H

#include <stdint.h>
#include <string.h>

#include "raw/raw.h"

#define H0 0xcbf29ce484222325ull

static inline void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}

static inline void u64le(uint64_t *h, uint64_t v)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    fnv(h, b, 8);
}

static inline void u16le(uint64_t *h, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    fnv(h, b, 2);
}

static inline void u32le(uint64_t *h, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fnv(h, b, 4);
}

static inline void f64b(uint64_t *h, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    u64le(h, u);
}

static inline void f32b(uint64_t *h, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    u32le(h, u);
}

static inline void str0(uint64_t *h, const char *s)
{
    fnv(h, s, strlen(s));
    fnv(h, "", 1);
}

static inline void mat(uint64_t *h, const pc_raw_mat3 *m)
{
    int i;
    for (i = 0; i < 9; i++) {
        f64b(h, m->m[i / 3][i % 3]);
    }
}

static inline void opt3(uint64_t *h, bool has, const double *v, int n)
{
    uint8_t one = 1;
    uint8_t zero = 0;
    int i;
    if (has) {
        fnv(h, &one, 1);
        for (i = 0; i < n; i++) {
            f64b(h, v[i]);
        }
    } else {
        fnv(h, &zero, 1);
    }
}

static inline uint64_t sensor_hash(const pc_raw_sensor *x)
{
    uint64_t h = H0;
    size_t i;
    uint8_t ff = 0xFF;
    str0(&h, pc_raw_format_name(x->format));
    str0(&h, x->make != NULL ? x->make : "-");
    str0(&h, x->model != NULL ? x->model : "-");
    u64le(&h, x->width);
    u64le(&h, x->height);
    u64le(&h, x->samples);
    for (i = 0; i < x->ndata; i++) {
        u16le(&h, x->data[i]);
    }
    if (x->has_cfa) {
        u64le(&h, x->cfa.width);
        u64le(&h, x->cfa.height);
        fnv(&h, x->cfa.colors, x->cfa.width * x->cfa.height);
        u64le(&h, x->cfa.origin_x);
        u64le(&h, x->cfa.origin_y);
    } else {
        fnv(&h, &ff, 1);
    }
    if (x->linearization != NULL) {
        u64le(&h, x->nlinearization);
        for (i = 0; i < x->nlinearization; i++) {
            u16le(&h, x->linearization[i]);
        }
    } else {
        fnv(&h, &ff, 1);
    }
    u64le(&h, x->black.rows);
    u64le(&h, x->black.cols);
    u64le(&h, x->black.nvalues);
    for (i = 0; i < x->black.nvalues; i++) {
        f32b(&h, x->black.values[i]);
    }
    u64le(&h, x->black.ndelta_h);
    for (i = 0; i < x->black.ndelta_h; i++) {
        f32b(&h, x->black.delta_h[i]);
    }
    u64le(&h, x->black.ndelta_v);
    for (i = 0; i < x->black.ndelta_v; i++) {
        f32b(&h, x->black.delta_v[i]);
    }
    for (i = 0; i < 3; i++) {
        f32b(&h, x->white[i]);
    }
    u64le(&h, x->active.x);
    u64le(&h, x->active.y);
    u64le(&h, x->active.width);
    u64le(&h, x->active.height);
    u64le(&h, x->crop.x);
    u64le(&h, x->crop.y);
    u64le(&h, x->crop.width);
    u64le(&h, x->crop.height);
    u64le(&h, x->color.ncalibrations);
    for (i = 0; i < x->color.ncalibrations; i++) {
        const pc_raw_calibration *c = &x->color.calibrations[i];
        uint8_t b = c->has_forward_matrix ? 1 : 0;
        f64b(&h, c->temperature);
        mat(&h, &c->color_matrix);
        fnv(&h, &b, 1);
        if (b) {
            mat(&h, &c->forward_matrix);
        }
        mat(&h, &c->camera_calibration);
    }
    opt3(&h, x->color.has_analog_balance, x->color.analog_balance, 3);
    opt3(&h, x->color.has_as_shot_neutral, x->color.as_shot_neutral, 3);
    opt3(&h, x->has_camera_wb, x->camera_wb, 3);
    opt3(&h, x->color.has_as_shot_white_xy, x->color.as_shot_white_xy, 2);
    u16le(&h, x->orientation);
    f64b(&h, x->baseline_exposure);
    u64le(&h, x->ngain_maps);
    for (i = 0; i < x->ngain_maps; i++) {
        const pc_raw_gain_map *g = &x->gain_maps[i];
        size_t v[11] = {g->top,       g->left,      g->bottom,   g->right,    g->plane,     g->planes,
                        g->row_pitch, g->col_pitch, g->points_v, g->points_h, g->map_planes};
        size_t k;
        for (k = 0; k < 11; k++) {
            u64le(&h, v[k]);
        }
        f64b(&h, g->spacing_v);
        f64b(&h, g->spacing_h);
        f64b(&h, g->origin_v);
        f64b(&h, g->origin_h);
        u64le(&h, g->ngains);
        for (k = 0; k < g->ngains; k++) {
            f32b(&h, g->gains[k]);
        }
    }
    u64le(&h, x->ntone_curve);
    for (i = 0; i < x->ntone_curve; i++) {
        f32b(&h, x->tone_curve[i][0]);
        f32b(&h, x->tone_curve[i][1]);
    }
    for (i = 0; i < x->warnings.n; i++) {
        str0(&h, x->warnings.v[i]);
    }
    return h;
}

static inline uint64_t developed_hash(const pc_raw_developed *d)
{
    uint64_t h = H0;
    size_t i;
    u32le(&h, d->width);
    u32le(&h, d->height);
    for (i = 0; i < d->nrgb; i++) {
        u16le(&h, d->rgb[i]);
    }
    str0(&h, pc_raw_format_name(d->info.format));
    str0(&h, d->info.make != NULL ? d->info.make : "-");
    str0(&h, d->info.model != NULL ? d->info.model : "-");
    u64le(&h, d->info.sensor_width);
    u64le(&h, d->info.sensor_height);
    str0(&h, d->info.has_cfa ? d->info.cfa : "-");
    for (i = 0; i < 3; i++) {
        f64b(&h, d->info.wb_multipliers[i]);
    }
    u16le(&h, d->info.orientation);
    f64b(&h, d->info.baseline_exposure);
    for (i = 0; i < d->warnings.n; i++) {
        str0(&h, d->warnings.v[i]);
    }
    return h;
}

#endif
