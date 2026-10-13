/* PNG (port of crates/codecs/src/codecs/png.rs; REWRITE_PLAN.md D26): libpng decodes the pixels
 * (palette and low bit depths expanded, tRNS to alpha, 16-bit kept); iCCP, eXIf, pHYs, the
 * text chunks (with one shared UTF-8 budget) and APNG frame counts come from our own chunk walk.
 * Writing is own code: IHDR, ancillary chunks, IDAT (adaptive minimum-sum filter, deflated in
 * parallel bands for large images, or Adam7 with filter 0), IEND. */
#include <math.h>
#include <png.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "base/compress.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/utf8.h"
#include "codecs/internal.h"
#include "codecs/orientation.h"
#include "codecs/resolution.h"

#define F PC_CF_PNG
#define XMP_KEYWORD "XML:com.adobe.xmp"
#define METERS_PER_INCH 0.0254f

static bool host_little(void)
{
    const uint16_t x = 1;
    uint8_t b;
    memcpy(&b, &x, 1);
    return b == 1;
}

/* ---- chunk walk ---- */

typedef struct png_chunk_t {
    uint8_t type[4];
    const uint8_t *data;
    size_t len;
    bool crc_ok;
} png_chunk_t;

/* Next chunk at *at (tolerant: false at a broken tail). */
static bool next_chunk(const uint8_t *b, size_t n, size_t *at, png_chunk_t *c)
{
    uint32_t len;
    size_t end;
    if (n - *at < 8) {
        return false;
    }
    len = cx_be32(b + *at);
    if (len > 0x7FFFFFFFu) {
        return false;
    }
    if (!pc_size_add(*at + 8, len, &end) || end > n || n - end < 4) {
        return false;
    }
    memcpy(c->type, b + *at + 4, 4);
    c->data = b + *at + 8;
    c->len = len;
    c->crc_ok = pc_crc32(pc_crc32(0, c->type, 4), c->data, len) == cx_be32(b + end);
    *at = end + 4;
    return true;
}

static pc_status text_limit(pc_error *err) { return cx_limit(err, "PNG decoded text metadata exceeds max_alloc"); }

static bool latin1_len(const uint8_t *b, size_t n, size_t *out)
{
    size_t i;
    size_t hi = 0;
    for (i = 0; i < n; i++) {
        hi += b[i] >= 128;
    }
    return pc_size_add(n, hi, out);
}

static const uint8_t *nul(const uint8_t *b, size_t n) { return memchr(b, 0, n); }

/* Appends bytes (Latin-1 converted to UTF-8 when latin1), within limit. -1 = over the limit. */
static int append_text(pc_buf *o, const uint8_t *b, size_t n, bool latin1, size_t limit, pc_error *err)
{
    size_t added = n;
    size_t i;
    if (latin1 && !latin1_len(b, n, &added)) {
        return -1;
    }
    if (o->len > limit || added > limit - o->len) {
        return -1;
    }
    if (pc_buf_reserve(o, added, err) != PC_OK) {
        return -1;
    }
    if (!latin1) {
        (void)pc_buf_append(o, b, n, NULL);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (b[i] < 128) {
            (void)pc_buf_append_byte(o, b[i], NULL);
        } else {
            uint8_t u[2] = {(uint8_t)(0xC0 | (b[i] >> 6)), (uint8_t)(0x80 | (b[i] & 0x3F))};
            (void)pc_buf_append(o, u, 2, NULL);
        }
    }
    return 0;
}

/* The decoded text of one chunk: 1 ok, 0 skip (invalid, never partial), -1 over the limit. */
static int bounded_text(const uint8_t *in, size_t n, bool compressed, bool latin1, size_t limit, pc_buf *out,
                        pc_error *err)
{
    pc_buf o = PC_BUF_INIT;
    size_t i = 0;
    if (compressed) {
        z_stream z;
        uint8_t scratch[8192];
        int r;
        memset(&z, 0, sizeof z);
        if (inflateInit(&z) != Z_OK) {
            return 0;
        }
        z.next_in = (Bytef *)(uintptr_t)in;
        z.avail_in = (uInt)(n > UINT32_MAX ? UINT32_MAX : n);
        for (;;) {
            size_t got;
            z.next_out = scratch;
            z.avail_out = sizeof scratch;
            r = inflate(&z, Z_NO_FLUSH);
            got = sizeof scratch - z.avail_out;
            if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) {
                inflateEnd(&z);
                pc_buf_fini(&o);
                return 0;
            }
            if (got > 0 && append_text(&o, scratch, got, latin1, limit, err) != 0) {
                inflateEnd(&z);
                pc_buf_fini(&o);
                return -1;
            }
            if (r == Z_STREAM_END) {
                break;
            }
            if (got == 0 && (r == Z_BUF_ERROR || z.avail_in == 0)) {
                /* Truncated stream: never keep partial text. */
                inflateEnd(&z);
                pc_buf_fini(&o);
                return 0;
            }
        }
        inflateEnd(&z);
    } else {
        uint32_t cp;
        if (!latin1) {
            while (i < n) {
                if (!pc_utf8_next(in, n, &i, &cp)) {
                    return 0;
                }
            }
        }
        if (append_text(&o, in, n, latin1, limit, err) != 0) {
            pc_buf_fini(&o);
            return -1;
        }
    }
    /* Must be valid UTF-8 (iTXt after inflation). */
    i = 0;
    while (i < o.len) {
        uint32_t cp;
        if (!pc_utf8_next(o.data, o.len, &i, &cp)) {
            pc_buf_fini(&o);
            return 0;
        }
    }
    *out = o;
    return 1;
}

typedef struct kv {
    pc_buf key;
    pc_buf val;
} kv;

typedef struct kvlist {
    kv *v;
    size_t n;
    size_t cap;
} kvlist;

static void kvlist_fini(kvlist *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        pc_buf_fini(&l->v[i].key);
        pc_buf_fini(&l->v[i].val);
    }
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static pc_status kvlist_push(kvlist *l, pc_buf *k, pc_buf *v, pc_error *err)
{
    kv *g = pc_grow_array(l->v, &l->cap, l->n + 1, sizeof *g);
    if (g == NULL) {
        pc_buf_fini(k);
        pc_buf_fini(v);
        return cx_nomem(err);
    }
    l->v = g;
    l->v[l->n].key = *k;
    l->v[l->n].val = *v;
    l->n++;
    return PC_OK;
}

static pc_status add_all(pc_cmeta *m, kvlist *l, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < l->n && st == PC_OK; i++) {
        st = pc_cmeta_add_text(m, (const char *)l->v[i].key.data, l->v[i].key.len,
                               l->v[i].val.data != NULL ? (const char *)l->v[i].val.data : "", l->v[i].val.len, err);
    }
    return st;
}

/* Every CRC-valid text chunk (also after IDAT) with one shared budget for the decoded UTF-8
 * keywords and values (an overwritten XMP packet still counts). Invalid text is skipped. */
static pc_status read_text_metadata(const uint8_t *b, size_t n, size_t remaining, pc_cmeta *meta, pc_error *err)
{
    kvlist plain = {NULL, 0, 0};
    kvlist comp = {NULL, 0, 0};
    kvlist intl = {NULL, 0, 0};
    size_t at = 8;
    png_chunk_t c;
    pc_status st = PC_OK;
    while (st == PC_OK && next_chunk(b, n, &at, &c)) {
        const uint8_t *z;
        const uint8_t *key;
        size_t key_len;
        const uint8_t *val;
        size_t val_len;
        bool deflated = false;
        bool latin1 = true;
        size_t kl8;
        pc_buf text = PC_BUF_INIT;
        pc_buf k = PC_BUF_INIT;
        int r;
        int kind;
        if (memcmp(c.type, "IEND", 4) == 0) {
            break;
        }
        kind = memcmp(c.type, "tEXt", 4) == 0 ? 0 : memcmp(c.type, "zTXt", 4) == 0 ? 1
                                                 : memcmp(c.type, "iTXt", 4) == 0   ? 2
                                                                                    : -1;
        if (kind < 0 || !c.crc_ok) {
            continue;
        }
        z = nul(c.data, c.len);
        if (z == NULL) {
            continue;
        }
        key = c.data;
        key_len = (size_t)(z - c.data);
        val = z + 1;
        val_len = c.len - key_len - 1;
        if (key_len == 0 || key_len > 79) {
            continue;
        }
        if (kind == 1) {
            if (val_len < 1 || val[0] != 0) {
                continue;
            }
            val++;
            val_len--;
            deflated = true;
        } else if (kind == 2) {
            const uint8_t *z2;
            const uint8_t *z3;
            uint8_t flag;
            uint8_t method;
            size_t i;
            uint32_t cp;
            if (val_len < 2) {
                continue;
            }
            flag = val[0];
            method = val[1];
            if (flag > 1 || (flag == 1 && method != 0)) {
                continue;
            }
            val += 2;
            val_len -= 2;
            z2 = nul(val, val_len);
            if (z2 == NULL) {
                continue;
            }
            for (i = 0; i < (size_t)(z2 - val); i++) {
                if (val[i] >= 128) {
                    break;
                }
            }
            if (i < (size_t)(z2 - val)) {
                continue; /* language tag not ASCII */
            }
            val_len -= (size_t)(z2 - val) + 1;
            val = z2 + 1;
            z3 = nul(val, val_len);
            if (z3 == NULL) {
                continue;
            }
            i = 0;
            while (i < (size_t)(z3 - val)) {
                if (!pc_utf8_next(val, (size_t)(z3 - val), &i, &cp)) {
                    break;
                }
            }
            if (i < (size_t)(z3 - val)) {
                continue; /* translated keyword not UTF-8 */
            }
            val_len -= (size_t)(z3 - val) + 1;
            val = z3 + 1;
            deflated = flag == 1;
            latin1 = false;
        }
        if (!latin1_len(key, key_len, &kl8)) {
            st = text_limit(err);
            break;
        }
        r = bounded_text(val, val_len, deflated, latin1, remaining > kl8 ? remaining - kl8 : 0, &text, err);
        if (r < 0) {
            st = text_limit(err);
            break;
        }
        if (r == 0) {
            continue;
        }
        if (remaining < kl8 || remaining - kl8 < text.len) {
            pc_buf_fini(&text);
            st = text_limit(err);
            break;
        }
        remaining -= kl8 + text.len;
        if (append_text(&k, key, key_len, true, SIZE_MAX, err) != 0) {
            pc_buf_fini(&text);
            st = cx_nomem(err);
            break;
        }
        /* Some writers put XMP in Latin-1 text chunks instead of the usual iTXt. */
        if (key_len == sizeof XMP_KEYWORD - 1 && memcmp(key, XMP_KEYWORD, key_len) == 0) {
            pc_buf_fini(&k);
            pc_meta_blob_fini(&meta->xmp);
            st = pc_meta_blob_set(&meta->xmp, text.data != NULL ? text.data : (const uint8_t *)"", text.len, err);
            pc_buf_fini(&text);
        } else {
            st = kvlist_push(kind == 0 ? &plain : kind == 1 ? &comp : &intl, &k, &text, err);
        }
    }
    /* The reference groups entries by chunk type. */
    if (st == PC_OK) {
        st = add_all(meta, &plain, err);
    }
    if (st == PC_OK) {
        st = add_all(meta, &comp, err);
    }
    if (st == PC_OK) {
        st = add_all(meta, &intl, err);
    }
    kvlist_fini(&plain);
    kvlist_fini(&comp);
    kvlist_fini(&intl);
    return st;
}

/* iCCP, eXIf, pHYs and the APNG image count, from CRC-valid chunks. */
static pc_status read_meta_chunks(const uint8_t *b, size_t n, size_t alloc, pc_cimage *img, uint64_t *images,
                                  pc_error *err)
{
    size_t at = 8;
    png_chunk_t c;
    bool seen_idat = false;
    bool actl = false;
    uint32_t frames = 0;
    bool fctl_before_idat = false;
    while (next_chunk(b, n, &at, &c)) {
        if (memcmp(c.type, "IEND", 4) == 0) {
            break;
        }
        if (memcmp(c.type, "IDAT", 4) == 0) {
            seen_idat = true;
            continue;
        }
        if (!c.crc_ok) {
            continue;
        }
        if (!seen_idat && memcmp(c.type, "acTL", 4) == 0 && c.len >= 8 && !actl) {
            actl = true;
            frames = cx_be32(c.data);
        } else if (!seen_idat && memcmp(c.type, "fcTL", 4) == 0) {
            fctl_before_idat = true;
        } else if (!seen_idat && memcmp(c.type, "iCCP", 4) == 0 && !img->icc.has) {
            const uint8_t *z = nul(c.data, c.len < 80 ? c.len : 80);
            pc_buf prof = PC_BUF_INIT;
            pc_decomp_info info;
            if (z != NULL && z > c.data && (size_t)(z - c.data) + 2 <= c.len && z[1] == 0) {
                size_t off = (size_t)(z - c.data) + 2;
                if (pc_inflate(PC_Z_ZLIB, c.data + off, c.len - off, alloc, &prof, &info, NULL) == PC_OK &&
                    info.stream_end) {
                    pc_status st = pc_meta_blob_set(&img->icc, prof.data != NULL ? prof.data : (const uint8_t *)"",
                                               prof.len, err);
                    pc_buf_fini(&prof);
                    if (st != PC_OK) {
                        return st;
                    }
                }
                pc_buf_fini(&prof);
            }
        } else if (memcmp(c.type, "eXIf", 4) == 0 && !img->meta.exif.has) {
            pc_status st = pc_meta_blob_set(&img->meta.exif, c.data, c.len, err);
            if (st != PC_OK) {
                return st;
            }
        } else if (!seen_idat && memcmp(c.type, "pHYs", 4) == 0 && c.len == 9) {
            uint32_t x = cx_be32(c.data);
            uint32_t y = cx_be32(c.data + 4);
            if (c.data[8] == 1 && x > 0 && y > 0) {
                img->meta.has_dpi = true;
                img->meta.dpi_x = (float)x * METERS_PER_INCH;
                img->meta.dpi_y = (float)y * METERS_PER_INCH;
            }
        }
    }
    *images = actl ? (uint64_t)frames + (fctl_before_idat ? 0 : 1) : 1;
    return PC_OK;
}

/* ---- libpng pixel decode ---- */

typedef struct png_src {
    const uint8_t *b;
    size_t n;
    size_t pos;
    char msg[160];
} png_src;

static void png_err(png_structp png, png_const_charp msg)
{
    png_src *s = png_get_error_ptr(png);
    if (s != NULL) {
        (void)snprintf(s->msg, sizeof s->msg, "%s", msg);
    }
    png_longjmp(png, 1);
}

static void png_warn(png_structp png, png_const_charp msg)
{
    (void)png;
    (void)msg;
}

static void png_read(png_structp png, png_bytep out, png_size_t len)
{
    png_src *s = png_get_io_ptr(png);
    if (s->n - s->pos < len) {
        png_error(png, "unexpected end of file");
    }
    memcpy(out, s->b + s->pos, len);
    s->pos += len;
}

static const png_byte IGNORED[] = "iCCP\0eXIf\0tEXt\0zTXt\0iTXt\0pHYs\0sPLT\0gAMA\0cHRM\0sRGB\0bKGD\0hIST\0"
                                  "tIME\0sBIT\0oFFs\0pCAL\0sCAL\0acTL\0fcTL\0fdAT\0";

typedef struct png_job {
    png_src src;
    uint8_t *buf;
    size_t len;
    png_bytep *rows;
    uint32_t w;
    uint32_t h;
    pc_clayout layout;
    pc_csample sample;
    pc_status st;
    bool limit;
} png_job;

/* Runs libpng; all state that crosses setjmp lives in *j. */
static void run_libpng(png_job *j)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &j->src, png_err, png_warn);
    png_infop info = NULL;
    if (png == NULL) {
        j->st = PC_ERR_NOMEM;
        return;
    }
    info = png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_read_struct(&png, NULL, NULL);
        j->st = PC_ERR_NOMEM;
        return;
    }
    if (setjmp(png_jmpbuf(png))) {
        if (j->st == PC_ERR_CANCELLED) {
            j->st = PC_OK; /* the pixels were complete; a damaged tail is tolerated */
        } else if (j->st == PC_OK) {
            j->st = PC_ERR_FORMAT;
        }
        png_destroy_read_struct(&png, &info, NULL);
        return;
    }
    png_set_read_fn(png, &j->src, png_read);
    png_set_user_limits(png, 0x7FFFFFFF, 0x7FFFFFFF);
    png_set_keep_unknown_chunks(png, PNG_HANDLE_CHUNK_NEVER, NULL, 0);
    png_set_keep_unknown_chunks(png, PNG_HANDLE_CHUNK_NEVER, IGNORED, (int)((sizeof IGNORED - 1) / 5));
    png_set_benign_errors(png, 1);
    png_read_info(png, info);
    {
        int depth = png_get_bit_depth(png, info);
        size_t row;
        size_t y;
        int ch;
        png_set_expand(png);
        if (depth == 16 && host_little()) {
            png_set_swap(png);
        }
        (void)png_set_interlace_handling(png);
        png_read_update_info(png, info);
        j->w = png_get_image_width(png, info);
        j->h = png_get_image_height(png, info);
        ch = png_get_channels(png, info);
        depth = png_get_bit_depth(png, info);
        j->layout = ch == 1 ? PC_CL_GRAY : ch == 2 ? PC_CL_GRAYA : ch == 3 ? PC_CL_RGB : PC_CL_RGBA;
        j->sample = depth == 16 ? PC_CS_U16 : PC_CS_U8;
        if ((depth != 8 && depth != 16) || ch < 1 || ch > 4) {
            png_error(png, "unexpected output format");
        }
        row = png_get_rowbytes(png, info);
        if (row != (size_t)j->w * (size_t)ch * (size_t)(depth / 8) || !pc_size_mul(row, j->h, &j->len)) {
            png_error(png, "image too large");
        }
        j->buf = malloc(j->len > 0 ? j->len : 1);
        j->rows = pc_malloc_array(j->h > 0 ? j->h : 1, sizeof *j->rows);
        if (j->buf == NULL || j->rows == NULL) {
            j->limit = true;
            png_error(png, "image too large");
        }
        for (y = 0; y < j->h; y++) {
            j->rows[y] = j->buf + y * row;
        }
        png_read_image(png, j->rows);
        j->st = PC_ERR_CANCELLED; /* marks "pixels complete" for the error handler */
        png_read_end(png, NULL);
        j->st = PC_OK;
    }
    png_destroy_read_struct(&png, &info, NULL);
}

pc_status cx_png_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    png_job j;
    pc_cimage img;
    uint64_t images = 1;
    pc_status st;
    if (n < 8 || memcmp(b, "\x89PNG\r\n\x1a\n", 8) != 0) {
        return cx_malformed(err, F, "Invalid PNG signature.");
    }
    /* The declared size is checked before anything is allocated (worst case 4 x 16 bit). */
    if (n >= 24 && memcmp(b + 12, "IHDR", 4) == 0) {
        uint32_t w = cx_be32(b + 16);
        uint32_t h = cx_be32(b + 20);
        if ((st = cx_check_bytes(l, w, h, 8, err)) != PC_OK) {
            return st;
        }
        /* zlib never beats 1032:1: refuse a file too small for the declared image before the output
         * buffer exists, measured on the stored samples (before palette or bit expansion). */
        if (n >= 26) {
            static const uint8_t CHANNELS[7] = {1, 0, 3, 1, 2, 0, 4};
            uint8_t depth = b[24];
            uint8_t ct = b[25];
            uint64_t bits = ct < 7 ? (uint64_t)CHANNELS[ct] * depth : 0;
            uint64_t px = (uint64_t)w * h;
            uint64_t stored = bits != 0 && px > UINT64_MAX / bits ? UINT64_MAX : px * bits;
            stored = stored / 8 + (stored % 8 != 0);
            if (bits != 0 && (st = cx_check_plausible(stored, n, 1100, err)) != PC_OK) {
                return st;
            }
        }
    }
    memset(&j, 0, sizeof j);
    j.src.b = b;
    j.src.n = n;
    run_libpng(&j);
    free(j.rows);
    if (j.st != PC_OK) {
        free(j.buf);
        if (j.limit) {
            return cx_limit(err, "PNG decoder memory limit");
        }
        if (j.st == PC_ERR_NOMEM) {
            return cx_nomem(err);
        }
        return cx_malformed(err, F, "%s", j.src.msg[0] != '\0' ? j.src.msg : "invalid PNG data");
    }
    st = pc_cimage_from_raw(j.w, j.h, j.layout, j.sample, j.buf, j.len, &img, err);
    if (st != PC_OK) {
        return st;
    }
    CK(read_meta_chunks(b, n, cx_alloc_usize(l), &img, &images, err));
    CK(read_text_metadata(b, n, cx_alloc_usize(l), &img.meta, err));
    if (images > 1) {
        pc_cwarning w;
        memset(&w, 0, sizeof w);
        w.kind = PC_CW_MORE_FRAMES;
        w.has_total = images <= UINT32_MAX;
        w.total = (uint32_t)images;
        CK(pc_cimage_add_warning(&img, w, err));
    }
    *out = img;
    return PC_OK;
fail:
    pc_cimage_fini(&img);
    return st;
}

/* ---- writing ---- */

static void put_chunk(cx_w *w, const char *type, const uint8_t *data, size_t len)
{
    uint32_t crc;
    if (len > 0x7FFFFFFFu) {
        if (w->st == PC_OK) {
            w->st = cx_encode(w->err, F, "chunk too large");
        }
        return;
    }
    cx_w_be32(w, (uint32_t)len);
    cx_w_put(w, type, 4);
    cx_w_put(w, data, len);
    crc = pc_crc32(pc_crc32(0, (const uint8_t *)type, 4), data, len);
    cx_w_be32(w, crc);
}

static int zlevel(pc_png_compression c)
{
    switch (c) {
    case PC_PNG_NONE:
        return 0;
    case PC_PNG_FAST:
        return 1;
    case PC_PNG_BEST:
        return 9;
    default:
        return 6;
    }
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = (int)a + (int)b - (int)c;
    int pa = abs(p - (int)a);
    int pb = abs(p - (int)b);
    int pc = abs(p - (int)c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

static void filter_with(int kind, const uint8_t *row, const uint8_t *prev, size_t n, size_t bpp, uint8_t *out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t x = row[i];
        uint8_t left = i >= bpp ? row[i - bpp] : 0;
        uint8_t up = prev != NULL ? prev[i] : 0;
        uint8_t ul = i >= bpp && prev != NULL ? prev[i - bpp] : 0;
        switch (kind) {
        case 1:
            out[i] = (uint8_t)(x - left);
            break;
        case 2:
            out[i] = (uint8_t)(x - up);
            break;
        case 3:
            out[i] = (uint8_t)(x - (uint8_t)(((unsigned)left + up) / 2));
            break;
        default:
            out[i] = (uint8_t)(x - paeth(left, up, ul));
        }
    }
}

static uint64_t cost(const uint8_t *v, size_t n)
{
    uint64_t c = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        int s = (int8_t)v[i];
        c += (uint64_t)(s < 0 ? -s : s);
    }
    return c;
}

/* One scanline with the adaptive (minimum sum of absolute differences) filter: type + data. */
static void filter_row(const uint8_t *row, const uint8_t *prev, size_t n, size_t bpp, uint8_t *scratch, uint8_t *out)
{
    uint64_t best = cost(row, n);
    int kind = 0;
    int k;
    for (k = 1; k <= 4; k++) {
        uint64_t c;
        filter_with(k, row, prev, n, bpp, scratch);
        c = cost(scratch, n);
        if (c < best) {
            best = c;
            kind = k;
        }
    }
    out[0] = (uint8_t)kind;
    if (kind == 0) {
        memcpy(out + 1, row, n);
    } else {
        filter_with(kind, row, prev, n, bpp, out + 1);
    }
}

/* Raw deflate of data (finish or sync flush) appended to out. */
static pc_status deflate_raw(const uint8_t *data, size_t n, int level, bool last, pc_buf *out)
{
    z_stream z;
    pc_status st = PC_OK;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return PC_ERR_NOMEM;
    }
    for (;;) {
        uint8_t tmp[65536];
        int flush;
        int r;
        if (z.avail_in == 0 && n > 0) {
            size_t chunk = n > UINT32_MAX ? UINT32_MAX : n;
            z.next_in = (Bytef *)(uintptr_t)data;
            z.avail_in = (uInt)chunk;
            data += chunk;
            n -= chunk;
        }
        flush = n > 0 ? Z_NO_FLUSH : last ? Z_FINISH : Z_SYNC_FLUSH;
        z.next_out = tmp;
        z.avail_out = sizeof tmp;
        r = deflate(&z, flush);
        if (r == Z_STREAM_ERROR) {
            st = PC_ERR_NOMEM;
            break;
        }
        st = pc_buf_append(out, tmp, sizeof tmp - z.avail_out, NULL);
        if (st != PC_OK || (flush == Z_FINISH && r == Z_STREAM_END) ||
            (flush == Z_SYNC_FLUSH && z.avail_in == 0 && z.avail_out != 0)) {
            break;
        }
    }
    deflateEnd(&z);
    return st;
}

typedef struct band_job {
    const uint8_t *data;
    size_t stride;
    size_t bpp;
    size_t h;
    size_t band_rows;
    int level;
    bool filter;
    pc_buf *parts;
    uLong *adler;
    size_t *flen;
} band_job;

static pc_status band_fn(void *ctx, size_t idx, size_t b0, size_t b1)
{
    band_job *j = ctx;
    size_t y0 = idx * j->band_rows;
    size_t y1 = y0 + j->band_rows < j->h ? y0 + j->band_rows : j->h;
    size_t rows = y1 - y0;
    uint8_t *f;
    uint8_t *scratch;
    size_t y;
    pc_status st;
    (void)b0;
    (void)b1;
    f = pc_malloc_array(rows, j->stride + 1);
    scratch = malloc(j->stride > 0 ? j->stride : 1);
    if (f == NULL || scratch == NULL) {
        free(f);
        free(scratch);
        return PC_ERR_NOMEM;
    }
    for (y = y0; y < y1; y++) {
        const uint8_t *row = j->data + y * j->stride;
        const uint8_t *prev = y > 0 ? row - j->stride : NULL;
        uint8_t *o = f + (y - y0) * (j->stride + 1);
        if (j->filter) {
            filter_row(row, prev, j->stride, j->bpp, scratch, o);
        } else {
            o[0] = 0;
            memcpy(o + 1, row, j->stride);
        }
    }
    j->flen[idx] = rows * (j->stride + 1);
    {
        /* adler32 takes uInt lengths: fold large bands in pieces */
        size_t done = 0;
        uLong a = 1L;
        while (done < j->flen[idx]) {
            size_t k = j->flen[idx] - done > 0x40000000u ? 0x40000000u : j->flen[idx] - done;
            a = adler32(a, f + done, (uInt)k);
            done += k;
        }
        j->adler[idx] = a;
    }
    st = deflate_raw(f, j->flen[idx], j->level, y1 == j->h, &j->parts[idx]);
    free(f);
    free(scratch);
    return st;
}

/* Filtered bytes per deflate band. The bands depend only on the image, never on the number of
 * threads, so the same image and options give the same bytes whatever the machine's core count. */
#define BAND_BYTES ((size_t)512 << 10)

/* The zlib stream of a non-interlaced image: filtered rows deflated in bands (in parallel for
 * images of at least 4 MB), each band ending on a sync flush so the raw streams concatenate. */
pc_status cx_png_idat_stream(const uint8_t *data, size_t w, size_t h, size_t bpp, pc_png_compression level,
                             pc_pool *pool, pc_buf *out, pc_error *err)
{
    band_job j;
    size_t stride = w * bpp;
    size_t nb;
    size_t i;
    uLong adler = 1L;
    pc_status st = PC_OK;
    static const uint8_t HDR[4][2] = {{0x78, 0x01}, {0x78, 0x01}, {0x78, 0x9C}, {0x78, 0xDA}};
    j.data = data;
    j.stride = stride;
    j.bpp = bpp;
    j.h = h;
    j.level = zlevel(level);
    j.filter = level != PC_PNG_NONE;
    if (stride * h >= ((size_t)4 << 20) && level != PC_PNG_NONE) {
        j.band_rows = BAND_BYTES / (stride + 1);
    } else {
        j.band_rows = h;
    }
    if (j.band_rows == 0) {
        j.band_rows = 1;
    }
    nb = (h + j.band_rows - 1) / j.band_rows;
    j.parts = calloc(nb, sizeof *j.parts);
    j.adler = calloc(nb, sizeof *j.adler);
    j.flen = calloc(nb, sizeof *j.flen);
    if (j.parts == NULL || j.adler == NULL || j.flen == NULL) {
        st = cx_nomem(err);
    } else {
        st = pc_pool_for(nb > 1 ? pool : NULL, nb, 1, band_fn, &j);
        if (st != PC_OK) {
            st = st == PC_ERR_NOMEM ? cx_nomem(err) : cx_encode(err, F, "deflate failed");
        }
    }
    if (st == PC_OK) {
        st = pc_buf_append(out, HDR[level], 2, err);
    }
    for (i = 0; st == PC_OK && i < nb; i++) {
        st = pc_buf_append(out, j.parts[i].data, j.parts[i].len, err);
        adler = i == 0 ? j.adler[0] : adler32_combine(adler, j.adler[i], (z_off_t)j.flen[i]);
    }
    if (st == PC_OK) {
        uint8_t a[4] = {(uint8_t)(adler >> 24), (uint8_t)(adler >> 16), (uint8_t)(adler >> 8), (uint8_t)adler};
        st = pc_buf_append(out, a, 4, err);
    }
    for (i = 0; j.parts != NULL && i < nb; i++) {
        pc_buf_fini(&j.parts[i]);
    }
    free(j.parts);
    free(j.adler);
    free(j.flen);
    return st;
}

/* The zlib stream of an Adam7-interlaced image (filter type 0). */
static pc_status adam7_stream(const uint8_t *data, size_t w, size_t h, size_t bpp, pc_png_compression level,
                              pc_buf *out, pc_error *err)
{
    static const size_t PASSES[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                        {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    pc_buf raw = PC_BUF_INIT;
    pc_status st = PC_OK;
    int p;
    for (p = 0; p < 7 && st == PC_OK; p++) {
        size_t x0 = PASSES[p][0];
        size_t y0 = PASSES[p][1];
        size_t dx = PASSES[p][2];
        size_t dy = PASSES[p][3];
        size_t y;
        if (x0 >= w || y0 >= h) {
            continue;
        }
        for (y = y0; y < h && st == PC_OK; y += dy) {
            size_t x;
            st = pc_buf_append_byte(&raw, 0, err);
            for (x = x0; x < w && st == PC_OK; x += dx) {
                st = pc_buf_append(&raw, data + (y * w + x) * bpp, bpp, err);
            }
        }
    }
    if (st == PC_OK) {
        st = pc_deflate(PC_Z_ZLIB, zlevel(level), raw.data, raw.len, out, err);
    }
    pc_buf_fini(&raw);
    return st;
}

static bool latin1_key(const char *k, size_t n)
{
    size_t i = 0;
    uint32_t cp;
    if (n == 0) {
        return false;
    }
    while (i < n) {
        if (!pc_utf8_next((const uint8_t *)k, n, &i, &cp)) {
            return false;
        }
        if (!((cp >= ' ' && cp <= '~') || (cp >= 0xA1 && cp <= 0xFF))) {
            return false;
        }
    }
    /* At most 79 Latin-1 bytes: the reference checks the UTF-8 length. */
    return n <= 79;
}

/* UTF-8 -> Latin-1 when every char is below 256 (false otherwise). */
static bool to_latin1(const char *s, size_t n, pc_buf *out, pc_error *err)
{
    size_t i = 0;
    uint32_t cp;
    while (i < n) {
        if (!pc_utf8_next((const uint8_t *)s, n, &i, &cp)) {
            i++;
            cp = 0xFFFD;
        }
        if (cp > 0xFF) {
            return false;
        }
        if (pc_buf_append_byte(out, (uint8_t)cp, err) != PC_OK) {
            return false;
        }
    }
    return true;
}

static void put_text(cx_w *w, const char *k, size_t kn, const char *v, size_t vn, bool itxt)
{
    pc_buf b = PC_BUF_INIT;
    pc_buf l1 = PC_BUF_INIT;
    pc_status st = PC_OK;
    if (w->st != PC_OK) {
        return;
    }
    if (!itxt && to_latin1(v, vn, &l1, w->err)) {
        pc_buf kb = PC_BUF_INIT;
        (void)to_latin1(k, kn, &kb, w->err);
        st = pc_buf_append(&b, kb.data, kb.len, w->err);
        pc_buf_fini(&kb);
        if (st == PC_OK) {
            st = pc_buf_append_byte(&b, 0, w->err);
        }
        if (st == PC_OK) {
            st = pc_buf_append(&b, l1.data, l1.len, w->err);
        }
        if (st == PC_OK) {
            put_chunk(w, "tEXt", b.data, b.len);
        }
    } else {
        pc_buf kb = PC_BUF_INIT;
        (void)to_latin1(k, kn, &kb, w->err);
        st = pc_buf_append(&b, kb.data, kb.len, w->err);
        pc_buf_fini(&kb);
        if (st == PC_OK) {
            static const uint8_t z[5] = {0, 0, 0, 0, 0}; /* NUL, flag 0, method 0, language, translated */
            st = pc_buf_append(&b, z, 5, w->err);
        }
        if (st == PC_OK) {
            st = pc_buf_append(&b, v, vn, w->err);
        }
        if (st == PC_OK) {
            put_chunk(w, "iTXt", b.data, b.len);
        }
    }
    if (st != PC_OK && w->st == PC_OK) {
        w->st = st;
    }
    pc_buf_fini(&b);
    pc_buf_fini(&l1);
}

pc_status cx_png_encode(const pc_cimage *src, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    uint8_t color;
    uint8_t depth;
    uint8_t *be = NULL;
    const uint8_t *data;
    cx_w w = {out, PC_OK, err};
    uint8_t ihdr[13];
    pc_buf idat = PC_BUF_INIT;
    size_t bpp;
    float dpiv[2];
    const float *dpi = NULL;
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    switch (img->layout) {
    case PC_CL_GRAY:
        color = 0;
        break;
    case PC_CL_GRAYA:
        color = 4;
        break;
    case PC_CL_RGB:
        color = 2;
        break;
    case PC_CL_RGBA:
        color = 6;
        break;
    default:
        st = cx_encode(err, F, "unsupported layout %s", pc_clayout_name(img->layout));
        goto done;
    }
    if (img->sample == PC_CS_U8) {
        depth = 8;
    } else if (img->sample == PC_CS_U16) {
        depth = 16;
    } else {
        st = cx_encode(err, F, "unsupported sample %s", pc_csample_name(img->sample));
        goto done;
    }
    data = img->data;
    if (depth == 16) {
        size_t i;
        be = malloc(img->len > 0 ? img->len : 1);
        if (be == NULL) {
            st = cx_nomem(err);
            goto done;
        }
        for (i = 0; i < img->len / 2; i++) {
            uint16_t v = cx_ne16(img->data, i);
            be[2 * i] = (uint8_t)(v >> 8);
            be[2 * i + 1] = (uint8_t)v;
        }
        data = be;
    }
    cx_w_put(&w, "\x89PNG\r\n\x1a\n", 8);
    ihdr[0] = (uint8_t)(img->width >> 24);
    ihdr[1] = (uint8_t)(img->width >> 16);
    ihdr[2] = (uint8_t)(img->width >> 8);
    ihdr[3] = (uint8_t)img->width;
    ihdr[4] = (uint8_t)(img->height >> 24);
    ihdr[5] = (uint8_t)(img->height >> 16);
    ihdr[6] = (uint8_t)(img->height >> 8);
    ihdr[7] = (uint8_t)img->height;
    ihdr[8] = depth;
    ihdr[9] = color;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = o->png_interlaced ? 1 : 0;
    put_chunk(&w, "IHDR", ihdr, 13);
    if (o->embed_icc && img->icc.has) {
        pc_buf c = PC_BUF_INIT;
        st = pc_buf_append(&c, "ICC profile\0\0", 13, err);
        if (st == PC_OK) {
            st = pc_deflate(PC_Z_ZLIB, 6, img->icc.data, img->icc.len, &c, err);
        }
        if (st == PC_OK) {
            put_chunk(&w, "iCCP", c.data, c.len);
        }
        pc_buf_fini(&c);
        if (st != PC_OK) {
            goto done;
        }
    }
    if (img->meta.has_dpi) {
        dpiv[0] = img->meta.dpi_x;
        dpiv[1] = img->meta.dpi_y;
        dpi = dpiv;
    }
    if (o->embed_metadata) {
        if (img->meta.exif.has) {
            /* The pixels are written as they are shown: never let a viewer rotate them again. */
            uint8_t *up = NULL;
            size_t un = 0;
            st = pc_export_exif(img->meta.exif.data, img->meta.exif.len, dpi, &up, &un, err);
            if (st != PC_OK) {
                goto done;
            }
            if (up != NULL) {
                put_chunk(&w, "eXIf", up, un);
                free(up);
            } else {
                put_chunk(&w, "eXIf", img->meta.exif.data, img->meta.exif.len);
            }
        }
        if (img->meta.has_dpi && img->meta.dpi_x > 0.0f && img->meta.dpi_y > 0.0f) {
            uint8_t phys[9];
            uint32_t x = pc_f32_to_u32(roundf(img->meta.dpi_x / METERS_PER_INCH));
            uint32_t y = pc_f32_to_u32(roundf(img->meta.dpi_y / METERS_PER_INCH));
            phys[0] = (uint8_t)(x >> 24);
            phys[1] = (uint8_t)(x >> 16);
            phys[2] = (uint8_t)(x >> 8);
            phys[3] = (uint8_t)x;
            phys[4] = (uint8_t)(y >> 24);
            phys[5] = (uint8_t)(y >> 16);
            phys[6] = (uint8_t)(y >> 8);
            phys[7] = (uint8_t)y;
            phys[8] = 1;
            put_chunk(&w, "pHYs", phys, 9);
        }
        {
            size_t i;
            for (i = 0; i < img->meta.ntext; i++) {
                const pc_ctext *t = &img->meta.text[i];
                pc_buf l1 = PC_BUF_INIT;
                bool latin1_text;
                if (!latin1_key(t->key, t->key_len)) {
                    continue;
                }
                latin1_text = to_latin1(t->value, t->value_len, &l1, NULL);
                pc_buf_fini(&l1);
                put_text(&w, t->key, t->key_len, t->value, t->value_len, !latin1_text);
            }
        }
        if (img->meta.xmp.has) {
            char *up = NULL;
            size_t un = 0;
            st = pc_export_xmp((const char *)img->meta.xmp.data, img->meta.xmp.len, dpi, &up, &un, err);
            if (st != PC_OK) {
                goto done;
            }
            if (up != NULL) {
                put_text(&w, XMP_KEYWORD, sizeof XMP_KEYWORD - 1, up, un, true);
                free(up);
            } else {
                put_text(&w, XMP_KEYWORD, sizeof XMP_KEYWORD - 1, (const char *)img->meta.xmp.data,
                         img->meta.xmp.len, true);
            }
        }
    }
    bpp = pc_clayout_channels(img->layout) * pc_csample_bytes(img->sample);
    if (w.st == PC_OK) {
        if (o->png_interlaced) {
            st = adam7_stream(data, img->width, img->height, bpp, o->png_compression, &idat, err);
        } else {
            st = cx_png_idat_stream(data, img->width, img->height, bpp, o->png_compression, pc_pool_global(),
                                    &idat, err);
        }
        if (st != PC_OK) {
            goto done;
        }
        put_chunk(&w, "IDAT", idat.data, idat.len);
        put_chunk(&w, "IEND", NULL, 0);
    }
    st = w.st;
done:
    pc_buf_fini(&idat);
    free(be);
    pc_cimage_fini(&tmp);
    return st;
}

pc_status pc_cencode_png_indexed(uint32_t w, uint32_t h, const uint8_t *indices, size_t n,
                                 const uint8_t (*palette)[3], size_t npal, int transparent, pc_buf *out,
                                 pc_error *err)
{
    return pc_cencode_png_indexed_dpi(w, h, indices, n, palette, npal, transparent, NULL, out, err);
}

pc_status pc_cencode_png_indexed_dpi(uint32_t w, uint32_t h, const uint8_t *indices, size_t n,
                                     const uint8_t (*palette)[3], size_t npal, int transparent,
                                     const float *dpi, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_buf idat = PC_BUF_INIT;
    cx_w wr = {&b, PC_OK, err};
    uint8_t ihdr[13];
    size_t i;
    pc_status st;
    if (w == 0 || h == 0 || (uint64_t)w * h != n) {
        return cx_invalid(err, "index data does not match the size");
    }
    if (npal == 0 || npal > 256) {
        return cx_invalid(err, "palette must have 1..=256 entries covering every index");
    }
    for (i = 0; i < n; i++) {
        if (indices[i] >= npal) {
            return cx_invalid(err, "palette must have 1..=256 entries covering every index");
        }
    }
    cx_w_put(&wr, "\x89PNG\r\n\x1a\n", 8);
    for (i = 0; i < 4; i++) {
        ihdr[i] = (uint8_t)(w >> (24 - 8 * i));
        ihdr[4 + i] = (uint8_t)(h >> (24 - 8 * i));
    }
    ihdr[8] = 8;
    ihdr[9] = 3;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    put_chunk(&wr, "IHDR", ihdr, 13);
    if (dpi != NULL && isfinite(dpi[0]) && isfinite(dpi[1]) && dpi[0] > 0.0f && dpi[1] > 0.0f) {
        uint8_t phys[9];
        uint32_t x = pc_f32_to_u32(roundf(dpi[0] / METERS_PER_INCH));
        uint32_t y = pc_f32_to_u32(roundf(dpi[1] / METERS_PER_INCH));
        for (i = 0; i < 4; i++) {
            phys[i] = (uint8_t)(x >> (24 - 8 * i));
            phys[4 + i] = (uint8_t)(y >> (24 - 8 * i));
        }
        phys[8] = 1;
        put_chunk(&wr, "pHYs", phys, 9);
    }
    put_chunk(&wr, "PLTE", palette[0], npal * 3);
    if (transparent >= 0 && (size_t)transparent < npal) {
        uint8_t trns[256];
        memset(trns, 255, sizeof trns);
        trns[transparent] = 0;
        put_chunk(&wr, "tRNS", trns, (size_t)transparent + 1);
    }
    st = cx_png_idat_stream(indices, w, h, 1, PC_PNG_DEFAULT, pc_pool_global(), &idat, err);
    if (st == PC_OK) {
        put_chunk(&wr, "IDAT", idat.data, idat.len);
        put_chunk(&wr, "IEND", NULL, 0);
        st = wr.st;
    }
    pc_buf_fini(&idat);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}
