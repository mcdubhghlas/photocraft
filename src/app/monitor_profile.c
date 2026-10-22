/* See monitor_profile.h. Behavioural port of the pure parts of monitor_profile.rs (Linux), plus the
 * live per-monitor ICC resolution over colord (dbus-1) and the RandR _ICC_PROFILE atom (xcb), each
 * under its build guard. */
#include "monitor_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/file.h"
#include "cms/cms.h"
#include "cms/profile.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

pc_status pc_monitor_icc_validate(const uint8_t *bytes, size_t len, size_t *out_valid_len,
                                  pc_error *err)
{
    if (!bytes) {
        return pc_fail(err, PC_ERR_INVALID, "ICC validate: missing bytes");
    }
    if (len > PC_MONITOR_MAX_ICC_BYTES) {
        return pc_fail(err, PC_ERR_LIMIT, "display ICC profile exceeds the memory budget");
    }
    if (len < 132 || memcmp(bytes + 36, "acsp", 4) != 0) {
        return pc_fail(err, PC_ERR_FORMAT, "not an ICC profile (missing 'acsp' signature)");
    }
    size_t declared = be32(bytes);
    if (declared < 132 || declared > len) {
        return pc_fail(err, PC_ERR_FORMAT, "ICC profile declared size is out of range");
    }
    pc_profile profile;
    pc_status st = pc_profile_parse(bytes, declared, &profile, err);
    if (st != PC_OK) {
        return st;
    }
    bool rgb = profile.space == PC_CS_RGB;
    pc_profile_fini(&profile);
    if (!rgb) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "display ICC profile is not an RGB profile");
    }
    if (out_valid_len) {
        *out_valid_len = declared;
    }
    return PC_OK;
}

pc_status pc_monitor_read_icc(const char *path, pc_buf *out, pc_error *err)
{
    if (!path || !out) {
        return pc_fail(err, PC_ERR_INVALID, "read ICC: missing argument");
    }
    pc_buf tmp = PC_BUF_INIT;
    pc_status st = pc_file_read(path, PC_MONITOR_MAX_ICC_BYTES, &tmp, err);
    if (st != PC_OK) {
        pc_buf_fini(&tmp);
        return st;
    }
    size_t valid = 0;
    st = pc_monitor_icc_validate(tmp.data, tmp.len, &valid, err);
    if (st != PC_OK) {
        pc_buf_fini(&tmp);
        return st;
    }
    tmp.len = valid; /* keep only the declared bytes; data stays allocated */
    pc_buf_fini(out);
    *out = tmp;
    return PC_OK;
}

/* Append dir (NUL-terminated) to out if there is room and it is non-empty. */
static void push_dir(char out[][PC_APP_PATH_MAX], size_t max, size_t *n, const char *dir)
{
    if (*n >= max || !dir || dir[0] == '\0') {
        return;
    }
    size_t len = strlen(dir);
    if (len >= PC_APP_PATH_MAX) {
        return;
    }
    memcpy(out[*n], dir, len + 1);
    (*n)++;
}

static void push_join(char out[][PC_APP_PATH_MAX], size_t max, size_t *n, const char *base,
                      const char *leaf)
{
    if (*n >= max || !base || base[0] == '\0') {
        return;
    }
    char path[PC_APP_PATH_MAX];
    int written = snprintf(path, sizeof path, "%s/%s", base, leaf);
    if (written > 0 && (size_t)written < sizeof path) {
        push_dir(out, max, n, path);
    }
}

size_t pc_monitor_icc_search_dirs(pc_app_env_fn env, void *ctx, char out[][PC_APP_PATH_MAX],
                                  size_t max)
{
    size_t n = 0;
    if (max == 0) {
        return 0;
    }
    const char *xdg_data_home = env ? env(ctx, "XDG_DATA_HOME") : NULL;
    if (xdg_data_home && xdg_data_home[0] != '\0') {
        push_join(out, max, &n, xdg_data_home, "icc");
    } else {
        const char *home = env ? env(ctx, "HOME") : NULL;
        if (home && home[0] != '\0') {
            push_join(out, max, &n, home, ".local/share/icc");
        }
    }
    const char *xdg_data_dirs = env ? env(ctx, "XDG_DATA_DIRS") : NULL;
    if (xdg_data_dirs && xdg_data_dirs[0] != '\0') {
        char copy[PC_APP_PATH_MAX];
        size_t dl = strlen(xdg_data_dirs);
        if (dl < sizeof copy) {
            memcpy(copy, xdg_data_dirs, dl + 1);
            char *save = NULL;
            for (char *tok = strtok_r(copy, ":", &save); tok && n < max;
                 tok = strtok_r(NULL, ":", &save)) {
                push_join(out, max, &n, tok, "color/icc");
            }
        }
    }
    static const char *const system_dirs[] = {"/var/lib/colord/icc", "/usr/local/share/color/icc",
                                              "/usr/share/color/icc"};
    for (size_t i = 0; i < sizeof system_dirs / sizeof system_dirs[0]; i++) {
        push_dir(out, max, &n, system_dirs[i]);
    }
    return n;
}

bool pc_monitor_clone_unambiguous(const uint64_t *const candidates[], const size_t counts[],
                                  size_t n_outputs, uint64_t *out_hash)
{
    if (!candidates || !counts || n_outputs == 0) {
        return false;
    }
    bool have_selected = false;
    uint64_t selected = 0;
    for (size_t i = 0; i < n_outputs; i++) {
        if (counts[i] == 0 || !candidates[i]) {
            return false; /* a missing profile is ambiguous */
        }
        uint64_t first = candidates[i][0];
        for (size_t j = 1; j < counts[i]; j++) {
            if (candidates[i][j] != first) {
                return false; /* this output has conflicting candidates */
            }
        }
        if (have_selected && selected != first) {
            return false; /* outputs disagree */
        }
        selected = first;
        have_selected = true;
    }
    if (out_hash) {
        *out_hash = selected;
    }
    return true;
}

/* ---------------------------------------------------------------------------------------------- */
/* Live colord (dbus-1) resolution: output name -> device -> default profile Filename -> ICC bytes. */

#ifndef PC_NO_COLORD

#include <dbus/dbus.h>

#define COLORD_DEST "org.freedesktop.ColorManager"
#define COLORD_ROOT "/org/freedesktop/ColorManager"
#define COLORD_MGR "org.freedesktop.ColorManager"
#define COLORD_DEVICE "org.freedesktop.ColorManager.Device"
#define COLORD_PROFILE "org.freedesktop.ColorManager.Profile"
#define DBUS_PROPS "org.freedesktop.DBus.Properties"
#define COLORD_TIMEOUT_MS 2000

/* Properties.Get(iface, prop) on (dest, path) whose variant is a string, copied into out[cap].
 * Returns true on success; leaves out untouched on failure. */
static bool prop_string(DBusConnection *conn, const char *path, const char *iface, const char *prop,
                        char *out, size_t cap)
{
    DBusMessage *msg = dbus_message_new_method_call(COLORD_DEST, path, DBUS_PROPS, "Get");
    if (!msg) {
        return false;
    }
    bool ok = false;
    if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
                                 DBUS_TYPE_INVALID)) {
        DBusError derr;
        dbus_error_init(&derr);
        DBusMessage *reply =
            dbus_connection_send_with_reply_and_block(conn, msg, COLORD_TIMEOUT_MS, &derr);
        if (reply) {
            DBusMessageIter it, var;
            if (dbus_message_iter_init(reply, &it) &&
                dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&it, &var);
                if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&var, &s);
                    if (s) {
                        size_t len = strlen(s);
                        if (len < cap) {
                            memcpy(out, s, len + 1);
                            ok = true;
                        }
                    }
                }
            }
            dbus_message_unref(reply);
        }
        dbus_error_free(&derr);
    }
    dbus_message_unref(msg);
    return ok;
}

/* True when the colord device at `path` has XRANDR_name metadata equal to `output_name`. */
static bool device_matches_output(DBusConnection *conn, const char *path, const char *output_name)
{
    DBusMessage *msg = dbus_message_new_method_call(COLORD_DEST, path, DBUS_PROPS, "Get");
    if (!msg) {
        return false;
    }
    const char *iface = COLORD_DEVICE;
    const char *prop = "Metadata";
    bool match = false;
    if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
                                 DBUS_TYPE_INVALID)) {
        DBusError derr;
        dbus_error_init(&derr);
        DBusMessage *reply =
            dbus_connection_send_with_reply_and_block(conn, msg, COLORD_TIMEOUT_MS, &derr);
        if (reply) {
            DBusMessageIter it, var, arr, entry;
            if (dbus_message_iter_init(reply, &it) &&
                dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&it, &var);
                if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&var, &arr);
                    while (!match && dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
                        dbus_message_iter_recurse(&arr, &entry);
                        const char *key = NULL;
                        const char *val = NULL;
                        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
                            dbus_message_iter_get_basic(&entry, &key);
                            if (dbus_message_iter_next(&entry) &&
                                dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
                                dbus_message_iter_get_basic(&entry, &val);
                            }
                        }
                        if (key && val && strcmp(key, "XRANDR_name") == 0 &&
                            strcmp(val, output_name) == 0) {
                            match = true;
                        }
                        dbus_message_iter_next(&arr);
                    }
                }
            }
            dbus_message_unref(reply);
        }
        dbus_error_free(&derr);
    }
    dbus_message_unref(msg);
    return match;
}

/* The object path of the colord device's default ("*") profile, copied into out[cap]. */
static bool device_default_profile_path(DBusConnection *conn, const char *device_path, char *out,
                                        size_t cap)
{
    DBusMessage *msg =
        dbus_message_new_method_call(COLORD_DEST, device_path, COLORD_DEVICE, "GetProfileForQualifiers");
    if (!msg) {
        return false;
    }
    bool ok = false;
    const char *star = "*";
    const char **quals = &star;
    if (dbus_message_append_args(msg, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &quals, 1,
                                 DBUS_TYPE_INVALID)) {
        DBusError derr;
        dbus_error_init(&derr);
        DBusMessage *reply =
            dbus_connection_send_with_reply_and_block(conn, msg, COLORD_TIMEOUT_MS, &derr);
        if (reply) {
            DBusMessageIter it;
            if (dbus_message_iter_init(reply, &it) &&
                dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
                const char *p = NULL;
                dbus_message_iter_get_basic(&it, &p);
                if (p) {
                    size_t len = strlen(p);
                    if (len < cap) {
                        memcpy(out, p, len + 1);
                        ok = true;
                    }
                }
            }
            dbus_message_unref(reply);
        }
        dbus_error_free(&derr);
    }
    dbus_message_unref(msg);
    return ok;
}

pc_status pc_monitor_colord_profile(const char *output_name, pc_buf *out, pc_error *err)
{
    if (!out) {
        return pc_fail(err, PC_ERR_INVALID, "colord profile: missing output buffer");
    }
    if (!output_name || output_name[0] == '\0') {
        return PC_OK; /* nothing to match; no profile */
    }
    DBusError derr;
    dbus_error_init(&derr);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &derr);
    if (!conn) {
        pc_status st = pc_fail(err, PC_ERR_UNSUPPORTED, "colord is unavailable: %s",
                               dbus_error_is_set(&derr) ? derr.message : "no system D-Bus");
        dbus_error_free(&derr);
        return st;
    }
    dbus_error_free(&derr);
    /* A shared bus connection must not take the whole process down if the bus drops. */
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    DBusMessage *msg =
        dbus_message_new_method_call(COLORD_DEST, COLORD_ROOT, COLORD_MGR, "GetDevicesByKind");
    if (!msg) {
        dbus_connection_unref(conn);
        return pc_fail(err, PC_ERR_NOMEM, "colord: could not build the device query");
    }
    const char *kind = "display";
    pc_status result = PC_OK;
    if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &kind, DBUS_TYPE_INVALID)) {
        result = pc_fail(err, PC_ERR_NOMEM, "colord: could not build the device query");
    } else {
        dbus_error_init(&derr);
        DBusMessage *reply =
            dbus_connection_send_with_reply_and_block(conn, msg, COLORD_TIMEOUT_MS, &derr);
        if (!reply) {
            result = pc_fail(err, PC_ERR_UNSUPPORTED, "colord could not list displays: %s",
                             dbus_error_is_set(&derr) ? derr.message : "no reply");
        } else {
            DBusMessageIter it, arr;
            bool done = false;
            if (dbus_message_iter_init(reply, &it) &&
                dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                dbus_message_iter_recurse(&it, &arr);
                while (!done && dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_OBJECT_PATH) {
                    const char *dev_path = NULL;
                    dbus_message_iter_get_basic(&arr, &dev_path);
                    if (dev_path && device_matches_output(conn, dev_path, output_name)) {
                        char prof_path[256];
                        char filename[PC_APP_PATH_MAX];
                        if (device_default_profile_path(conn, dev_path, prof_path, sizeof prof_path) &&
                            prop_string(conn, prof_path, COLORD_PROFILE, "Filename", filename,
                                        sizeof filename) &&
                            filename[0] != '\0') {
                            pc_buf tmp = PC_BUF_INIT;
                            if (pc_monitor_read_icc(filename, &tmp, NULL) == PC_OK) {
                                pc_buf_fini(out);
                                *out = tmp;
                            } else {
                                pc_buf_fini(&tmp);
                            }
                        }
                        done = true; /* the matching device decides, profile or not */
                    }
                    dbus_message_iter_next(&arr);
                }
            }
            dbus_message_unref(reply);
        }
        dbus_error_free(&derr);
    }
    dbus_message_unref(msg);
    dbus_connection_unref(conn);
    return result;
}

#else /* PC_NO_COLORD */

pc_status pc_monitor_colord_profile(const char *output_name, pc_buf *out, pc_error *err)
{
    (void)output_name;
    if (!out) {
        return pc_fail(err, PC_ERR_INVALID, "colord profile: missing output buffer");
    }
    return pc_fail(err, PC_ERR_UNSUPPORTED,
                   "live colord monitor profiles need libdbus-1, which this build does not link "
                   "(REWRITE_PLAN P2); displays use the sRGB fallback");
}

#endif /* PC_NO_COLORD */

/* ---------------------------------------------------------------------------------------------- */
/* RandR _ICC_PROFILE[_N] root-window atom fallback (xcb). */

#ifndef PC_NO_X11SCREEN

#include <xcb/xcb.h>

static xcb_screen_t *mp_screen_of(xcb_connection_t *conn, int num)
{
    const xcb_setup_t *setup = xcb_get_setup(conn);
    if (!setup) {
        return NULL;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < num && it.rem; i++) {
        xcb_screen_next(&it);
    }
    return it.rem ? it.data : NULL;
}

static xcb_atom_t mp_intern(xcb_connection_t *conn, const char *name)
{
    size_t len = strlen(name);
    if (len == 0u || len > 0xffffu) {
        return XCB_ATOM_NONE;
    }
    xcb_intern_atom_cookie_t c = xcb_intern_atom(conn, 1, (uint16_t)len, name);
    xcb_generic_error_t *e = NULL;
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, c, &e);
    xcb_atom_t atom = XCB_ATOM_NONE;
    if (r) {
        atom = r->atom;
        free(r);
    }
    free(e);
    return atom;
}

pc_status pc_monitor_randr_atom_profile(uint32_t atom_index, pc_buf *out, pc_error *err)
{
    if (!out) {
        return pc_fail(err, PC_ERR_INVALID, "RandR ICC: missing output buffer");
    }
    char name[32];
    if (atom_index == 0u) {
        memcpy(name, "_ICC_PROFILE", sizeof "_ICC_PROFILE");
    } else {
        int n = snprintf(name, sizeof name, "_ICC_PROFILE_%u", atom_index);
        if (n <= 0 || (size_t)n >= sizeof name) {
            return pc_fail(err, PC_ERR_INVALID, "invalid _ICC_PROFILE atom index");
        }
    }
    int screen_num = 0;
    xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
    if (!conn || xcb_connection_has_error(conn)) {
        if (conn) {
            xcb_disconnect(conn);
        }
        return pc_fail(err, PC_ERR_UNSUPPORTED,
                       "no X11 display for the _ICC_PROFILE atom (is DISPLAY set?)");
    }
    xcb_screen_t *screen = mp_screen_of(conn, screen_num);
    if (!screen) {
        xcb_disconnect(conn);
        return pc_fail(err, PC_ERR_UNSUPPORTED, "the X11 server provided no screen");
    }
    xcb_atom_t atom = mp_intern(conn, name);
    if (atom != XCB_ATOM_NONE) {
        /* Read up to 16 MiB (4 Mi 32-bit units); pc_monitor_icc_validate bounds and truncates. */
        xcb_get_property_cookie_t c =
            xcb_get_property(conn, 0, screen->root, atom, XCB_ATOM_ANY, 0, (16u * 1024u * 1024u) / 4u);
        xcb_generic_error_t *e = NULL;
        xcb_get_property_reply_t *r = xcb_get_property_reply(conn, c, &e);
        free(e);
        if (r) {
            if (r->format == 8 && r->bytes_after == 0) {
                const uint8_t *data = xcb_get_property_value(r);
                int len = xcb_get_property_value_length(r);
                size_t valid = 0;
                if (data && len > 0 &&
                    pc_monitor_icc_validate(data, (size_t)len, &valid, NULL) == PC_OK) {
                    pc_buf tmp = PC_BUF_INIT;
                    if (pc_buf_append(&tmp, data, valid, NULL) == PC_OK) {
                        pc_buf_fini(out);
                        *out = tmp;
                    } else {
                        pc_buf_fini(&tmp);
                    }
                }
            }
            free(r);
        }
    }
    xcb_disconnect(conn);
    return PC_OK;
}

#else /* PC_NO_X11SCREEN */

pc_status pc_monitor_randr_atom_profile(uint32_t atom_index, pc_buf *out, pc_error *err)
{
    (void)atom_index;
    if (!out) {
        return pc_fail(err, PC_ERR_INVALID, "RandR ICC: missing output buffer");
    }
    return pc_fail(err, PC_ERR_UNSUPPORTED,
                   "the RandR _ICC_PROFILE atom needs libxcb, which this build does not link "
                   "(REWRITE_PLAN P2); displays use the sRGB fallback");
}

#endif /* PC_NO_X11SCREEN */

pc_status pc_monitor_profile_for_output(const char *output_name, uint32_t atom_index, pc_buf *out,
                                        pc_error *err)
{
    if (!out) {
        return pc_fail(err, PC_ERR_INVALID, "monitor profile: missing output buffer");
    }
    pc_error scratch;
    pc_status st = pc_monitor_colord_profile(output_name, out, &scratch);
    if (st == PC_OK && out->len > 0) {
        return PC_OK;
    }
    pc_buf_truncate(out, 0);
    st = pc_monitor_randr_atom_profile(atom_index, out, &scratch);
    if (st == PC_OK && out->len > 0) {
        return PC_OK;
    }
    pc_buf_truncate(out, 0);
    return PC_OK;
}
