/* File > Import > WIA Support (port of crates/engine/src/wia_cmds.rs).
 *
 * Windows Image Acquisition is a Windows-only scanner/camera API; Photoshop shows this item only on
 * Windows. We register it cross-platform and report the available devices -- none off Windows, where
 * the feature is unavailable. Real device enumeration/transfer needs the Windows COM API (IWiaDevMgr)
 * and hardware, which is not wired here. */
#include "engine_internal.h"

static pc_status run_wia(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
#if defined(_WIN32)
    /* A real implementation enumerates WIA devices via COM and transfers an image into a new
     * document; none is wired yet, so report zero devices. */
    yyjson_mut_obj_add_bool(d, root, "available", true);
    yyjson_mut_obj_add_val(d, root, "devices", yyjson_mut_arr(d));
    yyjson_mut_obj_add_str(d, root, "note", "WIA device enumeration not yet wired");
#else
    yyjson_mut_obj_add_bool(d, root, "available", false);
    yyjson_mut_obj_add_val(d, root, "devices", yyjson_mut_arr(d));
    yyjson_mut_obj_add_str(d, root, "note", "WIA scanner/camera import is available on Windows only");
#endif
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static const char *const MENU_FILE_IMPORT[] = {"File", "Import"};

const pc_engine_command *pc_engine_wia_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"file.import.wiaSupport", "WIA Support\xe2\x80\xa6", MENU_FILE_IMPORT, 2, NULL,
         "{} \xe2\x86\x92 {available, devices, note}: acquire from a scanner/camera (Windows only)",
         pc_engine_always, run_wia, false},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
