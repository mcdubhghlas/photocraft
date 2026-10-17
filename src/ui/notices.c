/* Notices: see notices.h. Port of crates/ui-egui/src/notices.rs (data model + auto-hide timer). */
#include "ui/notices.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/i18n.h"

/* A full canvas refresh on the CPU slower than this (ms) gets a notice. */
#define SLOW_CPU_REFRESH_MS 1000.0

void pc_ui_notices_init(pc_ui_notices *ns)
{
    memset(ns, 0, sizeof *ns);
    ns->next_id = 1;
}

static void notice_fini(pc_ui_notice *n)
{
    free(n->title);
    for (size_t i = 0; i < n->n_lines; i++) {
        free(n->lines[i]);
    }
    free(n->lines);
    free(n->dismiss_pref);
    for (size_t i = 0; i < n->n_args; i++) {
        free(n->args[i].key);
        free(n->args[i].val);
    }
    free(n->args);
}

void pc_ui_notices_fini(pc_ui_notices *ns)
{
    if (ns == NULL) {
        return;
    }
    for (size_t i = 0; i < ns->n; i++) {
        notice_fini(&ns->items[i]);
    }
    free(ns->items);
    free(ns->slow_noticed);
    memset(ns, 0, sizeof *ns);
}

static char **dup_lines(const char *const *lines, size_t n)
{
    if (n == 0) {
        return NULL;
    }
    char **out = calloc(n, sizeof(char *));
    if (out == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = strdup(lines[i] ? lines[i] : "");
    }
    return out;
}

static void cap_notices(pc_ui_notices *ns)
{
    while (ns->n > PC_UI_MAX_NOTICES) {
        /* Drop the oldest temporary (no dismiss_pref), else the first. */
        size_t at = 0;
        for (size_t i = 0; i < ns->n; i++) {
            if (ns->items[i].dismiss_pref == NULL) {
                at = i;
                break;
            }
        }
        notice_fini(&ns->items[at]);
        memmove(&ns->items[at], &ns->items[at + 1], (ns->n - at - 1) * sizeof(pc_ui_notice));
        ns->n--;
    }
}

uint64_t pc_ui_notices_post(pc_ui_notices *ns, const char *title, const char *const *lines,
                            size_t n_lines, bool error, const char *dismiss_pref)
{
    pc_ui_notice *grown = realloc(ns->items, (ns->n + 1) * sizeof(pc_ui_notice));
    if (grown == NULL) {
        return 0;
    }
    ns->items = grown;
    uint64_t id = ns->next_id++;
    pc_ui_notice *n = &ns->items[ns->n];
    memset(n, 0, sizeof *n);
    n->id = id;
    n->title = strdup(title ? title : "");
    n->lines = dup_lines(lines, n_lines);
    n->n_lines = n->lines ? n_lines : 0;
    n->error = error;
    n->dismiss_pref = dismiss_pref ? strdup(dismiss_pref) : NULL;
    ns->n++;
    cap_notices(ns);
    return id;
}

bool pc_ui_notices_dismiss(pc_ui_notices *ns, uint64_t id, char *pref_out, size_t pref_cap)
{
    if (pref_out != NULL && pref_cap > 0) {
        pref_out[0] = '\0';
    }
    for (size_t i = 0; i < ns->n; i++) {
        if (ns->items[i].id != id) {
            continue;
        }
        if (pref_out != NULL && pref_cap > 0 && ns->items[i].dismiss_pref != NULL) {
            size_t k = strlen(ns->items[i].dismiss_pref);
            if (k >= pref_cap) {
                k = pref_cap - 1;
            }
            memcpy(pref_out, ns->items[i].dismiss_pref, k);
            pref_out[k] = '\0';
        }
        notice_fini(&ns->items[i]);
        memmove(&ns->items[i], &ns->items[i + 1], (ns->n - i - 1) * sizeof(pc_ui_notice));
        ns->n--;
        return true;
    }
    return false;
}

double pc_ui_notices_expire(pc_ui_notices *ns, double now, bool hovered, bool just_left,
                            bool auto_hide, double duration, bool *has_next)
{
    *has_next = false;
    if (!auto_hide) {
        return 0.0;
    }
    uint64_t expired[PC_UI_MAX_NOTICES + 8];
    size_t n_expired = 0;
    double next = INFINITY;
    for (size_t i = 0; i < ns->n; i++) {
        pc_ui_notice *n = &ns->items[i];
        if (!n->has_shown_at) {
            n->has_shown_at = true;
            n->shown_at = now;
        }
        if (hovered) {
            n->shown_at = now;
            continue;
        }
        if (just_left) {
            n->shown_at = now;
        }
        double elapsed = now - n->shown_at;
        if (elapsed < 0.0) {
            elapsed = 0.0;
        }
        if (elapsed >= duration) {
            if (n_expired < sizeof expired / sizeof expired[0]) {
                expired[n_expired++] = n->id;
            }
        } else {
            double rem = duration - elapsed;
            if (rem < next) {
                next = rem;
            }
        }
    }
    for (size_t e = 0; e < n_expired; e++) {
        for (size_t i = 0; i < ns->n; i++) {
            if (ns->items[i].id == expired[e]) {
                notice_fini(&ns->items[i]);
                memmove(&ns->items[i], &ns->items[i + 1], (ns->n - i - 1) * sizeof(pc_ui_notice));
                ns->n--;
                break;
            }
        }
    }
    if (isfinite(next)) {
        *has_next = true;
        return next;
    }
    return 0.0;
}

void pc_ui_notice_text(const pc_ui_notice *n, const char *s, pc_buf *out)
{
    const char *translated = pc_i18n_t(s);
    if (n->n_args == 0) {
        pc_buf_truncate(out, 0);
        pc_buf_append(out, translated, strlen(translated), NULL);
        return;
    }
    const char **keys = calloc(n->n_args, sizeof(char *));
    const char **vals = calloc(n->n_args, sizeof(char *));
    if (keys == NULL || vals == NULL) {
        free(keys);
        free(vals);
        pc_buf_truncate(out, 0);
        pc_buf_append(out, translated, strlen(translated), NULL);
        return;
    }
    for (size_t i = 0; i < n->n_args; i++) {
        keys[i] = n->args[i].key;
        vals[i] = n->args[i].val;
    }
    (void)pc_i18n_fmt(translated, keys, vals, n->n_args, out, NULL);
    free(keys);
    free(vals);
}

void pc_ui_notices_io_warnings(pc_ui_notices *ns, const char *what, const char *const *warnings,
                               size_t n, pc_buf *status_out, bool *status_error)
{
    if (n == 0) {
        return;
    }
    const char *first = warnings[0] ? warnings[0] : "";
    pc_buf_truncate(status_out, 0);
    if (n == 1) {
        /* "{what}: {first}" */
        pc_buf_append(status_out, what, strlen(what), NULL);
        pc_buf_append(status_out, ": ", 2, NULL);
        pc_buf_append(status_out, first, strlen(first), NULL);
    } else {
        /* title = trn(n, "{what} with {n} warning", "{what} with {n} warnings"), {what} filled. */
        pc_buf title_t = PC_BUF_INIT;
        (void)pc_i18n_trn(pc_i18n_current(), (uint64_t)n, "{what} with {n} warning",
                          "{what} with {n} warnings", &title_t, NULL);
        pc_buf title = PC_BUF_INIT;
        const char *k = "what";
        (void)pc_i18n_fmt(pc_buf_cstr(&title_t), &k, &what, 1, &title, NULL);
        /* status = "{title}: {first} …" */
        pc_buf_append(status_out, pc_buf_cstr(&title), strlen(pc_buf_cstr(&title)), NULL);
        pc_buf_append(status_out, ": ", 2, NULL);
        pc_buf_append(status_out, first, strlen(first), NULL);
        pc_buf_append(status_out, " \xe2\x80\xa6", 4, NULL);
        pc_buf_fini(&title_t);
        pc_buf_fini(&title);
    }
    if (status_error != NULL) {
        *status_error = true;
    }
    /* The notice's title is the translated count line; post with the raw warnings as lines. */
    pc_buf title_t = PC_BUF_INIT;
    (void)pc_i18n_trn(pc_i18n_current(), (uint64_t)n, "{what} with {n} warning",
                      "{what} with {n} warnings", &title_t, NULL);
    pc_buf title = PC_BUF_INIT;
    const char *k = "what";
    (void)pc_i18n_fmt(pc_buf_cstr(&title_t), &k, &what, 1, &title, NULL);
    (void)pc_ui_notices_post(ns, pc_buf_cstr(&title), warnings, n, false, NULL);
    pc_buf_fini(&title_t);
    pc_buf_fini(&title);
}

static bool slow_seen(const pc_ui_notices *ns, uint64_t doc)
{
    for (size_t i = 0; i < ns->n_slow; i++) {
        if (ns->slow_noticed[i] == doc) {
            return true;
        }
    }
    return false;
}

bool pc_ui_notices_slow_cpu_refresh(pc_ui_notices *ns, uint64_t doc, double ms, const char *reason)
{
    if (isnan(ms) || ms < SLOW_CPU_REFRESH_MS || slow_seen(ns, doc)) {
        return false;
    }
    uint64_t *grown = realloc(ns->slow_noticed, (ns->n_slow + 1) * sizeof(uint64_t));
    if (grown == NULL) {
        return false;
    }
    ns->slow_noticed = grown;
    ns->slow_noticed[ns->n_slow++] = doc;

    char seconds[32];
    (void)snprintf(seconds, sizeof seconds, "%.1f", ms / 1000.0);

    char *lines[3];
    size_t nl = 0;
    pc_buf l0 = PC_BUF_INIT;
    const char *sk = "seconds";
    const char *sv = seconds;
    (void)pc_i18n_fmt(pc_i18n_t("This redraw took {seconds} s on the CPU."), &sk, &sv, 1, &l0, NULL);
    lines[nl++] = strdup(pc_buf_cstr(&l0));
    lines[nl++] = strdup(pc_i18n_t("Edits that change the whole document, such as a layer's blend "
                                   "mode, opacity or visibility, redraw all of it."));
    pc_buf l2 = PC_BUF_INIT;
    if (reason != NULL && strcmp(reason, PC_UI_GPU_OVER_BUDGET) == 0) {
        lines[nl++] = strdup(pc_i18n_t(
            "Its layers don't fit the GPU memory budget. Raising Memory usage in Preferences > "
            "Performance raises the budget, up to a quarter of this computer's memory."));
    } else if (reason != NULL) {
        const char *rk = "reason";
        (void)pc_i18n_fmt(pc_i18n_t("The GPU compositor wasn't used: {reason}"), &rk, &reason, 1, &l2,
                          NULL);
        lines[nl++] = strdup(pc_buf_cstr(&l2));
    }
    (void)pc_ui_notices_post(ns, pc_i18n_t("Large document: redrawing on the CPU"),
                             (const char *const *)lines, nl, false, NULL);
    for (size_t i = 0; i < nl; i++) {
        free(lines[i]);
    }
    pc_buf_fini(&l0);
    pc_buf_fini(&l2);
    return true;
}
