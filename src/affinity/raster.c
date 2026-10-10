/* Pixel layers (Rstr) and placed images (ImgN), a port of crates/affinity/src/raster.rs. A placed
 * image keeps its original file (DyBm.Bckg -> archive entry c/N), imported as is; other pixels
 * are planar tiles of 256 bytes x 256 rows per channel, each its own archive entry. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "affinity/internal.h"
#include "base/mem.h"
#include "base/num.h"

#define MAX_PIXELS ((uint64_t)64 << 20)
#define TILE 256u
#define TILE_BYTES (TILE * TILE)

/* A small name -> tile cache shared by the channels of one bitmap. */
typedef struct tile_cache {
    char **names;
    uint8_t **data;
    size_t n;
} tile_cache;

static void cache_fini(tile_cache *c)
{
    size_t i;
    for (i = 0; i < c->n; i++) {
        free(c->names[i]);
        free(c->data[i]);
    }
    free(c->names);
    free(c->data);
    memset(c, 0, sizeof *c);
}

static const uint8_t *cache_get(const tile_cache *c, const char *name)
{
    size_t i;
    for (i = 0; i < c->n; i++) {
        if (strcmp(c->names[i], name) == 0) {
            return c->data[i];
        }
    }
    return NULL;
}

static uint32_t dim(const pc_af_stream *s, size_t b, const char *tag)
{
    int64_t v;
    return ax_int(s, b, tag, &v) && v >= 0 && v <= (int64_t)UINT32_MAX ? (uint32_t)v : 0;
}

/* The original image file: entry c/N is a stream whose root (Blck) holds it as Data. */
static pc_status original(ax_reader *r, const char *name, uint8_t **out, size_t *n, pc_error *err)
{
    uint8_t *bytes;
    size_t nb;
    pc_af_stream s;
    const pc_af_value *v;
    pc_status st;
    if ((st = pc_af_archive_read(r->archive, name, &bytes, &nb, err)) != PC_OK) {
        return st;
    }
    st = pc_af_stream_parse(bytes, nb, &s, err);
    free(bytes);
    if (st != PC_OK) {
        return st;
    }
    v = s.root < s.nobjects ? pc_af_object_get(&s.objects[s.root], PC_AF_TAG("Data")) : NULL;
    if (s.root >= s.nobjects) {
        pc_af_stream_fini(&s);
        return ax_malformed(err, "embedded image");
    }
    if (v == NULL || v->kind != PC_AF_BLOB || v->as.bytes.n == 0) {
        pc_af_stream_fini(&s);
        return ax_malformed(err, "embedded image without data");
    }
    *out = malloc(v->as.bytes.n);
    if (*out == NULL) {
        pc_af_stream_fini(&s);
        return ax_nomem(err);
    }
    memcpy(*out, v->as.bytes.v, v->as.bytes.n);
    *n = v->as.bytes.n;
    pc_af_stream_fini(&s);
    return PC_OK;
}

typedef struct crop {
    size_t bx0, py0, bx1, py1;
} crop;

/* Copies the overlap of a tile at byte column ox, row oy into the cropped plane. */
static void copy_tile(uint8_t *out, size_t outn, size_t row, crop c, size_t ox, size_t oy, const uint8_t *tile,
                      int fill)
{
    size_t y0 = oy > c.py0 ? oy : c.py0;
    size_t y1 = oy + TILE < c.py1 ? oy + TILE : c.py1;
    size_t x0 = ox > c.bx0 ? ox : c.bx0;
    size_t x1 = ox + TILE < c.bx1 ? ox + TILE : c.bx1;
    size_t y;
    for (y = y0; y < y1; y++) {
        size_t x;
        for (x = x0; x < x1; x++) {
            size_t at = (y - c.py0) * row + (x - c.bx0);
            if (at < outn) {
                out[at] = fill >= 0 ? (uint8_t)fill : tile[(y - oy) * TILE + (x - ox)];
            }
        }
    }
}

static void tagc(char out[5], const char *prefix, size_t c)
{
    out[0] = prefix[0];
    out[1] = prefix[1];
    out[2] = prefix[2];
    out[3] = (char)('0' + c);
    out[4] = '\0';
}

/* One channel of level 0, cropped to (x0, y0, w, h) pixels; bps bytes per sample. */
static pc_status plane(ax_reader *r, size_t bitmap, size_t c, size_t bps, uint32_t x0, uint32_t y0, uint32_t w,
                       uint32_t h, tile_cache *cache, uint8_t **out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    size_t width;
    size_t height;
    int64_t v;
    char tg[5];
    size_t tw;
    size_t th;
    const pc_af_value *states;
    const pc_af_value *tiles;
    size_t row = (size_t)w * bps;
    uint8_t *o;
    size_t on = row * h;
    crop cr;
    size_t t;
    size_t limit;
    size_t ti = 0;
    width = ax_int(s, bitmap, "BmpW", &v) && v >= 0 ? (size_t)v : 0;
    height = ax_int(s, bitmap, "BmpH", &v) && v >= 0 ? (size_t)v : 0;
    tagc(tg, "TWi", c);
    tw = ax_int(s, bitmap, tg, &v) && v >= 0 ? (size_t)v : (width * bps + TILE - 1) / TILE;
    tagc(tg, "THi", c);
    th = ax_int(s, bitmap, tg, &v) && v >= 0 ? (size_t)v : (height + TILE - 1) / TILE;
    tagc(tg, "Sta", c);
    states = ax_field(s, bitmap, tg);
    if (states != NULL && states->kind != PC_AF_ARRAY) {
        states = NULL;
    }
    tagc(tg, "Idx", c);
    tiles = ax_objs(s, bitmap, tg);
    o = calloc(on > 0 ? on : 1, 1);
    if (o == NULL) {
        return ax_nomem(err);
    }
    cr.bx0 = (size_t)x0 * bps;
    cr.bx1 = ((size_t)x0 + w) * bps;
    cr.py0 = y0;
    cr.py1 = (size_t)y0 + h;
    limit = tw != 0 && th > SIZE_MAX / tw ? SIZE_MAX : tw * th;
    for (t = 0; states != NULL && t < states->as.arr.n && t < limit; t++) {
        const pc_af_value *sv = &states->as.arr.v[t];
        uint8_t state = sv->kind == PC_AF_UINT ? (sv->as.u <= 255 ? (uint8_t)sv->as.u : 255) : 255;
        size_t twm = tw > 0 ? tw : 1;
        size_t ty = t / twm;
        size_t tx = t % twm;
        int fill = -1;
        if (state == 0 || state == 1) {
            continue;
        }
        if (state == 2) {
            fill = 0xFF;
        } else if (state == 4) {
            size_t blck;
            size_t ox = tx * TILE;
            size_t oy = ty * TILE;
            const char *name;
            const uint8_t *tile;
            if (!ax_next_obj(tiles, &ti, &blck)) {
                free(o);
                return ax_malformed(err, "missing pixel tile");
            }
            if (ox >= cr.bx1 || ox + TILE <= cr.bx0 || oy >= cr.py1 || oy + TILE <= cr.py0) {
                continue;
            }
            name = ax_entry(s, blck, "Data");
            if (name == NULL) {
                free(o);
                return ax_malformed(err, "pixel tile without data");
            }
            tile = cache_get(cache, name);
            if (tile == NULL) {
                uint8_t *data;
                size_t nd;
                pc_status st = pc_af_archive_read(r->archive, name, &data, &nd, err);
                char **nn;
                uint8_t **dd;
                char *copy;
                if (st != PC_OK) {
                    free(o);
                    return st;
                }
                /* Older files wrap a tile in a one-field stream: header, then a 64 KiB blob. */
                if (nd == TILE_BYTES + 22 && memcmp(data, "\x00\xffKS", 4) == 0) {
                    memmove(data, data + 21, TILE_BYTES);
                    nd = TILE_BYTES;
                }
                if (nd != TILE_BYTES) {
                    free(data);
                    free(o);
                    return ax_malformed(err, "pixel tile size");
                }
                nn = realloc(cache->names, (cache->n + 1) * sizeof *nn);
                if (nn != NULL) {
                    cache->names = nn;
                }
                dd = realloc(cache->data, (cache->n + 1) * sizeof *dd);
                if (dd != NULL) {
                    cache->data = dd;
                }
                copy = pc_strndup(name, strlen(name));
                if (nn == NULL || dd == NULL || copy == NULL) {
                    free(copy);
                    free(data);
                    free(o);
                    return ax_nomem(err);
                }
                cache->names[cache->n] = copy;
                cache->data[cache->n] = data;
                cache->n++;
                tile = data;
            }
            copy_tile(o, on, row, cr, ox, oy, tile, -1);
            continue;
        } else if (state == 3) {
            ax_warn(r, "32-bit pixel layers");
            continue;
        } else if (state == 5) {
            ax_warn(r, "pixel layers drawn from an embedded image (left empty)");
            continue;
        } else {
            free(o);
            return ax_malformed(err, "unknown pixel tile state");
        }
        copy_tile(o, on, row, cr, tx * TILE, ty * TILE, NULL, fill);
    }
    *out = o;
    return PC_OK;
}

/* Decodes the tiles of a bitmap inside the crop, placed by world (bitmap pixels to document). */
static pc_status decode(ax_reader *r, size_t bitmap, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                        pc_af_affine world, bool *has, pc_af_image *out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    uint32_t w;
    uint32_t h;
    uint16_t fmt;
    uint16_t ver;
    size_t channels;
    size_t bps;
    uint8_t *planes[5] = {NULL, NULL, NULL, NULL, NULL};
    tile_cache cache = {NULL, NULL, 0};
    size_t n;
    uint8_t *rgba;
    size_t c;
    size_t i;
    pc_af_affine place = {{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}};
    pc_status st;
    *has = false;
    if (x1 <= x0 || y1 <= y0) {
        return PC_OK;
    }
    w = x1 - x0;
    h = y1 - y0;
    if ((uint64_t)w * h > MAX_PIXELS) {
        ax_warn(r, "pixel layers larger than 64 megapixels");
        return PC_OK;
    }
    if (!ax_enum(s, bitmap, "Frmt", &fmt, &ver)) {
        fmt = 0xFFFF;
    }
    switch (fmt) {
    case 0:
        channels = 4;
        bps = 1;
        break;
    case 1:
        channels = 4;
        bps = 2;
        break;
    case 6:
        channels = 1;
        bps = 1;
        break;
    case 7:
        channels = 1;
        bps = 2;
        break;
    case 4:
        ax_warn(r, "CMYK pixel layers (converted to RGB without the document's colour profile)");
        channels = 5;
        bps = 1;
        break;
    default:
        ax_warn(r, "pixel layers in grey, Lab or 32-bit formats");
        return PC_OK;
    }
    for (c = 0; c < channels; c++) {
        if ((st = plane(r, bitmap, c + 1, bps, x0, y0, w, h, &cache, &planes[c], err)) != PC_OK) {
            goto fail;
        }
    }
    n = (size_t)w * h;
    rgba = calloc(n * 4 > 0 ? n * 4 : 1, 1);
    if (rgba == NULL) {
        st = ax_nomem(err);
        goto fail;
    }
    for (i = 0; i < n; i++) {
        double v[5];
        double px[4];
        int k;
        for (c = 0; c < channels; c++) {
            if (bps == 2) {
                v[c] = (double)(uint16_t)(planes[c][2 * i] | planes[c][2 * i + 1] << 8) / 65535.0;
            } else {
                v[c] = (double)planes[c][i] / 255.0;
            }
        }
        if (channels == 1) {
            px[0] = px[1] = px[2] = v[0];
            px[3] = 1.0;
        } else if (channels == 5) {
            double kk = 1.0 - v[3];
            px[0] = (1.0 - v[0]) * kk;
            px[1] = (1.0 - v[1]) * kk;
            px[2] = (1.0 - v[2]) * kk;
            px[3] = v[4];
        } else {
            px[0] = v[0];
            px[1] = v[1];
            px[2] = v[2];
            px[3] = v[3];
        }
        for (k = 0; k < 4; k++) {
            rgba[4 * i + (size_t)k] = pc_f64_to_u8(round(pc_clampd(px[k], 0.0, 1.0) * 255.0));
        }
    }
    for (c = 0; c < channels; c++) {
        free(planes[c]);
    }
    cache_fini(&cache);
    place.m[4] = (double)x0;
    place.m[5] = (double)y0;
    out->width = w;
    out->height = h;
    out->format = PC_AF_RGBA8;
    out->pixels = rgba;
    out->npixels = n * 4;
    out->transform = ax_affine_then(place, world);
    *has = true;
    return PC_OK;
fail:
    for (c = 0; c < 5; c++) {
        free(planes[c]);
    }
    cache_fini(&cache);
    return st;
}

pc_status ax_node_image(ax_reader *r, size_t id, pc_af_affine world, bool *has, pc_af_image *out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    size_t bitmap;
    uint32_t width;
    uint32_t height;
    bool has_tiles;
    const char *name;
    const pc_af_value *bitr;
    uint32_t c[4];
    *has = false;
    memset(out, 0, sizeof *out);
    if (!ax_obj(s, id, "Bitm", &bitmap)) {
        ax_warn(r, "pixel layers without pixels");
        return PC_OK;
    }
    if (!ax_is(s, bitmap, "DyBm")) {
        ax_warn(r, "pixel layers stored in an unknown way");
        return PC_OK;
    }
    width = dim(s, bitmap, "BmpW");
    height = dim(s, bitmap, "BmpH");
    if (width == 0 || height == 0) {
        return PC_OK;
    }
    /* A placed image: the original file, byte for byte. */
    has_tiles = ax_field(s, bitmap, "Sta1") != NULL;
    name = ax_entry(s, bitmap, "Bckg");
    if (name != NULL && name[0] != '\0' && (ax_is(s, id, "ImgN") || !has_tiles)) {
        uint8_t *bytes;
        size_t nb;
        pc_error e;
        pc_status st = original(r, name, &bytes, &nb, &e);
        if (st == PC_OK) {
            out->width = width;
            out->height = height;
            out->format = PC_AF_ENCODED;
            out->pixels = bytes;
            out->npixels = nb;
            out->transform = world;
            *has = true;
            return PC_OK;
        }
        if (st == PC_ERR_LIMIT || st == PC_ERR_NOMEM) {
            if (err != NULL) {
                *err = e;
            }
            return st;
        }
        ax_warn(r, "an embedded image file that could not be read (its cached pixels were used)");
    }
    /* The content's bounding box, so a mostly empty canvas-sized layer stays small. */
    c[0] = 0;
    c[1] = 0;
    c[2] = width;
    c[3] = height;
    bitr = ax_field(s, id, "BitR");
    if (bitr != NULL && bitr->kind == PC_AF_INTS && bitr->as.ints.n == 4 &&
        bitr->as.ints.v[0] > (int64_t)INT32_MIN + 1) {
        int k;
        for (k = 0; k < 4; k++) {
            int64_t lim = k % 2 == 0 ? (int64_t)width : (int64_t)height;
            int64_t x = bitr->as.ints.v[k];
            c[k] = (uint32_t)(x < 0 ? 0 : x > lim ? lim : x);
        }
    }
    if (ax_field(s, bitmap, "Bckg") != NULL && has_tiles && !ax_is(s, id, "ImgN")) {
        ax_warn(r, "edited placed images use their stored pixels");
    }
    return decode(r, bitmap, c[0], c[1], c[2], c[3], world, has, out, err);
}

pc_status ax_mask(ax_reader *r, size_t id, pc_af_affine world, bool *has, pc_af_image *out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    size_t bitmap;
    uint32_t width;
    uint32_t height;
    uint16_t fmt;
    uint16_t ver;
    *has = false;
    memset(out, 0, sizeof *out);
    if (!ax_obj(s, id, "Bitm", &bitmap)) {
        return PC_OK;
    }
    width = dim(s, bitmap, "BmpW");
    height = dim(s, bitmap, "BmpH");
    if (width == 0 || height == 0 || !ax_enum(s, bitmap, "Frmt", &fmt, &ver) || (fmt != 6 && fmt != 7)) {
        ax_warn(r, "pixel masks in an unknown format (imported without them)");
        return PC_OK;
    }
    return decode(r, bitmap, 0, 0, width, height, world, has, out, err);
}

pc_status ax_embedded(ax_reader *r, size_t id, pc_af_affine world, bool *has, pc_af_image *out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    size_t e;
    size_t cache;
    double bb[4];
    uint32_t width;
    uint32_t height;
    double cx;
    double cy;
    pc_af_affine place;
    *has = false;
    memset(out, 0, sizeof *out);
    if (!ax_obj(s, id, "Bitm", &e) || !ax_obj(s, e, "Cach", &cache)) {
        return PC_OK;
    }
    if (!ax_floats(s, e, "ChBB", 4, bb)) {
        return PC_OK;
    }
    width = dim(s, cache, "BmpW");
    height = dim(s, cache, "BmpH");
    if (width == 0 || height == 0) {
        return PC_OK;
    }
    /* The instance's origin is the centre of the embedded content. */
    cx = (bb[0] + bb[2]) / 2.0;
    cy = (bb[1] + bb[3]) / 2.0;
    place.m[0] = (bb[2] - bb[0]) / (double)width;
    place.m[1] = 0.0;
    place.m[2] = 0.0;
    place.m[3] = (bb[3] - bb[1]) / (double)height;
    place.m[4] = bb[0] - cx;
    place.m[5] = bb[1] - cy;
    return decode(r, cache, 0, 0, width, height, ax_affine_then(place, world), has, out, err);
}
