#include "gpu/adjust.h"
#include "compose/compose.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "base/mem.h"
static float clamp(float x, float low, float high)
{
    return fminf(high, fmaxf(low, x));
}
void pc_gpu_adjust_fini(pc_gpu_adjust_program *p)
{
    if (p) {
        free(p->lut);
        *p = (pc_gpu_adjust_program){0};
    }
}
pc_status pc_gpu_adjust_build(const pc_adjustment *adj, const pc_document *doc, pc_gpu_adjust_program *out,
                              pc_error *err)
{
    if (!adj || !doc || !out) {
        return pc_fail(err, PC_ERR_INVALID, "invalid GPU adjustment");
    }
    if ((doc->mode == PC_COLOR_MODE_LAB && pc_compose_lab_channel_adjustment(adj)) ||
        (doc->mode == PC_COLOR_MODE_CMYK && pc_compose_cmyk_channel_adjustment(adj))) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "native channel adjustment (composited on the CPU)");
    }
    pc_gpu_adjust_program p = {0};
    float (*v)[4] = p.params;
    pc_status rc = PC_OK;
    switch (adj->kind) {
    case PC_ADJ_INVERT:
        p.kind = 1;
        break;
    case PC_ADJ_THRESHOLD:
        p.kind = 2;
        v[0][0] = roundf(adj->u.threshold.level * 255);
        break;
    case PC_ADJ_POSTERIZE:
        p.kind = 3;
        v[0][0] = clamp((float)adj->u.posterize.levels, 2, 255);
        break;
    case PC_ADJ_BRIGHTNESS_CONTRAST:
        if (adj->u.brightness_contrast.legacy) {
            p.kind = 4;
            float c = clamp(adj->u.brightness_contrast.contrast, -100, 99);
            v[0][0] = c >= 0 ? adj->u.brightness_contrast.brightness / 255 : 0;
            v[0][1] = c >= 0 ? 1 / (1 - c / 100) : 1 + c / 100;
            v[0][2] = PC_COMPOSE_LEGACY_PIVOT;
            v[0][3] = c < 0 ? adj->u.brightness_contrast.brightness / 255 : 0;
        } else {
            p.kind = 5;
            v[0][0] = adj->u.brightness_contrast.brightness;
            v[0][1] = adj->u.brightness_contrast.contrast;
        }
        break;
    case PC_ADJ_EXPOSURE: {
        p.kind = 6;
        pc_compose_transfer transfer =
            pc_compose_transfer_for_exposure(pc_compose_transfer_for_document(doc->mode, doc->depth));
        v[0][0] = exp2f(adj->u.exposure.exposure);
        v[0][1] = adj->u.exposure.offset;
        v[0][2] = fmaxf(.01f, adj->u.exposure.gamma);
        v[0][3] = transfer.kind == PC_COMPOSE_TRANSFER_GAMMA ? transfer.gamma : 0;
        break;
    }
    case PC_ADJ_LEVELS:
    case PC_ADJ_CURVES: {
        pc_tone_space space = adj->kind == PC_ADJ_LEVELS ? adj->u.levels.space : adj->u.curves.space;
        if (space != PC_TONE_RGB) {
            return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU tone colour space");
        }
        if (adj->kind == PC_ADJ_LEVELS && doc->depth == PC_SAMPLE_TYPE_F32) {
            p.kind = 17;
            p.width = 5;
            p.lut = pc_calloc_array(5, sizeof *p.lut);
            if (!p.lut) {
                return pc_fail(err, PC_ERR_NOMEM, "GPU float Levels");
            }
            for (size_t i = 0; i < 4; i++) {
                const pc_levels_channel *ch = i ? &adj->u.levels.per_channel[i - 1] : &adj->u.levels.master;
                p.lut[0][i] = ch->in_black;
                p.lut[1][i] = ch->in_white;
                p.lut[2][i] = fmaxf(.01f, ch->gamma);
                p.lut[3][i] = ch->out_black;
                p.lut[4][i] = ch->out_white;
            }
        } else {
            p.kind = 7;
            p.width = PC_COMPOSE_LUT_SIZE;
            float *rows = pc_malloc_array(4 * PC_COMPOSE_LUT_SIZE, sizeof *rows);
            p.lut = pc_calloc_array(p.width, sizeof *p.lut);
            if (!rows || !p.lut) {
                free(rows);
                pc_gpu_adjust_fini(&p);
                return pc_fail(err, PC_ERR_NOMEM, "GPU tone LUT");
            }
            rc = pc_compose_tone_luts_depth(adj, true, doc->depth, rows, err);
            bool identity[3];
            pc_compose_float_tone_identity(adj, true, doc->depth, identity);
            for (size_t ch = 0; ch < 3; ch++) {
                v[0][ch] = identity[ch] ? 1 : 0;
                for (size_t i = 0; i < p.width; i++) {
                    p.lut[i][ch] = rows[ch * p.width + i];
                }
            }
            free(rows);
        }
        break;
    }
    case PC_ADJ_CHANNEL_MIXER:
        p.kind = 10;
        memcpy(v, adj->u.channel_mixer.matrix, sizeof adj->u.channel_mixer.matrix);
        v[3][0] = adj->u.channel_mixer.monochrome ? 1 : 0;
        break;
    case PC_ADJ_PHOTO_FILTER: {
        p.kind = 11;
        float matrix[3][3];
        bool linear = doc->depth == PC_SAMPLE_TYPE_F32;
        pc_compose_photo_filter_matrix(adj->u.photo_filter.color, adj->u.photo_filter.density,
                                       adj->u.photo_filter.preserve_luminosity && linear, matrix);
        for (size_t row = 0; row < 3; row++) {
            memcpy(v[row], matrix[row], sizeof matrix[row]);
        }
        pc_compose_transfer transfer = pc_compose_transfer_for_document(doc->mode, doc->depth);
        v[3][0] = adj->u.photo_filter.preserve_luminosity && !linear ? 1 : 0;
        v[3][1] = transfer.kind == PC_COMPOSE_TRANSFER_GAMMA ? transfer.gamma : 0;
        break;
    }
    case PC_ADJ_BLACK_WHITE:
        p.kind = 12;
        memcpy(v[0], adj->u.black_white.weights, sizeof v[0]);
        v[1][0] = adj->u.black_white.weights[4];
        v[1][1] = adj->u.black_white.weights[5];
        v[1][2] = adj->u.black_white.has_tint ? 1 : 0;
        memcpy(v[2], adj->u.black_white.tint, sizeof adj->u.black_white.tint);
        break;
    case PC_ADJ_COLOR_BALANCE: {
        p.kind = 7;
        p.width = PC_COMPOSE_LUT_SIZE;
        p.lut = pc_calloc_array(p.width, sizeof *p.lut);
        if (!p.lut) {
            return pc_fail(err, PC_ERR_NOMEM, "GPU balance LUT");
        }
        float tones[3][3];
        memcpy(tones[0], adj->u.color_balance.shadows, sizeof tones[0]);
        memcpy(tones[1], adj->u.color_balance.midtones, sizeof tones[1]);
        memcpy(tones[2], adj->u.color_balance.highlights, sizeof tones[2]);
        for (size_t i = 0; i < p.width; i++) {
            for (int ch = 0; ch < 3; ch++) {
                p.lut[i][ch] = pc_compose_color_balance_channel((const float (*)[3])tones,
                                                                adj->u.color_balance.preserve_luminosity, ch,
                                                                (float)i / (float)(p.width - 1));
            }
        }
        break;
    }
    case PC_ADJ_HUE_SATURATION: {
        p.kind = 8;
        v[0][0] = adj->u.hue_saturation.hue;
        v[0][1] = adj->u.hue_saturation.saturation / 100;
        v[0][2] = adj->u.hue_saturation.lightness / 100;
        v[0][3] = adj->u.hue_saturation.colorize ? 1 : 0;
        p.width = PC_COMPOSE_LUT_SIZE;
        p.lut = pc_calloc_array(p.width, sizeof *p.lut);
        float *rows = pc_malloc_array(3 * p.width, sizeof *rows);
        if (!rows || !p.lut) {
            free(rows);
            pc_gpu_adjust_fini(&p);
            return pc_fail(err, PC_ERR_NOMEM, "GPU hue LUT");
        }
        pc_compose_hue_range_tables(adj->u.hue_saturation.ranges, rows);
        for (size_t i = 0; i < p.width; i++) {
            for (size_t ch = 0; ch < 3; ch++) {
                p.lut[i][ch] = rows[ch * p.width + i];
            }
        }
        free(rows);
        break;
    }
    case PC_ADJ_VIBRANCE: {
        p.kind = 9;
        v[0][0] = adj->u.vibrance.vibrance / 100;
        v[0][1] = adj->u.vibrance.saturation / 100;
        pc_compose_transfer transfer = pc_compose_transfer_for_document(doc->mode, doc->depth);
        v[0][2] = transfer.kind == PC_COMPOSE_TRANSFER_GAMMA ? transfer.gamma : 0;
        v[0][3] = doc->depth == PC_SAMPLE_TYPE_F32 ? FLT_MAX : 1;
        memcpy(v[1], PC_COMPOSE_VIBRANCE_BOOST, 4 * sizeof(float));
        memcpy(v[2], PC_COMPOSE_VIBRANCE_BOOST + 4, 3 * sizeof(float));
        break;
    }
    case PC_ADJ_SELECTIVE_COLOR:
        p.kind = 15;
        v[0][0] = adj->u.selective_color.relative ? 1 : 0;
        p.width = 9;
        p.lut = pc_malloc_array(9, sizeof *p.lut);
        if (!p.lut) {
            return pc_fail(err, PC_ERR_NOMEM, "GPU Selective Color");
        }
        memcpy(p.lut, adj->u.selective_color.adjustments, 9 * sizeof *p.lut);
        break;
    case PC_ADJ_COLOR_LOOKUP: {
        size_t length, expected;
        const float *table = pc_adjustment_lut(adj, &length);
        if (!table || !pc_lut3d_len(adj->u.color_lookup.size, &expected) || length < expected) {
            break;
        }
        if (adj->u.color_lookup.size > 128) {
            return pc_fail(err, PC_ERR_LIMIT, "GPU Color Lookup budget");
        }
        p.kind = 16;
        v[0][0] = (float)adj->u.color_lookup.size;
        v[0][1] = adj->u.color_lookup.tetrahedral ? 1 : 0;
        v[0][2] = adj->u.color_lookup.dither ? 1 : 0;
        size_t cells = expected / 3;
        p.width = cells < 4096 ? cells : 4096;
        p.height = (cells + p.width - 1) / p.width;
        p.lut = pc_calloc_array(p.width * p.height, sizeof *p.lut);
        if (!p.lut) {
            return pc_fail(err, PC_ERR_NOMEM, "GPU Color Lookup table");
        }
        for (size_t i = 0; i < cells; i++) {
            memcpy(p.lut[i], table + i * 3, 3 * sizeof(float));
        }
        break;
    }
    case PC_ADJ_GRADIENT_MAP: {
        size_t stops = adj->u.gradient_map.nstops;
        if (stops > 4096) {
            return pc_fail(err, PC_ERR_LIMIT, "GPU Gradient Map stops");
        }
        for (size_t i = 0; i < stops; i++) {
            float at = adj->u.gradient_map.stops[i].pos;
            if (!isfinite(at)) {
                return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU Gradient Map positions");
            }
            for (size_t j = 0; j < i; j++) {
                if (fabsf(at - adj->u.gradient_map.stops[j].pos) < 1.f / 4095) {
                    return pc_fail(err, PC_ERR_UNSUPPORTED, "coincident Gradient Map stops");
                }
            }
        }
        p.kind = 13;
        v[0][0] = adj->u.gradient_map.reverse ? 1 : 0;
        v[0][1] = adj->u.gradient_map.dither ? 1 : 0;
        pc_compose_buffer samples = {0};
        rc = pc_compose_buffer_transparent(pc_rect_make(0, 0, PC_COMPOSE_LUT_SIZE, 1), &samples, err);
        if (rc != PC_OK) {
            return rc;
        }
        for (size_t i = 0; i < samples.len; i++) {
            for (size_t ch = 0; ch < 3; ch++) {
                samples.px[i][ch] = (float)i / (float)(samples.len - 1);
            }
            samples.px[i][3] = 1;
        }
        pc_adjustment sampler = *adj;
        sampler.u.gradient_map.reverse = sampler.u.gradient_map.dither = false;
        rc = pc_compose_adjust_apply_depth(&sampler, &samples,
                                           pc_compose_transfer_for_document(doc->mode, doc->depth), true,
                                           doc->depth, err);
        if (rc == PC_OK) {
            p.lut = samples.px;
            p.width = samples.len;
            samples.px = NULL;
        }
        pc_compose_buffer_fini(&samples);
        break;
    }
    case PC_ADJ_UNSUPPORTED:
        break; /* Identity in the CPU reference. */
    default:
        return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU adjustment program");
    }
    if (rc != PC_OK) {
        pc_gpu_adjust_fini(&p);
        return rc;
    }
    if (p.width && !p.height) {
        p.height = 1;
    }
    pc_gpu_adjust_fini(out);
    *out = p;
    return PC_OK;
}
