#ifndef PC_ENGINE_TONE_PREVIEW_H
#define PC_ENGINE_TONE_PREVIEW_H
#include "engine/engine.h"
typedef struct pc_engine_tone_histogram {
    uint32_t bins[5][256];
    size_t channels;
    pc_color_mode mode;
    bool grayscale;
} pc_engine_tone_histogram;
/* Bounded 384px source thumbnail, matching tone.rs; a channel/mask target is gray. */
pc_status pc_engine_tone_histogram_compute(const pc_engine_session *session, bool below_layer,
                                           pc_engine_tone_histogram *out, pc_error *err);
pc_status pc_engine_tone_curve_sample(const double (*points)[2], size_t count, float out[256], pc_error *err);
#endif
