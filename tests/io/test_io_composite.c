/* Port of crates/io/tests/composite.rs and corpus_regressions.rs (synthetic; no corpus needed).
 *
 * composite.rs: the merged image a PSD stores must match the compositor's rendering of the
 * imported document (our exporter's merged image, and the independent PsdBuilder's).
 * corpus_regressions.rs: synthetic reproductions of failures the psd-tools corpus found.
 * The corpus halves (corpus.rs, tiff_corpus.rs) are in test_io_corpus.c. */
#include "harness.h"

#include "codecs/codecs.h"
#include "codecs/image.h"
#include "psd/builder.h"
#include "psd_rt.h"
#include "vector/vector.h"

/* ================================================================ shared helpers */

typedef float px4[4];

/* common::max_diff on plain arrays (pixels where either shows; premultiplied colour, alpha). */
static float max_diff(const px4 *a, size_t na, const px4 *b, size_t nb)
{
    float m = 0.0f;
    for (size_t i = 0; i < na && i < nb; i++) {
        const float *p = a[i];
        const float *q = b[i];
        if (p[3] <= 0.0f && q[3] <= 0.0f) {
            continue;
        }
        for (int c = 0; c < 4; c++) {
            float x = c < 3 ? p[c] * p[3] : p[c];
            float y = c < 3 ? q[c] * q[3] : q[c];
            m = fmaxf(m, fabsf(x - y));
        }
    }
    return m;
}

static pc_compose_buffer flatten(const pc_document *d)
{
    pc_compose_buffer b;
    pc_error e;
    if (pc_compose_flatten(d, &b, &e) != PC_OK) {
        (void)fprintf(stderr, "flatten: %s\n", e.msg);
        abort();
    }
    return b;
}

/* ================================================================ composite.rs */

#define TOL (1.0f / 255.0f + 1e-4f)

static void check_export_oracle(const pc_document *d, float tol)
{
    pc_buf bytes = rt_export(d, "x.psd", NULL);
    pc_psd_file f;
    float (*merged)[4] = NULL;
    size_t nm = 0;
    pc_document d2;
    pc_compose_buffer ours;
    float m;
    must(pc_psd_file_parse(bytes.data, bytes.len, &f, NULL));
    must(pc_io_merged_composite(&f, &merged, &nm, NULL));
    d2 = rt_import("x.psd", &bytes);
    ours = flatten(&d2);
    if (d->mode == PC_COLOR_MODE_CMYK) {
        /* The merged CMYK image is the RGB composite separated through the CMYK profile, which
         * gamut-maps colours that blend modes push outside CMYK: project ours the same way. */
        pc_pixel_format fmt = pc_document_pixel_format(d);
        for (size_t i = 0; i < ours.len; i++) {
            float v[PC_RASTER_MAX_CHANNELS];
            size_t n = pc_from_rgba(fmt, ours.px[i], v);
            float out[4];
            pc_to_rgba(fmt, v, n, out);
            memcpy(ours.px[i], out, sizeof out);
        }
    }
    m = max_diff((const px4 *)ours.px, ours.len, (const px4 *)merged, nm);
    pc_t_checks++;
    if (!(m <= tol)) {
        PC_T_FAIL("max diff %g > %g", (double)m, (double)tol);
    }
    pc_compose_buffer_fini(&ours);
    pc_document_fini(&d2);
    free(merged);
    pc_psd_file_fini(&f);
    pc_buf_fini(&bytes);
}

static void oracle(pc_color_mode mode, pc_sample_type depth, bool all, float tol)
{
    pc_document d = all ? t_gen_doc_all(mode, depth) : t_gen_doc(mode, depth);
    check_export_oracle(&d, tol);
    pc_document_fini(&d);
}

static void test_oracle_rgb8(void)
{
    oracle(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true, TOL);
}
static void test_oracle_rgb16(void)
{
    oracle(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true, TOL);
}
static void test_oracle_rgb32(void)
{
    oracle(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true, TOL);
}
static void test_oracle_gray8(void)
{
    oracle(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, true, TOL);
}
static void test_oracle_gray16(void)
{
    oracle(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, true, TOL);
}
/* CMYK/Lab merged data is converted from the RGB composite (CMYK through the built-in profile),
 * so the stored merged image differs by a quantization round trip through the model. */
static void test_oracle_cmyk8(void)
{
    oracle(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, false, 3.0f / 255.0f);
}
/* Lab: one 8-bit Lab quantization step is amplified up to ~15/255 near the sRGB gamut edge. */
static void test_oracle_lab8(void)
{
    oracle(PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U8, false, 16.0f / 255.0f);
}

/* Interleaved samples of PixelData built from straight RGBA pixels (to_pixels). */
typedef struct pixdata {
    pc_psd_pixels px;
    void *mem;
} pixdata;

static pixdata to_pixels(pc_pixel_format fmt, pc_color_mode mode, uint16_t depth, const px4 *px, size_t n)
{
    pixdata r;
    size_t ch = pc_pixel_format_channels(fmt);
    size_t total = n * ch;
    bool gray = mode == PC_COLOR_MODE_GRAYSCALE;
    r.mem = xmalloc(total * (depth == 8 ? 1 : 2));
    for (size_t i = 0; i < n; i++) {
        float v[PC_RASTER_MAX_CHANNELS];
        size_t k = pc_from_rgba(fmt, px[i], v);
        for (size_t c = 0; c < k && c < ch; c++) {
            if (depth == 8) {
                ((uint8_t *)r.mem)[i * ch + c] = (uint8_t)lroundf(fminf(fmaxf(v[c], 0.0f), 1.0f) * 255.0f);
            } else {
                ((uint16_t *)r.mem)[i * ch + c] =
                    (uint16_t)lroundf(fminf(fmaxf(v[c], 0.0f), 1.0f) * 65535.0f);
            }
        }
    }
    r.px.kind = gray ? (depth == 8 ? PC_PSD_PX_GRAYA8 : PC_PSD_PX_GRAYA16)
                     : (depth == 8 ? PC_PSD_PX_RGBA8 : PC_PSD_PX_RGBA16);
    r.px.data = r.mem;
    r.px.len = total;
    return r;
}

/* gen_doc with Features { groups: false, masks, all_blends }: PIXELS without the two groups. */
static pc_document gen_doc_no_groups(pc_color_mode mode, pc_sample_type depth)
{
    pc_document d = t_gen_doc(mode, depth);
    for (size_t i = d.nlayers; i-- > 0;) {
        if (d.layers[i].content.kind == PC_LAYER_GROUP) {
            pc_layer gone;
            must(pc_document_remove(&d, d.layers[i].id, &gone, NULL));
            pc_layer_fini(&gone);
        }
    }
    return d;
}

static void builder_oracle(uint16_t depth, pc_color_mode mode)
{
    pc_sample_type sample = depth == 16 ? PC_SAMPLE_TYPE_U16 : PC_SAMPLE_TYPE_U8;
    pc_document d = gen_doc_no_groups(mode, sample);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    pc_compose_buffer flat = flatten(&d);
    pc_psd_builder *b = pc_psd_builder_new(d.size.width, d.size.height);
    pc_psd_bytes bytes = {NULL, 0};
    pc_psd_file f;
    float (*merged)[4] = NULL;
    size_t nm = 0;
    pc_buf copy = PC_BUF_INIT;
    pc_document d2;
    pc_compose_buffer ours;
    pixdata comp;
    float m;
    if (b == NULL) {
        abort();
    }
    pc_psd_builder_depth(b, depth);
    pc_psd_builder_color_mode(b, mode == PC_COLOR_MODE_GRAYSCALE ? PC_PSD_MODE_GRAYSCALE : PC_PSD_MODE_RGB);
    for (size_t i = 0; i < d.nlayers; i++) {
        const pc_layer *l = &d.layers[i];
        const pc_surface *s = pc_layer_surface(l);
        pc_rect r;
        px4 *buf = NULL;
        size_t nbuf = 0;
        pixdata pd;
        pc_psd_layer_spec spec;
        pc_psd_mask_spec ms;
        uint8_t *mdata = NULL;
        if (s == NULL) {
            abort();
        }
        r = pc_surface_content_bounds(s);
        if (!pc_rect_is_empty(r)) {
            pc_pixel_format sf = pc_surface_format(s);
            size_t ch = pc_pixel_format_channels(sf);
            float *vals = NULL;
            size_t nv = 0;
            must(pc_surface_read_region(s, r, &vals, &nv, NULL));
            nbuf = nv / ch;
            buf = xmalloc(nbuf * sizeof *buf);
            for (size_t k = 0; k < nbuf; k++) {
                pc_to_rgba(sf, vals + k * ch, ch, buf[k]);
            }
            free(vals);
        }
        pd = to_pixels(fmt, mode, depth, (const px4 *)buf, nbuf);
        spec = pc_psd_layer_spec_make(pc_str_c(&l->name), r.x0, r.y0, pc_rect_width(r), pc_rect_height(r),
                                      pd.px);
        memcpy(spec.blend, pc_blend_psd_key(l->blend), 4);
        spec.opacity = (uint8_t)lroundf(l->opacity * 255.0f);
        spec.has_fill_opacity = true;
        spec.fill_opacity = (uint8_t)lroundf(l->fill_opacity * 255.0f);
        spec.visible = l->visible;
        spec.clipping = l->clipped;
        if (l->has_mask) {
            pc_rect mr = pc_surface_content_bounds(&l->mask.surface);
            float def[PC_RASTER_MAX_CHANNELS];
            float *mv = NULL;
            size_t nmv = 0;
            if (!pc_rect_is_empty(mr)) {
                must(pc_surface_read_region(&l->mask.surface, mr, &mv, &nmv, NULL));
            }
            mdata = xmalloc(nmv);
            for (size_t k = 0; k < nmv; k++) {
                mdata[k] = (uint8_t)lroundf(mv[k] * 255.0f);
            }
            free(mv);
            pc_surface_default_pixel(&l->mask.surface, def);
            ms.rect = (pc_psd_rect){mr.y0, mr.x0, mr.y1, mr.x1};
            ms.data = mdata;
            ms.len = nmv;
            ms.default_color = (uint8_t)lroundf(def[0] * 255.0f);
            ms.disabled = !l->mask.enabled;
            spec.mask = &ms;
        }
        must(pc_psd_builder_push_layer(b, &spec, NULL));
        free(mdata);
        free(pd.mem);
        free(buf);
    }
    comp = to_pixels(fmt, mode, depth, (const px4 *)flat.px, flat.len);
    must(pc_psd_builder_composite(b, comp.px, NULL));
    free(comp.mem);
    must(pc_psd_builder_to_bytes(b, &bytes, NULL));
    must(pc_psd_file_parse(bytes.data, bytes.len, &f, NULL));
    must(pc_io_merged_composite(&f, &merged, &nm, NULL));
    must(pc_buf_append(&copy, bytes.data, bytes.len, NULL));
    d2 = rt_import("b.psd", &copy);
    ours = flatten(&d2);
    m = max_diff((const px4 *)ours.px, ours.len, (const px4 *)merged, nm);
    /* Density/feather of masks are not written by the builder; the generated doc uses them
     * only on a disabled mask, so results must match. */
    pc_t_checks++;
    if (!(m <= 2.0f / 255.0f)) {
        PC_T_FAIL("builder oracle %s %u: max diff %g", pc_color_mode_name(mode), (unsigned)depth, (double)m);
    }
    pc_compose_buffer_fini(&ours);
    pc_document_fini(&d2);
    pc_buf_fini(&copy);
    free(merged);
    pc_psd_file_fini(&f);
    pc_psd_bytes_fini(&bytes);
    pc_psd_builder_free(b);
    pc_compose_buffer_fini(&flat);
    pc_document_fini(&d);
}

static void test_builder_oracle_rgb8(void)
{
    builder_oracle(8, PC_COLOR_MODE_RGB);
}
static void test_builder_oracle_rgb16(void)
{
    builder_oracle(16, PC_COLOR_MODE_RGB);
}
static void test_builder_oracle_gray8(void)
{
    builder_oracle(8, PC_COLOR_MODE_GRAYSCALE);
}
static void test_builder_oracle_gray16(void)
{
    builder_oracle(16, PC_COLOR_MODE_GRAYSCALE);
}

static void test_every_blend_mode_single_layer_oracle(void)
{
    for (size_t i = 0; i < PC_BLEND_LAYER_MODE_COUNT; i++) {
        pc_blend_mode m = PC_BLEND_LAYER_MODES[i];
        pc_document d;
        pc_pixel_format fmt;
        pc_layer l;
        if (m == PC_BLEND_DISSOLVE) {
            continue;
        }
        d = t_doc("b", 8, 8, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        fmt = pc_document_pixel_format(&d);
        l = t_raster("bg", fmt, pc_document_bounds(&d), 1, false);
        t_push(&d, &l);
        l = t_raster("top", fmt, pc_rect_make(2, 2, 7, 7), 9, true);
        l.blend = m;
        l.opacity = g255(190);
        t_push(&d, &l);
        check_export_oracle(&d, TOL);
        pc_document_fini(&d);
    }
}

static void test_hidden_and_clipped_affect_composite(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document d2;
    pc_psd_file f;
    float (*merged)[4] = NULL;
    size_t nm = 0;
    pc_compose_buffer other;
    must(pc_io_document_to_psd(&d, &f, NULL));
    must(pc_io_merged_composite(&f, &merged, &nm, NULL));
    must(pc_document_clone(&d, &d2, NULL));
    for (size_t i = 0; i < d2.nlayers; i++) {
        d2.layers[i].visible = true;
    }
    other = flatten(&d2);
    /* unhiding a layer must change the composite */
    CHECK(max_diff((const px4 *)other.px, other.len, (const px4 *)merged, nm) > 0.0f);
    pc_compose_buffer_fini(&other);
    pc_document_fini(&d2);
    free(merged);
    pc_psd_file_fini(&f);
    pc_document_fini(&d);
}

static void test_merged_alpha_written_when_transparent(void)
{
    pc_document d = t_doc("t", 4, 4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer l = t_raster("half", pc_document_pixel_format(&d), pc_rect_make(0, 0, 2, 4), 3, true);
    pc_psd_file f;
    float (*merged)[4] = NULL;
    size_t nm = 0;
    t_push(&d, &l);
    must(pc_io_document_to_psd(&d, &f, NULL));
    CHECK_EQ_UINT(f.header.channels, 4);
    CHECK(pc_psd_file_merged_has_alpha(&f));
    must(pc_io_merged_composite(&f, &merged, &nm, NULL));
    CHECK(nm > 3);
    if (nm > 3) {
        CHECK(merged[3][3] == 0.0f);
    }
    free(merged);
    pc_psd_file_fini(&f);
    pc_document_fini(&d);

    /* U8 rounding must not erase transparency that is representable in the exported U16 plane. */
    {
        pc_document near = t_doc("near opaque", 1, 1, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16);
        pc_layer layer;
        const float px[4] = {1.0f, 0.0f, 0.0f, 65500.0f / 65535.0f};
        pc_buf out;
        pc_psd_file file;
        pc_psd_bytes samples = {NULL, 0};
        must(pc_layer_raster(&layer, "pixel", pc_document_pixel_format(&near), NULL));
        must(pc_surface_fill_rect(pc_layer_surface_mut(&layer), pc_document_bounds(&near), px, 4, NULL));
        t_push(&near, &layer);
        out = rt_export(&near, "near.psd", NULL);
        must(pc_psd_file_parse(out.data, out.len, &file, NULL));
        CHECK_EQ_UINT(file.header.channels, 4);
        CHECK(pc_psd_file_merged_has_alpha(&file));
        must(pc_psd_file_decode_merged(&file, &samples, NULL));
        CHECK(samples.len >= 8);
        if (samples.len >= 8) {
            CHECK_EQ_UINT((unsigned)samples.data[6] << 8 | samples.data[7], 65500);
        }
        pc_psd_bytes_fini(&samples);
        pc_psd_file_fini(&file);
        pc_buf_fini(&out);
        pc_document_fini(&near);
    }
}

/* ================================================================ corpus_regressions.rs */

static pc_document reg_doc(pc_sample_type depth)
{
    pc_document d;
    must(pc_document_with_background(&d, "t", pc_size_make(32, 32), PC_COLOR_MODE_RGB, depth, PC_COLOR_WHITE,
                                     NULL));
    return d;
}

static pc_path square(double x0, double y0, double x1, double y1)
{
    const double pts[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    pc_path p;
    memset(&p, 0, sizeof p);
    p.subpaths = xmalloc(sizeof *p.subpaths);
    must(pc_subpath_polygon(pts, 4, &p.subpaths[0], NULL));
    p.nsubpaths = 1;
    p.fill_rule = PC_FILL_RULE_NONZERO;
    p.inverted = false;
    return p;
}

static pc_psd_file export_psd(const pc_document *d)
{
    pc_buf b = rt_export(d, "x.psd", NULL);
    pc_psd_file f;
    must(pc_psd_file_parse(b.data, b.len, &f, NULL));
    pc_buf_fini(&b);
    return f;
}

static pc_io_import_result reimport(const pc_psd_file *f)
{
    pc_psd_bytes b = {NULL, 0};
    pc_io_import_result r;
    pc_error e;
    must(pc_psd_file_to_bytes(f, &b, NULL));
    if (pc_io_import("x.psd", b.data, b.len, &r, &e) != PC_OK) {
        (void)fprintf(stderr, "reimport: %s\n", e.msg);
        abort();
    }
    pc_psd_bytes_fini(&b);
    return r;
}

static pc_psd_layer *last_record(pc_psd_file *f)
{
    if (!f->has_layer_info || f->layer_info.n == 0) {
        abort();
    }
    return &f->layer_info.layers[f->layer_info.n - 1];
}

static void reencode(pc_psd_channel *ch, uint16_t compression, const uint8_t *data, size_t len, size_t w,
                     size_t h, uint16_t bits)
{
    pc_psd_channel n;
    must(pc_psd_channel_encode(ch->id, compression, data, len, w, h, bits, PC_PSD_V1, &n, NULL));
    pc_psd_bytes_fini(&ch->data);
    *ch = n;
}

/* A shape layer named name with path (taken over), a solid black fill and its rendered cache. */
static pc_layer shape_layer(const pc_document *d, const char *name, pc_path path)
{
    pc_layer_content c;
    pc_layer l;
    pc_surface cache;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SHAPE;
    c.u.shape.path = path;
    c.u.shape.has_fill = true;
    c.u.shape.fill = pc_fill_solid(PC_COLOR_BLACK);
    must(pc_vec_render_shape(&c.u.shape, pc_document_pixel_format(d), pc_document_bounds(d), &cache, NULL));
    c.u.shape.has_cache = true;
    c.u.shape.cache = cache;
    must(pc_layer_init(&l, name, &c, NULL));
    return l;
}

/* psd-tools 32bit.psd, 300dpi.psd, transparentbg.psd, vector-mask2.psd: shape layers whose record
 * stores no pixels (empty rect; every 32-bit shape layer) imported as invisible. They are now
 * rendered from their path and fill. */
static void test_shape_layer_without_stored_pixels_renders_from_its_path(void)
{
    const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t k = 0; k < 3; k++) {
        pc_document d = reg_doc(depths[k]);
        pc_layer l = shape_layer(&d, "Shape 1", square(8.0, 8.0, 24.0, 24.0));
        pc_psd_file file;
        pc_psd_layer *rec;
        uint16_t bits;
        pc_io_import_result imp;
        pc_compose_buffer f;
        const float *in;
        const float *out;
        t_push(&d, &l);
        file = export_psd(&d);
        /* Drop the shape record's pixels, as Photoshop does for 32-bit documents. */
        bits = file.header.depth;
        rec = last_record(&file);
        CHECK(pc_psd_layer_block(rec, "vmsk") != NULL || pc_psd_layer_block(rec, "vsms") != NULL);
        rec->rect = (pc_psd_rect){0, 0, 0, 0};
        for (size_t i = 0; i < rec->nchannels; i++) {
            reencode(&rec->channels[i], PC_PSD_RAW, NULL, 0, 0, 0, bits);
        }
        imp = reimport(&file);
        f = flatten(&imp.document);
        in = pc_compose_buffer_get(&f, 16, 16);
        out = pc_compose_buffer_get(&f, 2, 2);
        pc_t_checks++;
        if (in == NULL || !(in[0] < 0.01f)) {
            PC_T_FAIL("%s: shape interior renders black: %g", pc_sample_type_name(depths[k]),
                      in != NULL ? (double)in[0] : -1.0);
        }
        CHECK(out != NULL && out[0] > 0.99f); /* outside stays white */
        pc_compose_buffer_fini(&f);
        pc_io_import_result_fini(&imp);
        pc_psd_file_fini(&file);
        pc_document_fini(&d);
    }
}

/* psd-tools mask-density-vectormask.psd, mask_parameters.psd: vector mask density and feather
 * were dropped on export (only the user mask's parameters were written). */
static void test_vector_mask_density_and_feather_survive_export(void)
{
    for (int with_user_mask = 0; with_user_mask < 2; with_user_mask++) {
        pc_document d = reg_doc(PC_SAMPLE_TYPE_U8);
        pc_layer l;
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        pc_path p = square(0.0, 0.0, 16.0, 32.0);
        pc_io_export_result r;
        pc_io_import_result b;
        pc_error e;
        const pc_layer *bl;
        must(pc_layer_raster(&l, "pix", pc_document_pixel_format(&d), NULL));
        must(pc_surface_fill_rect(pc_layer_surface_mut(&l), pc_rect_make(0, 0, 32, 32), black, 4, NULL));
        pc_vector_mask_init(&l.vector_mask, &p);
        l.has_vector_mask = true;
        l.vector_mask.density = 128.0f / 255.0f;
        l.vector_mask.feather = 1.5f;
        if (with_user_mask) {
            pc_layer_mask_reveal_all(&l.mask);
            l.mask.density = 204.0f / 255.0f;
            l.mask.feather = 2.5f;
            l.has_mask = true;
        }
        t_push(&d, &l);
        must(pc_io_export(&d, "x.psd", NULL, &r, NULL));
        CHECK(!pc_io_warnings_any(&r.warnings, "density"));
        if (pc_io_import("x.psd", r.bytes, r.len, &b, &e) != PC_OK) {
            PC_T_FAIL("import: %s", e.msg);
            pc_io_export_result_fini(&r);
            pc_document_fini(&d);
            continue;
        }
        CHECK(b.document.nlayers >= 2);
        if (b.document.nlayers >= 2) {
            bl = &b.document.layers[1];
            CHECK(bl->has_vector_mask);
            if (bl->has_vector_mask) {
                CHECK(bl->vector_mask.density == 128.0f / 255.0f);
                CHECK(bl->vector_mask.feather == 1.5f);
            }
            CHECK_EQ_INT(bl->has_mask, with_user_mask);
            if (bl->has_mask) {
                CHECK(bl->mask.density == 204.0f / 255.0f);
                CHECK(bl->mask.feather == 2.5f);
            }
        }
        pc_io_import_result_fini(&b);
        pc_io_export_result_fini(&r);
        pc_document_fini(&d);
    }
}

/* psd-tools layer_mask_data.psd, mask-density-layervectormask.psd: a layer with both a user mask
 * and a vector mask stores the "real" user mask (channel -3) and mask parameters; the real-mask
 * fields precede the parameters, so the -3 channel failed to decode. */
static void test_real_user_mask_with_parameters_is_decoded(void)
{
    pc_document d = reg_doc(PC_SAMPLE_TYPE_U8);
    pc_layer l;
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    pc_path p = square(0.0, 0.0, 32.0, 32.0);
    pc_psd_file file;
    pc_psd_layer *rec;
    pc_psd_layer_mask *pm;
    pc_psd_channel ch;
    uint8_t zeros[16 * 32];
    pc_io_import_result imp;
    must(pc_layer_raster(&l, "pix", pc_document_pixel_format(&d), NULL));
    must(pc_surface_fill_rect(pc_layer_surface_mut(&l), pc_rect_make(0, 0, 32, 32), black, 4, NULL));
    pc_layer_mask_reveal_all(&l.mask);
    l.mask.density = 0.5f;
    l.has_mask = true;
    pc_vector_mask_init(&l.vector_mask, &p);
    l.vector_mask.density = 0.5f;
    l.has_vector_mask = true;
    t_push(&d, &l);
    file = export_psd(&d);
    /* Add a real user mask hiding the left half (0) inside its rect, revealing elsewhere. */
    rec = last_record(&file);
    if (rec->mask.kind != PC_PSD_MASK_PARSED) {
        PC_T_FAIL("mask record");
        pc_psd_file_fini(&file);
        pc_document_fini(&d);
        return;
    }
    pm = &rec->mask.mask;
    CHECK(pm->has_params);
    pm->has_real = true;
    pm->real = (pc_psd_real_mask){0, 255, (pc_psd_rect){0, 0, 32, 16}};
    pc_psd_bytes_fini(&pm->trailing);
    memset(zeros, 0, sizeof zeros);
    must(pc_psd_channel_encode(PC_PSD_CH_REAL_USER_MASK, PC_PSD_RLE, zeros, sizeof zeros, 16, 32, 8,
                               PC_PSD_V1, &ch, NULL));
    must(pc_psd_layer_push_channel(rec, &ch, NULL));
    imp = reimport(&file);
    CHECK(!pc_io_warnings_any(&imp.warnings, "could not be decoded"));
    CHECK(imp.document.nlayers >= 2 && imp.document.layers[1].has_mask);
    if (imp.document.nlayers >= 2 && imp.document.layers[1].has_mask) {
        const pc_layer *il = &imp.document.layers[1];
        const pc_layer_mask *mask = &il->mask;
        /* Hidden at density 0.5: 1 - 0.5 * (1 - 0). */
        CHECK_NEAR(pc_layer_mask_value(mask, 4, 4), 0.5, 0.01);
        CHECK(pc_layer_mask_value(mask, 24, 4) == 1.0f); /* and reveals outside its rect */
        CHECK(mask->density == 128.0f / 255.0f);
        CHECK(il->has_vector_mask && il->vector_mask.density == 128.0f / 255.0f);
    }
    pc_io_import_result_fini(&imp);
    pc_psd_file_fini(&file);
    pc_document_fini(&d);
}

/* psd-tools layer_mask_data.psd, mask-parameters-no-real-channel.psd: a shape layer whose vector
 * mask has a density (or feather) shows its fill beyond the path at 1 - density; the stored
 * pixels hold only the shape. It imports as a fill layer with a soft vector mask. */
static void test_shape_with_vector_mask_density_imports_as_fill_with_soft_mask(void)
{
    pc_document d = reg_doc(PC_SAMPLE_TYPE_U8);
    pc_layer_content c;
    pc_layer l;
    pc_path p = square(8.0, 8.0, 24.0, 24.0);
    pc_psd_file file;
    pc_psd_layer *rec;
    pc_io_import_result imp;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_FILL;
    c.u.fill = pc_fill_solid(PC_COLOR_BLACK);
    must(pc_layer_init(&l, "Ellipse", &c, NULL));
    pc_vector_mask_init(&l.vector_mask, &p);
    l.vector_mask.density = 0.8f;
    l.has_vector_mask = true;
    t_push(&d, &l);
    file = export_psd(&d);
    /* Photoshop stores the shape alone: replace the record's pixels with the path's coverage. */
    rec = last_record(&file);
    CHECK(pc_psd_layer_block(rec, "SoCo") != NULL && pc_psd_layer_block(rec, "vmsk") != NULL);
    rec->rect = (pc_psd_rect){8, 8, 24, 24};
    for (size_t i = 0; i < rec->nchannels; i++) {
        if (rec->channels[i].id >= -1) {
            uint8_t v[16 * 16];
            memset(v, rec->channels[i].id == -1 ? 255 : 0, sizeof v);
            reencode(&rec->channels[i], PC_PSD_RAW, v, sizeof v, 16, 16, 8);
        }
    }
    imp = reimport(&file);
    CHECK(imp.document.nlayers >= 2);
    if (imp.document.nlayers >= 2) {
        const pc_layer *il = &imp.document.layers[1];
        pc_compose_buffer f;
        const float *in;
        const float *out;
        if (il->content.kind != PC_LAYER_FILL) {
            PC_T_FAIL("imported as %s", pc_layer_content_kind_name(&il->content));
        }
        pc_t_checks++;
        CHECK(il->has_vector_mask && il->vector_mask.density == 204.0f / 255.0f);
        f = flatten(&imp.document);
        in = pc_compose_buffer_get(&f, 16, 16);
        out = pc_compose_buffer_get(&f, 2, 2);
        CHECK(in != NULL && in[0] < 0.01f); /* inside the path: black */
        /* outside: 20% of the fill shows */
        CHECK(out != NULL);
        if (out != NULL) {
            CHECK_NEAR(out[0], 0.8, 0.01);
        }
        pc_compose_buffer_fini(&f);
    }
    pc_io_import_result_fini(&imp);
    pc_psd_file_fini(&file);
    pc_document_fini(&d);
}

/* psd-tools adjustment-fillers.psd: a solid-colour shape whose vector path is empty with the
 * initial-fill record set (it covers the canvas) and no stored pixels rendered nothing. */
static void test_shape_without_pixels_and_full_initial_fill_covers_the_canvas(void)
{
    pc_document d = reg_doc(PC_SAMPLE_TYPE_U8);
    pc_path path;
    pc_layer l;
    pc_psd_file file;
    pc_psd_layer *rec;
    pc_io_import_result imp;
    pc_compose_buffer f;
    const float *a;
    const float *b;
    memset(&path, 0, sizeof path);
    path.inverted = true;
    l = shape_layer(&d, "Color Fill 1", path);
    t_push(&d, &l);
    file = export_psd(&d);
    rec = last_record(&file);
    rec->rect = (pc_psd_rect){0, 0, 0, 0};
    for (size_t i = 0; i < rec->nchannels; i++) {
        reencode(&rec->channels[i], PC_PSD_RAW, NULL, 0, 0, 0, 8);
    }
    imp = reimport(&file);
    f = flatten(&imp.document);
    a = pc_compose_buffer_get(&f, 0, 0);
    b = pc_compose_buffer_get(&f, 31, 31);
    pc_t_checks++; /* covers everything */
    if (a == NULL || b == NULL || !(a[0] < 0.01f && b[0] < 0.01f)) {
        PC_T_FAIL("(0,0) = %g, (31,31) = %g", a != NULL ? (double)a[0] : -1.0,
                  b != NULL ? (double)b[0] : -1.0);
    }
    pc_compose_buffer_fini(&f);
    pc_io_import_result_fini(&imp);
    pc_psd_file_fini(&file);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_oracle_rgb8);
    RUN(test_oracle_rgb16);
    RUN(test_oracle_rgb32);
    RUN(test_oracle_gray8);
    RUN(test_oracle_gray16);
    RUN(test_oracle_cmyk8);
    RUN(test_oracle_lab8);
    RUN(test_builder_oracle_rgb8);
    RUN(test_builder_oracle_rgb16);
    RUN(test_builder_oracle_gray8);
    RUN(test_builder_oracle_gray16);
    RUN(test_every_blend_mode_single_layer_oracle);
    RUN(test_hidden_and_clipped_affect_composite);
    RUN(test_merged_alpha_written_when_transparent);
    RUN(test_shape_layer_without_stored_pixels_renders_from_its_path);
    RUN(test_vector_mask_density_and_feather_survive_export);
    RUN(test_real_user_mask_with_parameters_is_decoded);
    RUN(test_shape_with_vector_mask_density_imports_as_fill_with_soft_mask);
    RUN(test_shape_without_pixels_and_full_initial_fill_covers_the_canvas);
    return pc_t_finish();
}
