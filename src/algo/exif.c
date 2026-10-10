/* Minimal EXIF reader / writer (port of crates/algo/src/exif.rs). */
#include "algo/exif.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "base/utf8.h"

typedef struct tiff {
    const uint8_t *b;
    size_t len;
    bool le;
} tiff;

static bool t_u16(const tiff *t, size_t o, uint16_t *v)
{
    if (o > t->len || t->len - o < 2) {
        return false;
    }
    *v = t->le ? pc_load_le16(t->b + o) : pc_load_be16(t->b + o);
    return true;
}

static bool t_u32(const tiff *t, size_t o, uint32_t *v)
{
    if (o > t->len || t->len - o < 4) {
        return false;
    }
    *v = t->le ? pc_load_le32(t->b + o) : pc_load_be32(t->b + o);
    return true;
}

/* Where an entry's value lives: inline in the 4-byte field, or at the offset it holds. */
static bool data_at(const tiff *t, uint16_t typ, uint32_t count, size_t field, size_t *at)
{
    uint64_t unit, size;
    uint32_t off;
    switch (typ) {
    case 1: case 2: case 6: case 7: unit = 1; break;
    case 3: case 8: unit = 2; break;
    case 4: case 9: case 11: unit = 4; break;
    case 5: case 10: case 12: unit = 8; break;
    default: return false;
    }
    size = unit * count;
    if (size <= 4) {
        *at = field;
        return true;
    }
    if (!t_u32(t, field, &off)) {
        return false;
    }
    *at = off;
    return true;
}

static pc_algo_exif_num number(const tiff *t, uint16_t typ, uint32_t count, size_t field)
{
    pc_algo_exif_num r = {false, 0.0};
    size_t at;
    uint16_t s;
    uint32_t n, d;
    if (!data_at(t, typ, count, field, &at)) {
        return r;
    }
    switch (typ) {
    case 3:
        if (t_u16(t, at, &s)) {
            r.set = true;
            r.v = (double)s;
        }
        break;
    case 4:
        if (t_u32(t, at, &n)) {
            r.set = true;
            r.v = (double)n;
        }
        break;
    case 5:
        if (at <= SIZE_MAX - 4 && t_u32(t, at, &n) && t_u32(t, at + 4, &d) && d != 0) {
            r.set = true;
            r.v = (double)n / (double)d;
        }
        break;
    case 10:
        if (at <= SIZE_MAX - 4 && t_u32(t, at, &n) && t_u32(t, at + 4, &d) && d != 0) {
            r.set = true;
            r.v = (double)pc_u32_as_i32(n) / (double)pc_u32_as_i32(d);
        }
        break;
    default:
        break;
    }
    return r;
}

/* ASCII value: lossy UTF-8, trailing NULs then Unicode white space trimmed; unset when empty. */
static pc_status text(const tiff *t, uint16_t typ, uint32_t count, size_t field, pc_algo_exif_str *out,
                      pc_error *err)
{
    size_t at, start, n;
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    out->s = NULL;
    out->len = 0;
    if (typ != 2 || !data_at(t, typ, count, field, &at) || at > t->len || t->len - at < count) {
        return PC_OK;
    }
    st = pc_utf8_lossy(t->b + at, count, &b, err);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    n = b.len;
    while (n > 0 && b.data[n - 1] == 0) {
        n--;
    }
    n = n > 0 ? pc_utf8_trim((const char *)b.data, n, &start) : 0;
    if (n == 0) {
        pc_buf_fini(&b);
        return PC_OK;
    }
    out->s = pc_strndup((const char *)b.data + start, n);
    pc_buf_fini(&b);
    if (out->s == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for an EXIF string");
    }
    out->len = n;
    return PC_OK;
}

static void str_free(pc_algo_exif_str *s)
{
    free(s->s);
    s->s = NULL;
    s->len = 0;
}

void pc_algo_camera_info_fini(pc_algo_camera_info *info)
{
    if (info != NULL) {
        str_free(&info->make);
        str_free(&info->model);
        str_free(&info->lens);
        memset(info, 0, sizeof *info);
    }
}

static bool str_eq(const pc_algo_exif_str *a, const pc_algo_exif_str *b)
{
    if (a->s == NULL || b->s == NULL) {
        return a->s == b->s;
    }
    return a->len == b->len && memcmp(a->s, b->s, a->len) == 0;
}

static bool num_eq(pc_algo_exif_num a, pc_algo_exif_num b)
{
    return a.set == b.set && (!a.set || a.v == b.v);
}

bool pc_algo_camera_info_equal(const pc_algo_camera_info *a, const pc_algo_camera_info *b)
{
    return str_eq(&a->make, &b->make) && str_eq(&a->model, &b->model) && str_eq(&a->lens, &b->lens) &&
           num_eq(a->exposure_time, b->exposure_time) && num_eq(a->f_number, b->f_number) &&
           num_eq(a->iso, b->iso) && num_eq(a->focal_length, b->focal_length) &&
           num_eq(a->focal_length_35mm, b->focal_length_35mm);
}

bool pc_algo_exif_exposure_factor(const pc_algo_camera_info *info, double *out)
{
    double n;
    if (!info->exposure_time.set) {
        return false;
    }
    n = pc_rs_max_k(info->f_number.set ? info->f_number.v : 1.0, 0.1);
    *out = info->exposure_time.v * (info->iso.set ? info->iso.v : 100.0) / 100.0 / (n * n);
    return true;
}

/* Replaces a string field (the reference assigns, so a later unreadable tag clears it). */
static pc_status set_text(const tiff *t, uint16_t typ, uint32_t count, size_t field, pc_algo_exif_str *dst,
                          pc_error *err)
{
    pc_algo_exif_str s;
    pc_status st = text(t, typ, count, field, &s, err);
    if (st != PC_OK) {
        return st;
    }
    str_free(dst);
    *dst = s;
    return PC_OK;
}

pc_status pc_algo_exif_read(const uint8_t *exif, size_t len, pc_algo_camera_info *out, pc_error *err)
{
    pc_algo_camera_info info;
    tiff t;
    uint32_t ifd0;
    size_t *queue = NULL, nq = 0, capq = 0, seen[9], nseen = 0;
    pc_status st = PC_OK;
    memset(&info, 0, sizeof info);
    if (len >= 6 && memcmp(exif, "Exif\0\0", 6) == 0) {
        exif += 6;
        len -= 6;
    }
    if (len < 4) {
        *out = info;
        return PC_OK;
    }
    if (memcmp(exif, "II*\0", 4) == 0) {
        t.le = true;
    } else if (memcmp(exif, "MM\0*", 4) == 0) {
        t.le = false;
    } else {
        *out = info;
        return PC_OK;
    }
    t.b = exif;
    t.len = len;
    if (!t_u32(&t, 4, &ifd0)) {
        *out = info;
        return PC_OK;
    }
    queue = pc_grow_array(NULL, &capq, 16, sizeof *queue);
    if (queue == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading EXIF");
    }
    queue[nq++] = ifd0;
    while (nq > 0 && st == PC_OK) {
        size_t off = queue[--nq], k;
        uint16_t n;
        bool dup = false;
        for (k = 0; k < nseen; k++) {
            dup = dup || seen[k] == off;
        }
        /* seen.len() > 8: the reference stops at nine visited IFDs (cyclic or hostile chains). */
        if (dup || nseen > 8) {
            continue;
        }
        seen[nseen++] = off;
        /* off + 2 + 12 * 65535 must not wrap (32-bit size_t). */
        if (off > SIZE_MAX - 2 - 12 * 65535u || !t_u16(&t, off, &n)) {
            continue;
        }
        for (k = 0; k < n && st == PC_OK; k++) {
            size_t e = off + 2 + k * 12;
            uint16_t tag, typ;
            uint32_t count, p;
            if (!t_u16(&t, e, &tag) || !t_u16(&t, e + 2, &typ) || !t_u32(&t, e + 4, &count)) {
                continue;
            }
            size_t field = e + 8;
            switch (tag) {
            case 0x010F: st = set_text(&t, typ, count, field, &info.make, err); break;
            case 0x0110: st = set_text(&t, typ, count, field, &info.model, err); break;
            case 0x829A: info.exposure_time = number(&t, typ, count, field); break;
            case 0x829D: info.f_number = number(&t, typ, count, field); break;
            case 0x8827: info.iso = number(&t, typ, count, field); break;
            case 0x920A: info.focal_length = number(&t, typ, count, field); break;
            case 0xA405:
                info.focal_length_35mm = number(&t, typ, count, field);
                if (!(info.focal_length_35mm.v > 0.0)) {
                    info.focal_length_35mm.set = false;
                    info.focal_length_35mm.v = 0.0;
                }
                break;
            case 0xA434: st = set_text(&t, typ, count, field, &info.lens, err); break;
            case 0x8769:
                if (t_u32(&t, field, &p)) {
                    if (nq + 1 > capq) {
                        size_t *q2 = pc_grow_array(queue, &capq, nq + 1, sizeof *queue);
                        if (q2 == NULL) {
                            st = pc_fail(err, PC_ERR_NOMEM, "out of memory reading EXIF");
                            break;
                        }
                        queue = q2;
                    }
                    queue[nq++] = p;
                }
                break;
            default:
                break;
            }
        }
    }
    free(queue);
    if (st != PC_OK) {
        pc_algo_camera_info_fini(&info);
        return st;
    }
    *out = info;
    return PC_OK;
}

/* ---- build ---- */

typedef struct entry {
    uint16_t tag, typ;
    uint32_t count;
    uint8_t inl[8]; /* small values */
    const uint8_t *bytes;
    size_t nbytes;
} entry;

static void rational(double v, uint8_t out[8])
{
    pc_store_le32(out, pc_f64_to_u32(round(v * 10000.0)));
    pc_store_le32(out + 4, 10000u);
}

static bool text_entry(entry *e, uint16_t tag, const pc_algo_exif_str *s)
{
    /* The value is the string plus its NUL terminator (pc_algo_exif_str keeps one). */
    if (s->len >= UINT32_MAX) {
        return false;
    }
    e->tag = tag;
    e->typ = 2;
    e->count = (uint32_t)s->len + 1;
    e->bytes = (const uint8_t *)s->s;
    e->nbytes = s->len + 1;
    return true;
}

static void num_entry(entry *e, uint16_t tag, uint16_t typ, double v)
{
    e->tag = tag;
    e->typ = typ;
    e->count = 1;
    memset(e->inl, 0, sizeof e->inl);
    if (typ == 5) {
        rational(v, e->inl);
        e->nbytes = 8;
    } else {
        pc_store_le16(e->inl, pc_f64_to_u16(v));
        e->nbytes = 2;
    }
    e->bytes = e->inl;
}

static pc_status write_ifd(pc_buf *out, pc_buf *data, size_t *data_off, const entry *es, size_t n,
                           bool extra, uint32_t extra_ptr, pc_error *err)
{
    size_t k;
    pc_status st = pc_buf_put_le16(out, (uint16_t)(n + (extra ? 1u : 0u)), err);
    for (k = 0; k < n && st == PC_OK; k++) {
        const entry *e = &es[k];
        st = pc_buf_put_le16(out, e->tag, err);
        if (st == PC_OK) { st = pc_buf_put_le16(out, e->typ, err); }
        if (st == PC_OK) { st = pc_buf_put_le32(out, e->count, err); }
        if (st != PC_OK) {
            break;
        }
        if (e->nbytes <= 4) {
            uint8_t f[4] = {0, 0, 0, 0};
            memcpy(f, e->bytes, e->nbytes);
            st = pc_buf_append(out, f, 4, err);
        } else {
            if (*data_off > UINT32_MAX) {
                return pc_fail(err, PC_ERR_LIMIT, "EXIF payload too large");
            }
            st = pc_buf_put_le32(out, (uint32_t)*data_off, err);
            if (st == PC_OK) { st = pc_buf_append(data, e->bytes, e->nbytes, err); }
            *data_off += e->nbytes;
        }
    }
    if (st == PC_OK && extra) {
        st = pc_buf_put_le16(out, 0x8769, err);
        if (st == PC_OK) { st = pc_buf_put_le16(out, 4, err); }
        if (st == PC_OK) { st = pc_buf_put_le32(out, 1, err); }
        if (st == PC_OK) { st = pc_buf_put_le32(out, extra_ptr, err); }
    }
    if (st == PC_OK) {
        st = pc_buf_put_le32(out, 0, err);
    }
    return st;
}

pc_status pc_algo_exif_build(const pc_algo_camera_info *info, pc_buf *out, pc_error *err)
{
    entry ifd0[2], ex[6];
    size_t n0 = 0, ne = 0, exif_off, data_off;
    pc_buf body = PC_BUF_INIT, data = PC_BUF_INIT;
    pc_status st;
    if (info->make.s != NULL && !text_entry(&ifd0[n0++], 0x010F, &info->make)) {
        return pc_fail(err, PC_ERR_LIMIT, "EXIF string too long");
    }
    if (info->model.s != NULL && !text_entry(&ifd0[n0++], 0x0110, &info->model)) {
        return pc_fail(err, PC_ERR_LIMIT, "EXIF string too long");
    }
    if (info->exposure_time.set) { num_entry(&ex[ne++], 0x829A, 5, info->exposure_time.v); }
    if (info->f_number.set) { num_entry(&ex[ne++], 0x829D, 5, info->f_number.v); }
    if (info->iso.set) { num_entry(&ex[ne++], 0x8827, 3, info->iso.v); }
    if (info->focal_length.set) { num_entry(&ex[ne++], 0x920A, 5, info->focal_length.v); }
    if (info->focal_length_35mm.set) { num_entry(&ex[ne++], 0xA405, 3, info->focal_length_35mm.v); }
    if (info->lens.s != NULL && !text_entry(&ex[ne++], 0xA434, &info->lens)) {
        return pc_fail(err, PC_ERR_LIMIT, "EXIF string too long");
    }
    /* IFD0 at 8 (its entries + the Exif pointer), the Exif IFD after it, then the data area. */
    exif_off = 8 + 2 + 12 * (n0 + 1) + 4;
    data_off = exif_off + 2 + 12 * ne + 4;
    st = pc_buf_append(&body, "II*\0", 4, err);
    if (st == PC_OK) { st = pc_buf_put_le32(&body, 8, err); }
    if (st == PC_OK) { st = write_ifd(&body, &data, &data_off, ifd0, n0, true, (uint32_t)exif_off, err); }
    if (st == PC_OK) { st = write_ifd(&body, &data, &data_off, ex, ne, false, 0, err); }
    if (st == PC_OK && data.len > 0) { st = pc_buf_append(&body, data.data, data.len, err); }
    if (st == PC_OK) { st = pc_buf_append(out, body.data, body.len, err); }
    pc_buf_fini(&body);
    pc_buf_fini(&data);
    return st;
}
