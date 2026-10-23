/* U10: OS clipboard integration (image in and out).
 *
 * OUT: mirrors the session clipboard onto the OS clipboard as PNG after Copy / Copy Merged / Cut, so
 * an image copied in PhotoCraft can be pasted into other apps (the `export_os_clipboard` half of the
 * reference's ui-egui clipboard services). Text clipboard is handled by the type tool (type_ui.c,
 * SDL text clipboard); here only the image direction is added.
 *
 * IN: on Edit > Paste / File > New from Clipboard, if the OS clipboard holds an image another app put
 * there, it is decoded (codecs) and set as the session clipboard (pc_engine_set_clipboard), so an
 * external image pastes in. We fingerprint the PNG we last exported (length + FNV-1a) so we do not
 * re-import our own data (the internal Copy -> Paste path is unaffected). */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdlib.h>

#include "base/buf.h"
#include "base/status.h"
#include "codecs/codecs.h"
#include "codecs/format.h"
#include "codecs/image.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/prefs.h"
#include "geom/geom.h"
#include "raster/surface.h"
#include "shell_internal.h"

/* An upper bound so a hostile/huge clipboard image never provokes an absurd allocation (Rule 9).
 * 64 MP covers any realistic copy; past it the mirror is skipped (the paste inside PhotoCraft still
 * works from the session clipboard). */
#define PC_UI_CLIP_MAX_PIXELS (64u * 1024u * 1024u)

/* Owned PNG bytes handed to SDL; SDL frees them through the cleanup callback when the clipboard is
 * replaced or cleared. */
typedef struct pc_ui_clip_png {
    uint8_t *bytes;
    size_t len;
} pc_ui_clip_png;

/* FNV-1a over the bytes (64-bit), paired with the length as a cheap fingerprint of a PNG. */
static uint64_t clip_fnv1a(const uint8_t *data, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static const void *SDLCALL clip_provide(void *userdata, const char *mime_type, size_t *size)
{
    (void)mime_type;
    pc_ui_clip_png *p = userdata;
    if (p == NULL || p->bytes == NULL) {
        if (size != NULL) {
            *size = 0;
        }
        return NULL;
    }
    if (size != NULL) {
        *size = p->len;
    }
    return p->bytes;
}

static void SDLCALL clip_cleanup(void *userdata)
{
    pc_ui_clip_png *p = userdata;
    if (p != NULL) {
        free(p->bytes);
        free(p);
    }
}

void pc_ui_shell_os_clipboard_export(pc_ui_shell_app *app)
{
    if (app == NULL || !SDL_WasInit(SDL_INIT_VIDEO)) {
        return; /* no clipboard owner without the video subsystem (e.g. the headless control test) */
    }
    pc_engine_session *session = pc_ui_shell_app_session(app);
    if (session == NULL) {
        return;
    }
    bool want = true; /* Preferences > General > Export Clipboard (default on) */
    pc_json_value pref = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(session, "exportClipboard", &pref, NULL) == PC_OK) {
        want = yyjson_mut_get_bool(pc_json_value_root(&pref));
    }
    pc_json_value_fini(&pref);
    if (!want) {
        return;
    }
    const pc_surface *surface = pc_engine_clipboard_surface(session);
    if (surface == NULL) {
        return;
    }
    pc_rect bounds = pc_surface_content_bounds(surface);
    uint32_t w = pc_rect_width(bounds), h = pc_rect_height(bounds);
    if (w == 0 || h == 0 || (uint64_t)w * h > PC_UI_CLIP_MAX_PIXELS) {
        return;
    }
    size_t count = (size_t)w * (size_t)h;
    uint8_t(*px)[4] = malloc(count * sizeof *px);
    if (px == NULL) {
        return;
    }
    pc_cimage img;
    pc_cimage_init(&img);
    pc_buf encoded = PC_BUF_INIT;
    pc_ui_clip_png *held = NULL;
    if (pc_surface_read_rgba8_into(surface, bounds, px, count, NULL) != PC_OK) {
        goto done;
    }
    if (pc_cimage_from_copy(w, h, PC_CL_RGBA, PC_CS_U8, px, count * 4, &img, NULL) != PC_OK) {
        goto done;
    }
    if (pc_cencode(&img, PC_CF_PNG, NULL, &encoded, NULL) != PC_OK || encoded.len == 0) {
        goto done;
    }
    held = malloc(sizeof *held);
    if (held == NULL) {
        goto done;
    }
    held->bytes = pc_buf_take(&encoded, &held->len);
    if (held->bytes == NULL) {
        free(held);
        held = NULL;
        goto done;
    }
    static const char *const mimes[] = {"image/png"};
    if (!SDL_SetClipboardData(clip_provide, clip_cleanup, held, mimes, 1)) {
        /* SDL did not take ownership on failure: free our copy ourselves. */
        free(held->bytes);
        free(held);
    } else {
        /* Remember what we put there so a later Paste does not import our own image back in. */
        pc_ui_shell_os_clip_mark_exported(app, clip_fnv1a(held->bytes, held->len), held->len);
    }
    held = NULL; /* owned by SDL on success */
done:
    pc_buf_fini(&encoded);
    pc_cimage_fini(&img);
    free(px);
}

bool pc_ui_shell_os_clipboard_import(pc_ui_shell_app *app)
{
    if (app == NULL || !SDL_WasInit(SDL_INIT_VIDEO)) {
        return false;
    }
    pc_engine_session *session = pc_ui_shell_app_session(app);
    if (session == NULL) {
        return false;
    }
    if (!SDL_HasClipboardData("image/png")) {
        return false;
    }
    /* Do not clobber a fresher internal copy: import freely when the session clipboard is empty, but
     * when it already holds pixels only trust the OS clipboard if Export Clipboard is on (then every
     * internal Copy claims the OS clipboard, so a non-ours image there is genuinely newer/external).
     * With mirroring off we cannot tell which is newer, so we leave the internal copy in place. */
    bool session_empty = !pc_engine_session_clipboard_size(session, NULL);
    if (!session_empty) {
        bool mirroring = true; /* Preferences > General > Export Clipboard (default on) */
        pc_json_value pref = PC_JSON_VALUE_INIT;
        if (pc_engine_prefs_get(session, "exportClipboard", &pref, NULL) == PC_OK) {
            mirroring = yyjson_mut_get_bool(pc_json_value_root(&pref));
        }
        pc_json_value_fini(&pref);
        if (!mirroring) {
            return false;
        }
    }
    size_t n = 0;
    void *data = SDL_GetClipboardData("image/png", &n);
    if (data == NULL || n == 0) {
        SDL_free(data);
        return false;
    }
    /* Our own exported PNG? Then the session clipboard already holds those pixels: nothing to do. */
    if (pc_ui_shell_os_clip_is_exported(app, clip_fnv1a(data, n), n)) {
        SDL_free(data);
        return false;
    }
    pc_cimage img;
    pc_cimage_init(&img);
    uint8_t *rgba = NULL;
    size_t rgba_len = 0;
    bool imported = false;
    if (pc_cdecode((const uint8_t *)data, n, &img, NULL) != PC_OK) {
        goto done;
    }
    if (img.width == 0 || img.height == 0 ||
        (uint64_t)img.width * img.height > PC_UI_CLIP_MAX_PIXELS) {
        goto done;
    }
    if (pc_cimage_to_rgba8(&img, &rgba, &rgba_len, NULL) != PC_OK || rgba == NULL) {
        goto done;
    }
    if (pc_engine_set_clipboard(session, rgba, img.width, img.height, NULL) == PC_OK) {
        imported = true;
    }
done:
    free(rgba);
    pc_cimage_fini(&img);
    SDL_free(data);
    return imported;
}
