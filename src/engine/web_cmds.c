/* File > Export > Save for Web (Legacy) / Export Preferences / Quick Export and File > Generate >
 * Image Assets. Port of crates/engine/src/web_cmds.rs (E12).
 *
 * STUBS (REWRITE_PLAN.md §6/§7): per the chunk's web decision these are registered with their
 * exact ids, labels, menus, shortcuts and params so the menu catalogue and parity count stay
 * correct, but return PC_ERR_UNSUPPORTED rather than running. The real behaviour (the Save-for-Web
 * quantiser and slice HTML, Quick Export, the Image Assets name parser and its save-time
 * regeneration) is a web-export follow-up: it needs the GIF/PNG8 quantise + slice export pipeline
 * and the prefs export-settings schema, which are not wired in this chunk. Nothing here crashes on
 * any params. */
#include "engine_internal.h"

static pc_status web_has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return PC_OK;
}

static pc_status web_always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

static pc_status stub(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err,
                      const char *what)
{
    (void)s;
    (void)p;
    (void)out;
    return pc_fail(err, PC_ERR_UNSUPPORTED,
                   "%s is not available yet in this build (web-export pipeline pending)", what);
}

static pc_status run_save_for_web(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return stub(s, p, out, err, "Save for Web");
}
static pc_status run_export_prefs(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return stub(s, p, out, err, "Export Preferences");
}
static pc_status run_quick_export(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return stub(s, p, out, err, "Quick Export");
}
static pc_status run_image_assets(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return stub(s, p, out, err, "Image Assets");
}

static const char *const MENU_FILE_EXPORT[] = {"File", "Export"};
static const char *const MENU_FILE_GENERATE[] = {"File", "Generate"};

static const pc_engine_command SPECS[] = {
    {"file.export.saveForWebLegacy", "Save for Web (Legacy)\xE2\x80\xA6", MENU_FILE_EXPORT, 2,
     "Cmd+Alt+Shift+S",
     "{\"preset\":\"GIF 128 Dithered|JPEG High|PNG-24|\xE2\x80\xA6\"?,\"format\":\"gif|png8|png24|jpeg|"
     "wbmp\"=\"png24\", \xE2\x80\xA6 image-size and slice options} \xE2\x86\x92 estimate, or {files, "
     "html, bytes}",
     web_has_doc, run_save_for_web, true},
    {"file.export.exportPreferences", "Export Preferences\xE2\x80\xA6", MENU_FILE_EXPORT, 2, NULL,
     "{\"quickExportFormat\":\"png|jpg|gif|webp\"?,\"quickExportLocation\":\"ask|sameFolder\"?,"
     "\"jpegQuality\":1..100?,\"webpLossless\":bool?,\"webpQuality\":1..100?,\"metadata\":\"none|"
     "copyright|all\"?,\"convertToSrgb\":bool?} \xE2\x86\x92 {values}",
     web_always, run_export_prefs, true},
    {"file.export.quickExport", "Quick Export", NULL, 0, NULL,
     "{\"path\":str? (required unless Export Preferences \xE2\x80\xBA Location is \"sameFolder\" and the "
     "document is saved)} \xE2\x86\x92 {path, format, bytes}",
     web_has_doc, run_quick_export, true},
    {"file.generate.imageAssets", "Image Assets", MENU_FILE_GENERATE, 2, NULL,
     "{\"on\":bool? (default: toggle),\"dir\":folder? (default <document>-assets next to the file)} "
     "\xE2\x86\x92 {enabled, files, errors}",
     web_has_doc, run_image_assets, true},
};

const pc_engine_command *pc_engine_web_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
