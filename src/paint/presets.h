/* Named brush presets and the built-in set (port of crates/paint/src/presets.rs and brush.rs's
 * BrushPreset). Built-ins are our own designs; their sampled tips and textures are generated
 * procedurally. Grouped like Photoshop's default Brushes panel folders.
 *
 * Ownership: a preset owns its name, group, folders and brush (pc_paint_preset_fini). A preset list owns
 * its presets. Thread-safe as plain data. Failure: PC_ERR_NOMEM; outputs written on success. */
#ifndef PC_PAINT_PRESETS_H
#define PC_PAINT_PRESETS_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "base/str.h"
#include "paint/brush.h"

#define PC_PAINT_GROUP_COUNT 4
/* Preset groups, in panel order: "General", "Dry Media", "Wet Media", "Special Effects". */
extern const char *const PC_PAINT_GROUPS[PC_PAINT_GROUP_COUNT];

/* An owned list of strings (Vec<String>). Zero-initialised = empty. */
typedef struct pc_paint_str_list {
    pc_str *v;
    size_t len;
} pc_paint_str_list;

void pc_paint_str_list_fini(pc_paint_str_list *l); /* NULL-safe */
pc_status pc_paint_str_list_clone(const pc_paint_str_list *src, pc_paint_str_list *out, pc_error *err);
bool pc_paint_str_list_eq(const pc_paint_str_list *a, const pc_paint_str_list *b);

/* The New Brush Preset dialog's boxes, kept with the preset. The default is a preset made without
 * the dialog (a built-in, an import, an older save): picking it brings its size and tool settings
 * and leaves the foreground colour alone. Serialised only when non-default. */
typedef struct pc_paint_preset_captures {
    bool size;          /* Capture Brush Size in Preset. Off: picking keeps the tool's size. */
    bool tool_settings; /* Include Tool Settings: mode, opacity, flow and the pressure buttons. */
    bool color;         /* Include Color: picking sets the foreground colour to the brush's color. */
} pc_paint_preset_captures;

void pc_paint_preset_captures_default(pc_paint_preset_captures *c); /* {true, true, false} */
bool pc_paint_preset_captures_is_default(const pc_paint_preset_captures *c);
bool pc_paint_preset_captures_eq(const pc_paint_preset_captures *a, const pc_paint_preset_captures *b);

typedef struct pc_paint_preset {
    pc_str name;
    pc_paint_brush brush;
    bool builtin; /* shipped with PhotoCraft (regenerated on reset) */
    pc_str group; /* folder in the Brushes panel; "" = none (not serialised) */
    /* Folders inside `group`, outermost first (empty = directly in the group; not serialised
     * when empty), like an imported .abr file's own folders. */
    pc_paint_str_list folder;
    pc_paint_preset_captures captures;
} pc_paint_preset;

/* An empty preset with a default brush. */
void pc_paint_preset_init(pc_paint_preset *p);
void pc_paint_preset_fini(pc_paint_preset *p); /* NULL-safe */
pc_status pc_paint_preset_clone(const pc_paint_preset *src, pc_paint_preset *out, pc_error *err);
bool pc_paint_preset_eq(const pc_paint_preset *a, const pc_paint_preset *b);

typedef struct pc_paint_preset_list {
    pc_paint_preset *v;
    size_t len;
} pc_paint_preset_list;

void pc_paint_preset_list_fini(pc_paint_preset_list *l); /* NULL-safe */
/* The built-in preset set, in display order (grouped by PC_PAINT_GROUPS). */
pc_status pc_paint_presets_builtin(pc_paint_preset_list *out, pc_error *err);
/* Find a preset by name (ASCII case-insensitive); NULL when absent. Borrowed from v. */
const pc_paint_preset *pc_paint_presets_find(const pc_paint_preset *v, size_t n, const char *name,
                                             size_t len);

#endif
