/* Parity with the reference for chunk I5 (tests/golden/io-extra-vectors.txt, written by
 * `rustref io-extra-vectors`): Camera Raw filter descriptors both ways, Affinity imports
 * (synthetic documents, their mutations and truncations, the public corpus when fetched) and SVG
 * imports. Shape geometry, fills, strokes, text runs, masks and every pixel of non-SVG documents
 * must match exactly; so must SVG pixel layers and the SVG rasteriser (our port of resvg's rendering
 * and of tiny-skia's scan converters), except for the cases listed in near_allowed(), which are
 * compared with a tolerance and counted separately. */
#define GOLDEN "tests/golden/io-extra-vectors.txt"
#include "harness.h"
#include "io/golden.h"

#include "doc/json.h"
#include "io/camera_raw_map.h"
#include "io/smart_map.h"
#include "io/svg_text.h"
#include "psd/descriptor.h"

static size_t n_exact;
static size_t n_near;
static size_t n_msg;
static size_t n_known;
static double worst_mean;

/* Drawings that differ by design (REWRITE_PLAN.md section 7). None since filter effects are
 * rendered (I5b): the filter cases compare exactly, or within the usual pixel tolerance where the
 * filter input has anti-aliased edges (the renderer's coverage differs slightly there). */
static bool known_difference(const char *name)
{
    (void)name;
    return false;
}

/* Text cases name fonts installed where the golden was made (the reference shapes text with the
 * system's fonts): a case is skipped when one of its families is missing here, and the cases that
 * rely on the generic families (fontconfig aliases) only run where those resolve to nothing, as on
 * the golden machine. */
static bool text_fonts_missing(const char *name)
{
    static const struct {
        const char *name;
        const char *fams; /* '|'-separated; "!generic": the generic families match no font */
    } T[] = {
        {"text", "Times New Roman"},
        {"text-fonts", "DejaVu Sans|Liberation Serif|Courier New|Georgia|Arial"},
        {"text-anchor", "DejaVu Serif"},
        {"text-tspans", "DejaVu Sans|Liberation Sans|Liberation Serif"},
        {"text-positions", "DejaVu Sans|DejaVu Serif"},
        {"text-spacing", "Verdana"},
        {"text-baseline", "DejaVu Sans"},
        {"text-dominant", "Liberation Sans"},
        {"text-decoration", "DejaVu Serif"},
        {"text-path", "DejaVu Sans"},
        {"text-path-misc", "Liberation Sans"},
        {"text-space", "DejaVu Sans Mono"},
        {"text-stroke", "DejaVu Sans"},
        {"text-opacity", "DejaVu Sans"},
        {"text-gradient", "DejaVu Sans"},
        {"text-clip", "DejaVu Sans"},
        {"text-mask", "Liberation Serif"},
        {"text-transform", "DejaVu Sans|DejaVu Serif"},
        {"text-vertical", "DejaVu Sans"},
        {"text-empty", "DejaVu Sans"},
        {"text-names", "DejaVu Sans"},
        {"text-weights", "DejaVu Sans|Liberation Mono"},
        {"text-length", "Liberation Serif"},
        {"text-tref", "DejaVu Sans"},
        {"text-shorthand", "DejaVu Serif|DejaVu Sans|Times New Roman"},
        {"text-kerning", "Liberation Serif"},
        {"text-ligature", "DejaVu Serif|DejaVu Sans"},
        {"text-units", "DejaVu Sans"},
        {"text-bidi", "DejaVu Sans"},
        {"text-marks", "DejaVu Sans|DejaVu Serif"},
        {"text-default-font", "Times New Roman|Arial"},
        {"text-unknown-font", "Verdana|!generic"},
        {"text-generic", "!generic"},
        {"text-in-use", "DejaVu Sans"},
        {"text-in-pattern", "DejaVu Sans"},
    };
    const svf_db *db;
    size_t i;
    const char *no = getenv("PHOTOCRAFT_NO_SYSTEM_FONTS");
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        const char *p;
        if (strcmp(T[i].name, name) != 0) {
            continue;
        }
        if (no != NULL && no[0] != '\0') {
            return true;
        }
        db = svf_db_get();
        if (db == NULL) {
            return true;
        }
        for (p = T[i].fams; *p != '\0';) {
            const char *e = strchr(p, '|');
            size_t n = e != NULL ? (size_t)(e - p) : strlen(p);
            char fam[64];
            if (n >= sizeof fam) {
                return true;
            }
            memcpy(fam, p, n);
            fam[n] = '\0';
            if (strcmp(fam, "!generic") == 0) {
                static const svf_kind G[5] = {SVF_SERIF, SVF_SANS, SVF_CURSIVE, SVF_FANTASY, SVF_MONO};
                size_t k;
                for (k = 0; k < 5; k++) {
                    svf_family f = {G[k], NULL};
                    if (svf_db_query(db, &f, 1, 400, 5, SVF_STYLE_NORMAL) != SIZE_MAX) {
                        return true;
                    }
                }
            } else {
                svf_family f = {SVF_NAMED, fam};
                if (svf_db_query(db, &f, 1, 400, 5, SVF_STYLE_NORMAL) == SIZE_MAX) {
                    return true;
                }
            }
            p = e != NULL ? e + 1 : p + n;
        }
        return false;
    }
    return false;
}
static unsigned worst_max;

/* ---------------------------------------------------------------- JSON helpers */

static void put_json(pc_buf *b, yyjson_mut_doc *d, yyjson_mut_val *v)
{
    if (v == NULL) {
        abort();
    }
    must(pc_json_write(v, 0, b, NULL));
    yyjson_mut_doc_free(d);
}

static void jstr(pc_buf *b, const char *s, size_t n)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    put_json(b, d, yyjson_mut_strncpy(d, s, n));
}

#define JPUT(b, T, ptr)                                                                                      \
    do {                                                                                                     \
        yyjson_mut_doc *jd_ = yyjson_mut_doc_new(NULL);                                                      \
        put_json((b), jd_, T##_to_json(jd_, (ptr), 0));                                                      \
    } while (0)

#define JOPT(b, T, has, ptr)                                                                                 \
    do {                                                                                                     \
        if (has) {                                                                                           \
            JPUT(b, T, ptr);                                                                                 \
        } else {                                                                                             \
            sb((b), "null");                                                                                 \
        }                                                                                                    \
    } while (0)

#define JARR(b, T, arr, n)                                                                                   \
    do {                                                                                                     \
        yyjson_mut_doc *jd_ = yyjson_mut_doc_new(NULL);                                                      \
        yyjson_mut_val *a_ = yyjson_mut_arr(jd_);                                                            \
        for (size_t i_ = 0; i_ < (n); i_++) {                                                                \
            if (!yyjson_mut_arr_append(a_, T##_to_json(jd_, &(arr)[i_], 0))) {                               \
                abort();                                                                                     \
            }                                                                                                \
        }                                                                                                    \
        put_json((b), jd_, a_);                                                                              \
    } while (0)

static void jwarn(pc_buf *b, const pc_io_warnings *w)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    for (size_t i = 0; i < w->n; i++) {
        if (!yyjson_mut_arr_add_strcpy(d, a, w->v[i])) {
            abort();
        }
    }
    put_json(b, d, a);
}

/* ---------------------------------------------------------------- dumps (as the rustref module) */

static void x_surf(pc_buf *b, const pc_surface *s, bool pixels)
{
    if (s == NULL) {
        sb(b, "-");
    } else if (pixels) {
        s_surf(b, s, false);
    } else {
        sb(b, "px");
    }
}

static void x_layer(pc_buf *b, const pc_layer *l, bool pixels)
{
    sb(b, "(");
    jstr(b, pc_str_c(&l->name), l->name.len);
    sb(b, "|%d|", l->visible ? 1 : 0);
    JPUT(b, pc_locks, &l->locks);
    sb(b, "|");
    JPUT(b, pc_blend_mode, &l->blend);
    sb(b, "|%08x|%08x|%d", fb(l->opacity), fb(l->fill_opacity), l->clipped ? 1 : 0);
    if (l->has_mask) {
        sb(b, "|m");
        x_surf(b, &l->mask.surface, pixels);
        sb(b, ":%d%d:%08x:%08x", l->mask.enabled, l->mask.linked, fb(l->mask.density), fb(l->mask.feather));
    } else {
        sb(b, "|-");
    }
    sb(b, "|");
    JOPT(b, pc_vector_mask, l->has_vector_mask, &l->vector_mask);
    sb(b, "|");
    switch (l->content.kind) {
    case PC_LAYER_GROUP: {
        const pc_group *g = &l->content.u.group;
        sb(b, "G%d:", g->expanded ? 1 : 0);
        JOPT(b, pc_artboard, g->has_artboard, &g->artboard);
        sb(b, "[");
        for (size_t i = 0; i < g->nchildren; i++) {
            x_layer(b, &g->children[i], pixels);
        }
        sb(b, "]");
        break;
    }
    case PC_LAYER_SHAPE: {
        const pc_shape_layer *s = &l->content.u.shape;
        sb(b, "S");
        JPUT(b, pc_path, &s->path);
        sb(b, "|");
        JOPT(b, pc_fill, s->has_fill, &s->fill);
        sb(b, "|");
        JOPT(b, pc_shape_stroke, s->has_stroke, &s->stroke);
        sb(b, "|");
        x_surf(b, s->has_cache ? &s->cache : NULL, true);
        break;
    }
    case PC_LAYER_TEXT: {
        const pc_text_layer *t = &l->content.u.text;
        sb(b, "T");
        jstr(b, pc_str_c(&t->text), t->text.len);
        sb(b, "|");
        jstr(b, pc_str_c(&t->font_family), t->font_family.len);
        sb(b, "|%08x|", fb(t->size_pt));
        JPUT(b, pc_color, &t->color);
        sb(b, "|");
        JPUT(b, pc_affine, &t->transform);
        sb(b, "|");
        JARR(b, pc_text_run, t->runs, t->nruns);
        sb(b, "|");
        JARR(b, pc_paragraph_run, t->paragraphs, t->nparagraphs);
        sb(b, "|");
        JPUT(b, pc_text_shape, &t->shape);
        sb(b, "|");
        x_surf(b, t->has_cache ? &t->cache : NULL, pixels);
        break;
    }
    case PC_LAYER_SMART: {
        const pc_smart_object *sm = &l->content.u.smart;
        char h[17];
        if (sm->source.linked) {
            sb(b, "OL");
            jstr(b, pc_str_c(&sm->source.path), sm->source.path.len);
        } else {
            sb(b, "OE");
            jstr(b, pc_str_c(&sm->source.file_name), sm->source.file_name.len);
            h16(pc_blob_len(sm->source.bytes) > 0 ? pc_blob_data(sm->source.bytes) : (const uint8_t *)"",
                pc_blob_len(sm->source.bytes), h);
            sb(b, ":%zu:%s", pc_blob_len(sm->source.bytes), h);
        }
        sb(b, "|");
        JPUT(b, pc_affine, &sm->transform);
        sb(b, "|");
        JARR(b, pc_smart_filter, sm->smart_filters, sm->nsmart_filters);
        sb(b, "|");
        x_surf(b, sm->has_cache ? &sm->cache : NULL, pixels);
        break;
    }
    case PC_LAYER_RASTER:
        sb(b, "R");
        x_surf(b, &l->content.u.raster, pixels);
        break;
    default:
        sb(b, "?");
        break;
    }
    sb(b, ")");
}

static const char *rust_kind(pc_io_err_kind k)
{
    static const char *const N[] = {"Psd", "Codec", "UnknownFormat", "Unsupported", "Pcraft",
                                    "Raw", "Svg",   "Cancelled",     "Other"};
    return N[k];
}

static void x_imp(pc_buf *b, pc_status st, const pc_io_import_result *r, const pc_error *e, bool pixels)
{
    const pc_document *d;
    if (st != PC_OK) {
        sb(b, "err %s ", rust_kind(pc_io_error_kind(st, e)));
        jstr(b, e->msg, strlen(e->msg));
        return;
    }
    d = &r->document;
    sb(b, "ok ");
    jstr(b, pc_str_c(&d->name), d->name.len);
    sb(b, ":%ux%u:%08x:", d->size.width, d->size.height, fb(d->resolution_dpi));
    JPUT(b, pc_color_mode, &d->mode);
    sb(b, ":");
    JPUT(b, pc_sample_type, &d->depth);
    sb(b, ":");
    for (size_t i = 0; i < d->nlayers; i++) {
        x_layer(b, &d->layers[i], pixels);
    }
    sb(b, " ");
    jwarn(b, &r->warnings);
    sb(b, " %d %d", r->source_read_only, r->preview_only);
}

static char *import_dump(const char *name, const uint8_t *bytes, size_t len, bool pixels)
{
    pc_io_import_result r;
    pc_error e;
    pc_buf b = PC_BUF_INIT;
    pc_status st = pc_io_import(name, bytes, len, &r, &e);
    x_imp(&b, st, &r, &e, pixels);
    if (st == PC_OK) {
        pc_io_import_result_fini(&r);
    }
    return (char *)b.data;
}

/* Exact, or both errors of one kind whose messages differ (counted as message differences). */
static void compare(const char *what, const char *want, const char *got)
{
    g_checked++;
    pc_t_checks++;
    if (strcmp(want, got) == 0) {
        n_exact++;
        return;
    }
    if (starts(want, "err ") && starts(got, "err ")) {
        const char *ws = strchr(want + 4, ' ');
        const char *gs = strchr(got + 4, ' ');
        if (ws != NULL && gs != NULL && ws - want == gs - got &&
            memcmp(want, got, (size_t)(ws - want)) == 0) {
            n_msg++;
            if (getenv("PC_PARITY_VERBOSE") != NULL) {
                (void)fprintf(stderr, "%s: wording\n  want %.300s\n  got  %.300s\n", what, want, got);
            }
            return;
        }
    }
    {
        size_t i = 0;
        while (want[i] != '\0' && want[i] == got[i]) {
            i++;
        }
        PC_T_FAIL("%s: differ at byte %zu:\n  want ...%.300s\n  got  ...%.300s", what, i,
                  want + (i > 40 ? i - 40 : 0), got + (i > 40 ? i - 40 : 0));
    }
}

/* ---------------------------------------------------------------- Camera Raw */

static uint8_t *unhex(const char *s, size_t *n)
{
    size_t len = strlen(s) / 2;
    uint8_t *b = xmalloc(len + 1);
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) {
            abort();
        }
        b[i] = (uint8_t)v;
    }
    *n = len;
    return b;
}

static void desc_line(pc_buf *b, pc_status st, const pc_psd_desc *d, const pc_error *e, bool with_hex)
{
    pc_psd_bytes by;
    char h[17];
    if (st != PC_OK) {
        sb(b, "err ");
        jstr(b, e->msg, strlen(e->msg));
        return;
    }
    must(pc_psd_desc_to_bytes(d, &by, NULL));
    h16(by.data, by.len, h);
    sb(b, "ok %zu %s", by.len, h);
    if (with_hex) {
        sb(b, " ");
        for (size_t i = 0; i < by.len; i++) {
            sb(b, "%02x", by.data[i]);
        }
    }
    free(by.data);
}

static void test_camera_raw(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if (is_kind(l, "cri") && l->n >= 5) {
            size_t n;
            uint8_t *bytes = unhex(l->f[2], &n);
            pc_psd_desc item;
            pc_smart_filter f;
            pc_psd_desc back;
            pc_error e;
            pc_buf got = PC_BUF_INIT;
            pc_status st;
            char what[128];
            must(pc_psd_desc_parse(bytes, n, &item, NULL));
            must(pc_io_filter_from_item(&item, &f, NULL));
            JPUT(&got, pc_smart_filter, &f);
            (void)snprintf(what, sizeof what, "cri %s", l->f[1]);
            compare(what, l->f[3], (const char *)got.data);
            pc_buf_fini(&got);
            st = pc_io_item_for_filter(&f, &back, &e);
            sb(&got, "rt ");
            if (st == PC_OK && pc_psd_desc_eq(&back, &item)) {
                sb(&got, "=");
            } else {
                desc_line(&got, st, &back, &e, false);
            }
            (void)snprintf(what, sizeof what, "cri-rt %s", l->f[1]);
            compare(what, l->f[4], (const char *)got.data);
            if (st == PC_OK) {
                pc_psd_desc_fini(&back);
            }
            pc_buf_fini(&got);
            pc_smart_filter_fini(&f);
            pc_psd_desc_fini(&item);
            free(bytes);
        } else if (is_kind(l, "cre") && l->n >= 4) {
            yyjson_doc *jd;
            pc_smart_filter f;
            pc_psd_desc out;
            pc_error e;
            pc_buf got = PC_BUF_INIT;
            pc_status st;
            char what[128];
            memset(&f, 0, sizeof f);
            must(pc_json_parse(l->f[2], strlen(l->f[2]), &jd, NULL));
            must(pc_json_value_from_val(yyjson_doc_get_root(jd), &f.params, NULL));
            yyjson_doc_free(jd);
            must(pc_str_set_c(&f.command, PC_IO_CAMERA_RAW_COMMAND, NULL));
            f.blend = PC_BLEND_NORMAL;
            f.opacity = 1.0f;
            f.visible = true;
            st = pc_io_item_for_filter(&f, &out, &e);
            desc_line(&got, st, &out, &e, true);
            (void)snprintf(what, sizeof what, "cre %s", l->f[1]);
            compare(what, l->f[3], (const char *)got.data);
            if (st == PC_OK) {
                pc_psd_desc_fini(&out);
            }
            pc_buf_fini(&got);
            pc_smart_filter_fini(&f);
        }
    }
}

/* ---------------------------------------------------------------- Affinity */

static void test_affinity(void)
{
    pc_buf ring = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        char what[256];
        if (is_kind(l, "af") && l->n >= 4) {
            pc_buf bytes = b64d(l->f[2]);
            char *got = import_dump(l->f[1], bytes.data, bytes.len, true);
            (void)snprintf(what, sizeof what, "af %s", l->f[1]);
            compare(what, l->f[3], got);
            free(got);
            if (strcmp(l->f[1], "ring.af") == 0) {
                ring = bytes;
            } else {
                pc_buf_fini(&bytes);
            }
        } else if (is_kind(l, "afm") && l->n >= 4 && ring.data != NULL) {
            uint8_t *b = xmalloc(ring.len);
            const char *p = l->f[2];
            char *got;
            memcpy(b, ring.data, ring.len);
            while (*p != '\0') {
                unsigned long at;
                unsigned v;
                char *end;
                at = strtoul(p, &end, 10);
                p = end + 1;
                v = (unsigned)strtoul(p, &end, 10);
                p = *end == ',' ? end + 1 : end;
                b[at] = (uint8_t)v;
            }
            got = import_dump("mutated.af", b, ring.len, true);
            (void)snprintf(what, sizeof what, "afm %s", l->f[1]);
            compare(what, l->f[3], got);
            free(got);
            free(b);
        } else if (is_kind(l, "aft") && l->n >= 3 && ring.data != NULL) {
            size_t end = (size_t)strtoul(l->f[1], NULL, 10);
            char *got = import_dump("cut.af", ring.data, end, true);
            (void)snprintf(what, sizeof what, "aft %s", l->f[1]);
            compare(what, l->f[2], got);
            free(got);
        } else if (is_kind(l, "afc") && l->n >= 4) {
            pc_buf bytes = PC_BUF_INIT;
            const char *name = strrchr(l->f[1], '/');
            char *got;
            char h[17];
            if (pc_file_read(l->f[1], (size_t)1 << 30, &bytes, NULL) != PC_OK) {
                g_skipped++;
                continue;
            }
            got = import_dump(name != NULL ? name + 1 : l->f[1], bytes.data, bytes.len, true);
            h16(got, strlen(got), h);
            (void)snprintf(what, sizeof what, "afc %s", l->f[1]);
            compare(what, l->f[2], h);
            free(got);
            pc_buf_fini(&bytes);
        }
    }
    pc_buf_fini(&ring);
}

/* ---------------------------------------------------------------- SVG */

/* SVG pictures allowed to differ from the reference, each with its reason (REWRITE_PLAN.md
 * section 7); matched against the start of "svgpx NAME" / "svgr NAME BITS". Path coverage is a
 * bit-exact port of tiny-skia's, so nothing is excused for anti-aliasing. None at present. */
static bool near_allowed(const char *what)
{
    static const char *const ALLOWED[] = {""};
    size_t i;
    for (i = 0; i < sizeof ALLOWED / sizeof ALLOWED[0]; i++) {
        if (ALLOWED[i][0] != '\0' && strncmp(what, ALLOWED[i], strlen(ALLOWED[i])) == 0) {
            return true;
        }
    }
    return false;
}

/* RGBA8 pictures that differ: false unless near_allowed() excuses the case and the difference
 * stays within the tolerance. */
static bool near_rgba8(const char *what, const uint8_t *want, const uint8_t *got, size_t n)
{
    unsigned mx = 0;
    double sum = 0.0;
    size_t big = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned d = (unsigned)abs((int)want[i] - (int)got[i]);
        mx = d > mx ? d : mx;
        sum += d;
        if (d > 64) {
            big++;
        }
    }
    {
        double mean = n > 0 ? sum / (double)n : 0.0;
        worst_mean = mean > worst_mean ? mean : worst_mean;
        if (getenv("PC_PARITY_VERBOSE") != NULL) {
            (void)fprintf(stderr, "%s: near: mean %.2f max %u\n", what, mean, mx);
        }
        worst_max = mx > worst_max ? mx : worst_max;
        if (!near_allowed(what)) {
            (void)fprintf(stderr, "%s: not exact (mean %.2f max %u)\n", what, mean, mx);
            return false;
        }
        /* Pictures of a few pixels are all edge: anti-aliasing alone may differ a lot. */
        if (n > 256 && (mean > 6.0 || (double)big > 0.04 * (double)n)) {
            (void)fprintf(stderr, "%s: mean %.2f max %u, %zu of %zu samples off by > 64\n", what, mean, mx,
                          big, n);
            return false;
        }
    }
    return true;
}

static void test_svg(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        char what[256];
        if ((is_kind(l, "svg") || is_kind(l, "svgpx")) && l->n >= 4 && text_fonts_missing(l->f[1])) {
            g_skipped++;
            continue;
        }
        if (is_kind(l, "svg") && l->n >= 4) {
            pc_buf bytes = b64d(l->f[2]);
            char name[128];
            char *got;
            (void)snprintf(name, sizeof name, "%s.svg", l->f[1]);
            got = import_dump(name, bytes.data, bytes.len, false);
            (void)snprintf(what, sizeof what, "svg %s", l->f[1]);
            if (strcmp(l->f[3], got) == 0) {
                n_exact++;
                g_checked++;
                pc_t_checks++;
            } else if (known_difference(l->f[1])) {
                n_known++;
            } else {
                compare(what, l->f[3], got);
            }
            free(got);
            pc_buf_fini(&bytes);
        } else if (is_kind(l, "svgpx") && l->n >= 5) {
            const gline *src = find_line("svg", l->f[1]);
            pc_buf bytes = b64d(src->f[2]);
            pc_buf want = b64d(l->f[4]);
            pc_io_import_result r;
            pc_compose_buffer fb2;
            pc_rgba8_image img;
            char name[128];
            (void)snprintf(name, sizeof name, "%s.svg", l->f[1]);
            must(pc_io_import(name, bytes.data, bytes.len, &r, NULL));
            must(pc_compose_flatten(&r.document, &fb2, NULL));
            must(pc_compose_buffer_to_rgba8(&fb2, &img, NULL));
            (void)snprintf(what, sizeof what, "svgpx %s", l->f[1]);
            g_checked++;
            pc_t_checks++;
            if (img.width != (uint32_t)atoi(l->f[2]) || img.height != (uint32_t)atoi(l->f[3]) ||
                want.len != (size_t)img.width * img.height * 4) {
                PC_T_FAIL("%s: size %ux%u", what, img.width, img.height);
            } else if (memcmp(want.data, img.pixels, want.len) == 0) {
                n_exact++;
            } else if (near_rgba8(what, want.data, img.pixels, want.len)) {
                n_near++;
            } else if (known_difference(l->f[1])) {
                n_known++;
            } else {
                PC_T_FAIL("%s: pixels past the tolerance", what);
            }
            free(img.pixels);
            free(fb2.px);
            pc_io_import_result_fini(&r);
            pc_buf_fini(&bytes);
            pc_buf_fini(&want);
        } else if (is_kind(l, "svgr") && l->n >= 5) {
            pc_buf bytes = b64d(l->f[2]);
            uint32_t bits = (uint32_t)strtoul(l->f[3], NULL, 16);
            float scale;
            pc_compose_buffer b;
            pc_error e;
            pc_status st;
            memcpy(&scale, &bits, 4);
            st = pc_io_svg_rasterize(bytes.data, bytes.len, scale, &b, &e);
            (void)snprintf(what, sizeof what, "svgr %s %s", l->f[1], l->f[3]);
            g_checked++;
            pc_t_checks++;
            if (starts(l->f[4], "err ")) {
                if (st == PC_OK) {
                    PC_T_FAIL("%s: rendered, want %s", what, l->f[4]);
                    free(b.px);
                } else {
                    pc_buf m = PC_BUF_INIT;
                    sb(&m, "err ");
                    jstr(&m, e.msg, strlen(e.msg));
                    if (strcmp((const char *)m.data, l->f[4]) == 0) {
                        n_exact++;
                    } else {
                        n_msg++;
                    }
                    pc_buf_fini(&m);
                }
            } else if (st != PC_OK) {
                PC_T_FAIL("%s: %s", what, e.msg);
            } else {
                unsigned w = 0;
                unsigned h = 0;
                char px[8] = "";
                if (sscanf(l->f[4], "%u %u %7s", &w, &h, px) < 2 || w != pc_rect_width(b.rect) ||
                    h != pc_rect_height(b.rect)) {
                    PC_T_FAIL("%s: size %ux%u, want %s", what, pc_rect_width(b.rect), pc_rect_height(b.rect),
                              l->f[4]);
                } else if (strcmp(px, "-") == 0) {
                    n_exact++;
                } else {
                    const char *sp = strrchr(l->f[4], ' ');
                    pc_buf want = b64d(sp + 1);
                    pc_rgba8_image img;
                    must(pc_compose_buffer_to_rgba8(&b, &img, NULL));
                    if (memcmp(want.data, img.pixels, want.len) == 0) {
                        n_exact++;
                    } else if (near_rgba8(what, want.data, img.pixels, want.len)) {
                        n_near++;
                    } else {
                        PC_T_FAIL("%s: pixels past the tolerance", what);
                    }
                    free(img.pixels);
                    pc_buf_fini(&want);
                }
                free(b.px);
            }
            pc_buf_fini(&bytes);
        }
    }
}

int main(int argc, char **argv)
{
    if (load_golden() != 0) {
        (void)printf("io-extra-vectors: golden file missing; skipped\n");
        return 0;
    }
    RUN(test_camera_raw);
    RUN(test_affinity);
    RUN(test_svg);
    golden_summary("io-extra-vectors");
    (void)printf("io-extra-vectors: %zu exact, %zu within the pixel tolerance (worst mean %.2f, max %u), %zu "
                 "errors of the same kind with other wording, %zu known differences\n",
                 n_exact, n_near, worst_mean, worst_max, n_msg, n_known);
    return pc_t_finish();
}
