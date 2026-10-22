/* See services.h. Behavioural port of the Linux-relevant parts of services.rs. */
#include "services.h"

#include <string.h>

static bool shell_word_safe(unsigned char b)
{
    if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9')) {
        return true;
    }
    return strchr("/._-+:,@", (char)b) != NULL;
}

/* Append s to out[cap] at *off, keeping it NUL-terminated; false (and out truncated) when full. */
static bool append(char *out, size_t cap, size_t *off, const char *s)
{
    size_t sl = strlen(s);
    if (*off + sl + 1 > cap) {
        return false;
    }
    memcpy(out + *off, s, sl);
    *off += sl;
    out[*off] = '\0';
    return true;
}

pc_status pc_services_shell_word(const char *s, char *out, size_t cap, pc_error *err)
{
    if (!s || !out || cap == 0) {
        return pc_fail(err, PC_ERR_INVALID, "shell_word: missing argument");
    }
    out[0] = '\0';
    bool all_safe = s[0] != '\0'; /* the empty string is not safe bare: it must be quoted '' */
    for (const char *p = s; *p; p++) {
        if (!shell_word_safe((unsigned char)*p)) {
            all_safe = false;
            break;
        }
    }
    if (all_safe) {
        size_t off = 0;
        if (!append(out, cap, &off, s)) {
            out[0] = '\0';
            return pc_fail(err, PC_ERR_LIMIT, "shell_word: result does not fit");
        }
        return PC_OK;
    }
    /* Single-quote, escaping each embedded ' as '\'' (close, escaped quote, reopen). */
    size_t off = 0;
    bool ok = append(out, cap, &off, "'");
    for (const char *p = s; ok && *p; p++) {
        if (*p == '\'') {
            ok = append(out, cap, &off, "'\\''");
        } else {
            char one[2] = {*p, '\0'};
            ok = append(out, cap, &off, one);
        }
    }
    ok = ok && append(out, cap, &off, "'");
    if (!ok) {
        out[0] = '\0';
        return pc_fail(err, PC_ERR_LIMIT, "shell_word: result does not fit");
    }
    return PC_OK;
}

pc_status pc_services_xwayland_command(const char *flatpak_id, const char *appimage, bool x_display,
                                       char *out, size_t cap, bool *available, pc_error *err)
{
    if (!out || cap == 0 || !available) {
        return pc_fail(err, PC_ERR_INVALID, "xwayland_command: missing argument");
    }
    out[0] = '\0';
    *available = false;

    if (flatpak_id && flatpak_id[0] != '\0') {
        char word[PC_APP_PATH_MAX];
        pc_status st = pc_services_shell_word(flatpak_id, word, sizeof word, err);
        if (st != PC_OK) {
            return st;
        }
        size_t off = 0;
        if (!append(out, cap, &off, "flatpak run --nosocket=wayland --socket=x11 ") ||
            !append(out, cap, &off, word)) {
            out[0] = '\0';
            return pc_fail(err, PC_ERR_LIMIT, "xwayland_command: result does not fit");
        }
        *available = true;
        return PC_OK;
    }

    /* Non-Flatpak: an AppImage relaunches by its own path, otherwise `photocraft` on PATH. The
     * relaunch is only possible when there is an X server to run on. */
    if (!x_display) {
        return PC_OK; /* *available stays false, out empty */
    }
    char exe[PC_APP_PATH_MAX];
    if (appimage && appimage[0] != '\0') {
        pc_status st = pc_services_shell_word(appimage, exe, sizeof exe, err);
        if (st != PC_OK) {
            return st;
        }
    } else {
        (void)snprintf(exe, sizeof exe, "photocraft");
    }
    size_t off = 0;
    if (!append(out, cap, &off, "WAYLAND_DISPLAY= ") || !append(out, cap, &off, exe)) {
        out[0] = '\0';
        return pc_fail(err, PC_ERR_LIMIT, "xwayland_command: result does not fit");
    }
    *available = true;
    return PC_OK;
}

void pc_services_beep_to(FILE *f)
{
    if (!f) {
        return;
    }
    (void)fputc('\x07', f);
    (void)fflush(f);
}

void pc_services_beep(void)
{
    pc_services_beep_to(stdout);
}
