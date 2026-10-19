#ifndef PC_ENGINE_SAMPLING_H
#define PC_ENGINE_SAMPLING_H
#include "engine/engine.h"
typedef enum pc_engine_sample_layers {
    PC_SAMPLE_CURRENT,
    PC_SAMPLE_CURRENT_AND_BELOW,
    PC_SAMPLE_ALL,
    PC_SAMPLE_ALL_NO_ADJUSTMENTS,
    PC_SAMPLE_CURRENT_AND_BELOW_NO_ADJUSTMENTS
} pc_engine_sample_layers;
bool pc_engine_sample_layers_parse(const char *name, pc_engine_sample_layers *out);
bool pc_engine_sample_size_valid(uint32_t size);
pc_status pc_engine_sample_size(yyjson_val *params, uint32_t *out, pc_error *err);
/* Alpha-weighted native-channel average, quantized at the supplied depth. Uses the calling
 * thread's document CMYK space. Empty/transparent/nonfinite input yields transparent black. */
void pc_engine_sample_average(const float (*pixels)[4], size_t count, pc_color_mode mode,
                              pc_sample_type depth, float out[4]);
/* Bounded 101x101 maximum render. Off-canvas/nonfinite points yield transparent black.
 * Current samples the layer alone at normal blend and full opacity; it keeps its own masks. */
pc_status pc_engine_sample_color(const pc_document *doc, bool has_active, pc_layer_id active, double x,
                                 double y, uint32_t size, pc_engine_sample_layers which, float out[4],
                                 pc_error *err);
const pc_engine_command *pc_engine_sampling_specs(size_t *count);
#endif
