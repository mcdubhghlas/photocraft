/* Multi-part OpenEXR / channel groups as document layers (port of crates/io/src/exr_parts.rs).
 * The codecs do all the EXR decoding; this file lists the chooser entries and builds the
 * document. See exr_parts.h. */
#include "io/exr_parts.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "codecs/codecs.h"
#include "io/flat.h"

#define EXR_UNSUP(err, ...) pc_io_unsupported((err), __VA_ARGS__)

/* The additive AOVs a beauty precomp is built from: the light-path set Arnold writes. */
static const char *const PRECOMP_REQUIRED[5] = {"directdiffuse", "indirectdiffuse", "directspecular",
                                                "indirectspecular", "emission"};
static const char *const PRECOMP_OPTIONAL[3] = {"transmission", "sss", "volume"};

/* One channel group: where it lives and which exact channels make up its RGB(A). */
typedef struct group {
    char *prefix;
    size_t part;
    char *channels[3]; /* the exact R/G/B channel names, in encounter order */
    size_t nchannels;
    char *alpha; /* the A channel name, or NULL */
} group;

static void group_fini(group *g)
{
    size_t i;
    if (g == NULL) {
        return;
    }
    free(g->prefix);
    for (i = 0; i < g->nchannels; i++) {
        free(g->channels[i]);
    }
    free(g->alpha);
}

static void groups_free(group *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        group_fini(&v[i]);
    }
    free(v);
}

/* The name an AOV normalizes to (ASCII alphanumerics, lowercased); caller frees. */
static char *normalize(const char *name, pc_error *err)
{
    size_t len = strlen(name);
    char *out = pc_malloc(len + 1);
    size_t k = 0;
    size_t i;
    if (out == NULL) {
        (void)pc_io_nomem(err, "normalizing an AOV name");
        return NULL;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c)) {
            out[k++] = (char)tolower(c);
        }
    }
    out[k] = '\0';
    return out;
}

/* The base channel name (after the last '.'), borrowed. */
static const char *channel_base(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL ? dot + 1 : name;
}

/* (prefix, base) when the channel name carries a non-empty prefix before its last dot. Returns
 * the prefix length (0 = no group); *base points past the dot. */
static size_t channel_base_split(const char *name, const char **base)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name) {
        return 0;
    }
    *base = dot + 1;
    return (size_t)(dot - name);
}

static bool base_is(const char *base, const char *want)
{
    return strcmp(base, want) == 0;
}

/* dup of n bytes of s as a C string. */
static char *dupn(const char *s, size_t n, pc_error *err)
{
    char *out = pc_strndup(s, n);
    if (out == NULL) {
        (void)pc_io_nomem(err, "copying a channel name");
    }
    return out;
}

/* Collect the prefixed RGB(A) channel sets of every flat part. *out is a new array. */
static pc_status collect_groups(const uint8_t *bytes, size_t len, group **out, size_t *nout, pc_error *err)
{
    pc_climits limits = pc_climits_default();
    pc_exr_part_info *parts = NULL;
    size_t nparts = 0;
    group *v = NULL;
    size_t n = 0;
    size_t cap = 0;
    size_t pi;
    pc_status st = pc_cexr_info(bytes, len, &limits, &parts, &nparts, err);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "codec: ");
    }
    for (pi = 0; pi < nparts; pi++) {
        const pc_exr_part_info *part = &parts[pi];
        size_t found_start = n; /* groups discovered in this part begin here */
        size_t ci;
        if (part->deep) {
            continue;
        }
        for (ci = 0; ci < part->nchannels; ci++) {
            const char *cname = part->channels[ci].name;
            const char *base = NULL;
            size_t plen = channel_base_split(cname, &base);
            size_t g;
            group *gp;
            if (plen == 0) {
                continue;
            }
            /* A prefix ending in a digit is a Cryptomatte stream, not a group. */
            if (isdigit((unsigned char)cname[plen - 1])) {
                continue;
            }
            gp = NULL;
            for (g = found_start; g < n; g++) {
                if (strlen(v[g].prefix) == plen && memcmp(v[g].prefix, cname, plen) == 0) {
                    gp = &v[g];
                    break;
                }
            }
            if (gp == NULL) {
                group ng;
                memset(&ng, 0, sizeof ng);
                ng.part = part->index;
                ng.prefix = dupn(cname, plen, err);
                if (ng.prefix == NULL) {
                    st = PC_ERR_NOMEM;
                    goto done;
                }
                if (n == cap) {
                    group *nv = pc_grow_array(v, &cap, n + 1, sizeof *v);
                    if (nv == NULL) {
                        free(ng.prefix);
                        st = pc_io_nomem(err, "listing EXR channel groups");
                        goto done;
                    }
                    v = nv;
                }
                v[n] = ng;
                gp = &v[n];
                n++;
            }
            if ((base_is(base, "R") || base_is(base, "G") || base_is(base, "B")) && gp->nchannels < 3) {
                gp->channels[gp->nchannels] = dupn(cname, strlen(cname), err);
                if (gp->channels[gp->nchannels] == NULL) {
                    st = PC_ERR_NOMEM;
                    goto done;
                }
                gp->nchannels++;
            } else if (base_is(base, "A") && gp->alpha == NULL) {
                gp->alpha = dupn(cname, strlen(cname), err);
                if (gp->alpha == NULL) {
                    st = PC_ERR_NOMEM;
                    goto done;
                }
            }
        }
        /* Keep only groups with exactly 3 colour channels (retain()); drop the rest. */
        {
            size_t w = found_start;
            size_t r;
            for (r = found_start; r < n; r++) {
                if (v[r].nchannels == 3) {
                    if (w != r) {
                        v[w] = v[r];
                    }
                    w++;
                } else {
                    group_fini(&v[r]);
                }
            }
            n = w;
        }
    }
    *out = v;
    *nout = n;
    v = NULL;
    st = PC_OK;
done:
    groups_free(v, n);
    pc_exr_parts_free(parts, nparts);
    return st;
}

/* ---- entries ---- */

void pc_io_exr_entries_free(pc_io_exr_entry *v, size_t n)
{
    size_t i;
    if (v == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        free(v[i].name);
        free(v[i].view);
    }
    free(v);
}

static pc_status entries_push(pc_io_exr_entry **v, size_t *n, size_t *cap, pc_io_exr_entry e, pc_error *err)
{
    if (*n == *cap) {
        pc_io_exr_entry *nv = pc_grow_array(*v, cap, *n + 1, sizeof **v);
        if (nv == NULL) {
            free(e.name);
            free(e.view);
            return pc_io_nomem(err, "listing EXR entries");
        }
        *v = nv;
    }
    (*v)[(*n)++] = e;
    return PC_OK;
}

static char *part_name(const pc_exr_part_info *p, pc_error *err)
{
    if (p->name != NULL) {
        return dupn(p->name, strlen(p->name), err);
    }
    {
        char buf[32];
        (void)snprintf(buf, sizeof buf, "part %zu", p->index);
        return dupn(buf, strlen(buf), err);
    }
}

static char *opt_dup(const char *s, pc_error *err)
{
    return s != NULL ? dupn(s, strlen(s), err) : NULL;
}

pc_status pc_io_exr_entries(const uint8_t *bytes, size_t len, pc_io_exr_entry **out, size_t *nout,
                            pc_error *err)
{
    pc_climits limits = pc_climits_default();
    pc_exr_part_info *parts = NULL;
    size_t nparts = 0;
    group *grps = NULL;
    size_t ngrps = 0;
    pc_crypto_layer *cl = NULL;
    size_t ncl = 0;
    pc_io_exr_entry *v = NULL;
    size_t n = 0;
    size_t cap = 0;
    bool all_deep = true;
    size_t i;
    pc_status st = pc_cexr_info(bytes, len, &limits, &parts, &nparts, err);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "codec: ");
    }
    for (i = 0; i < nparts; i++) {
        if (!parts[i].deep) {
            all_deep = false;
        }
    }
    if (nparts == 0 || all_deep) {
        pc_exr_parts_free(parts, nparts);
        return EXR_UNSUP(err, "this EXR holds only deep parts; it opens through the deep decoder");
    }
    for (i = 0; i < nparts; i++) {
        const pc_exr_part_info *p = &parts[i];
        pc_io_exr_entry e;
        size_t ci;
        if (p->deep) {
            continue;
        }
        memset(&e, 0, sizeof e);
        e.kind = PC_IO_EXR_PART;
        e.name = part_name(p, err);
        e.view = opt_dup(p->view, err);
        e.width = p->width;
        e.height = p->height;
        if (e.name == NULL) {
            free(e.view);
            st = PC_ERR_NOMEM;
            goto done;
        }
        for (ci = 0; ci < p->nchannels; ci++) {
            const char *b = channel_base(p->channels[ci].name);
            if (base_is(b, "R") || base_is(b, "G") || base_is(b, "B") || base_is(b, "A") ||
                base_is(b, "Y")) {
                e.channels++;
            }
        }
        st = entries_push(&v, &n, &cap, e, err);
        if (st != PC_OK) {
            goto done;
        }
    }
    st = collect_groups(bytes, len, &grps, &ngrps, err);
    if (st != PC_OK) {
        goto done;
    }
    for (i = 0; i < ngrps; i++) {
        pc_io_exr_entry e;
        memset(&e, 0, sizeof e);
        e.kind = PC_IO_EXR_GROUP;
        e.name = dupn(grps[i].prefix, strlen(grps[i].prefix), err);
        if (e.name == NULL) {
            st = PC_ERR_NOMEM;
            goto done;
        }
        e.channels = 3 + (grps[i].alpha != NULL ? 1u : 0u);
        st = entries_push(&v, &n, &cap, e, err);
        if (st != PC_OK) {
            goto done;
        }
    }
    st = pc_ccryptomatte_layers(bytes, len, &limits, &cl, &ncl, err);
    if (st != PC_OK) {
        st = pc_io_err_prefix(st, err, "codec: ");
        goto done;
    }
    for (i = 0; i < ncl; i++) {
        pc_io_exr_entry e;
        memset(&e, 0, sizeof e);
        e.kind = PC_IO_EXR_CRYPTOMATTE;
        e.name = dupn(cl[i].name, strlen(cl[i].name), err);
        if (e.name == NULL) {
            st = PC_ERR_NOMEM;
            goto done;
        }
        e.channels = cl[i].nchannels;
        e.has_objects = true;
        e.objects = cl[i].nmanifest;
        st = entries_push(&v, &n, &cap, e, err);
        if (st != PC_OK) {
            goto done;
        }
    }
    *out = v;
    *nout = n;
    v = NULL;
    n = 0;
    st = PC_OK;
done:
    pc_io_exr_entries_free(v, n);
    groups_free(grps, ngrps);
    pc_crypto_layers_free(cl, ncl);
    pc_exr_parts_free(parts, nparts);
    return st;
}

/* ---- import ---- */

/* An image as an importable layer: F16 becomes F32, the layout matches the document's. *gray says
 * whether this layer is grayscale; *max is grown to the image size. */
static pc_status layer_surface(const pc_cimage *img, pc_surface *out, bool *gray, uint32_t *maxw,
                               uint32_t *maxh, bool *f16_seen, pc_error *err)
{
    pc_clayout target;
    if (img->layout == PC_CL_GRAY || img->layout == PC_CL_GRAYA) {
        target = PC_CL_GRAYA;
        *gray = true;
    } else {
        target = PC_CL_RGBA;
        *gray = false;
    }
    if (img->width > *maxw) {
        *maxw = img->width;
    }
    if (img->height > *maxh) {
        *maxh = img->height;
    }
    if (img->sample == PC_CS_F16) {
        *f16_seen = true;
    }
    return pc_io_image_surface(img, target, PC_CS_F32, out, err);
}

/* ASCII case-insensitive string equality. */
static bool ascii_ieq(const char *a, const char *b)
{
    size_t i;
    if (a == NULL || b == NULL) {
        return a == b;
    }
    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return a[i] == b[i];
}

/* view.as_ref().is_none_or(|v| sel.view == "both" || v == sel.view) (ASCII case-insensitive). */
static bool view_passes(const char *view, const char *sel_view)
{
    if (view == NULL) {
        return true;
    }
    return ascii_ieq(sel_view, "both") || ascii_ieq(view, sel_view);
}

/* Rust Option<String> debug: `None` or `Some("value")`, into a fixed buffer. */
static void view_debug(const char *view, char *buf, size_t cap)
{
    if (view == NULL) {
        (void)snprintf(buf, cap, "None");
    } else {
        (void)snprintf(buf, cap, "Some(\"%s\")", view);
    }
}

static pc_status add_layer(pc_document *doc, const char *name, pc_surface *s, pc_error *err)
{
    pc_layer l;
    pc_status st = pc_io_raster_layer(name, s, &l, err);
    pc_surface_fini(s);
    if (st != PC_OK) {
        return st;
    }
    st = pc_io_push_layer(doc, &l, err);
    if (st != PC_OK) {
        pc_layer_fini(&l);
    }
    return st;
}

/* The additive beauty from the file's AOV groups. */
static pc_status precomp_layer(const uint8_t *bytes, size_t len, const group *grps, size_t ngrps,
                               const pc_cdecode_opts *opts, pc_document *doc, uint32_t *maxw,
                               uint32_t *maxh, pc_io_warnings *warnings, bool *f16_seen, pc_error *err)
{
    bool have_required[5] = {false, false, false, false, false};
    float *sum_rgb = NULL;
    float *alpha = NULL;
    uint32_t sw = 0;
    uint32_t sh = 0;
    size_t gi;
    pc_surface surf;
    bool gray;
    pc_cimage out_img;
    size_t px;
    size_t i;
    bool have_img = false;
    pc_status st = PC_OK;
    /* Which required AOVs are present? */
    for (gi = 0; gi < ngrps; gi++) {
        char *nrm = normalize(grps[gi].prefix, err);
        size_t r;
        if (nrm == NULL) {
            return PC_ERR_NOMEM;
        }
        for (r = 0; r < 5; r++) {
            if (strcmp(nrm, PRECOMP_REQUIRED[r]) == 0) {
                have_required[r] = true;
            }
        }
        free(nrm);
    }
    {
        char missing[256];
        size_t off = 0;
        size_t r;
        missing[0] = '\0';
        for (r = 0; r < 5; r++) {
            if (!have_required[r]) {
                off += (size_t)snprintf(missing + off, off < sizeof missing ? sizeof missing - off : 0,
                                        "%s%s", off > 0 ? ", " : "", PRECOMP_REQUIRED[r]);
            }
        }
        if (missing[0] != '\0') {
            return EXR_UNSUP(err, "the AOV set is incomplete for a beauty precomp; missing: %s", missing);
        }
    }
    for (gi = 0; gi < ngrps; gi++) {
        char *nrm = normalize(grps[gi].prefix, err);
        bool wanted = false;
        const char *names[4];
        size_t nn = 0;
        pc_cimage img;
        const float *fx;
        uint32_t w;
        uint32_t h;
        size_t r;
        if (nrm == NULL) {
            st = PC_ERR_NOMEM;
            goto cleanup;
        }
        for (r = 0; r < 5; r++) {
            if (strcmp(nrm, PRECOMP_REQUIRED[r]) == 0) {
                wanted = true;
            }
        }
        for (r = 0; r < 3; r++) {
            if (strcmp(nrm, PRECOMP_OPTIONAL[r]) == 0) {
                wanted = true;
            }
        }
        free(nrm);
        if (!wanted) {
            continue;
        }
        for (r = 0; r < grps[gi].nchannels; r++) {
            names[nn++] = grps[gi].channels[r];
        }
        if (grps[gi].alpha != NULL) {
            names[nn++] = grps[gi].alpha;
        }
        st = pc_cdecode_exr_channels(bytes, len, grps[gi].part, names, nn, opts, &img, err);
        if (st != PC_OK) {
            st = pc_io_err_prefix(st, err, "codec: ");
            goto cleanup;
        }
        if (img.sample == PC_CS_F16) {
            *f16_seen = true;
        }
        /* Convert to RGBA F32 for the additive sum. */
        {
            pc_cimage rgba;
            st = pc_cimage_convert(&img, PC_CL_RGBA, PC_CS_F32, &rgba, err);
            pc_cimage_fini(&img);
            if (st != PC_OK) {
                goto cleanup;
            }
            img = rgba;
        }
        w = img.width;
        h = img.height;
        fx = (const float *)img.data;
        if (sum_rgb == NULL) {
            size_t n3;
            size_t n1;
            sw = w;
            sh = h;
            if (!pc_size_mul((size_t)w * h, 3, &n3) || !pc_size_mul((size_t)w, h, &n1)) {
                pc_cimage_fini(&img);
                st = EXR_UNSUP(err, "the AOV groups are too large");
                goto cleanup;
            }
            sum_rgb = pc_calloc_array(n3 > 0 ? n3 : 1, sizeof *sum_rgb);
            alpha = pc_calloc_array(n1 > 0 ? n1 : 1, sizeof *alpha);
            if (sum_rgb == NULL || alpha == NULL) {
                pc_cimage_fini(&img);
                st = pc_io_nomem(err, "building a beauty precomp");
                goto cleanup;
            }
        } else if (w != sw || h != sh) {
            pc_cimage_fini(&img);
            st = EXR_UNSUP(err, "the AOV groups differ in size; a beauty precomp needs one common extent");
            goto cleanup;
        }
        for (i = 0; i < (size_t)w * h; i++) {
            float a = fx[i * 4 + 3];
            sum_rgb[i * 3] += fx[i * 4];
            sum_rgb[i * 3 + 1] += fx[i * 4 + 1];
            sum_rgb[i * 3 + 2] += fx[i * 4 + 2];
            if (a > alpha[i]) {
                alpha[i] = a;
            }
        }
        pc_cimage_fini(&img);
    }
    if (sw == 0 || sh == 0) {
        st = EXR_UNSUP(err, "the AOV groups are empty");
        goto cleanup;
    }
    px = (size_t)sw * sh;
    {
        float *straight = pc_calloc_array(px * 4 > 0 ? px * 4 : 1, sizeof *straight);
        if (straight == NULL) {
            st = pc_io_nomem(err, "building a beauty precomp");
            goto cleanup;
        }
        for (i = 0; i < px; i++) {
            float a = alpha[i];
            straight[i * 4] = a > 0.0f ? sum_rgb[i * 3] / a : 0.0f;
            straight[i * 4 + 1] = a > 0.0f ? sum_rgb[i * 3 + 1] / a : 0.0f;
            straight[i * 4 + 2] = a > 0.0f ? sum_rgb[i * 3 + 2] / a : 0.0f;
            straight[i * 4 + 3] = a;
        }
        st = pc_cimage_from_normalized(sw, sh, PC_CL_RGBA, PC_CS_F32, straight, px * 4, &out_img, err);
        free(straight);
        if (st != PC_OK) {
            goto cleanup;
        }
        have_img = true;
    }
    st = layer_surface(&out_img, &surf, &gray, maxw, maxh, f16_seen, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    st = add_layer(doc, "beauty (precomp)", &surf, err);
    if (st == PC_OK) {
        st = pc_io_warn(warnings, err,
                        "beauty (precomp) is the additive sum of the file's AOVs; alpha is the "
                        "maximum coverage");
    }
cleanup:
    if (have_img) {
        pc_cimage_fini(&out_img);
    }
    free(sum_rgb);
    free(alpha);
    return st;
}

pc_status pc_io_exr_import(const char *name, const uint8_t *bytes, size_t len,
                           const pc_io_exr_selection *sel, pc_io_import_result *out, pc_error *err)
{
    pc_cdecode_opts opts = pc_cdecode_opts_default();
    pc_exr_part_info *info = NULL;
    size_t ninfo = 0;
    group *grps = NULL;
    size_t ngrps = 0;
    pc_document doc;
    pc_io_warnings warnings = PC_IO_WARNINGS_INIT;
    bool gray_only = true;
    bool f16_seen = false;
    uint32_t maxw = 1;
    uint32_t maxh = 1;
    bool doc_ready = false;
    size_t i;
    pc_status st = pc_cexr_info(bytes, len, &opts.limits, &info, &ninfo, err);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "codec: ");
    }
    if (ninfo == 0) {
        pc_exr_parts_free(info, ninfo);
        return EXR_UNSUP(err, "the EXR has no parts");
    }
    st = collect_groups(bytes, len, &grps, &ngrps, err);
    if (st != PC_OK) {
        pc_exr_parts_free(info, ninfo);
        return st;
    }
    st = pc_document_init(&doc, name, (pc_size){1, 1}, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, err);
    if (st != PC_OK) {
        goto done;
    }
    doc_ready = true;

    /* Chosen parts (deduped, view-filtered). */
    for (i = 0; i < sel->nparts; i++) {
        const char *want = sel->parts[i];
        const pc_exr_part_info *p = NULL;
        pc_cimage img;
        pc_surface surf;
        bool gray;
        char vbuf[64];
        char *lname;
        size_t j;
        bool dup = false;
        for (j = 0; j < ninfo; j++) {
            if (!info[j].deep && info[j].name != NULL && strcmp(info[j].name, want) == 0) {
                p = &info[j];
                break;
            }
        }
        if (p == NULL) {
            st = EXR_UNSUP(err, "no flat part named %s", want);
            goto done;
        }
        if (!view_passes(p->view, sel->view)) {
            view_debug(p->view, vbuf, sizeof vbuf);
            st = EXR_UNSUP(err, "the part %s is of the %s view, the filter is %s", want, vbuf, sel->view);
            goto done;
        }
        /* Dedup: skip if an earlier chosen part had the same name. */
        {
            size_t k;
            for (k = 0; k < i; k++) {
                if (strcmp(sel->parts[k], want) == 0) {
                    dup = true;
                    break;
                }
            }
        }
        if (dup) {
            continue;
        }
        st = pc_cdecode_exr_part(bytes, len, p->index, &opts, &img, err);
        if (st != PC_OK) {
            st = pc_io_err_prefix(st, err, "codec: ");
            goto done;
        }
        st = layer_surface(&img, &surf, &gray, &maxw, &maxh, &f16_seen, err);
        if (st != PC_OK) {
            pc_cimage_fini(&img);
            goto done;
        }
        gray_only = gray_only && gray;
        lname = part_name(p, err);
        if (lname == NULL) {
            pc_surface_fini(&surf);
            pc_cimage_fini(&img);
            st = PC_ERR_NOMEM;
            goto done;
        }
        st = add_layer(&doc, lname, &surf, err);
        free(lname);
        pc_cimage_fini(&img);
        if (st != PC_OK) {
            goto done;
        }
    }

    /* Chosen groups. */
    for (i = 0; i < sel->ngroups; i++) {
        const char *want = sel->groups[i];
        const group *g = NULL;
        const char *vw;
        pc_cimage img;
        pc_surface surf;
        bool gray;
        const char *names[4];
        size_t nn = 0;
        size_t j;
        bool dup = false;
        char vbuf[64];
        for (j = 0; j < ngrps; j++) {
            if (strcmp(grps[j].prefix, want) == 0) {
                g = &grps[j];
                break;
            }
        }
        if (g == NULL) {
            st = EXR_UNSUP(err, "no channel group named %s", want);
            goto done;
        }
        vw = g->part < ninfo ? info[g->part].view : NULL;
        if (!view_passes(vw, sel->view)) {
            view_debug(vw, vbuf, sizeof vbuf);
            st = EXR_UNSUP(err, "the group %s is of the %s view, the filter is %s", want, vbuf, sel->view);
            goto done;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(sel->groups[j], want) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        for (j = 0; j < g->nchannels; j++) {
            names[nn++] = g->channels[j];
        }
        if (g->alpha != NULL) {
            names[nn++] = g->alpha;
        }
        st = pc_cdecode_exr_channels(bytes, len, g->part, names, nn, &opts, &img, err);
        if (st != PC_OK) {
            st = pc_io_err_prefix(st, err, "codec: ");
            goto done;
        }
        st = layer_surface(&img, &surf, &gray, &maxw, &maxh, &f16_seen, err);
        if (st != PC_OK) {
            pc_cimage_fini(&img);
            goto done;
        }
        gray_only = gray_only && gray;
        st = add_layer(&doc, g->prefix, &surf, err);
        pc_cimage_fini(&img);
        if (st != PC_OK) {
            goto done;
        }
    }

    if (doc.nlayers == 0 && !sel->precomp) {
        st = EXR_UNSUP(err, "no parts or groups selected");
        goto done;
    }
    if (sel->precomp) {
        st = precomp_layer(bytes, len, grps, ngrps, &opts, &doc, &maxw, &maxh, &warnings, &f16_seen, err);
        if (st != PC_OK) {
            goto done;
        }
        gray_only = false; /* the precomp is an RGB layer by construction */
    }
    if (doc.nlayers == 0) {
        st = EXR_UNSUP(err, "nothing imported");
        goto done;
    }
    if (f16_seen) {
        st = pc_io_warn(&warnings, err, "16-bit float samples are stored as 32-bit float");
        if (st != PC_OK) {
            goto done;
        }
    }
    doc.size = (pc_size){maxw, maxh};
    doc.mode = gray_only ? PC_COLOR_MODE_GRAYSCALE : PC_COLOR_MODE_RGB;
    if (doc.mode == PC_COLOR_MODE_RGB) {
        const pc_profile *p = pc_builtin_profile(PC_BUILTIN_LINEAR_SRGB);
        pc_buf icc = PC_BUF_INIT;
        st = p != NULL ? pc_profile_to_bytes(p, &icc, err) : pc_io_nomem(err, "building a linear profile");
        if (st == PC_OK) {
            st = pc_io_set_blob(&doc.icc_profile, icc.data, icc.len, err);
        }
        pc_buf_fini(&icc);
        if (st != PC_OK) {
            goto done;
        }
    }
    out->document = doc;
    out->warnings = warnings;
    out->source_read_only = false;
    out->preview_only = false;
    doc_ready = false;
    warnings = (pc_io_warnings)PC_IO_WARNINGS_INIT;
    st = PC_OK;
done:
    if (doc_ready) {
        pc_document_fini(&doc);
    }
    pc_io_warnings_fini(&warnings);
    groups_free(grps, ngrps);
    pc_exr_parts_free(info, ninfo);
    return st;
}
