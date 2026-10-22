#include "ui/autoform.h"
#include <stdio.h>
#include <string.h>

bool pc_ui_autoform_command(const char *id)
{
    if (!id) {
        return false;
    }
    return !strcmp(id, "file.automate.contactSheetII") || !strcmp(id, "file.scripts.statistics") ||
           !strcmp(id, "file.scripts.scriptEventsManager") || !strcmp(id, "file.package") ||
           !strcmp(id, "file.export.pathsToIllustrator") || !strcmp(id, "file.scripts.browse");
}

const char *pc_ui_autoform_label(const char *id)
{
    if (!id) {
        return NULL;
    }
    if (!strcmp(id, "file.automate.contactSheetII")) {
        return "Contact Sheet II";
    }
    if (!strcmp(id, "file.scripts.statistics")) {
        return "Image Statistics";
    }
    if (!strcmp(id, "file.scripts.scriptEventsManager")) {
        return "Script Events Manager";
    }
    if (!strcmp(id, "file.package")) {
        return "Package";
    }
    if (!strcmp(id, "file.export.pathsToIllustrator")) {
        return "Export Paths to File";
    }
    if (!strcmp(id, "file.scripts.browse")) {
        return "Browse";
    }
    return NULL;
}

const char *pc_ui_autoform_spec(const char *id)
{
    if (!id) {
        return NULL;
    }
    /* Folder/path/script fields are bare `str` (no schema default); the shell seeds a sensible
     * runtime value (the active document's folder, etc.) after parsing. `depth` is an integer so
     * the engine's pc_json_as_i64 accepts it. */
    if (!strcmp(id, "file.automate.contactSheetII")) {
        return "{\"input\":str,"
               "\"units\":\"inches|cm|mm|pixels\"=\"inches\","
               "\"width\":1..100000=8,\"height\":1..100000=10,\"resolution\":1..10000=300,"
               "\"mode\":\"rgb|gray|cmyk|lab\"=\"rgb\",\"depth\":int=8,"
               "\"columns\":1..100=5,\"rows\":1..100=6,"
               "\"placeAcrossFirst\":bool=true,\"autoSpacing\":bool=true,"
               "\"rotateForBestFit\":bool=false,\"caption\":bool=true,"
               "\"fontSize\":1..500=12,\"flatten\":bool=false}";
    }
    if (!strcmp(id, "file.scripts.statistics")) {
        return "{\"mode\":\"mean|median|maximum|minimum|range|summation|variance|"
               "standardDeviation|skewness|kurtosis|entropy\"=\"median\","
               "\"input\":str,\"align\":bool=false}";
    }
    if (!strcmp(id, "file.scripts.scriptEventsManager")) {
        return "{\"enabled\":bool=false,"
               "\"event\":\"startApplication|newDocument|openDocument|saveDocument|closeDocument|"
               "print|export|everything\"=\"openDocument\","
               "\"name\":str,\"script\":str}";
    }
    if (!strcmp(id, "file.package")) {
        return "{\"dir\":str,\"format\":\"pcraft|psd|psb\"=\"pcraft\"}";
    }
    if (!strcmp(id, "file.scripts.browse")) {
        return "{\"path\":str}";
    }
    return NULL; /* pathsToIllustrator is built dynamically */
}

const char *pc_ui_autoform_browse_for(const char *id, pc_ui_autoform_browse *kind)
{
    pc_ui_autoform_browse k = PC_UI_AUTOFORM_BROWSE_NONE;
    const char *field = NULL;
    if (id) {
        if (!strcmp(id, "file.automate.contactSheetII") || !strcmp(id, "file.scripts.statistics")) {
            field = "input";
            k = PC_UI_AUTOFORM_BROWSE_OPEN_FOLDER;
        } else if (!strcmp(id, "file.package")) {
            field = "dir";
            k = PC_UI_AUTOFORM_BROWSE_OPEN_FOLDER;
        } else if (!strcmp(id, "file.export.pathsToIllustrator")) {
            field = "path";
            k = PC_UI_AUTOFORM_BROWSE_SAVE_FILE;
        } else if (!strcmp(id, "file.scripts.browse")) {
            field = "path";
            k = PC_UI_AUTOFORM_BROWSE_OPEN_FILE;
        } else if (!strcmp(id, "file.scripts.scriptEventsManager")) {
            field = "script";
            k = PC_UI_AUTOFORM_BROWSE_OPEN_FILE;
        }
    }
    if (kind) {
        *kind = k;
    }
    return field;
}

/* Append one sanitised, length-capped choice token to out (bounded by cap). */
static size_t append_choice(char *out, size_t cap, size_t at, const char *name)
{
    for (size_t i = 0; name && name[i] && at + 1 < cap; i++) {
        unsigned char c = (unsigned char)name[i];
        if (i >= 48) {
            break; /* cap each token */
        }
        if (c == '|' || c == '"' || c == '\'' || c == '\\' || c < 0x20) {
            c = '_';
        }
        out[at++] = (char)c;
    }
    return at;
}

bool pc_ui_autoform_paths_spec(const char *const *names, size_t count, bool has_work, char *out,
                               size_t cap)
{
    if (!out || cap < 48) {
        return false;
    }
    const char *prefix = "{\"path\":str,\"paths\":\"all";
    size_t at = strlen(prefix);
    if (at + 1 >= cap) {
        return false;
    }
    memcpy(out, prefix, at);
    if (has_work) {
        const char *w = "|work";
        if (at + strlen(w) + 1 >= cap) {
            out[at] = 0;
            return false;
        }
        memcpy(out + at, w, strlen(w));
        at += strlen(w);
    }
    /* The schema parser allows at most 64 choices total; reserve two for "all" and "work". */
    size_t limit = count < 62 ? count : 62;
    for (size_t i = 0; i < limit && names; i++) {
        if (at + 2 >= cap) {
            break;
        }
        out[at++] = '|';
        at = append_choice(out, cap, at, names[i] ? names[i] : "");
    }
    const char *suffix = "\"=\"all\"}";
    if (at + strlen(suffix) + 1 >= cap) {
        out[at < cap ? at : cap - 1] = 0;
        return false;
    }
    memcpy(out + at, suffix, strlen(suffix));
    at += strlen(suffix);
    out[at] = 0;
    return true;
}
