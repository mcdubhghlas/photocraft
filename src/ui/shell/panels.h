#ifndef PC_UI_SHELL_PANELS_H
#define PC_UI_SHELL_PANELS_H
#include "app.h"
#include "ui/command_queue.h"
#include "ui/layer_drop.h"
typedef struct pc_ui_panel_hit {
    char command[64];
    pc_layer_id layer;
    float rect[4];
} pc_ui_panel_hit;
typedef struct pc_ui_panel_runtime {
    pc_ui_panel_hit hits[1024];
    size_t nhits;
    pc_ui_command_queue commands;
    pc_layer_id editing_layer, name_layer;
    bool editing, document_linked;
    struct {
        bool pending, cancelled;
        pc_doc_id document;
        uint64_t revision;
        double width, height;
        int unit;
    } document_size;
    char name[256];
    float opacity, fill;
    bool opacity_pending, fill_pending;
    size_t action;
    struct {
        bool active, focus;
        pc_doc_id document;
        pc_layer_id layer;
        uint64_t revision;
        char name[4096];
    } rename;
    struct {
        bool pressed, active, allowed, copy;
        pc_doc_id document;
        uint64_t revision;
        pc_layer_id source, target;
        pc_layer_id *layers;
        pc_doc_arc *snapshot;
        size_t count;
        float press[2], point[2], rect[4];
        pc_ui_layer_drop_position position;
    } drag;
    bool snapshot_selected;
    uint64_t snapshot;
} pc_ui_panel_runtime;
void pc_ui_shell_panel_hit(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const char *command,
                           pc_layer_id layer);
void pc_ui_shell_panel_draw(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const char *panel);
void pc_ui_shell_panel_apply(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime);
#endif
