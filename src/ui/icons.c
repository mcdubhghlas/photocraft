/* Tool -> icon-name mapping (port of crates/ui-egui/src/icons.rs tool_icon). */
#include "ui/icons.h"

const char *pc_ui_tool_icon(pc_ui_tool t)
{
    switch (t) {
    case PC_TOOL_MOVE: return "move";
    case PC_TOOL_ARTBOARD: return "artboard";
    case PC_TOOL_RECT_MARQUEE: return "square-dashed";
    case PC_TOOL_ELLIPSE_MARQUEE: return "circle-dashed";
    case PC_TOOL_SINGLE_ROW_MARQUEE: return "marquee-row";
    case PC_TOOL_SINGLE_COLUMN_MARQUEE: return "marquee-column";
    case PC_TOOL_BRUSH: return "brush";
    case PC_TOOL_PENCIL: return "pencil";
    case PC_TOOL_MIXER_BRUSH: return "palette";
    case PC_TOOL_COLOR_REPLACEMENT: return "brush-replace";
    case PC_TOOL_ERASER: return "eraser";
    case PC_TOOL_BACKGROUND_ERASER: return "eraser-background";
    case PC_TOOL_MAGIC_ERASER: return "eraser-magic";
    case PC_TOOL_EYEDROPPER: return "pipette";
    case PC_TOOL_COLOR_SAMPLER: return "sampler";
    case PC_TOOL_RULER: return "ruler";
    case PC_TOOL_NOTE: return "message-square";
    case PC_TOOL_COUNT_TOOL: return "circle-dot";
    case PC_TOOL_LASSO: return "lasso";
    case PC_TOOL_POLYGON_LASSO: return "pentagon";
    case PC_TOOL_MAGNETIC_LASSO: return "lasso-magnetic";
    case PC_TOOL_MAGIC_WAND: return "wand-sparkles";
    case PC_TOOL_CROP: return "crop";
    case PC_TOOL_PERSPECTIVE_CROP: return "perspective-crop";
    case PC_TOOL_SLICE: return "slice-knife";
    case PC_TOOL_SLICE_SELECT: return "square-dashed-mouse-pointer";
    case PC_TOOL_FRAME: return "frame-placeholder";
    case PC_TOOL_GRADIENT: return "gradient";
    case PC_TOOL_PAINT_BUCKET: return "paint-bucket";
    case PC_TOOL_TYPE: case PC_TOOL_VERTICAL_TYPE: return "type";
    case PC_TOOL_HORIZONTAL_TYPE_MASK: return "type-mask";
    case PC_TOOL_VERTICAL_TYPE_MASK: return "type-mask-vertical";
    case PC_TOOL_HAND: return "hand";
    case PC_TOOL_ROTATE_VIEW: return "compass";
    case PC_TOOL_ZOOM: return "zoom-in";
    case PC_TOOL_SPOT_HEALING: case PC_TOOL_HEALING: return "bandage";
    case PC_TOOL_REMOVE: return "bandage-sparkle";
    case PC_TOOL_PATCH: return "lasso-select";
    case PC_TOOL_CONTENT_AWARE_MOVE: return "arrow-left-right";
    case PC_TOOL_RED_EYE: return "eye";
    case PC_TOOL_CLONE_STAMP: return "stamp";
    case PC_TOOL_PATTERN_STAMP: return "grid-3x3";
    case PC_TOOL_HISTORY_BRUSH: return "clock";
    case PC_TOOL_BLUR: return "droplet";
    case PC_TOOL_SHARPEN: return "triangle";
    case PC_TOOL_SMUDGE: return "pointer";
    case PC_TOOL_DODGE: return "lollipop";
    case PC_TOOL_BURN: return "flame";
    case PC_TOOL_SPONGE: return "cloud";
    case PC_TOOL_QUICK_SELECTION: return "circle-dashed";
    case PC_TOOL_OBJECT_SELECTION: return "square-dashed-mouse-pointer";
    case PC_TOOL_PEN: return "pen-tool";
    case PC_TOOL_FREEFORM_PEN: return "pen-freeform";
    case PC_TOOL_CURVATURE_PEN: return "pen-curvature";
    case PC_TOOL_ADD_ANCHOR_POINT: return "pen-add-anchor";
    case PC_TOOL_DELETE_ANCHOR_POINT: return "pen-delete-anchor";
    case PC_TOOL_CONVERT_POINT: return "convert-point";
    case PC_TOOL_PATH_SELECTION: return "mouse-pointer-2";
    case PC_TOOL_DIRECT_SELECTION: return "direct-select";
    case PC_TOOL_RECTANGLE: return "rectangle-horizontal";
    case PC_TOOL_ELLIPSE_SHAPE: return "circle";
    case PC_TOOL_TRIANGLE: return "triangle";
    case PC_TOOL_POLYGON: return "pentagon";
    case PC_TOOL_LINE: return "slash";
    case PC_TOOL_CUSTOM_SHAPE: return "cloud";
    default: return "square";
    }
}
