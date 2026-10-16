/* Photoshop menu parity report (port of crates/ui-egui/src/parity.rs). */
#include "ui/parity.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "ui/menu_catalog.h"
#include "ui/menus.h"

double pc_ui_parity_percent(const pc_ui_parity *p)
{
    return p->total == 0 ? 0.0 : 100.0 * (double)p->live / (double)p->total;
}

void pc_ui_parity_free(pc_ui_parity *p)
{
    if (p == NULL) {
        return;
    }
    for (size_t i = 0; i < p->n_menus; i++) {
        free(p->menus[i].menu);
        for (size_t j = 0; j < p->menus[i].n_missing; j++) {
            free(p->menus[i].missing[j]);
        }
        free(p->menus[i].missing);
    }
    free(p->menus);
    memset(p, 0, sizeof *p);
}

/* The submenu-path + label line for a missing item: "Sub › Deeper › Label (`id`)". */
static char *missing_line(const pc_ui_catalog_item *c)
{
    const char *sep = " \xE2\x80\xBA "; /* " › " */
    size_t need = 1;
    for (size_t i = 1; i < c->depth; i++) {
        need += strlen(c->path[i]) + strlen(sep);
    }
    need += strlen(c->label) + strlen(" (`") + strlen(c->id) + strlen("`)");
    char *s = pc_malloc(need);
    if (s == NULL) {
        return NULL;
    }
    s[0] = '\0';
    for (size_t i = 1; i < c->depth; i++) {
        strcat(s, c->path[i]);
        strcat(s, sep);
    }
    strcat(s, c->label);
    strcat(s, " (`");
    strcat(s, c->id);
    strcat(s, "`)");
    return s;
}

static pc_ui_menu_parity *menu_for(pc_ui_parity *p, const char *top, size_t *cap, pc_error *err)
{
    for (size_t i = 0; i < p->n_menus; i++) {
        if (strcmp(p->menus[i].menu, top) == 0) {
            return &p->menus[i];
        }
    }
    p->menus = pc_grow_array(p->menus, cap, p->n_menus + 1, sizeof *p->menus);
    if (p->menus == NULL) {
        (void)pc_fail(err, PC_ERR_NOMEM, "parity: out of memory");
        return NULL;
    }
    pc_ui_menu_parity *m = &p->menus[p->n_menus];
    memset(m, 0, sizeof *m);
    m->menu = pc_strndup(top, strlen(top));
    if (m->menu == NULL) {
        (void)pc_fail(err, PC_ERR_NOMEM, "parity: out of memory");
        return NULL;
    }
    p->n_menus++;
    return m;
}

/* Has this id already been counted (its first catalogue occurrence is before index `upto`)? */
static bool already_counted(const char *id, size_t upto)
{
    for (size_t i = 0; i < upto; i++) {
        if (!pc_ui_catalog_is_separator(PC_UI_CATALOG[i].id) &&
            strcmp(PC_UI_CATALOG[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

pc_status pc_ui_parity_compute(pc_ui_parity *out, pc_error *err)
{
    if (out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "parity: NULL out");
    }
    memset(out, 0, sizeof *out);
    size_t menus_cap = 0;
    size_t *missing_caps = NULL; /* parallel per-menu missing capacity */
    size_t missing_caps_len = 0, missing_caps_cap = 0;
    pc_status st = PC_OK;

    for (size_t i = 0; i < PC_UI_CATALOG_N; i++) {
        const pc_ui_catalog_item *c = &PC_UI_CATALOG[i];
        if (pc_ui_catalog_is_separator(c->id) || already_counted(c->id, i)) {
            continue;
        }
        const char *top = c->depth > 0 ? c->path[0] : "?";
        size_t before = out->n_menus;
        pc_ui_menu_parity *m = menu_for(out, top, &menus_cap, err);
        if (m == NULL) {
            st = PC_ERR_NOMEM;
            goto done;
        }
        /* Keep the parallel missing-capacity array in step with n_menus. */
        if (out->n_menus > before) {
            missing_caps = pc_grow_array(missing_caps, &missing_caps_cap, out->n_menus, sizeof *missing_caps);
            if (missing_caps == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "parity: out of memory");
                goto done;
            }
            missing_caps[missing_caps_len++] = 0;
        }
        size_t mi = (size_t)(m - out->menus);
        m->total++;
        if (pc_ui_is_live(c->id)) {
            m->live++;
        } else {
            char *line = missing_line(c);
            if (line == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "parity: out of memory");
                goto done;
            }
            char **grown = pc_grow_array(m->missing, &missing_caps[mi], m->n_missing + 1, sizeof *m->missing);
            if (grown == NULL) {
                free(line);
                st = pc_fail(err, PC_ERR_NOMEM, "parity: out of memory");
                goto done;
            }
            m->missing = grown;
            m->missing[m->n_missing++] = line;
        }
    }

    for (size_t i = 0; i < out->n_menus; i++) {
        out->live += out->menus[i].live;
        out->total += out->menus[i].total;
    }
    free(missing_caps);
    return PC_OK;

done:
    free(missing_caps);
    pc_ui_parity_free(out);
    return st;
}

pc_status pc_ui_parity_to_markdown(const pc_ui_parity *p, pc_buf *out, pc_error *err)
{
    pc_buf_truncate(out, 0);
    pc_status st = PC_OK;
    st = pc_buf_append_str(out, "# Photoshop menu parity\n\n", err);
    if (st == PC_OK) {
        st = pc_buf_append_str(out,
                               "Generated by `tools/parity.sh` from the menu catalog (Photoshop's menu tree) and "
                               "the live command registry. Do not edit by hand.\n\n",
                               err);
    }
    if (st == PC_OK) {
        st = pc_buf_appendf(out, err, "**%zu of %zu menu items live (%.1f%%).**\n\n", p->live, p->total,
                            pc_ui_parity_percent(p));
    }
    if (st == PC_OK) {
        st = pc_buf_append_str(out, "| Menu | Live | Total | % |\n|---|---:|---:|---:|\n", err);
    }
    for (size_t i = 0; i < p->n_menus && st == PC_OK; i++) {
        const pc_ui_menu_parity *m = &p->menus[i];
        double pct = m->total == 0 ? 0.0 : 100.0 * (double)m->live / (double)m->total;
        st = pc_buf_appendf(out, err, "| %s | %zu | %zu | %.0f%% |\n", m->menu, m->live, m->total, pct);
    }
    if (st == PC_OK) {
        st = pc_buf_append_str(out, "\n## Not yet implemented\n", err);
    }
    for (size_t i = 0; i < p->n_menus && st == PC_OK; i++) {
        const pc_ui_menu_parity *m = &p->menus[i];
        if (m->n_missing == 0) {
            continue;
        }
        st = pc_buf_appendf(out, err, "\n### %s (%zu)\n\n", m->menu, m->n_missing);
        for (size_t j = 0; j < m->n_missing && st == PC_OK; j++) {
            st = pc_buf_appendf(out, err, "- %s\n", m->missing[j]);
        }
    }
    return st;
}
