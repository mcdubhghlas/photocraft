#ifndef PC_ENGINE_GALLERY_CATALOG_H
#define PC_ENGINE_GALLERY_CATALOG_H
#include "engine/engine.h"
/* Pure catalogue query; no history, action or journal mutation. */
pc_status pc_engine_gallery_catalogue(pc_json_value *out, pc_error *err);
#endif
