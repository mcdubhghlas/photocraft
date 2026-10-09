/* JPEG (port of crates/codecs/src/codecs/jpeg.rs): decode and encode with libjpeg-turbo (gray,
 * YCbCr, CMYK and YCCK in; gray, RGB and CMYK out). Metadata (APP0 JFIF density, APP1 EXIF/XMP,
 * APP2 multi-segment ICC, APP14 Adobe) is read by our own marker scanner, and a truncated file
 * decodes with its missing part grey and a warning, as the reference does. */
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "base/utf8.h"
#include "codecs/internal.h"
#include "codecs/orientation.h"

#define F PC_CF_JPEG
#define EXIF_HEADER "Exif\0\0"
#define EXIF_HEADER_LEN 6
#define XMP_HEADER "http://ns.adobe.com/xap/1.0/\0"
#define XMP_HEADER_LEN 29
#define ICC_HEADER "ICC_PROFILE\0"
#define MAX_SEGMENT 65533u

/* ---- marker scan ---- */

typedef struct icc_part {
    uint8_t seq;
    uint8_t total;
    const uint8_t *data;
    size_t len;
} icc_part;

typedef struct jmeta {
    pc_blob icc;
    pc_blob exif;
    pc_blob xmp;
    bool has_dpi;
    float dpi_x;
    float dpi_y;
    bool has_frame;
    uint32_t fw;
    uint32_t fh;
    uint8_t fc;
} jmeta;

static void jmeta_fini(jmeta *m)
{
    pc_blob_fini(&m->icc);
    pc_blob_fini(&m->exif);
    pc_blob_fini(&m->xmp);
}

static bool valid_utf8(const uint8_t *b, size_t n)
{
    size_t i = 0;
    uint32_t cp;
    while (i < n) {
        if (!pc_utf8_next(b, n, &i, &cp)) {
            return false;
        }
    }
    return true;
}

static pc_status scan_metadata(const uint8_t *b, size_t n, jmeta *m, pc_error *err)
{
    icc_part *parts = NULL;
    size_t np = 0;
    size_t cap = 0;
    size_t i = 2;
    pc_status st = PC_OK;
    memset(m, 0, sizeof *m);
    while (st == PC_OK && i + 4 <= n) {
        uint8_t marker;
        size_t len;
        const uint8_t *seg;
        size_t sl;
        if (b[i] != 0xFF) {
            break;
        }
        marker = b[i + 1];
        if (marker == 0xFF) {
            i += 1;
            continue;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            i += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) {
            break;
        }
        len = cx_be16(b + i + 2);
        if (len < 2 || i + 2 + len > n) {
            break;
        }
        seg = b + i + 4;
        sl = len - 2;
        if (marker == 0xE0 && sl >= 12 && memcmp(seg, "JFIF\0", 5) == 0) {
            uint8_t unit = seg[7];
            float x = (float)cx_be16(seg + 8);
            float y = (float)cx_be16(seg + 10);
            if (x > 0.0f && y > 0.0f) {
                m->has_dpi = unit == 1 || unit == 2;
                m->dpi_x = unit == 2 ? x * 2.54f : x;
                m->dpi_y = unit == 2 ? y * 2.54f : y;
            }
        } else if (marker == 0xE1 && sl >= EXIF_HEADER_LEN && memcmp(seg, EXIF_HEADER, EXIF_HEADER_LEN) == 0 &&
                   !m->exif.has) {
            st = pc_blob_set(&m->exif, seg + EXIF_HEADER_LEN, sl - EXIF_HEADER_LEN, err);
        } else if (marker == 0xE1 && sl >= XMP_HEADER_LEN && memcmp(seg, XMP_HEADER, XMP_HEADER_LEN) == 0 &&
                   !m->xmp.has) {
            const uint8_t *x = seg + XMP_HEADER_LEN;
            size_t xn = sl - XMP_HEADER_LEN;
            if (valid_utf8(x, xn)) {
                while (xn > 0 && x[xn - 1] == 0) {
                    xn--;
                }
                st = pc_blob_set(&m->xmp, x, xn, err);
            }
        } else if (marker == 0xE2 && sl >= 14 && memcmp(seg, ICC_HEADER, 12) == 0) {
            icc_part *g = pc_grow_array(parts, &cap, np + 1, sizeof *g);
            if (g == NULL) {
                st = cx_nomem(err);
                break;
            }
            parts = g;
            parts[np].seq = seg[12];
            parts[np].total = seg[13];
            parts[np].data = seg + 14;
            parts[np].len = sl - 14;
            np++;
        } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC &&
                   sl >= 6 && !m->has_frame) {
            m->has_frame = true;
            m->fh = cx_be16(seg + 1);
            m->fw = cx_be16(seg + 3);
            m->fc = seg[5];
        }
        i += 2 + len;
    }
    if (st == PC_OK && np > 0) {
        size_t k;
        size_t j;
        bool ok;
        /* stable sort by sequence number */
        for (k = 1; k < np; k++) {
            icc_part t = parts[k];
            j = k;
            while (j > 0 && parts[j - 1].seq > t.seq) {
                parts[j] = parts[j - 1];
                j--;
            }
            parts[j] = t;
        }
        ok = np == parts[0].total;
        for (k = 0; ok && k < np; k++) {
            ok = parts[k].seq == k + 1 && parts[k].total == parts[0].total;
        }
        if (ok || np == 1) {
            pc_buf all = PC_BUF_INIT;
            for (k = 0; st == PC_OK && k < np; k++) {
                st = pc_buf_append(&all, parts[k].data, parts[k].len, err);
            }
            if (st == PC_OK) {
                st = pc_blob_set(&m->icc, all.data != NULL ? all.data : (const uint8_t *)"", all.len, err);
            }
            pc_buf_fini(&all);
        }
    }
    free(parts);
    if (st != PC_OK) {
        jmeta_fini(m);
    }
    return st;
}

/* How a JPEG's data ends, seen from its marker structure. */
enum { END_COMPLETE, END_TRUNCATED, END_EMPTY };

static int data_end(const uint8_t *b, size_t n)
{
    size_t i = 2;
    bool scan_data = false;
    for (;;) {
        uint8_t code;
        if (i >= n) {
            return scan_data ? END_TRUNCATED : END_EMPTY;
        }
        if (b[i] != 0xFF) {
            return END_COMPLETE; /* too odd to judge */
        }
        if (i + 1 >= n) {
            return scan_data ? END_TRUNCATED : END_EMPTY;
        }
        code = b[i + 1];
        if (code == 0xD9) {
            return END_COMPLETE;
        }
        if (code == 0xFF) {
            i += 1;
        } else if (code == 0x01 || (code >= 0xD0 && code <= 0xD8)) {
            i += 2;
        } else {
            size_t len;
            if (i + 4 > n) {
                return scan_data ? END_TRUNCATED : END_EMPTY;
            }
            len = cx_be16(b + i + 2);
            i = i + 2 + len < i ? SIZE_MAX : i + 2 + len;
            if (code == 0xDA) {
                size_t start = i;
                for (;;) {
                    const uint8_t *p = i < n ? memchr(b + i, 0xFF, n - i) : NULL;
                    if (p == NULL) {
                        return (scan_data || n > start) ? END_TRUNCATED : END_EMPTY;
                    }
                    i = (size_t)(p - b);
                    if (i + 1 < n && (b[i + 1] == 0x00 || (b[i + 1] >= 0xD0 && b[i + 1] <= 0xD7))) {
                        i += 2;
                    } else if (i + 1 < n && b[i + 1] == 0xFF) {
                        i += 1;
                    } else {
                        break;
                    }
                }
                scan_data = scan_data || i > start;
            }
        }
    }
}

/* ---- libjpeg glue ---- */

typedef struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX];
} jerr;

static void on_error(j_common_ptr c)
{
    jerr *e = (jerr *)(void *)c->err;
    (*c->err->format_message)(c, e->msg);
    longjmp(e->jb, 1);
}

static void on_message(j_common_ptr c, int level)
{
    (void)c;
    (void)level;
}

static const JOCTET FAKE_EOI[2] = {0xFF, JPEG_EOI};

static void src_init(j_decompress_ptr c) { (void)c; }

static boolean src_fill(j_decompress_ptr c)
{
    /* Out of data: feed an end-of-image marker; the missing rows come out grey. */
    c->src->next_input_byte = FAKE_EOI;
    c->src->bytes_in_buffer = 2;
    return TRUE;
}

static void src_skip(j_decompress_ptr c, long k)
{
    struct jpeg_source_mgr *s = c->src;
    if (k <= 0) {
        return;
    }
    if ((size_t)k > s->bytes_in_buffer) {
        (void)src_fill(c);
    } else {
        s->next_input_byte += k;
        s->bytes_in_buffer -= (size_t)k;
    }
}

static void src_term(j_decompress_ptr c) { (void)c; }

typedef struct dec_job {
    const uint8_t *b;
    size_t n;
    const pc_climits *l;
    pc_error *err;
    uint8_t *px;
    uint32_t w;
    uint32_t h;
    pc_clayout layout;
    J_COLOR_SPACE in;
    pc_status st;
    bool limit_checked;
} dec_job;

static void run_decode(dec_job *j)
{
    struct jpeg_decompress_struct c;
    struct jpeg_source_mgr src;
    jerr e;
    memset(&c, 0, sizeof c);
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = on_error;
    e.pub.emit_message = on_message;
    e.msg[0] = '\0';
    if (setjmp(e.jb)) {
        jpeg_destroy_decompress(&c);
        if (j->st == PC_OK) {
            j->st = cx_malformed(j->err, F, "%s", e.msg);
        }
        return;
    }
    jpeg_create_decompress(&c);
    src.next_input_byte = j->b;
    src.bytes_in_buffer = j->n;
    src.init_source = src_init;
    src.fill_input_buffer = src_fill;
    src.skip_input_data = src_skip;
    src.resync_to_restart = jpeg_resync_to_restart;
    src.term_source = src_term;
    c.src = &src;
    (void)jpeg_read_header(&c, TRUE);
    j->in = c.jpeg_color_space;
    switch (c.jpeg_color_space) {
    case JCS_GRAYSCALE:
        c.out_color_space = JCS_GRAYSCALE;
        j->layout = PC_CL_GRAY;
        break;
    case JCS_CMYK:
    case JCS_YCCK:
        c.out_color_space = JCS_CMYK;
        j->layout = PC_CL_CMYK;
        break;
    case JCS_UNKNOWN:
        if (c.num_components != 3) {
            j->st = cx_unsupported(j->err, F, "%d-component images", c.num_components);
            jpeg_destroy_decompress(&c);
            return;
        }
        c.out_color_space = JCS_RGB;
        j->layout = PC_CL_RGB;
        break;
    default:
        c.out_color_space = JCS_RGB;
        j->layout = PC_CL_RGB;
    }
    jpeg_calc_output_dimensions(&c);
    j->w = c.output_width;
    j->h = c.output_height;
    if ((j->st = pc_climits_check(j->l, j->w, j->h, j->layout, PC_CS_U8, j->err)) != PC_OK) {
        jpeg_destroy_decompress(&c);
        return;
    }
    (void)jpeg_start_decompress(&c);
    if ((size_t)c.output_components != pc_clayout_channels(j->layout)) {
        j->st = cx_malformed(j->err, F, "short pixel buffer");
        jpeg_destroy_decompress(&c);
        return;
    }
    {
        size_t row = (size_t)j->w * pc_clayout_channels(j->layout);
        j->px = pc_malloc_array(j->h > 0 ? j->h : 1, row > 0 ? row : 1);
        if (j->px == NULL) {
            j->st = cx_limit(j->err, "cannot allocate the decoded image");
            jpeg_destroy_decompress(&c);
            return;
        }
        while (c.output_scanline < c.output_height) {
            JSAMPROW r = j->px + (size_t)c.output_scanline * row;
            (void)jpeg_read_scanlines(&c, &r, 1);
        }
    }
    (void)jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
}

pc_status cx_jpeg_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    int end = data_end(b, n);
    jmeta m;
    dec_job j;
    pc_cimage img;
    size_t i;
    pc_status st;
    if (end == END_EMPTY) {
        return cx_malformed(err, F, "the file ends before any image data");
    }
    if ((st = scan_metadata(b, n, &m, err)) != PC_OK) {
        return st;
    }
    if (m.has_frame && (st = cx_check_bytes(l, m.fw, m.fh, m.fc > 0 ? m.fc : 1, err)) != PC_OK) {
        jmeta_fini(&m);
        return st;
    }
    memset(&j, 0, sizeof j);
    j.b = b;
    j.n = n;
    j.l = l;
    j.err = err;
    run_decode(&j);
    if (j.st != PC_OK) {
        free(j.px);
        jmeta_fini(&m);
        return j.st;
    }
    if (j.layout == PC_CL_CMYK) {
        /* Adobe-style CMYK is stored inverted (255 = no ink); libjpeg's YCCK conversion gives
         * the same inverted CMYK. */
        size_t len = (size_t)j.w * j.h * 4;
        for (i = 0; i < len; i++) {
            j.px[i] = (uint8_t)(255 - j.px[i]);
        }
    }
    st = pc_cimage_from_raw(j.w, j.h, j.layout, PC_CS_U8, j.px, (size_t)j.w * j.h * pc_clayout_channels(j.layout),
                            &img, err);
    if (st != PC_OK) {
        jmeta_fini(&m);
        return st;
    }
    img.icc = m.icc;
    img.meta.exif = m.exif;
    img.meta.xmp = m.xmp;
    img.meta.has_dpi = m.has_dpi;
    img.meta.dpi_x = m.dpi_x;
    img.meta.dpi_y = m.dpi_y;
    if (end == END_TRUNCATED) {
        pc_cwarning w;
        memset(&w, 0, sizeof w);
        w.kind = PC_CW_TRUNCATED;
        w.format = F;
        if ((st = pc_cimage_add_warning(&img, w, err)) != PC_OK) {
            pc_cimage_fini(&img);
            return st;
        }
    }
    *out = img;
    return PC_OK;
}

/* ---- segments ---- */

bool cx_jpeg_exif_segment(const uint8_t *exif, size_t n, pc_buf *seg)
{
    uint8_t *up = NULL;
    size_t un = 0;
    const uint8_t *d = exif;
    bool ok;
    /* The pixels are written as they are shown: never let a viewer rotate them again. */
    if (pc_upright_exif(exif, n, &up, &un, NULL) != PC_OK) {
        return false;
    }
    if (up != NULL) {
        d = up;
        n = un;
    }
    pc_buf_truncate(seg, 0);
    ok = EXIF_HEADER_LEN + n <= MAX_SEGMENT && pc_buf_append(seg, EXIF_HEADER, EXIF_HEADER_LEN, NULL) == PC_OK &&
         pc_buf_append(seg, d, n, NULL) == PC_OK;
    free(up);
    return ok;
}

static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.';
}

static const char *find_str(const char *s, size_t n, const char *k, size_t kl)
{
    size_t i;
    for (i = 0; i + kl <= n; i++) {
        if (memcmp(s + i, k, kl) == 0) {
            return s + i;
        }
    }
    return NULL;
}

/* xmp without its <prefix:name ...>...</prefix:name> and <prefix:name .../> elements. */
static pc_status remove_element(const char *x, size_t n, const char *name, pc_buf *out)
{
    char local[64];
    size_t ll;
    const char *rest = x;
    size_t rn = n;
    const char *at;
    pc_status st = PC_OK;
    (void)snprintf(local, sizeof local, ":%s", name);
    ll = strlen(local);
    while (st == PC_OK && (at = find_str(rest, rn, local, ll)) != NULL) {
        size_t j = (size_t)(at - rest);
        size_t prefix_len = 0;
        size_t lt;
        const char *after = rest + j + ll;
        size_t an = rn - j - ll;
        bool opening;
        const char *gt;
        size_t end;
        while (prefix_len < j && name_char(rest[j - 1 - prefix_len])) {
            prefix_len++;
        }
        lt = j - prefix_len;
        opening = prefix_len > 0 && lt > 0 && rest[lt - 1] == '<';
        if (!opening || an == 0 ||
            !(after[0] == '>' || after[0] == '/' || after[0] == ' ' || after[0] == '\t' || after[0] == '\r' ||
              after[0] == '\n')) {
            st = pc_buf_append(out, rest, j + ll, NULL);
            rest = after;
            rn = an;
            continue;
        }
        gt = memchr(after, '>', an);
        if (gt == NULL) {
            break;
        }
        if (gt > after && gt[-1] == '/') {
            end = (size_t)(gt - after) + 1;
        } else {
            char close[160];
            const char *c;
            int cl = snprintf(close, sizeof close, "</%.*s%s>", (int)(j - lt), rest + lt, local);
            if (cl < 0 || (size_t)cl >= sizeof close) {
                break;
            }
            c = find_str(after, an, close, (size_t)cl);
            if (c == NULL) {
                break;
            }
            end = (size_t)(c - after) + (size_t)cl;
        }
        st = pc_buf_append(out, rest, lt - 1, NULL);
        rest = after + end;
        rn = an - end;
    }
    if (st == PC_OK) {
        st = pc_buf_append(out, rest, rn, NULL);
    }
    return st;
}

bool cx_jpeg_xmp_segment(const uint8_t *xmp, size_t n, pc_buf *seg)
{
    char *up = NULL;
    size_t un = 0;
    const char *x = (const char *)xmp;
    pc_buf a = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    bool ok = false;
    if (pc_upright_xmp((const char *)xmp, n, &up, &un, NULL) != PC_OK) {
        return false;
    }
    if (up != NULL) {
        x = up;
        n = un;
    }
    pc_buf_truncate(seg, 0);
    if (XMP_HEADER_LEN + n <= MAX_SEGMENT) {
        ok = pc_buf_append(seg, XMP_HEADER, XMP_HEADER_LEN, NULL) == PC_OK && pc_buf_append(seg, x, n, NULL) == PC_OK;
    } else if (remove_element(x, n, "DocumentAncestors", &a) == PC_OK &&
               remove_element((const char *)a.data, a.len, "TextLayers", &b) == PC_OK &&
               XMP_HEADER_LEN + b.len <= MAX_SEGMENT) {
        ok = pc_buf_append(seg, XMP_HEADER, XMP_HEADER_LEN, NULL) == PC_OK &&
             pc_buf_append(seg, b.data, b.len, NULL) == PC_OK;
    }
    pc_buf_fini(&a);
    pc_buf_fini(&b);
    free(up);
    return ok;
}

/* ---- encoding ---- */

typedef struct dest {
    struct jpeg_destination_mgr pub;
    pc_buf *out;
    uint8_t tmp[65536];
    bool failed;
} dest;

static void dst_init(j_compress_ptr c)
{
    dest *d = (dest *)(void *)c->dest;
    d->pub.next_output_byte = d->tmp;
    d->pub.free_in_buffer = sizeof d->tmp;
}

static boolean dst_empty(j_compress_ptr c)
{
    dest *d = (dest *)(void *)c->dest;
    if (pc_buf_append(d->out, d->tmp, sizeof d->tmp, NULL) != PC_OK) {
        d->failed = true;
    }
    d->pub.next_output_byte = d->tmp;
    d->pub.free_in_buffer = sizeof d->tmp;
    return TRUE;
}

static void dst_term(j_compress_ptr c)
{
    dest *d = (dest *)(void *)c->dest;
    if (pc_buf_append(d->out, d->tmp, sizeof d->tmp - d->pub.free_in_buffer, NULL) != PC_OK) {
        d->failed = true;
    }
}

typedef struct enc_job {
    const uint8_t *px;
    uint32_t w;
    uint32_t h;
    pc_clayout layout;
    int quality;
    bool subsample;
    bool progressive;
    bool optimize;
    bool has_density;
    uint16_t dx;
    uint16_t dy;
    const pc_buf *app1[2];
    const uint8_t *icc;
    size_t icc_len;
    pc_buf *out;
    char msg[JMSG_LENGTH_MAX];
    bool failed;
} enc_job;

static void run_encode(enc_job *j)
{
    struct jpeg_compress_struct c;
    jerr e;
    dest *d = calloc(1, sizeof *d);
    uint8_t *volatile row = NULL; /* set after setjmp, freed by the handler */
    size_t ch = pc_clayout_channels(j->layout);
    int i;
    if (d == NULL) {
        j->failed = true;
        (void)snprintf(j->msg, sizeof j->msg, "out of memory");
        return;
    }
    memset(&c, 0, sizeof c);
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = on_error;
    e.pub.emit_message = on_message;
    if (setjmp(e.jb)) {
        jpeg_destroy_compress(&c);
        (void)snprintf(j->msg, sizeof j->msg, "%s", e.msg);
        j->failed = true;
        free(row);
        free(d);
        return;
    }
    jpeg_create_compress(&c);
    d->out = j->out;
    d->pub.init_destination = dst_init;
    d->pub.empty_output_buffer = dst_empty;
    d->pub.term_destination = dst_term;
    c.dest = &d->pub;
    c.image_width = j->w;
    c.image_height = j->h;
    c.input_components = (int)ch;
    c.in_color_space = j->layout == PC_CL_GRAY ? JCS_GRAYSCALE : j->layout == PC_CL_RGB ? JCS_RGB : JCS_CMYK;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, j->quality, TRUE);
    for (i = 0; i < c.num_components; i++) {
        c.comp_info[i].h_samp_factor = 1;
        c.comp_info[i].v_samp_factor = 1;
    }
    if (j->subsample && j->layout == PC_CL_RGB) {
        c.comp_info[0].h_samp_factor = 2;
        c.comp_info[0].v_samp_factor = 2;
    }
    if (j->progressive) {
        jpeg_simple_progression(&c);
    }
    c.optimize_coding = j->optimize ? TRUE : FALSE;
    if (j->has_density) {
        c.density_unit = 1;
        c.X_density = j->dx;
        c.Y_density = j->dy;
    }
    jpeg_start_compress(&c, TRUE);
    for (i = 0; i < 2; i++) {
        if (j->app1[i] != NULL) {
            jpeg_write_marker(&c, JPEG_APP0 + 1, j->app1[i]->data, (unsigned)j->app1[i]->len);
        }
    }
    if (j->icc != NULL) {
        jpeg_write_icc_profile(&c, j->icc, (unsigned)j->icc_len);
    }
    if (j->layout == PC_CL_CMYK) {
        row = malloc((size_t)j->w * 4);
        if (row == NULL) {
            (void)snprintf(e.msg, sizeof e.msg, "out of memory");
            longjmp(e.jb, 1);
        }
    }
    while (c.next_scanline < c.image_height) {
        const uint8_t *src = j->px + (size_t)c.next_scanline * j->w * ch;
        JSAMPROW r;
        if (row != NULL) {
            size_t k;
            /* Adobe-style CMYK: stored inverted (255 = no ink). */
            for (k = 0; k < (size_t)j->w * 4; k++) {
                row[k] = (uint8_t)(255 - src[k]);
            }
            r = row;
        } else {
            r = (JSAMPROW)(uintptr_t)src;
        }
        (void)jpeg_write_scanlines(&c, &r, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    if (d->failed) {
        j->failed = true;
        (void)snprintf(j->msg, sizeof j->msg, "out of memory");
    }
    free(row);
    free(d);
}

pc_status cx_jpeg_encode(const pc_cimage *src, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    enc_job j;
    pc_buf exif = PC_BUF_INIT;
    pc_buf xmp = PC_BUF_INIT;
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    memset(&j, 0, sizeof j);
    if (img->width > 65535 || img->height > 65535) {
        st = cx_encode(err, F, "JPEG dimensions are limited to 65535");
        goto done;
    }
    if (img->layout != PC_CL_GRAY && img->layout != PC_CL_RGB && img->layout != PC_CL_CMYK) {
        st = cx_encode(err, F, "unsupported layout %s", pc_clayout_name(img->layout));
        goto done;
    }
    j.px = img->data;
    j.w = img->width;
    j.h = img->height;
    j.layout = img->layout;
    j.quality = o->jpeg_quality < 1 ? 1 : o->jpeg_quality > 100 ? 100 : o->jpeg_quality;
    /* CMYK is always written 4:4:4: subsampled 4-component JPEGs decode inconsistently. */
    j.subsample = o->jpeg_chroma_subsampling;
    j.out = out;
    if (o->embed_metadata) {
        if (img->meta.has_dpi && img->meta.dpi_x >= 1.0f && img->meta.dpi_y >= 1.0f) {
            j.has_density = true;
            j.dx = pc_f32_to_u16(pc_rs_minf_k(roundf(img->meta.dpi_x), 65535.0f));
            j.dy = pc_f32_to_u16(pc_rs_minf_k(roundf(img->meta.dpi_y), 65535.0f));
        }
        /* Metadata too large for its one segment is left out (the fidelity warnings say so). */
        if (img->meta.exif.has && cx_jpeg_exif_segment(img->meta.exif.data, img->meta.exif.len, &exif)) {
            j.app1[0] = &exif;
        }
        if (img->meta.xmp.has && cx_jpeg_xmp_segment(img->meta.xmp.data, img->meta.xmp.len, &xmp)) {
            j.app1[1] = &xmp;
        }
    }
    if (o->embed_icc && img->icc.has && img->icc.len > 0) {
        j.icc = img->icc.data;
        j.icc_len = img->icc.len;
    }
    run_encode(&j);
    if (j.failed) {
        st = cx_encode(err, F, "%s", j.msg);
    }
done:
    pc_buf_fini(&exif);
    pc_buf_fini(&xmp);
    pc_cimage_fini(&tmp);
    return st;
}

/* Save for Web (web.c): baseline or progressive 8-bit RGB with optional ICC, density and XMP. */
pc_status cx_jpeg_encode_rgb8(uint32_t w, uint32_t h, const uint8_t *rgb, int quality, bool progressive,
                              bool optimized, const uint8_t *icc, size_t icc_len, bool has_dpi, float dpi,
                              const char *xmp, size_t xmp_len, pc_buf *out, pc_error *err);
pc_status cx_jpeg_encode_rgb8(uint32_t w, uint32_t h, const uint8_t *rgb, int quality, bool progressive,
                              bool optimized, const uint8_t *icc, size_t icc_len, bool has_dpi, float dpi,
                              const char *xmp, size_t xmp_len, pc_buf *out, pc_error *err)
{
    enc_job j;
    pc_buf seg = PC_BUF_INIT;
    pc_status st = PC_OK;
    memset(&j, 0, sizeof j);
    j.px = rgb;
    j.w = w;
    j.h = h;
    j.layout = PC_CL_RGB;
    j.quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
    j.subsample = j.quality < 90;
    j.progressive = progressive;
    j.optimize = progressive || optimized;
    j.out = out;
    if (has_dpi && dpi >= 1.0f) {
        j.has_density = true;
        j.dx = j.dy = pc_f32_to_u16(pc_rs_minf_k(roundf(dpi), 65535.0f));
    }
    if (xmp != NULL) {
        if (pc_buf_append(&seg, XMP_HEADER, XMP_HEADER_LEN, err) != PC_OK ||
            pc_buf_append(&seg, xmp, xmp_len, err) != PC_OK) {
            pc_buf_fini(&seg);
            return PC_ERR_NOMEM;
        }
        if (seg.len > MAX_SEGMENT) {
            pc_buf_fini(&seg);
            return cx_encode(err, F, "XMP is too large for one segment");
        }
        j.app1[0] = &seg;
    }
    j.icc = icc;
    j.icc_len = icc_len;
    run_encode(&j);
    if (j.failed) {
        st = cx_encode(err, F, "%s", j.msg);
    }
    pc_buf_fini(&seg);
    return st;
}
