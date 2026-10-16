/* Save for Web (Legacy) internals, exposed for unit tests (port of crates/engine/src/web_cmds.rs).
 *
 * The command itself is registered through pc_engine_web_specs; these declarations expose the
 * optimisation settings and the GIF/PNG-8/PNG-24/JPEG/WBMP optimiser so the ported unit tests can
 * drive them directly (WebSettings::from_params, optimize, preset_settings). The Image Assets /
 * Generator feature (parse_asset_name, generate_assets) is a separate follow-up (needs
 * layer_menu_cmds::layer_document) and stays a stub; see REWRITE_PLAN.md §7. */
#ifndef PC_ENGINE_WEB_CMDS_H
#define PC_ENGINE_WEB_CMDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/quantize.h"
#include "base/buf.h"
#include "base/json.h"
#include "base/status.h"
#include "geom/geom.h"

typedef enum pc_web_format {
    PC_WEB_GIF,
    PC_WEB_PNG8,
    PC_WEB_PNG24,
    PC_WEB_JPEG,
    PC_WEB_WBMP
} pc_web_format;

/* WebFormat::from_id ("gif", "png8"/"png-8", "png24"/"png-24"/"png"/"png32", "jpeg"/"jpg",
 * "wbmp"); false otherwise. */
bool pc_web_format_from_id(const char *s, pc_web_format *out);
/* The output extension ("gif", "png", "jpg", "wbmp"). */
const char *pc_web_format_ext(pc_web_format f);

typedef enum pc_web_metadata {
    PC_WEB_META_NONE,
    PC_WEB_META_COPYRIGHT,
    PC_WEB_META_CONTACT, /* copyright + author */
    PC_WEB_META_ALL
} pc_web_metadata;

typedef struct pc_web_edit {
    size_t index;
    uint8_t color[3];
} pc_web_edit;

/* One optimisation preset (the right-hand side of the Save for Web dialog). */
typedef struct pc_web_settings {
    pc_web_format format;
    pc_algo_palette_kind palette;
    size_t colors;
    bool has_color_table; /* fixed opaque source palette in index order */
    uint8_t (*color_table)[3];
    size_t ncolor_table;
    pc_web_edit *edits; /* replacements applied after quantization, index order */
    size_t nedits;
    pc_algo_dither dither;
    float dither_amount; /* 0..1 */
    bool transparency;
    bool has_matte; /* false = no matte (hard alpha threshold) */
    float matte[3]; /* 0..1 RGB */
    bool interlaced;
    float web_snap; /* 0..1 */
    uint8_t quality; /* JPEG 0..100 */
    bool progressive;
    bool optimized;
    bool embed_icc;
    pc_web_metadata metadata;
    bool convert_to_srgb;
} pc_web_settings;

void pc_web_settings_default(pc_web_settings *st);
void pc_web_settings_fini(pc_web_settings *st);
/* Settings from command parameters (missing keys keep the defaults; a "preset" name such as
 * "GIF 128 Dithered" seeds the whole set). cmd prefixes bad-parameter errors. */
pc_status pc_web_settings_from_params(yyjson_val *p, const char *cmd, pc_web_settings *out, pc_error *err);

/* Save for Web's named presets; true and fills *out when name is one, false otherwise. */
bool pc_web_preset_settings(const char *name, pc_web_settings *out);

/* An optimised image. */
typedef struct pc_web_optimized {
    pc_buf bytes;
    const char *ext;
    uint32_t width, height;
    bool has_colors; /* indexed formats */
    size_t colors;
    uint8_t (*color_table)[3]; /* opaque source palette before replacements */
    size_t ncolor_table;
    bool has_transparent;
    size_t transparent_index;
    uint8_t *indices; /* preview only; indexed formats */
    size_t nindices;
    uint8_t *preview; /* preview only; straight RGBA8 */
    size_t npreview;
} pc_web_optimized;

void pc_web_optimized_fini(pc_web_optimized *o);

/* Optimises `rect` of `px` (straight RGBA, `bw` wide) with `st`. `preview` also fills the straight
 * RGBA8 of what the file looks like (the dialog's panes); exports pass false. icc/xmp may be NULL. */
pc_status pc_web_optimize(const float (*px)[4], size_t bw, pc_rect rect, const pc_web_settings *st,
                          const uint8_t *icc, size_t icc_len, const char *xmp, size_t xmp_len, float dpi,
                          bool preview, pc_web_optimized *out, pc_error *err);

#endif /* PC_ENGINE_WEB_CMDS_H */
