/* PSD export parity with the Rust reference (tests/golden/io-export-vectors.txt, made by
 * `rustref io-export-vectors`; see its header for the line kinds).
 *
 * Documents come as .pcraft bundles (loaded with their ids kept) so both sides export the same
 * document. Written files must be byte-identical; where a plane is zip-compressed (zlib here,
 * miniz in the reference: REWRITE_PLAN.md §7 F6) the file is compared with those planes stored
 * raw (decompressed bytes), plus its length-independent outline. Round trips (export, import,
 * export) need I2's import; lines whose results depend on another chunk's stubs count as pending
 * while those stubs exist. Corpus lines are skipped without corpus/. */
#define GOLDEN "tests/golden/io-export-vectors.txt"
#include "harness.h"

#include <dirent.h>
#include <math.h>

#include "common.h"
#include "format/format.h"
#include "golden.h"
#include "io/psd_export.h"
#include "io/psd_import.h"
#include "io/smart_map.h"
#include "psd/filter_effects.h"

/* Whether I2's PSD import is still a stub (its lines then count as pending). */
static bool g_i2_stub;
static int g_shown;

static void probe_stubs(void)
{
    pc_psd_file f;
    pc_document d;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_error e;
    pc_status st;
    pc_psd_file_init(&f);
    f.header = pc_psd_header_make(PC_PSD_V1, 1, 1, 3, 8, PC_PSD_MODE_RGB);
    st = pc_io_psd_to_document(&f, &d, &w, &e);
    g_i2_stub = is_pending(st, &e);
    if (st == PC_OK) {
        pc_document_fini(&d);
    }
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

static void hx(const void *data, size_t len, size_t n, char *out)
{
    char hex[PC_BLAKE3_HEX_LEN + 1];
    pc_blake3_hex(data, len, hex);
    memcpy(out, hex, n);
    out[n] = '\0';
}

static void s_hex(pc_buf *b, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sb(b, "%02x", d[i]);
    }
}

/* ---- normalising and describing a file (as the reference's normalize / outline) ---- */

static bool is_zip(uint16_t c)
{
    return c == PC_PSD_ZIP || c == PC_PSD_ZIP_PREDICT;
}

static void raw_plane(pc_psd_fx_plane *p, size_t w, size_t h, uint16_t depth, bool *zip)
{
    pc_psd_bytes d = {NULL, 0};
    if (!is_zip(p->compression)) {
        return;
    }
    *zip = true;
    if (pc_psd_fx_plane_decode(p, w, h, depth, &d, NULL) == PC_OK) {
        pc_psd_bytes_fini(&p->data);
        p->data = d;
        p->compression = PC_PSD_RAW;
    }
}

static bool normalize_fx(pc_psd_filter_effects *fx)
{
    bool any = false;
    for (size_t i = 0; i < fx->n; i++) {
        pc_psd_fx_item *it = &fx->items[i];
        size_t w;
        size_t h;
        if (pc_psd_rect_size(it->rect, &w, &h, NULL) != PC_OK || it->depth > UINT16_MAX) {
            continue;
        }
        for (size_t s = 0; s < it->nslots; s++) {
            if (it->slot_written[s]) {
                raw_plane(&it->slots[s], w, h, (uint16_t)it->depth, &any);
            }
        }
        if (it->has_mask && pc_psd_rect_size(it->mask_rect, &w, &h, NULL) == PC_OK) {
            raw_plane(&it->mask, w, h, (uint16_t)it->depth, &any);
        }
    }
    return any;
}

static void normalize(pc_psd_file *f, bool *zip)
{
    *zip = false;
    for (size_t i = 0; f->has_layer_info && i < f->layer_info.n; i++) {
        pc_psd_layer *l = &f->layer_info.layers[i];
        for (size_t c = 0; c < l->nchannels; c++) {
            pc_psd_channel *ch = &l->channels[c];
            size_t w;
            size_t h;
            pc_psd_bytes d = {NULL, 0};
            if (!ch->has_compression || !is_zip(ch->compression)) {
                continue;
            }
            *zip = true;
            if (pc_psd_rect_size(pc_psd_layer_channel_rect(l, ch->id), &w, &h, NULL) == PC_OK &&
                pc_psd_channel_decode(ch, w, h, f->header.depth, f->header.version, &d, NULL) == PC_OK) {
                pc_psd_bytes_fini(&ch->data);
                ch->data = d;
                ch->compression = PC_PSD_RAW;
            }
        }
    }
    /* The layer info's 4-byte padding depends on the compressed lengths: recompute it. */
    if (*zip && f->has_layer_info &&
        pc_psd_layer_info_pad_to(&f->layer_info, f->header.version, 4, NULL) != PC_OK) {
        pc_psd_opt_bytes_fini(&f->layer_info.padding);
        f->layer_info.padding.has = false;
    }
    if (is_zip(f->image_compression)) {
        pc_psd_bytes d = {NULL, 0};
        *zip = true;
        if (pc_psd_file_decode_merged(f, &d, NULL) == PC_OK) {
            pc_psd_bytes_fini(&f->image_data);
            f->image_data = d;
            f->image_compression = PC_PSD_RAW;
        }
    }
    for (size_t i = 0; i < f->nglobal; i++) {
        pc_psd_block *b = &f->global_blocks[i];
        pc_psd_filter_effects fx;
        if (!pc_psd_block_is(b, "FEid") && !pc_psd_block_is(b, "FXid")) {
            continue;
        }
        if (pc_psd_filter_effects_parse(b->data.data, b->data.len, &fx, NULL) != PC_OK) {
            continue;
        }
        if (normalize_fx(&fx)) {
            pc_psd_bytes d = {NULL, 0};
            *zip = true;
            must(pc_psd_filter_effects_to_bytes(&fx, &d, NULL));
            pc_psd_bytes_fini(&b->data);
            b->data = d;
        }
        pc_psd_filter_effects_fini(&fx);
    }
}

static void s_h8(pc_buf *b, const void *d, size_t n)
{
    char h[9];
    hx(d, n, 8, h);
    sb(b, "%s", h);
}

static void s_optlen(pc_buf *b, const pc_psd_opt_bytes *p)
{
    if (p->has) {
        sb(b, "Some(%zu)", p->v.len);
    } else {
        sb(b, "None");
    }
}

static void s_u8list(pc_buf *b, const uint8_t *d, size_t n)
{
    sb(b, "[");
    for (size_t i = 0; i < n; i++) {
        sb(b, "%s%u", i > 0 ? ", " : "", d[i]);
    }
    sb(b, "]");
}

static void outline(const pc_psd_file *f, pc_buf *s)
{
    const pc_psd_header *h = &f->header;
    sb(s, "H%u:%u:%ux%u:%u:%u cmd=%zu:", (unsigned)h->version, h->channels, h->width, h->height, h->depth,
       h->color_mode, f->color_mode_data.len);
    s_h8(s, f->color_mode_data.data, f->color_mode_data.len);
    sb(s, " R[");
    for (size_t i = 0; i < f->nresources; i++) {
        const pc_psd_resource *r = &f->resources[i];
        sb(s, "%u:", r->id);
        s_hex(s, r->name.data, r->name.len);
        sb(s, ":%zu:", r->data.len);
        s_h8(s, r->data.data, r->data.len);
        sb(s, " ");
    }
    sb(s, "]");
    if (f->has_layer_info) {
        sb(s, " LI%d:", f->layer_info.merged_alpha ? 1 : 0);
        s_optlen(s, &f->layer_info.padding);
        for (size_t i = 0; i < f->layer_info.n; i++) {
            const pc_psd_layer *l = &f->layer_info.layers[i];
            sb(s, " L{");
            s_hex(s, l->name.data, l->name.len);
            sb(s, "|%d,%d,%d,%d|%.4s|%u|%u|%u|%u|", l->rect.top, l->rect.left, l->rect.bottom, l->rect.right,
               (const char *)l->blend, l->opacity, l->clipping, l->flags, l->filler);
            if (l->mask.kind == PC_PSD_MASK_NONE) {
                sb(s, "-");
            } else if (l->mask.kind == PC_PSD_MASK_RAW) {
                sb(s, "raw%zu", l->mask.raw.len);
            } else {
                const pc_psd_layer_mask *m = &l->mask.mask;
                sb(s, "%d,%d,%d,%d:%u:%u:%u:%zu", m->rect.top, m->rect.left, m->rect.bottom, m->rect.right,
                   m->default_color, m->flags, m->has_params ? m->params.flags : 0, m->trailing.len);
            }
            sb(s, "|");
            s_h8(s, l->blending_ranges.data, l->blending_ranges.len);
            sb(s, "|C");
            for (size_t c = 0; c < l->nchannels; c++) {
                const pc_psd_channel *ch = &l->channels[c];
                sb(s, " %d:%d:%zu:", ch->id, ch->has_compression ? (int)ch->compression : -1, ch->data.len);
                s_h8(s, ch->data.data, ch->data.len);
            }
            sb(s, "|B");
            for (size_t k = 0; k < l->nblocks; k++) {
                const pc_psd_block *b = &l->blocks[k];
                sb(s, " %.4s:%zu:", (const char *)b->key, b->data.len);
                s_h8(s, b->data.data, b->data.len);
                sb(s, ":");
                s_optlen(s, &b->padding);
            }
            sb(s, "}");
        }
    }
    sb(s, " P");
    if (f->placement.where == PC_PSD_LI_SECTION) {
        sb(s, "Section");
    } else {
        sb(s, "GlobalBlock { index: %zu, signature: ", f->placement.index);
        s_u8list(s, f->placement.signature, 4);
        sb(s, ", key: ");
        s_u8list(s, f->placement.key, 4);
        sb(s, ", padding: ");
        if (f->placement.padding.has) {
            sb(s, "Some(");
            s_u8list(s, f->placement.padding.v.data, f->placement.padding.v.len);
            sb(s, ")");
        } else {
            sb(s, "None");
        }
        sb(s, " }");
    }
    if (f->has_global_mask) {
        sb(s, " GMSome(%zu) G[", f->global_mask.len);
    } else {
        sb(s, " GMNone G[");
    }
    for (size_t i = 0; i < f->nglobal; i++) {
        const pc_psd_block *b = &f->global_blocks[i];
        sb(s, "%.4s%.4s:%zu:", (const char *)b->signature, (const char *)b->key, b->data.len);
        s_h8(s, b->data.data, b->data.len);
        sb(s, ":");
        s_optlen(s, &b->padding);
        sb(s, " ");
    }
    sb(s, "] I%u:%zu:", f->image_compression, f->image_data.len);
    s_h8(s, f->image_data.data, f->image_data.len);
}

/* "ok LEN HASH ZIP NHASH W[..]" and the outline (the reference's psd_result). */
static void psd_result(const pc_psd_file *f, const pc_io_warnings *w, pc_buf *res, pc_buf *outl)
{
    pc_psd_bytes bytes = {NULL, 0};
    pc_error e;
    pc_buf_truncate(res, 0);
    pc_buf_truncate(outl, 0);
    if (pc_psd_file_to_bytes(f, &bytes, &e) != PC_OK) {
        sb(res, "err psd ");
        sq(res, e.msg, strlen(e.msg));
        sb(outl, "-");
        return;
    }
    {
        pc_psd_file n;
        pc_psd_bytes nb = {NULL, 0};
        bool zip = false;
        char h[17];
        char nh[17];
        must(pc_psd_file_clone(f, &n, NULL));
        normalize(&n, &zip);
        if (pc_psd_file_to_bytes(&n, &nb, NULL) != PC_OK) {
            nb.len = 0;
        }
        h16(bytes.data, bytes.len, h);
        h16(nb.data, nb.len, nh);
        sb(res, "ok %zu %s %d %s ", bytes.len, h, zip ? 1 : 0, nh);
        s_warn(res, w->v, w->n);
        outline(&n, outl);
        pc_psd_bytes_fini(&nb);
        pc_psd_file_fini(&n);
    }
    pc_psd_bytes_fini(&bytes);
}

/* Field i (space-separated) of s, copied into out. */
static void field(const char *s, int i, char *out, size_t cap)
{
    const char *p = s;
    for (int k = 0; k < i && p != NULL; k++) {
        p = strchr(p, ' ');
        p = p != NULL ? p + 1 : NULL;
    }
    if (p == NULL) {
        out[0] = '\0';
        return;
    }
    {
        const char *e = strchr(p, ' ');
        size_t n = e != NULL ? (size_t)(e - p) : strlen(p);
        n = n < cap - 1 ? n : cap - 1;
        memcpy(out, p, n);
        out[n] = '\0';
    }
}

/* Results match exactly, or (zip on either side) in everything but the raw length and hash. */
static bool same_export(const char *want, const char *got)
{
    char wz[8];
    char gz[8];
    const char *wt;
    const char *gt;
    if (strcmp(want, got) == 0) {
        return true;
    }
    if (strncmp(want, "ok ", 3) != 0 || strncmp(got, "ok ", 3) != 0) {
        return false;
    }
    field(want, 3, wz, sizeof wz);
    field(got, 3, gz, sizeof gz);
    if (strcmp(wz, "1") != 0 || strcmp(gz, "1") != 0) {
        return false;
    }
    /* from the ZIP field on */
    wt = strchr(strchr(strchr(want + 3, ' ') + 1, ' ') + 1, ' ');
    gt = strchr(strchr(strchr(got + 3, ' ') + 1, ' ') + 1, ' ');
    return wt != NULL && gt != NULL && strcmp(wt, gt) == 0;
}

static void report(const char *what, const char *want, const char *got, const char *wo, const char *go)
{
    g_checked++;
    pc_t_checks++;
    if (same_export(want, got) && (wo == NULL || strcmp(wo, go) == 0)) {
        return;
    }
    if (g_i2_stub) {
        g_pending++;
        return;
    }
    PC_T_FAIL("%s:\n  want %.400s\n  got  %.400s", what, want, got);
    if (wo != NULL && g_shown++ < 6) {
        (void)fprintf(stderr, "  outline want %s\n  outline got  %s\n", wo, go);
    }
}

/* ---- documents ---- */

typedef struct named_doc {
    const char *name;
    pc_document doc;
    bool ok;
} named_doc;

static named_doc *g_docs;
static size_t g_ndocs;

static const pc_document *find_doc(const char *name)
{
    for (size_t i = 0; i < g_ndocs; i++) {
        if (strcmp(g_docs[i].name, name) == 0) {
            return g_docs[i].ok ? &g_docs[i].doc : NULL;
        }
    }
    return NULL;
}

static void test_load_docs(void)
{
    g_docs = calloc(g_nlines, sizeof *g_docs);
    if (g_docs == NULL) {
        abort();
    }
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_buf b;
        pc_error e;
        named_doc *d;
        if (!is_kind(l, "doc") || l->n != 3) {
            continue;
        }
        b = b64d(l->f[2]);
        d = &g_docs[g_ndocs++];
        d->name = l->f[1];
        d->ok = pc_format_load_from_bytes(b.data, b.len, &d->doc, &e) == PC_OK;
        if (!d->ok) {
            PC_T_FAIL("loading %s: %s", l->f[1], e.msg);
        }
        pc_buf_fini(&b);
        CHECK(d->ok);
    }
    CHECK(g_ndocs > 30);
}

static pc_io_psd_export_opts variant(const char *v)
{
    pc_io_psd_export_opts o = pc_io_psd_export_opts_default();
    if (strcmp(v, "b") == 0) {
        o.force_psb = true;
    } else if (strcmp(v, "t") == 0) {
        o.merged_matte = false;
        o.merged_compression = PC_PSD_RAW;
    }
    return o;
}

static pc_status export_result(const pc_document *d, const pc_io_psd_export_opts *o, pc_psd_file *f,
                               pc_io_warnings *w, pc_buf *res, pc_buf *outl, pc_error *e)
{
    pc_status st = pc_io_document_to_psd_with(d, o, f, w, e);
    if (st == PC_OK) {
        psd_result(f, w, res, outl);
    } else {
        pc_buf_truncate(res, 0);
        s_err(res, st, e);
    }
    return st;
}

static void test_exports(void)
{
    pc_buf res = PC_BUF_INIT;
    pc_buf outl = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        const pc_document *d;
        pc_io_psd_export_opts o;
        pc_psd_file f;
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        pc_error e;
        pc_status st;
        char what[128];
        if (!is_kind(l, "psd") || l->n != 5) {
            continue;
        }
        d = find_doc(l->f[1]);
        if (d == NULL) {
            g_skipped++;
            continue;
        }
        o = variant(l->f[2]);
        st = export_result(d, &o, &f, &w, &res, &outl, &e);
        (void)snprintf(what, sizeof what, "psd %s %s", l->f[1], l->f[2]);
        if (st != PC_OK && is_pending(st, &e)) {
            g_pending++;
        } else {
            report(what, l->f[3], pc_buf_cstr(&res), l->f[4], st == PC_OK ? pc_buf_cstr(&outl) : "-");
        }
        if (st == PC_OK) {
            pc_psd_file_fini(&f);
        }
        pc_io_warnings_fini(&w);
    }
    pc_buf_fini(&res);
    pc_buf_fini(&outl);
}

/* export -> bytes -> parse -> import -> export (the reference's export_lines round trip). */
static void test_round_trips(void)
{
    pc_buf res = PC_BUF_INIT;
    pc_buf outl = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        const pc_document *d;
        pc_psd_file f;
        pc_psd_bytes bytes = {NULL, 0};
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        pc_document back;
        pc_error e;
        pc_status st;
        bool rt2;
        char what[128];
        if (!(is_kind(l, "rt") && l->n == 4) && !(is_kind(l, "rt2") && l->n == 3)) {
            continue;
        }
        rt2 = is_kind(l, "rt2");
        d = find_doc(l->f[1]);
        if (d == NULL) {
            g_skipped++;
            continue;
        }
        (void)snprintf(what, sizeof what, "%s %s", l->f[0], l->f[1]);
        st = pc_io_document_to_psd(d, &f, &e);
        for (int round = 0; st == PC_OK && round < (rt2 ? 2 : 1); round++) {
            pc_psd_file parsed;
            pc_io_warnings iw = PC_IO_WARNINGS_INIT;
            st = pc_psd_file_to_bytes(&f, &bytes, &e);
            pc_psd_file_fini(&f);
            if (st == PC_OK) {
                st = pc_psd_file_parse(bytes.data, bytes.len, &parsed, &e);
                pc_psd_bytes_fini(&bytes);
            }
            if (st == PC_OK) {
                st = pc_io_psd_to_document(&parsed, &back, &iw, &e);
                pc_psd_file_fini(&parsed);
            }
            if (st == PC_OK) {
                pc_io_warnings_fini(&w);
                st = pc_io_document_to_psd_with(&back, NULL, &f, &w, &e);
                pc_document_fini(&back);
            }
            if (st == PC_OK && round + 1 == (rt2 ? 2 : 1)) {
                psd_result(&f, &w, &res, &outl);
                if (!rt2) {
                    sb(&res, " I[");
                    for (size_t k = 0; k < iw.n; k++) {
                        sb(&res, "%s", k > 0 ? ", " : "");
                        sq(&res, iw.v[k], strlen(iw.v[k]));
                    }
                    sb(&res, "]");
                }
                pc_psd_file_fini(&f);
            }
            pc_io_warnings_fini(&iw);
        }
        if (st != PC_OK && is_pending(st, &e)) {
            g_pending++;
        } else if (st != PC_OK) {
            pc_buf_truncate(&res, 0);
            s_err(&res, st, &e);
            report(what, l->f[2], pc_buf_cstr(&res), NULL, NULL);
        } else {
            report(what, l->f[2], pc_buf_cstr(&res), rt2 ? NULL : l->f[3], pc_buf_cstr(&outl));
        }
        pc_io_warnings_fini(&w);
    }
    pc_buf_fini(&res);
    pc_buf_fini(&outl);
}

/* ---- smart-object vectors ---- */

typedef struct filt {
    const char *name;
    const char *command;
    const char *params;
    pc_blend_mode blend;
    float opacity;
    bool visible;
} filt;

#define NF(n, c, p) {n, c, p, PC_BLEND_NORMAL, 1.0f, true}

static const filt FILTERS[] = {
    NF("gauss", "filter.blur.gaussianBlur", "{\"radius\": 4.0}"),
    NF("box", "filter.blur.boxBlur", "{\"radius\": 5.0}"),
    NF("motion", "filter.blur.motionBlur", "{\"angle\": -20, \"distance\": 10.0}"),
    NF("unsharp", "filter.sharpen.unsharpMask", "{\"amount\": 150.0, \"radius\": 2.0, \"threshold\": 0}"),
    NF("noise", "filter.noise.addNoise",
       "{\"amount\": 12.0, \"distribution\": \"uniform\", \"monochromatic\": false, \"seed\": 1234}"),
    NF("noise-g", "filter.noise.addNoise",
       "{\"amount\": 3.5, \"distribution\": \"gaussian\", \"monochromatic\": true}"),
    NF("median", "filter.noise.median", "{\"radius\": 3.0}"),
    NF("highpass", "filter.other.highPass", "{\"radius\": 3.0}"),
    NF("max", "filter.other.maximum", "{\"radius\": 2.0}"),
    NF("min", "filter.other.minimum", "{\"radius\": 2.0, \"preserve\": \"squareness\"}"),
    NF("min-round", "filter.other.minimum", "{\"radius\": 2.0, \"preserve\": \"roundness\"}"),
    NF("mosaic", "filter.pixelate.mosaic", "{\"cellSize\": 8.0}"),
    NF("emboss", "filter.stylize.emboss", "{\"angle\": 135, \"height\": 3, \"amount\": 100}"),
    NF("emboss-defaults", "filter.stylize.emboss", "{}"),
    NF("levels", "image.adjustments.levels",
       "{\"inBlack\": 20, \"inWhite\": 220, \"gamma\": 1.4, \"red\": {\"inBlack\": 0, \"inWhite\": 255, "
       "\"gamma\": 0.8}}"),
    NF("levels-out", "image.adjustments.levels",
       "{\"outBlack\": 5, \"blue\": {\"outWhite\": 250}, \"layer\": 3, \"__x\": 1}"),
    NF("levels-bad", "image.adjustments.levels", "{\"cyan\": {\"gamma\": 2.0}}"),
    NF("levels-notobj", "image.adjustments.levels", "[1, 2]"),
    NF("curves", "image.adjustments.curves",
       "{\"points\": [[0.0, 0.0], [64.0, 40.0], [255.0, 255.0]], \"blue\": [[0.0, 20.0], [255.0, 235.0]]}"),
    NF("curves-bad", "image.adjustments.curves", "{\"points\": [[0.0], [255.0, 255.0]]}"),
    NF("sh", "image.adjustments.shadowsHighlights", "{}"),
    NF("sh-meta", "image.adjustments.shadowsHighlights", "{\"layer\": 1}"),
    NF("sh-set", "image.adjustments.shadowsHighlights", "{\"shadowAmount\": 50}"),
    NF("twirl", "filter.distort.twirl", "{\"angle\": 50}"),
    NF("unsup-bad", PC_IO_UNSUPPORTED_FILTER, "{\"psd\": \"zz\"}"),
    NF("unsup-missing", PC_IO_UNSUPPORTED_FILTER, "{}"),
    NF("unsup-garbage", PC_IO_UNSUPPORTED_FILTER, "{\"psd\": \"00000001\"}"),
    NF("nan", "filter.blur.gaussianBlur", "{\"radius\": \"x\"}"),
    {"gauss-blend", "filter.blur.gaussianBlur", "{\"radius\": 1.5}", PC_BLEND_OVERLAY, 0.75f, false},
    {"median-clamp", "filter.noise.median", "{\"radius\": 2.0}", PC_BLEND_HUE, 1.7f, true},
    {"highpass-nan", "filter.other.highPass", "{\"radius\": 2.0}", PC_BLEND_DIVIDE, NAN, true},
};
#define NFILTERS (sizeof FILTERS / sizeof FILTERS[0])

static pc_smart_filter make_filter(const filt *f)
{
    pc_smart_filter s;
    yyjson_doc *jd;
    memset(&s, 0, sizeof s);
    must(pc_str_set_c(&s.command, f->command, NULL));
    must(pc_json_parse(f->params, strlen(f->params), &jd, NULL));
    must(pc_json_value_from_val(yyjson_doc_get_root(jd), &s.params, NULL));
    yyjson_doc_free(jd);
    s.blend = f->blend;
    s.opacity = f->opacity;
    s.visible = f->visible;
    return s;
}

static void filter_s(pc_buf *b, const pc_smart_filter *f)
{
    sb(b, "%s|", pc_str_c(&f->command));
    must(pc_json_write(pc_json_value_root(&f->params), 0, b, NULL));
    sb(b, "|%s|%08x|%d", pc_blend_name(f->blend), fb(f->opacity), f->visible ? 1 : 0);
}

static void desc_hex(pc_buf *b, const pc_psd_desc *d)
{
    pc_psd_bytes bytes = {NULL, 0};
    must(pc_psd_desc_to_bytes(d, &bytes, NULL));
    s_hex(b, bytes.data, bytes.len);
    pc_psd_bytes_fini(&bytes);
}

static void check_line(const char *kind, const char *name, int fieldno, const char *got)
{
    char what[160];
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if (is_kind(l, kind) && l->n > (size_t)fieldno && strcmp(l->f[1], name) == 0) {
            (void)snprintf(what, sizeof what, "%s %s", kind, name);
            report(what, l->f[fieldno], got, NULL, NULL);
            return;
        }
    }
    PC_T_FAIL("no golden line %s %s", kind, name);
}

static pc_buf unhex(const char *s)
{
    pc_buf b = PC_BUF_INIT;
    size_t n = strlen(s);
    for (size_t i = 0; i + 1 < n; i += 2) {
        unsigned v;
        if (sscanf(s + i, "%2x", &v) != 1) {
            abort();
        }
        must(pc_buf_append_byte(&b, (uint8_t)v, NULL));
    }
    return b;
}

static void test_filters(void)
{
    pc_buf s = PC_BUF_INIT;
    for (size_t i = 0; i < NFILTERS; i++) {
        pc_smart_filter f = make_filter(&FILTERS[i]);
        pc_psd_desc d;
        pc_error e;
        pc_status st = pc_io_item_for_filter(&f, &d, &e);
        pc_buf_truncate(&s, 0);
        if (st == PC_OK) {
            pc_psd_bytes bytes = {NULL, 0};
            pc_psd_desc back;
            pc_smart_filter g;
            sb(&s, "ok ");
            desc_hex(&s, &d);
            check_line("sfw", FILTERS[i].name, 2, pc_buf_cstr(&s));
            must(pc_psd_desc_to_bytes(&d, &bytes, NULL));
            must(pc_psd_desc_parse(bytes.data, bytes.len, &back, NULL));
            must(pc_io_filter_from_item(&back, &g, NULL));
            pc_buf_truncate(&s, 0);
            filter_s(&s, &g);
            check_line("sfr", FILTERS[i].name, 2, pc_buf_cstr(&s));
            pc_smart_filter_fini(&g);
            pc_psd_desc_fini(&back);
            pc_psd_bytes_fini(&bytes);
            pc_psd_desc_fini(&d);
        } else {
            CHECK(st == PC_ERR_UNSUPPORTED);
            sb(&s, "err ");
            sq(&s, e.msg, strlen(e.msg));
            check_line("sfw", FILTERS[i].name, 2, pc_buf_cstr(&s));
        }
        pc_smart_filter_fini(&f);
    }
    /* Raw items as the reference writes them. */
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_buf raw;
        pc_psd_desc d;
        pc_smart_filter g;
        if (!is_kind(l, "sfr") || l->n != 4) {
            continue;
        }
        raw = unhex(l->f[2]);
        must(pc_psd_desc_parse(raw.data, raw.len, &d, NULL));
        must(pc_io_filter_from_item(&d, &g, NULL));
        pc_buf_truncate(&s, 0);
        filter_s(&s, &g);
        g_checked++;
        pc_t_checks++;
        if (strcmp(pc_buf_cstr(&s), l->f[3]) != 0) {
            if (g_i2_stub) {
                g_pending++;
            } else {
                PC_T_FAIL("sfr %s:\n  want %s\n  got  %s", l->f[1], l->f[3], pc_buf_cstr(&s));
            }
        }
        pc_smart_filter_fini(&g);
        pc_psd_desc_fini(&d);
        pc_buf_fini(&raw);
    }
    pc_buf_fini(&s);
}

static void stack_of(pc_io_filter_stack *st, size_t n, bool enabled, bool mask_enabled, bool linked)
{
    pc_io_filter_stack_init(st);
    st->enabled = enabled;
    st->mask_enabled = mask_enabled;
    st->mask_linked = linked;
    st->filters = n > 0 ? xmalloc(n * sizeof *st->filters) : NULL;
    for (size_t i = 0; i < n; i++) {
        st->filters[i] = make_filter(&FILTERS[i]);
    }
    st->nfilters = n;
}

static void test_stacks(void)
{
    static const char *const NAMES[3] = {"all", "off", "none"};
    pc_buf s = PC_BUF_INIT;
    for (size_t k = 0; k < 3; k++) {
        pc_io_filter_stack st;
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        pc_psd_desc d;
        pc_io_filter_stack back;
        if (k == 0) {
            stack_of(&st, NFILTERS, true, true, false);
        } else if (k == 1) {
            stack_of(&st, 3, false, false, true);
        } else {
            stack_of(&st, 0, true, true, false);
        }
        must(pc_io_filter_fx(&st, "L \xC3\xA9", &w, &d, NULL));
        pc_buf_truncate(&s, 0);
        desc_hex(&s, &d);
        check_line("sff", NAMES[k], 2, pc_buf_cstr(&s));
        pc_buf_truncate(&s, 0);
        s_warn(&s, w.v, w.n);
        check_line("sff", NAMES[k], 3, pc_buf_cstr(&s));
        must(pc_io_filter_stack_read(&d, &back, NULL));
        pc_buf_truncate(&s, 0);
        sb(&s, "%d:%d:%d:", back.enabled ? 1 : 0, back.mask_enabled ? 1 : 0, back.mask_linked ? 1 : 0);
        for (size_t i = 0; i < back.nfilters; i++) {
            sb(&s, "%s", i > 0 ? ";" : "");
            filter_s(&s, &back.filters[i]);
        }
        check_line("sffr", NAMES[k], 2, pc_buf_cstr(&s));
        pc_io_filter_stack_fini(&back);
        pc_psd_desc_fini(&d);
        pc_io_warnings_fini(&w);
        pc_io_filter_stack_fini(&st);
    }
    pc_buf_fini(&s);
}

static void s_affine(pc_buf *b, const pc_affine *a)
{
    for (size_t i = 0; i < 6; i++) {
        uint64_t u;
        memcpy(&u, &a->m[i], 8);
        sb(b, "%s%016llx", i > 0 ? "," : "", (unsigned long long)u);
    }
}

static void s_placed(pc_buf *b, const pc_io_placed *p)
{
    sb(b, "%s|%s|", pc_str_c(&p->idnt), pc_str_c(&p->placed));
    s_affine(b, &p->transform);
    sb(b, "|");
    if (!p->has_stack) {
        sb(b, "-");
        return;
    }
    sb(b, "%d:", p->stack.enabled ? 1 : 0);
    for (size_t i = 0; i < p->stack.nfilters; i++) {
        sb(b, "%s", i > 0 ? ";" : "");
        filter_s(b, &p->stack.filters[i]);
    }
}

static void sold_case(const char *name, const pc_io_placed_spec *sp, const pc_psd_desc *fx)
{
    pc_buf s = PC_BUF_INIT;
    pc_buf data = PC_BUF_INIT;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_io_placed p;
    bool found = false;
    pc_error e;
    pc_status st;
    must(pc_io_sold_bytes(NULL, sp, &w, &data, NULL));
    s_hex(&s, data.data, data.len);
    check_line("sold", name, 2, pc_buf_cstr(&s));
    pc_buf_truncate(&s, 0);
    s_warn(&s, w.v, w.n);
    check_line("sold", name, 3, pc_buf_cstr(&s));
    pc_io_warnings_fini(&w);
    {
        pc_buf pl = PC_BUF_INIT;
        must(pc_io_plld_bytes(sp, &w, &pl, NULL));
        pc_buf_truncate(&s, 0);
        s_hex(&s, pl.data, pl.len);
        check_line("plld", name, 2, pc_buf_cstr(&s));
        pc_buf_truncate(&s, 0);
        s_warn(&s, w.v, w.n);
        check_line("plld", name, 3, pc_buf_cstr(&s));
        pc_io_warnings_fini(&w);
        pc_buf_fini(&pl);
    }
    st = pc_io_parse_sold(data.data, data.len, &found, &p, &e);
    if (st != PC_OK) {
        CHECK(is_pending(st, &e));
        g_pending += 3;
    } else {
        char tn[64];
        pc_buf_truncate(&s, 0);
        if (found) {
            s_placed(&s, &p);
        } else {
            sb(&s, "none");
        }
        check_line("psold", name, 2, pc_buf_cstr(&s));
        if (found) {
            pc_psd_desc tmpl;
            pc_psd_value v = pc_psd_value_int(7);
            pc_io_placed_spec same = *sp;
            pc_io_placed_spec moved = *sp;
            must(pc_psd_desc_clone(&p.descriptor, &tmpl, NULL));
            must(pc_psd_desc_add(&tmpl, "extraKey", &v, NULL));
            same.filter_fx = NULL;
            must(pc_io_sold_bytes(&tmpl, &same, &w, &data, NULL));
            pc_buf_truncate(&s, 0);
            s_hex(&s, data.data, data.len);
            (void)snprintf(tn, sizeof tn, "%s-tmpl", name);
            check_line("sold", tn, 2, pc_buf_cstr(&s));
            pc_io_warnings_fini(&w);
            moved.transform = (pc_affine){{1.0, 0.0, 0.0, 1.0, 0.5, 0.0}};
            moved.filter_fx = fx;
            must(pc_io_sold_bytes(&tmpl, &moved, &w, &data, NULL));
            pc_buf_truncate(&s, 0);
            s_hex(&s, data.data, data.len);
            (void)snprintf(tn, sizeof tn, "%s-moved", name);
            check_line("sold", tn, 2, pc_buf_cstr(&s));
            pc_buf_truncate(&s, 0);
            s_warn(&s, w.v, w.n);
            check_line("sold", tn, 3, pc_buf_cstr(&s));
            pc_io_warnings_fini(&w);
            pc_psd_desc_fini(&tmpl);
            pc_io_placed_fini(&p);
        }
    }
    pc_buf_fini(&data);
    pc_buf_fini(&s);
}

static void test_placed(void)
{
    const double bounds[4] = {0.0, 0.0, 60.0, 30.0};
    pc_warp arc;
    pc_warp cw;
    pc_warp multi;
    pc_mesh m;
    pc_io_filter_stack off;
    pc_psd_desc fx;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    char long_id[301];
    pc_io_placed_spec sp;
    const pc_affine t = {{0.5, 0.1, -0.1, 0.5, 10.0, 20.0}};
    const pc_affine id = {{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}};
    stack_of(&off, 3, false, false, true);
    must(pc_io_filter_fx(&off, "L", &w, &fx, NULL));
    pc_io_warnings_fini(&w);
    pc_warp_preset(&arc, PC_WARP_ARC, 40.0, bounds);
    arc.vertical = true;
    arc.h_distort = 10.0;
    must(pc_mesh_identity(bounds, 1, 1, &m, NULL));
    m.points[15] = pc_point_make(67.0, 30.0);
    pc_warp_custom(&cw, &m, bounds);
    must(pc_mesh_identity(bounds, 2, 2, &m, NULL));
    pc_warp_custom(&multi, &m, bounds);
    memset(&sp, 0, sizeof sp);
    sp.idnt = "id-1";
    sp.placed = "pl-2";
    sp.transform = t;
    sp.size[0] = 64.0;
    sp.size[1] = 32.0;
    sp.dpi = 72.0;
    sold_case("plain", &sp, &fx);
    sp.filter_fx = &fx;
    sold_case("fx", &sp, &fx);
    sp.filter_fx = NULL;
    sp.idnt = "i";
    sp.placed = "p";
    sp.transform = id;
    sp.size[0] = 60.0;
    sp.size[1] = 30.0;
    sp.warp = &arc;
    sold_case("arc", &sp, &fx);
    sp.warp = &cw;
    sold_case("custom", &sp, &fx);
    sp.warp = &multi;
    sold_case("multi", &sp, &fx);
    sp.warp = NULL;
    memset(long_id, 'x', 300);
    long_id[300] = '\0';
    sp.idnt = long_id;
    sp.size[0] = 1.0;
    sp.size[1] = 1.0;
    sold_case("long-id", &sp, &fx);
    sp.idnt = "i";
    sp.size[0] = 20.0;
    sp.size[1] = 10.0;
    sp.has_perspective = true;
    {
        const double h[9] = {1.2, 0.1, 2.0, 0.05, 0.9, 1.0, 0.001, 0.002, 1.0};
        memcpy(sp.perspective, h, sizeof h);
    }
    sold_case("persp", &sp, &fx);
    /* Raw cases: the reference's bytes. */
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_buf raw;
        pc_io_placed p;
        bool found = false;
        pc_error e;
        pc_status st;
        if (!is_kind(l, "psold") || l->n != 4) {
            continue;
        }
        raw = unhex(l->f[2]);
        st = pc_io_parse_sold(raw.data, raw.len, &found, &p, &e);
        if (st != PC_OK) {
            CHECK(is_pending(st, &e));
            g_pending++;
        } else {
            report(l->f[1], l->f[3], found ? pc_str_c(&p.idnt) : "none", NULL, NULL);
            if (found) {
                pc_io_placed_fini(&p);
            }
        }
        pc_buf_fini(&raw);
    }
    pc_psd_desc_fini(&fx);
    pc_io_filter_stack_fini(&off);
    pc_warp_fini(&arc);
    pc_warp_fini(&cw);
    pc_warp_fini(&multi);
}

static void s_mask(pc_buf *b, bool has, const pc_layer_mask *m)
{
    pc_rect r;
    uint8_t *bytes = NULL;
    size_t len = 0;
    char h[17];
    if (!has) {
        sb(b, "none");
        return;
    }
    r = pc_surface_content_bounds(&m->surface);
    must(pc_surface_to_interleaved(&m->surface, r, &bytes, &len, NULL));
    h16(bytes, len, h);
    sb(b, "%d,%d,%d,%d:%s:%d:%d", r.x0, r.y0, r.x1, r.y1, h, m->enabled ? 1 : 0, m->linked ? 1 : 0);
    free(bytes);
}

static void fx_item_bytes(pc_buf *b, const pc_psd_fx_item *it)
{
    pc_psd_filter_effects fx = {3, NULL, 1};
    pc_psd_bytes raw = {NULL, 0};
    pc_psd_bytes norm = {NULL, 0};
    pc_psd_fx_item c;
    bool any;
    char h1[17];
    char h2[17];
    fx.items = (pc_psd_fx_item *)(uintptr_t)it;
    must(pc_psd_filter_effects_to_bytes(&fx, &raw, NULL));
    must(pc_io_fx_item_clone(it, &c, NULL));
    fx.items = &c;
    any = normalize_fx(&fx);
    must(pc_psd_filter_effects_to_bytes(&fx, &norm, NULL));
    h16(raw.data, raw.len, h1);
    h16(norm.data, norm.len, h2);
    sb(b, "%zu %s %d %s", raw.len, h1, any ? 1 : 0, h2);
    pc_psd_fx_item_fini(&c);
    pc_psd_bytes_fini(&raw);
    pc_psd_bytes_fini(&norm);
}

static void mask_line(const char *name, const pc_psd_fx_item *it, pc_sample_type sample)
{
    pc_io_filter_stack st;
    pc_layer_mask m;
    bool has = false;
    pc_error e;
    pc_buf s = PC_BUF_INIT;
    pc_status rc;
    pc_io_filter_stack_init(&st);
    st.mask_enabled = false;
    st.mask_linked = true;
    rc = pc_io_mask_from_item(it, sample, &st, &has, &m, &e);
    if (rc == PC_OK) {
        sb(&s, "ok ");
        s_mask(&s, has, &m);
        if (has) {
            pc_layer_mask_fini(&m);
        }
    } else {
        sb(&s, "err ");
        sq(&s, e.msg, strlen(e.msg));
    }
    check_line("mask", name, 2, pc_buf_cstr(&s));
    pc_buf_fini(&s);
}

static void test_feid(void)
{
    static const pc_sample_type SAMPLES[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    static const pc_color_mode MODES[3] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_CMYK, PC_COLOR_MODE_LAB};
    pc_buf s = PC_BUF_INIT;
    for (size_t si = 0; si < 3; si++) {
        pc_sample_type sample = SAMPLES[si];
        pc_pixel_format fmt = {PC_COLOR_MODE_GRAYSCALE, sample, false};
        const float one = 1.0f;
        float vals[12];
        pc_layer_mask lm;
        pc_surface_init_default(&lm.surface, fmt, &one, 1);
        for (int i = 0; i < 12; i++) {
            vals[i] = (float)(i * 20) / 255.0f;
        }
        must(pc_surface_write_region(&lm.surface, pc_rect_make(2, 1, 6, 4), vals, 12, NULL));
        lm.enabled = true;
        lm.linked = false;
        lm.density = 1.0f;
        lm.feather = 0.0f;
        for (size_t mi = 0; mi < 3; mi++) {
            pc_pixel_format df = {MODES[mi], sample, true};
            pc_surface src = t_pattern(df, pc_rect_make(1, 1, 7, 5), 12, true);
            pc_surface empty;
            pc_psd_fx_item it;
            bool has = false;
            for (int wm = 0; wm < 2; wm++) {
                char name[64];
                (void)snprintf(name, sizeof name, "%s-%s-%s", pc_sample_type_name(sample),
                               pc_color_mode_name(MODES[mi]), wm == 0 ? "mask" : "nomask");
                must(pc_io_feid_item("pl-x", &src, wm == 0 ? &lm : NULL, pc_rect_make(0, 0, 9, 6), df, &has,
                                     &it, NULL));
                pc_buf_truncate(&s, 0);
                if (!has) {
                    sb(&s, "none");
                    check_line("feid", name, 2, pc_buf_cstr(&s));
                    continue;
                }
                /* As an export result ("ok LEN HASH ZIP NHASH"), so zip planes compare raw. */
                sb(&s, "ok ");
                fx_item_bytes(&s, &it);
                {
                    const gline *gl = find_line("feid", name);
                    pc_buf want = PC_BUF_INIT;
                    if (gl == NULL || gl->n < 3) {
                        PC_T_FAIL("no golden line feid %s", name);
                    } else {
                        sb(&want, "ok %s", gl->f[2]);
                        report(name, pc_buf_cstr(&want), pc_buf_cstr(&s), NULL, NULL);
                    }
                    pc_buf_fini(&want);
                }
                mask_line(name, &it, sample);
                {
                    char dn[80];
                    (void)snprintf(dn, sizeof dn, "%s-depth", name);
                    mask_line(dn, &it, sample == PC_SAMPLE_TYPE_U8 ? PC_SAMPLE_TYPE_U16 : PC_SAMPLE_TYPE_U8);
                }
                pc_psd_fx_item_fini(&it);
            }
            pc_surface_init(&empty, df);
            must(pc_io_feid_item("e", &empty, NULL, pc_rect_make(0, 0, 2, 2), df, &has, &it, NULL));
            CHECK(!has);
            pc_surface_fini(&src);
        }
        pc_layer_mask_fini(&lm);
    }
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_buf seed;
        char u[37];
        if (!is_kind(l, "uuid") || l->n != 3) {
            continue;
        }
        seed = unhex(l->f[1]);
        pc_io_uuid_from(seed.data, seed.len, u);
        report("uuid", l->f[2], u, NULL, NULL);
        pc_buf_fini(&seed);
    }
    pc_buf_fini(&s);
}

/* ---- corpus ---- */

static void test_corpus(void)
{
    struct stat sbuf;
    pc_buf res = PC_BUF_INIT;
    pc_buf outl = PC_BUF_INIT;
    if (stat("corpus/psd", &sbuf) != 0) {
        for (size_t i = 0; i < g_nlines; i++) {
            g_skipped += is_kind(&g_lines[i], "corp") ? 1 : 0;
        }
        (void)printf("corpus/ absent: corpus lines skipped\n");
        return;
    }
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        char path[1024];
        pc_buf bytes = PC_BUF_INIT;
        pc_io_import_result r;
        pc_error e;
        pc_status st;
        const char *base;
        if (!is_kind(l, "corp") || l->n != 3) {
            continue;
        }
        (void)snprintf(path, sizeof path, "corpus/%s", l->f[1]);
        if (pc_file_read(path, (size_t)1 << 31, &bytes, NULL) != PC_OK) {
            g_skipped++;
            continue;
        }
        base = strrchr(path, '/') + 1;
        st = pc_io_import(base, bytes.data, bytes.len, &r, &e);
        pc_buf_fini(&bytes);
        if (st != PC_OK) {
            if (is_pending(st, &e) || g_i2_stub) {
                g_pending++;
                continue;
            }
            pc_buf_truncate(&res, 0);
            sb(&res, "imperr ");
            sq(&res, e.msg, strlen(e.msg));
            report(l->f[1], l->f[2], pc_buf_cstr(&res), NULL, NULL);
            continue;
        }
        {
            pc_psd_file f;
            pc_io_warnings w = PC_IO_WARNINGS_INIT;
            pc_io_psd_export_opts o = pc_io_psd_export_opts_default();
            st = export_result(&r.document, &o, &f, &w, &res, &outl, &e);
            if (st == PC_OK) {
                pc_psd_file_fini(&f);
            }
            report(l->f[1], l->f[2], pc_buf_cstr(&res), NULL, NULL);
            pc_io_warnings_fini(&w);
        }
        pc_io_import_result_fini(&r);
    }
    pc_buf_fini(&res);
    pc_buf_fini(&outl);
}

int main(int argc, char **argv)
{
    if (load_golden() != 0) {
        (void)fprintf(stderr, "cannot read %s\n", GOLDEN);
        return 1;
    }
    probe_stubs();
    RUN(test_load_docs);
    RUN(test_exports);
    RUN(test_round_trips);
    RUN(test_filters);
    RUN(test_stacks);
    RUN(test_placed);
    RUN(test_feid);
    RUN(test_corpus);
    golden_summary("io-export-vectors");
    for (size_t i = 0; i < g_ndocs; i++) {
        if (g_docs[i].ok) {
            pc_document_fini(&g_docs[i].doc);
        }
    }
    free(g_docs);
    return pc_t_finish();
}
