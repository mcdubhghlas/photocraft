#include "snap.h"
#include "engine/prefs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
void pc_ui_snap_fini(pc_ui_snap *s)
{
    if (!s)
        return;
    pc_snap_targets_fini(&s->targets);
    pc_snap_targets_fini(&s->smart);
    free(s->lines);
    *s = (pc_ui_snap){0};
}
static double number(yyjson_mut_val *object, const char *key, double fallback)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(object, key);
    double n = yyjson_mut_get_num(v);
    return yyjson_mut_is_num(v) && isfinite(n) && n > 0 ? n : fallback;
}
static void smart_only(pc_snap_targets *t)
{
    for (unsigned axis = 0; axis < 2; axis++) {
        pc_snap_target *a = axis ? t->y : t->x;
        size_t *n = axis ? &t->ny : &t->nx, count = 0;
        for (size_t i = 0; i < *n; i++)
            if (pc_snap_kind_is_smart(a[i].kind))
                a[count++] = a[i];
        *n = count;
    }
}
pc_status pc_ui_snap_begin(pc_ui_snap *s, pc_engine_session *session, const pc_ui_state *ui,
                           const double start[2], const double *moving, const pc_layer_id *exclude,
                           size_t nexclude, bool smart, bool exclude_selection, bool exclude_guides,
                           pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!s || !ui || !state || !start || !isfinite(start[0]) || !isfinite(start[1]))
        return pc_fail(err, PC_ERR_INVALID, "invalid snap gesture");
    const pc_document *doc = pc_doc_arc_get(state->doc);
    pc_ui_snap next = {.active = true,
                       .moving = moving != NULL,
                       .document = doc->id,
                       .revision = state->revision,
                       .smart_enabled = smart};
    memcpy(next.start, start, sizeof next.start);
    if (moving) {
        for (size_t i = 0; i < 4; i++)
            if (!isfinite(moving[i]) || fabs(moving[i]) > 1e9)
                return pc_fail(err, PC_ERR_INVALID, "invalid snap bounds");
        memcpy(next.rect, moving, sizeof next.rect);
    }
    pc_snap_options opts = pc_snap_options_default();
    opts.guides = ui->view.snap_to[0] && ui->extras.guides && !exclude_guides;
    opts.grid = ui->view.snap_to[1] && ui->extras.grid;
    opts.layers = ui->view.snap_to[2];
    opts.document = ui->view.snap_to[4];
    opts.selection = !exclude_selection;
    if (opts.grid) {
        pc_json_value pref = PC_JSON_VALUE_INIT;
        (void)pc_engine_prefs_get(session, "guidesGridAndSlices", &pref, NULL);
        yyjson_mut_val *p = pc_json_value_root(&pref);
        pc_pref_unit unit = PC_UNIT_INCHES;
        const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(p, "gridUnit"));
        if (name)
            (void)pc_pref_unit_from_suffix(name, strlen(name), &unit);
        double every = number(p, "gridlineEvery", 1), subdivisions = number(p, "subdivisions", 1);
        pc_json_value points = PC_JSON_VALUE_INIT;
        (void)pc_engine_prefs_get(session, "unitsAndRulers.pointSize", &points, NULL);
        const char *point_name = yyjson_mut_get_str(pc_json_value_root(&points));
        double point_size = point_name && !strcmp(point_name, "traditional") ? 72.27 : 72;
        for (unsigned axis = 0; axis < 2; axis++)
            opts.grid_step[axis] = pc_pref_unit_to_px(unit, every, fmax(1, doc->resolution_dpi),
                                                      axis ? doc->size.height : doc->size.width, point_size) /
                                   fmin(100, subdivisions);
        pc_json_value_fini(&points);
        pc_json_value_fini(&pref);
    }
    pc_status status = PC_OK;
    if (ui->extras.snap)
        status = pc_snap_targets_from_document(doc, &opts, exclude, nexclude, &next.targets, err);
    if (status == PC_OK && smart) {
        opts = (pc_snap_options){.layers = true, .document = true};
        status = pc_snap_targets_from_document(doc, &opts, exclude, nexclude, &next.smart, err);
        smart_only(&next.smart);
    }
    if (status != PC_OK) {
        pc_ui_snap_fini(&next);
        return status;
    }
    pc_ui_snap_fini(s);
    *s = next;
    return PC_OK;
}
pc_status pc_ui_snap_point(pc_ui_snap *s, const double p[2], double tolerance, bool disabled, double out[2],
                           pc_error *err)
{
    if (!s || !p || !out || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(tolerance) || tolerance < 0)
        return pc_fail(err, PC_ERR_INVALID, "invalid snap point");
    free(s->lines);
    s->lines = NULL;
    s->nlines = 0;
    if (!s->active || disabled) {
        memcpy(out, p, 2 * sizeof *p);
        return PC_OK;
    }
    const pc_snap_targets *targets = &s->targets;
    if (s->moving && pc_snap_targets_is_empty(targets))
        targets = &s->smart;
    if (s->moving) {
        double moved[4];
        for (unsigned i = 0; i < 4; i++)
            moved[i] = s->rect[i] + p[i % 2] - s->start[i % 2];
        double adjust[2];
        pc_status status =
            pc_snap_targets_snap_rect(targets, moved, tolerance, adjust, &s->lines, &s->nlines, err);
        if (status != PC_OK)
            return status;
        out[0] = p[0] + adjust[0];
        out[1] = p[1] + adjust[1];
        return PC_OK;
    }
    return pc_snap_targets_snap_point(targets, p, tolerance, out, &s->lines, &s->nlines, err);
}
