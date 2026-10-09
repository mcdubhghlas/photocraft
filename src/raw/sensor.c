/* Undeveloped sensor data and the shared strip/tile reader (port of crates/raw/src/sensor.rs),
 * plus the small shared helpers (rectangles, CFA, black levels, warning lists, par.rs). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "raw/internal.h"

size_t rx_band_rows(size_t width)
{
    size_t r = 65536 / (width > 0 ? width : 1);
    return r < 1 ? 1 : r > 256 ? 256 : r;
}

/* ---- rectangles ---- */

pc_raw_rect pc_raw_rect_make(size_t x, size_t y, size_t width, size_t height)
{
    pc_raw_rect r;
    r.x = x;
    r.y = y;
    r.width = width;
    r.height = height;
    return r;
}

static size_t sat_add(size_t a, size_t b) { return a > SIZE_MAX - b ? SIZE_MAX : a + b; }

pc_raw_rect pc_raw_rect_intersect(pc_raw_rect a, pc_raw_rect b)
{
    size_t x0 = a.x > b.x ? a.x : b.x;
    size_t y0 = a.y > b.y ? a.y : b.y;
    size_t ax1 = sat_add(a.x, a.width);
    size_t bx1 = sat_add(b.x, b.width);
    size_t ay1 = sat_add(a.y, a.height);
    size_t by1 = sat_add(b.y, b.height);
    size_t x1 = ax1 < bx1 ? ax1 : bx1;
    size_t y1 = ay1 < by1 ? ay1 : by1;
    return pc_raw_rect_make(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

bool pc_raw_rect_is_empty(pc_raw_rect r) { return r.width == 0 || r.height == 0; }

bool pc_raw_rect_eq(pc_raw_rect a, pc_raw_rect b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/* ---- CFA ---- */

bool pc_raw_cfa_bayer(const char *pattern, pc_raw_cfa *out)
{
    pc_raw_cfa c;
    size_t i;
    if (pattern == NULL || strlen(pattern) != 4) {
        return false;
    }
    memset(&c, 0, sizeof c);
    for (i = 0; i < 4; i++) {
        switch (pattern[i]) {
        case 'R':
            c.colors[i] = 0;
            break;
        case 'G':
            c.colors[i] = 1;
            break;
        case 'B':
            c.colors[i] = 2;
            break;
        default:
            return false;
        }
    }
    c.width = 2;
    c.height = 2;
    *out = c;
    return true;
}

uint8_t pc_raw_cfa_color(const pc_raw_cfa *c, size_t x, size_t y)
{
    size_t cx;
    size_t cy;
    if (c->width == 0 || c->height == 0 || c->width > PC_RAW_CFA_MAX || c->height > PC_RAW_CFA_MAX) {
        return 1;
    }
    cx = (x + c->width - c->origin_x % c->width) % c->width;
    cy = (y + c->height - c->origin_y % c->height) % c->height;
    return c->colors[cy * c->width + cx];
}

void pc_raw_cfa_phase(const pc_raw_cfa *c, size_t x0, size_t y0, uint8_t out[4])
{
    out[0] = pc_raw_cfa_color(c, x0, y0);
    out[1] = pc_raw_cfa_color(c, x0 + 1, y0);
    out[2] = pc_raw_cfa_color(c, x0, y0 + 1);
    out[3] = pc_raw_cfa_color(c, x0 + 1, y0 + 1);
}

bool pc_raw_cfa_is_bayer(const pc_raw_cfa *c)
{
    uint8_t p[4];
    size_t x;
    size_t y;
    bool periodic = true;
    bool greens;
    if (c->width == 0 || c->height == 0 || c->width > 16 || c->height > 16) {
        return false;
    }
    pc_raw_cfa_phase(c, c->origin_x, c->origin_y, p);
    for (y = 0; y < c->height && periodic; y++) {
        for (x = 0; x < c->width && periodic; x++) {
            periodic = pc_raw_cfa_color(c, c->origin_x + x, c->origin_y + y) == p[(y % 2) * 2 + x % 2];
        }
    }
    greens = (p[0] == 1 && p[3] == 1 && p[1] != 1 && p[2] != 1 && p[1] + p[2] == 2) ||
             (p[1] == 1 && p[2] == 1 && p[0] != 1 && p[3] != 1 && p[0] + p[3] == 2);
    return periodic && greens;
}

/* ---- black levels ---- */

float pc_raw_black_at(const pc_raw_black *b, size_t ax, size_t ay, size_t s, size_t spp)
{
    size_t rows = b->rows > 0 ? b->rows : 1;
    size_t cols = b->cols > 0 ? b->cols : 1;
    size_t r = ay % rows;
    size_t c = ax % cols;
    size_t i = (r * cols + c) * spp + s;
    float base = i < b->nvalues ? b->values[i] : b->nvalues > 0 ? b->values[0] : 0.0f;
    float dh = ax < b->ndelta_h ? b->delta_h[ax] : 0.0f;
    float dv = ay < b->ndelta_v ? b->delta_v[ay] : 0.0f;
    return base + dh + dv;
}

bool pc_raw_black_is_uniform(const pc_raw_black *b)
{
    size_t i;
    for (i = 0; i < b->ndelta_h; i++) {
        if (b->delta_h[i] != 0.0f) {
            return false;
        }
    }
    for (i = 0; i < b->ndelta_v; i++) {
        if (b->delta_v[i] != 0.0f) {
            return false;
        }
    }
    for (i = 1; i < b->nvalues; i++) {
        if (!(b->values[i - 1] == b->values[i])) {
            return false;
        }
    }
    return true;
}

void rx_black_fini(pc_raw_black *b)
{
    free(b->values);
    free(b->delta_h);
    free(b->delta_v);
    memset(b, 0, sizeof *b);
}

pc_status rx_black_uniform(pc_raw_black *b, float v, pc_error *err)
{
    memset(b, 0, sizeof *b);
    b->values = malloc(sizeof *b->values);
    if (b->values == NULL) {
        return rx_nomem(err);
    }
    b->values[0] = v;
    b->nvalues = 1;
    b->rows = 1;
    b->cols = 1;
    return PC_OK;
}

pc_status rx_black_2x2(pc_raw_black *b, const float v[4], pc_error *err)
{
    memset(b, 0, sizeof *b);
    b->values = malloc(4 * sizeof *b->values);
    if (b->values == NULL) {
        return rx_nomem(err);
    }
    memcpy(b->values, v, 4 * sizeof *v);
    b->nvalues = 4;
    b->rows = 2;
    b->cols = 2;
    return PC_OK;
}

/* ---- strings ---- */

void pc_raw_strings_fini(pc_raw_strings *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        free(s->v[i]);
    }
    free(s->v);
    s->v = NULL;
    s->n = 0;
}

bool pc_raw_strings_any(const pc_raw_strings *s, const char *needle)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        if (strstr(s->v[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static pc_status push_owned(pc_raw_strings *w, char *s, pc_error *err)
{
    char **v = realloc(w->v, (w->n + 1) * sizeof *v);
    if (v == NULL) {
        free(s);
        return rx_nomem(err);
    }
    w->v = v;
    w->v[w->n++] = s;
    return PC_OK;
}

pc_status rx_warn(pc_raw_strings *w, pc_error *err, const char *fmt, ...)
{
    va_list ap;
    va_list ap2;
    int n;
    char *s;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || (s = malloc((size_t)n + 1)) == NULL) {
        va_end(ap2);
        return rx_nomem(err);
    }
    (void)vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return push_owned(w, s, err);
}

void rx_strings_remove_containing(pc_raw_strings *w, const char *needle)
{
    size_t i;
    size_t k = 0;
    for (i = 0; i < w->n; i++) {
        if (strstr(w->v[i], needle) != NULL) {
            free(w->v[i]);
        } else {
            w->v[k++] = w->v[i];
        }
    }
    w->n = k;
}

pc_status rx_strings_clone(const pc_raw_strings *src, pc_raw_strings *dst, pc_error *err)
{
    size_t i;
    pc_raw_strings d = {NULL, 0};
    for (i = 0; i < src->n; i++) {
        char *s = pc_strndup(src->v[i], strlen(src->v[i]));
        if (s == NULL || push_owned(&d, s, err) != PC_OK) {
            if (s == NULL) {
                (void)rx_nomem(err);
            }
            pc_raw_strings_fini(&d);
            return PC_ERR_NOMEM;
        }
    }
    *dst = d;
    return PC_OK;
}

/* ---- sensor ---- */

void pc_raw_sensor_init(pc_raw_sensor *s)
{
    memset(s, 0, sizeof *s);
    s->orientation = 1;
}

void rx_gain_map_fini(pc_raw_gain_map *m)
{
    free(m->gains);
    m->gains = NULL;
    m->ngains = 0;
}

void pc_raw_sensor_fini(pc_raw_sensor *s)
{
    size_t i;
    if (s == NULL) {
        return;
    }
    free(s->make);
    free(s->model);
    free(s->data);
    free(s->linearization);
    rx_black_fini(&s->black);
    for (i = 0; i < s->ngain_maps; i++) {
        rx_gain_map_fini(&s->gain_maps[i]);
    }
    free(s->gain_maps);
    free(s->tone_curve);
    pc_raw_strings_fini(&s->warnings);
    pc_raw_sensor_init(s);
}

/* ---- strips and tiles ---- */

/* The values of a list tag, malloc'd (n = 0 and NULL when absent or unreadable). */
static pc_status tag_list(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, uint32_t **out, size_t *n,
                          pc_error *err)
{
    rx_entry e;
    size_t k;
    size_t i;
    *out = NULL;
    *n = 0;
    if (!rx_ifd_get(t, ifd, tag, &e) || (k = rx_uints_n(t, &e)) == 0) {
        return PC_OK;
    }
    *out = pc_malloc_array(k, sizeof **out);
    if (*out == NULL) {
        return rx_nomem(err);
    }
    for (i = 0; i < k; i++) {
        (void)rx_uint(t, &e, i, &(*out)[i]);
    }
    *n = k;
    return PC_OK;
}

pc_status rx_segments(const rx_tiff *t, const rx_ifd *ifd, size_t width, size_t height, rx_segment **out,
                      size_t *nout, pc_error *err)
{
    uint32_t tw32;
    uint32_t th32;
    uint32_t *offs = NULL;
    uint32_t *counts = NULL;
    size_t noffs;
    size_t ncounts;
    rx_segment *v = NULL;
    size_t n;
    size_t i;
    pc_status st;
    if (rx_tag_uint(t, ifd, RX_TILE_WIDTH, &tw32) && rx_tag_uint(t, ifd, RX_TILE_LENGTH, &th32)) {
        size_t tw = tw32;
        size_t th = th32;
        size_t across;
        size_t down;
        if (tw == 0 || th == 0 || tw > (1u << 16) || th > (1u << 16)) {
            return rx_malformed(err, "bad tile size");
        }
        across = width / tw + (width % tw != 0);
        down = height / th + (height % th != 0);
        if (down != 0 && across > SIZE_MAX / down) {
            return rx_malformed(err, "too many tiles");
        }
        n = across * down;
        if ((st = tag_list(t, ifd, RX_TILE_OFFSETS, &offs, &noffs, err)) != PC_OK ||
            (st = tag_list(t, ifd, RX_TILE_BYTE_COUNTS, &counts, &ncounts, err)) != PC_OK) {
            free(offs);
            return st;
        }
        if (noffs < n || ncounts < n) {
            free(offs);
            free(counts);
            return rx_malformed(err, "missing tile offsets");
        }
        v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
        if (v == NULL) {
            free(offs);
            free(counts);
            return rx_nomem(err);
        }
        for (i = 0; i < n; i++) {
            v[i].x = (i % across) * tw;
            v[i].y = (i / across) * th;
            v[i].w = tw;
            v[i].h = th;
            v[i].offset = offs[i];
            v[i].len = counts[i];
        }
    } else {
        uint32_t r;
        size_t rps;
        if (rx_tag_uint(t, ifd, RX_ROWS_PER_STRIP, &r) && r > 0) {
            rps = r < height ? r : height;
        } else {
            rps = height;
        }
        n = rps > 0 ? height / rps + (height % rps != 0) : height;
        if ((st = tag_list(t, ifd, RX_STRIP_OFFSETS, &offs, &noffs, err)) != PC_OK ||
            (st = tag_list(t, ifd, RX_STRIP_BYTE_COUNTS, &counts, &ncounts, err)) != PC_OK) {
            free(offs);
            return st;
        }
        if (noffs < n) {
            free(offs);
            free(counts);
            return rx_malformed(err, "missing strip offsets");
        }
        if (ncounts < n) {
            if (n == 1) {
                size_t left = t->len > offs[0] ? t->len - offs[0] : 0;
                free(counts);
                counts = malloc(sizeof *counts);
                if (counts == NULL) {
                    free(offs);
                    return rx_nomem(err);
                }
                counts[0] = (uint32_t)left;
                ncounts = 1;
            } else {
                free(offs);
                free(counts);
                return rx_malformed(err, "missing strip byte counts");
            }
        }
        v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
        if (v == NULL) {
            free(offs);
            free(counts);
            return rx_nomem(err);
        }
        for (i = 0; i < n; i++) {
            v[i].x = 0;
            v[i].y = i * rps;
            v[i].w = width;
            v[i].h = rps;
            v[i].offset = offs[i];
            v[i].len = counts[i];
        }
    }
    free(offs);
    free(counts);
    *out = v;
    *nout = n;
    return PC_OK;
}

pc_status rx_unpack(const uint8_t *src, size_t srclen, size_t w, size_t rows, size_t samples, uint32_t bits, bool le,
                    uint16_t *out, pc_error *err)
{
    size_t n = w * rows * samples;
    size_t i;
    size_t row_bytes;
    size_t r;
    size_t k = 0;
    if (bits == 16 || (bits > 8 && srclen / 2 >= n)) {
        if (srclen / 2 < n) {
            return rx_malformed(err, "uncompressed raw data is truncated");
        }
        for (i = 0; i < n; i++) {
            const uint8_t *c = src + 2 * i;
            out[i] = (uint16_t)(le ? (unsigned)(c[0] | c[1] << 8) : (unsigned)(c[0] << 8 | c[1]));
        }
        return PC_OK;
    }
    if (bits == 8) {
        if (srclen < n) {
            return rx_malformed(err, "uncompressed raw data is truncated");
        }
        for (i = 0; i < n; i++) {
            out[i] = src[i];
        }
        return PC_OK;
    }
    row_bytes = (w * samples * bits + 7) / 8;
    if (rows != 0 && row_bytes > srclen / rows) {
        return rx_malformed(err, "uncompressed raw data is truncated");
    }
    for (r = 0; r < rows; r++) {
        const uint8_t *row = src + r * row_bytes;
        size_t at = 0;
        uint32_t acc = 0;
        uint32_t have = 0;
        for (i = 0; i < w * samples; i++) {
            while (have < bits) {
                acc = (acc << 8) | (at < row_bytes ? row[at] : 0u);
                at++;
                have += 8;
            }
            have -= bits;
            out[k++] = (uint16_t)((acc >> have) & ((1u << bits) - 1));
        }
    }
    return PC_OK;
}

typedef struct plane_ctx {
    const rx_tiff *t;
    const rx_segment *segs;
    size_t height;
    size_t samples;
    uint32_t bits;
    uint32_t compression;
    rx_jpeg_layout layout;
    uint16_t **out;
    pc_error *errs;
} plane_ctx;

static pc_status decode_segment(void *ctx, size_t chunk, size_t begin, size_t end)
{
    plane_ctx *c = ctx;
    const rx_segment *s = &c->segs[begin];
    size_t rows = s->h < c->height - s->y ? s->h : c->height - s->y;
    pc_error *err = &c->errs[chunk];
    const uint8_t *src = rx_bytes(c->t, s->offset, s->len);
    (void)end;
    if (src == NULL) {
        return rx_malformed(err, "raw data lies outside the file");
    }
    if (c->compression == 1) {
        size_t n = s->w * rows * c->samples;
        uint16_t *v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
        pc_status st;
        if (v == NULL) {
            return rx_nomem(err);
        }
        if ((st = rx_unpack(src, s->len, s->w, rows, c->samples, c->bits, c->t->le, v, err)) != PC_OK) {
            free(v);
            return st;
        }
        c->out[begin] = v;
        return PC_OK;
    } else {
        size_t need = s->w * rows * c->samples;
        size_t max = s->w;
        rx_frame f;
        uint16_t *v;
        size_t nv;
        pc_status st;
        size_t k;
        for (k = 0; k < 3; k++) {
            size_t m = k == 0 ? s->h : k == 1 ? c->samples : 4;
            max = max != 0 && m > SIZE_MAX / max ? SIZE_MAX : max * m;
        }
        if ((st = rx_ljpeg_decode(src, s->len, max, &f, &v, &nv, err)) != PC_OK) {
            return st;
        }
        if (nv < need) {
            free(v);
            return rx_malformed(err, "lossless JPEG tile is smaller than its tile");
        }
        if (c->layout == RX_QUADS && c->samples == 1 && f.components == 4 && f.width * 2 == s->w &&
            f.height * 2 == s->h) {
            uint16_t *tile = pc_calloc_array(s->w * s->h, sizeof *tile);
            size_t i;
            if (tile == NULL) {
                free(v);
                return rx_nomem(err);
            }
            for (i = 0; i < nv / 4; i++) {
                size_t at = 2 * (i / f.width) * s->w + 2 * (i % f.width);
                tile[at] = v[4 * i];
                tile[at + 1] = v[4 * i + 1];
                tile[at + s->w] = v[4 * i + 2];
                tile[at + s->w + 1] = v[4 * i + 3];
            }
            free(v);
            v = tile;
        }
        c->out[begin] = v;
        return PC_OK;
    }
}

pc_status rx_read_plane(const rx_tiff *t, const rx_ifd *ifd, const pc_raw_limits *limits, rx_jpeg_layout layout,
                        rx_plane *out, pc_error *err)
{
    uint32_t v;
    size_t width;
    size_t height;
    size_t samples;
    uint32_t bits;
    uint32_t compression;
    rx_segment *segs = NULL;
    size_t nseg = 0;
    plane_ctx c;
    uint16_t *data = NULL;
    pc_status st;
    size_t i;
    if (!rx_tag_uint(t, ifd, RX_IMAGE_WIDTH, &v)) {
        return rx_malformed(err, "raw image has no width");
    }
    width = v;
    if (!rx_tag_uint(t, ifd, RX_IMAGE_LENGTH, &v)) {
        return rx_malformed(err, "raw image has no height");
    }
    height = v;
    samples = rx_tag_uint(t, ifd, RX_SAMPLES_PER_PIXEL, &v) ? v : 1;
    bits = rx_tag_uint(t, ifd, RX_BITS_PER_SAMPLE, &v) ? v : 1;
    compression = rx_tag_uint(t, ifd, RX_COMPRESSION, &v) ? v : 1;
    if (samples < 1 || samples > 4) {
        return rx_unsupported(err, "%zu samples per pixel", samples);
    }
    if (rx_tag_uint(t, ifd, RX_SAMPLE_FORMAT, &v) && v == 3) {
        return rx_unsupported(err, "floating-point raw data");
    }
    if (bits < 1 || bits > 16) {
        return rx_unsupported(err, "%u-bit raw samples", (unsigned)bits);
    }
    if (samples > 1 && rx_tag_uint(t, ifd, RX_PLANAR_CONFIGURATION, &v) && v == 2) {
        return rx_unsupported(err, "planar raw data");
    }
    switch (compression) {
    case 1:
    case 7:
        break;
    case 8:
        return rx_unsupported(err, "Deflate-compressed (floating-point) DNG");
    case 34892:
        return rx_unsupported(err, "lossy-compressed DNG");
    case 52546:
        return rx_unsupported(err, "JPEG XL-compressed DNG");
    case 34713:
        return rx_unsupported(err, "Nikon compressed NEF");
    case 32767:
        return rx_unsupported(err, "Sony compressed ARW");
    case 65535:
        return rx_unsupported(err, "Pentax compressed PEF");
    default:
        return rx_unsupported(err, "raw compression scheme %u", (unsigned)compression);
    }
    if ((st = pc_raw_limits_check(limits, width, height, (uint64_t)samples * 2, err)) != PC_OK ||
        (st = rx_segments(t, ifd, width, height, &segs, &nseg, err)) != PC_OK) {
        return st;
    }
    c.t = t;
    c.segs = segs;
    c.height = height;
    c.samples = samples;
    c.bits = bits;
    c.compression = compression;
    c.layout = layout;
    c.out = pc_calloc_array(nseg > 0 ? nseg : 1, sizeof *c.out);
    c.errs = pc_calloc_array(nseg > 0 ? nseg : 1, sizeof *c.errs);
    if (c.out == NULL || c.errs == NULL) {
        st = rx_nomem(err);
        goto done;
    }
    st = pc_pool_for(pc_pool_global(), nseg, 1, decode_segment, &c);
    if (st != PC_OK) {
        for (i = 0; i < nseg; i++) {
            if (c.errs[i].code != PC_OK) {
                if (err != NULL) {
                    *err = c.errs[i];
                }
                st = c.errs[i].code;
                break;
            }
        }
        goto done;
    }
    data = pc_calloc_array(width * height * samples, sizeof *data);
    if (data == NULL) {
        st = rx_nomem(err);
        goto done;
    }
    for (i = 0; i < nseg; i++) {
        const rx_segment *s = &segs[i];
        size_t rows = s->h < height - s->y ? s->h : height - s->y;
        size_t cols = s->w < width - s->x ? s->w : width - s->x;
        size_t r;
        for (r = 0; r < rows; r++) {
            memcpy(data + ((s->y + r) * width + s->x) * samples, c.out[i] + r * s->w * samples,
                   cols * samples * sizeof *data);
        }
    }
    out->width = width;
    out->height = height;
    out->samples = samples;
    out->bits = bits;
    out->data = data;
    data = NULL;
done:
    if (c.out != NULL) {
        for (i = 0; i < nseg; i++) {
            free(c.out[i]);
        }
    }
    free(c.out);
    free(c.errs);
    free(segs);
    free(data);
    return st;
}
