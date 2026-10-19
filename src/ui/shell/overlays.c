/* Ruler guides/grid and cached selection ants. All geometry is clipped before drawing;
 * tracing is keyed by document revision and display stride, never repeated on an idle frame. */
#include "overlays.h"
#include "snap_ui.h"
#include "ui/lasso_tool.h"
#include "dialogs.h"
#include <float.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "base/mem.h"
#include "base/num.h"
#include "engine/prefs.h"
#include "ui/widgets.h"

static bool clip_line(const pc_ui_canvas_xform *xf, float a[2], float b[2])
{
    double t0 = 0, t1 = 1, dx = (double)b[0] - a[0], dy = (double)b[1] - a[1];
    double p[] = {-dx, dx, -dy, dy},
           q[] = {a[0] - xf->x, xf->x + xf->width - a[0], a[1] - xf->y, xf->y + xf->height - a[1]};
    for (size_t i = 0; i < 4; i++) {
        if (!isfinite(p[i]) || !isfinite(q[i])) {
            return false;
        }
        if (p[i] == 0) {
            if (q[i] < 0) {
                return false;
            }
        } else {
            double t = q[i] / p[i];
            if (p[i] < 0) {
                t0 = fmax(t0, t);
            } else {
                t1 = fmin(t1, t);
            }
        }
    }
    if (t0 > t1) {
        return false;
    }
    b[0] = (float)(a[0] + dx * t1);
    b[1] = (float)(a[1] + dy * t1);
    a[0] = (float)(a[0] + dx * t0);
    a[1] = (float)(a[1] + dy * t0);
    return true;
}
static void line(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, float x0, float y0, float x1, float y1,
                 struct nk_color color, unsigned style, double phase, size_t *budget)
{
    float a[2], b[2];
    float original[2];
    if (pc_ui_canvas_to_screen(xf, x0, y0, original) != PC_OK) {
        return;
    }
    if (*budget == 0 || pc_ui_canvas_to_screen(xf, x0, y0, a) != PC_OK ||
        pc_ui_canvas_to_screen(xf, x1, y1, b) != PC_OK || !clip_line(xf, a, b)) {
        return;
    }
    struct nk_command_buffer *c = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    if (!style) {
        nk_stroke_line(c, a[0], a[1], b[0], b[1], 1, color);
        --*budget;
        return;
    }
    float dx = b[0] - a[0], dy = b[1] - a[1], len = hypotf(dx, dy);
    if (len < .01f || !isfinite(len)) {
        return;
    }
    float period = style == 2 ? 7.f : style == 3 ? 3.f : 8.f;
    float offset = (float)fmod(phase + hypotf(a[0] - original[0], a[1] - original[1]), period);
    for (float at = -offset; at < len && *budget; at += period) {
        if (style == 3) {
            if (at >= 0) {
                float t = at / len;
                nk_fill_circle(c, nk_rect(a[0] + dx * t - .6f, a[1] + dy * t - .6f, 1.2f, 1.2f), color);
                --*budget;
            }
            continue;
        }
        float begin = fmaxf(0, at) / len, end = fminf(len, at + 4.f) / len;
        if (end > begin) {
            nk_stroke_line(c, a[0] + dx * begin, a[1] + dy * begin, a[0] + dx * end, a[1] + dy * end, 1,
                           color);
            --*budget;
        }
    }
}
static struct nk_color pref_color(yyjson_mut_val *p, const char *key, struct nk_color fallback)
{
    const char *s = yyjson_mut_get_str(yyjson_mut_obj_get(p, key));
    if (!s || strlen(s) != 7 || s[0] != '#') {
        return fallback;
    }
    unsigned rgb[3] = {0};
    for (size_t i = 0; i < 6; i++) {
        char ch = s[i + 1];
        int n = ch >= '0' && ch <= '9'   ? ch - '0'
                : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                         : -1;
        if (n < 0) {
            return fallback;
        }
        rgb[i / 2] = rgb[i / 2] * 16 + (unsigned)n;
    }
    return nk_rgb((int)rgb[0], (int)rgb[1], (int)rgb[2]);
}
static unsigned dotted(yyjson_mut_val *p, const char *key)
{
    const char *s = yyjson_mut_get_str(yyjson_mut_obj_get(p, key));
    return s && strcmp(s, "dashedLines") == 0 ? 2u : s && strcmp(s, "dots") == 0 ? 3u : 0u;
}
static double point_size(pc_ui_shell_app *app)
{
    pc_json_value pref = PC_JSON_VALUE_INIT;
    double ppi = 72;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "unitsAndRulers.pointSize", &pref, NULL) == PC_OK) {
        const char *name = yyjson_mut_get_str(pc_json_value_root(&pref));
        if (name && strcmp(name, "traditional") == 0) {
            ppi = 72.27;
        }
    }
    pc_json_value_fini(&pref);
    return ppi;
}
static void visible_bounds(const pc_ui_canvas_xform *xf, float lo[2], float hi[2])
{
    lo[0] = lo[1] = FLT_MAX;
    hi[0] = hi[1] = -FLT_MAX;
    for (unsigned i = 0; i < 4; i++) {
        float p[2];
        if (pc_ui_canvas_to_doc(xf, xf->x + ((i & 1) ? xf->width : 0), xf->y + ((i & 2) ? xf->height : 0),
                                p) != PC_OK) {
            continue;
        }
        for (size_t j = 0; j < 2; j++) {
            lo[j] = fminf(lo[j], p[j]);
            hi[j] = fmaxf(hi[j], p[j]);
        }
    }
}
pc_status pc_ui_overlays_draw(pc_ui_shell_app *app, const pc_engine_doc_state *st,
                              const pc_ui_canvas_xform *xf, pc_ui_overlay_cache *cache,
                              const pc_ui_guide_drag *drag, pc_error *err)
{
    if (!st) {
        return PC_OK;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    uint64_t generation = 0, revision = st->revision;
    pc_doc_arc *preview = pc_ui_shell_dialog_preview(app, &generation);
    if (preview) {
        doc = pc_doc_arc_get(preview);
        revision = UINT64_MAX / 2 - generation;
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!ui->view.extras) {
        return PC_OK;
    }
    struct nk_command_buffer *c = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    struct nk_rect saved = c->clip;
    nk_push_scissor(c, nk_rect(xf->x, xf->y, xf->width, xf->height));
    pc_json_value pref = PC_JSON_VALUE_INIT;
    (void)pc_engine_prefs_get(pc_ui_shell_app_session(app), "guidesGridAndSlices", &pref, NULL);
    yyjson_mut_val *p = pc_json_value_root(&pref);
    float lo[2], hi[2];
    visible_bounds(xf, lo, hi);
    size_t budget = 100000;
    if (ui->view.show[0] && st->has_active_layer) {
        const pc_layer *layer = pc_document_layer(doc, st->active_layer);
        const pc_surface *surface = layer ? pc_layer_surface(layer) : NULL;
        pc_rect bounds = surface ? pc_surface_content_bounds(surface) : PC_RECT_EMPTY;
        if (!pc_rect_is_empty(bounds)) {
            struct nk_color edge = nk_rgb(0x2d, 0x8c, 0xeb);
            const float corners[4][2] = {{(float)bounds.x0, (float)bounds.y0},
                                         {(float)bounds.x1, (float)bounds.y0},
                                         {(float)bounds.x1, (float)bounds.y1},
                                         {(float)bounds.x0, (float)bounds.y1}};
            for (size_t i = 0; i < 4; i++) {
                size_t next = (i + 1) % 4;
                line(app, xf, corners[i][0], corners[i][1], corners[next][0], corners[next][1], edge, false,
                     0, &budget);
            }
        }
    }
    if (ui->extras.grid) {
        const char *unit_name = yyjson_mut_get_str(yyjson_mut_obj_get(p, "gridUnit"));
        pc_pref_unit unit = PC_UNIT_INCHES;
        if (unit_name) {
            (void)pc_pref_unit_from_suffix(unit_name, strlen(unit_name), &unit);
        }
        double every = yyjson_mut_get_num(yyjson_mut_obj_get(p, "gridlineEvery"));
        double subdivisions = yyjson_mut_get_num(yyjson_mut_obj_get(p, "subdivisions"));
        if (!isfinite(every) || every <= 0) {
            every = 1;
        }
        if (!isfinite(subdivisions) || subdivisions < 1 || subdivisions > 100) {
            subdivisions = 1;
        }
        struct nk_color color = pref_color(p, "gridColor", pc_ui_nk_color(t->text_faint));
        unsigned dash = dotted(p, "gridStyle");
        for (unsigned pass = 0; pass < 2; pass++) {
            struct nk_color ink = color;
            if (!pass) {
                ink.a = (nk_byte)((float)ink.a * .45f);
            }
            if (!pass && subdivisions <= 1) {
                continue;
            }
            for (unsigned axis = 0; axis < 2; axis++) {
                double extent = axis ? doc->size.height : doc->size.width;
                double step =
                    pc_pref_unit_to_px(unit, every, fmax(1, doc->resolution_dpi), extent, point_size(app));
                if (!pass) {
                    step /= subdivisions;
                }
                double zoom = xf->zoom * (axis ? 1 : xf->aspect);
                if (!isfinite(step) || step <= 0 || step * zoom < 6) {
                    continue;
                }
                double at = fmax(1, ceil(fmax(0, lo[axis]) / step)) * step;
                double end = fmin(extent, hi[axis]);
                for (size_t n = 0; n < 16384 && budget && at < end; n++, at += step) {
                    float v = (float)at;
                    line(app, xf, axis ? 0 : v, axis ? v : 0, axis ? (float)doc->size.width : v,
                         axis ? v : (float)doc->size.height, ink, dash, 0, &budget);
                }
            }
        }
    }
    if (ui->extras.guides && ui->view.show[11]) {
        struct nk_color base = pref_color(p, "guideColor", pc_ui_nk_color(t->accent));
        for (unsigned axis = 0; axis < 2; axis++) {
            bool vertical = axis == 0;
            size_t count = vertical ? doc->guides.nvertical : doc->guides.nhorizontal;
            const double *values = vertical ? doc->guides.vertical : doc->guides.horizontal;
            for (size_t i = 0; i < count && budget; i++) {
                if (drag->active && drag->existing && drag->vertical == vertical && drag->index == i) {
                    continue;
                }
                if (!isfinite(values[i]) || fabs(values[i]) > FLT_MAX) {
                    continue;
                }
                uint8_t rgb[3];
                struct nk_color ink = base;
                if (pc_guides_color(&doc->guides, vertical, values[i], rgb)) {
                    ink = nk_rgb(rgb[0], rgb[1], rgb[2]);
                }
                float v = (float)values[i];
                line(app, xf, vertical ? v : lo[0], vertical ? lo[1] : v, vertical ? v : hi[0],
                     vertical ? hi[1] : v, ink, dotted(p, "guideStyle"), 0, &budget);
            }
        }
    }
    if (drag->active) {
        struct nk_color ink = pref_color(p, "guideColor", pc_ui_nk_color(t->accent));
        size_t count = drag->vertical ? doc->guides.nvertical : doc->guides.nhorizontal;
        const double *values = drag->vertical ? doc->guides.vertical : doc->guides.horizontal;
        uint8_t rgb[3];
        if (drag->existing && drag->index < count &&
            pc_guides_color(&doc->guides, drag->vertical, values[drag->index], rgb)) {
            ink = nk_rgb(rgb[0], rgb[1], rgb[2]);
        }
        line(app, xf, drag->vertical ? drag->position : lo[0], drag->vertical ? lo[1] : drag->position,
             drag->vertical ? drag->position : hi[0], drag->vertical ? hi[1] : drag->position, ink, 0, 0,
             &budget);
    }
    pc_json_value_fini(&pref);
    pc_status rc = PC_OK;
    if (doc->has_selection && ui->view.show[1] && !pc_ui_lasso_replaces_selection(ui)) {
        const pc_ui_view *camera = NULL;
        size_t index;
        if (pc_engine_session_active_index(pc_ui_shell_app_session(app), &index) && index < ui->n_views) {
            camera = &ui->views[index];
        }
        float zoom = camera ? camera->zoom : xf->zoom;
        uint32_t step = (uint32_t)fminf(64, fmaxf(1, exp2f(floorf(log2f(1.f / fmaxf(.001f, zoom))))));
        double quantum = 256. * step;
        int32_t left =
            pc_f64_to_i32(floor(fmax(-1073741824., fmin(1073741824., lo[0])) / quantum) * quantum - quantum);
        int32_t top =
            pc_f64_to_i32(floor(fmax(-1073741824., fmin(1073741824., lo[1])) / quantum) * quantum - quantum);
        int32_t right =
            pc_f64_to_i32((floor(fmax(-1073741824., fmin(1073741824., hi[0])) / quantum) + 2) * quantum);
        int32_t bottom =
            pc_f64_to_i32((floor(fmax(-1073741824., fmin(1073741824., hi[1])) / quantum) + 2) * quantum);
        pc_rect region = pc_rect_make(left, top, right, bottom);
        if (!cache->valid || cache->document != doc->id || cache->revision != revision ||
            cache->step != step || !pc_rect_eq(cache->region, region)) {
            pc_rect bounds = pc_rect_intersect(pc_surface_content_bounds(&doc->selection), region);
            rc = pc_ui_outline_trace(&doc->selection, bounds, step, &cache->selection, err);
            if (rc == PC_OK) {
                cache->valid = true;
                cache->document = doc->id;
                cache->revision = revision;
                cache->step = step;
                cache->region = region;
            }
        }
        if (rc == PC_OK) {
            double phase = fmod((double)SDL_GetTicks() / 100., 8.);
            /* The outline is semantic black/white, independent of the interface theme. */
            for (size_t i = 0; i < cache->selection.count && budget; i++) {
                const pc_ui_segment *s = &cache->selection.segments[i];
                line(app, xf, (float)s->a[0], (float)s->a[1], (float)s->b[0], (float)s->b[1],
                     nk_rgb(255, 255, 255), false, 0, &budget);
                line(app, xf, (float)s->a[0], (float)s->a[1], (float)s->b[0], (float)s->b[1], nk_rgb(0, 0, 0),
                     true, phase, &budget);
            }
        }
    }
    nk_push_scissor(c, saved);
    return rc;
}

bool pc_ui_guide_event(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, pc_ui_guide_drag *drag,
                       const SDL_Event *event)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_doc_state *st = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!st) {
        drag->active = false;
        return false;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    if (drag->active &&
        (drag->document != doc->id || drag->revision != st->revision || ui->extras.lock_guides)) {
        drag->active = false;
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST ||
        (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE)) {
        bool consumed = drag->active;
        drag->active = false;
        return consumed;
    }
    bool down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT;
    bool up = event->type == SDL_EVENT_MOUSE_BUTTON_UP && event->button.button == SDL_BUTTON_LEFT;
    bool motion = event->type == SDL_EVENT_MOUSE_MOTION;
    if (!down && !up && !motion) {
        return false;
    }
    float factor = SDL_GetWindowPixelDensity(pc_ui_shell_app_sdl_window(app)) / pc_ui_shell_app_ui_scale(app);
    float x = (motion ? event->motion.x : event->button.x) * factor;
    float y = (motion ? event->motion.y : event->button.y) * factor;
    float point[2];
    if (pc_ui_canvas_to_doc(xf, x, y, point) != PC_OK) {
        if (up) {
            drag->active = false;
        }
        return false;
    }
    if (down && !ui->extras.lock_guides) {
        bool top = ui->extras.rulers && x >= xf->x && x < xf->x + xf->width && y >= xf->y - 16 && y < xf->y;
        bool left = ui->extras.rulers && y >= xf->y && y < xf->y + xf->height && x >= xf->x - 16 && x < xf->x;
        if (top || left) {
            *drag = (pc_ui_guide_drag){
                .active = true, .vertical = left, .document = doc->id, .revision = st->revision};
        } else if (ui->extras.guides && ui->view.extras && ui->view.show[11] && x >= xf->x &&
                   x < xf->x + xf->width && y >= xf->y && y < xf->y + xf->height &&
                   (ui->tool == PC_TOOL_MOVE || (SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_GUI)))) {
            for (unsigned axis = 0; axis < 2 && !drag->active; axis++) {
                size_t count = axis ? doc->guides.nhorizontal : doc->guides.nvertical;
                const double *values = axis ? doc->guides.horizontal : doc->guides.vertical;
                float zoom = xf->zoom * (axis ? 1 : xf->aspect);
                for (size_t i = 0; i < count; i++) {
                    if (fabs(values[i] - point[axis]) * (double)zoom <= 4) {
                        *drag = (pc_ui_guide_drag){.active = true,
                                                   .vertical = axis == 0,
                                                   .existing = true,
                                                   .index = i,
                                                   .document = doc->id,
                                                   .revision = st->revision};
                        break;
                    }
                }
            }
        }
    }
    if (!drag->active) {
        return false;
    }
    if (motion || down || up) {
        drag->position = point[drag->vertical ? 0 : 1];
    }
    if (SDL_GetModState() & SDL_KMOD_SHIFT) {
        pc_json_value units = PC_JSON_VALUE_INIT;
        pc_pref_unit unit = PC_UNIT_PIXELS;
        if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "unitsAndRulers.rulers", &units, NULL) ==
            PC_OK) {
            const char *name = yyjson_mut_get_str(pc_json_value_root(&units));
            if (name) {
                (void)pc_pref_unit_from_suffix(name, strlen(name), &unit);
            }
        }
        pc_json_value_fini(&units);
        double pixels =
            pc_pref_unit_to_px(unit, 1, fmax(1, doc->resolution_dpi),
                               drag->vertical ? doc->size.width : doc->size.height, point_size(app));
        double tick = pc_ui_ruler_nice_step(60. / (xf->zoom * fmax(1e-9, pixels)), unit == PC_UNIT_PIXELS) *
                      pixels / 10.;
        if (isfinite(tick) && tick > 0) {
            drag->position = (float)(round(drag->position / tick) * tick);
        }
    }
    drag->position = (float)pc_ui_shell_snap_guide(app, drag->vertical, drag->position);
    if (up) {
        pc_ui_guide_drag done = *drag;
        drag->active = false;
        double extent = done.vertical ? doc->size.width : doc->size.height;
        bool inside = done.position >= 0 && done.position <= extent;
        if (!done.existing && !inside) {
            return true;
        }
        yyjson_mut_doc *mut = yyjson_mut_doc_new(NULL);
        if (!mut) {
            return true;
        }
        yyjson_mut_val *p = yyjson_mut_obj(mut);
        bool ok = p &&
                  yyjson_mut_obj_add_str(mut, p, "orientation", done.vertical ? "vertical" : "horizontal") &&
                  yyjson_mut_obj_add_uint(mut, p, "index", done.index) &&
                  yyjson_mut_obj_add_real(mut, p, "position", round((double)done.position * 1000) / 1000);
        pc_json_value result = PC_JSON_VALUE_INIT;
        pc_error err;
        yyjson_doc *immutable = ok ? yyjson_mut_val_imut_copy(p, NULL) : NULL;
        if (immutable) {
            pc_status rc = pc_ui_shell_app_invoke(app,
                                                  !done.existing ? "view.newGuide"
                                                  : inside       ? "view.moveGuide"
                                                                 : "view.deleteGuide",
                                                  yyjson_doc_get_root(immutable), &result, &err);
            if (rc == PC_OK && !done.existing) {
                ui->extras.guides = true;
                ui->view.extras = true;
            }
            if (rc != PC_OK) {
                free(ui->status);
                ui->status = pc_strndup(err.msg, strlen(err.msg));
                ui->status_error = true;
            }
        }
        yyjson_doc_free(immutable);
        pc_json_value_fini(&result);
        yyjson_mut_doc_free(mut);
    }
    return true;
}
