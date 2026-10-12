/* Shared helpers of test_maps_parity.c and test_maps_parity2.c (chunk I4): reading
 * tests/golden/io-maps-vectors.txt and recomputing each line kind's result. */
#ifndef PC_TESTS_IO_MAPS_PARITY_H
#define PC_TESTS_IO_MAPS_PARITY_H

#define GOLDEN "tests/golden/io-maps-vectors.txt"

#include "harness.h"

#include "golden.h"
#include "maps.h"
#include "paint/json.h"
#include "psd/file.h"
#include "psd/patterns.h"
#include "psd/slices.h"

static pc_buf g_res;

static inline const char *res_reset(void)
{
    pc_buf_truncate(&g_res, 0);
    return pc_buf_cstr(&g_res);
}

/* A status from the code under test: pending stubs are counted, anything else fails. */
static inline bool ok_or_pending(pc_status st, const pc_error *e, const char *what)
{
    if (st == PC_OK) {
        return true;
    }
    if (is_pending(st, e)) {
        g_pending++;
        return false;
    }
    PC_T_FAIL("%s: status %d %s", what, (int)st, e->msg);
    return false;
}

static inline pc_buf dec(const char *s)
{
    if (strcmp(s, "-") == 0) {
        pc_buf b = PC_BUF_INIT;
        return b;
    }
    return b64d(s);
}

static inline const uint8_t *bdata(const pc_buf *b)
{
    return b->data != NULL ? b->data : (const uint8_t *)"";
}

static inline float bits_f(const char *hex)
{
    uint32_t u = (uint32_t)strtoul(hex, NULL, 16);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static inline uint32_t u32_of(const char *s)
{
    return (uint32_t)strtoul(s, NULL, 10);
}

/* ---------------------------------------------------------------- vector_map */

static inline pc_status path_res(pc_buf *b, const uint8_t *d, size_t len, uint32_t w, uint32_t h, pc_error *e)
{
    bool found = false;
    pc_path p;
    pc_status st = pc_io_path_from_resource(d, len, w, h, &found, &p, e);
    if (st != PC_OK) {
        return st;
    }
    if (!found) {
        sb(b, "none");
        return PC_OK;
    }
    sb(b, "some ");
    JW(b, pc_path, &p);
    pc_path_fini(&p);
    return PC_OK;
}

static inline pc_status vmsk_res(pc_buf *b, const uint8_t *d, size_t len, uint32_t w, uint32_t h, pc_error *e)
{
    bool found = false;
    pc_path p;
    uint32_t flags = 0;
    pc_vector_mask m;
    pc_status st = pc_io_path_from_vmsk(d, len, w, h, &found, &p, &flags, e);
    if (st != PC_OK) {
        return st;
    }
    if (found) {
        sb(b, "some ");
        JW(b, pc_path, &p);
        sb(b, " %u", flags);
        pc_path_fini(&p);
    } else {
        sb(b, "none");
    }
    sb(b, " | ");
    st = pc_io_vector_mask_from_block(d, len, w, h, &found, &m, e);
    if (st != PC_OK) {
        return st;
    }
    if (found) {
        pc_buf out = PC_BUF_INIT;
        bool matches = false;
        sb(b, "some ");
        JW(b, pc_vector_mask, &m);
        must(pc_io_vector_mask_bytes(&m, w, h, &out, NULL));
        sb(b, " ");
        sb64(b, out.data, out.len);
        must(pc_io_vector_mask_block_matches(d, len, &m, w, h, &matches, NULL));
        sb(b, " %s", matches ? "true" : "false");
        pc_buf_fini(&out);
        pc_vector_mask_fini(&m);
    } else {
        sb(b, "none");
    }
    return PC_OK;
}

static inline pc_status vstk_res(pc_buf *b, const uint8_t *d, size_t len, float dpi, pc_error *e)
{
    bool found = false;
    pc_io_vstk v;
    pc_status st = pc_io_parse_vstk(d, len, dpi, &found, &v, e);
    if (st != PC_OK) {
        return st;
    }
    if (!found) {
        sb(b, "none");
        return PC_OK;
    }
    sb(b, "some ");
    JW_OPT(b, pc_shape_stroke, v.has_stroke, &v.stroke);
    sb(b, " %s", v.fill_enabled ? "true" : "false");
    pc_io_vstk_fini(&v);
    return PC_OK;
}

static inline pc_status vscg_res(pc_buf *b, const uint8_t *d, size_t len, pc_error *e)
{
    bool found = false;
    pc_fill f;
    pc_status st = pc_io_fill_from_vscg(d, len, &found, &f, e);
    if (st != PC_OK) {
        return st;
    }
    if (!found) {
        sb(b, "none");
        return PC_OK;
    }
    sb(b, "some ");
    JW(b, pc_fill, &f);
    pc_fill_fini(&f);
    return PC_OK;
}

static inline pc_status vogk_res(pc_buf *b, const uint8_t *d, size_t len, pc_error *e)
{
    bool found = false;
    pc_live_shape l;
    pc_status st = pc_io_live_from_vogk(d, len, &found, &l, e);
    if (st != PC_OK) {
        return st;
    }
    if (!found) {
        sb(b, "none");
        return PC_OK;
    }
    sb(b, "some ");
    JW(b, pc_live_shape, &l);
    return PC_OK;
}

static inline pc_status art_res(pc_buf *b, const uint8_t *d, size_t len, pc_error *e)
{
    bool found = false;
    pc_artboard a;
    pc_status st = pc_io_parse_artboard(d, len, &found, &a, e);
    if (st != PC_OK) {
        return st;
    }
    if (!found) {
        sb(b, "none");
        return PC_OK;
    }
    sb(b, "some ");
    JW(b, pc_artboard, &a);
    pc_artboard_fini(&a);
    return PC_OK;
}

static inline void check_line(const gline *l, pc_status st, const pc_error *e)
{
    char what[160];
    (void)snprintf(what, sizeof what, "%s %s", l->f[0], l->f[1]);
    if (ok_or_pending(st, e, what)) {
        CHECK_RESULT(what, l->f[l->n - 1], pc_buf_cstr(&g_res));
    }
}

/* key:b64,... -> raw blocks ("-" = none). */
static inline void parse_raw(const char *s, pc_io_raw_blocks *raw)
{
    char *copy;
    char *p;
    if (strcmp(s, "-") == 0) {
        return;
    }
    copy = pc_strndup(s, strlen(s));
    p = copy;
    while (p != NULL && *p != '\0') {
        char *comma = strchr(p, ',');
        pc_buf d;
        if (comma != NULL) {
            *comma = '\0';
        }
        d = b64d(p + 5);
        must(pc_io_raw_blocks_push(raw, (const uint8_t *)p, bdata(&d), d.len, NULL));
        pc_buf_fini(&d);
        p = comma != NULL ? comma + 1 : NULL;
    }
    free(copy);
}

static inline void raw_s(pc_buf *b, const pc_io_raw_blocks *raw)
{
    if (raw->n == 0) {
        sb(b, "-");
        return;
    }
    for (size_t i = 0; i < raw->n; i++) {
        sb(b, "%s%.4s:", i > 0 ? "," : "", (const char *)raw->v[i].key);
        sb64(b, raw->v[i].data, raw->v[i].len);
    }
}

typedef struct lookup_ctx {
    const pc_io_raw_blocks *raw;
} lookup_ctx;

static inline const uint8_t *lookup(void *ctx, const uint8_t key[4], size_t *len)
{
    const pc_io_raw_block *b = pc_io_raw_blocks_find(((lookup_ctx *)ctx)->raw, key);
    if (b == NULL) {
        return NULL;
    }
    *len = b->len;
    return b->data != NULL ? b->data : (const uint8_t *)"";
}

/* The SHAPE JSON object of a shape line. */
static inline void shape_from_json(const char *text, pc_shape_layer *sh)
{
    yyjson_doc *d = jparse(text);
    yyjson_val *root = yyjson_doc_get_root(d);
    yyjson_val *v;
    memset(sh, 0, sizeof *sh);
    must(pc_path_from_json(pc_json_get(root, "path"), &sh->path, NULL));
    v = pc_json_get(root, "fill");
    sh->has_fill = !yyjson_is_null(v);
    if (sh->has_fill) {
        must(pc_fill_from_json(v, &sh->fill, NULL));
    }
    v = pc_json_get(root, "stroke");
    sh->has_stroke = !yyjson_is_null(v);
    if (sh->has_stroke) {
        must(pc_shape_stroke_from_json(v, &sh->stroke, NULL));
    }
    v = pc_json_get(root, "live");
    sh->has_live = !yyjson_is_null(v);
    if (sh->has_live) {
        must(pc_live_shape_from_json(v, &sh->live, NULL));
    }
    v = pc_json_get(root, "psd_raw");
    if (!yyjson_is_null(v)) {
        pc_buf raw = b64d(yyjson_get_str(v));
        must(pc_blob_new(raw.data, raw.len, &sh->psd_raw, NULL));
        pc_buf_fini(&raw);
    }
    yyjson_doc_free(d);
}

/* ---------------------------------------------------------------- comps_map */

/* comps_s of rustref: layer ids as walk indices. */
static inline void comp_s(pc_buf *b, const pc_walk *w, const pc_layer_comp *c)
{
    sb(b, "%u|", c->id);
    sq(b, pc_str_c(&c->name), c->name.len);
    sb(b, "|");
    sq(b, pc_str_c(&c->comment), c->comment.len);
    sb(b, "|%d%d%d|[", c->apply_visibility, c->apply_position, c->apply_appearance);
    for (size_t i = 0; i < c->nstates; i++) {
        const pc_comp_layer_state *s = &c->states[i];
        size_t ix = SIZE_MAX;
        for (size_t k = 0; k < w->len && ix == SIZE_MAX; k++) {
            if (w->items[k].layer->id == s->layer) {
                ix = k;
            }
        }
        sb(b, "%s", i > 0 ? ";" : "");
        if (ix == SIZE_MAX) {
            sb(b, "?");
        } else {
            sb(b, "%zu", ix);
        }
        sb(b, ":%s:", !s->has_visible ? "-" : s->visible ? "true" : "false");
        if (s->has_position) {
            sb(b, "%d,%d", s->position[0], s->position[1]);
        } else {
            sb(b, "-");
        }
    }
    sb(b, "]");
}

static inline void comps_s(pc_buf *b, const pc_document *d, const pc_layer_comp *comps, size_t n, bool has_la,
                           uint32_t la, const pc_layer_comp *last)
{
    pc_walk w;
    must(pc_document_walk(d, &w, NULL));
    sb(b, "C[");
    for (size_t i = 0; i < n; i++) {
        sb(b, "%s", i > 0 ? " " : "");
        comp_s(b, &w, &comps[i]);
    }
    sb(b, "] la=");
    if (has_la) {
        sb(b, "%u", la);
    } else {
        sb(b, "-");
    }
    sb(b, " last=");
    if (last != NULL) {
        comp_s(b, &w, last);
    } else {
        sb(b, "-");
    }
    pc_walk_fini(&w);
}

static inline void state_set(pc_comp_layer_state *s, pc_layer_id id, int vis, bool has_pos, int32_t x,
                             int32_t y)
{
    memset(s, 0, sizeof *s);
    s->layer = id;
    s->has_visible = vis >= 0;
    s->visible = vis > 0;
    s->has_position = has_pos;
    s->position[0] = x;
    s->position[1] = y;
}

static inline void push_comp(pc_document *d, uint32_t id, const char *name, const char *comment, bool v,
                             bool p, bool a, pc_comp_layer_state st)
{
    pc_layer_comp *c;
    d->layer_comps = realloc(d->layer_comps, (d->nlayer_comps + 1) * sizeof *d->layer_comps);
    if (d->layer_comps == NULL) {
        abort();
    }
    c = &d->layer_comps[d->nlayer_comps++];
    memset(c, 0, sizeof *c);
    c->id = id;
    must(pc_str_set_c(&c->name, name, NULL));
    must(pc_str_set_c(&c->comment, comment, NULL));
    c->apply_visibility = v;
    c->apply_position = p;
    c->apply_appearance = a;
    c->states = xmalloc(sizeof *c->states);
    c->states[0] = st;
    c->nstates = 1;
}

static inline void cw_check(const char *name, const char *got)
{
    const gline *l = find_line("cw", name);
    if (l == NULL || l->n != 3) {
        PC_T_FAIL("cw %s missing from the golden", name);
        return;
    }
    CHECK_RESULT(name, l->f[2], got);
}

/* ---------------------------------------------------------------- annotations_map */

static inline pc_status anno_res(pc_buf *b, const uint8_t *d, size_t len, pc_error *e)
{
    bool found = false;
    pc_note *n = NULL;
    size_t nn = 0;
    pc_status st = pc_io_parse_anno(d, len, &found, &n, &nn, e);
    if (st != PC_OK) {
        return st;
    }
    if (!found) {
        sb(b, "none");
        return PC_OK;
    }
    sb(b, "some ");
    JW_ARR(b, pc_note, n, nn);
    for (size_t i = 0; i < nn; i++) {
        pc_note_fini(&n[i]);
    }
    free(n);
    return PC_OK;
}

static inline void read_notes(const char *json, pc_note **out, size_t *n)
{
    yyjson_doc *d = jparse(json);
    yyjson_val *arr = yyjson_doc_get_root(d);
    size_t k = yyjson_arr_size(arr);
    pc_note *v = xmalloc((k > 0 ? k : 1) * sizeof *v);
    for (size_t i = 0; i < k; i++) {
        must(pc_note_from_json(yyjson_arr_get(arr, i), &v[i], NULL));
    }
    yyjson_doc_free(d);
    *out = v;
    *n = k;
}

/* SIGKEY:b64[=],... (rustref blocks_s); "=" marks a block sharing the input's data. */
static inline void blocks_s(pc_buf *b, const pc_doc_psd_global_block *v, size_t n,
                            const pc_doc_psd_global_block *orig, size_t norig)
{
    for (size_t i = 0; i < n; i++) {
        bool same = false;
        for (size_t k = 0; k < norig; k++) {
            same = same || orig[k].data == v[i].data;
        }
        sb(b, "%s%.4s%.4s:", i > 0 ? "," : "", (const char *)v[i].signature, (const char *)v[i].key);
        sb64(b, pc_blob_data(v[i].data), pc_blob_len(v[i].data));
        sb(b, "%s", same ? "=" : "");
    }
}

static inline void parse_blocks(const char *s, pc_document *d)
{
    char *copy = pc_strndup(s, strlen(s));
    char *p = copy;
    while (p != NULL && *p != '\0') {
        char *comma = strchr(p, ',');
        pc_buf data;
        if (comma != NULL) {
            *comma = '\0';
        }
        data = b64d(p + 9);
        m_push_global(d, p + 4, bdata(&data), data.len);
        pc_buf_fini(&data);
        p = comma != NULL ? comma + 1 : NULL;
    }
    free(copy);
}

static inline pc_document ndoc(void)
{
    return t_doc("n", 10, 10, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
}

/* ---------------------------------------------------------------- slices_map */

/* Two layers whose PSD ids are 5 and 7, and a third sharing id 5. */
static inline pc_document slice_doc(void)
{
    pc_document d = t_doc("site.psd", 120, 90, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    static const char *const names[3] = {"Logo", "Nav", "Dup"};
    static const uint32_t ids[3] = {5, 7, 5};
    for (size_t i = 0; i < 3; i++) {
        pc_layer l;
        must(pc_layer_raster(&l, names[i], pc_document_pixel_format(&d), NULL));
        l.has_psd_id = true;
        l.psd_id = ids[i];
        t_push(&d, &l);
    }
    return d;
}

/* Slices JSON with layer ids replaced by walk indices (999 when missing). */
static inline void slices_s(pc_buf *b, const pc_document *d, const pc_slices *s)
{
    pc_slices c;
    pc_walk w;
    must(pc_slices_clone(s, &c, NULL));
    must(pc_document_walk(d, &w, NULL));
    for (size_t i = 0; i < c.len; i++) {
        if (c.list[i].has_layer) {
            uint64_t ix = 999;
            for (size_t k = 0; k < w.len; k++) {
                if (w.items[k].layer->id == c.list[i].layer) {
                    ix = k;
                    break;
                }
            }
            c.list[i].layer = ix;
        }
    }
    pc_walk_fini(&w);
    JW(b, pc_slices, &c);
    pc_slices_fini(&c);
}

/* Slices JSON whose layer fields are walk indices -> the document's layer ids. */
static inline void slices_into(pc_document *d, const char *json)
{
    pc_walk w;
    pc_slices_fini(&d->slices);
    JR(pc_slices, json, &d->slices);
    must(pc_document_walk(d, &w, NULL));
    for (size_t i = 0; i < d->slices.len; i++) {
        if (d->slices.list[i].has_layer) {
            d->slices.list[i].layer = w.items[d->slices.list[i].layer].layer->id;
        }
    }
    pc_walk_fini(&w);
}

static inline pc_status slx_res(pc_buf *b, const pc_document *d, const pc_io_layer_id_map *ids,
                                pc_buf *written, pc_error *e)
{
    pc_io_slices_action act;
    pc_buf out = PC_BUF_INIT;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_status st = pc_io_slices_export_resource(d, ids, &act, &out, &w, e);
    if (st != PC_OK) {
        return st;
    }
    if (act == PC_IO_SLICES_KEEP) {
        sb(b, "keep");
    } else if (act == PC_IO_SLICES_REMOVE) {
        sb(b, "remove");
    } else {
        sb(b, "write ");
        sb64(b, out.data, out.len);
        if (written != NULL) {
            must(pc_buf_append(written, bdata(&out), out.len, NULL));
        }
    }
    sb(b, " ");
    if (w.n > 0) {
        sq(b, w.v[0], strlen(w.v[0]));
    } else {
        sb(b, "-");
    }
    pc_io_warnings_fini(&w);
    pc_buf_fini(&out);
    return PC_OK;
}

/* ---------------------------------------------------------------- pattern_map */

static inline pc_status pblk_res(pc_buf *b, const uint8_t *d, size_t len, pc_error *e)
{
    pc_psd_pattern *ps = NULL;
    size_t n = 0;
    pc_error pe = {0};
    pc_status st = pc_psd_parse_pattern_block(d, len, &ps, &n, &pe);
    if (st != PC_OK) {
        sb(b, "err");
        return st == PC_ERR_NOMEM ? st : PC_OK;
    }
    sb(b, "ok [");
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        bool found = false;
        pc_pattern p;
        st = pc_io_pattern_from_psd(&ps[i], &found, &p, e);
        sb(b, "%s", i > 0 ? ";" : "");
        if (st == PC_OK && found) {
            m_pat_s(b, &p);
            pc_pattern_fini(&p);
        } else if (st == PC_OK) {
            sb(b, "none");
        }
    }
    sb(b, "]");
    pc_psd_patterns_free(ps, n);
    return st;
}

static inline void ptx_check(const char *name, const char *got)
{
    const gline *l = find_line("ptx", name);
    if (l == NULL || l->n != 3) {
        PC_T_FAIL("ptx %s missing from the golden", name);
        return;
    }
    CHECK_RESULT(name, l->f[2], got);
}

static inline void ptx_export(const char *name, const pc_document *d)
{
    pc_pattern *cur = NULL;
    size_t ncur = 0;
    pc_doc_psd_global_block *out = NULL;
    size_t nout = 0;
    char full[64];
    (void)res_reset();
    must(pc_io_patterns_from_global_blocks(d, &cur, &ncur, NULL));
    sb(&g_res, "[");
    for (size_t i = 0; i < ncur; i++) {
        sb(&g_res, "%s", i > 0 ? ";" : "");
        m_pat_s(&g_res, &cur[i]);
    }
    sb(&g_res, "] ");
    pc_io_patterns_free(cur, ncur);
    must(pc_io_export_pattern_blocks(d, &out, &nout, NULL));
    blocks_s(&g_res, out, nout, d->metadata.psd_global_blocks, d->metadata.npsd_global_blocks);
    m_global_blocks_free(out, nout);
    (void)snprintf(full, sizeof full, "exp/%s", name);
    ptx_check(full, pc_buf_cstr(&g_res));
}

static inline void push_pattern(pc_document *d, pc_pattern p)
{
    d->patterns = realloc(d->patterns, (d->npatterns + 1) * sizeof *d->patterns);
    if (d->patterns == NULL) {
        abort();
    }
    d->patterns[d->npatterns++] = p;
}

/* ---------------------------------------------------------------- gradient_bake */

static inline void color_s(pc_buf *b, const pc_color *c)
{
    sb(b, "%d:%08x:%08x:%08x:%08x:%08x", (int)c->mode, fb(c->c[0]), fb(c->c[1]), fb(c->c[2]), fb(c->c[3]),
       fb(c->alpha));
}

/* ---------------------------------------------------------------- abr_map */

static inline pc_status abr_res(pc_buf *b, const uint8_t *d, size_t len, pc_error *e)
{
    pc_io_abr_import imp;
    pc_status st = pc_io_read_abr(d, len, "G", &imp, e);
    if (st == PC_ERR_NOMEM) {
        return st;
    }
    if (st != PC_OK) {
        sb(b, "err ");
        sq(b, e->msg, strlen(e->msg));
        return PC_OK;
    }
    {
        yyjson_mut_doc *jd = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *arr = yyjson_mut_arr(jd);
        pc_buf js = PC_BUF_INIT;
        for (size_t i = 0; i < imp.presets.len; i++) {
            yyjson_mut_val *v = pc_paint_json_write(PC_PAINT_JSON_PRESET, jd, &imp.presets.v[i], 0);
            if (v == NULL || !yyjson_mut_arr_append(arr, v)) {
                abort();
            }
        }
        must(pc_json_write(arr, 0, &js, NULL));
        sb(b, "ok v=%u P=%zu ", imp.version, imp.presets.len);
        sh16(b, js.data, js.len);
        sb(b, " ");
        s_warn(b, imp.warnings.v, imp.warnings.n);
        pc_buf_fini(&js);
        yyjson_mut_doc_free(jd);
    }
    pc_io_abr_import_fini(&imp);
    return PC_OK;
}

/* io warnings are C strings and stop at an embedded U+0000 (REWRITE_PLAN.md section 7, I1): the
 * reference's text of such a warning is cut at its "\0" escape up to the closing quote. src/psd's
 * own warnings print a block key only up to its NUL (F9): there the key's rest is dropped
 * (up to the next space). */
static inline void cut_at_nul(const char *want, bool key_only, pc_buf *out)
{
    bool quoted = false;
    for (const char *p = want; *p != '\0'; p++) {
        if (quoted && p[0] == '\\' && p[1] == '0') {
            if (key_only) {
                while (*p != '\0' && *p != ' ' && *p != '"') {
                    p++;
                }
            } else {
                while (*p != '\0' && !(p[0] == '"' && p[-1] != '\\')) {
                    p++;
                }
            }
            if (*p == '\0') {
                break;
            }
        }
        if (*p == '"' && (p == want || p[-1] != '\\')) {
            quoted = !quoted;
        } else if (quoted && p[0] == '\\' && p[1] != '\0') {
            must(pc_buf_append(out, p, 2, NULL));
            p++;
            continue;
        }
        must(pc_buf_append_byte(out, (uint8_t)*p, NULL));
    }
}

/* ---------------------------------------------------------------- text */

static inline void ts_check(const char *name, const char *got)
{
    const gline *l = find_line("ts", name);
    if (l == NULL || l->n != 3) {
        PC_T_FAIL("ts %s missing from the golden", name);
        return;
    }
    CHECK_RESULT(name, l->f[2], got);
}

/* A document with the type layer of data twice (at the root and in a group). */
static inline pc_document ts_doc(const uint8_t *data, size_t len)
{
    pc_document d = t_doc("t", 200, 80, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_text_layer t;
    pc_text_layer t2;
    pc_layer l;
    pc_layer *kids = xmalloc(sizeof *kids);
    if (pc_text_layer_from_tysh(data, len, 72.0f, &t, NULL) != PC_OK) {
        pc_text_layer_default(&t);
    }
    must(pc_blob_new(data, len, &t.psd_raw, NULL));
    must(pc_text_layer_clone(&t, &t2, NULL));
    l = m_text("t", &t);
    t_push(&d, &l);
    kids[0] = m_text("t2", &t2);
    must(pc_layer_group(&l, "g", kids, 1, NULL));
    t_push(&d, &l);
    return d;
}

static inline void ts_import_s(pc_buf *b, pc_document *d)
{
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_walk wk;
    must(pc_io_text_styles_import(d, &w, NULL));
    JW(b, pc_text_styles, &d->text_styles);
    must(pc_document_walk(d, &wk, NULL));
    for (size_t i = 0; i < wk.len; i++) {
        if (wk.items[i].layer->content.kind == PC_LAYER_TEXT) {
            sb(b, " ");
            m_run_refs(b, &wk.items[i].layer->content.u.text);
        }
    }
    pc_walk_fini(&wk);
    sb(b, " ");
    s_warn(b, w.v, w.n);
    pc_io_warnings_fini(&w);
}

static inline pc_text_layer ti_text(const char *text, const char *family, const char *ps, float size)
{
    pc_text_layer t;
    pc_text_layer_default(&t);
    must(pc_str_set_c(&t.text, text, NULL));
    t.runs = xmalloc(sizeof *t.runs);
    t.nruns = 1;
    t.runs[0].len = strlen(text);
    pc_char_style_default(&t.runs[0].style);
    must(pc_str_set_c(&t.runs[0].style.font_family, family, NULL));
    if (ps != NULL) {
        t.runs[0].style.has_postscript_name = true;
        must(pc_str_set_c(&t.runs[0].style.postscript_name, ps, NULL));
    }
    t.runs[0].style.size_pt = size;
    t.transform = pc_affine_translate(3.0, 30.0);
    must(pc_text_layer_sync_summary(&t, NULL));
    return t;
}

static inline void ti_summary(pc_buf *b, const pc_document *d)
{
    pc_walk w;
    bool first = true;
    must(pc_document_walk(d, &w, NULL));
    for (size_t i = 0; i < w.len; i++) {
        const pc_layer *l = w.items[i].layer;
        const pc_text_layer *t;
        if (l->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        t = &l->content.u.text;
        sb(b, "%s", first ? "" : " ");
        first = false;
        sq(b, pc_str_c(&l->name), l->name.len);
        sb(b, "|");
        sq(b, pc_str_c(&t->font_family), t->font_family.len);
        sb(b, "|");
        for (size_t r = 0; r < t->nruns; r++) {
            const pc_char_style *s = &t->runs[r].style;
            sb(b, "%s", r > 0 ? "," : "");
            sq(b, pc_str_c(&s->font_family), s->font_family.len);
            sb(b, "/%u/%s", s->weight, s->italic ? "true" : "false");
        }
        sb(b, "|");
        if (t->has_cache) {
            pc_rect r = pc_surface_content_bounds(&t->cache);
            m_surf_s(b, &t->cache, r);
            sb(b, ":%d,%d,%d,%d", r.x0, r.y0, r.x1, r.y1);
        } else {
            sb(b, "-");
        }
    }
    pc_walk_fini(&w);
}

/* ---------------------------------------------------------------- corpus */

static inline bool key_is(const uint8_t k[4], const char *s)
{
    return memcmp(k, s, 4) == 0;
}

/* One map result reduced to its hash ("NAME=h16"). */
static inline void part(pc_buf *b, const char *name, const pc_buf *res)
{
    sb(b, "%s%s=", b->len > 0 ? " " : "", name);
    sh16(b, res->data != NULL ? res->data : (const uint8_t *)"", res->len);
}

static inline void corpus_blocks(pc_buf *b, const pc_psd_file *f)
{
    uint32_t w = f->header.width;
    uint32_t h = f->header.height;
    pc_buf r = PC_BUF_INIT;
    char name[32];
    for (size_t i = 0; i < f->nresources; i++) {
        const pc_psd_resource *res = &f->resources[i];
        const uint8_t *d = res->data.data != NULL ? res->data.data : (const uint8_t *)"";
        pc_buf_truncate(&r, 0);
        if (res->id == 1025 || (res->id >= 2000 && res->id <= 2997)) {
            must(path_res(&r, d, res->data.len, w, h, NULL));
            (void)snprintf(name, sizeof name, "r%u", res->id);
            part(b, name, &r);
        } else if (res->id == 1050) {
            pc_document doc = t_doc("c", w, h, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
            pc_slices s;
            must(pc_io_slices_from_psd(d, res->data.len, &doc, &s, NULL));
            slices_s(&r, &doc, &s);
            pc_slices_fini(&s);
            pc_document_fini(&doc);
            part(b, "r1050", &r);
        } else if (res->id == 1074) {
            bool found = false;
            pc_measurement_scale s;
            must(pc_io_scale_from_resource(d, res->data.len, &found, &s, NULL));
            sb(b, "%sr1074=", b->len > 0 ? " " : "");
            if (found) {
                JW(b, pc_measurement_scale, &s);
                pc_measurement_scale_fini(&s);
            } else {
                sb(b, "none");
            }
        }
    }
    for (size_t i = 0; i < f->nglobal; i++) {
        const pc_psd_block *g = &f->global_blocks[i];
        const uint8_t *d = g->data.data != NULL ? g->data.data : (const uint8_t *)"";
        pc_buf_truncate(&r, 0);
        if (key_is(g->key, "Anno")) {
            must(anno_res(&r, d, g->data.len, NULL));
            part(b, "Anno", &r);
        } else if (key_is(g->key, "Patt") || key_is(g->key, "Pat2") || key_is(g->key, "Pat3")) {
            must(pblk_res(&r, d, g->data.len, NULL));
            (void)snprintf(name, sizeof name, "%.4s", (const char *)g->key);
            part(b, name, &r);
        }
    }
    for (size_t i = 0; f->has_layer_info && i < f->layer_info.n; i++) {
        const pc_psd_layer *l = &f->layer_info.layers[i];
        for (size_t k = 0; k < l->nblocks; k++) {
            const pc_psd_block *g = &l->blocks[k];
            const uint8_t *d = g->data.data != NULL ? g->data.data : (const uint8_t *)"";
            pc_status st;
            pc_error e = {0};
            pc_buf_truncate(&r, 0);
            if (key_is(g->key, "vmsk") || key_is(g->key, "vsms")) {
                st = vmsk_res(&r, d, g->data.len, w, h, &e);
            } else if (key_is(g->key, "vstk")) {
                st = vstk_res(&r, d, g->data.len, 72.0f, &e);
            } else if (key_is(g->key, "vscg")) {
                st = vscg_res(&r, d, g->data.len, &e);
            } else if (key_is(g->key, "vogk")) {
                st = vogk_res(&r, d, g->data.len, &e);
            } else if (key_is(g->key, "artb") || key_is(g->key, "artd") || key_is(g->key, "abdd")) {
                st = art_res(&r, d, g->data.len, &e);
            } else {
                continue;
            }
            must(st);
            (void)snprintf(name, sizeof name, "%zu%.4s", i, (const char *)g->key);
            part(b, name, &r);
        }
    }
    pc_buf_fini(&r);
}

static inline void corpus_doc(pc_buf *b, const pc_document *d)
{
    pc_buf r = PC_BUF_INIT;
    pc_walk w;
    bool first = true;
    for (size_t i = 0; i < d->npaths; i++) {
        sq(&r, pc_str_c(&d->paths[i].name), d->paths[i].name.len);
        JW(&r, pc_path, &d->paths[i].path);
    }
    sb(b, "paths=");
    sh16(b, bdata(&r), r.len);
    sb(b, " work=");
    if (d->has_work_path) {
        pc_buf_truncate(&r, 0);
        JW(&r, pc_path, &d->work_path);
        sh16(b, r.data, r.len);
    } else {
        sb(b, "-");
    }
    pc_buf_truncate(&r, 0);
    JW_ARR(&r, pc_note, d->notes, d->nnotes);
    sb(b, " notes=");
    sh16(b, r.data, r.len);
    pc_buf_truncate(&r, 0);
    slices_s(&r, d, &d->slices);
    sb(b, " slices=");
    sh16(b, r.data, r.len);
    pc_buf_truncate(&r, 0);
    comps_s(&r, d, d->layer_comps, d->nlayer_comps, d->has_last_applied_comp, d->last_applied_comp,
            d->has_last_document_state ? &d->last_document_state : NULL);
    sb(b, " comps=");
    sh16(b, r.data, r.len);
    pc_buf_truncate(&r, 0);
    JW(&r, pc_text_styles, &d->text_styles);
    sb(b, " styles=");
    sh16(b, r.data, r.len);
    pc_buf_truncate(&r, 0);
    for (size_t i = 0; i < d->npatterns; i++) {
        m_pat_s(&r, &d->patterns[i]);
    }
    sb(b, " patterns=");
    sh16(b, bdata(&r), r.len);
    sb(b, " scale=");
    JW(b, pc_measurement_scale, &d->measurement.scale);
    sb(b, " L[");
    must(pc_document_walk(d, &w, NULL));
    for (size_t i = 0; i < w.len; i++) {
        const pc_layer *l = w.items[i].layer;
        pc_buf s = PC_BUF_INIT;
        if (l->has_vector_mask) {
            pc_buf_truncate(&r, 0);
            JW(&r, pc_vector_mask, &l->vector_mask);
            sb(&s, "vm=");
            sh16(&s, r.data, r.len);
        }
        if (l->content.kind == PC_LAYER_SHAPE) {
            const pc_shape_layer *sh = &l->content.u.shape;
            pc_buf_truncate(&r, 0);
            JW(&r, pc_path, &sh->path);
            JW_OPT(&r, pc_fill, sh->has_fill, &sh->fill);
            JW_OPT(&r, pc_shape_stroke, sh->has_stroke, &sh->stroke);
            JW_OPT(&r, pc_live_shape, sh->has_live, &sh->live);
            sb(&s, "sh=");
            sh16(&s, r.data, r.len);
        } else if (l->content.kind == PC_LAYER_GROUP && l->content.u.group.has_artboard) {
            pc_buf_truncate(&r, 0);
            JW(&r, pc_artboard, &l->content.u.group.artboard);
            sb(&s, "ab=");
            sh16(&s, r.data, r.len);
        } else if (l->content.kind == PC_LAYER_TEXT) {
            sb(&s, "tx=");
            m_run_refs(&s, &l->content.u.text);
        }
        if (s.len > 0) {
            sb(b, "%s%zu:%s", first ? "" : ",", i, pc_buf_cstr(&s));
            first = false;
        }
        pc_buf_fini(&s);
    }
    pc_walk_fini(&w);
    sb(b, "]");
    pc_buf_fini(&r);
}

#endif
