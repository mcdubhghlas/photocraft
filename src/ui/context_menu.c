#include "context_menu.h"
static const pc_ui_context_row SELECTION_MENU[] = {
    {"Deselect", "select.deselect"},
    {"Select Inverse", "select.inverse"},
    {"Feather…", "select.modify.feather"},
    {"Select and Mask…", "select.selectAndMask"},
    {NULL, NULL},
    {"Save Selection…", "select.saveSelection"},
    {"Make Work Path…", "select.toWorkPath"},
    {NULL, NULL},
    {"Layer via Copy", "layer.new.layerViaCopy"},
    {"Layer via Cut", "layer.new.layerViaCut"},
    {"New Layer…", "layer.new.layer"},
    {NULL, NULL},
    {"Free Transform", "edit.freeTransform"},
    {"Transform Selection", "select.transformSelection"},
    {"Distort", "edit.transform.distort"},
    {"Perspective", "edit.transform.perspective"},
    {NULL, NULL},
    {"Delete and Fill Selection", "edit.deleteAndFillSelection"},
    {"Fill…", "edit.fill"},
    {"Stroke…", "edit.stroke"},
    {"Content-Aware Fill…", "edit.contentAwareFill"},
    {NULL, NULL},
    {"Last Filter", "filter.lastFilter"},
    {NULL, NULL},
    {"Fade…", "edit.fade"},
};
static const pc_ui_context_row NO_SELECTION_MENU[] = {
    {"Select All", "select.all"},
    {"Reselect", "select.reselect"},
    {"Color Range…", "select.colorRange"},
    {NULL, NULL},
    {"Load Selection…", "select.loadSelection"},
};
static const pc_ui_context_row PEN_MENU[] = {
    {"Create Vector Mask", "layer.vectorMask.fromPath"},
    {"Delete Path", "path.delete"},
    {NULL, NULL},
    {"Define Custom Shape…", "edit.defineCustomShape"},
    {NULL, NULL},
    {"Make Selection…", "path.toSelection"},
    {"New Guides From Shape", "view.newGuidesFromShape"},
    {"Fill Path…", "path.fill"},
    {"Stroke Path…", "path.stroke"},
    {NULL, NULL},
    {"Clipping Path…", "path.clippingPath.set"},
    {NULL, NULL},
    {"Free Transform Path", "path.transform"},
    {NULL, NULL},
    {"Unite Shapes", "layer.combineShapes.unite"},
    {"Subtract Front Shape", "layer.combineShapes.subtractFrontShape"},
    {"Unite Shapes at Overlap", "layer.combineShapes.intersectShapeAreas"},
    {"Subtract Shapes at Overlap", "layer.combineShapes.excludeOverlappingShapes"},
    {NULL, NULL},
    {"Copy Fill", "path.style.copyFill"},
    {"Copy Complete Stroke", "path.style.copyStroke"},
    {NULL, NULL},
    {"Paste Fill", "path.style.pasteFill"},
    {"Paste Complete Stroke", "path.style.pasteStroke"},
    {NULL, NULL},
    {"Isolate Layers", "select.isolateLayers"},
    {NULL, NULL},
    {"Make Symmetry Path", "paint.symmetryFromPath"},
    {"Disable Symmetry Path", "paint.symmetryDisable"},
};
static const pc_ui_context_row VIEW_MENU[] = {
    {"Zoom In", "view.zoomIn"},
    {"Zoom Out", "view.zoomOut"},
    {NULL, NULL},
    {"Fit on Screen", "view.fitOnScreen"},
    {"Fill Screen", "view.fillScreen"},
    {NULL, NULL},
    {"100%", "view.actualPixels"},
    {"200%", "view.twoHundredPercent"},
    {"Print Size", "view.printSize"},
};
static const pc_ui_context_row TRANSFORM_MENU[] = {
    {"Free Transform", "edit.freeTransform"},
    {"Scale", "edit.transform.scale"},
    {"Rotate", "edit.transform.rotate"},
    {"Skew", "edit.transform.skew"},
    {"Distort", "edit.transform.distort"},
    {"Perspective", "edit.transform.perspective"},
    {"Warp", "edit.transform.warp"},
    {NULL, NULL},
    {"Content-Aware Scale", "edit.contentAwareScale"},
    {"Puppet Warp", "edit.puppetWarp"},
    {NULL, NULL},
    {"Rotate 180°", "edit.transform.rotate180"},
    {"Rotate 90° Clockwise", "edit.transform.rotate90Cw"},
    {"Rotate 90° Counter Clockwise", "edit.transform.rotate90Ccw"},
    {NULL, NULL},
    {"Flip Horizontal", "edit.transform.flipHorizontal"},
    {"Flip Vertical", "edit.transform.flipVertical"},
};
static const pc_ui_context_row PASTEBOARD_MENU[] = {
    {"Default", "pasteboard.default"},
    {"Black", "pasteboard.black"},
    {"Dark Gray", "pasteboard.darkGray"},
    {"Medium Gray", "pasteboard.mediumGray"},
    {"Light Gray", "pasteboard.lightGray"},
    {"Custom", "pasteboard.custom"},
    {NULL, NULL},
    {"Select Custom Color…", "pasteboard.selectCustomColor"},
};
static const pc_ui_context_row TYPE_EDIT_MENU[] = {
    {"Undo", "edit.undo"},
    {NULL, NULL},
    {"Cut", "edit.cut"},
    {"Copy", "edit.copy"},
    {"Paste", "edit.paste"},
    {"Select All", "select.all"},
    {NULL, NULL},
    {"Check Spelling…", "edit.checkSpelling"},
    {"Find and Replace Text…", "edit.findAndReplaceText"},
    {NULL, NULL},
    {"Faux Bold", "type.fauxBold"},
    {"Faux Italic", "type.fauxItalic"},
    {NULL, NULL},
    {"Rasterize Type", "type.rasterizeTypeLayer"},
    {"Create Work Path", "type.createWorkPath"},
    {"Convert to Shape", "type.convertToShape"},
    {NULL, NULL},
    {"Horizontal", "type.orientation.horizontal"},
    {"Vertical", "type.orientation.vertical"},
    {NULL, NULL},
    {"None", "type.antiAlias.none"},
    {"Sharp", "type.antiAlias.sharp"},
    {"Crisp", "type.antiAlias.crisp"},
    {"Strong", "type.antiAlias.strong"},
    {"Smooth", "type.antiAlias.smooth"},
    {"Windows LCD", "type.antiAlias.windowsLcd"},
    {"Windows", "type.antiAlias.windows"},
    {NULL, NULL},
    {"Convert to Paragraph Text", "type.convertToParagraphText"},
    {"Convert to Point Text", "type.convertToPointText"},
    {"Warp Text…", "type.warpText"},
};
static const pc_ui_context_row LAYER_MENU[] = {
    {"Blending Options…", "layer.layerStyle.blendingOptions"},
    {"Rename Layer…", "layer.renameLayer"},
    {"Duplicate Layer…", "layer.duplicate"},
    {"Delete Layer", "layer.delete"},
    {NULL, NULL},
    {"Convert to Smart Object", "layer.smartObjects.convertToSmartObject"},
    {"Rasterize Layer", "layer.rasterize.layer"},
    {"Create Clipping Mask", "layer.createClippingMask"},
    {"Release Clipping Mask", "layer.releaseClippingMask"},
    {NULL, NULL},
    {"Merge Down", "layer.mergeDown"},
    {"Merge Visible", "layer.mergeVisible"},
    {"Flatten Image", "layer.flattenImage"}};
static const pc_ui_context_row MASK_MENU[] = {{"Disable Layer Mask", "layer.layerMask.disable"},
                                              {"Enable Layer Mask", "layer.layerMask.enable"},
                                              {"Delete Layer Mask", "layer.layerMask.delete"},
                                              {"Apply Layer Mask", "layer.layerMask.apply"},
                                              {NULL, NULL},
                                              {"Select and Mask…", "select.selectAndMask"},
                                              {"Load Selection", "layer.layerMask.toSelection"}};
const pc_ui_context_row *pc_ui_context_rows(pc_ui_context_kind kind, size_t *count)
{
    switch (kind) {
    case PC_UI_CONTEXT_SELECTION:
        *count = sizeof SELECTION_MENU / sizeof *SELECTION_MENU;
        return SELECTION_MENU;
    case PC_UI_CONTEXT_EMPTY_SELECTION:
        *count = sizeof NO_SELECTION_MENU / sizeof *NO_SELECTION_MENU;
        return NO_SELECTION_MENU;
    case PC_UI_CONTEXT_PEN:
        *count = sizeof PEN_MENU / sizeof *PEN_MENU;
        return PEN_MENU;
    case PC_UI_CONTEXT_VIEW:
        *count = sizeof VIEW_MENU / sizeof *VIEW_MENU;
        return VIEW_MENU;
    case PC_UI_CONTEXT_TRANSFORM:
        *count = sizeof TRANSFORM_MENU / sizeof *TRANSFORM_MENU;
        return TRANSFORM_MENU;
    case PC_UI_CONTEXT_PASTEBOARD:
        *count = sizeof PASTEBOARD_MENU / sizeof *PASTEBOARD_MENU;
        return PASTEBOARD_MENU;
    case PC_UI_CONTEXT_TEXT:
        *count = sizeof TYPE_EDIT_MENU / sizeof *TYPE_EDIT_MENU;
        return TYPE_EDIT_MENU;
    case PC_UI_CONTEXT_LAYER:
        *count = sizeof LAYER_MENU / sizeof *LAYER_MENU;
        return LAYER_MENU;
    case PC_UI_CONTEXT_MASK:
        *count = sizeof MASK_MENU / sizeof *MASK_MENU;
        return MASK_MENU;
    default:
        *count = 0;
        return NULL;
    }
}
pc_status pc_ui_context_to_json(const pc_ui_context_menu *menu, yyjson_mut_doc *d, yyjson_mut_val **out,
                                pc_error *err)
{
    if (!menu->open) {
        *out = yyjson_mut_null(d);
        return *out ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "context menu");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d), *point = yyjson_mut_arr(d), *anchor = yyjson_mut_arr(d);
    bool ok = root && point && anchor && yyjson_mut_arr_add_real(d, point, menu->point[0]) &&
              yyjson_mut_arr_add_real(d, point, menu->point[1]) &&
              yyjson_mut_arr_add_real(d, anchor, menu->anchor[0]) &&
              yyjson_mut_arr_add_real(d, anchor, menu->anchor[1]) &&
              yyjson_mut_obj_add_val(d, root, "pos", point) &&
              yyjson_mut_obj_add_val(d, root, "anchor", anchor) &&
              yyjson_mut_obj_add_uint(d, root, "kind", menu->kind) &&
              yyjson_mut_obj_add_uint(d, root, "document", menu->document) &&
              yyjson_mut_obj_add_uint(d, root, "revision", menu->revision) &&
              yyjson_mut_obj_add_uint(d, root, "layer", menu->layer) &&
              yyjson_mut_obj_add_strcpy(d, root, "pathName", menu->path);
    if (!ok)
        return pc_fail(err, PC_ERR_NOMEM, "context menu");
    *out = root;
    return PC_OK;
}
