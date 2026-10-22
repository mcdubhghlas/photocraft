#include "analysis.h"

#include "base/json.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const double PC_UI_ANALYSIS_PI = 3.14159265358979323846;

void pc_ui_analysis_init(pc_ui_analysis *a)
{
    if (!a)
        return;
    memset(a, 0, sizeof *a);
    /* The reference's default new-note colour (1, 1, 0.51). */
    a->note_color[0] = 1.0f;
    a->note_color[1] = 1.0f;
    a->note_color[2] = 0.51f;
}

void pc_ui_analysis_fini(pc_ui_analysis *a)
{
    if (!a)
        return;
    free(a->log_selected);
    a->log_selected = NULL;
    a->nlog_selected = a->cap_log_selected = 0;
}

bool pc_ui_analysis_tool(pc_ui_tool tool)
{
    return tool == PC_TOOL_RULER || tool == PC_TOOL_COUNT_TOOL || tool == PC_TOOL_NOTE;
}

/* ---- pure geometry / math ---- */

double pc_ui_analysis_dist(const double a[2], const double b[2])
{
    if (!a || !b)
        return INFINITY;
    return hypot(a[0] - b[0], a[1] - b[1]);
}

void pc_ui_analysis_constrain45(const double from[2], const double to[2], bool shift, double out[2])
{
    if (!out || !to)
        return;
    out[0] = to[0];
    out[1] = to[1];
    if (!shift || !from)
        return;
    double dx = to[0] - from[0], dy = to[1] - from[1];
    double quarter = PC_UI_ANALYSIS_PI / 4.0;
    double a = round(atan2(dy, dx) / quarter) * quarter;
    double l = hypot(dx, dy);
    out[0] = from[0] + l * cos(a);
    out[1] = from[1] + l * sin(a);
}

double pc_ui_analysis_tolerance(double zoom)
{
    double z = (isfinite(zoom) && zoom > 0.01) ? zoom : 0.01;
    return 7.0 / z;
}

double pc_ui_analysis_suggested_length(uint32_t width, const pc_measurement_scale *scale)
{
    double factor = scale ? pc_measurement_scale_factor(scale) : 1.0;
    if (!isfinite(factor) || factor <= 0.0)
        factor = 1.0;
    double target = (double)width / 5.0 * factor;
    if (!isfinite(target) || target <= 0.0)
        return 1.0;
    double e = pow(10.0, floor(log10(target)));
    if (!isfinite(e) || e <= 0.0)
        return 1.0;
    const double mult[3] = {5.0, 2.0, 1.0};
    for (size_t i = 0; i < 3; i++)
        if (mult[i] * e <= target)
            return mult[i] * e;
    return e;
}

static double round4(double v)
{
    if (!isfinite(v))
        return v;
    return round(v * 10000.0) / 10000.0;
}

bool pc_ui_analysis_ruler_readout(const pc_ruler *r, const pc_measurement_scale *scale, bool use_scale,
                                  pc_ui_ruler_readout *out)
{
    if (!r || !out)
        return false;
    double f = 1.0;
    if (use_scale && scale) {
        f = pc_measurement_scale_factor(scale);
        if (!isfinite(f) || f <= 0.0)
            f = 1.0;
    }
    double dx = r->end[0] - r->start[0], dy = r->end[1] - r->start[1];
    double l1 = hypot(dx, dy);
    out->x = r->start[0] * f;
    out->y = r->start[1] * f;
    out->w = dx * f;
    out->h = dy * f;
    out->l1 = round4(l1) * f;
    out->has_protractor = r->has_protractor;
    if (!r->has_protractor) {
        out->angle = round4(atan2(-dy, dx) * 180.0 / PC_UI_ANALYSIS_PI);
        out->l2 = NAN;
    } else {
        double ex = r->protractor[0] - r->start[0], ey = r->protractor[1] - r->start[1];
        double a1 = atan2(-dy, dx), a2 = atan2(-ey, ex);
        double ang = fabs((a1 - a2) * 180.0 / PC_UI_ANALYSIS_PI);
        if (ang > 180.0)
            ang = 360.0 - ang;
        out->angle = round4(ang);
        out->l2 = round4(hypot(ex, ey)) * f;
    }
    return true;
}

bool pc_ui_analysis_nearest_count(const pc_measurement *m, const double at[2], double tol, size_t *group,
                                  size_t *index)
{
    if (!m || !at || !group || !index)
        return false;
    bool found = false;
    double best = INFINITY;
    for (size_t gi = 0; gi < m->ncount_groups; gi++) {
        const pc_count_group *g = &m->count_groups[gi];
        if (!g->visible)
            continue;
        for (size_t pi = 0; pi < g->npoints; pi++) {
            double d = pc_ui_analysis_dist(g->points[pi], at);
            if (d <= tol && d < best) {
                best = d;
                *group = gi;
                *index = pi;
                found = true;
            }
        }
    }
    return found;
}

long pc_ui_analysis_nearest_note(const pc_note *notes, size_t nnotes, const double at[2], double zoom)
{
    if (!notes || !at)
        return -1;
    double z = (isfinite(zoom) && zoom > 0.01) ? zoom : 0.01;
    double w = 16.0 / z, h = 20.0 / z;
    /* Topmost first: the later note in the list draws on top, so scan from the end. */
    for (size_t k = nnotes; k > 0; k--) {
        size_t i = k - 1;
        const double *p = notes[i].position;
        if (at[0] >= p[0] && at[0] <= p[0] + w && at[1] >= p[1] && at[1] <= p[1] + h)
            return (long)i;
    }
    return -1;
}

/* ---- Measurement Log row selection ---- */

bool pc_ui_analysis_log_contains(const pc_ui_analysis *a, uint64_t id)
{
    if (!a)
        return false;
    for (size_t i = 0; i < a->nlog_selected; i++)
        if (a->log_selected[i] == id)
            return true;
    return false;
}

void pc_ui_analysis_log_clear(pc_ui_analysis *a)
{
    if (a)
        a->nlog_selected = 0;
}

static pc_status log_reserve(pc_ui_analysis *a, size_t need)
{
    if (need <= a->cap_log_selected)
        return PC_OK;
    size_t cap = a->cap_log_selected ? a->cap_log_selected : 8;
    while (cap < need) {
        if (cap > SIZE_MAX / 2)
            return PC_ERR_NOMEM;
        cap *= 2;
    }
    uint64_t *grown = realloc(a->log_selected, cap * sizeof *grown);
    if (!grown)
        return PC_ERR_NOMEM;
    a->log_selected = grown;
    a->cap_log_selected = cap;
    return PC_OK;
}

pc_status pc_ui_analysis_log_toggle(pc_ui_analysis *a, uint64_t id)
{
    if (!a)
        return PC_ERR_INVALID;
    for (size_t i = 0; i < a->nlog_selected; i++) {
        if (a->log_selected[i] == id) {
            a->log_selected[i] = a->log_selected[a->nlog_selected - 1];
            a->nlog_selected--;
            return PC_OK;
        }
    }
    pc_status rc = log_reserve(a, a->nlog_selected + 1);
    if (rc != PC_OK)
        return rc;
    a->log_selected[a->nlog_selected++] = id;
    return PC_OK;
}

pc_status pc_ui_analysis_log_set(pc_ui_analysis *a, const uint64_t *ids, size_t n)
{
    if (!a)
        return PC_ERR_INVALID;
    if (n && !ids)
        return PC_ERR_INVALID;
    pc_status rc = log_reserve(a, n);
    if (rc != PC_OK)
        return rc;
    for (size_t i = 0; i < n; i++)
        a->log_selected[i] = ids[i];
    a->nlog_selected = n;
    return PC_OK;
}

/* ---- pointer routing ---- */

static pc_status run(pc_engine_session *s, const char *id, const char *json, pc_json_value *out,
                     pc_error *err)
{
    pc_json_value scratch = PC_JSON_VALUE_INIT;
    pc_json_value *result = out ? out : &scratch;
    pc_status rc = pc_engine_session_execute_json(s, id, json, result, err);
    if (!out)
        pc_json_value_fini(&scratch);
    return rc;
}

/* Build the rulerTool params for a full line (start+end, and an optional protractor arm). */
static pc_status set_ruler(pc_engine_session *s, const pc_ruler *r, pc_error *err)
{
    char json[256];
    if (r->has_protractor)
        (void)snprintf(json, sizeof json,
                       "{\"start\":[%.9g,%.9g],\"end\":[%.9g,%.9g],\"protractor\":[%.9g,%.9g]}",
                       r->start[0], r->start[1], r->end[0], r->end[1], r->protractor[0], r->protractor[1]);
    else
        (void)snprintf(json, sizeof json,
                       "{\"start\":[%.9g,%.9g],\"end\":[%.9g,%.9g],\"protractor\":null}", r->start[0],
                       r->start[1], r->end[0], r->end[1]);
    return run(s, "image.analysis.rulerTool", json, NULL, err);
}

static pc_status ruler_down(pc_ui_analysis *a, pc_engine_session *s, const pc_measurement *m,
                            const double p[2], double tol, unsigned mods, pc_error *err)
{
    bool near_start = m->has_ruler && pc_ui_analysis_dist(p, m->ruler.start) <= tol;
    bool near_end = m->has_ruler && pc_ui_analysis_dist(p, m->ruler.end) <= tol;
    bool near_prot = m->has_ruler && m->ruler.has_protractor &&
                     pc_ui_analysis_dist(p, m->ruler.protractor) <= tol;
    bool alt = (mods & PC_UI_MOD_ALT) != 0;
    if (m->has_ruler && alt && (near_start || near_end)) {
        pc_ruler r = m->ruler;
        if (near_end && !near_start) {
            double t0 = r.start[0], t1 = r.start[1];
            r.start[0] = r.end[0];
            r.start[1] = r.end[1];
            r.end[0] = t0;
            r.end[1] = t1;
        }
        r.has_protractor = true;
        r.protractor[0] = p[0];
        r.protractor[1] = p[1];
        a->drag = PC_UI_ANALYSIS_DRAG_RULER_PROT;
        return set_ruler(s, &r, err);
    }
    if (near_prot) {
        a->drag = PC_UI_ANALYSIS_DRAG_RULER_PROT;
        return PC_OK;
    }
    if (near_end) {
        a->drag = PC_UI_ANALYSIS_DRAG_RULER_END;
        return PC_OK;
    }
    if (near_start) {
        a->drag = PC_UI_ANALYSIS_DRAG_RULER_START;
        return PC_OK;
    }
    pc_ruler r = {.start = {p[0], p[1]}, .end = {p[0], p[1]}, .has_protractor = false};
    a->drag = PC_UI_ANALYSIS_DRAG_RULER_NEW;
    return set_ruler(s, &r, err);
}

static pc_status ruler_move(pc_ui_analysis *a, pc_engine_session *s, const pc_measurement *m,
                            const double p[2], unsigned mods, pc_error *err)
{
    if (a->drag == PC_UI_ANALYSIS_DRAG_NONE || !m->has_ruler)
        return PC_OK;
    pc_ruler r = m->ruler;
    bool shift = (mods & PC_UI_MOD_SHIFT) != 0;
    switch (a->drag) {
    case PC_UI_ANALYSIS_DRAG_RULER_NEW:
    case PC_UI_ANALYSIS_DRAG_RULER_END:
        pc_ui_analysis_constrain45(r.start, p, shift, r.end);
        break;
    case PC_UI_ANALYSIS_DRAG_RULER_START:
        pc_ui_analysis_constrain45(r.end, p, shift, r.start);
        break;
    case PC_UI_ANALYSIS_DRAG_RULER_PROT: {
        double out[2];
        pc_ui_analysis_constrain45(r.start, p, shift, out);
        r.has_protractor = true;
        r.protractor[0] = out[0];
        r.protractor[1] = out[1];
        break;
    }
    default:
        return PC_OK;
    }
    return set_ruler(s, &r, err);
}

pc_status pc_ui_analysis_pointer(pc_ui_analysis *a, pc_engine_session *session, const pc_ui_state *ui,
                                 pc_ui_pointer_kind kind, pc_ui_tool_point point, unsigned modifiers,
                                 double zoom, pc_json_value *out, pc_error *err)
{
    if (!a || !ui || !pc_ui_analysis_tool(ui->tool))
        return pc_fail(err, PC_ERR_NOT_FOUND, "not an analysis tool");
    if (kind == PC_UI_POINTER_CANCEL) {
        a->drag = PC_UI_ANALYSIS_DRAG_NONE;
        return PC_OK;
    }
    const pc_engine_doc_state *state = session ? pc_engine_session_active(session) : NULL;
    if (!state) {
        a->drag = PC_UI_ANALYSIS_DRAG_NONE;
        return PC_OK; /* no document: the tools are inert but still owned */
    }
    if (!isfinite(point.x) || !isfinite(point.y)) {
        a->drag = PC_UI_ANALYSIS_DRAG_NONE;
        return pc_fail(err, PC_ERR_INVALID, "pointer must be finite");
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (!doc)
        return PC_OK;
    double p[2] = {point.x, point.y};
    double tol = pc_ui_analysis_tolerance(zoom);
    (void)out;

    if (ui->tool == PC_TOOL_RULER) {
        const pc_measurement *m = &doc->measurement;
        if (kind == PC_UI_POINTER_DOWN)
            return ruler_down(a, session, m, p, tol, modifiers, err);
        if (kind == PC_UI_POINTER_MOVE)
            return ruler_move(a, session, m, p, modifiers, err);
        /* Up: a mere click (no drag) that spawned a new line clears it. */
        pc_ui_analysis_drag was = a->drag;
        a->drag = PC_UI_ANALYSIS_DRAG_NONE;
        if (was == PC_UI_ANALYSIS_DRAG_RULER_NEW && m->has_ruler &&
            pc_ui_analysis_dist(m->ruler.start, m->ruler.end) < 0.5)
            return run(session, "image.analysis.rulerTool", "{\"clear\":true}", NULL, err);
        return PC_OK;
    }

    if (ui->tool == PC_TOOL_COUNT_TOOL) {
        const pc_measurement *m = &doc->measurement;
        bool alt = (modifiers & PC_UI_MOD_ALT) != 0;
        if (kind == PC_UI_POINTER_DOWN) {
            size_t g = 0, i = 0;
            bool hit = pc_ui_analysis_nearest_count(m, p, tol, &g, &i);
            char json[128];
            if (hit && alt) {
                (void)snprintf(json, sizeof json, "{\"group\":%zu,\"index\":%zu}", g, i);
                return run(session, "count.remove", json, NULL, err);
            }
            if (hit) {
                a->drag = PC_UI_ANALYSIS_DRAG_COUNT;
                a->drag_group = g;
                a->drag_index = i;
                a->drag_from[0] = p[0];
                a->drag_from[1] = p[1];
                return PC_OK;
            }
            if (!alt) {
                (void)snprintf(json, sizeof json, "{\"x\":%.9g,\"y\":%.9g}", p[0], p[1]);
                return run(session, "count.add", json, NULL, err);
            }
            return PC_OK;
        }
        if (kind == PC_UI_POINTER_UP) {
            if (a->drag == PC_UI_ANALYSIS_DRAG_COUNT && pc_ui_analysis_dist(a->drag_from, p) > 0.5) {
                a->drag = PC_UI_ANALYSIS_DRAG_NONE;
                char json[160];
                (void)snprintf(json, sizeof json, "{\"group\":%zu,\"index\":%zu,\"to\":[%.9g,%.9g]}",
                               a->drag_group, a->drag_index, p[0], p[1]);
                return run(session, "count.move", json, NULL, err);
            }
            a->drag = PC_UI_ANALYSIS_DRAG_NONE;
        }
        return PC_OK;
    }

    /* Note tool. */
    if (kind == PC_UI_POINTER_DOWN) {
        long hit = pc_ui_analysis_nearest_note(doc->notes, doc->nnotes, p, zoom);
        if (hit >= 0) {
            size_t i = (size_t)hit;
            a->has_note_selected = true;
            a->note_selected = i;
            a->notes_panel = true;
            a->drag = PC_UI_ANALYSIS_DRAG_NOTE;
            a->drag_index = i;
            a->drag_from[0] = p[0] - doc->notes[i].position[0];
            a->drag_from[1] = p[1] - doc->notes[i].position[1];
            return PC_OK;
        }
        char json[512];
        char author[2 * sizeof a->note_author];
        /* Escape the author for JSON (quotes and backslashes); it is a UI-entered string. */
        size_t j = 0;
        for (size_t k = 0; a->note_author[k] && j + 2 < sizeof author; k++) {
            char c = a->note_author[k];
            if (c == '"' || c == '\\')
                author[j++] = '\\';
            if ((unsigned char)c >= 0x20)
                author[j++] = c;
        }
        author[j] = '\0';
        (void)snprintf(json, sizeof json,
                       "{\"x\":%.0f,\"y\":%.0f,\"author\":\"%s\",\"color\":[%.6g,%.6g,%.6g]}",
                       round(p[0]), round(p[1]), author, (double)a->note_color[0], (double)a->note_color[1],
                       (double)a->note_color[2]);
        pc_json_value res = PC_JSON_VALUE_INIT;
        pc_status rc = run(session, "notes.add", json, &res, err);
        if (rc == PC_OK) {
            yyjson_mut_val *root = pc_json_value_root(&res);
            yyjson_mut_val *iv = root ? yyjson_mut_obj_get(root, "index") : NULL;
            if (iv && yyjson_mut_is_uint(iv)) {
                a->has_note_selected = true;
                a->note_selected = (size_t)yyjson_mut_get_uint(iv);
            }
            a->notes_panel = true;
        }
        pc_json_value_fini(&res);
        return rc;
    }
    if (kind == PC_UI_POINTER_UP) {
        if (a->drag == PC_UI_ANALYSIS_DRAG_NOTE) {
            size_t i = a->drag_index;
            a->drag = PC_UI_ANALYSIS_DRAG_NONE;
            double to[2] = {round(p[0] - a->drag_from[0]), round(p[1] - a->drag_from[1])};
            if (i < doc->nnotes && pc_ui_analysis_dist(doc->notes[i].position, to) >= 1.0) {
                char json[128];
                (void)snprintf(json, sizeof json, "{\"index\":%zu,\"x\":%.0f,\"y\":%.0f}", i, to[0], to[1]);
                return run(session, "notes.set", json, NULL, err);
            }
        }
    }
    return PC_OK;
}
