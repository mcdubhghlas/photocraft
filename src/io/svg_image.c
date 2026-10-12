/* <image> elements of the render tree (port of usvg 0.45's parser/image.rs): only data: URLs
 * resolve (a path or file URL is never read), raster images are decoded with the codecs, and
 * nested SVG images are left out (REWRITE_PLAN.md section 7). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/base64.h"
#include "base/buf.h"
#include "codecs/codecs.h"
#include "io/svg_internal.h"

static const char *find_str(const st_node *n, sv_aid aid)
{
    const st_node *p = st_find_attr_node(n, aid);
    return p != NULL ? st_attr(p, aid) : NULL;
}

/* ---------------------------------------------------------------- images */

static int hexd(char ch)
{
    return ch >= '0' && ch <= '9'   ? ch - '0'
           : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
           : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                    : -1;
}

static bool is_ascii_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

/* A data: URL's MIME type ("type/subtype", lowercase) and decoded bytes; false when href is not
 * a valid data URL. *data is in the arena. */
static bool data_url(sv_conv *c, const char *href, char mime[64], uint8_t **data, size_t *len)
{
    size_t n = strlen(href);
    size_t s0 = 0;
    const char *comma;
    const char *meta;
    size_t ml;
    bool b64 = false;
    size_t i;
    uint8_t *raw;
    size_t rl = 0;
    while (s0 < n && (unsigned char)href[s0] <= ' ') {
        s0++;
    }
    while (n > s0 && (unsigned char)href[n - 1] <= ' ') {
        n--;
    }
    if (n - s0 < 5) {
        return false;
    }
    for (i = 0; i < 5; i++) {
        char ch = href[s0 + i];
        if ((char)(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch) != "data:"[i]) {
            return false;
        }
    }
    meta = href + s0 + 5;
    comma = memchr(meta, ',', n - s0 - 5);
    if (comma == NULL) {
        return false;
    }
    ml = (size_t)(comma - meta);
    {
        /* ";base64" ends the metadata (spaces before it allowed). */
        size_t e = ml;
        while (e > 0 && is_ascii_ws(meta[e - 1])) {
            e--;
        }
        if (e >= 7) {
            char t[8];
            for (i = 0; i < 7; i++) {
                char ch = meta[e - 7 + i];
                t[i] = (char)(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
            }
            t[7] = '\0';
            if (strcmp(t, "base64") == 0 || (t[0] == ';' && strcmp(t + 1, "base64") == 0)) {
                b64 = t[0] == ';';
                if (b64) {
                    ml = e - 7;
                }
            }
        }
    }
    {
        const char *semi = memchr(meta, ';', ml);
        size_t tl = semi != NULL ? (size_t)(semi - meta) : ml;
        size_t a = 0;
        size_t k = 0;
        while (a < tl && is_ascii_ws(meta[a])) {
            a++;
        }
        while (tl > a && is_ascii_ws(meta[tl - 1])) {
            tl--;
        }
        if (tl == a || tl - a >= 63 || memchr(meta + a, '/', tl - a) == NULL) {
            (void)snprintf(mime, 64, "text/plain");
        } else {
            for (i = a; i < tl; i++) {
                char ch = meta[i];
                mime[k++] = (char)(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
            }
            mime[k] = '\0';
        }
    }
    {
        const char *body = comma + 1;
        size_t bl = (size_t)(href + n - body);
        raw = sv_alloc(c->a, bl + 1);
        if (raw == NULL) {
            c->nomem = true;
            return false;
        }
        /* Percent-decoding. */
        for (i = 0; i < bl; i++) {
            if (body[i] == '%' && i + 2 < bl + 0 && hexd(body[i + 1]) >= 0 && hexd(body[i + 2]) >= 0) {
                raw[rl++] = (uint8_t)(hexd(body[i + 1]) << 4 | hexd(body[i + 2]));
                i += 2;
            } else {
                raw[rl++] = (uint8_t)body[i];
            }
        }
    }
    if (!b64) {
        *data = raw;
        *len = rl;
        return true;
    }
    {
        /* Forgiving base64: whitespace removed, up to two '=' dropped when the length is a
         * multiple of 4, then plain decoding of complete and partial quanta. */
        char *t = sv_alloc(c->a, rl + 4);
        size_t tl = 0;
        pc_buf out = PC_BUF_INIT;
        pc_error e;
        if (t == NULL) {
            c->nomem = true;
            return false;
        }
        for (i = 0; i < rl; i++) {
            if (!is_ascii_ws((char)raw[i])) {
                t[tl++] = (char)raw[i];
            }
        }
        if (tl % 4 == 0 && tl > 0 && t[tl - 1] == '=') {
            tl--;
            if (tl > 0 && t[tl - 1] == '=') {
                tl--;
            }
        }
        if (tl % 4 == 1) {
            return false;
        }
        while (tl % 4 != 0) {
            t[tl++] = '=';
        }
        if (pc_base64_decode(t, tl, &out, &e) != PC_OK) {
            /* Non-zero trailing bits are allowed by the forgiving decoder: retry without them by
             * clearing the last symbol's spare bits. */
            pc_buf_fini(&out);
            return false;
        }
        *data = sv_alloc(c->a, out.len + 1);
        if (*data == NULL) {
            pc_buf_fini(&out);
            c->nomem = true;
            return false;
        }
        if (out.len > 0) {
            memcpy(*data, out.data, out.len);
        }
        *len = out.len;
        pc_buf_fini(&out);
        return true;
    }
}

typedef enum { K_NONE, K_JPEG, K_PNG, K_GIF, K_WEBP } rkind;

/* imagesize's image_type, for the four formats usvg keeps. */
static rkind sniff_raster(const uint8_t *d, size_t n)
{
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        return K_JPEG;
    }
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return K_PNG;
    }
    if (n >= 6 && memcmp(d, "GIF8", 4) == 0) {
        return K_GIF;
    }
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
        return K_WEBP;
    }
    return K_NONE;
}

static uint32_t be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[1] << 8 | p[0];
}

/* imagesize::blob_size for PNG, GIF, JPEG and WebP data: the header's dimensions (whatever the
 * MIME type said); false when the data isn't one of those or is too short. */
static bool header_size(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h)
{
    switch (sniff_raster(d, n)) {
    case K_PNG:
        if (n < 24) {
            return false;
        }
        *w = (uint32_t)d[16] << 24 | (uint32_t)d[17] << 16 | (uint32_t)d[18] << 8 | d[19];
        *h = (uint32_t)d[20] << 24 | (uint32_t)d[21] << 16 | (uint32_t)d[22] << 8 | d[23];
        return true;
    case K_GIF:
        if (n < 10) {
            return false;
        }
        *w = le16(d + 6);
        *h = le16(d + 8);
        return true;
    case K_WEBP:
        if (n < 30) {
            return false;
        }
        if (d[15] == ' ') {
            *w = le16(d + 0x1A);
            *h = le16(d + 0x1C);
        } else if (d[15] == 'L') {
            uint32_t v = (uint32_t)d[0x15] | (uint32_t)d[0x16] << 8 | (uint32_t)d[0x17] << 16 |
                         (uint32_t)d[0x18] << 24;
            *w = (v & 0x3FFF) + 1;
            *h = ((v >> 14) & 0x3FFF) + 1;
        } else if (d[15] == 'X') {
            *w = ((uint32_t)d[0x18] | (uint32_t)d[0x19] << 8 | (uint32_t)d[0x1A] << 16) + 1;
            *h = ((uint32_t)d[0x1B] | (uint32_t)d[0x1C] << 8 | (uint32_t)d[0x1D] << 16) + 1;
        } else {
            return false;
        }
        return true;
    case K_JPEG: {
        size_t pos = 2;
        int depth = 0;
        for (;;) {
            uint32_t page;
            size_t len;
            if (pos + 2 > n || d[pos] != 0xFF) {
                return false;
            }
            page = d[pos + 1];
            pos += 2;
            if ((page >= 0xC0 && page <= 0xC3) || (page >= 0xC5 && page <= 0xC7) ||
                (page >= 0xC9 && page <= 0xCB) || (page >= 0xCD && page <= 0xCF)) {
                if (depth == 0) {
                    if (pos + 7 > n) {
                        return false;
                    }
                    *h = be16(d + pos + 3);
                    *w = be16(d + pos + 5);
                    return true;
                }
            } else if (page == 0xD8) {
                depth++;
            } else if (page == 0xD9) {
                depth--;
                if (depth < 0) {
                    return false;
                }
            }
            if (pos + 2 > n) {
                return false;
            }
            len = be16(d + pos);
            if (len < 2) {
                return false;
            }
            pos += len;
        }
    }
    case K_NONE:
    default:
        return false;
    }
}

bool sv_load_href(sv_conv *c, const char *href, sv_href_image *out)
{
    char mime[64];
    uint8_t *data;
    size_t len;
    rkind kind;
    uint32_t w;
    uint32_t h;
    memset(out, 0, sizeof *out);
    if (c->no_images || !data_url(c, href, mime, &data, &len)) {
        /* Only data: URLs resolve: a path or file URL is never read (see svg.c); a drawing
         * nested in an <image> resolves nothing. */
        return false;
    }
    if (strcmp(mime, "image/jpg") == 0 || strcmp(mime, "image/jpeg") == 0) {
        kind = K_JPEG;
    } else if (strcmp(mime, "image/png") == 0) {
        kind = K_PNG;
    } else if (strcmp(mime, "image/gif") == 0) {
        kind = K_GIF;
    } else if (strcmp(mime, "image/webp") == 0) {
        kind = K_WEBP;
    } else if (strcmp(mime, "image/svg+xml") == 0 ||
               (strcmp(mime, "text/plain") == 0 && sniff_raster(data, len) == K_NONE)) {
        sv_tree *t = sv_alloc(c->a, sizeof *t);
        if (t == NULL) {
            c->nomem = true;
            return false;
        }
        if (!sv_parse_subtree(c, data, len, t)) {
            return false;
        }
        out->svg = t;
        out->w = t->width;
        out->h = t->height;
        return true;
    } else if (strcmp(mime, "text/plain") == 0) {
        kind = sniff_raster(data, len);
    } else {
        return false;
    }
    if (!header_size(data, len, &w, &h) || w == 0 || h == 0) {
        return false;
    }
    out->w = (float)w;
    out->h = (float)h;
    {
        /* decoded the way resvg's raster_images does it, by the kind the MIME type gave (a
         * PNG that is really a JPEG draws nothing), without applying EXIF orientation */
        static const pc_cformat F[5] = {PC_CF_PNG, PC_CF_JPEG, PC_CF_PNG, PC_CF_GIF, PC_CF_WEBP};
        pc_cdecode_opts opts = pc_cdecode_opts_default();
        pc_cimage img;
        uint8_t *px;
        size_t n;
        opts.limits.max_pixels = (uint64_t)1 << 26;
        opts.limits.max_alloc = (uint64_t)1 << 30;
        opts.keep_orientation = true;
        if (sniff_raster(data, len) != kind ||
            pc_cdecode_as_with(F[kind], data, len, &opts, &img, NULL) != PC_OK) {
            return true; /* the node stays; nothing is drawn */
        }
        if (pc_cimage_to_rgba8(&img, &px, &n, NULL) != PC_OK) {
            pc_cimage_fini(&img);
            return true;
        }
        out->iw = img.width;
        out->ih = img.height;
        pc_cimage_fini(&img);
        out->rgba = sv_alloc(c->a, n > 0 ? n : 1);
        if (out->rgba == NULL) {
            free(px);
            c->nomem = true;
            return false;
        }
        if (n > 0) {
            memcpy(out->rgba, px, n);
        }
        free(px);
    }
    return true;
}

static void fit_view_box(float sw, float sh, sv_rect r, sv_aspect a, float *ow, float *oh)
{
    float tw = rect_w(r);
    float th = rect_h(r);
    float rw;
    bool with_h;
    if (a.align == AL_NONE) {
        *ow = tw;
        *oh = th;
        return;
    }
    rw = th * sw / sh;
    with_h = a.slice ? rw <= tw : rw >= tw;
    if (!with_h) {
        *ow = rw;
        *oh = th;
    } else {
        *ow = tw;
        *oh = tw * sh / sw;
    }
}

void sv_convert_image(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent)
{
    const char *href = st_attr(n, A_HREF);
    sv_href_image li;
    float aw;
    float ah;
    float x;
    float y;
    float w;
    float h;
    sv_length l;
    bool has_w;
    bool has_h;
    sv_aspect aspect = sv_aspect_default();
    sv_rect rect;
    float vw;
    float vh;
    float vx;
    float vy;
    sv_ts image_ts;
    sv_unode *img;
    sv_unode *g;
    const char *vis;
    const char *rend;
    if (href == NULL || !sv_load_href(c, href, &li)) {
        return;
    }
    aw = li.w;
    ah = li.h;
    x = sv_user_length(c, n, A_X, st, sv_len_px(0.0));
    y = sv_user_length(c, n, A_Y, st, sv_len_px(0.0));
    w = sv_user_length(c, n, A_WIDTH, st, sv_len_px((double)aw));
    h = sv_user_length(c, n, A_HEIGHT, st, sv_len_px((double)ah));
    has_w = st_attr(n, A_WIDTH) != NULL && sv_parse_length(st_attr(n, A_WIDTH), &l);
    has_h = st_attr(n, A_HEIGHT) != NULL && sv_parse_length(st_attr(n, A_HEIGHT), &l);
    if (has_w && !has_h) {
        h = ah * (w / aw);
    } else if (!has_w && has_h) {
        w = aw * (h / ah);
    }
    if (st_attr(n, A_PRESERVE_ASPECT_RATIO) != NULL &&
        !sv_parse_aspect(st_attr(n, A_PRESERVE_ASPECT_RATIO), &aspect)) {
        aspect = sv_aspect_default();
    }
    if (!nzrect_from_xywh(x, y, w, h, &rect)) {
        return;
    }
    fit_view_box(aw, ah, rect, aspect, &vw, &vh);
    sv_aligned_pos(aspect.align, rect.l, rect.t, rect_w(rect) - vw, rect_h(rect) - vh, &vx, &vy);
    image_ts = ts_row(vw / aw, 0.0f, 0.0f, vh / ah, vx, vy);
    img = sv_alloc(c->a, sizeof *img);
    g = sv_group_empty(c->a);
    if (img == NULL || g == NULL) {
        c->nomem = true;
        return;
    }
    img->kind = UN_IMAGE;
    img->id = "";
    vis = find_str(n, A_VISIBILITY);
    img->visible = vis == NULL || strcmp(vis, "visible") == 0;
    rend = find_str(n, A_IMAGE_RENDERING);
    img->pixelated = rend != NULL && (strcmp(rend, "optimizeSpeed") == 0 ||
                                      strcmp(rend, "crisp-edges") == 0 || strcmp(rend, "pixelated") == 0);
    /* resvg's FilterQuality for the image-rendering value (auto / optimizeQuality /
     * high-quality / unknown: bicubic) */
    img->quality = img->pixelated                                ? SQ_NEAREST
                   : rend != NULL && strcmp(rend, "smooth") == 0 ? SQ_BILINEAR
                                                                 : SQ_BICUBIC;
    img->iw = li.iw;
    img->ih = li.ih;
    img->rgba = li.rgba;
    img->svg = li.svg;
    img->size_w = aw;
    img->size_h = ah;
    img->transform = ts_identity();
    img->abs_transform = ts_pre_concat(parent->abs_transform, image_ts);
    if (!rect_transform(rect, img->abs_transform, &img->abs_bbox)) {
        return;
    }
    (void)rect_from_xywh(0.0f, 0.0f, aw, ah, &img->bbox);
    img->stroke_bbox = img->bbox;
    img->abs_stroke_bbox = img->abs_bbox;
    g->id = st->markers == NULL ? st_id(n) : "";
    if (!sv_push_child(c->a, g, img)) {
        c->nomem = true;
        return;
    }
    g->transform = image_ts;
    g->abs_transform = img->abs_transform;
    (void)sv_group_bboxes(g);
    if (aspect.slice) {
        sv_unode *g2 = sv_group_empty(c->a);
        sv_clip *clip = sv_alloc(c->a, sizeof *clip);
        sv_unode *root = sv_group_empty(c->a);
        sv_fill *fill = sv_alloc(c->a, sizeof *fill);
        const sv_path *data = path_from_rect(c->a, rect);
        sv_unode *p;
        if (g2 == NULL || clip == NULL || root == NULL || fill == NULL || data == NULL) {
            c->nomem = true;
            return;
        }
        fill->paint.kind = PAINT_COLOR;
        fill->opacity = 1.0f;
        p = sv_new_path(c, "", true, fill, NULL, false, data, ts_identity());
        if (p == NULL || !sv_push_child(c->a, root, p)) {
            c->nomem = true;
            return;
        }
        clip->transform = ts_identity();
        clip->root = root;
        g2->id = g->id;
        g->id = "";
        g2->abs_transform = parent->abs_transform;
        g2->clip = clip;
        if (!sv_push_child(c->a, g2, g)) {
            c->nomem = true;
            return;
        }
        (void)sv_group_bboxes(g2);
        g = g2;
    }
    if (!sv_push_child(c->a, parent, g)) {
        c->nomem = true;
    }
}
