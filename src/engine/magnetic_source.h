/* Immutable source and bounded live-wire cache for interactive Magnetic Lasso. */
#ifndef PC_ENGINE_MAGNETIC_SOURCE_H
#define PC_ENGINE_MAGNETIC_SOURCE_H
#include "engine/engine.h"
#include "algo/magnetic.h"
typedef struct pc_engine_magnetic_source pc_engine_magnetic_source;
pc_status pc_engine_magnetic_source_new(pc_engine_session *session, pc_engine_magnetic_source **out,
                                        pc_error *err);
void pc_engine_magnetic_source_free(pc_engine_magnetic_source *source);
pc_status pc_engine_magnetic_source_snap(pc_engine_magnetic_source *source, const double point[2],
                                         pc_algo_magnetic_settings settings, double out[2], pc_error *err);
pc_status pc_engine_magnetic_source_trace(pc_engine_magnetic_source *source, const double from[2],
                                          const double to[2], const double (*guide)[2], size_t nguide,
                                          pc_algo_magnetic_settings settings, double (**out)[2], size_t *n,
                                          pc_error *err);
#endif
