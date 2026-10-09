#include "codecs/orientation.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "base/pool.h"
#include "codecs/internal.h"

#define TAG_ORIENTATION 274
#define TYPE_SHORT 3
#define TYPE_LONG 4

typedef struct tif {
    const uint8_t *b;
    size_t n;
    bool le;
} tif;

static bool rd(const tif *t, size_t at, size_t k, uint64_t *v)
{
    size_t i;
    uint64_t x = 0;
    if (at > t->n || k > t->n - at) {
        return false;
    }
    for (i = 0; i < k; i++) {
        uint8_t byte = t->le ? t->b[at + k - 1 - i] : t->b[at + i];
        x = (x << 8) | byte;
    }
    *v = x;
    return true;
}

static bool add(size_t a, uint64_t b, size_t *out)
{
    if (b > SIZE_MAX - a) {
        return false;
    }
    *out = a + (size_t)b;
    return true;
}

/* The TIFF payload (a JPEG-style Exif\0\0 prefix skipped). */
static size_t body_offset(const uint8_t *b, size_t n)
{
    return n >= 6 && memcmp(b, "Exif\0\0", 6) == 0 ? 6 : 0;
}

typedef struct ifd0 {
    size_t first;
    size_t count;
    size_t entry;
    bool big;
} ifd0;

/* IFD0: the whole table and the next-IFD pointer must be present, after the header. */
static bool ifd0_entries(tif *t, ifd0 *out)
{
    uint64_t v;
    uint64_t at;
    uint64_t count;
    size_t count_bytes;
    size_t entry;
    size_t next_bytes;
    size_t header;
    size_t ifd;
    size_t first;
    size_t next;
    size_t total;
    if (t->n < 4) {
        return false;
    }
    if (memcmp(t->b, "II\x2a\0", 4) == 0 || memcmp(t->b, "II\x2b\0", 4) == 0) {
        t->le = true;
    } else if (memcmp(t->b, "MM\0\x2a", 4) == 0 || memcmp(t->b, "MM\0\x2b", 4) == 0) {
        t->le = false;
    } else {
        return false;
    }
    out->big = t->b[2] == 0x2b || t->b[3] == 0x2b;
    if (out->big) {
        if (!rd(t, 4, 2, &v) || v != 8 || !rd(t, 6, 2, &v) || v != 0 || !rd(t, 8, 8, &at)) {
            return false;
        }
        count_bytes = 8;
        entry = 20;
        next_bytes = 8;
        header = 16;
    } else {
        if (!rd(t, 4, 4, &at)) {
            return false;
        }
        count_bytes = 2;
        entry = 12;
        next_bytes = 4;
        header = 8;
    }
    if (at > SIZE_MAX || at < header) {
        return false;
    }
    ifd = (size_t)at;
    if (!rd(t, ifd, count_bytes, &count) || count > SIZE_MAX / entry) {
        return false;
    }
    if (!add(ifd, count_bytes, &first) || !add(first, count * entry, &next) || !add(next, next_bytes, &total) ||
        total > t->n) {
        return false;
    }
    out->first = first;
    out->count = (size_t)count;
    out->entry = entry;
    return true;
}

typedef struct val {
    size_t at;
    uint16_t ty;
} val;

/* The entry at e as a well-formed Orientation entry: exactly one SHORT or LONG. */
static bool orientation_value(const tif *t, size_t e, bool big, val *out)
{
    uint64_t count;
    uint64_t tag;
    uint64_t ty;
    if (!rd(t, e + 4, big ? 8 : 4, &count) || !rd(t, e, 2, &tag) || tag != TAG_ORIENTATION || count != 1 ||
        !rd(t, e + 2, 2, &ty) || (ty != TYPE_SHORT && ty != TYPE_LONG)) {
        return false;
    }
    out->at = e + (big ? 12 : 8);
    out->ty = (uint16_t)ty;
    return true;
}

static bool read_val(const tif *t, val v, uint64_t *out) { return rd(t, v.at, v.ty == TYPE_SHORT ? 2 : 4, out); }

uint16_t pc_exif_orientation(const uint8_t *exif, size_t n)
{
    size_t off = body_offset(exif, n);
    tif t = {exif + off, n - off, false};
    ifd0 d;
    size_t i;
    if (!ifd0_entries(&t, &d)) {
        return 1;
    }
    for (i = 0; i < d.count; i++) {
        size_t e = d.first + i * d.entry;
        uint64_t tag;
        if (rd(&t, e, 2, &tag) && tag == TAG_ORIENTATION) {
            val v;
            uint64_t x;
            /* The first entry wins; a malformed first one hides later duplicates. */
            if (orientation_value(&t, e, d.big, &v) && read_val(&t, v, &x) && x >= 1 && x <= 8) {
                return (uint16_t)x;
            }
            return 1;
        }
    }
    return 1;
}

pc_status pc_upright_exif(const uint8_t *exif, size_t n, uint8_t **out, size_t *out_len, pc_error *err)
{
    size_t off = body_offset(exif, n);
    tif t = {exif + off, n - off, false};
    ifd0 d;
    size_t i;
    bool all_one = true;
    uint8_t *c;
    *out = NULL;
    *out_len = 0;
    if (!ifd0_entries(&t, &d)) {
        return PC_OK;
    }
    for (i = 0; i < d.count; i++) {
        val v;
        uint64_t x;
        if (orientation_value(&t, d.first + i * d.entry, d.big, &v) && !(read_val(&t, v, &x) && x == 1)) {
            all_one = false;
        }
    }
    if (all_one) {
        return PC_OK;
    }
    c = malloc(n > 0 ? n : 1);
    if (c == NULL) {
        return cx_nomem(err);
    }
    memcpy(c, exif, n);
    for (i = 0; i < d.count; i++) {
        val v;
        if (orientation_value(&t, d.first + i * d.entry, d.big, &v)) {
            size_t k = v.ty == TYPE_SHORT ? 2 : 4;
            size_t at = off + v.at;
            if (at + k <= n) {
                memset(c + at, 0, k);
                c[t.le ? at : at + k - 1] = 1;
            }
        }
    }
    *out = c;
    *out_len = n;
    return PC_OK;
}

static bool ascii_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

static const char *find(const char *s, size_t n, const char *key, size_t kl)
{
    size_t i;
    for (i = 0; i + kl <= n; i++) {
        if (memcmp(s + i, key, kl) == 0) {
            return s + i;
        }
    }
    return NULL;
}

pc_status pc_upright_xmp(const char *xmp, size_t n, char **out, size_t *out_len, pc_error *err)
{
    static const char KEY[] = "tiff:Orientation";
    const size_t kl = sizeof KEY - 1;
    pc_buf o = PC_BUF_INIT;
    const char *rest = xmp;
    size_t rn = n;
    bool changed = false;
    pc_status st = PC_OK;
    const char *at;
    *out = NULL;
    *out_len = 0;
    if (find(xmp, n, KEY, kl) == NULL) {
        return PC_OK;
    }
    while (st == PC_OK && (at = find(rest, rn, KEY, kl)) != NULL) {
        size_t head = (size_t)(at - rest) + kl;
        size_t j = 0;
        size_t last = SIZE_MAX;
        size_t start;
        size_t digits = 0;
        st = pc_buf_append(&o, rest, head, err);
        rest += head;
        rn -= head;
        /* tiff:Orientation="6", ='6' or >6< */
        while (j < rn && (ascii_ws(rest[j]) || rest[j] == '=' || rest[j] == '"' || rest[j] == '\'' || rest[j] == '>')) {
            last = j++;
        }
        if (last == SIZE_MAX || (rest[last] != '"' && rest[last] != '\'' && rest[last] != '>')) {
            continue;
        }
        start = last + 1;
        while (start + digits < rn && rest[start + digits] >= '0' && rest[start + digits] <= '9') {
            digits++;
        }
        if (digits == 0 || (digits == 1 && rest[start] == '1')) {
            continue;
        }
        if (st == PC_OK) {
            st = pc_buf_append(&o, rest, start, err);
        }
        if (st == PC_OK) {
            st = pc_buf_append_byte(&o, '1', err);
        }
        rest += start + digits;
        rn -= start + digits;
        changed = true;
    }
    if (st == PC_OK) {
        st = pc_buf_append(&o, rest, rn, err);
    }
    if (st != PC_OK || !changed) {
        pc_buf_fini(&o);
        return st;
    }
    *out_len = o.len;
    *out = (char *)o.data; /* pc_buf keeps a NUL after len */
    return PC_OK;
}

/* ---- the pixel turn ---- */

/* Source pixel index of output pixel (0, y2) and the step per output column. */
static void walk(uint16_t o, ptrdiff_t w, ptrdiff_t h, ptrdiff_t y2, ptrdiff_t *base, ptrdiff_t *step)
{
    switch (o) {
    case 2:
        *base = y2 * w + w - 1;
        *step = -1;
        break;
    case 3:
        *base = (h - 1 - y2) * w + w - 1;
        *step = -1;
        break;
    case 4:
        *base = (h - 1 - y2) * w;
        *step = 1;
        break;
    case 5:
        *base = y2;
        *step = w;
        break;
    case 6:
        *base = (h - 1) * w + y2;
        *step = -w;
        break;
    case 7:
        *base = (h - 1) * w + (w - 1 - y2);
        *step = -w;
        break;
    case 8:
        *base = w - 1 - y2;
        *step = w;
        break;
    default:
        *base = y2 * w;
        *step = 1;
    }
}

#define BLOCK 64
#define BAND 64

typedef struct orient_job {
    const uint8_t *src;
    uint8_t *dst;
    size_t bpp;
    uint16_t o;
    size_t w;
    size_t h;
    size_t ow;
    size_t oh;
} orient_job;

static pc_status band(void *ctx, size_t chunk, size_t y0, size_t y1)
{
    const orient_job *j = ctx;
    size_t npx = j->w * j->h;
    size_t x0;
    (void)chunk;
    for (x0 = 0; x0 < j->ow; x0 += BLOCK) {
        size_t x1 = x0 + BLOCK < j->ow ? x0 + BLOCK : j->ow;
        size_t y;
        for (y = y0; y < y1; y++) {
            ptrdiff_t base;
            ptrdiff_t step;
            ptrdiff_t i;
            size_t x;
            uint8_t *d = j->dst + (y * j->ow + x0) * j->bpp;
            walk(j->o, (ptrdiff_t)j->w, (ptrdiff_t)j->h, (ptrdiff_t)y, &base, &step);
            i = base + (ptrdiff_t)x0 * step;
            for (x = x0; x < x1; x++, i += step, d += j->bpp) {
                if (i >= 0 && (size_t)i < npx) {
                    memcpy(d, j->src + (size_t)i * j->bpp, j->bpp);
                }
            }
        }
    }
    return PC_OK;
}

pc_status pc_cimage_oriented(pc_cimage *img, uint16_t o, pc_cimage *out, pc_error *err)
{
    size_t w = img->width;
    size_t h = img->height;
    bool swap = o >= 5;
    size_t bpp = pc_clayout_channels(img->layout) * pc_csample_bytes(img->sample);
    orient_job j;
    uint8_t *data;
    uint8_t *ex = NULL;
    size_t exn = 0;
    char *xm = NULL;
    size_t xmn = 0;
    pc_status st;
    if (o < 2 || o > 8) {
        *out = *img;
        pc_cimage_init(img);
        return PC_OK;
    }
    if (w == 0 || h == 0) {
        st = cx_invalid(err, "cannot orient %s %s %zux%zu", pc_clayout_name(img->layout), pc_csample_name(img->sample),
                        w, h);
        pc_cimage_fini(img);
        return st;
    }
    data = calloc(img->len > 0 ? img->len : 1, 1);
    if (data == NULL) {
        st = cx_limit(err, "cannot allocate %zu bytes to orient the image", img->len);
        pc_cimage_fini(img);
        return st;
    }
    j.src = img->data;
    j.dst = data;
    j.bpp = bpp;
    j.o = o;
    j.w = w;
    j.h = h;
    j.ow = swap ? h : w;
    j.oh = swap ? w : h;
    (void)pc_pool_for(pc_pool_global(), j.oh, BAND, band, &j);
    if (img->meta.exif.has) {
        st = pc_upright_exif(img->meta.exif.data, img->meta.exif.len, &ex, &exn, err);
        if (st != PC_OK) {
            free(data);
            pc_cimage_fini(img);
            return st;
        }
    }
    if (img->meta.xmp.has) {
        st = pc_upright_xmp((const char *)img->meta.xmp.data, img->meta.xmp.len, &xm, &xmn, err);
        if (st != PC_OK) {
            free(ex);
            free(data);
            pc_cimage_fini(img);
            return st;
        }
    }
    if (ex != NULL) {
        free(img->meta.exif.data);
        img->meta.exif.data = ex;
        img->meta.exif.len = exn;
    }
    if (xm != NULL) {
        free(img->meta.xmp.data);
        img->meta.xmp.data = (uint8_t *)xm;
        img->meta.xmp.len = xmn;
    }
    if (swap && img->meta.has_dpi) {
        float t = img->meta.dpi_x;
        img->meta.dpi_x = img->meta.dpi_y;
        img->meta.dpi_y = t;
    }
    free(img->data);
    img->data = data;
    img->width = (uint32_t)j.ow;
    img->height = (uint32_t)j.oh;
    *out = *img;
    pc_cimage_init(img);
    return PC_OK;
}
