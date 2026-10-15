/* Window > Gradients panel commands (port of crates/engine/src/presets/gradients.rs commands plus
 * gradient.presets.importGrd from preset_import_cmds.rs, E2b).
 *
 * gradient.presets.apply (New Gradient Fill Layer) is deferred to E10 (gradient_fill_cmds), which
 * owns Gradient Fill layer creation. select's recolour of a selected Gradient Fill layer is
 * deferred with it. Never crashes on corrupt .grd files. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "engine_internal.h"
#include "presets.h"
#include "psd/grd.h"

static const pc_preset_item_ops *OPS;
static void init_ops(void)
{
    OPS = pc_gradient_preset_ops();
}

static char *dup_str(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

/* A trimmed non-empty string param copied into out[cap]; false when absent/empty. */
static bool str_copy(yyjson_val *p, const char *key, char *out, size_t cap)
{
    size_t len = 0;
    const char *s = pc_json_as_str(pc_json_get(p, key), &len);
    if (s == NULL) {
        return false;
    }
    size_t a = 0, b = len;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) {
        a++;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) {
        b--;
    }
    if (b == a) {
        return false;
    }
    size_t n = b - a;
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, s + a, n);
    out[n] = '\0';
    return true;
}

/* Resolve the preset a command names: "preset" (clone from the groups), explicit "stops", or the
 * current gradient. Writes *out (finalise with pc_gradient_preset_fini) on success. */
static pc_status preset_param(pc_engine_session *s, yyjson_val *p, const char *cmd,
                              pc_gradient_preset *out, pc_error *err)
{
    char name[512], group[512];
    if (str_copy(p, "preset", name, sizeof name)) {
        size_t gi, ii;
        const char *grp = str_copy(p, "group", group, sizeof group) ? group : NULL;
        if (!pc_preset_find(&s->presets.gradients, name, grp, OPS, &gi, &ii)) {
            return pc_engine_err_bad_params(err, cmd, "no gradient preset \"%s\" (see gradient.presets.list)",
                                            name);
        }
        return pc_gradient_preset_clone(
            (const pc_gradient_preset *)((char *)s->presets.gradients.v[gi].items + ii * OPS->size), out, err);
    }
    if (pc_json_get(p, "stops") != NULL) {
        char nm[512];
        const char *base = str_copy(p, "name", nm, sizeof nm) ? nm : "Custom";
        return pc_gradient_from_params(base, p, cmd, out, err);
    }
    return pc_gradient_preset_clone(&s->presets.gradient, out, err);
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *groups = yyjson_mut_arr(d);
    for (size_t i = 0; i < s->presets.gradients.n; i++) {
        const pc_preset_group *g = &s->presets.gradients.v[i];
        yyjson_mut_val *go = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, go, "name", g->name);
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        for (size_t j = 0; j < g->n; j++) {
            yyjson_mut_arr_append(arr, pc_gradient_preset_to_json(
                                           d, (const pc_gradient_preset *)((char *)g->items + j * OPS->size)));
        }
        yyjson_mut_obj_add_val(d, go, "presets", arr);
        yyjson_mut_arr_append(groups, go);
    }
    yyjson_mut_obj_add_val(d, root, "groups", groups);
    yyjson_mut_obj_add_val(d, root, "current", pc_gradient_preset_to_json(d, &s->presets.gradient));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status result_current(pc_json_value *out, const pc_gradient_preset *g, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, root, "current", pc_gradient_preset_to_json(d, g));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_select(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_gradient_preset g;
    if (preset_param(s, p, "gradient.presets.select", &g, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_gradient_preset_fini(&s->presets.gradient);
    s->presets.gradient = g;
    pc_engine_presets_changed(s);
    /* Recolouring a selected Gradient Fill layer is deferred to E10 (Fill layer creation). */
    return result_current(out, &s->presets.gradient, err);
}

static pc_status run_stop(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_gradient_preset g;
    if (pc_gradient_apply_preset_stop(&s->presets.gradient, p, s->tools.foreground, s->tools.background,
                                      "gradient.presets.stop", &g, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_gradient_preset_fini(&s->presets.gradient);
    s->presets.gradient = g;
    pc_engine_presets_changed(s);
    return result_current(out, &s->presets.gradient, err);
}

static pc_status run_new(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "gradient.presets.new";
    pc_gradient_preset g;
    if (pc_json_get(p, "stops") != NULL) {
        if (pc_gradient_from_params("", p, cmd, &g, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    } else {
        if (pc_gradient_preset_clone(&s->presets.gradient, &g, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    char nm[512];
    const char *base = str_copy(p, "name", nm, sizeof nm) ? nm : "Custom";
    char *uniq = pc_preset_unique_name(&s->presets.gradients, base, OPS);
    if (uniq == NULL) {
        pc_gradient_preset_fini(&g);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    free(g.name);
    g.name = uniq;
    char grp[512];
    const char *group = str_copy(p, "group", grp, sizeof grp) ? grp : NULL;
    size_t gi = pc_preset_group_index(&s->presets.gradients, group, err);
    if (gi == SIZE_MAX) {
        pc_gradient_preset_fini(&g);
        return PC_ERR_NOMEM;
    }
    char *gname = NULL;
    {
        const char *gn = s->presets.gradients.v[gi].name;
        gname = dup_str(gn);
    }
    if (pc_preset_group_push(&s->presets.gradients, gi, &g, OPS, err) != PC_OK) {
        pc_gradient_preset_fini(&g);
        free(gname);
        return PC_ERR_NOMEM;
    }
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(gname);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", g.name);
    yyjson_mut_obj_add_strcpy(d, root, "group", gname != NULL ? gname : "");
    yyjson_mut_doc_set_root(d, root);
    free(gname);
    return PC_OK;
}

static pc_status run_edit(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "gradient.presets.edit";
    size_t al = 0;
    const char *action = pc_json_as_str(pc_json_get(p, "action"), &al);
    if (action == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `action`");
    }
    char act[32];
    if (al >= sizeof act) {
        return pc_engine_err_bad_params(err, cmd, "unknown action");
    }
    memcpy(act, action, al);
    act[al] = '\0';
    pc_status rc = pc_preset_edit_groups(&s->presets.gradients, act, p, cmd, OPS, out, err);
    if (rc == PC_OK) {
        pc_engine_presets_changed(s);
    }
    return rc;
}

static pc_status run_reset(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool append = false;
    (void)pc_json_as_bool(pc_json_get(p, "append"), &append);
    pc_preset_groups b;
    if (pc_gradients_builtin(&b, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (append) {
        for (size_t i = 0; i < b.n; i++) {
            char *nm = pc_preset_unique_group(&s->presets.gradients, b.v[i].name);
            if (nm != NULL) {
                free(b.v[i].name);
                b.v[i].name = nm;
            }
            /* move the group over */
            pc_preset_group *gr = pc_preset_groups_push(&s->presets.gradients, b.v[i].name, err);
            if (gr == NULL) {
                pc_preset_groups_fini(&b, OPS);
                return PC_ERR_NOMEM;
            }
            gr->items = b.v[i].items;
            gr->n = b.v[i].n;
            gr->cap = b.v[i].cap;
            b.v[i].items = NULL;
            b.v[i].n = b.v[i].cap = 0;
        }
        pc_preset_groups_fini(&b, OPS);
    } else {
        pc_preset_groups_fini(&s->presets.gradients, OPS);
        s->presets.gradients = b;
    }
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "groups", s->presets.gradients.n);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- .grd import -------------------------------------------------------------------------- */

static float cl01(float v)
{
    if (!isfinite(v)) {
        return 0.0f;
    }
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static void imp_hsb(const float hsb[3], float out[3])
{
    float h = hsb[0] - 360.0f * floorf(hsb[0] / 360.0f);
    h /= 60.0f;
    float c = hsb[2] * hsb[1];
    float x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f));
    float r, g, b;
    switch ((unsigned)h) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    float m = hsb[2] - c;
    out[0] = r + m;
    out[1] = g + m;
    out[2] = b + m;
}

static void imp_lab(const float lab[3], float out[3])
{
    float fy = (lab[0] + 16.0f) / 116.0f;
    float fx = fy + lab[1] / 500.0f, fz = fy - lab[2] / 200.0f;
    const float d = 6.0f / 29.0f;
    const float k3 = 3.0f * (d * d);
#define INV(t) ((t) > d ? (t) * (t) * (t) : k3 * ((t) - 4.0f / 29.0f))
    float x = 0.9642f * INV(fx), y = INV(fy), z = 0.8249f * INV(fz);
#undef INV
    float r = 3.1339f * x - 1.6169f * y - 0.4906f * z;
    float g = -0.9788f * x + 1.9161f * y + 0.0335f * z;
    float bb = 0.0719f * x - 0.2290f * y + 1.4052f * z;
#define GAM(c) ((c) <= 0.0031308f ? 12.92f * (c) : 1.055f * powf((c), 1.0f / 2.4f) - 0.055f)
    float rc = r < 0 ? 0 : (r > 1 ? 1 : r), gc = g < 0 ? 0 : (g > 1 ? 1 : g), bc = bb < 0 ? 0 : (bb > 1 ? 1 : bb);
    out[0] = GAM(rc);
    out[1] = GAM(gc);
    out[2] = GAM(bc);
#undef GAM
}

static void grd_stop_color(const pc_psd_grd_color *c, pc_stop_color *out)
{
    float rgb[3] = {0, 0, 0};
    switch (c->model) {
    case PC_PSD_GRD_FOREGROUND:
        out->kind = PC_STOP_FOREGROUND;
        return;
    case PC_PSD_GRD_BACKGROUND:
        out->kind = PC_STOP_BACKGROUND;
        return;
    case PC_PSD_GRD_RGB:
        rgb[0] = c->c[0];
        rgb[1] = c->c[1];
        rgb[2] = c->c[2];
        break;
    case PC_PSD_GRD_HSB:
        imp_hsb(c->c, rgb);
        break;
    case PC_PSD_GRD_CMYK:
        rgb[0] = (1.0f - c->c[0]) * (1.0f - c->c[3]);
        rgb[1] = (1.0f - c->c[1]) * (1.0f - c->c[3]);
        rgb[2] = (1.0f - c->c[2]) * (1.0f - c->c[3]);
        break;
    case PC_PSD_GRD_LAB:
        imp_lab(c->c, rgb);
        break;
    default: /* GRAY */
        rgb[0] = rgb[1] = rgb[2] = 1.0f - c->c[0];
        break;
    }
    out->kind = PC_STOP_RGB;
    out->rgb[0] = cl01(rgb[0]);
    out->rgb[1] = cl01(rgb[1]);
    out->rgb[2] = cl01(rgb[2]);
}

static int cmp_cst(const void *a, const void *b)
{
    float x = ((const pc_gradient_cstop *)a)->loc, y = ((const pc_gradient_cstop *)b)->loc;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static int cmp_ost(const void *a, const void *b)
{
    float x = ((const pc_gradient_ostop *)a)->loc, y = ((const pc_gradient_ostop *)b)->loc;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static pc_status run_import_grd(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "gradient.presets.importGrd";
    pc_buf bytes;
    char stem[256];
    if (pc_engine_preset_file_bytes(p, cmd, ".grd file", &bytes, stem, sizeof stem, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    char group[512];
    yyjson_val *gv = pc_json_get(p, "group");
    if (gv != NULL) {
        if (!str_copy(p, "group", group, sizeof group)) {
            pc_buf_fini(&bytes);
            return pc_engine_err_bad_params(err, cmd, "`group` must be a non-empty string");
        }
    } else if (stem[0] != '\0') {
        (void)snprintf(group, sizeof group, "%s", stem);
    } else {
        (void)snprintf(group, sizeof group, "Imported Gradients");
    }
    pc_psd_grd_gradient *grads = NULL;
    size_t ng = 0;
    pc_error pe = {0};
    if (pc_psd_grd_parse(bytes.data, bytes.len, &grads, &ng, &pe) != PC_OK) {
        pc_buf_fini(&bytes);
        return pc_engine_err_bad_params(err, cmd, "not a readable gradient file (.grd): %s",
                                        pe.msg[0] ? pe.msg : "parse error");
    }
    pc_buf_fini(&bytes);

    /* Build the imported group. */
    pc_gradient_preset *items = ng > 0 ? calloc(ng, sizeof *items) : NULL;
    if (ng > 0 && items == NULL) {
        pc_psd_grd_free(grads, ng);
        return pc_fail(err, PC_ERR_NOMEM, "import");
    }
    size_t nitems = 0, noise = 0;
    bool midpoints = false;
    for (size_t i = 0; i < ng; i++) {
        const pc_psd_grd_gradient *g = &grads[i];
        if (g->noise || g->nstops < 2) {
            noise++;
            continue;
        }
        for (size_t j = 0; j < g->nstops && !midpoints; j++) {
            if (fabsf(g->stops[j].midpoint - 0.5f) > 0.01f) {
                midpoints = true;
            }
        }
        for (size_t j = 0; j < g->nopacity && !midpoints; j++) {
            if (fabsf(g->opacity[j].midpoint - 0.5f) > 0.01f) {
                midpoints = true;
            }
        }
        pc_gradient_preset *gp = &items[nitems];
        memset(gp, 0, sizeof *gp);
        gp->stops = malloc(g->nstops * sizeof *gp->stops);
        if (gp->stops == NULL) {
            goto oom;
        }
        for (size_t j = 0; j < g->nstops; j++) {
            gp->stops[j].loc = g->stops[j].location;
            grd_stop_color(&g->stops[j].color, &gp->stops[j].color);
        }
        gp->nstops = g->nstops;
        qsort(gp->stops, gp->nstops, sizeof *gp->stops, cmp_cst);
        if (g->nopacity > 0) {
            gp->opacity = malloc(g->nopacity * sizeof *gp->opacity);
            if (gp->opacity == NULL) {
                free(gp->stops);
                goto oom;
            }
            for (size_t j = 0; j < g->nopacity; j++) {
                gp->opacity[j].loc = g->opacity[j].location;
                gp->opacity[j].opacity = g->opacity[j].opacity;
            }
            gp->nopacity = g->nopacity;
            qsort(gp->opacity, gp->nopacity, sizeof *gp->opacity, cmp_ost);
            bool all_opaque = true;
            for (size_t j = 0; j < gp->nopacity; j++) {
                if (gp->opacity[j].opacity < 1.0f) {
                    all_opaque = false;
                    break;
                }
            }
            if (all_opaque) {
                free(gp->opacity);
                gp->opacity = NULL;
                gp->nopacity = 0;
            }
        }
        const char *nm = pc_psd_str_c(&g->name);
        /* trim */
        while (*nm == ' ' || *nm == '\t') {
            nm++;
        }
        if (*nm == '\0') {
            char buf[64];
            (void)snprintf(buf, sizeof buf, "Gradient %zu", i + 1);
            gp->name = dup_str(buf);
        } else {
            char buf[1024];
            (void)snprintf(buf, sizeof buf, "%s", nm);
            size_t bl = strlen(buf);
            while (bl > 0 && (buf[bl - 1] == ' ' || buf[bl - 1] == '\t')) {
                buf[--bl] = '\0';
            }
            gp->name = dup_str(buf);
        }
        if (gp->name == NULL) {
            free(gp->stops);
            free(gp->opacity);
            goto oom;
        }
        nitems++;
    }
    pc_psd_grd_free(grads, ng);
    grads = NULL;

    if (nitems == 0) {
        for (size_t i = 0; i < nitems; i++) {
            pc_gradient_preset_fini(&items[i]);
        }
        free(items);
        return pc_engine_err_bad_params(
            err, cmd, "the file holds no gradients PhotoCraft can use (noise gradients only)");
    }

    /* replace a group of the same name (default true) */
    bool replace = true;
    (void)pc_json_as_bool(pc_json_get(p, "replace"), &replace);
    if (replace) {
        for (size_t i = 0; i < s->presets.gradients.n;) {
            if (strcmp(s->presets.gradients.v[i].name, group) == 0) {
                pc_preset_group *gr = &s->presets.gradients.v[i];
                for (size_t j = 0; j < gr->n; j++) {
                    OPS->fini((char *)gr->items + j * OPS->size);
                }
                free(gr->items);
                free(gr->name);
                size_t tail = s->presets.gradients.n - i - 1;
                if (tail > 0) {
                    memmove(gr, gr + 1, tail * sizeof *gr);
                }
                s->presets.gradients.n--;
            } else {
                i++;
            }
        }
    }
    pc_preset_group *gr = pc_preset_groups_push(&s->presets.gradients, group, err);
    if (gr == NULL) {
        goto oom;
    }
    size_t gi = s->presets.gradients.n - 1;
    for (size_t i = 0; i < nitems; i++) {
        if (pc_preset_group_push(&s->presets.gradients, gi, &items[i], OPS, err) != PC_OK) {
            /* the remaining items are still owned by us */
            for (size_t j = i; j < nitems; j++) {
                pc_gradient_preset_fini(&items[j]);
            }
            free(items);
            return PC_ERR_NOMEM;
        }
    }
    free(items);
    pc_engine_presets_changed(s);

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "group", group);
    yyjson_mut_val *imported = yyjson_mut_arr(d);
    for (size_t i = 0; i < gr->n; i++) {
        yyjson_mut_arr_add_strcpy(d, imported,
                                  ((const pc_gradient_preset *)((char *)gr->items + i * OPS->size))->name);
    }
    yyjson_mut_obj_add_val(d, root, "imported", imported);
    yyjson_mut_obj_add_uint(d, root, "count", gr->n);
    yyjson_mut_val *warn = yyjson_mut_arr(d);
    if (noise > 0) {
        char w[128];
        (void)snprintf(w, sizeof w, "%zu noise gradients are not supported and were skipped", noise);
        yyjson_mut_arr_add_strcpy(d, warn, w);
    }
    if (midpoints) {
        yyjson_mut_arr_add_strcpy(d, warn, "colour and opacity midpoints other than 50 % are drawn at 50 %");
    }
    yyjson_mut_obj_add_val(d, root, "warnings", warn);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
oom:
    if (grads != NULL) {
        pc_psd_grd_free(grads, ng);
    }
    for (size_t i = 0; i < nitems; i++) {
        pc_gradient_preset_fini(&items[i]);
    }
    free(items);
    return pc_fail(err, PC_ERR_NOMEM, "import");
}

static const char STOPS_DOC[] =
    "\"stops\":[[t 0..1,\"#rrggbb\"|\"foreground\"|\"background\"],...],\"transparency\":[[t,opacity "
    "0..100],...]?";

static const pc_engine_command SPECS[] = {
    {"gradient.presets.list", "Gradient Presets", NULL, 0, NULL,
     "{} -> {groups:[{name,presets:[{name,stops,transparency}]}],current}", pc_engine_always, run_list, false},
    {"gradient.presets.select", "Select Gradient", NULL, 0, NULL,
     "{\"preset\":name|stops,\"group\":name?,\"applyToLayer\":bool=true} -> {current,layer?}. The "
     "Gradient tool paints with it.",
     pc_engine_always, run_select, true},
    {"gradient.presets.stop", "Edit Gradient Stop", NULL, 0, NULL,
     "{\"action\":\"add|move|delete|color|opacity\",\"kind\":\"color|opacity\"=\"color\",\"index\":stop "
     "number,\"location\":0..1,\"color\":\"#rrggbb\"|\"foreground\"|\"background\",\"opacity\":0..100} -> "
     "{\"current\"}. Edits the Gradient tool's current gradient (no layer, midpoints stay centred).",
     pc_engine_always, run_stop, true},
    {"gradient.presets.new", "New Gradient Preset", NULL, 0, NULL,
     "{\"name\":str=\"Custom\",\"group\":name?=first,stops (default: the current gradient)}",
     pc_engine_always, run_new, true},
    {"gradient.presets.edit", "Edit Gradient Presets", NULL, 0, NULL,
     "{\"action\":\"rename|delete|move|newGroup|renameGroup|deleteGroup\",...}", pc_engine_always, run_edit,
     true},
    {"gradient.presets.reset", "Restore Default Gradients", NULL, 0, NULL, "{\"append\":bool=false}",
     pc_engine_always, run_reset, true},
    {"gradient.presets.importGrd", "Import Gradients...", NULL, 0, NULL,
     "{\"path\":\".grd file\"?,\"data\":base64 bytes?,\"group\":string?=file name,\"replace\":bool=true "
     "(replace a group of the same name)}",
     pc_engine_always, run_import_grd, true},
};

const pc_engine_command *pc_engine_gradient_preset_specs(size_t *n)
{
    init_ops();
    (void)STOPS_DOC;
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
