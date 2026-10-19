/* Tool -> icon-name mapping (port of crates/ui-egui/src/icons.rs tool_icon). */
#include "ui/icons.h"
#include "icon_data.h"
#include "io/svg.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>

const char *pc_ui_tool_icon(pc_ui_tool t)
{
    switch (t) {
    case PC_TOOL_MOVE:
        return "move";
    case PC_TOOL_ARTBOARD:
        return "artboard";
    case PC_TOOL_RECT_MARQUEE:
        return "square-dashed";
    case PC_TOOL_ELLIPSE_MARQUEE:
        return "circle-dashed";
    case PC_TOOL_SINGLE_ROW_MARQUEE:
        return "marquee-row";
    case PC_TOOL_SINGLE_COLUMN_MARQUEE:
        return "marquee-column";
    case PC_TOOL_BRUSH:
        return "brush";
    case PC_TOOL_PENCIL:
        return "pencil";
    case PC_TOOL_MIXER_BRUSH:
        return "palette";
    case PC_TOOL_COLOR_REPLACEMENT:
        return "brush-replace";
    case PC_TOOL_ERASER:
        return "eraser";
    case PC_TOOL_BACKGROUND_ERASER:
        return "eraser-background";
    case PC_TOOL_MAGIC_ERASER:
        return "eraser-magic";
    case PC_TOOL_EYEDROPPER:
        return "pipette";
    case PC_TOOL_COLOR_SAMPLER:
        return "sampler";
    case PC_TOOL_RULER:
        return "ruler";
    case PC_TOOL_NOTE:
        return "message-square";
    case PC_TOOL_COUNT_TOOL:
        return "circle-dot";
    case PC_TOOL_LASSO:
        return "lasso";
    case PC_TOOL_POLYGON_LASSO:
        return "pentagon";
    case PC_TOOL_MAGNETIC_LASSO:
        return "lasso-magnetic";
    case PC_TOOL_MAGIC_WAND:
        return "wand-sparkles";
    case PC_TOOL_CROP:
        return "crop";
    case PC_TOOL_PERSPECTIVE_CROP:
        return "perspective-crop";
    case PC_TOOL_SLICE:
        return "slice-knife";
    case PC_TOOL_SLICE_SELECT:
        return "square-dashed-mouse-pointer";
    case PC_TOOL_FRAME:
        return "frame-placeholder";
    case PC_TOOL_GRADIENT:
        return "gradient";
    case PC_TOOL_PAINT_BUCKET:
        return "paint-bucket";
    case PC_TOOL_TYPE:
    case PC_TOOL_VERTICAL_TYPE:
        return "type";
    case PC_TOOL_HORIZONTAL_TYPE_MASK:
        return "type-mask";
    case PC_TOOL_VERTICAL_TYPE_MASK:
        return "type-mask-vertical";
    case PC_TOOL_HAND:
        return "hand";
    case PC_TOOL_ROTATE_VIEW:
        return "compass";
    case PC_TOOL_ZOOM:
        return "zoom-in";
    case PC_TOOL_SPOT_HEALING:
    case PC_TOOL_HEALING:
        return "bandage";
    case PC_TOOL_REMOVE:
        return "bandage-sparkle";
    case PC_TOOL_PATCH:
        return "lasso-select";
    case PC_TOOL_CONTENT_AWARE_MOVE:
        return "arrow-left-right";
    case PC_TOOL_RED_EYE:
        return "eye";
    case PC_TOOL_CLONE_STAMP:
        return "stamp";
    case PC_TOOL_PATTERN_STAMP:
        return "grid-3x3";
    case PC_TOOL_HISTORY_BRUSH:
        return "clock";
    case PC_TOOL_BLUR:
        return "droplet";
    case PC_TOOL_SHARPEN:
        return "triangle";
    case PC_TOOL_SMUDGE:
        return "pointer";
    case PC_TOOL_DODGE:
        return "lollipop";
    case PC_TOOL_BURN:
        return "flame";
    case PC_TOOL_SPONGE:
        return "cloud";
    case PC_TOOL_QUICK_SELECTION:
        return "circle-dashed";
    case PC_TOOL_OBJECT_SELECTION:
        return "square-dashed-mouse-pointer";
    case PC_TOOL_PEN:
        return "pen-tool";
    case PC_TOOL_FREEFORM_PEN:
        return "pen-freeform";
    case PC_TOOL_CURVATURE_PEN:
        return "pen-curvature";
    case PC_TOOL_ADD_ANCHOR_POINT:
        return "pen-add-anchor";
    case PC_TOOL_DELETE_ANCHOR_POINT:
        return "pen-delete-anchor";
    case PC_TOOL_CONVERT_POINT:
        return "convert-point";
    case PC_TOOL_PATH_SELECTION:
        return "mouse-pointer-2";
    case PC_TOOL_DIRECT_SELECTION:
        return "direct-select";
    case PC_TOOL_RECTANGLE:
        return "rectangle-horizontal";
    case PC_TOOL_ELLIPSE_SHAPE:
        return "circle";
    case PC_TOOL_TRIANGLE:
        return "triangle";
    case PC_TOOL_POLYGON:
        return "pentagon";
    case PC_TOOL_LINE:
        return "slash";
    case PC_TOOL_CUSTOM_SHAPE:
        return "cloud";
    default:
        return "square";
    }
}

static const pc_ui_embedded_icon *find_icon(const char *name)
{
    if (!name) {
        return NULL;
    }
    size_t low = 0, high = pc_ui_embedded_icons_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(name, pc_ui_embedded_icons[middle].name);
        if (!order) {
            return &pc_ui_embedded_icons[middle];
        }
        if (order < 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return NULL;
}
size_t pc_ui_icon_count(void)
{
    return pc_ui_embedded_icons_count;
}
const char *pc_ui_icon_name(size_t index)
{
    return index < pc_ui_embedded_icons_count ? pc_ui_embedded_icons[index].name : NULL;
}
bool pc_ui_icon_exists(const char *name)
{
    return find_icon(name) != NULL;
}
const uint8_t *pc_ui_icon_svg(const char *name, size_t *length)
{
    const pc_ui_embedded_icon *icon = find_icon(name);
    if (!icon) {
        icon = find_icon("square");
    }
    if (length) {
        *length = icon ? icon->len : 0;
    }
    return icon ? icon->svg : NULL;
}
pc_status pc_ui_icon_rasterize(const char *name, uint16_t side, pc_rgba8_image *out, pc_error *err)
{
    if (!out || side < 8 || side > 256) {
        return pc_fail(err, PC_ERR_INVALID, "icon size must be 8..256 pixels");
    }
    size_t len = 0;
    const uint8_t *source = pc_ui_icon_svg(name, &len);
    if (!source) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "embedded icon is unavailable");
    }
    pc_buf svg = PC_BUF_INIT;
    pc_status status = PC_OK;
    const char *needles[] = {"currentColor", "stroke-width=\"2\""};
    const char *values[] = {"#ffffff", "stroke-width=\"1.75\""};
    for (size_t i = 0; i < len && status == PC_OK;) {
        size_t match = 2;
        for (size_t j = 0; j < 2; j++) {
            size_t n = strlen(needles[j]);
            if (n <= len - i && !memcmp(source + i, needles[j], n)) {
                match = j;
                break;
            }
        }
        if (match < 2) {
            status = pc_buf_append_str(&svg, values[match], err);
            i += strlen(needles[match]);
        } else {
            status = pc_buf_append(&svg, source + i, 1, err);
            i++;
        }
    }
    pc_compose_buffer raster = {0};
    if (status == PC_OK) {
        status = pc_io_svg_rasterize(svg.data, svg.len, (float)side / 24, &raster, err);
    }
    pc_rgba8_image image = {0};
    if (status == PC_OK) {
        status = pc_compose_buffer_to_rgba8(&raster, &image, err);
    }
    if (status == PC_OK && (image.width != side || image.height != side)) {
        /* Raster bounds round outward; a fractional scale can add one transparent edge pixel. */
        if (image.width < side || image.height < side) {
            status = pc_fail(err, PC_ERR_FORMAT, "icon dimensions differ from its 24-pixel viewBox");
        } else {
            uint8_t *pixels = pc_malloc_array((size_t)side * side, 4);
            if (!pixels) {
                status = pc_fail(err, PC_ERR_NOMEM, "icon pixels");
            } else {
                for (size_t y = 0; y < side; y++) {
                    memcpy(pixels + y * side * 4, image.pixels + y * image.width * 4, (size_t)side * 4);
                }
                free(image.pixels);
                image = (pc_rgba8_image){side, side, pixels};
            }
        }
    }
    pc_compose_buffer_fini(&raster);
    pc_buf_fini(&svg);
    if (status != PC_OK) {
        free(image.pixels);
        return status;
    }
    *out = image;
    return PC_OK;
}
