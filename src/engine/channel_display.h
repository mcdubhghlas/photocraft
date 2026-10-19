/* Read-only Channels-panel view state, safe for UI consumers (no engine-private types). */
#ifndef PC_ENGINE_CHANNEL_DISPLAY_H
#define PC_ENGINE_CHANNEL_DISPLAY_H
#include "engine.h"
size_t pc_engine_color_count(const pc_document *doc);
size_t pc_engine_channel_visible_colors(const pc_engine_doc_state *state, size_t colors);
bool pc_engine_channel_alpha_shown(const pc_engine_doc_state *state, size_t index);
bool pc_engine_channel_quick_mask_hidden(const pc_engine_doc_state *state);
bool pc_engine_channel_color_shown(const pc_engine_doc_state *state, size_t index);
size_t pc_engine_targeted_color(const pc_engine_session *session);
bool pc_engine_targeted_vector_mask(const pc_engine_session *session);
bool pc_engine_targeted_mask(const pc_engine_session *session);
bool pc_engine_targeted_channel(const pc_engine_session *session, bool *is_quick, size_t *alpha_index);
void pc_engine_quick_mask_display(const pc_engine_session *session, float color[3], float *opacity);
uint64_t pc_engine_channel_display_key(const pc_engine_doc_state *state);
#endif
