/* View menu command classifier (port of crate view_cmds::handles and the slot maps). */
#include "ui/view_cmds.h"

#include <string.h>

static bool in_list(const char *s, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

#define LIST(name, ...)                                                                            \
    static const char *const name[] = {__VA_ARGS__};                                               \
    enum { name##_N = (int)(sizeof(name) / sizeof(name[0])) }

LIST(SHOW_SLOTS, "layerEdges", "selectionEdges", "targetPath", "pixelGrid", "smartGuides",
     "brushPreview", "slices", "notes", "mesh", "editPins", "count", "canvasGuides",
     "artboardGuides");
LIST(SNAP_SLOTS, "guides", "grid", "layers", "slices", "documentBounds");
LIST(SCREEN_MODES, "standard", "fullScreenWithMenuBar", "fullScreen");
LIST(PIXEL_ASPECTS, "square", "d1DvNtsc", "d1DvPal", "d1DvNtscWidescreen", "hdv1080",
     "d1DvPalWidescreen", "dvcproHd1080", "anamorphic2To1");
LIST(FONT_PREVIEW, "small", "medium", "large", "extraLarge", "huge");
LIST(LANGUAGE, "defaultFeatures", "eastAsianFeatures", "middleEasternFeatures");
LIST(LAYOUTS, "tileAllVertically", "tileAllHorizontally", "twoUpVertical", "twoUpHorizontal",
     "threeUpVertical", "threeUpHorizontal", "threeUpStacked", "fourUp", "sixUp", "tile");
LIST(ARRANGE_EXTRA, "consolidateAllToTabs", "cascade", "floatInWindow", "floatAllInWindows",
     "newWindowForDocument", "matchZoom", "matchLocation", "matchRotation", "matchAll");
LIST(INDIVIDUAL, "view.extras", "view.twoHundredPercent", "view.printSize", "view.fitLayersOnScreen",
     "view.fitArtboardOnScreen", "window.panel.layerComps", "view.flipHorizontal",
     "view.pixelAspectRatioCorrection", "view.patternPreview", "view.pixelArtPreview",
     "view.rotateView", "view.resetView", "window.panel.layers", "window.panel.history",
     "window.panel.navigator", "window.panel.properties", "window.panel.color",
     "window.panel.actions", "window.panel.channels", "window.panel.paths", "window.panel.character",
     "window.panel.paragraph", "window.panel.info", "window.panel.histogram", "window.panel.swatches",
     "window.panel.adjustments", "type.panels.character", "type.panels.paragraph");

bool pc_ui_view_show_slot_exists(const char *key)
{
    return key != NULL && in_list(key, SHOW_SLOTS, (size_t)SHOW_SLOTS_N);
}

bool pc_ui_view_snap_slot_exists(const char *key)
{
    return key != NULL && in_list(key, SNAP_SLOTS, (size_t)SNAP_SLOTS_N);
}

/* strip prefix; returns the remainder or NULL. */
static const char *after(const char *id, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(id, prefix, n) == 0 ? id + n : NULL;
}

bool pc_ui_view_cmds_handles(const char *id)
{
    if (id == NULL) {
        return false;
    }
    const char *k;
    if ((k = after(id, "view.show.")) != NULL) {
        return pc_ui_view_show_slot_exists(k) || strcmp(k, "all") == 0;
    }
    if ((k = after(id, "view.snapTo.")) != NULL) {
        return pc_ui_view_snap_slot_exists(k) || strcmp(k, "all") == 0 || strcmp(k, "none") == 0;
    }
    if ((k = after(id, "view.screenMode.")) != NULL) {
        return in_list(k, SCREEN_MODES, (size_t)SCREEN_MODES_N) || strcmp(k, "cycle") == 0;
    }
    if ((k = after(id, "view.pixelAspectRatio.")) != NULL) {
        return in_list(k, PIXEL_ASPECTS, (size_t)PIXEL_ASPECTS_N);
    }
    if ((k = after(id, "type.fontPreviewSize.")) != NULL) {
        return in_list(k, FONT_PREVIEW, (size_t)FONT_PREVIEW_N);
    }
    if ((k = after(id, "type.languageOptions.")) != NULL) {
        return in_list(k, LANGUAGE, (size_t)LANGUAGE_N) ||
               strcmp(k, "middleEasternAndSouthAsianComposer") == 0;
    }
    if ((k = after(id, "window.arrange.")) != NULL) {
        return in_list(k, LAYOUTS, (size_t)LAYOUTS_N) || in_list(k, ARRANGE_EXTRA, (size_t)ARRANGE_EXTRA_N);
    }
    return in_list(id, INDIVIDUAL, (size_t)INDIVIDUAL_N);
}
