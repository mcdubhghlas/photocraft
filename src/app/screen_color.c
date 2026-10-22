/* See screen_color.h. Behavioural port of the pure parts of screen_color.rs (Linux), plus the live
 * X11 screen-pixel grab for the eyedropper (under PC_NO_X11SCREEN guards). */
#include "screen_color.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "cms/cms.h"
#include "cms/profile.h"

/* Largest ICC profile accepted from a screen/output (matches the reference 1 MiB cap). */
#define SCREEN_MAX_ICC_BYTES (1024u * 1024u)

pc_status pc_screen_pixel_budget(size_t w, size_t h, size_t *out_count, pc_error *err)
{
    if (!out_count) {
        return pc_fail(err, PC_ERR_INVALID, "pixel budget: missing output");
    }
    if (w == 0 || h == 0) {
        return pc_fail(err, PC_ERR_LIMIT, "screen capture has zero pixels");
    }
    if (w > PC_SCREEN_MAX_PIXELS / h) {
        return pc_fail(err, PC_ERR_LIMIT, "screen capture exceeds the 64 MP memory budget");
    }
    size_t n = w * h;
    if (n > PC_SCREEN_MAX_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "screen capture exceeds the 64 MP memory budget");
    }
    *out_count = n;
    return PC_OK;
}

bool pc_screen_find_monitor(const pc_screen_placement *placements, size_t n, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, size_t *out_index)
{
    if (!placements || !out_index) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (placements[i].x == x && placements[i].y == y && placements[i].width == w &&
            placements[i].height == h) {
            *out_index = placements[i].index;
            return true;
        }
    }
    return false;
}

uint8_t pc_screen_chan8(uint16_t v)
{
    return (uint8_t)(((unsigned)v + 128u) / 257u);
}

pc_status pc_screen_pack_rgba(const uint16_t *rgb16, size_t pixels, uint8_t *out_rgba, pc_error *err)
{
    if (!rgb16 || !out_rgba) {
        return pc_fail(err, PC_ERR_INVALID, "screen pack: missing buffer");
    }
    if (pixels > PC_SCREEN_MAX_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "screen capture exceeds the 64 MP memory budget");
    }
    for (size_t i = 0; i < pixels; i++) {
        out_rgba[i * 4 + 0] = pc_screen_chan8(rgb16[i * 3 + 0]);
        out_rgba[i * 4 + 1] = pc_screen_chan8(rgb16[i * 3 + 1]);
        out_rgba[i * 4 + 2] = pc_screen_chan8(rgb16[i * 3 + 2]);
        out_rgba[i * 4 + 3] = 255;
    }
    return PC_OK;
}

pc_status pc_screen_icc_validate(const uint8_t *bytes, size_t len, pc_error *err)
{
    if (!bytes || len == 0) {
        return pc_fail(err, PC_ERR_FORMAT, "screen ICC profile is empty");
    }
    if (len > SCREEN_MAX_ICC_BYTES) {
        return pc_fail(err, PC_ERR_LIMIT, "screen ICC profile exceeds the memory budget");
    }
    pc_profile profile;
    pc_status st = pc_profile_parse(bytes, len, &profile, err);
    if (st != PC_OK) {
        return st;
    }
    bool rgb = profile.space == PC_CS_RGB;
    pc_profile_fini(&profile);
    if (!rgb) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "screen ICC profile is not an RGB profile");
    }
    return PC_OK;
}

/* The Wayland eyedropper routes through the xdg-desktop-portal PickColor dialog, deferred for the
 * first pass (decision D19). Both builds report that the same way. */
#define SCREEN_WAYLAND_MSG                                                                          \
    "screen colour picking on Wayland uses the desktop portal, which is not available yet "         \
    "(deferred, REWRITE_PLAN decision D19)"

#ifndef PC_NO_X11SCREEN

#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcb_image.h>

/* At most 256 Ki 32-bit units (1 MiB) of an _ICC_PROFILE property, the pc_screen_icc_validate cap. */
#define SCREEN_ICC_LONG_LEN (256u * 1024u)

/* The screen at index `num` in the connection's setup, or NULL. */
static xcb_screen_t *screen_of(xcb_connection_t *conn, int num)
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

/* The visual type for id `vid` on `screen`, or NULL. */
static const xcb_visualtype_t *visual_of(const xcb_screen_t *screen, xcb_visualid_t vid)
{
    xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen);
    for (; di.rem; xcb_depth_next(&di)) {
        xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
        for (; vi.rem; xcb_visualtype_next(&vi)) {
            if (vi.data->visual_id == vid) {
                return vi.data;
            }
        }
    }
    return NULL;
}

/* Extract the channel selected by `mask` from `pixel` and scale it to a full-range 16-bit value,
 * matching x11rb's PixelLayout::decode (which the pure chan8/pack helpers then round to 8 bits). */
static uint16_t chan16_from_mask(uint32_t pixel, uint32_t mask)
{
    if (mask == 0u) {
        return 0u;
    }
    unsigned shift = 0u;
    while (shift < 32u && ((mask >> shift) & 1u) == 0u) {
        shift++;
    }
    uint32_t span = mask >> shift; /* e.g. 0xff for an 8-bit channel */
    if (span == 0u) {
        return 0u;
    }
    uint32_t field = (pixel & mask) >> shift;
    if (field > span) {
        field = span; /* defensive: never scale above full-scale */
    }
    uint64_t scaled = ((uint64_t)field * 65535u + span / 2u) / span;
    return (uint16_t)scaled;
}

/* Intern an atom only if it already exists; XCB_ATOM_NONE on absence or any error. */
static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name)
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

/* Append a window-property ICC profile (root _ICC_PROFILE) to *out when present and valid. Returns
 * true only when *out was filled. Never hard-fails. */
static bool read_window_icc(xcb_connection_t *conn, xcb_window_t win, xcb_atom_t atom, pc_buf *out)
{
    if (atom == XCB_ATOM_NONE || !out) {
        return false;
    }
    xcb_get_property_cookie_t c =
        xcb_get_property(conn, 0, win, atom, XCB_ATOM_ANY, 0, SCREEN_ICC_LONG_LEN);
    xcb_generic_error_t *e = NULL;
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, c, &e);
    free(e);
    if (!r) {
        return false;
    }
    bool ok = false;
    if (r->format == 8 && r->bytes_after == 0) {
        const uint8_t *data = xcb_get_property_value(r);
        int len = xcb_get_property_value_length(r);
        if (data && len > 0 && pc_screen_icc_validate(data, (size_t)len, NULL) == PC_OK &&
            pc_buf_append(out, data, (size_t)len, NULL) == PC_OK) {
            ok = true;
        }
    }
    free(r);
    return ok;
}

/* Append a RandR per-output ICC profile (_ICC_PROFILE CARDINAL) to *out when present and valid. */
static bool read_output_icc(xcb_connection_t *conn, xcb_randr_output_t output, xcb_atom_t atom,
                            pc_buf *out)
{
    if (atom == XCB_ATOM_NONE || !out) {
        return false;
    }
    xcb_randr_get_output_property_cookie_t c =
        xcb_randr_get_output_property(conn, output, atom, XCB_ATOM_CARDINAL, 0, SCREEN_ICC_LONG_LEN,
                                      0, 0);
    xcb_generic_error_t *e = NULL;
    xcb_randr_get_output_property_reply_t *r = xcb_randr_get_output_property_reply(conn, c, &e);
    free(e);
    if (!r) {
        return false;
    }
    bool ok = false;
    if (r->type == XCB_ATOM_CARDINAL && r->format == 8 && r->bytes_after == 0) {
        const uint8_t *data = xcb_randr_get_output_property_data(r);
        int len = xcb_randr_get_output_property_data_length(r);
        if (data && len > 0 && pc_screen_icc_validate(data, (size_t)len, NULL) == PC_OK &&
            pc_buf_append(out, data, (size_t)len, NULL) == PC_OK) {
            ok = true;
        }
    }
    free(r);
    return ok;
}

/* Map the point to its RandR monitor: *out_monitor to the matching placement index (when placements
 * are given), and *out_profile to that monitor's ICC profile (per-output, else whole-screen root).
 * Best-effort: any missing step just means no index / no profile, never a failure. */
static void resolve_monitor(xcb_connection_t *conn, xcb_screen_t *screen, int32_t x, int32_t y,
                            const pc_screen_placement *placements, size_t n_placements,
                            size_t *out_monitor, pc_buf *out_profile)
{
    if (!out_monitor && !out_profile) {
        return;
    }
    xcb_atom_t icc_atom = out_profile ? intern_atom(conn, "_ICC_PROFILE") : XCB_ATOM_NONE;
    bool got_profile = false;
    xcb_randr_get_monitors_cookie_t c = xcb_randr_get_monitors(conn, screen->root, 1);
    xcb_generic_error_t *e = NULL;
    xcb_randr_get_monitors_reply_t *r = xcb_randr_get_monitors_reply(conn, c, &e);
    free(e);
    if (r) {
        xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(r);
        for (; it.rem; xcb_randr_monitor_info_next(&it)) {
            const xcb_randr_monitor_info_t *m = it.data;
            int32_t mx = m->x;
            int32_t my = m->y;
            int32_t mw = m->width;
            int32_t mh = m->height;
            if (mw <= 0 || mh <= 0 || x < mx || y < my || x >= mx + mw || y >= my + mh) {
                continue; /* the point is not on this monitor */
            }
            if (out_monitor && placements) {
                size_t idx = 0;
                if (pc_screen_find_monitor(placements, n_placements, mx, my, (uint32_t)mw,
                                           (uint32_t)mh, &idx)) {
                    *out_monitor = idx;
                }
            }
            if (out_profile && icc_atom != XCB_ATOM_NONE) {
                const xcb_randr_output_t *outs = xcb_randr_monitor_info_outputs(m);
                int nouts = xcb_randr_monitor_info_outputs_length(m);
                for (int i = 0; outs && i < nouts; i++) {
                    if (read_output_icc(conn, outs[i], icc_atom, out_profile)) {
                        got_profile = true;
                        break;
                    }
                }
            }
            break; /* the containing monitor was found */
        }
        free(r);
    }
    if (out_profile && !got_profile) {
        (void)read_window_icc(conn, screen->root, intern_atom(conn, "_ICC_PROFILE"), out_profile);
    }
}

pc_status pc_screen_pick(bool wayland, pc_error *err)
{
    if (wayland) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, SCREEN_WAYLAND_MSG);
    }
    int screen_num = 0;
    xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
    if (!conn || xcb_connection_has_error(conn)) {
        if (conn) {
            xcb_disconnect(conn);
        }
        return pc_fail(err, PC_ERR_UNSUPPORTED,
                       "screen colour picking is unavailable: could not connect to an X11 display "
                       "(is DISPLAY set?)");
    }
    xcb_screen_t *screen = screen_of(conn, screen_num);
    const xcb_visualtype_t *visual = screen ? visual_of(screen, screen->root_visual) : NULL;
    if (!screen || !visual) {
        xcb_disconnect(conn);
        return pc_fail(err, PC_ERR_UNSUPPORTED,
                       "screen colour picking is unavailable: no usable X11 screen visual");
    }
    bool truecolor = visual->_class == XCB_VISUAL_CLASS_TRUE_COLOR;
    xcb_disconnect(conn);
    if (!truecolor) {
        return pc_fail(err, PC_ERR_UNSUPPORTED,
                       "screen colour picking requires a TrueColor X11 visual");
    }
    return PC_OK;
}

pc_status pc_screen_pick_at(bool wayland, int32_t x, int32_t y,
                            const pc_screen_placement *placements, size_t n_placements,
                            uint8_t out_rgba[4], size_t *out_monitor, pc_buf *out_profile,
                            pc_error *err)
{
    if (out_monitor) {
        *out_monitor = SIZE_MAX;
    }
    if (!out_rgba) {
        return pc_fail(err, PC_ERR_INVALID, "screen pick: missing output buffer");
    }
    if (wayland) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, SCREEN_WAYLAND_MSG);
    }
    size_t budget = 0;
    pc_status bst = pc_screen_pixel_budget(1, 1, &budget, err);
    if (bst != PC_OK) {
        return bst;
    }
    (void)budget;
    int screen_num = 0;
    xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
    if (!conn || xcb_connection_has_error(conn)) {
        if (conn) {
            xcb_disconnect(conn);
        }
        return pc_fail(err, PC_ERR_UNSUPPORTED,
                       "could not connect to an X11 display for screen colour picking "
                       "(is DISPLAY set?)");
    }
    xcb_screen_t *screen = screen_of(conn, screen_num);
    if (!screen) {
        xcb_disconnect(conn);
        return pc_fail(err, PC_ERR_UNSUPPORTED, "the X11 server provided no screen");
    }
    /* X11 addresses pixels with signed 16-bit coordinates; guard the range and the screen bounds. */
    if (x < 0 || y < 0 || x > INT16_MAX || y > INT16_MAX ||
        (uint32_t)x >= (uint32_t)screen->width_in_pixels ||
        (uint32_t)y >= (uint32_t)screen->height_in_pixels) {
        xcb_disconnect(conn);
        return pc_fail(err, PC_ERR_INVALID, "screen pick coordinate is off the X11 screen");
    }
    const xcb_visualtype_t *visual = visual_of(screen, screen->root_visual);
    if (!visual) {
        xcb_disconnect(conn);
        return pc_fail(err, PC_ERR_UNSUPPORTED, "the X11 root visual is unavailable");
    }
    if (visual->_class != XCB_VISUAL_CLASS_TRUE_COLOR) {
        xcb_disconnect(conn);
        return pc_fail(err, PC_ERR_UNSUPPORTED,
                       "screen colour picking requires a TrueColor X11 visual");
    }
    xcb_image_t *img = xcb_image_get(conn, screen->root, (int16_t)x, (int16_t)y, 1, 1, 0xffffffffu,
                                     XCB_IMAGE_FORMAT_Z_PIXMAP);
    if (!img) {
        xcb_disconnect(conn);
        return pc_fail(err, PC_ERR_IO, "could not read the screen pixel from X11");
    }
    uint32_t pixel = xcb_image_get_pixel(img, 0, 0);
    xcb_image_destroy(img);

    uint16_t rgb16[3];
    rgb16[0] = chan16_from_mask(pixel, visual->red_mask);
    rgb16[1] = chan16_from_mask(pixel, visual->green_mask);
    rgb16[2] = chan16_from_mask(pixel, visual->blue_mask);
    pc_status pst = pc_screen_pack_rgba(rgb16, 1, out_rgba, err);
    if (pst != PC_OK) {
        xcb_disconnect(conn);
        return pst;
    }
    resolve_monitor(conn, screen, x, y, placements, n_placements, out_monitor, out_profile);
    xcb_disconnect(conn);
    return PC_OK;
}

#else /* PC_NO_X11SCREEN */

pc_status pc_screen_pick(bool wayland, pc_error *err)
{
    if (wayland) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, SCREEN_WAYLAND_MSG);
    }
    return pc_fail(err, PC_ERR_UNSUPPORTED,
                   "screen colour picking on X11 needs libxcb/libX11 to read a screen pixel, "
                   "which this build does not link yet (REWRITE_PLAN P2)");
}

pc_status pc_screen_pick_at(bool wayland, int32_t x, int32_t y,
                            const pc_screen_placement *placements, size_t n_placements,
                            uint8_t out_rgba[4], size_t *out_monitor, pc_buf *out_profile,
                            pc_error *err)
{
    (void)x;
    (void)y;
    (void)placements;
    (void)n_placements;
    (void)out_profile;
    if (out_monitor) {
        *out_monitor = SIZE_MAX;
    }
    if (!out_rgba) {
        return pc_fail(err, PC_ERR_INVALID, "screen pick: missing output buffer");
    }
    if (wayland) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, SCREEN_WAYLAND_MSG);
    }
    return pc_fail(err, PC_ERR_UNSUPPORTED,
                   "screen colour picking on X11 needs libxcb/libX11 to read a screen pixel, "
                   "which this build does not link yet (REWRITE_PLAN P2)");
}

#endif /* PC_NO_X11SCREEN */
