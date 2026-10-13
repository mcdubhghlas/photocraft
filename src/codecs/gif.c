/* GIF (REWRITE_PLAN.md D24): the first frame, drawn onto a transparent-black logical screen as
 * RGBA (a transparent index keeps its palette colour with alpha 0), as the reference's image/gif
 * crates do; more frames add a MoreFrames warning (port of via_image.rs gif_more_frames).
 * Writing: alpha 0 becomes the transparent index, other alpha opaque; up to 256 colours are
 * kept exactly, more are quantised by median cut (the reference used NeuQuant). */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "codecs/internal.h"
#include "codecs/web.h"

#define F PC_CF_GIF

/* Counts the frames (image descriptors) by walking the blocks without decompressing; the total
 * is unknown when the blocks end before the trailer or break off. */
static bool more_frames(const uint8_t *b, size_t n, pc_cwarning *w)
{
    size_t i;
    uint32_t frames = 0;
    bool known = false;
#define TABLE(flags) (((flags) & 0x80) != 0 ? (size_t)3 << (((flags) & 7) + 1) : 0)
    if (n <= 10) {
        return false;
    }
    i = 13 + TABLE(b[10]);
    for (;;) {
        size_t j;
        if (i >= n) {
            break;
        }
        if (b[i] == 0x3B) {
            known = true;
            break;
        }
        if (b[i] == 0x21) {
            j = i + 2;
        } else if (b[i] == 0x2C) {
            frames = frames < UINT32_MAX ? frames + 1 : frames;
            if (i + 9 >= n) {
                break;
            }
            j = i + 11 + TABLE(b[i + 9]);
        } else {
            break;
        }
        /* data sub-blocks, ended by a zero length */
        for (;;) {
            size_t len;
            if (j >= n) {
                j = SIZE_MAX;
                break;
            }
            len = b[j];
            j += 1 + len;
            if (len == 0) {
                break;
            }
        }
        if (j == SIZE_MAX) {
            break;
        }
        i = j;
    }
#undef TABLE
    if (frames <= 1) {
        return false;
    }
    memset(w, 0, sizeof *w);
    w->kind = PC_CW_MORE_FRAMES;
    w->has_total = known;
    w->total = frames;
    return true;
}

/* GIF LZW (LSB first, deferred clear); fills up to need indices. false on malformed data or
 * when the data ends before need indices. */
static bool lzw_decode(const uint8_t *in, size_t n, unsigned min, uint8_t *out, size_t need)
{
    uint16_t prefix[4096] = {0};
    uint8_t suffix[4096] = {0};
    uint16_t length[4096] = {0};
    uint8_t stack[4097] = {0};
    unsigned clear = 1u << min;
    unsigned eoi = clear + 1;
    unsigned size = min + 1;
    unsigned next = eoi + 1;
    int prev = -1;
    size_t pos = 0;
    uint32_t acc = 0;
    unsigned nbits = 0;
    size_t o = 0;
    unsigned i;
    for (i = 0; i < clear; i++) {
        prefix[i] = 0;
        suffix[i] = (uint8_t)i;
        length[i] = 1;
    }
    while (o < need) {
        unsigned code;
        unsigned c;
        unsigned len;
        unsigned k;
        while (nbits < size) {
            if (pos >= n) {
                return false;
            }
            acc |= (uint32_t)in[pos++] << nbits;
            nbits += 8;
        }
        code = acc & ((1u << size) - 1);
        acc >>= size;
        nbits -= size;
        if (code == clear) {
            size = min + 1;
            next = eoi + 1;
            prev = -1;
            continue;
        }
        if (code == eoi) {
            return false;
        }
        if (prev < 0) {
            if (code >= clear) {
                return false;
            }
            out[o++] = (uint8_t)code;
            prev = (int)code;
            continue;
        }
        if (code > next || (code == next && next >= 4096)) {
            return false;
        }
        c = code < next ? code : (unsigned)prev;
        len = length[c];
        for (k = len; k > 0; k--) {
            stack[k - 1] = suffix[c];
            c = prefix[c];
        }
        if (code == next) {
            stack[len++] = stack[0];
        }
        if (next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = stack[0];
            length[next] = (uint16_t)(length[prev] + 1);
            next++;
            if (next == (1u << size) && size < 12) {
                size++;
            }
        }
        for (k = 0; k < len && o < need; k++) {
            out[o++] = stack[k];
        }
        prev = (int)code;
    }
    return true;
}

pc_status cx_gif_open(const uint8_t *b, size_t n, cx_gif *g, pc_error *err)
{
    memset(g, 0, sizeof *g);
    if (n < 13 || (memcmp(b, "GIF87a", 6) != 0 && memcmp(b, "GIF89a", 6) != 0)) {
        return cx_malformed(err, F, "not a GIF file");
    }
    g->b = b;
    g->n = n;
    g->sw = cx_le16(b + 6);
    g->sh = cx_le16(b + 8);
    g->pos = 13;
    if (b[10] & 0x80) {
        g->ngct = (size_t)1 << ((b[10] & 7) + 1);
        if (n - g->pos < 3 * g->ngct) {
            return cx_malformed(err, F, "file ends inside the global colour table");
        }
        g->gct = b + g->pos;
        g->pos += 3 * g->ngct;
    }
    return PC_OK;
}

/* Skips data sub-blocks from *i (ended by a zero length); copies up to cap bytes of their
 * contents into out (when not NULL) and counts them all in *total. */
static bool sub_blocks(const uint8_t *b, size_t n, size_t *i, uint8_t *out, size_t cap, size_t *total)
{
    *total = 0;
    for (;;) {
        size_t len;
        if (*i >= n) {
            return false;
        }
        len = b[*i];
        if (n - *i - 1 < len) {
            return false;
        }
        if (out != NULL && *total < cap) {
            size_t k = cap - *total < len ? cap - *total : len;
            memcpy(out + *total, b + *i + 1, k);
        }
        *total += len;
        *i += 1 + len;
        if (len == 0) {
            return true;
        }
    }
}

pc_status cx_gif_next(cx_gif *g, uint64_t budget, bool band_free, cx_gif_frame *f, bool *done, pc_error *err)
{
    const uint8_t *b = g->b;
    size_t n = g->n;
    size_t i = g->pos;
    int transparent = -1;
    int disposal = 0;
    uint16_t delay = 0;
    bool any = false;
    memset(f, 0, sizeof *f);
    *done = false;
    for (;;) {
        uint8_t kind;
        if (i >= n) {
            return cx_malformed(err, F,
                                any ? "file ends before the next image" : "file ends before the first image");
        }
        kind = b[i];
        if (kind == 0x3B) {
            *done = true;
            g->pos = i;
            return PC_OK;
        }
        if (kind == 0x21) {
            uint8_t label;
            uint8_t ctl[4];
            size_t total;
            if (n - i < 2) {
                return cx_malformed(err, F, "file ends inside an extension");
            }
            label = b[i + 1];
            i += 2;
            if (!sub_blocks(b, n, &i, ctl, sizeof ctl, &total)) {
                return cx_malformed(err, F, "file ends inside an extension");
            }
            if (label == 0xF9) {
                if (total != 4) {
                    return cx_malformed(err, F, "control extension has wrong length");
                }
                disposal = (ctl[0] >> 2) & 7;
                disposal = disposal > 3 ? 0 : disposal;
                delay = (uint16_t)(ctl[1] | (ctl[2] << 8));
                transparent = (ctl[0] & 1) ? ctl[3] : -1;
            }
            any = true;
            continue;
        }
        if (kind != 0x2C) {
            return cx_malformed(err, F, "unknown block type %u", (unsigned)kind);
        }
        break;
    }
    {
        uint8_t flags;
        const uint8_t *pal = g->gct;
        size_t npal = g->ngct;
        unsigned min;
        pc_buf data = PC_BUF_INIT;
        uint8_t *idx;
        size_t fw;
        size_t fh;
        size_t px;
        size_t x;
        size_t y;
        pc_status st;
        if (n - i < 10) {
            return cx_malformed(err, F, "file ends inside an image descriptor");
        }
        f->left = cx_le16(b + i + 1);
        f->top = cx_le16(b + i + 3);
        f->w = cx_le16(b + i + 5);
        f->h = cx_le16(b + i + 7);
        flags = b[i + 9];
        i += 10;
        if (flags & 0x80) {
            npal = (size_t)1 << ((flags & 7) + 1);
            if (n - i < 3 * npal) {
                return cx_malformed(err, F, "file ends inside a local colour table");
            }
            pal = b + i;
            i += 3 * npal;
        }
        if (pal == NULL) {
            return cx_malformed(err, F, "no color table available for current frame");
        }
        fw = f->w;
        fh = f->h;
        px = fw * fh;
        if ((uint64_t)px * 4 > budget &&
            !(band_free && f->left == 0 && f->w == g->sw && (uint64_t)f->top + f->h <= g->sh)) {
            return cx_limit(err, "Memory limit exceeded");
        }
        if (i >= n) {
            return cx_malformed(err, F, "file ends before the image data");
        }
        min = b[i++];
        if (min < 1 || min > 11) {
            return cx_malformed(err, F, "invalid minimal code size %u", min);
        }
        for (;;) {
            size_t len;
            if (i >= n) {
                pc_buf_fini(&data);
                return cx_malformed(err, F, "image truncated");
            }
            len = b[i];
            if (n - i - 1 < len) {
                pc_buf_fini(&data);
                return cx_malformed(err, F, "image truncated");
            }
            i += 1 + len;
            if (len == 0) {
                break;
            }
            if ((st = pc_buf_append(&data, b + i - len, len, err)) != PC_OK) {
                pc_buf_fini(&data);
                return st;
            }
        }
        idx = calloc(px > 0 ? px : 1, 1);
        f->rgba = calloc(px > 0 ? px * 4 : 1, 1);
        if (idx == NULL || f->rgba == NULL) {
            free(idx);
            free(f->rgba);
            f->rgba = NULL;
            pc_buf_fini(&data);
            return cx_nomem(err);
        }
        if (px > 0 && !lzw_decode(data.data, data.len, min, idx, px)) {
            free(idx);
            free(f->rgba);
            f->rgba = NULL;
            pc_buf_fini(&data);
            return cx_malformed(err, F, "image truncated");
        }
        pc_buf_fini(&data);
        for (y = 0; y < fh; y++) {
            size_t sy = y;
            if (flags & 0x40) {
                /* interlaced: the rows were stored in passes */
                size_t p1 = (fh + 7) / 8;
                size_t p2 = (fh + 3) / 8;
                size_t p3 = (fh + 1) / 4;
                if (y % 8 == 0) {
                    sy = y / 8;
                } else if (y % 8 == 4) {
                    sy = p1 + y / 8;
                } else if (y % 4 == 2) {
                    sy = p1 + p2 + y / 4;
                } else {
                    sy = p1 + p2 + p3 + y / 2;
                }
            }
            for (x = 0; x < fw; x++) {
                uint8_t k = idx[sy * fw + x];
                uint8_t *o = f->rgba + (y * fw + x) * 4;
                if ((size_t)k < npal) {
                    o[0] = pal[3 * k];
                    o[1] = pal[3 * k + 1];
                    o[2] = pal[3 * k + 2];
                    o[3] = transparent == k ? 0 : 0xFF;
                }
            }
        }
        free(idx);
        f->disposal = disposal;
        f->delay_cs = delay;
        g->pos = i;
        return PC_OK;
    }
}

pc_status cx_gif_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    cx_gif g;
    cx_gif_frame f;
    bool done;
    pc_cimage img;
    pc_status st;
    size_t x;
    size_t y;
    uint64_t screen;
    if ((st = cx_gif_open(b, n, &g, err)) != PC_OK) {
        return st;
    }
    if ((st = pc_climits_check(l, g.sw, g.sh, PC_CL_RGBA, PC_CS_U8, err)) != PC_OK) {
        return st;
    }
    screen = (uint64_t)g.sw * g.sh * 4;
    /* The output is budgeted; a frame that is not a full-width band of it needs its own buffer. */
    if ((st = cx_gif_next(&g, l->max_alloc - screen, true, &f, &done, err)) != PC_OK) {
        return st;
    }
    if (done) {
        return cx_malformed(err, F, "the file has no image");
    }
    if ((st = pc_cimage_new(g.sw, g.sh, PC_CL_RGBA, PC_CS_U8, &img, err)) != PC_OK) {
        free(f.rgba);
        return st;
    }
    /* The frame drawn onto the transparent logical screen, cropped to it. */
    for (y = 0; y < f.h && f.top + y < g.sh; y++) {
        for (x = 0; x < f.w && f.left + x < g.sw; x++) {
            memcpy(img.data + (((size_t)f.top + y) * g.sw + f.left + x) * 4, f.rgba + (y * f.w + x) * 4, 4);
        }
    }
    free(f.rgba);
    {
        pc_cwarning w;
        if (more_frames(b, n, &w) && (st = pc_cimage_add_warning(&img, w, err)) != PC_OK) {
            pc_cimage_fini(&img);
            return st;
        }
    }
    *out = img;
    return PC_OK;
}

/* ---- quantisation ---- */

typedef struct box {
    size_t lo; /* range into the colour list */
    size_t hi;
} box;

typedef struct col {
    uint8_t c[3];
    uint32_t count;
} col;

static int cmp0(const void *a, const void *b) { return (int)((const col *)a)->c[0] - (int)((const col *)b)->c[0]; }
static int cmp1(const void *a, const void *b) { return (int)((const col *)a)->c[1] - (int)((const col *)b)->c[1]; }
static int cmp2(const void *a, const void *b) { return (int)((const col *)a)->c[2] - (int)((const col *)b)->c[2]; }

/* Median cut of the distinct colours into at most k boxes; each box's weighted mean becomes a
 * palette entry. */
static size_t median_cut(col *cols, size_t ncols, size_t k, uint8_t (*pal)[3])
{
    box *boxes = malloc(k * sizeof *boxes);
    size_t nb = 1;
    size_t i;
    if (boxes == NULL) {
        return 0;
    }
    boxes[0].lo = 0;
    boxes[0].hi = ncols;
    while (nb < k) {
        size_t best = SIZE_MAX;
        int axis = 0;
        int range = -1;
        size_t mid;
        for (i = 0; i < nb; i++) {
            int a;
            if (boxes[i].hi - boxes[i].lo < 2) {
                continue;
            }
            for (a = 0; a < 3; a++) {
                int lo = 255;
                int hi = 0;
                size_t j;
                for (j = boxes[i].lo; j < boxes[i].hi; j++) {
                    lo = cols[j].c[a] < lo ? cols[j].c[a] : lo;
                    hi = cols[j].c[a] > hi ? cols[j].c[a] : hi;
                }
                if (hi - lo > range) {
                    range = hi - lo;
                    best = i;
                    axis = a;
                }
            }
        }
        if (best == SIZE_MAX || range <= 0) {
            break;
        }
        qsort(cols + boxes[best].lo, boxes[best].hi - boxes[best].lo, sizeof *cols,
              axis == 0 ? cmp0 : axis == 1 ? cmp1 : cmp2);
        {
            /* split at the weighted median */
            uint64_t total = 0;
            uint64_t acc = 0;
            size_t j;
            for (j = boxes[best].lo; j < boxes[best].hi; j++) {
                total += cols[j].count;
            }
            mid = boxes[best].lo + 1;
            for (j = boxes[best].lo; j < boxes[best].hi - 1; j++) {
                acc += cols[j].count;
                if (acc * 2 >= total) {
                    mid = j + 1;
                    break;
                }
            }
        }
        boxes[nb].lo = mid;
        boxes[nb].hi = boxes[best].hi;
        boxes[best].hi = mid;
        nb++;
    }
    for (i = 0; i < nb; i++) {
        uint64_t s[3] = {0, 0, 0};
        uint64_t total = 0;
        size_t j;
        int a;
        for (j = boxes[i].lo; j < boxes[i].hi; j++) {
            for (a = 0; a < 3; a++) {
                s[a] += (uint64_t)cols[j].c[a] * cols[j].count;
            }
            total += cols[j].count;
        }
        for (a = 0; a < 3; a++) {
            pal[i][a] = (uint8_t)(total > 0 ? (s[a] + total / 2) / total : 0);
        }
    }
    free(boxes);
    return nb;
}

typedef struct hent {
    uint32_t key; /* rgb + 1 (0 = empty) */
    uint32_t val;
} hent;

pc_status cx_gif_encode(const pc_cimage *src, cx_plan p, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    size_t npx;
    size_t i;
    enum { HS = 1 << 17 };
    hent *h = NULL;
    col *cols = NULL;
    size_t ncols = 0;
    bool any_transparent = false;
    uint8_t pal[256][3];
    size_t npal = 0;
    uint8_t *idx = NULL;
    int transparent = -1;
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    npx = pc_cimage_pixel_count(img);
    h = calloc(HS, sizeof *h);
    idx = malloc(npx > 0 ? npx : 1);
    if (h == NULL || idx == NULL) {
        st = cx_nomem(err);
        goto done;
    }
    /* distinct opaque colours (counting) */
    for (i = 0; i < npx; i++) {
        const uint8_t *px = img->data + 4 * i;
        uint32_t key;
        size_t s;
        if (px[3] == 0) {
            any_transparent = true;
            continue;
        }
        key = ((uint32_t)px[0] << 16 | (uint32_t)px[1] << 8 | px[2]) + 1;
        s = (key * 2654435761u) & (HS - 1);
        while (h[s].key != 0 && h[s].key != key) {
            s = (s + 1) & (HS - 1);
        }
        if (h[s].key == 0) {
            h[s].key = key;
            h[s].val = 0;
            ncols++;
        }
        h[s].val++;
    }
    {
        size_t room = any_transparent ? 255 : 256;
        size_t k = 0;
        cols = malloc((ncols > 0 ? ncols : 1) * sizeof *cols);
        if (cols == NULL) {
            st = cx_nomem(err);
            goto done;
        }
        for (i = 0; i < HS; i++) {
            if (h[i].key != 0) {
                uint32_t c = h[i].key - 1;
                cols[k].c[0] = (uint8_t)(c >> 16);
                cols[k].c[1] = (uint8_t)(c >> 8);
                cols[k].c[2] = (uint8_t)c;
                cols[k].count = h[i].val;
                k++;
            }
        }
        if (ncols <= room) {
            for (i = 0; i < ncols; i++) {
                memcpy(pal[i], cols[i].c, 3);
            }
            npal = ncols;
        } else {
            npal = median_cut(cols, ncols, room, pal);
            if (npal == 0) {
                st = cx_nomem(err);
                goto done;
            }
        }
        if (any_transparent) {
            transparent = (int)npal;
            pal[npal][0] = pal[npal][1] = pal[npal][2] = 0;
            npal++;
        }
        if (npal == 0) {
            pal[0][0] = pal[0][1] = pal[0][2] = 0;
            npal = 1;
        }
    }
    /* map every pixel to its palette entry (exact colours hash to themselves) */
    memset(h, 0, HS * sizeof *h);
    for (i = 0; i < npx; i++) {
        const uint8_t *px = img->data + 4 * i;
        uint32_t key;
        size_t s;
        if (px[3] == 0) {
            idx[i] = (uint8_t)transparent;
            continue;
        }
        key = ((uint32_t)px[0] << 16 | (uint32_t)px[1] << 8 | px[2]) + 1;
        s = (key * 2654435761u) & (HS - 1);
        while (h[s].key != 0 && h[s].key != key) {
            s = (s + 1) & (HS - 1);
        }
        if (h[s].key == 0) {
            size_t best = 0;
            uint32_t bd = UINT32_MAX;
            size_t k;
            size_t lim = transparent >= 0 ? (size_t)transparent : npal;
            for (k = 0; k < lim; k++) {
                int dr = (int)px[0] - pal[k][0];
                int dg = (int)px[1] - pal[k][1];
                int db = (int)px[2] - pal[k][2];
                uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
                if (d < bd) {
                    bd = d;
                    best = k;
                }
            }
            h[s].key = key;
            h[s].val = (uint32_t)best;
        }
        idx[i] = (uint8_t)h[s].val;
    }
    st = pc_web_encode_gif_indexed(img->width, img->height, idx, npx, (const uint8_t(*)[3])pal, npal, transparent,
                                   false, out, err);
done:
    free(h);
    free(cols);
    free(idx);
    pc_cimage_fini(&tmp);
    return st;
}
