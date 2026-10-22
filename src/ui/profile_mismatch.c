#include "profile_mismatch.h"

#include <string.h>

static const pc_ui_pm_option MISSING_OPTS[] = {
    {"preserve", "Leave as is (don't color manage)"},
    {"assignWorking", "Assign working space"},
};
static const pc_ui_pm_option MISMATCH_OPTS[] = {
    {"preserve", "Use the embedded profile (instead of the working space)"},
    {"convert", "Convert document's colors to the working space"},
    {"discard", "Discard the embedded profile (don't color manage)"},
};

bool pc_ui_profile_mismatch_should_open(yyjson_mut_val *report)
{
    if (report == NULL || !yyjson_mut_is_obj(report)) {
        return false;
    }
    bool ask = yyjson_mut_get_bool(yyjson_mut_obj_get(report, "ask"));
    if (!ask) {
        return false;
    }
    bool mismatch = yyjson_mut_get_bool(yyjson_mut_obj_get(report, "mismatch"));
    bool missing = yyjson_mut_obj_get(report, "missing") != NULL;
    return mismatch || missing;
}

bool pc_ui_profile_mismatch_parse(yyjson_mut_val *report, pc_ui_profile_mismatch *out)
{
    if (out == NULL) {
        return false;
    }
    out->missing = false;
    out->embedded = "";
    out->working = "";
    out->initial_action = "preserve";
    if (report == NULL || !yyjson_mut_is_obj(report)) {
        return false;
    }
    out->missing = yyjson_mut_get_bool(yyjson_mut_obj_get(report, "missing"));
    const char *emb = yyjson_mut_get_str(yyjson_mut_obj_get(report, "embedded"));
    const char *work = yyjson_mut_get_str(yyjson_mut_obj_get(report, "working"));
    if (emb != NULL) {
        out->embedded = emb;
    }
    if (work != NULL) {
        out->working = work;
    }
    /* The engine already applied an action on open; start the radio on it. */
    const char *action = yyjson_mut_get_str(yyjson_mut_obj_get(report, "action"));
    if (action != NULL && strcmp(action, "converted") == 0) {
        out->initial_action = "convert";
    } else if (action != NULL && strcmp(action, "discarded") == 0) {
        out->initial_action = "discard";
    } else {
        out->initial_action = "preserve";
    }
    return true;
}

size_t pc_ui_profile_mismatch_options(bool missing, const pc_ui_pm_option **out)
{
    if (missing) {
        if (out != NULL) {
            *out = MISSING_OPTS;
        }
        return sizeof MISSING_OPTS / sizeof MISSING_OPTS[0];
    }
    if (out != NULL) {
        *out = MISMATCH_OPTS;
    }
    return sizeof MISMATCH_OPTS / sizeof MISMATCH_OPTS[0];
}
