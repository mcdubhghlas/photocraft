/* Properties-panel layout helpers: see props_layout.h. Port of crates/ui-egui/src/props_layout.rs
 * (the pure parts). */
#include "ui/props_layout.h"

const char *pc_ui_props_kind_icon(pc_layer_kind kind)
{
    switch (kind) {
    case PC_LAYER_ADJUSTMENT: return "sliders-horizontal";
    case PC_LAYER_GROUP: return "folder";
    case PC_LAYER_FILL: return "paint-bucket";
    case PC_LAYER_TEXT: return "type";
    case PC_LAYER_SHAPE: return "pentagon";
    case PC_LAYER_SMART: return "app-window";
    case PC_LAYER_RASTER: return "image";
    default: return "image";
    }
}

float pc_ui_props_field_width(float avail, size_t cols, float label_w)
{
    float n = cols < 1 ? 1.0f : (float)cols;
    float w = (avail - n * (label_w + PC_UI_PROPS_LABEL_GAP) - (n - 1.0f) * PC_UI_PROPS_COL_GAP) / n;
    return w > 36.0f ? w : 36.0f;
}

const pc_ui_quick_action *pc_ui_props_quick_actions(pc_layer_kind kind, size_t *n_out)
{
    static const pc_ui_quick_action raster[] = {
        {"Remove Background", "layer.removeBackground"},
        {"Select Subject", "select.subject"},
    };
    static const pc_ui_quick_action text[] = {
        {"Convert to Shape", "type.convertToShape"},
        {"Convert to Paragraph Text", "type.convertToParagraphText"},
        {"Convert to Point Text", "type.convertToPointText"},
        {"Create Warped Text", "type.warpText"},
        {"Rasterize Type", "type.rasterizeTypeLayer"},
    };
    static const pc_ui_quick_action shape[] = {
        {"Rasterize Shape", "layer.rasterize.shape"},
        {"Convert to Smart Object", "layer.smartObjects.convertToSmartObject"},
    };
    static const pc_ui_quick_action smart[] = {
        {"Edit Contents", "layer.smartObjects.editContents"},
        {"Convert to Layers", "layer.smartObjects.convertToLayers"},
        {"Rasterize Smart Object", "layer.rasterize.smartObject"},
    };
    static const pc_ui_quick_action group[] = {
        {"Ungroup Layers", "layer.ungroupLayers"},
        {"Convert to Smart Object", "layer.smartObjects.convertToSmartObject"},
    };
    static const pc_ui_quick_action fill[] = {
        {"Rasterize Fill Content", "layer.rasterize.fillContent"},
    };
    switch (kind) {
    case PC_LAYER_RASTER: *n_out = 2; return raster;
    case PC_LAYER_TEXT: *n_out = 5; return text;
    case PC_LAYER_SHAPE: *n_out = 2; return shape;
    case PC_LAYER_SMART: *n_out = 3; return smart;
    case PC_LAYER_GROUP: *n_out = 2; return group;
    case PC_LAYER_FILL: *n_out = 1; return fill;
    case PC_LAYER_ADJUSTMENT: default: *n_out = 0; return NULL;
    }
}
