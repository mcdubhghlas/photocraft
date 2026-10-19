/* Persistent preferences, keyboard-shortcut / menu / toolbar customisation (port of
 * crates/engine/src/prefs.rs). See prefs.h.
 *
 * The canonical state is one yyjson document (the camelCase Preferences object). get/set/reset
 * operate on it by dotted path exactly as the reference does on a serde_json::Value, and the
 * typed reads (apply_prefs) read fields straight from the tree. Preferences are user/disk data:
 * every path here validates its input and defaults what is missing, and never crashes on a
 * corrupt settings file. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "engine_internal.h"
#include "ops/ops.h"
#include "prefs.h"

/* ========================================================================================== *
 * Enumerated choices
 * ========================================================================================== */

/* Each choice set is a NULL-terminated-less array of its JSON names in declaration order. */
#define CHOICES(name, ...)                                                                                   \
    static const char *const name[] = {__VA_ARGS__};                                                         \
    enum { name##_N = (int)(sizeof(name) / sizeof((name)[0])) }

CHOICES(C_unit, "pixels", "inches", "cm", "mm", "points", "picas", "percent");
CHOICES(C_typeUnit, "points", "pixels", "mm");
CHOICES(C_pointSize, "postScript", "traditional");
CHOICES(C_interp, "bicubicAutomatic", "nearestNeighbor", "bilinear", "bicubic", "bicubicSmoother",
        "bicubicSharper", "preserveDetails");
CHOICES(C_colorPicker, "adobe", "system");
CHOICES(C_theme, "pro", "proMedium", "proLightGray", "proLight", "studio", "studioLight", "classic",
        "solarizedDark", "adwaita", "adwaitaDark", "breezeLight", "breezeDark", "softDark", "softLight");
CHOICES(C_appearance, "auto", "dark", "light");
CHOICES(C_darkTheme, "pro", "proMedium", "studio", "solarizedDark", "adwaitaDark", "breezeDark", "softDark");
CHOICES(C_lightTheme, "studioLight", "proLightGray", "proLight", "classic", "adwaita", "breezeLight",
        "softLight");
CHOICES(C_canvasColor, "default", "black", "darkGray", "mediumGray", "lightGray", "custom");
CHOICES(C_canvasBorder, "dropShadow", "line", "none");
CHOICES(C_uiScale, "auto", "75", "80", "85", "90", "95", "100", "125", "150", "175", "200", "250", "300");
CHOICES(C_gpuBackend, "auto", "vulkan", "dx12", "metal", "gl", "cpu");
CHOICES(C_renderingMode, "auto", "gpu", "cpu");
CHOICES(C_linuxDisplay, "auto", "x11");
CHOICES(C_uiFontSize, "tiny", "small", "medium", "large");
CHOICES(C_logDest, "metadata", "textFile", "both");
CHOICES(C_logDetail, "sessionsOnly", "concise", "detailed");
CHOICES(C_ask, "always", "never", "ask");
CHOICES(C_quickExport, "png", "jpg", "gif", "webp");
CHOICES(C_exportLoc, "ask", "sameFolder");
CHOICES(C_exportMeta, "none", "copyright", "all");
CHOICES(C_paintCursor, "standard", "precise", "normalTip", "fullSizeTip");
CHOICES(C_rightClick, "brushPicker", "erase");
CHOICES(C_otherCursor, "standard", "precise");
CHOICES(C_checkerSize, "none", "small", "medium", "large");
CHOICES(C_checkerColors, "light", "medium", "dark", "red", "orange", "green", "blue", "purple", "custom");
CHOICES(C_lineStyle, "lines", "dashedLines", "dots");
CHOICES(C_textEngine, "modern", "eastAsian", "worldReady");
CHOICES(C_fontPreview, "off", "small", "medium", "large", "extraLarge", "huge");
CHOICES(C_rawColorSpace, "srgb", "adobeRgb", "proPhoto", "displayP3");
CHOICES(C_rawDepth, "8", "16");
CHOICES(C_rawSharpen, "none", "screen", "glossy", "matte");

const char *const *pc_pref_choices(const char *path, size_t *n)
{
    static const struct {
        const char *path;
        const char *const *names;
        int count;
    } table[] = {
        {"general.colorPicker", C_colorPicker, C_colorPicker_N},
        {"general.imageInterpolation", C_interp, C_interp_N},
        {"interface.theme", C_theme, C_theme_N},
        {"interface.appearanceMode", C_appearance, C_appearance_N},
        {"interface.darkTheme", C_darkTheme, C_darkTheme_N},
        {"interface.lightTheme", C_lightTheme, C_lightTheme_N},
        {"interface.canvasColor", C_canvasColor, C_canvasColor_N},
        {"interface.canvasBorder", C_canvasBorder, C_canvasBorder_N},
        {"interface.uiScale", C_uiScale, C_uiScale_N},
        {"interface.uiFontSize", C_uiFontSize, C_uiFontSize_N},
        {"historyLog.destination", C_logDest, C_logDest_N},
        {"historyLog.detail", C_logDetail, C_logDetail_N},
        {"fileHandling.imagePreviews", C_ask, C_ask_N},
        {"fileHandling.maximizePsdCompatibility", C_ask, C_ask_N},
        {"export.quickExportFormat", C_quickExport, C_quickExport_N},
        {"export.quickExportLocation", C_exportLoc, C_exportLoc_N},
        {"export.metadata", C_exportMeta, C_exportMeta_N},
        {"cursors.painting", C_paintCursor, C_paintCursor_N},
        {"cursors.other", C_otherCursor, C_otherCursor_N},
        {"tools.rightClickWithPaintingTools", C_rightClick, C_rightClick_N},
        {"transparencyAndGamut.gridSize", C_checkerSize, C_checkerSize_N},
        {"transparencyAndGamut.gridColors", C_checkerColors, C_checkerColors_N},
        {"unitsAndRulers.rulers", C_unit, C_unit_N},
        {"guidesGridAndSlices.gridUnit", C_unit, C_unit_N},
        {"unitsAndRulers.typeUnits", C_typeUnit, C_typeUnit_N},
        {"unitsAndRulers.pointSize", C_pointSize, C_pointSize_N},
        {"guidesGridAndSlices.guideStyle", C_lineStyle, C_lineStyle_N},
        {"guidesGridAndSlices.gridStyle", C_lineStyle, C_lineStyle_N},
        {"type.textEngine", C_textEngine, C_textEngine_N},
        {"type.fontPreview", C_fontPreview, C_fontPreview_N},
        {"rawDefaults.colorSpace", C_rawColorSpace, C_rawColorSpace_N},
        {"rawDefaults.bitDepth", C_rawDepth, C_rawDepth_N},
        {"rawDefaults.sharpenFor", C_rawSharpen, C_rawSharpen_N},
        {"performance.gpuBackend", C_gpuBackend, C_gpuBackend_N},
        {"performance.linuxDisplayServer", C_linuxDisplay, C_linuxDisplay_N},
        {"performance.renderingMode", C_renderingMode, C_renderingMode_N},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(table[i].path, path) == 0) {
            *n = (size_t)table[i].count;
            return table[i].names;
        }
    }
    return NULL;
}

bool pc_pref_range(const char *path, double *lo, double *hi)
{
    static const struct {
        const char *path;
        double lo, hi;
    } table[] = {
        {"fileHandling.autosaveMinutes", 1.0, 240.0},
        {"fileHandling.recentFileCount", 0.0, 100.0},
        {"fileHandling.rasterizeSvgGroupsDeeperThan", 0.0, (double)PC_MAX_GROUP_DEPTH},
        {"interface.notificationDurationSeconds", 1.0, 120.0},
        {"export.jpegQuality", 1.0, 100.0},
        {"export.webpQuality", 1.0, 100.0},
        {"performance.memoryUsageMb", 256.0, 1048576.0},
        {"performance.historyStates", 1.0, 1000.0},
        {"performance.cacheLevels", 1.0, 8.0},
        {"performance.cacheTileSize", 256.0, 16384.0},
        {"performance.effectCacheMb", 16.0, 65536.0},
        {"transparencyAndGamut.gamutWarningOpacity", 1.0, 100.0},
        {"unitsAndRulers.columnWidth", 0.0, 10000.0},
        {"unitsAndRulers.gutter", 0.0, 10000.0},
        {"unitsAndRulers.printResolution", 1.0, 10000.0},
        {"unitsAndRulers.screenResolution", 1.0, 10000.0},
        {"rawDefaults.resolution", 1.0, 10000.0},
        {"guidesGridAndSlices.gridlineEvery", 0.001, 10000.0},
        {"guidesGridAndSlices.subdivisions", 1.0, 100.0},
        {"type.recentFonts", 0.0, 50.0},
        {"integrations.controlPort", 0.0, 65535.0},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(table[i].path, path) == 0) {
            *lo = table[i].lo;
            *hi = table[i].hi;
            return true;
        }
    }
    return false;
}

static const char *leaf_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot != NULL ? dot + 1 : path;
}

bool pc_pref_is_color(const char *path)
{
    size_t cn;
    if (pc_pref_choices(path, &cn) != NULL) {
        return false;
    }
    if (strncmp(path, "shortcuts.", 10) == 0 || strncmp(path, "menus.", 6) == 0) {
        return false;
    }
    const char *leaf = leaf_of(path);
    static const char *const colors[] = {"guideColor",        "smartGuideColor",   "gridColor",
                                         "sliceColor",        "canvasCustomColor", "brushPreviewColor",
                                         "gamutWarningColor", "customLight",       "customDark"};
    for (size_t i = 0; i < sizeof colors / sizeof colors[0]; i++) {
        if (strcmp(leaf, colors[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool pc_pref_is_hidden(const char *path)
{
    static const char *const hidden[] = {"general.colorPicker",
                                         "general.animatedZoom",
                                         "general.zoomResizesWindows",
                                         "workspace.autoShowHiddenPanels",
                                         "workspace.enableFloatingDocumentWindowDocking",
                                         "fileHandling.saveInBackground",
                                         "performance.cacheLevels",
                                         "performance.effectCacheMb",
                                         "performance.legacyCompositing",
                                         "scratchDisks.disks",
                                         "unitsAndRulers.columnWidth",
                                         "unitsAndRulers.gutter",
                                         "plugIns.showExtensionPanels",
                                         "plugIns.allowScriptsToConnect",
                                         "plugIns.generatorEnabled",
                                         "type.textEngine",
                                         "type.recentFonts",
                                         "enhancedControls.scrubbySliderAcceleration",
                                         "enhancedControls.touchGestures",
                                         "rawDefaults.colorSpace",
                                         "rawDefaults.bitDepth",
                                         "rawDefaults.resolution",
                                         "rawDefaults.sharpenFor",
                                         "rawDefaults.openAsSmartObject",
                                         "rawDefaults.applyAutoTone",
                                         "integrations.allowAgentControl",
                                         "integrations.controlPort"};
    for (size_t i = 0; i < sizeof hidden / sizeof hidden[0]; i++) {
        if (strcmp(hidden[i], path) == 0) {
            return true;
        }
    }
#if !defined(__linux__)
    if (strcmp(path, "performance.linuxDisplayServer") == 0) {
        return true;
    }
#endif
    return false;
}

/* ========================================================================================== *
 * Dialog sections (Photoshop's order)
 * ========================================================================================== */

typedef struct {
    const char *id;
    const char *title;
} pc_pref_section;

static const pc_pref_section SECTIONS[] = {
    {"general", "General"},
    {"interface", "Interface"},
    {"workspace", "Workspace"},
    {"tools", "Tools"},
    {"historyLog", "History Log"},
    {"fileHandling", "File Handling"},
    {"export", "Export"},
    {"performance", "Performance"},
    {"scratchDisks", "Scratch Disks"},
    {"cursors", "Cursors"},
    {"transparencyAndGamut", "Transparency & Gamut"},
    {"unitsAndRulers", "Units & Rulers"},
    {"guidesGridAndSlices", "Guides, Grid & Slices"},
    {"plugIns", "Plug-ins"},
    {"type", "Type"},
    {"enhancedControls", "Enhanced Controls"},
    {"rawDefaults", "Camera Raw Defaults"},
    {"integrations", "Integrations"},
};
enum { SECTIONS_N = (int)(sizeof(SECTIONS) / sizeof(SECTIONS[0])) };

static bool is_section(const char *path)
{
    for (int i = 0; i < SECTIONS_N; i++) {
        if (strcmp(SECTIONS[i].id, path) == 0) {
            return true;
        }
    }
    return false;
}

/* Temporary tools (held keys): bindable like shortcuts but held, not commands. */
static const pc_engine_temporary_tool TEMPORARY_TOOLS[] = {
    {"tools.temporary.hand", "Hand Tool (hold)", "Space"},
    {"tools.temporary.zoomIn", "Zoom In (hold, drag to scrub)", "Cmd+Space"},
    {"tools.temporary.zoomOut", "Zoom Out (hold)", "Alt+Space"},
    {"tools.temporary.erase", "Erase with Brush or Pencil (hold)", "`"},
};
enum { TEMPORARY_TOOLS_N = (int)(sizeof(TEMPORARY_TOOLS) / sizeof(TEMPORARY_TOOLS[0])) };

const pc_engine_temporary_tool *pc_engine_temporary_tools(size_t *n)
{
    if (n != NULL) {
        *n = (size_t)TEMPORARY_TOOLS_N;
    }
    return TEMPORARY_TOOLS;
}

/* ========================================================================================== *
 * Default tree
 * ========================================================================================== */

static yyjson_mut_val *obj(yyjson_mut_doc *d)
{
    return yyjson_mut_obj(d);
}

static void section_general(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_str(d, o, "colorPicker", "adobe");
    yyjson_mut_obj_add_str(d, o, "imageInterpolation", "bicubicAutomatic");
    yyjson_mut_obj_add_bool(d, o, "beepWhenDone", false);
    yyjson_mut_obj_add_bool(d, o, "exportClipboard", true);
    yyjson_mut_obj_add_bool(d, o, "resizeImageDuringPlace", true);
    yyjson_mut_obj_add_bool(d, o, "alwaysCreateSmartObjectsWhenPlacing", true);
    yyjson_mut_obj_add_bool(d, o, "zoomWithScrollWheel", false);
    yyjson_mut_obj_add_bool(d, o, "animatedZoom", true);
    yyjson_mut_obj_add_bool(d, o, "zoomResizesWindows", false);
    yyjson_mut_obj_add_bool(d, o, "useLegacyFreeTransform", false);
    yyjson_mut_obj_add_bool(d, o, "autoShowHomeScreen", true);
}

static void section_interface(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_str(d, o, "theme", "proMedium");
    yyjson_mut_obj_add_str(d, o, "appearanceMode", "dark");
    yyjson_mut_obj_add_str(d, o, "darkTheme", "proMedium");
    yyjson_mut_obj_add_str(d, o, "lightTheme", "studioLight");
    yyjson_mut_obj_add_val(d, o, "favoriteThemes", yyjson_mut_arr(d));
    yyjson_mut_obj_add_str(d, o, "canvasColor", "default");
    yyjson_mut_obj_add_str(d, o, "canvasCustomColor", "#282828");
    yyjson_mut_obj_add_bool(d, o, "showCanvasDots", true);
    yyjson_mut_obj_add_str(d, o, "canvasBorder", "dropShadow");
    yyjson_mut_obj_add_str(d, o, "uiScale", "auto");
    yyjson_mut_obj_add_str(d, o, "language", "auto");
    yyjson_mut_obj_add_str(d, o, "uiFontSize", "small");
    yyjson_mut_obj_add_bool(d, o, "showChannelsInColor", false);
    yyjson_mut_obj_add_bool(d, o, "dynamicColorSliders", true);
    yyjson_mut_obj_add_bool(d, o, "showMenuColors", true);
    yyjson_mut_obj_add_bool(d, o, "showTooltips", true);
    yyjson_mut_obj_add_bool(d, o, "enableAnimations", true);
    yyjson_mut_obj_add_bool(d, o, "showBoundingBoxWhenDraggingLayer", false);
    yyjson_mut_obj_add_bool(d, o, "systemTitleBar", false);
    yyjson_mut_obj_add_bool(d, o, "notificationAutoHide", true);
    yyjson_mut_obj_add_uint(d, o, "notificationDurationSeconds", 6);
}

static void section_workspace(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_bool(d, o, "autoCollapseIconPanels", false);
    yyjson_mut_obj_add_bool(d, o, "autoShowHiddenPanels", true);
    yyjson_mut_obj_add_bool(d, o, "openDocumentsAsTabs", true);
    yyjson_mut_obj_add_bool(d, o, "enableFloatingDocumentWindowDocking", true);
    yyjson_mut_obj_add_bool(d, o, "largeTabs", false);
    yyjson_mut_obj_add_bool(d, o, "enableNarrowOptionsBar", false);
    yyjson_mut_obj_add_bool(d, o, "rememberWorkspaceChanges", true);
}

static void section_tools(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_bool(d, o, "showTooltips", true);
    yyjson_mut_obj_add_bool(d, o, "useShiftKeyForToolSwitch", true);
    yyjson_mut_obj_add_bool(d, o, "zoomClickedPointToCenter", false);
    yyjson_mut_obj_add_bool(d, o, "enableFlickPanning", true);
    yyjson_mut_obj_add_bool(d, o, "varyRoundBrushHardnessOnHud", true);
    yyjson_mut_obj_add_bool(d, o, "snapVectorToolsAndTransformsToPixelGrid", true);
    yyjson_mut_obj_add_bool(d, o, "showTransformationValues", true);
    yyjson_mut_obj_add_bool(d, o, "showHighlightOnRollover", true);
    yyjson_mut_obj_add_bool(d, o, "overscroll", true);
    yyjson_mut_obj_add_bool(d, o, "doubleClickLayerMaskLaunchesSelectAndMask", true);
    yyjson_mut_obj_add_str(d, o, "rightClickWithPaintingTools", "brushPicker");
    yyjson_mut_obj_add_bool(d, o, "useTabletPressure", true);
    yyjson_mut_val *curve = yyjson_mut_arr(d);
    for (int i = 0; i < 2; i++) {
        yyjson_mut_val *p = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, p, i == 0 ? 0.0 : 1.0);
        yyjson_mut_arr_add_real(d, p, i == 0 ? 0.0 : 1.0);
        yyjson_mut_arr_add_val(curve, p);
    }
    yyjson_mut_obj_add_val(d, o, "pressureCurve", curve);
}

static void section_history_log(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_bool(d, o, "enabled", false);
    yyjson_mut_obj_add_str(d, o, "destination", "metadata");
    yyjson_mut_obj_add_str(d, o, "filePath", "");
    yyjson_mut_obj_add_str(d, o, "detail", "concise");
}

static void section_file_handling(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_str(d, o, "imagePreviews", "always");
    yyjson_mut_obj_add_bool(d, o, "lowercaseExtension", true);
    yyjson_mut_obj_add_bool(d, o, "saveInBackground", true);
    yyjson_mut_obj_add_bool(d, o, "autosave", true);
    yyjson_mut_obj_add_uint(d, o, "autosaveMinutes", 10);
    yyjson_mut_obj_add_bool(d, o, "recoverOnLaunch", true);
    yyjson_mut_obj_add_bool(d, o, "ignoreExifProfileTag", false);
    yyjson_mut_obj_add_bool(d, o, "askBeforeSavingLayeredTiff", true);
    yyjson_mut_obj_add_str(d, o, "maximizePsdCompatibility", "always");
    yyjson_mut_obj_add_uint(d, o, "rasterizeSvgGroupsDeeperThan", (uint64_t)PC_MAX_GROUP_DEPTH);
    yyjson_mut_obj_add_uint(d, o, "recentFileCount", 20);
    yyjson_mut_obj_add_val(d, o, "recentFiles", yyjson_mut_arr(d));
}

static void section_export(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_str(d, o, "quickExportFormat", "png");
    yyjson_mut_obj_add_str(d, o, "quickExportLocation", "ask");
    yyjson_mut_obj_add_uint(d, o, "jpegQuality", 85);
    yyjson_mut_obj_add_bool(d, o, "webpLossless", true);
    yyjson_mut_obj_add_uint(d, o, "webpQuality", 85);
    yyjson_mut_obj_add_str(d, o, "metadata", "copyright");
    yyjson_mut_obj_add_bool(d, o, "convertToSrgb", true);
}

static void section_performance(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_uint(d, o, "memoryUsageMb", 8192);
    yyjson_mut_obj_add_uint(d, o, "historyStates", 50);
    yyjson_mut_obj_add_uint(d, o, "cacheLevels", 4);
    yyjson_mut_obj_add_uint(d, o, "cacheTileSize", 8192);
    yyjson_mut_obj_add_bool(d, o, "useGpu", true);
    yyjson_mut_obj_add_val(d, o, "renderingMode", yyjson_mut_null(d));
    yyjson_mut_obj_add_str(d, o, "gpuBackend", "auto");
    yyjson_mut_obj_add_bool(d, o, "lowResolutionPreviews", true);
    yyjson_mut_obj_add_str(d, o, "linuxDisplayServer", "auto");
    yyjson_mut_obj_add_uint(d, o, "effectCacheMb", 768);
    yyjson_mut_obj_add_bool(d, o, "legacyCompositing", false);
}

static void section_scratch_disks(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_val *disks = yyjson_mut_arr(d);
    yyjson_mut_val *disk = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, disk, "path", "(system temp)");
    yyjson_mut_obj_add_bool(d, disk, "enabled", true);
    yyjson_mut_arr_add_val(disks, disk);
    yyjson_mut_obj_add_val(d, o, "disks", disks);
}

static void section_cursors(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_str(d, o, "painting", "normalTip");
    yyjson_mut_obj_add_bool(d, o, "showCrosshairInBrushTip", false);
    yyjson_mut_obj_add_bool(d, o, "showOnlyCrosshairWhilePainting", false);
    yyjson_mut_obj_add_str(d, o, "other", "standard");
    yyjson_mut_obj_add_str(d, o, "brushPreviewColor", "#ff0000");
}

static void section_transparency(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_str(d, o, "gridSize", "medium");
    yyjson_mut_obj_add_str(d, o, "gridColors", "light");
    yyjson_mut_obj_add_str(d, o, "customLight", "#ffffff");
    yyjson_mut_obj_add_str(d, o, "customDark", "#cccccc");
    yyjson_mut_obj_add_str(d, o, "gamutWarningColor", "#808080");
    yyjson_mut_obj_add_uint(d, o, "gamutWarningOpacity", 100);
}

static void section_units(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_str(d, o, "rulers", "pixels");
    yyjson_mut_obj_add_str(d, o, "typeUnits", "points");
    yyjson_mut_obj_add_real(d, o, "columnWidth", 180.0);
    yyjson_mut_obj_add_real(d, o, "gutter", 12.0);
    yyjson_mut_obj_add_real(d, o, "printResolution", 300.0);
    yyjson_mut_obj_add_real(d, o, "screenResolution", 72.0);
    yyjson_mut_obj_add_str(d, o, "pointSize", "postScript");
    yyjson_mut_obj_add_bool(d, o, "showRulers", false);
}

static void section_guides(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_str(d, o, "guideColor", "#4affff");
    yyjson_mut_obj_add_str(d, o, "guideStyle", "lines");
    yyjson_mut_obj_add_str(d, o, "smartGuideColor", "#ff00ff");
    yyjson_mut_obj_add_str(d, o, "gridColor", "#8c8c8c");
    yyjson_mut_obj_add_str(d, o, "gridStyle", "lines");
    yyjson_mut_obj_add_real(d, o, "gridlineEvery", 1.0);
    yyjson_mut_obj_add_str(d, o, "gridUnit", "inches");
    yyjson_mut_obj_add_uint(d, o, "subdivisions", 4);
    yyjson_mut_obj_add_str(d, o, "sliceColor", "#38b5ff");
    yyjson_mut_obj_add_bool(d, o, "showSliceNumbers", true);
}

static void section_plugins(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_bool(d, o, "useAdditionalPluginsFolder", false);
    yyjson_mut_obj_add_str(d, o, "additionalPluginsFolder", "");
    yyjson_mut_obj_add_bool(d, o, "showExtensionPanels", true);
    yyjson_mut_obj_add_bool(d, o, "allowScriptsToConnect", false);
    yyjson_mut_obj_add_bool(d, o, "generatorEnabled", false);
}

static void section_type(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_bool(d, o, "smartQuotes", true);
    yyjson_mut_obj_add_bool(d, o, "missingGlyphProtection", true);
    yyjson_mut_obj_add_bool(d, o, "showFontNamesInEnglish", true);
    yyjson_mut_obj_add_bool(d, o, "useEscToCommit", true);
    yyjson_mut_obj_add_str(d, o, "textEngine", "worldReady");
    yyjson_mut_obj_add_str(d, o, "fontPreview", "medium");
    yyjson_mut_obj_add_bool(d, o, "fillNewTypeLayersWithPlaceholder", true);
    yyjson_mut_obj_add_uint(d, o, "recentFonts", 10);
}

static void section_enhanced(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_bool(d, o, "scrubbySliderAcceleration", true);
    yyjson_mut_obj_add_bool(d, o, "touchGestures", true);
    yyjson_mut_obj_add_bool(d, o, "zoomWithTrackpadPinch", true);
    yyjson_mut_obj_add_bool(d, o, "rotateViewWithTrackpad", false);
}

static void section_raw(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_str(d, o, "colorSpace", "adobeRgb");
    yyjson_mut_obj_add_str(d, o, "bitDepth", "16");
    yyjson_mut_obj_add_real(d, o, "resolution", 300.0);
    yyjson_mut_obj_add_str(d, o, "sharpenFor", "none");
    yyjson_mut_obj_add_bool(d, o, "openAsSmartObject", false);
    yyjson_mut_obj_add_bool(d, o, "applyAutoTone", false);
    yyjson_mut_obj_add_bool(d, o, "openInCameraRaw", true);
}

static void section_integrations(yyjson_mut_doc *d, yyjson_mut_val *o)
{
    yyjson_mut_obj_add_bool(d, o, "allowAgentControl", true);
    yyjson_mut_obj_add_uint(d, o, "controlPort", 0);
}

/* Build the full default Preferences object into doc (returns its root object, or NULL). */
static yyjson_mut_val *default_tree(yyjson_mut_doc *d)
{
    yyjson_mut_val *root = yyjson_mut_obj(d);
    if (root == NULL) {
        return NULL;
    }
    struct {
        const char *id;
        void (*fill)(yyjson_mut_doc *, yyjson_mut_val *);
    } builders[] = {
        {"general", section_general},
        {"interface", section_interface},
        {"workspace", section_workspace},
        {"tools", section_tools},
        {"historyLog", section_history_log},
        {"fileHandling", section_file_handling},
        {"export", section_export},
        {"performance", section_performance},
        {"scratchDisks", section_scratch_disks},
        {"cursors", section_cursors},
        {"transparencyAndGamut", section_transparency},
        {"unitsAndRulers", section_units},
        {"guidesGridAndSlices", section_guides},
        {"plugIns", section_plugins},
        {"type", section_type},
        {"enhancedControls", section_enhanced},
        {"rawDefaults", section_raw},
        {"integrations", section_integrations},
    };
    for (size_t i = 0; i < sizeof builders / sizeof builders[0]; i++) {
        yyjson_mut_val *o = obj(d);
        builders[i].fill(d, o);
        yyjson_mut_obj_add_val(d, root, builders[i].id, o);
    }
    /* Top-level free-form / map fields. */
    yyjson_mut_obj_add_val(d, root, "shortcuts", yyjson_mut_obj(d));
    yyjson_mut_val *menus = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, menus, "hidden", yyjson_mut_arr(d));
    yyjson_mut_obj_add_val(d, menus, "colors", yyjson_mut_obj(d));
    yyjson_mut_obj_add_val(d, root, "menus", menus);
    yyjson_mut_val *toolbar = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, toolbar, "hidden", yyjson_mut_arr(d));
    yyjson_mut_obj_add_val(d, toolbar, "order", yyjson_mut_arr(d));
    yyjson_mut_obj_add_val(d, root, "toolbar", toolbar);
    yyjson_mut_obj_add_val(d, root, "userDictionary", yyjson_mut_arr(d));
    yyjson_mut_obj_add_val(d, root, "workspaces", yyjson_mut_obj(d));
    yyjson_mut_obj_add_bool(d, root, "workspaceLocked", false);
    yyjson_mut_obj_add_val(d, root, "panelLayout", yyjson_mut_null(d));
    yyjson_mut_obj_add_val(d, root, "dialogs", yyjson_mut_obj(d));
    yyjson_mut_obj_add_val(d, root, "brushPicker", yyjson_mut_null(d));
    yyjson_mut_obj_add_val(d, root, "toolOptions", yyjson_mut_null(d));
    yyjson_mut_obj_add_val(d, root, "viewExtras", yyjson_mut_null(d));
    yyjson_mut_val *se = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, se, "enabled", false);
    yyjson_mut_obj_add_val(d, se, "bindings", yyjson_mut_arr(d));
    yyjson_mut_obj_add_val(d, root, "scriptEvents", se);
    return root;
}

/* ========================================================================================== *
 * PrefsStore lifecycle
 * ========================================================================================== */

void pc_engine_prefs_init(pc_engine_prefs *p)
{
    p->rev = 0;
    p->values = yyjson_mut_doc_new(NULL);
    if (p->values == NULL) {
        return;
    }
    yyjson_mut_val *root = default_tree(p->values);
    if (root == NULL) {
        yyjson_mut_doc_free(p->values);
        p->values = NULL;
        return;
    }
    yyjson_mut_doc_set_root(p->values, root);
}

void pc_engine_prefs_fini(pc_engine_prefs *p)
{
    if (p == NULL) {
        return;
    }
    yyjson_mut_doc_free(p->values);
    p->values = NULL;
}

/* Reset to defaults in place; keeps the same doc object where possible. false on OOM. */
static bool prefs_reset_all(pc_engine_prefs *p)
{
    yyjson_mut_doc *nd = yyjson_mut_doc_new(NULL);
    if (nd == NULL) {
        return false;
    }
    yyjson_mut_val *root = default_tree(nd);
    if (root == NULL) {
        yyjson_mut_doc_free(nd);
        return false;
    }
    yyjson_mut_doc_set_root(nd, root);
    yyjson_mut_doc_free(p->values);
    p->values = nd;
    return true;
}

/* ========================================================================================== *
 * Units
 * ========================================================================================== */

double pc_pref_unit_from_px(pc_pref_unit u, double px, double dpi, double extent, double ppi)
{
    dpi = dpi > 1e-6 ? dpi : 1e-6;
    switch (u) {
    case PC_UNIT_PIXELS:
        return px;
    case PC_UNIT_INCHES:
        return px / dpi;
    case PC_UNIT_CENTIMETERS:
        return px / dpi * 2.54;
    case PC_UNIT_MILLIMETERS:
        return px / dpi * 25.4;
    case PC_UNIT_POINTS:
        return px / dpi * ppi;
    case PC_UNIT_PICAS:
        return px / dpi * ppi / 12.0;
    case PC_UNIT_PERCENT:
        return px / (extent > 1e-6 ? extent : 1e-6) * 100.0;
    }
    return px;
}

double pc_pref_unit_to_px(pc_pref_unit u, double v, double dpi, double extent, double ppi)
{
    dpi = dpi > 1e-6 ? dpi : 1e-6;
    switch (u) {
    case PC_UNIT_PIXELS:
        return v;
    case PC_UNIT_INCHES:
        return v * dpi;
    case PC_UNIT_CENTIMETERS:
        return v / 2.54 * dpi;
    case PC_UNIT_MILLIMETERS:
        return v / 25.4 * dpi;
    case PC_UNIT_POINTS:
        return v / (ppi > 1e-6 ? ppi : 1e-6) * dpi;
    case PC_UNIT_PICAS:
        return v * 12.0 / (ppi > 1e-6 ? ppi : 1e-6) * dpi;
    case PC_UNIT_PERCENT:
        return v * extent / 100.0;
    }
    return v;
}

const char *pc_pref_unit_suffix(pc_pref_unit u)
{
    switch (u) {
    case PC_UNIT_PIXELS:
        return "px";
    case PC_UNIT_INCHES:
        return "in";
    case PC_UNIT_CENTIMETERS:
        return "cm";
    case PC_UNIT_MILLIMETERS:
        return "mm";
    case PC_UNIT_POINTS:
        return "pt";
    case PC_UNIT_PICAS:
        return "pica";
    case PC_UNIT_PERCENT:
        return "%";
    }
    return "px";
}

size_t pc_pref_unit_decimals(pc_pref_unit u)
{
    switch (u) {
    case PC_UNIT_PIXELS:
        return 0;
    case PC_UNIT_POINTS:
    case PC_UNIT_PERCENT:
        return 1;
    case PC_UNIT_MILLIMETERS:
    case PC_UNIT_CENTIMETERS:
    case PC_UNIT_PICAS:
        return 2;
    case PC_UNIT_INCHES:
        return 3;
    }
    return 0;
}

static bool ascii_eq_ci(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

bool pc_pref_unit_from_suffix(const char *suffix, size_t len, pc_pref_unit *out)
{
    char buf[32];
    if (len >= sizeof buf) {
        return false;
    }
    memcpy(buf, suffix, len);
    buf[len] = '\0';
    static const struct {
        const char *name;
        pc_pref_unit unit;
    } table[] = {
        {"px", PC_UNIT_PIXELS},
        {"pixel", PC_UNIT_PIXELS},
        {"pixels", PC_UNIT_PIXELS},
        {"in", PC_UNIT_INCHES},
        {"inch", PC_UNIT_INCHES},
        {"inches", PC_UNIT_INCHES},
        {"\"", PC_UNIT_INCHES},
        {"cm", PC_UNIT_CENTIMETERS},
        {"centimeter", PC_UNIT_CENTIMETERS},
        {"centimeters", PC_UNIT_CENTIMETERS},
        {"centimetre", PC_UNIT_CENTIMETERS},
        {"centimetres", PC_UNIT_CENTIMETERS},
        {"mm", PC_UNIT_MILLIMETERS},
        {"millimeter", PC_UNIT_MILLIMETERS},
        {"millimeters", PC_UNIT_MILLIMETERS},
        {"millimetre", PC_UNIT_MILLIMETERS},
        {"millimetres", PC_UNIT_MILLIMETERS},
        {"pt", PC_UNIT_POINTS},
        {"point", PC_UNIT_POINTS},
        {"points", PC_UNIT_POINTS},
        {"pc", PC_UNIT_PICAS},
        {"pica", PC_UNIT_PICAS},
        {"picas", PC_UNIT_PICAS},
        {"%", PC_UNIT_PERCENT},
        {"percent", PC_UNIT_PERCENT},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (ascii_eq_ci(buf, table[i].name)) {
            *out = table[i].unit;
            return true;
        }
    }
    return false;
}

size_t pc_pref_fmt_decimals(double v, size_t decimals, char *out, size_t cap)
{
    double p = pow(10.0, (double)decimals);
    double r = round(v * p) / p;
    if (r == 0.0) {
        r = 0.0; /* clears -0.0 */
    }
    char tmp[64];
    int w = snprintf(tmp, sizeof tmp, "%.*f", (int)decimals, r);
    if (w < 0) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    /* Only a fractional part may lose zeros. */
    if (strchr(tmp, '.') != NULL) {
        size_t n = strlen(tmp);
        while (n > 0 && tmp[n - 1] == '0') {
            n--;
        }
        if (n > 0 && tmp[n - 1] == '.') {
            n--;
        }
        tmp[n] = '\0';
    }
    size_t len = strlen(tmp);
    if (cap > 0) {
        size_t copy = len < cap - 1 ? len : cap - 1;
        memcpy(out, tmp, copy);
        out[copy] = '\0';
    }
    return len;
}

bool pc_pref_parse_hex(const char *s, size_t len, uint8_t out[3])
{
    /* trim leading '#' and surrounding whitespace, as Rust's trim/trim_start_matches('#'). */
    while (len > 0 && isspace((unsigned char)s[0])) {
        s++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        len--;
    }
    if (len > 0 && s[0] == '#') {
        s++;
        len--;
    }
    if (len != 6) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        int hi = s[i * 2], lo = s[i * 2 + 1];
        int dh, dl;
        if (hi >= '0' && hi <= '9') {
            dh = hi - '0';
        } else if (hi >= 'a' && hi <= 'f') {
            dh = hi - 'a' + 10;
        } else if (hi >= 'A' && hi <= 'F') {
            dh = hi - 'A' + 10;
        } else {
            return false;
        }
        if (lo >= '0' && lo <= '9') {
            dl = lo - '0';
        } else if (lo >= 'a' && lo <= 'f') {
            dl = lo - 'a' + 10;
        } else if (lo >= 'A' && lo <= 'F') {
            dl = lo - 'A' + 10;
        } else {
            return false;
        }
        out[i] = (uint8_t)(dh * 16 + dl);
    }
    return true;
}

/* ========================================================================================== *
 * Pressure curve
 * ========================================================================================== */

void pc_pressure_curve_init(pc_pressure_curve *c, const float (*points)[2], size_t npts)
{
    /* Collect finite points clamped to 0..1, sorted by input (total order), one per input
     * (later wins), capped at MAX_POINTS. */
    float xs[PC_PRESSURE_CURVE_MAX_POINTS + 64];
    float ys[PC_PRESSURE_CURVE_MAX_POINTS + 64];
    size_t m = 0;
    for (size_t i = 0; i < npts && m < sizeof xs / sizeof xs[0]; i++) {
        float x = points[i][0], y = points[i][1];
        if (!isfinite(x) || !isfinite(y)) {
            continue;
        }
        x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        y = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
        xs[m] = x;
        ys[m] = y;
        m++;
    }
    /* Stable insertion sort by x using total_cmp order (ties keep insertion order). */
    for (size_t i = 1; i < m; i++) {
        float kx = xs[i], ky = ys[i];
        size_t j = i;
        while (j > 0 && xs[j - 1] > kx) {
            xs[j] = xs[j - 1];
            ys[j] = ys[j - 1];
            j--;
        }
        xs[j] = kx;
        ys[j] = ky;
    }
    /* Dedup by input (within 1e-6): keep the later point's output. */
    size_t w = 0;
    for (size_t i = 0; i < m; i++) {
        if (w > 0 && fabsf(xs[i] - xs[w - 1]) < 1e-6f) {
            ys[w - 1] = ys[i]; /* later output wins */
        } else {
            xs[w] = xs[i];
            ys[w] = ys[i];
            w++;
        }
    }
    m = w;
    if (m > PC_PRESSURE_CURVE_MAX_POINTS) {
        m = PC_PRESSURE_CURVE_MAX_POINTS;
    }
    if (m < 2) {
        xs[0] = 0.0f;
        ys[0] = 0.0f;
        xs[1] = 1.0f;
        ys[1] = 1.0f;
        m = 2;
    }
    for (size_t i = 0; i < m; i++) {
        c->xs[i] = xs[i];
        c->ys[i] = ys[i];
    }
    c->n = m;
    /* Outputs non-decreasing. */
    for (size_t i = 1; i < m; i++) {
        if (c->ys[i] < c->ys[i - 1]) {
            c->ys[i] = c->ys[i - 1];
        }
    }
    /* Fritsch-Carlson monotone tangents. */
    size_t n = m;
    float dsec[PC_PRESSURE_CURVE_MAX_POINTS];
    for (size_t k = 0; k + 1 < n; k++) {
        dsec[k] = (c->ys[k + 1] - c->ys[k]) / (c->xs[k + 1] - c->xs[k]);
    }
    for (size_t i = 0; i < n; i++) {
        c->ms[i] = 0.0f;
    }
    c->ms[0] = dsec[0];
    c->ms[n - 1] = dsec[n - 2];
    for (size_t k = 1; k + 1 < n; k++) {
        c->ms[k] = (dsec[k - 1] * dsec[k] <= 0.0f) ? 0.0f : (dsec[k - 1] + dsec[k]) / 2.0f;
    }
    for (size_t k = 0; k + 1 < n; k++) {
        if (dsec[k] == 0.0f) {
            c->ms[k] = 0.0f;
            c->ms[k + 1] = 0.0f;
            continue;
        }
        float a = c->ms[k] / dsec[k];
        float b = c->ms[k + 1] / dsec[k];
        float s = a * a + b * b;
        if (s > 9.0f) {
            float t = 3.0f / sqrtf(s);
            c->ms[k] = t * a * dsec[k];
            c->ms[k + 1] = t * b * dsec[k];
        }
    }
}

float pc_pressure_curve_eval(const pc_pressure_curve *c, float x)
{
    x = isnan(x) ? 0.0f : (x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x));
    size_t n = c->n;
    if (n == 0) {
        return x;
    }
    float x0 = c->xs[0], xn = c->xs[n - 1];
    if (x <= x0) {
        return c->ys[0];
    }
    if (x >= xn) {
        return c->ys[n - 1];
    }
    /* partition_point(v <= x) - 1, clamped to n-2. */
    size_t pp = 0;
    while (pp < n && c->xs[pp] <= x) {
        pp++;
    }
    size_t k = pp > 0 ? pp - 1 : 0;
    if (k > n - 2) {
        k = n - 2;
    }
    float xa = c->xs[k], xb = c->xs[k + 1];
    float ya = c->ys[k], yb = c->ys[k + 1];
    float ma = c->ms[k], mb = c->ms[k + 1];
    float h = xb - xa;
    float t = (x - xa) / h;
    float t2 = t * t, t3 = t * t * t;
    float y = (2.0f * t3 - 3.0f * t2 + 1.0f) * ya + (t3 - 2.0f * t2 + t) * h * ma +
              (-2.0f * t3 + 3.0f * t2) * yb + (t3 - t2) * h * mb;
    float lo = ya < yb ? ya : yb;
    float hi = ya > yb ? ya : yb;
    return y < lo ? lo : (y > hi ? hi : y);
}

bool pc_pressure_curve_is_linear(const pc_pressure_curve *c)
{
    return c->n == 2 && c->xs[0] == 0.0f && c->xs[1] == 1.0f && c->ys[0] == 0.0f && c->ys[1] == 1.0f;
}

/* ========================================================================================== *
 * Shortcut normalisation and conflicts
 * ========================================================================================== */

static void capitalize(const char *s, char *out, size_t cap)
{
    size_t len = strlen(s);
    if (len == 0 || cap == 0) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return;
    }
    char first = s[0];
    bool all_digits = len > 1;
    for (size_t i = 1; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) {
            all_digits = false;
            break;
        }
    }
    if ((first == 'f' || first == 'F') && len > 1 && all_digits) {
        int w = snprintf(out, cap, "F%s", s + 1);
        (void)w;
        return;
    }
    if (isalpha((unsigned char)first)) {
        size_t w = 0;
        if (w < cap - 1) {
            out[w++] = (char)toupper((unsigned char)first);
        }
        for (size_t i = 1; i < len && w < cap - 1; i++) {
            out[w++] = (char)tolower((unsigned char)s[i]);
        }
        out[w] = '\0';
    } else {
        size_t copy = len < cap - 1 ? len : cap - 1;
        memcpy(out, s, copy);
        out[copy] = '\0';
    }
}

bool pc_pref_normalize_shortcut(const char *s, size_t len, char *out, size_t cap)
{
    /* trim */
    while (len > 0 && isspace((unsigned char)s[0])) {
        s++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        len--;
    }
    if (len == 0) {
        return false;
    }
    char body[128];
    if (len >= sizeof body) {
        return false;
    }
    memcpy(body, s, len);
    body[len] = '\0';
    bool cmd = false, ctrl = false, alt = false, shift = false;
    char key[64];
    bool has_key = false;
    /* A trailing "+" key is spelled "Cmd++"; or the whole thing is "+". */
    bool plus_key = false;
    size_t blen = len;
    if ((blen >= 2 && body[blen - 1] == '+' && body[blen - 2] == '+') || (blen == 1 && body[0] == '+')) {
        plus_key = true;
        while (blen > 0 && body[blen - 1] == '+') {
            blen--;
        }
        body[blen] = '\0';
    }
    char *save = NULL;
    for (char *part = strtok_r(body, "+", &save); part != NULL; part = strtok_r(NULL, "+", &save)) {
        if (part[0] == '\0') {
            continue;
        }
        char lower[64];
        size_t pl = strlen(part);
        size_t copy = pl < sizeof lower - 1 ? pl : sizeof lower - 1;
        for (size_t i = 0; i < copy; i++) {
            lower[i] = (char)tolower((unsigned char)part[i]);
        }
        lower[copy] = '\0';
        if (strcmp(lower, "cmd") == 0 || strcmp(lower, "command") == 0 ||
            strcmp(lower, "\xe2\x8c\x98") == 0 || strcmp(lower, "meta") == 0 || strcmp(lower, "super") == 0) {
            cmd = true;
        } else if (strcmp(lower, "ctrl") == 0 || strcmp(lower, "control") == 0 ||
                   strcmp(lower, "\xe2\x8c\x83") == 0) {
            ctrl = true;
        } else if (strcmp(lower, "alt") == 0 || strcmp(lower, "option") == 0 || strcmp(lower, "opt") == 0 ||
                   strcmp(lower, "\xe2\x8c\xa5") == 0) {
            alt = true;
        } else if (strcmp(lower, "shift") == 0 || strcmp(lower, "\xe2\x87\xa7") == 0) {
            shift = true;
        } else {
            if (has_key) {
                return false;
            }
            /* single character -> uppercase; else capitalise. count chars (UTF-8 aware-ish). */
            size_t nchars = 0;
            for (size_t i = 0; part[i] != '\0';) {
                unsigned char ch = (unsigned char)part[i];
                i += ch < 0x80 ? 1 : (ch >> 5) == 0x6 ? 2 : (ch >> 4) == 0xE ? 3 : 4;
                nchars++;
            }
            if (nchars == 1) {
                /* to_uppercase of a single char: for ASCII just upper; non-ASCII kept. */
                if ((unsigned char)part[0] < 0x80) {
                    key[0] = (char)toupper((unsigned char)part[0]);
                    key[1] = '\0';
                } else {
                    size_t kc = pl < sizeof key - 1 ? pl : sizeof key - 1;
                    memcpy(key, part, kc);
                    key[kc] = '\0';
                }
            } else {
                capitalize(part, key, sizeof key);
            }
            has_key = true;
        }
    }
    if (plus_key) {
        if (has_key) {
            return false;
        }
        key[0] = '+';
        key[1] = '\0';
        has_key = true;
    }
    if (!has_key) {
        return false;
    }
    char result[128];
    result[0] = '\0';
    size_t w = 0;
    const char *mods[4] = {NULL, NULL, NULL, NULL};
    size_t nm = 0;
    if (cmd) {
        mods[nm++] = "Cmd";
    }
    if (ctrl) {
        mods[nm++] = "Ctrl";
    }
    if (alt) {
        mods[nm++] = "Alt";
    }
    if (shift) {
        mods[nm++] = "Shift";
    }
    for (size_t i = 0; i < nm; i++) {
        int n = snprintf(result + w, sizeof result - w, "%s%s", i == 0 ? "" : "+", mods[i]);
        if (n < 0) {
            return false;
        }
        w += (size_t)n;
        if (w >= sizeof result) {
            return false;
        }
    }
    if (nm > 0) {
        if (w + 1 >= sizeof result) {
            return false;
        }
        result[w++] = '+';
        result[w] = '\0';
    }
    int n = snprintf(result + w, sizeof result - w, "%s", key);
    if (n < 0 || (size_t)n >= sizeof result - w) {
        return false;
    }
    size_t rl = strlen(result);
    if (cap == 0) {
        return true;
    }
    size_t copy = rl < cap - 1 ? rl : cap - 1;
    memcpy(out, result, copy);
    out[copy] = '\0';
    return true;
}

/* ========================================================================================== *
 * Dotted-path get / set / reset on the tree
 * ========================================================================================== */

/* Keyed maps whose keys are command ids (contain dots): shortcuts.<id>, menus.colors.<id>.
 * On a match fills *map and *id (borrowed into path) and returns true. */
static bool keyed(const char *path, const char **map, const char **id)
{
    static const char *const prefixes[] = {"shortcuts.", "menus.colors."};
    for (size_t i = 0; i < 2; i++) {
        size_t pl = strlen(prefixes[i]);
        if (strncmp(path, prefixes[i], pl) == 0) {
            *map = prefixes[i] == prefixes[0] ? "shortcuts" : "menus.colors";
            *id = path + pl;
            return true;
        }
    }
    return false;
}

/* Walk a dotted path over mutable objects (no keyed handling); NULL if missing. */
static yyjson_mut_val *walk(yyjson_mut_val *root, const char *path)
{
    if (path[0] == '\0') {
        return root;
    }
    yyjson_mut_val *cur = root;
    const char *p = path;
    char key[128];
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t kl = dot != NULL ? (size_t)(dot - p) : strlen(p);
        if (kl == 0 || kl >= sizeof key) {
            return NULL;
        }
        memcpy(key, p, kl);
        key[kl] = '\0';
        if (cur == NULL || !yyjson_mut_is_obj(cur)) {
            return NULL;
        }
        cur = yyjson_mut_obj_get(cur, key);
        if (dot == NULL) {
            break;
        }
        p = dot + 1;
    }
    return cur;
}

/* get_path including keyed handling. */
static yyjson_mut_val *get_path(yyjson_mut_val *root, const char *path)
{
    if (path[0] == '\0') {
        return root;
    }
    const char *map, *id;
    if (keyed(path, &map, &id)) {
        yyjson_mut_val *m = walk(root, map);
        if (m == NULL || !yyjson_mut_is_obj(m)) {
            return NULL;
        }
        return yyjson_mut_obj_get(m, id);
    }
    return walk(root, path);
}

/* Broad serde type class for the type-compatibility guard. */
typedef enum { T_NULL, T_BOOL, T_NUM, T_STR, T_ARR, T_OBJ, T_OTHER } type_class;

static type_class class_of(yyjson_mut_val *v)
{
    if (v == NULL || yyjson_mut_is_null(v)) {
        return T_NULL;
    }
    if (yyjson_mut_is_bool(v)) {
        return T_BOOL;
    }
    if (yyjson_mut_is_num(v)) {
        return T_NUM;
    }
    if (yyjson_mut_is_str(v)) {
        return T_STR;
    }
    if (yyjson_mut_is_arr(v)) {
        return T_ARR;
    }
    if (yyjson_mut_is_obj(v)) {
        return T_OBJ;
    }
    return T_OTHER;
}

/* Set `value` (a mut val already owned by `doc`) at `path`, creating keys only inside free-form
 * maps (shortcuts, menus.colors). On failure writes an error message into msg[msgcap]. */
static bool set_path(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *path, yyjson_mut_val *value,
                     char *msg, size_t msgcap)
{
    const char *map, *id;
    if (keyed(path, &map, &id)) {
        yyjson_mut_val *cur = walk(root, map);
        if (cur == NULL) {
            snprintf(msg, msgcap, "unknown preference `%s`", path);
            return false;
        }
        if (!yyjson_mut_is_obj(cur)) {
            snprintf(msg, msgcap, "`%s` is not a map", map);
            return false;
        }
        yyjson_mut_obj_put(cur, yyjson_mut_strcpy(doc, id), value);
        return true;
    }
    /* Split into parents + last, ignoring empty segments. */
    char parents[8][128];
    size_t nparents = 0;
    char last[128];
    last[0] = '\0';
    const char *p = path;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t kl = dot != NULL ? (size_t)(dot - p) : strlen(p);
        if (kl > 0) {
            char seg[128];
            if (kl >= sizeof seg) {
                snprintf(msg, msgcap, "unknown preference `%s`", path);
                return false;
            }
            memcpy(seg, p, kl);
            seg[kl] = '\0';
            /* shift: current last becomes a parent */
            if (last[0] != '\0') {
                if (nparents >= 8) {
                    snprintf(msg, msgcap, "unknown preference `%s`", path);
                    return false;
                }
                memcpy(parents[nparents], last, strlen(last) + 1);
                nparents++;
            }
            memcpy(last, seg, kl + 1);
        }
        if (dot == NULL) {
            break;
        }
        p = dot + 1;
    }
    if (last[0] == '\0') {
        snprintf(msg, msgcap, "empty path");
        return false;
    }
    yyjson_mut_val *cur = root;
    for (size_t i = 0; i < nparents; i++) {
        if (cur == NULL || !yyjson_mut_is_obj(cur)) {
            snprintf(msg, msgcap, "unknown preference `%s`", path);
            return false;
        }
        cur = yyjson_mut_obj_get(cur, parents[i]);
    }
    if (cur == NULL || !yyjson_mut_is_obj(cur)) {
        snprintf(msg, msgcap, "`%s` is not inside a section", path);
        return false;
    }
    bool free_map = (nparents >= 1 && strcmp(parents[0], "shortcuts") == 0) ||
                    (nparents == 2 && strcmp(parents[0], "menus") == 0 && strcmp(parents[1], "colors") == 0);
    yyjson_mut_val *existing = yyjson_mut_obj_get(cur, last);
    if (existing == NULL && !free_map) {
        snprintf(msg, msgcap, "unknown preference `%s`", path);
        return false;
    }
    /* Type-compatibility guard (approximates serde re-coercion): an Option/null leaf accepts
     * anything; otherwise the broad class must match (any number class). */
    if (existing != NULL) {
        type_class et = class_of(existing);
        type_class nt = class_of(value);
        if (et != T_NULL && et != nt) {
            snprintf(msg, msgcap, "invalid value for `%s`", path);
            return false;
        }
    }
    yyjson_mut_obj_put(cur, yyjson_mut_strcpy(doc, last), value);
    return true;
}

/* Validate one incoming value (yyjson_val from params) for `path`. msg gets the reason. */
static bool check_value(const char *path, yyjson_val *v, char *msg, size_t msgcap)
{
    if (strcmp(path, "interface.favoriteThemes") == 0) {
        if (v == NULL || !yyjson_is_arr(v)) {
            snprintf(msg, msgcap, "`interface.favoriteThemes` must be a list of theme ids");
            return false;
        }
        size_t count = yyjson_arr_size(v);
        if (count > (size_t)C_theme_N) {
            snprintf(msg, msgcap, "`interface.favoriteThemes` contains too many themes");
            return false;
        }
        const char *seen[C_theme_N];
        size_t nseen = 0;
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(v, idx, max, item)
        {
            const char *s = yyjson_get_str(item);
            bool valid = false;
            if (s != NULL) {
                for (int i = 0; i < C_theme_N; i++) {
                    if (strcmp(s, C_theme[i]) == 0) {
                        valid = true;
                        break;
                    }
                }
            }
            if (!valid) {
                snprintf(msg, msgcap, "unknown favorite theme `%s`", s != NULL ? s : "null");
                return false;
            }
            for (size_t i = 0; i < nseen; i++) {
                if (strcmp(seen[i], s) == 0) {
                    snprintf(msg, msgcap, "duplicate favorite theme `%s`", s);
                    return false;
                }
            }
            if (nseen < (size_t)C_theme_N) {
                seen[nseen++] = s;
            }
        }
        return true;
    }
    if (strcmp(path, "performance.renderingMode") == 0 && v != NULL && yyjson_is_null(v)) {
        return true; /* legacy policy */
    }
    size_t cn;
    const char *const *choices = pc_pref_choices(path, &cn);
    if (choices != NULL) {
        const char *s = v != NULL ? yyjson_get_str(v) : NULL;
        if (s == NULL) {
            snprintf(msg, msgcap, "`%s` must be one of one of the listed values", path);
            return false;
        }
        bool ok = false;
        for (size_t i = 0; i < cn; i++) {
            if (strcmp(choices[i], s) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            snprintf(msg, msgcap, "`%s` is not a valid value (got `%s`)", path, s);
            return false;
        }
    }
    if (strcmp(path, "interface.language") == 0) {
        const char *s = v != NULL ? yyjson_get_str(v) : NULL;
        bool ok = s != NULL && s[0] != '\0' && strlen(s) <= 16;
        if (ok) {
            for (const char *c = s; *c != '\0'; c++) {
                if (!(isalnum((unsigned char)*c) || *c == '-')) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok) {
            snprintf(msg, msgcap, "`interface.language` must be `auto` or a language code");
            return false;
        }
    }
    double lo, hi;
    if (pc_pref_range(path, &lo, &hi)) {
        double x;
        if (v == NULL || !pc_json_as_f64(v, &x)) {
            snprintf(msg, msgcap, "`%s` must be a number", path);
            return false;
        }
        if (!(x >= lo && x <= hi)) {
            snprintf(msg, msgcap, "`%s` is out of range", path);
            return false;
        }
    }
    if (pc_pref_is_color(path)) {
        const char *s = v != NULL ? yyjson_get_str(v) : NULL;
        uint8_t rgb[3];
        if (s == NULL || !pc_pref_parse_hex(s, strlen(s), rgb)) {
            snprintf(msg, msgcap, "`%s` must be a #rrggbb colour", path);
            return false;
        }
    }
    if (strcmp(path, "tools.pressureCurve") == 0) {
        if (v == NULL || !yyjson_is_arr(v)) {
            snprintf(msg, msgcap, "`tools.pressureCurve` must be a list of [input, output] points");
            return false;
        }
        size_t count = yyjson_arr_size(v);
        if (count < 2 || count > PC_PRESSURE_CURVE_MAX_POINTS) {
            snprintf(msg, msgcap, "`tools.pressureCurve` needs 2..=%d points (got %zu)",
                     PC_PRESSURE_CURVE_MAX_POINTS, count);
            return false;
        }
        size_t idx, max;
        yyjson_val *pt;
        yyjson_arr_foreach(v, idx, max, pt)
        {
            bool ok = yyjson_is_arr(pt) && yyjson_arr_size(pt) == 2;
            if (ok) {
                size_t j, jm;
                yyjson_val *comp;
                yyjson_arr_foreach(pt, j, jm, comp)
                {
                    double x;
                    if (!pc_json_as_f64(comp, &x) || !(x >= 0.0 && x <= 1.0)) {
                        ok = false;
                        break;
                    }
                }
            }
            if (!ok) {
                snprintf(msg, msgcap, "`tools.pressureCurve` points are [input, output] pairs within 0..1");
                return false;
            }
        }
    }
    if (strncmp(path, "shortcuts.", 10) == 0) {
        const char *s = v != NULL ? yyjson_get_str(v) : NULL;
        if (s == NULL) {
            snprintf(msg, msgcap, "shortcut for `%s` must be a string", path + 10);
            return false;
        }
        char norm[128];
        if (s[0] != '\0' && !pc_pref_normalize_shortcut(s, strlen(s), norm, sizeof norm)) {
            snprintf(msg, msgcap, "`%s` is not a valid shortcut (e.g. Cmd+Shift+N, F7)", s);
            return false;
        }
    }
    return true;
}

/* ========================================================================================== *
 * Preferences set / reset on a prefs store
 * ========================================================================================== */

/* Copy a params value (yyjson_val) into doc and apply at path, after check_value. The
 * shortcuts.* normalisation and null-removal are handled by the caller. */
static bool prefs_set_one(pc_engine_prefs *pf, const char *path, yyjson_val *value, char *msg, size_t msgcap)
{
    /* shortcuts.<id> with "" or a string: normalise; null removes the override. */
    if (strncmp(path, "shortcuts.", 10) == 0) {
        if (value != NULL && yyjson_is_null(value)) {
            const char *id = path + 10;
            yyjson_mut_val *sc = walk(yyjson_mut_doc_get_root(pf->values), "shortcuts");
            if (sc != NULL && yyjson_mut_is_obj(sc)) {
                yyjson_mut_obj_remove_key(sc, id);
            }
            return true;
        }
        const char *s = value != NULL ? yyjson_get_str(value) : NULL;
        if (s != NULL && s[0] != '\0') {
            char norm[128];
            if (!pc_pref_normalize_shortcut(s, strlen(s), norm, sizeof norm)) {
                snprintf(msg, msgcap, "`%s` is not a valid shortcut", s);
                return false;
            }
            if (!check_value(path, value, msg, msgcap)) {
                return false;
            }
            yyjson_mut_val *nv = yyjson_mut_strcpy(pf->values, norm);
            return set_path(pf->values, yyjson_mut_doc_get_root(pf->values), path, nv, msg, msgcap);
        }
        /* empty string: disables the default */
    }
    if (!check_value(path, value, msg, msgcap)) {
        return false;
    }
    yyjson_mut_val *nv = yyjson_val_mut_copy(pf->values, value);
    if (nv == NULL) {
        snprintf(msg, msgcap, "out of memory");
        return false;
    }
    return set_path(pf->values, yyjson_mut_doc_get_root(pf->values), path, nv, msg, msgcap);
}

/* Theme -> (appearanceMode, slotKey, slotValue==theme name). */
static bool theme_appearance(const char *theme, const char **mode, const char **slot)
{
    static const char *const dark[] = {"pro",         "proMedium",  "studio",  "solarizedDark",
                                       "adwaitaDark", "breezeDark", "softDark"};
    static const char *const light[] = {"studioLight", "proLightGray", "proLight", "classic",
                                        "adwaita",     "breezeLight",  "softLight"};
    for (size_t i = 0; i < sizeof dark / sizeof dark[0]; i++) {
        if (strcmp(theme, dark[i]) == 0) {
            *mode = "dark";
            *slot = "darkTheme";
            return true;
        }
    }
    for (size_t i = 0; i < sizeof light / sizeof light[0]; i++) {
        if (strcmp(theme, light[i]) == 0) {
            *mode = "light";
            *slot = "lightTheme";
            return true;
        }
    }
    return false;
}

/* Apply the interface.theme side-effect (set darkTheme/lightTheme + appearanceMode). */
static void apply_theme_side_effect(pc_engine_prefs *pf)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(pf->values);
    yyjson_mut_val *iface = yyjson_mut_obj_get(root, "interface");
    if (iface == NULL || !yyjson_mut_is_obj(iface)) {
        return;
    }
    const char *theme = yyjson_mut_get_str(yyjson_mut_obj_get(iface, "theme"));
    if (theme == NULL) {
        return;
    }
    const char *mode, *slot;
    if (!theme_appearance(theme, &mode, &slot)) {
        return;
    }
    yyjson_mut_obj_put(iface, yyjson_mut_strcpy(pf->values, slot), yyjson_mut_strcpy(pf->values, theme));
    yyjson_mut_obj_put(iface, yyjson_mut_strcpy(pf->values, "appearanceMode"),
                       yyjson_mut_strcpy(pf->values, mode));
}

/* ========================================================================================== *
 * Session helpers
 * ========================================================================================== */

static double prefs_root_num(const pc_engine_session *s, const char *path, double def)
{
    yyjson_mut_val *v = get_path(yyjson_mut_doc_get_root(s->prefs.values), path);
    if (v != NULL && yyjson_mut_is_num(v)) {
        return yyjson_mut_get_num(v); /* handles int / uint / real */
    }
    return def;
}

void pc_engine_prefs_history_limits(const pc_engine_session *s, size_t *max_states, size_t *max_bytes)
{
    double states = prefs_root_num(s, "performance.historyStates", 50.0);
    double mb = prefs_root_num(s, "performance.memoryUsageMb", 8192.0);
    size_t n = states >= 1.0 ? (size_t)states : 1;
    if (n < 1) {
        n = 1;
    }
    *max_states = n;
    size_t mbytes = mb > 0 ? (size_t)mb : 0;
    /* saturating_mul by 1<<20 */
    size_t budget;
    if (mbytes > SIZE_MAX >> 20) {
        budget = SIZE_MAX;
    } else {
        budget = mbytes << 20;
    }
    *max_bytes = budget;
}

void pc_engine_apply_prefs(pc_engine_session *s)
{
    if (s->prefs.values == NULL) {
        return;
    }
    size_t max_states, max_bytes;
    pc_engine_prefs_history_limits(s, &max_states, &max_bytes);
    for (size_t i = 0; i < s->ndocs; i++) {
        pc_history *h = s->docs[i].history;
        pc_history_set_max_states(h, max_states);
        pc_history_set_max_bytes(h, max_bytes);
        (void)pc_history_trim(h, pc_doc_arc_get(s->docs[i].doc), NULL, NULL);
    }
    double budget_mb = prefs_root_num(s, "performance.effectCacheMb", 768.0);
    size_t bmb = budget_mb > 0 ? (size_t)budget_mb : 0;
    size_t bytes = bmb > (SIZE_MAX >> 20) ? SIZE_MAX : bmb << 20;
    pc_compose_set_effect_cache_budget(bytes);
    /* Text missing-glyph protection and plug-in prefs sync are wired by E10/E11/E12 (the engine
     * session has no shared font database yet); documented in REWRITE_PLAN.md section 7. */
}

pc_status pc_engine_prefs_get(const pc_engine_session *s, const char *path, pc_json_value *out, pc_error *err)
{
    if (strcmp(path, "colorSettings") == 0 ||
        (strncmp(path, "colorSettings.", 14) == 0 && path[14] != '\0')) {
        return pc_engine_color_settings_get(&s->color, path[13] == '.' ? path + 14 : "", out, err);
    }
    yyjson_mut_val *v = get_path(yyjson_mut_doc_get_root(s->prefs.values), path);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "unknown preference `%s`", path);
    }
    return pc_json_value_from_mut(v, out, err);
}

char *pc_engine_session_prefs_to_json(const pc_engine_session *s)
{
    if (s->prefs.values == NULL) {
        return NULL;
    }
    /* prefs_value(): the preferences plus version:1. colorSettings (E8) and presets (E2 preset
     * model) are added by their owning chunks; a frontend merges what storage holds. */
    yyjson_mut_doc *copy = yyjson_mut_doc_mut_copy(s->prefs.values, NULL);
    if (copy == NULL) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(copy);
    yyjson_mut_obj_add_uint(copy, root, "version", 1);
    /* E8: colour settings live in the ColorState, persisted in the same document. */
    yyjson_mut_val *csv = NULL;
    if (pc_engine_color_settings_to_mut(&s->color, copy, &csv) == PC_OK && csv != NULL) {
        yyjson_mut_obj_add_val(copy, root, "colorSettings", csv);
    }
    /* E2b/E3: the preset model (gradients, swatches, pattern groups, tool presets, layer
     * defaults, File > New documents and the Recent list) lives under "presets". */
    pc_json_value pv = PC_JSON_VALUE_INIT;
    if (pc_engine_presets_to_json(s, &pv, NULL) == PC_OK) {
        yyjson_mut_val *pvr = pc_json_value_root(&pv);
        if (pvr != NULL) {
            yyjson_mut_val *copyv = yyjson_mut_val_mut_copy(copy, pvr);
            if (copyv != NULL) {
                yyjson_mut_obj_add_val(copy, root, "presets", copyv);
            }
        }
    }
    pc_json_value_fini(&pv);
    pc_buf buf = PC_BUF_INIT;
    pc_status rc = pc_json_write(root, PC_JSON_PRETTY | PC_JSON_SORT_KEYS, &buf, NULL);
    yyjson_mut_doc_free(copy);
    if (rc != PC_OK) {
        pc_buf_fini(&buf);
        return NULL;
    }
    char *s2 = pc_strndup((const char *)buf.data, buf.len);
    pc_buf_fini(&buf);
    return s2;
}

/* Merge the known sub-keys of a loaded section object into the default tree. */
static void merge_section(pc_engine_prefs *pf, const char *section, yyjson_val *loaded)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(pf->values);
    yyjson_mut_val *dst = yyjson_mut_obj_get(root, section);
    if (dst == NULL || !yyjson_mut_is_obj(dst) || loaded == NULL || !yyjson_is_obj(loaded)) {
        return;
    }
    size_t idx, max;
    yyjson_val *key, *val;
    yyjson_obj_foreach(loaded, idx, max, key, val)
    {
        const char *k = yyjson_get_str(key);
        if (k == NULL) {
            continue;
        }
        yyjson_mut_val *existing = yyjson_mut_obj_get(dst, k);
        if (existing == NULL) {
            continue; /* unknown sub-key: ignored */
        }
        char path[192];
        snprintf(path, sizeof path, "%s.%s", section, k);
        char msg[PC_ERROR_MSG_MAX];
        /* Validate enum/range/colour fields; invalid values keep the default (lenient, never a
         * crash on a hand-edited file). */
        if (!check_value(path, val, msg, sizeof msg)) {
            continue;
        }
        /* type class guard */
        type_class et = class_of(existing);
        type_class nt;
        if (yyjson_is_null(val)) {
            nt = T_NULL;
        } else if (yyjson_is_bool(val)) {
            nt = T_BOOL;
        } else if (yyjson_is_num(val)) {
            nt = T_NUM;
        } else if (yyjson_is_str(val)) {
            nt = T_STR;
        } else if (yyjson_is_arr(val)) {
            nt = T_ARR;
        } else if (yyjson_is_obj(val)) {
            nt = T_OBJ;
        } else {
            nt = T_OTHER;
        }
        if (et != T_NULL && et != nt) {
            continue;
        }
        yyjson_mut_val *nv = yyjson_val_mut_copy(pf->values, val);
        if (nv != NULL) {
            yyjson_mut_obj_put(dst, yyjson_mut_strcpy(pf->values, k), nv);
        }
    }
}

/* The shape a top-level field must have to be accepted from a loaded file. */
typedef enum { W_ANY, W_OBJ, W_ARR, W_BOOL } want_shape;

/* Copy a loaded top-level field wholesale into the tree, but only when its JSON shape matches
 * `want`: a corrupt or hand-edited file that stores the wrong type keeps the default instead of
 * poisoning the tree (which later commands must never crash on). */
static void merge_whole(pc_engine_prefs *pf, const char *key, yyjson_val *loaded, want_shape want)
{
    if (loaded == NULL) {
        return;
    }
    switch (want) {
    case W_OBJ:
        if (!yyjson_is_obj(loaded)) {
            return;
        }
        break;
    case W_ARR:
        if (!yyjson_is_arr(loaded)) {
            return;
        }
        break;
    case W_BOOL:
        if (!yyjson_is_bool(loaded)) {
            return;
        }
        break;
    case W_ANY:
        break;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(pf->values);
    yyjson_mut_val *nv = yyjson_val_mut_copy(pf->values, loaded);
    if (nv != NULL) {
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(pf->values, key), nv);
    }
}

pc_status pc_engine_session_load_prefs_json(pc_engine_session *s, const char *text, size_t len, pc_error *err)
{
    yyjson_doc *doc = NULL;
    if (pc_json_parse(text, len, &doc, err) != PC_OK) {
        /* match reference "preferences: <e>" shape loosely */
        if (err != NULL) {
            char tmp[PC_ERROR_MSG_MAX];
            snprintf(tmp, sizeof tmp, "preferences: %.230s", err->msg);
            (void)pc_fail(err, PC_ERR_INVALID, "%s", tmp);
        }
        return PC_ERR_INVALID;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == NULL || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return pc_fail(err, PC_ERR_INVALID, "preferences: expected an object");
    }
    /* Start from defaults. */
    if (!prefs_reset_all(&s->prefs)) {
        yyjson_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "preferences");
    }
    /* Legacy appearance migration: if interface has no appearanceMode but a legacy theme, set the
     * derived appearanceMode + slot after merging the section. */
    for (int i = 0; i < SECTIONS_N; i++) {
        merge_section(&s->prefs, SECTIONS[i].id, yyjson_obj_get(root, SECTIONS[i].id));
    }
    yyjson_val *iface = yyjson_obj_get(root, "interface");
    if (iface != NULL && yyjson_is_obj(iface) && yyjson_obj_get(iface, "appearanceMode") == NULL) {
        const char *theme = yyjson_get_str(yyjson_obj_get(iface, "theme"));
        const char *mode, *slot;
        if (theme != NULL && theme_appearance(theme, &mode, &slot)) {
            yyjson_mut_val *o = yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->prefs.values), "interface");
            if (o != NULL) {
                yyjson_mut_obj_put(o, yyjson_mut_strcpy(s->prefs.values, "appearanceMode"),
                                   yyjson_mut_strcpy(s->prefs.values, mode));
                yyjson_mut_obj_put(o, yyjson_mut_strcpy(s->prefs.values, slot),
                                   yyjson_mut_strcpy(s->prefs.values, theme));
            }
        }
    }
    /* Free-form top-level fields (only accepted when the stored shape is right). */
    merge_whole(&s->prefs, "shortcuts", yyjson_obj_get(root, "shortcuts"), W_OBJ);
    merge_whole(&s->prefs, "menus", yyjson_obj_get(root, "menus"), W_OBJ);
    merge_whole(&s->prefs, "toolbar", yyjson_obj_get(root, "toolbar"), W_OBJ);
    merge_whole(&s->prefs, "userDictionary", yyjson_obj_get(root, "userDictionary"), W_ARR);
    merge_whole(&s->prefs, "workspaces", yyjson_obj_get(root, "workspaces"), W_OBJ);
    merge_whole(&s->prefs, "workspaceLocked", yyjson_obj_get(root, "workspaceLocked"), W_BOOL);
    merge_whole(&s->prefs, "panelLayout", yyjson_obj_get(root, "panelLayout"), W_ANY);
    merge_whole(&s->prefs, "dialogs", yyjson_obj_get(root, "dialogs"), W_OBJ);
    merge_whole(&s->prefs, "brushPicker", yyjson_obj_get(root, "brushPicker"), W_ANY);
    merge_whole(&s->prefs, "toolOptions", yyjson_obj_get(root, "toolOptions"), W_ANY);
    merge_whole(&s->prefs, "viewExtras", yyjson_obj_get(root, "viewExtras"), W_ANY);
    merge_whole(&s->prefs, "scriptEvents", yyjson_obj_get(root, "scriptEvents"), W_OBJ);
    /* E8: restore the colour settings (lenient: invalid values keep the defaults). */
    (void)pc_engine_color_settings_load(&s->color, yyjson_obj_get(root, "colorSettings"), NULL);
    /* E2b/E3: restore the preset model (a malformed blob keeps the built-in defaults). */
    pc_engine_load_presets_json(s, yyjson_obj_get(root, "presets"));
    yyjson_doc_free(doc);
    s->prefs.rev++;
    pc_engine_apply_prefs(s);
    return PC_OK;
}

/* ========================================================================================== *
 * Commands
 * ========================================================================================== */

static pc_status result_json_mut(pc_json_value *out, yyjson_mut_val *v, pc_error *err)
{
    return pc_json_value_from_mut(v, out, err);
}

/* prefs.get */
static pc_status run_prefs_get(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *path = "";
    yyjson_val *pv = pc_json_get(p, "path");
    if (pv != NULL) {
        const char *str = yyjson_get_str(pv);
        if (str != NULL) {
            path = str;
        }
    }
    if (strcmp(path, "colorSettings") == 0 ||
        (strncmp(path, "colorSettings.", 14) == 0 && path[14] != '\0')) {
        const char *sub = path[13] == '.' ? path + 14 : "";
        if (pc_engine_color_settings_get(&s->color, sub, out, err) != PC_OK) {
            return pc_engine_err_bad_params(err, "prefs.get", "unknown preference `%s`", path);
        }
        return PC_OK;
    }
    yyjson_mut_val *v = get_path(yyjson_mut_doc_get_root(s->prefs.values), path);
    if (v == NULL) {
        return pc_engine_err_bad_params(err, "prefs.get", "unknown preference `%s`", path);
    }
    return result_json_mut(out, v, err);
}

/* One change (path,value) within a prefs.set batch. */
typedef struct {
    const char *path;
    yyjson_val *value;
} pref_change;

/* Apply a whole-section/root merge or a single set to the working prefs store. */
static bool set_pref_path(pc_engine_prefs *pf, const char *path, yyjson_val *value, char *msg, size_t msgcap)
{
    bool whole = (path[0] == '\0') || is_section(path) || strcmp(path, "shortcuts") == 0 ||
                 strcmp(path, "menus") == 0 || strcmp(path, "toolbar") == 0;
    if (whole) {
        if (value == NULL || !yyjson_is_obj(value)) {
            snprintf(msg, msgcap, "`%s` needs an object", path);
            return false;
        }
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(value, idx, max, key, val)
        {
            const char *k = yyjson_get_str(key);
            if (k == NULL) {
                continue;
            }
            char sub[256];
            if (path[0] == '\0') {
                snprintf(sub, sizeof sub, "%s", k);
            } else {
                snprintf(sub, sizeof sub, "%s.%s", path, k);
            }
            /* colorSettings whole-object merge is handled by E8; skip here. */
            if (path[0] == '\0' && strcmp(k, "colorSettings") == 0) {
                continue;
            }
            yyjson_mut_val *cur = get_path(yyjson_mut_doc_get_root(pf->values), sub);
            bool nested = yyjson_is_obj(val) && strncmp(sub, "shortcuts", 9) != 0 &&
                          !(strlen(sub) >= 7 && strcmp(sub + strlen(sub) - 7, ".colors") == 0) &&
                          cur != NULL && yyjson_mut_is_obj(cur);
            if (nested) {
                size_t j, jm;
                yyjson_val *k2, *v2;
                yyjson_obj_foreach(val, j, jm, k2, v2)
                {
                    const char *kk = yyjson_get_str(k2);
                    if (kk == NULL) {
                        continue;
                    }
                    char sub2[320];
                    snprintf(sub2, sizeof sub2, "%s.%s", sub, kk);
                    if (!prefs_set_one(pf, sub2, v2, msg, msgcap)) {
                        return false;
                    }
                }
            } else {
                if (!prefs_set_one(pf, sub, val, msg, msgcap)) {
                    return false;
                }
            }
        }
    } else {
        if (!prefs_set_one(pf, path, value, msg, msgcap)) {
            return false;
        }
    }
    return true;
}

/* prefs.set */
/* E8: a prefs path that routes to the ColorState ("colorSettings" or "colorSettings.<key>"). */
static bool is_color_path(const char *path, const char **sub)
{
    if (strcmp(path, "colorSettings") == 0) {
        *sub = "";
        return true;
    }
    if (strncmp(path, "colorSettings.", 14) == 0 && path[14] != '\0') {
        *sub = path + 14;
        return true;
    }
    return false;
}

static pc_status run_prefs_set(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "prefs.set";
    yyjson_val *replace_value = pc_json_get(p, "replace");
    if (replace_value && !yyjson_is_bool(replace_value))
        return pc_engine_err_bad_params(err, cmd, "replace must be a boolean");
    bool replace = yyjson_is_true(replace_value);
    pref_change changes[256];
    size_t nchanges = 0;
    yyjson_val *path_v = pc_json_get(p, "path");
    if (path_v != NULL) {
        const char *path = yyjson_get_str(path_v);
        if (path != NULL) {
            yyjson_val *value = pc_json_get(p, "value");
            if (value == NULL) {
                return pc_engine_err_bad_params(err, cmd, "missing `value`");
            }
            changes[nchanges].path = path;
            changes[nchanges].value = value;
            nchanges++;
        }
    }
    yyjson_val *values = pc_json_get(p, "values");
    if (values != NULL && yyjson_is_obj(values)) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(values, idx, max, key, val)
        {
            if (nchanges >= sizeof changes / sizeof changes[0]) {
                break;
            }
            const char *k = yyjson_get_str(key);
            if (k == NULL) {
                continue;
            }
            changes[nchanges].path = k;
            changes[nchanges].value = val;
            nchanges++;
        }
    }
    if (nchanges == 0) {
        return pc_engine_err_bad_params(err, cmd, "give `path` and `value`, or `values: {path: value, ...}`");
    }
    /* All or nothing: validate against a scratch copy first. */
    pc_engine_prefs scratch;
    scratch.rev = s->prefs.rev;
    scratch.values = yyjson_mut_doc_mut_copy(s->prefs.values, NULL);
    if (scratch.values == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "prefs");
    }
    /* E8: colorSettings.* changes route to a scratch ColorState, validated and committed together. */
    pc_color_settings next_color;
    bool any_color = false;
    if (pc_color_settings_clone(&s->color.settings, &next_color, err) != PC_OK) {
        yyjson_mut_doc_free(scratch.values);
        return PC_ERR_NOMEM;
    }
    char msg[PC_ERROR_MSG_MAX];
    const char *prev_theme = yyjson_mut_get_str(yyjson_mut_obj_get(
        yyjson_mut_obj_get(yyjson_mut_doc_get_root(scratch.values), "interface"), "theme"));
    char prev_theme_buf[64] = {0};
    if (prev_theme != NULL) {
        snprintf(prev_theme_buf, sizeof prev_theme_buf, "%s", prev_theme);
    }
    bool set_theme_path = false;
    for (size_t i = 0; i < nchanges; i++) {
        const char *csub;
        if (is_color_path(changes[i].path, &csub)) {
            if (pc_engine_color_settings_set_path(&next_color, csub, changes[i].value, err) != PC_OK) {
                pc_color_settings_fini(&next_color);
                yyjson_mut_doc_free(scratch.values);
                return PC_ERR_INVALID;
            }
            any_color = true;
            continue;
        }
        if (strcmp(changes[i].path, "interface.theme") == 0) {
            set_theme_path = true;
        }
        bool replace_map =
            replace && (!strcmp(changes[i].path, "shortcuts") || !strcmp(changes[i].path, "menus") ||
                        !strcmp(changes[i].path, "toolbar") || !strcmp(changes[i].path, "workspaces"));
        bool reset_map = true;
        if (replace_map && !strcmp(changes[i].path, "shortcuts")) {
            yyjson_mut_val *empty = yyjson_mut_obj(scratch.values);
            reset_map = empty && set_path(scratch.values, yyjson_mut_doc_get_root(scratch.values),
                                          changes[i].path, empty, msg, sizeof msg);
        }
        if (!reset_map || !set_pref_path(&scratch, changes[i].path, changes[i].value, msg, sizeof msg) ||
            (replace_map && strcmp(changes[i].path, "shortcuts") &&
             !prefs_set_one(&scratch, changes[i].path, changes[i].value, msg, sizeof msg))) {
            pc_color_settings_fini(&next_color);
            yyjson_mut_doc_free(scratch.values);
            return pc_engine_err_bad_params(err, cmd, "%s", msg);
        }
    }
    if (any_color && pc_color_settings_validate(&next_color, err) != PC_OK) {
        pc_color_settings_fini(&next_color);
        yyjson_mut_doc_free(scratch.values);
        return PC_ERR_INVALID;
    }
    /* interface.theme side-effect. */
    const char *new_theme = yyjson_mut_get_str(yyjson_mut_obj_get(
        yyjson_mut_obj_get(yyjson_mut_doc_get_root(scratch.values), "interface"), "theme"));
    bool theme_changed = new_theme != NULL && (prev_theme == NULL || strcmp(new_theme, prev_theme_buf) != 0);
    if (set_theme_path || theme_changed) {
        apply_theme_side_effect(&scratch);
    }
    /* Commit. */
    yyjson_mut_doc_free(s->prefs.values);
    s->prefs.values = scratch.values;
    s->prefs.rev++;
    if (any_color) {
        pc_psblend_set_text_gamma(next_color.blend_text_gamma);
        pc_color_settings_fini(&s->color.settings);
        s->color.settings = next_color;
    } else {
        pc_color_settings_fini(&next_color);
    }
    pc_engine_apply_prefs(s);
    /* Result: each changed path's new value. */
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *obj_out = yyjson_mut_obj(d);
    for (size_t i = 0; i < nchanges; i++) {
        const char *csub;
        yyjson_mut_val *v;
        if (is_color_path(changes[i].path, &csub)) {
            pc_json_value cv;
            if (pc_engine_color_settings_get(&s->color, csub, &cv, NULL) == PC_OK) {
                v = yyjson_mut_val_mut_copy(d, pc_json_value_root(&cv));
                pc_json_value_fini(&cv);
            } else {
                v = yyjson_mut_null(d);
            }
        } else {
            yyjson_mut_val *cur = get_path(yyjson_mut_doc_get_root(s->prefs.values), changes[i].path);
            v = cur != NULL ? yyjson_mut_val_mut_copy(d, cur) : yyjson_mut_null(d);
        }
        /* The key is borrowed from the params doc (freed after run); copy it. */
        yyjson_mut_obj_put(obj_out, yyjson_mut_strcpy(d, changes[i].path),
                           v != NULL ? v : yyjson_mut_null(d));
    }
    yyjson_mut_doc_set_root(d, obj_out);
    return PC_OK;
}

/* prefs.reset */
static pc_status run_prefs_reset(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "prefs.reset";
    const char *path = NULL;
    yyjson_val *pv = pc_json_get(p, "path");
    if (pv == NULL) {
        pv = pc_json_get(p, "section");
    }
    if (pv != NULL) {
        path = yyjson_get_str(pv);
    }
    bool is_keyed = false;
    if (path != NULL) {
        const char *map, *id;
        is_keyed = keyed(path, &map, &id);
    }
    const char *csub_r;
    bool is_color = path != NULL && is_color_path(path, &csub_r);
    if (path == NULL || path[0] == '\0') {
        if (!prefs_reset_all(&s->prefs)) {
            return pc_fail(err, PC_ERR_NOMEM, "prefs");
        }
        s->prefs.rev++;
        /* E8: a full reset also restores the default colour settings. */
        pc_color_settings cdef;
        if (pc_color_settings_default(&cdef, NULL) == PC_OK) {
            pc_color_settings_fini(&s->color.settings);
            s->color.settings = cdef;
            pc_psblend_set_text_gamma(s->color.settings.blend_text_gamma);
        }
    } else if (is_color) {
        /* E8: reset the whole colorSettings object or one of its keys. */
        pc_color_settings next_color;
        if (pc_color_settings_clone(&s->color.settings, &next_color, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        if (pc_engine_color_settings_reset_key(&next_color, csub_r, err) != PC_OK) {
            pc_color_settings_fini(&next_color);
            return pc_engine_err_bad_params(err, cmd, "unknown preference `%s`", path);
        }
        pc_color_settings_fini(&s->color.settings);
        s->color.settings = next_color;
        pc_psblend_set_text_gamma(s->color.settings.blend_text_gamma);
        s->prefs.rev++;
    } else {
        const char *map, *id;
        if (keyed(path, &map, &id)) {
            yyjson_mut_val *m = walk(yyjson_mut_doc_get_root(s->prefs.values), map);
            if (m == NULL || !yyjson_mut_is_obj(m)) {
                return pc_engine_err_bad_params(err, cmd, "unknown preference `%s`", path);
            }
            yyjson_mut_obj_remove_key(m, id);
            s->prefs.rev++;
        } else if (strcmp(path, "shortcuts") == 0) {
            yyjson_mut_val *sc = walk(yyjson_mut_doc_get_root(s->prefs.values), "shortcuts");
            if (sc != NULL && yyjson_mut_is_obj(sc)) {
                /* clear */
                yyjson_mut_obj_clear(sc);
            }
            s->prefs.rev++;
        } else {
            /* default value for the path. */
            yyjson_mut_doc *defdoc = yyjson_mut_doc_new(NULL);
            if (defdoc == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "prefs");
            }
            yyjson_mut_val *defroot = default_tree(defdoc);
            if (defroot == NULL) {
                yyjson_mut_doc_free(defdoc);
                return pc_fail(err, PC_ERR_NOMEM, "prefs");
            }
            yyjson_mut_doc_set_root(defdoc, defroot);
            yyjson_mut_val *def = get_path(defroot, path);
            if (def == NULL) {
                yyjson_mut_doc_free(defdoc);
                return pc_engine_err_bad_params(err, cmd, "unknown preference `%s`", path);
            }
            yyjson_mut_val *nv = yyjson_mut_val_mut_copy(s->prefs.values, def);
            char msg[PC_ERROR_MSG_MAX];
            if (nv == NULL || !set_path(s->prefs.values, yyjson_mut_doc_get_root(s->prefs.values), path, nv,
                                        msg, sizeof msg)) {
                yyjson_mut_doc_free(defdoc);
                return pc_engine_err_bad_params(err, cmd, "unknown preference `%s`", path);
            }
            yyjson_mut_doc_free(defdoc);
            s->prefs.rev++;
        }
    }
    pc_engine_apply_prefs(s);
    if (is_keyed) {
        return pc_engine_result_null(out, err);
    }
    if (is_color) {
        return pc_engine_color_settings_get(&s->color, csub_r, out, err);
    }
    yyjson_mut_val *v = get_path(yyjson_mut_doc_get_root(s->prefs.values), path != NULL ? path : "");
    if (v == NULL) {
        return pc_engine_err_bad_params(err, cmd, "unknown preference `%s`", path);
    }
    return result_json_mut(out, v, err);
}

/* edit.preferences.<section>: the section's values. */
static pc_status run_preferences_section(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                         pc_error *err)
{
    const char *section = "general";
    yyjson_val *sv = pc_json_get(p, "__section");
    if (sv != NULL) {
        const char *str = yyjson_get_str(sv);
        if (str != NULL) {
            section = str;
        }
    }
    const char *title = "General";
    for (int i = 0; i < SECTIONS_N; i++) {
        if (strcmp(SECTIONS[i].id, section) == 0) {
            title = SECTIONS[i].title;
            break;
        }
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "section", section); /* section is borrowed from params */
    yyjson_mut_obj_add_str(d, root, "title", title);        /* title is a static literal */
    yyjson_mut_val *sec = get_path(yyjson_mut_doc_get_root(s->prefs.values), section);
    yyjson_mut_val *values = sec != NULL ? yyjson_mut_val_mut_copy(d, sec) : yyjson_mut_null(d);
    yyjson_mut_obj_add_val(d, root, "values", values != NULL ? values : yyjson_mut_null(d));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* edit.menus */
static pc_status run_menus(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const COLORS[] = {"none",  "red",  "orange", "yellow",
                                         "green", "blue", "violet", "gray"};
    yyjson_val *color = pc_json_get(p, "color");
    if (color != NULL && yyjson_is_obj(color)) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(color, idx, max, key, val)
        {
            const char *c = yyjson_get_str(val);
            if (c != NULL) {
                bool ok = false;
                for (size_t i = 0; i < sizeof COLORS / sizeof COLORS[0]; i++) {
                    if (strcmp(c, COLORS[i]) == 0) {
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    return pc_engine_err_bad_params(err, "edit.menus", "unknown colour `%s`", c);
                }
            }
        }
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(s->prefs.values);
    yyjson_mut_val *menus = yyjson_mut_obj_get(root, "menus");
    if (menus == NULL || !yyjson_mut_is_obj(menus)) {
        return pc_fail(err, PC_ERR_INVALID, "menus preference missing");
    }
    yyjson_val *reset = pc_json_get(p, "reset");
    bool do_reset = reset != NULL && yyjson_is_true(reset);
    yyjson_mut_val *hidden = yyjson_mut_obj_get(menus, "hidden");
    yyjson_mut_val *colors = yyjson_mut_obj_get(menus, "colors");
    /* A corrupt/hand-edited settings file may have left these the wrong shape; normalise them so
     * the edits below never operate on a non-array/non-object (never crash). */
    if (hidden == NULL || !yyjson_mut_is_arr(hidden)) {
        hidden = yyjson_mut_arr(s->prefs.values);
        yyjson_mut_obj_put(menus, yyjson_mut_strcpy(s->prefs.values, "hidden"), hidden);
    }
    if (colors == NULL || !yyjson_mut_is_obj(colors)) {
        colors = yyjson_mut_obj(s->prefs.values);
        yyjson_mut_obj_put(menus, yyjson_mut_strcpy(s->prefs.values, "colors"), colors);
    }
    if (do_reset) {
        yyjson_mut_obj_clear(hidden);
        yyjson_mut_obj_clear(colors);
    }
    /* hide */
    yyjson_val *hide = pc_json_get(p, "hide");
    if (hide != NULL && yyjson_is_arr(hide)) {
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(hide, idx, max, item)
        {
            const char *id = yyjson_get_str(item);
            if (id == NULL) {
                continue;
            }
            bool present = false;
            size_t j, jm;
            yyjson_mut_val *h;
            yyjson_mut_arr_foreach(hidden, j, jm, h)
            {
                const char *hs = yyjson_mut_get_str(h);
                if (hs != NULL && strcmp(hs, id) == 0) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                yyjson_mut_arr_add_val(hidden, yyjson_mut_strcpy(s->prefs.values, id));
            }
        }
    }
    /* show: remove from hidden */
    yyjson_val *show = pc_json_get(p, "show");
    if (show != NULL && yyjson_is_arr(show)) {
        yyjson_mut_val *kept = yyjson_mut_arr(s->prefs.values);
        size_t j, jm;
        yyjson_mut_val *h;
        yyjson_mut_arr_foreach(hidden, j, jm, h)
        {
            const char *hs = yyjson_mut_get_str(h);
            bool remove = false;
            if (hs != NULL) {
                size_t idx, mx;
                yyjson_val *item;
                yyjson_arr_foreach(show, idx, mx, item)
                {
                    const char *ss = yyjson_get_str(item);
                    if (ss != NULL && strcmp(ss, hs) == 0) {
                        remove = true;
                        break;
                    }
                }
            }
            if (!remove && hs != NULL) {
                yyjson_mut_arr_add_val(kept, yyjson_mut_strcpy(s->prefs.values, hs));
            }
        }
        yyjson_mut_obj_put(menus, yyjson_mut_strcpy(s->prefs.values, "hidden"), kept);
    }
    /* colors */
    if (color != NULL && yyjson_is_obj(color)) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(color, idx, max, key, val)
        {
            const char *id = yyjson_get_str(key);
            const char *c = yyjson_get_str(val);
            if (id == NULL) {
                continue;
            }
            if (c != NULL && strcmp(c, "none") != 0) {
                yyjson_mut_obj_put(colors, yyjson_mut_strcpy(s->prefs.values, id),
                                   yyjson_mut_strcpy(s->prefs.values, c));
            } else {
                yyjson_mut_obj_remove_key(colors, id);
            }
        }
    }
    s->prefs.rev++;
    pc_engine_apply_prefs(s);
    return result_json_mut(out, menus, err);
}

/* edit.toolbar */
static pc_status run_toolbar(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(s->prefs.values);
    yyjson_mut_val *tb = yyjson_mut_obj_get(root, "toolbar");
    if (tb == NULL || !yyjson_mut_is_obj(tb)) {
        return pc_fail(err, PC_ERR_INVALID, "toolbar preference missing");
    }
    yyjson_val *reset = pc_json_get(p, "reset");
    if (reset != NULL && yyjson_is_true(reset)) {
        yyjson_mut_obj_put(tb, yyjson_mut_strcpy(s->prefs.values, "hidden"), yyjson_mut_arr(s->prefs.values));
        yyjson_mut_obj_put(tb, yyjson_mut_strcpy(s->prefs.values, "order"), yyjson_mut_arr(s->prefs.values));
    }
    const char *keys[2] = {"hidden", "order"};
    for (int i = 0; i < 2; i++) {
        yyjson_val *arr = pc_json_get(p, keys[i]);
        if (arr != NULL && yyjson_is_arr(arr)) {
            yyjson_mut_val *nv = yyjson_val_mut_copy(s->prefs.values, arr);
            if (nv != NULL) {
                yyjson_mut_obj_put(tb, yyjson_mut_strcpy(s->prefs.values, keys[i]), nv);
            }
        }
    }
    s->prefs.rev++;
    pc_engine_apply_prefs(s);
    return result_json_mut(out, tb, err);
}

/* ------------------------------------------- keyboard shortcuts ---------------------------- */

/* The effective shortcut for id: the override (empty = none), else the default. Writes into
 * out[cap]; returns false when there is no effective shortcut. */
static bool effective_shortcut(const pc_engine_prefs *pf, const char *id, const char *def, char *out,
                               size_t cap)
{
    yyjson_mut_val *sc = walk(yyjson_mut_doc_get_root(pf->values), "shortcuts");
    if (sc != NULL && yyjson_mut_is_obj(sc)) {
        yyjson_mut_val *o = yyjson_mut_obj_get(sc, id);
        if (o != NULL) {
            const char *s2 = yyjson_mut_get_str(o);
            if (s2 == NULL || s2[0] == '\0') {
                return false;
            }
            snprintf(out, cap, "%s", s2);
            return true;
        }
    }
    if (def == NULL) {
        return false;
    }
    snprintf(out, cap, "%s", def);
    return true;
}

bool pc_engine_prefs_effective_shortcut(const pc_engine_session *s, const char *id, const char *def,
                                        char *out, size_t cap)
{
    if (s == NULL || id == NULL || out == NULL || cap == 0) {
        return false;
    }
    return effective_shortcut(&s->prefs, id, def, out, cap);
}

/* Is id a bindable command or temporary tool? */
static bool is_bindable(const char *id)
{
    if (pc_engine_command_find(id) != NULL) {
        return true;
    }
    for (int i = 0; i < TEMPORARY_TOOLS_N; i++) {
        if (strcmp(TEMPORARY_TOOLS[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

static pc_status run_keyboard_shortcuts(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                        pc_error *err)
{
    const char *cmd = "edit.keyboardShortcuts";
    yyjson_mut_val *shortcuts = walk(yyjson_mut_doc_get_root(s->prefs.values), "shortcuts");
    /* reset: true clears all; array removes those ids. */
    yyjson_val *reset = pc_json_get(p, "reset");
    if (reset != NULL) {
        if (yyjson_is_true(reset)) {
            yyjson_mut_obj_clear(shortcuts);
            s->prefs.rev++;
        } else if (yyjson_is_arr(reset)) {
            size_t idx, max;
            yyjson_val *item;
            yyjson_arr_foreach(reset, idx, max, item)
            {
                const char *id = yyjson_get_str(item);
                if (id != NULL) {
                    yyjson_mut_obj_remove_key(shortcuts, id);
                }
            }
            s->prefs.rev++;
        }
    }
    yyjson_val *importkys = pc_json_get(p, "importKys");
    if (importkys != NULL) {
        /* The Photoshop .kys importer (crate::kys) is not ported yet; it is an orphan module in
         * the reference (only prefs.rs uses it). Deferred (REWRITE_PLAN.md section 7). */
        return pc_fail(err, PC_ERR_UNSUPPORTED, "importing a Photoshop .kys set is not supported yet");
    }
    yyjson_val *set = pc_json_get(p, "set");
    if (set != NULL && yyjson_is_obj(set)) {
        bool allow_unknown = false;
        yyjson_val *au = pc_json_get(p, "allowUnknown");
        if (au != NULL && yyjson_is_true(au)) {
            allow_unknown = true;
        }
        /* Validate all ids/values against a scratch copy, all-or-nothing. */
        pc_engine_prefs scratch;
        scratch.rev = s->prefs.rev;
        scratch.values = yyjson_mut_doc_mut_copy(s->prefs.values, NULL);
        if (scratch.values == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "prefs");
        }
        char msg[PC_ERROR_MSG_MAX];
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(set, idx, max, key, val)
        {
            const char *id = yyjson_get_str(key);
            if (id == NULL) {
                continue;
            }
            if (!is_bindable(id) && !allow_unknown) {
                yyjson_mut_doc_free(scratch.values);
                return pc_engine_err_bad_params(err, cmd, "unknown command `%s`", id);
            }
            char sub[256];
            snprintf(sub, sizeof sub, "shortcuts.%s", id);
            if (!prefs_set_one(&scratch, sub, val, msg, sizeof msg)) {
                yyjson_mut_doc_free(scratch.values);
                return pc_engine_err_bad_params(err, cmd, "%s", msg);
            }
        }
        /* removeConflicts (default true): a shortcut moved to a new command is stripped from the
         * command that held it (except ids this same call assigns). */
        bool remove_conflicts = true;
        yyjson_val *rc = pc_json_get(p, "removeConflicts");
        if (rc != NULL && yyjson_is_bool(rc)) {
            remove_conflicts = yyjson_get_bool(rc);
        }
        if (remove_conflicts) {
            yyjson_mut_val *sc2 = walk(yyjson_mut_doc_get_root(scratch.values), "shortcuts");
            size_t i2, m2;
            yyjson_val *k2, *v2;
            yyjson_obj_foreach(set, i2, m2, k2, v2)
            {
                const char *id = yyjson_get_str(k2);
                const char *scv = yyjson_get_str(v2);
                if (id == NULL || scv == NULL) {
                    continue;
                }
                char norm[128];
                if (!pc_pref_normalize_shortcut(scv, strlen(scv), norm, sizeof norm)) {
                    continue;
                }
                /* every bindable command + temporary tool */
                size_t nc;
                const pc_engine_command *all = pc_engine_commands(&nc);
                for (size_t ci = 0; ci <= nc + (size_t)TEMPORARY_TOOLS_N; ci++) {
                    const char *cid;
                    const char *cdef;
                    if (ci < nc) {
                        cid = all[ci].id;
                        cdef = all[ci].shortcut;
                    } else if (ci < nc + (size_t)TEMPORARY_TOOLS_N) {
                        cid = TEMPORARY_TOOLS[ci - nc].id;
                        cdef = TEMPORARY_TOOLS[ci - nc].def;
                    } else {
                        break;
                    }
                    if (strcmp(cid, id) == 0 || yyjson_obj_get(set, cid) != NULL) {
                        continue;
                    }
                    /* effective shortcut of cid in scratch */
                    char eff[128];
                    bool has = effective_shortcut(&scratch, cid, cdef, eff, sizeof eff);
                    char effn[128];
                    if (has && pc_pref_normalize_shortcut(eff, strlen(eff), effn, sizeof effn) &&
                        strcmp(effn, norm) == 0) {
                        yyjson_mut_obj_put(sc2, yyjson_mut_strcpy(scratch.values, cid),
                                           yyjson_mut_strcpy(scratch.values, ""));
                    }
                }
            }
        }
        yyjson_mut_doc_free(s->prefs.values);
        s->prefs.values = scratch.values;
        s->prefs.rev++;
        pc_engine_apply_prefs(s);
        shortcuts = walk(yyjson_mut_doc_get_root(s->prefs.values), "shortcuts");
    }
    /* Build the result: overrides + the command list + conflicts. */
    char filterbuf[128];
    const char *filter = NULL;
    yyjson_val *fv = pc_json_get(p, "filter");
    if (fv != NULL) {
        const char *f = yyjson_get_str(fv);
        if (f != NULL) {
            size_t fl = strlen(f);
            size_t copy = fl < sizeof filterbuf - 1 ? fl : sizeof filterbuf - 1;
            for (size_t i = 0; i < copy; i++) {
                filterbuf[i] = (char)tolower((unsigned char)f[i]);
            }
            filterbuf[copy] = '\0';
            filter = filterbuf;
        }
    }
    bool list_all = false;
    yyjson_val *lv = pc_json_get(p, "list");
    if (lv != NULL && yyjson_is_true(lv)) {
        list_all = true;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *overrides = shortcuts != NULL ? yyjson_mut_val_mut_copy(d, shortcuts) : yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, root, "overrides", overrides != NULL ? overrides : yyjson_mut_obj(d));
    yyjson_mut_val *list = yyjson_mut_arr(d);
    size_t nc;
    const pc_engine_command *all = pc_engine_commands(&nc);
    for (size_t ci = 0; ci < nc + (size_t)TEMPORARY_TOOLS_N; ci++) {
        const char *id, *label, *def;
        const char *const *menu = NULL;
        size_t menu_len = 0;
        bool hold = false;
        if (ci < nc) {
            id = all[ci].id;
            label = all[ci].label;
            def = all[ci].shortcut;
            menu = all[ci].menu;
            menu_len = all[ci].menu_len;
        } else {
            int t = (int)(ci - nc);
            id = TEMPORARY_TOOLS[t].id;
            label = TEMPORARY_TOOLS[t].label;
            def = TEMPORARY_TOOLS[t].def;
            hold = true;
        }
        /* filter by id or label (case-insensitive substring). */
        if (filter != NULL) {
            char idl[256], labl[256];
            size_t i;
            for (i = 0; id[i] != '\0' && i < sizeof idl - 1; i++) {
                idl[i] = (char)tolower((unsigned char)id[i]);
            }
            idl[i] = '\0';
            for (i = 0; label[i] != '\0' && i < sizeof labl - 1; i++) {
                labl[i] = (char)tolower((unsigned char)label[i]);
            }
            labl[i] = '\0';
            if (strstr(idl, filter) == NULL && strstr(labl, filter) == NULL) {
                continue;
            }
        }
        bool has_override = false;
        if (shortcuts != NULL && yyjson_mut_obj_get(shortcuts, id) != NULL) {
            has_override = true;
        }
        if (!list_all && filter == NULL && !has_override) {
            continue;
        }
        yyjson_mut_val *row = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, row, "id", id);
        yyjson_mut_obj_add_strcpy(d, row, "label", label);
        yyjson_mut_val *marr = yyjson_mut_arr(d);
        if (ci >= nc) {
            yyjson_mut_arr_add_strcpy(d, marr, "Tools");
            yyjson_mut_arr_add_strcpy(d, marr, "Temporary");
        } else {
            for (size_t mi = 0; mi < menu_len; mi++) {
                yyjson_mut_arr_add_strcpy(d, marr, menu[mi]);
            }
        }
        yyjson_mut_obj_add_val(d, row, "menu", marr);
        if (def != NULL) {
            yyjson_mut_obj_add_strcpy(d, row, "default", def);
        } else {
            yyjson_mut_obj_add_null(d, row, "default");
        }
        char eff[128];
        if (effective_shortcut(&s->prefs, id, def, eff, sizeof eff)) {
            yyjson_mut_obj_add_strcpy(d, row, "shortcut", eff);
        } else {
            yyjson_mut_obj_add_null(d, row, "shortcut");
        }
        if (hold) {
            yyjson_mut_obj_add_bool(d, row, "hold", true);
        }
        yyjson_mut_arr_add_val(list, row);
    }
    yyjson_mut_obj_add_val(d, root, "commands", list);
    /* conflicts: group bindable ids by normalised effective shortcut; report groups > 1. */
    yyjson_mut_val *conflicts = yyjson_mut_arr(d);
    /* Collect (normShortcut, id) pairs. */
    for (size_t a = 0; a < nc + (size_t)TEMPORARY_TOOLS_N; a++) {
        const char *ida, *defa;
        if (a < nc) {
            ida = all[a].id;
            defa = all[a].shortcut;
        } else {
            ida = TEMPORARY_TOOLS[a - nc].id;
            defa = TEMPORARY_TOOLS[a - nc].def;
        }
        char effa[128], norma[128];
        if (!effective_shortcut(&s->prefs, ida, defa, effa, sizeof effa)) {
            continue;
        }
        if (!pc_pref_normalize_shortcut(effa, strlen(effa), norma, sizeof norma)) {
            continue;
        }
        /* skip if an earlier index already produced this shortcut (dedup groups). */
        bool first = true;
        for (size_t b = 0; b < a; b++) {
            const char *idb, *defb;
            if (b < nc) {
                idb = all[b].id;
                defb = all[b].shortcut;
            } else {
                idb = TEMPORARY_TOOLS[b - nc].id;
                defb = TEMPORARY_TOOLS[b - nc].def;
            }
            char effb[128], normb[128];
            if (effective_shortcut(&s->prefs, idb, defb, effb, sizeof effb) &&
                pc_pref_normalize_shortcut(effb, strlen(effb), normb, sizeof normb) &&
                strcmp(normb, norma) == 0) {
                first = false;
                break;
            }
        }
        if (!first) {
            continue;
        }
        /* gather every id sharing norma. */
        yyjson_mut_val *ids = yyjson_mut_arr(d);
        size_t count = 0;
        for (size_t b = 0; b < nc + (size_t)TEMPORARY_TOOLS_N; b++) {
            const char *idb, *defb;
            if (b < nc) {
                idb = all[b].id;
                defb = all[b].shortcut;
            } else {
                idb = TEMPORARY_TOOLS[b - nc].id;
                defb = TEMPORARY_TOOLS[b - nc].def;
            }
            char effb[128], normb[128];
            if (effective_shortcut(&s->prefs, idb, defb, effb, sizeof effb) &&
                pc_pref_normalize_shortcut(effb, strlen(effb), normb, sizeof normb) &&
                strcmp(normb, norma) == 0) {
                yyjson_mut_arr_add_strcpy(d, ids, idb);
                count++;
            }
        }
        if (count > 1) {
            yyjson_mut_val *co = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, co, "shortcut", norma);
            yyjson_mut_obj_add_val(d, co, "commands", ids);
            yyjson_mut_arr_add_val(conflicts, co);
        }
    }
    yyjson_mut_obj_add_val(d, root, "conflicts", conflicts);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ========================================================================================== *
 * Command specs
 * ========================================================================================== */

/* Dispatchers for the 18 edit.preferences.<section> commands: inject __section from the id. */
#define SECTION_RUN(fn, name)                                                                                \
    static pc_status fn(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)              \
    {                                                                                                        \
        (void)p;                                                                                             \
        yyjson_doc *d = NULL;                                                                                \
        char j[64];                                                                                          \
        snprintf(j, sizeof j, "{\"__section\":\"%s\"}", name);                                               \
        if (pc_json_parse(j, strlen(j), &d, err) != PC_OK) {                                                 \
            return PC_ERR_NOMEM;                                                                             \
        }                                                                                                    \
        pc_status rc = run_preferences_section(s, yyjson_doc_get_root(d), out, err);                         \
        yyjson_doc_free(d);                                                                                  \
        return rc;                                                                                           \
    }

SECTION_RUN(run_sec_general, "general")
SECTION_RUN(run_sec_interface, "interface")
SECTION_RUN(run_sec_workspace, "workspace")
SECTION_RUN(run_sec_tools, "tools")
SECTION_RUN(run_sec_historyLog, "historyLog")
SECTION_RUN(run_sec_fileHandling, "fileHandling")
SECTION_RUN(run_sec_export, "export")
SECTION_RUN(run_sec_performance, "performance")
SECTION_RUN(run_sec_scratchDisks, "scratchDisks")
SECTION_RUN(run_sec_cursors, "cursors")
SECTION_RUN(run_sec_transparencyAndGamut, "transparencyAndGamut")
SECTION_RUN(run_sec_unitsAndRulers, "unitsAndRulers")
SECTION_RUN(run_sec_guidesGridAndSlices, "guidesGridAndSlices")
SECTION_RUN(run_sec_plugIns, "plugIns")
SECTION_RUN(run_sec_type, "type")
SECTION_RUN(run_sec_enhancedControls, "enhancedControls")
SECTION_RUN(run_sec_rawDefaults, "rawDefaults")
SECTION_RUN(run_sec_integrations, "integrations")

static const char *const MENU_EDIT[] = {"Edit"};
static const char *const MENU_EDIT_PREFS[] = {"Edit", "Preferences"};

const pc_engine_command *pc_engine_prefs_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"prefs.get", "Get Preferences", NULL, 0, NULL, "{\"path\":\"section.key\"?=everything}",
         pc_engine_always, run_prefs_get, false},
        {"prefs.set", "Set Preferences", NULL, 0, NULL,
         "{\"path\":\"section.key\",\"value\":json} or {\"values\":{\"section.key\":json,...}} "
         "(validated; all or nothing; replace:bool=false replaces whole shortcuts/menus/toolbar/workspaces "
         "maps)",
         pc_engine_always, run_prefs_set, false},
        {"prefs.reset", "Reset Preferences", NULL, 0, NULL, "{\"path\":\"section|section.key\"?=everything}",
         pc_engine_always, run_prefs_reset, false},
        {"edit.preferences.general", "General\xe2\x80\xa6", MENU_EDIT_PREFS, 2, "Cmd+K", "{}",
         pc_engine_always, run_sec_general, false},
        {"edit.preferences.interface", "Interface\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}",
         pc_engine_always, run_sec_interface, false},
        {"edit.preferences.workspace", "Workspace\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}",
         pc_engine_always, run_sec_workspace, false},
        {"edit.preferences.tools", "Tools\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}", pc_engine_always,
         run_sec_tools, false},
        {"edit.preferences.historyLog", "History Log\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}",
         pc_engine_always, run_sec_historyLog, false},
        {"edit.preferences.fileHandling", "File Handling\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}",
         pc_engine_always, run_sec_fileHandling, false},
        {"edit.preferences.export", "Export\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}", pc_engine_always,
         run_sec_export, false},
        {"edit.preferences.performance", "Performance\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}",
         pc_engine_always, run_sec_performance, false},
        {"edit.preferences.scratchDisks", "Scratch Disks\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}",
         pc_engine_always, run_sec_scratchDisks, false},
        {"edit.preferences.cursors", "Cursors\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}", pc_engine_always,
         run_sec_cursors, false},
        {"edit.preferences.transparencyAndGamut", "Transparency & Gamut\xe2\x80\xa6", MENU_EDIT_PREFS, 2,
         NULL, "{}", pc_engine_always, run_sec_transparencyAndGamut, false},
        {"edit.preferences.unitsAndRulers", "Units & Rulers\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}",
         pc_engine_always, run_sec_unitsAndRulers, false},
        {"edit.preferences.guidesGridAndSlices", "Guides, Grid & Slices\xe2\x80\xa6", MENU_EDIT_PREFS, 2,
         NULL, "{}", pc_engine_always, run_sec_guidesGridAndSlices, false},
        {"edit.preferences.plugIns", "Plug-ins\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}", pc_engine_always,
         run_sec_plugIns, false},
        {"edit.preferences.type", "Type\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}", pc_engine_always,
         run_sec_type, false},
        {"edit.preferences.enhancedControls", "Enhanced Controls\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}",
         pc_engine_always, run_sec_enhancedControls, false},
        {"edit.preferences.rawDefaults", "Camera Raw\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}",
         pc_engine_always, run_sec_rawDefaults, false},
        {"edit.preferences.integrations", "Integrations\xe2\x80\xa6", MENU_EDIT_PREFS, 2, NULL, "{}",
         pc_engine_always, run_sec_integrations, false},
        {"edit.keyboardShortcuts", "Keyboard Shortcuts\xe2\x80\xa6", MENU_EDIT, 1, "Cmd+Alt+Shift+K",
         "{\"set\":{\"<command id>|tools.temporary.hand|zoomIn|zoomOut\":\"Cmd+Shift+X\"|\"\"(remove)"
         "|null(default)}?,\"reset\":true|[\"<id>\",...]?,\"removeConflicts\":bool=true,\"filter\":"
         "str?,\"list\":bool=false,\"importKys\":\"<.kys XML>\"?}",
         pc_engine_always, run_keyboard_shortcuts, true},
        {"edit.menus", "Menus\xe2\x80\xa6", MENU_EDIT, 1, "Cmd+Alt+Shift+M",
         "{\"hide\":[\"<id>\",...]?,\"show\":[\"<id>\",...]?,\"color\":{\"<id>\":\"red|orange|yellow|"
         "green|blue|violet|gray|none\"}?,\"reset\":bool=false}",
         pc_engine_always, run_menus, true},
        {"edit.toolbar", "Toolbar\xe2\x80\xa6", MENU_EDIT, 1, NULL,
         "{\"hidden\":[\"<tool>\",...]?,\"order\":[\"<tool>\",...]?,\"reset\":bool=false}", pc_engine_always,
         run_toolbar, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}

uint64_t pc_engine_prefs_revision(const pc_engine_session *session)
{
    return session ? session->prefs.rev : 0;
}

pc_status pc_engine_prefs_defaults(pc_json_value *out, pc_error *err)
{
    if (!out) {
        return pc_fail(err, PC_ERR_INVALID, "missing Preferences result");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "default Preferences");
    }
    yyjson_mut_val *root = default_tree(doc);
    pc_status status =
        root ? pc_json_value_from_mut(root, out, err) : pc_fail(err, PC_ERR_NOMEM, "default Preferences");
    yyjson_mut_doc_free(doc);
    return status;
}
