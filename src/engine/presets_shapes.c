/* Shape presets data model, the built-in shape path language and the built-in shapes (port of
 * crates/engine/src/presets/shapes.rs data, E2b). Shapes are drawn in a 100x100 box with a small
 * path language: M/L/C/Q/Z, O cx cy r (circle) and `!` before M/O to subtract. Never crashes. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "geom/geom.h"
#include "presets.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TAU (2.0 * M_PI)
#define FRAC_PI_2 (M_PI / 2.0)

static char *dup_str(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

/* ---- lifecycle + ops ---------------------------------------------------------------------- */

void pc_shape_preset_fini(pc_shape_preset *s)
{
    if (s == NULL) {
        return;
    }
    free(s->name);
    pc_path_fini(&s->path);
    s->name = NULL;
}

pc_status pc_shape_preset_clone(const pc_shape_preset *src, pc_shape_preset *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->name = dup_str(src->name);
    if (out->name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "shape");
    }
    if (pc_path_clone(&src->path, &out->path, err) != PC_OK) {
        pc_shape_preset_fini(out);
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

static const char *sh_get_name(const void *item)
{
    return ((const pc_shape_preset *)item)->name;
}
static bool sh_set_name(void *item, const char *name)
{
    char *nm = dup_str(name);
    if (nm == NULL) {
        return false;
    }
    pc_shape_preset *s = item;
    free(s->name);
    s->name = nm;
    return true;
}
static void sh_fini(void *item)
{
    pc_shape_preset_fini(item);
}
static pc_status sh_clone(const void *src, void *dst, pc_error *err)
{
    return pc_shape_preset_clone(src, dst, err);
}

const pc_preset_item_ops *pc_shape_preset_ops(void)
{
    static const pc_preset_item_ops ops = {sizeof(pc_shape_preset), sh_get_name, sh_set_name, sh_fini, sh_clone};
    return &ops;
}

/* ---- path builder helpers ----------------------------------------------------------------- */

typedef struct {
    pc_knot *knots;
    size_t n, cap;
    bool closed;
    pc_path_op op;
    bool active;
} sub_build;

typedef struct {
    pc_subpath *subs;
    size_t n, cap;
} path_build;

static bool knots_push(sub_build *s, pc_knot k)
{
    if (s->n == s->cap) {
        size_t cap = s->cap == 0 ? 8 : s->cap * 2;
        pc_knot *v = realloc(s->knots, cap * sizeof *v);
        if (v == NULL) {
            return false;
        }
        s->knots = v;
        s->cap = cap;
    }
    s->knots[s->n++] = k;
    return true;
}

static bool subs_push(path_build *pb, pc_subpath sp)
{
    if (pb->n == pb->cap) {
        size_t cap = pb->cap == 0 ? 8 : pb->cap * 2;
        pc_subpath *v = realloc(pb->subs, cap * sizeof *v);
        if (v == NULL) {
            return false;
        }
        pb->subs = v;
        pb->cap = cap;
    }
    pb->subs[pb->n++] = sp;
    return true;
}

/* Finish the current subpath into pb (closing + dedup of a repeated first/last anchor). */
static bool finish_sub(sub_build *cur, path_build *pb, bool closed)
{
    if (!cur->active) {
        return true;
    }
    cur->active = false;
    cur->closed = closed;
    if (closed && cur->n > 1) {
        pc_point f = cur->knots[0].anchor;
        pc_point l = cur->knots[cur->n - 1].anchor;
        if (fabs(f.x - l.x) < 1e-9 && fabs(f.y - l.y) < 1e-9) {
            pc_knot last = cur->knots[cur->n - 1];
            cur->n--;
            cur->knots[0].in_ctrl = last.in_ctrl;
        }
    }
    if (cur->n > 0) {
        pc_subpath sp = {cur->closed, cur->knots, cur->n, cur->op};
        if (!subs_push(pb, sp)) {
            free(cur->knots);
            cur->knots = NULL;
            cur->n = cur->cap = 0;
            return false;
        }
        /* ownership moved into pb */
        cur->knots = NULL;
        cur->n = cur->cap = 0;
    } else {
        free(cur->knots);
        cur->knots = NULL;
        cur->cap = 0;
    }
    return true;
}

static bool push_circle(path_build *pb, double cx, double cy, double r, pc_path_op op)
{
    double k = r * 0.552284750;
    pc_knot ks[4];
    ks[0] = pc_knot_smooth(pc_point_make(cx, cy - r), pc_point_make(cx - k, cy - r), pc_point_make(cx + k, cy - r));
    ks[1] = pc_knot_smooth(pc_point_make(cx + r, cy), pc_point_make(cx + r, cy - k), pc_point_make(cx + r, cy + k));
    ks[2] = pc_knot_smooth(pc_point_make(cx, cy + r), pc_point_make(cx + k, cy + r), pc_point_make(cx - k, cy + r));
    ks[3] = pc_knot_smooth(pc_point_make(cx - r, cy), pc_point_make(cx - r, cy + k), pc_point_make(cx - r, cy - k));
    pc_knot *kn = malloc(4 * sizeof *kn);
    if (kn == NULL) {
        return false;
    }
    memcpy(kn, ks, sizeof ks);
    pc_subpath sp = {true, kn, 4, op};
    if (!subs_push(pb, sp)) {
        free(kn);
        return false;
    }
    return true;
}

/* ---- tokenizer + parser ------------------------------------------------------------------- */

static bool tok_num(char **toks, size_t ntok, size_t *i, double *out)
{
    if (*i >= ntok) {
        return false;
    }
    char *end = NULL;
    double v = strtod(toks[*i], &end);
    if (end == toks[*i] || *end != '\0') {
        return false;
    }
    *out = v;
    (*i)++;
    *out = v;
    return true;
}

static bool tok_is_num(char **toks, size_t ntok, size_t i)
{
    if (i >= ntok) {
        return false;
    }
    char *end = NULL;
    (void)strtod(toks[i], &end);
    return end != toks[i] && *end == '\0';
}

bool pc_shape_parse_path(const char *src, pc_path *out, char msg[256])
{
    memset(out, 0, sizeof *out);
    /* Build a mutable copy with ',' -> ' ' and '!' isolated, then split on whitespace. */
    size_t srclen = strlen(src);
    char *buf = malloc(srclen * 3 + 1);
    if (buf == NULL) {
        (void)snprintf(msg, 256, "out of memory");
        return false;
    }
    size_t w = 0;
    for (size_t k = 0; k < srclen; k++) {
        char c = src[k];
        if (c == ',') {
            buf[w++] = ' ';
        } else if (c == '!') {
            buf[w++] = ' ';
            buf[w++] = '!';
            buf[w++] = ' ';
        } else {
            buf[w++] = c;
        }
    }
    buf[w] = '\0';
    /* tokenise */
    char **toks = NULL;
    size_t ntok = 0, tcap = 0;
    for (size_t k = 0; k < w;) {
        while (k < w && (buf[k] == ' ' || buf[k] == '\t' || buf[k] == '\n' || buf[k] == '\r' || buf[k] == '\f' || buf[k] == '\v')) {
            k++;
        }
        if (k >= w) {
            break;
        }
        size_t start = k;
        while (k < w && !(buf[k] == ' ' || buf[k] == '\t' || buf[k] == '\n' || buf[k] == '\r' || buf[k] == '\f' || buf[k] == '\v')) {
            k++;
        }
        buf[k < w ? k : w] = '\0';
        if (ntok == tcap) {
            size_t nc = tcap == 0 ? 16 : tcap * 2;
            char **nt = realloc(toks, nc * sizeof *nt);
            if (nt == NULL) {
                free(toks);
                free(buf);
                (void)snprintf(msg, 256, "out of memory");
                return false;
            }
            toks = nt;
            tcap = nc;
        }
        toks[ntok++] = &buf[start];
        k++;
    }

    path_build pb = {0};
    sub_build cur = {0};
    pc_path_op op = PC_PATH_COMBINE;
    bool ok = true;
    const char *errmsg = NULL;
    size_t i = 0;
    while (i < ntok) {
        const char *t = toks[i];
        i++;
        if (strcmp(t, "!") == 0) {
            op = PC_PATH_SUBTRACT;
        } else if (strcmp(t, "M") == 0) {
            if (!finish_sub(&cur, &pb, true)) {
                ok = false;
                errmsg = "out of memory";
                break;
            }
            double x, y;
            if (!tok_num(toks, ntok, &i, &x) || !tok_num(toks, ntok, &i, &y)) {
                ok = false;
                errmsg = "expected a number";
                break;
            }
            cur.active = true;
            cur.closed = true;
            cur.op = op;
            cur.knots = NULL;
            cur.n = cur.cap = 0;
            if (!knots_push(&cur, pc_knot_corner(x, y))) {
                ok = false;
                errmsg = "out of memory";
                break;
            }
            op = PC_PATH_COMBINE;
        } else if (strcmp(t, "L") == 0) {
            if (!cur.active) {
                ok = false;
                errmsg = "L before M";
                break;
            }
            while (tok_is_num(toks, ntok, i)) {
                double x, y;
                if (!tok_num(toks, ntok, &i, &x) || !tok_num(toks, ntok, &i, &y)) {
                    ok = false;
                    errmsg = "expected a number";
                    break;
                }
                if (!knots_push(&cur, pc_knot_corner(x, y))) {
                    ok = false;
                    errmsg = "out of memory";
                    break;
                }
            }
            if (!ok) {
                break;
            }
        } else if (strcmp(t, "C") == 0 || strcmp(t, "Q") == 0) {
            bool cubic = strcmp(t, "C") == 0;
            if (!cur.active || cur.n == 0) {
                ok = false;
                errmsg = "curve before M";
                break;
            }
            while (tok_is_num(toks, ntok, i)) {
                pc_point last = cur.knots[cur.n - 1].anchor;
                pc_point c1, c2, p;
                if (cubic) {
                    double a, b, c, d, e, f;
                    if (!tok_num(toks, ntok, &i, &a) || !tok_num(toks, ntok, &i, &b) ||
                        !tok_num(toks, ntok, &i, &c) || !tok_num(toks, ntok, &i, &d) ||
                        !tok_num(toks, ntok, &i, &e) || !tok_num(toks, ntok, &i, &f)) {
                        ok = false;
                        errmsg = "expected a number";
                        break;
                    }
                    c1 = pc_point_make(a, b);
                    c2 = pc_point_make(c, d);
                    p = pc_point_make(e, f);
                } else {
                    double qx, qy, px, py;
                    if (!tok_num(toks, ntok, &i, &qx) || !tok_num(toks, ntok, &i, &qy) ||
                        !tok_num(toks, ntok, &i, &px) || !tok_num(toks, ntok, &i, &py)) {
                        ok = false;
                        errmsg = "expected a number";
                        break;
                    }
                    pc_point q = pc_point_make(qx, qy);
                    p = pc_point_make(px, py);
                    c1 = pc_point_make(last.x + (q.x - last.x) * 2.0 / 3.0, last.y + (q.y - last.y) * 2.0 / 3.0);
                    c2 = pc_point_make(p.x + (q.x - p.x) * 2.0 / 3.0, p.y + (q.y - p.y) * 2.0 / 3.0);
                }
                cur.knots[cur.n - 1].out_ctrl = c1;
                pc_knot k = {p, c2, p, false};
                if (!knots_push(&cur, k)) {
                    ok = false;
                    errmsg = "out of memory";
                    break;
                }
            }
            if (!ok) {
                break;
            }
        } else if (strcmp(t, "Z") == 0) {
            if (!finish_sub(&cur, &pb, true)) {
                ok = false;
                errmsg = "out of memory";
                break;
            }
        } else if (strcmp(t, "O") == 0) {
            if (!finish_sub(&cur, &pb, true)) {
                ok = false;
                errmsg = "out of memory";
                break;
            }
            double cx, cy, r;
            if (!tok_num(toks, ntok, &i, &cx) || !tok_num(toks, ntok, &i, &cy) || !tok_num(toks, ntok, &i, &r)) {
                ok = false;
                errmsg = "expected a number";
                break;
            }
            if (!push_circle(&pb, cx, cy, r, op)) {
                ok = false;
                errmsg = "out of memory";
                break;
            }
            op = PC_PATH_COMBINE;
        } else {
            ok = false;
            (void)snprintf(msg, 256, "unknown command `%s`", t);
            errmsg = NULL; /* already set msg */
            goto cleanup;
        }
    }
    if (ok) {
        if (!finish_sub(&cur, &pb, true)) {
            ok = false;
            errmsg = "out of memory";
        }
    }
    if (ok && pb.n == 0) {
        ok = false;
        errmsg = "empty path";
    }
    if (ok) {
        out->subpaths = pb.subs;
        out->nsubpaths = pb.n;
        out->fill_rule = PC_FILL_RULE_NONZERO;
        out->inverted = false;
    }
cleanup:
    if (!ok) {
        if (errmsg != NULL) {
            (void)snprintf(msg, 256, "%s", errmsg);
        }
        free(cur.knots);
        for (size_t s = 0; s < pb.n; s++) {
            pc_subpath_fini(&pb.subs[s]);
        }
        free(pb.subs);
    }
    free(toks);
    free(buf);
    return ok;
}

/* ---- fit ---------------------------------------------------------------------------------- */

pc_status pc_shape_fit(const pc_path *path, const double rect[4], bool keep, pc_path *out, pc_error *err)
{
    double b[4];
    if (!pc_path_control_bounds(path, b)) {
        return pc_path_clone(path, out, err);
    }
    double bw = (b[2] - b[0]) < 1e-9 ? 1e-9 : (b[2] - b[0]);
    double bh = (b[3] - b[1]) < 1e-9 ? 1e-9 : (b[3] - b[1]);
    double sx = rect[2] / bw, sy = rect[3] / bh;
    double ox = rect[0], oy = rect[1];
    if (keep) {
        double k = sx < sy ? sx : sy;
        ox += (rect[2] - bw * k) / 2.0;
        oy += (rect[3] - bh * k) / 2.0;
        sx = k;
        sy = k;
    }
    pc_affine a = {{sx, 0.0, 0.0, sy, ox - b[0] * sx, oy - b[1] * sy}};
    return pc_path_transform(path, a, out, err);
}

/* ---- generated shapes --------------------------------------------------------------------- */

/* Append printf output to a growable string at *buf (size *cap, length *len). */
static bool sb_add(char **buf, size_t *cap, size_t *len, const char *fmt, ...)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        va_list ap;
        va_start(ap, fmt);
        int need = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
        va_end(ap);
        if (need < 0) {
            return false;
        }
        if ((size_t)need < *cap - *len) {
            *len += (size_t)need;
            return true;
        }
        size_t nc = *cap * 2;
        while (nc < *len + (size_t)need + 1) {
            nc *= 2;
        }
        char *nb = realloc(*buf, nc);
        if (nb == NULL) {
            return false;
        }
        *buf = nb;
        *cap = nc;
    }
    return false;
}

static char *gen_star(size_t n, double ratio, double rot)
{
    size_t cap = 256, len = 0;
    char *s = malloc(cap);
    if (s == NULL) {
        return NULL;
    }
    s[0] = '\0';
    for (size_t k = 0; k < 2 * n; k++) {
        double r = (k % 2 == 0) ? 50.0 : 50.0 * ratio;
        double a = rot + M_PI * (double)k / (double)n - FRAC_PI_2;
        if (!sb_add(&s, &cap, &len, "%s %.3f %.3f ", k == 0 ? "M" : "L", 50.0 + r * cos(a), 50.0 + r * sin(a))) {
            free(s);
            return NULL;
        }
    }
    if (!sb_add(&s, &cap, &len, "Z")) {
        free(s);
        return NULL;
    }
    return s;
}

static char *gen_sun(void)
{
    size_t cap = 512, len = 0;
    char *s = malloc(cap);
    if (s == NULL) {
        return NULL;
    }
    s[0] = '\0';
    if (!sb_add(&s, &cap, &len, "O 50 50 22 ")) {
        free(s);
        return NULL;
    }
    for (int k = 0; k < 12; k++) {
        double a = TAU * (double)k / 12.0;
        double b1x = 50.0 + 28.0 * cos(a - 0.12), b1y = 50.0 + 28.0 * sin(a - 0.12);
        double b2x = 50.0 + 28.0 * cos(a + 0.12), b2y = 50.0 + 28.0 * sin(a + 0.12);
        double tx = 50.0 + 50.0 * cos(a), ty = 50.0 + 50.0 * sin(a);
        if (!sb_add(&s, &cap, &len, "M %.3f %.3f L %.3f %.3f L %.3f %.3f Z ", b1x, b1y, tx, ty, b2x, b2y)) {
            free(s);
            return NULL;
        }
    }
    return s;
}

static char *gen_flower(void)
{
    size_t cap = 256, len = 0;
    char *s = malloc(cap);
    if (s == NULL) {
        return NULL;
    }
    s[0] = '\0';
    for (int k = 0; k < 6; k++) {
        double a = TAU * (double)k / 6.0 - FRAC_PI_2;
        if (!sb_add(&s, &cap, &len, "O %.3f %.3f 18 ", 50.0 + 30.0 * cos(a), 50.0 + 30.0 * sin(a))) {
            free(s);
            return NULL;
        }
    }
    if (!sb_add(&s, &cap, &len, "O 50 50 20 ! O 50 50 9")) {
        free(s);
        return NULL;
    }
    return s;
}

static char *gen_shout(void)
{
    size_t cap = 512, len = 0;
    char *s = malloc(cap);
    if (s == NULL) {
        return NULL;
    }
    s[0] = '\0';
    int n = 14;
    for (int k = 0; k < 2 * n; k++) {
        double r = (k % 2 == 0) ? 1.0 : 0.78;
        double a = M_PI * (double)k / (double)n;
        if (!sb_add(&s, &cap, &len, "%s %.3f %.3f ", k == 0 ? "M" : "L", 50.0 + 50.0 * r * cos(a),
                    40.0 + 40.0 * r * sin(a))) {
            free(s);
            return NULL;
        }
    }
    if (!sb_add(&s, &cap, &len, "Z M 30 70 L 14 100 L 46 74 Z")) {
        free(s);
        return NULL;
    }
    return s;
}

static pc_status add_shape(pc_preset_groups *g, size_t gi, const char *name, const char *src, pc_error *err)
{
    char msg[256];
    pc_shape_preset sp;
    memset(&sp, 0, sizeof sp);
    if (!pc_shape_parse_path(src, &sp.path, msg)) {
        return PC_OK; /* Rust filter_map: unparseable built-ins are silently skipped */
    }
    sp.name = dup_str(name);
    if (sp.name == NULL) {
        pc_shape_preset_fini(&sp);
        return pc_fail(err, PC_ERR_NOMEM, "shape");
    }
    if (pc_preset_group_push(g, gi, &sp, pc_shape_preset_ops(), err) != PC_OK) {
        pc_shape_preset_fini(&sp);
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

static pc_status add_gen(pc_preset_groups *g, size_t gi, const char *name, char *src, pc_error *err)
{
    if (src == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "shape");
    }
    pc_status rc = add_shape(g, gi, name, src, err);
    free(src);
    return rc;
}

pc_status pc_shapes_builtin(pc_preset_groups *out, pc_error *err)
{
    pc_error e = {0};
    pc_preset_groups_init(out);
#define ADD(name, src) do { if (add_shape(out, out->n - 1, name, src, &e) != PC_OK) goto oom; } while (0)
#define ADDG(name, gen) do { if (add_gen(out, out->n - 1, name, gen, &e) != PC_OK) goto oom; } while (0)

    if (pc_preset_groups_push(out, "Symbols", &e) == NULL) {
        goto oom;
    }
    ADD("Heart", "M 50 92 C 20 70 0 52 0 30 C 0 12 14 0 28 0 C 40 0 47 8 50 16 C 53 8 60 0 72 0 C 86 0 100 12 100 30 C 100 52 80 70 50 92 Z");
    ADDG("Star", gen_star(5, 0.4, 0.0));
    ADDG("Six-Point Star", gen_star(6, 0.55, 0.0));
    ADDG("Burst", gen_star(16, 0.72, 0.0));
    ADD("Check Mark", "M 5 55 L 20 40 L 38 58 L 82 12 L 97 27 L 38 88 Z");
    ADD("Cross", "M 20 5 L 50 35 L 80 5 L 95 20 L 65 50 L 95 80 L 80 95 L 50 65 L 20 95 L 5 80 L 35 50 L 5 20 Z");
    ADD("Plus", "M 38 0 L 62 0 L 62 38 L 100 38 L 100 62 L 62 62 L 62 100 L 38 100 L 38 62 L 0 62 L 0 38 L 38 38 Z");
    ADD("Ring", "O 50 50 50 ! O 50 50 32");
    ADD("Lightning", "M 58 0 L 18 56 L 44 56 L 34 100 L 82 38 L 55 38 L 70 0 Z");
    ADD("Diamond", "M 50 0 L 90 50 L 50 100 L 10 50 Z");

    if (pc_preset_groups_push(out, "Arrows", &e) == NULL) {
        goto oom;
    }
    ADD("Arrow Right", "M 0 35 L 55 35 L 55 15 L 100 50 L 55 85 L 55 65 L 0 65 Z");
    ADD("Arrow Left", "M 100 35 L 45 35 L 45 15 L 0 50 L 45 85 L 45 65 L 100 65 Z");
    ADD("Arrow Up", "M 35 100 L 35 45 L 15 45 L 50 0 L 85 45 L 65 45 L 65 100 Z");
    ADD("Arrow Down", "M 35 0 L 35 55 L 15 55 L 50 100 L 85 55 L 65 55 L 65 0 Z");
    ADD("Double Arrow", "M 0 50 L 25 20 L 25 38 L 75 38 L 75 20 L 100 50 L 75 80 L 75 62 L 25 62 L 25 80 Z");
    ADD("Chevron", "M 10 0 L 45 0 L 95 50 L 45 100 L 10 100 L 60 50 Z");
    ADD("Curved Arrow", "M 8 92 C 8 45 35 22 68 22 L 68 4 L 98 34 L 68 64 L 68 44 C 48 44 30 58 30 92 Z");

    if (pc_preset_groups_push(out, "Speech Bubbles", &e) == NULL) {
        goto oom;
    }
    ADD("Speech Bubble",
        "M 15 5 L 85 5 C 93 5 100 12 100 20 L 100 60 C 100 68 93 75 85 75 L 45 75 L 20 98 L 25 75 L 15 75 C 7 75 0 68 0 60 L 0 20 C 0 12 7 5 15 5 Z");
    ADD("Round Bubble",
        "M 50 5 C 78 5 100 22 100 42 C 100 62 78 79 50 79 C 44 79 38 78 33 77 L 12 95 L 18 72 C 7 65 0 54 0 42 C 0 22 22 5 50 5 Z");
    ADD("Thought Bubble", "M 50 5 C 78 5 100 20 100 38 C 100 56 78 71 50 71 C 22 71 0 56 0 38 C 0 20 22 5 50 5 Z O 22 82 8 O 9 95 5");
    ADDG("Shout Bubble", gen_shout());

    if (pc_preset_groups_push(out, "Nature", &e) == NULL) {
        goto oom;
    }
    ADD("Leaf",
        "M 50 0 C 80 20 95 50 80 75 C 72 88 60 92 53 92 L 53 100 L 47 100 L 47 92 C 40 92 28 88 20 75 C 5 50 20 20 50 0 Z ! M 48.5 22 L 51.5 22 L 51.5 88 L 48.5 88 Z");
    ADDG("Sun", gen_sun());
    ADD("Crescent Moon", "O 45 50 45 ! O 66 40 38");
    ADD("Cloud",
        "M 25 80 C 10 80 0 70 0 58 C 0 46 10 37 22 37 C 24 20 38 10 54 10 C 70 10 82 20 85 34 C 95 35 100 45 100 57 C 100 70 90 80 78 80 Z");
    ADD("Raindrop", "M 50 0 C 60 25 85 45 85 68 C 85 87 69 100 50 100 C 31 100 15 87 15 68 C 15 45 40 25 50 0 Z");
    ADDG("Flower", gen_flower());
    ADD("Tree", "M 50 0 L 85 45 L 68 45 L 92 75 L 56 75 L 56 100 L 44 100 L 44 75 L 8 75 L 32 45 L 15 45 Z");

    if (pc_preset_groups_push(out, "Animals", &e) == NULL) {
        goto oom;
    }
    ADD("Fish", "M 0 50 C 15 25 45 15 70 32 L 96 12 L 88 50 L 96 88 L 70 68 C 45 85 15 75 0 50 Z ! O 20 45 4");
    ADD("Cat", "M 15 10 L 35 30 C 45 27 55 27 65 30 L 85 10 L 88 50 C 92 75 72 95 50 95 C 28 95 8 75 12 50 Z ! O 36 58 6 ! O 64 58 6");
    ADD("Bird", "M 0 40 C 15 30 35 32 50 50 C 65 32 85 30 100 40 C 85 40 65 48 50 70 C 35 48 15 40 0 40 Z");
    ADD("Butterfly",
        "M 50 30 C 40 5 5 0 5 25 C 5 45 30 50 45 50 C 25 55 10 75 25 90 C 38 100 48 80 50 65 C 52 80 62 100 75 90 C 90 75 75 55 55 50 C 70 50 95 45 95 25 C 95 0 60 5 50 30 Z");
    ADD("Paw Print",
        "M 50 50 C 68 50 82 66 82 80 C 82 92 70 96 60 94 C 55 93 52 91 50 91 C 48 91 45 93 40 94 C 30 96 18 92 18 80 C 18 66 32 50 50 50 Z O 18 42 10 O 37 22 11 O 63 22 11 O 82 42 10");
    ADD("Rabbit", "O 50 72 26 M 34 52 C 22 30 24 2 34 2 C 44 2 46 30 44 50 Z M 66 52 C 78 30 76 2 66 2 C 56 2 54 30 56 50 Z");
#undef ADD
#undef ADDG
    return PC_OK;
oom:
    pc_preset_groups_fini(out, pc_shape_preset_ops());
    pc_preset_groups_init(out);
    if (err != NULL) {
        *err = e;
    }
    return PC_ERR_NOMEM;
}
