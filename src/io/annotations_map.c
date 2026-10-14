/* Notes and Image > Analysis data <-> PSD (port of crates/io/src/annotations_map.rs).
 *
 *  - Notes: the global block Anno ("Annotations (Photoshop 6.0)" in the Adobe PSD spec): u16
 *    major version 2, u16 minor 1, u32 count, then per annotation a u32 length (including
 *    itself), type txtA (text) or sndA (sound), open flag u8, flags u8, optional blocks u16, icon
 *    and popup rects (four i32 each: top, left, bottom, right), a colour (u16 space + four u16),
 *    author, name and modification date as even-padded Pascal strings, then a u32 length
 *    (including itself) of the data block: txtC / sndM, u32 size and the data. Text is UTF-16BE
 *    with a byte-order mark, lines separated by CR. Sound annotations are not modelled; they are
 *    carried over verbatim when notes are rewritten.
 *  - Measurement scale: image resource 1074 (a version-16 descriptor with pixelLength,
 *    logicalLength, logicalUnits; read leniently, kept byte-exact while unchanged).
 *  - Count information: resource 1080 is kept verbatim unless the document has its own count
 *    markers (only .pcraft stores them). */
#include "io/annotations_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "io/core.h"
#include "io/maps_desc.h"

#define COUNT_INFO 1080

static const uint8_t ANNO[4] = {'A', 'n', 'n', 'o'};

/* ---- reading ---- */

typedef struct rd {
    const uint8_t *d;
    size_t len;
    size_t p;
} rd;

static const uint8_t *take(rd *r, size_t n)
{
    const uint8_t *s;
    if (r->p > r->len || n > r->len - r->p) {
        return NULL;
    }
    s = r->d + r->p;
    r->p += n;
    return s;
}

static bool u8_(rd *r, uint8_t *v)
{
    const uint8_t *b = take(r, 1);
    if (b != NULL) {
        *v = b[0];
    }
    return b != NULL;
}

static bool u16_(rd *r, uint16_t *v)
{
    const uint8_t *b = take(r, 2);
    if (b != NULL) {
        *v = (uint16_t)((unsigned)b[0] << 8 | b[1]);
    }
    return b != NULL;
}

static bool u32_(rd *r, uint32_t *v)
{
    const uint8_t *b = take(r, 4);
    if (b != NULL) {
        *v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    }
    return b != NULL;
}

/* Latin-1 bytes -> UTF-8 into *out (appended). */
static pc_status latin1(const uint8_t *s, size_t n, pc_buf *out, pc_error *err)
{
    pc_status st = PC_OK;
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        st = pc_utf8_put(out, s[i], err);
    }
    return st;
}

static pc_status set_from_buf(pc_str *dst, pc_buf *b)
{
    size_t n = 0;
    char *p = (char *)pc_buf_take(b, &n);
    pc_str_fini(dst);
    *dst = pc_str_adopt(p, n);
    return PC_OK;
}

/* Pascal string, even-padded; *ok false when cut off. */
static pc_status pascal(rd *r, bool *ok, pc_str *out, pc_error *err)
{
    uint8_t n;
    const uint8_t *s;
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    *ok = false;
    if (!u8_(r, &n) || (s = take(r, n)) == NULL || ((n + 1) % 2 == 1 && take(r, 1) == NULL)) {
        return PC_OK;
    }
    st = latin1(s, n, &b, err);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    *ok = true;
    return set_from_buf(out, &b);
}

static float f16(uint16_t v)
{
    return (float)v / 65535.0f;
}

/* PSD colour struct (space + 4 x u16) -> RGB. */
static pc_color color_from(uint16_t space, const uint16_t c[4])
{
    switch (space) {
    case 1: { /* HSB: hue 0..65535 maps to 0..360 degrees */
        float h = f16(c[0]) * 6.0f;
        float s = f16(c[1]);
        float v = f16(c[2]);
        float fl = floorf(h);
        int32_t i = pc_f32_to_i32(fl) % 6;
        float fr = h - fl;
        float p = v * (1.0f - s);
        float q = v * (1.0f - s * fr);
        float t = v * (1.0f - s * (1.0f - fr));
        if (i < 0) {
            i += 6;
        }
        switch (i) {
        case 0:
            return pc_color_rgb(v, t, p);
        case 1:
            return pc_color_rgb(q, v, p);
        case 2:
            return pc_color_rgb(p, v, t);
        case 3:
            return pc_color_rgb(p, q, v);
        case 4:
            return pc_color_rgb(t, p, v);
        default:
            return pc_color_rgb(v, p, q);
        }
    }
    case 2: { /* CMYK (inverted: 0 = 100 % ink), naive */
        float k = 1.0f - f16(c[3]);
        return pc_color_rgb(f16(c[0]) * (1.0f - k), f16(c[1]) * (1.0f - k), f16(c[2]) * (1.0f - k));
    }
    case 8: { /* grayscale 0..10000 */
        float g = 1.0f - pc_clampf((float)c[0] / 10000.0f, 0.0f, 1.0f);
        return pc_color_rgb(g, g, g);
    }
    default:
        return pc_color_rgb(f16(c[0]), f16(c[1]), f16(c[2]));
    }
}

/* Note text: UTF-16BE after a BOM, else Latin-1; trailing NULs dropped, CR LF and CR -> LF. */
static pc_status decode_text(const uint8_t *data, size_t n, pc_str *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_buf c = PC_BUF_INIT;
    size_t end;
    pc_status st;
    if (n >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        st = pc_utf16be_lossy(data + 2, (n - 2) / 2, &b, err);
    } else {
        st = latin1(data, n, &b, err);
    }
    end = b.len;
    while (end > 0 && b.data[end - 1] == 0) {
        end--;
    }
    for (size_t i = 0; i < end && st == PC_OK; i++) {
        if (b.data[i] == '\r') {
            if (i + 1 < end && b.data[i + 1] == '\n') {
                i++;
            }
            st = pc_buf_append_byte(&c, '\n', err);
        } else {
            st = pc_buf_append_byte(&c, b.data[i], err);
        }
    }
    pc_buf_fini(&b);
    if (st != PC_OK) {
        pc_buf_fini(&c);
        return st;
    }
    return set_from_buf(out, &c);
}

/* One parsed Anno entry: a note, or a raw entry we keep (sound annotations). */
typedef struct entry {
    bool is_text;
    pc_note note;
    const uint8_t *raw; /* borrowed from the block */
    size_t raw_len;
} entry;

static void entries_free(entry *e, size_t n)
{
    for (size_t i = 0; e != NULL && i < n; i++) {
        if (e[i].is_text) {
            pc_note_fini(&e[i].note);
        }
    }
    free(e);
}

/* One txtA entry after its type; *ok false when cut off. */
static pc_status parse_note(rd *r, bool *ok, pc_note *n, pc_error *err)
{
    uint8_t open;
    uint8_t flags;
    uint16_t opt;
    uint16_t space;
    uint16_t c[4];
    uint32_t v[8];
    uint32_t data_len;
    uint32_t tn;
    const uint8_t *text;
    bool got;
    pc_str name = PC_STR_INIT;
    pc_status st;
    *ok = false;
    pc_note_default(n);
    if (!u8_(r, &open) || !u8_(r, &flags) || !u16_(r, &opt)) {
        return PC_OK;
    }
    for (size_t i = 0; i < 8; i++) {
        if (!u32_(r, &v[i])) {
            return PC_OK;
        }
    }
    if (!u16_(r, &space) || !u16_(r, &c[0]) || !u16_(r, &c[1]) || !u16_(r, &c[2]) || !u16_(r, &c[3])) {
        return PC_OK;
    }
    st = pascal(r, &got, &n->author, err);
    if (st == PC_OK && got) {
        st = pascal(r, &got, &name, err);
        pc_str_fini(&name);
    }
    if (st == PC_OK && got) {
        st = pascal(r, &got, &n->modified, err);
    }
    if (st != PC_OK || !got || !u32_(r, &data_len) || take(r, 4) == NULL || !u32_(r, &tn) ||
        (text = take(r, tn)) == NULL) {
        pc_note_fini(n);
        return st;
    }
    st = decode_text(text, tn, &n->text, err);
    if (st != PC_OK) {
        pc_note_fini(n);
        return st;
    }
    n->color = color_from(space, c);
    n->position[0] = (double)pc_u32_as_i32(v[1]);
    n->position[1] = (double)pc_u32_as_i32(v[0]);
    n->popup[0] = (double)pc_u32_as_i32(v[5]);
    n->popup[1] = (double)pc_u32_as_i32(v[4]);
    n->popup[2] = (double)pc_u32_as_i32(v[7]);
    n->popup[3] = (double)pc_u32_as_i32(v[6]);
    n->open = open != 0;
    *ok = true;
    return PC_OK;
}

/* parse_entries; *found false when the block doesn't parse. A count above the block's length
 * is unreadable here (the reference would try to allocate that many entries). */
static pc_status parse_entries(const uint8_t *data, size_t len, bool *found, entry **out, size_t *nout,
                               pc_error *err)
{
    rd r = {data, len, 0};
    uint16_t major;
    uint16_t minor;
    uint32_t count;
    entry *v = NULL;
    size_t n = 0;
    *found = false;
    if (!u16_(&r, &major) || !u16_(&r, &minor) || !u32_(&r, &count)) {
        return PC_OK;
    }
    if ((uint64_t)count > (uint64_t)len) {
        return PC_OK;
    }
    if (count > 0) {
        v = pc_calloc_array(count, sizeof *v);
        if (v == NULL) {
            return pc_io_nomem(err, "reading notes");
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        size_t start = r.p;
        uint32_t elen;
        size_t end;
        const uint8_t *kind;
        /* The length counts itself and the type: anything shorter (< 8) would put the cursor
         * back on this entry, and a hostile count would then loop until memory runs out. */
        if (!u32_(&r, &elen) || elen < 8 || elen > len || start > len - elen) {
            entries_free(v, n);
            return PC_OK;
        }
        end = start + elen;
        kind = take(&r, 4);
        if (kind == NULL) {
            entries_free(v, n);
            return PC_OK;
        }
        if (memcmp(kind, "txtA", 4) != 0) {
            v[n].raw = data + start;
            v[n].raw_len = elen;
            n++;
            r.p = end;
            continue;
        }
        {
            bool ok = false;
            pc_status st = parse_note(&r, &ok, &v[n].note, err);
            if (st != PC_OK || !ok) {
                entries_free(v, n);
                return st;
            }
            v[n].is_text = true;
            n++;
        }
        r.p = end;
    }
    *out = v;
    *nout = n;
    *found = true;
    return PC_OK;
}

/* The notes of the entries (cloned). */
static pc_status notes_of(const entry *e, size_t n, pc_note **out, size_t *nout, pc_error *err)
{
    pc_note *v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
    size_t k = 0;
    if (v == NULL) {
        return pc_io_nomem(err, "reading notes");
    }
    for (size_t i = 0; i < n; i++) {
        if (e[i].is_text) {
            pc_status st = pc_note_clone(&e[i].note, &v[k], err);
            if (st != PC_OK) {
                for (size_t j = 0; j < k; j++) {
                    pc_note_fini(&v[j]);
                }
                free(v);
                return st;
            }
            k++;
        }
    }
    *out = v;
    *nout = k;
    return PC_OK;
}

pc_status pc_io_parse_anno(const uint8_t *data, size_t len, bool *found, pc_note **out, size_t *n,
                           pc_error *err)
{
    entry *e = NULL;
    size_t ne = 0;
    bool ok = false;
    pc_status st = parse_entries(data, len, &ok, &e, &ne, err);
    if (st != PC_OK || !ok) {
        if (st == PC_OK) {
            *found = false;
        }
        return st;
    }
    st = notes_of(e, ne, out, n, err);
    entries_free(e, ne);
    if (st == PC_OK) {
        *found = true;
    }
    return st;
}

/* ---- writing ---- */

static pc_status put16(pc_buf *b, uint16_t v, pc_error *err)
{
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return pc_buf_append(b, x, 2, err);
}

static pc_status put32(pc_buf *b, uint32_t v, pc_error *err)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return pc_buf_append(b, x, 4, err);
}

/* Pascal string: code points below 256 as bytes, others '?', at most 255, even-padded. */
static pc_status put_pascal(pc_buf *b, const pc_str *s, pc_error *err)
{
    uint8_t bytes[256];
    size_t n = 0;
    size_t i = 0;
    pc_status st;
    while (i < s->len && n < 255) {
        uint32_t cp = 0xFFFD;
        if (!pc_utf8_next((const uint8_t *)s->ptr, s->len, &i, &cp)) {
            i++; /* not reached for valid UTF-8 */
        }
        bytes[1 + n++] = cp < 256 ? (uint8_t)cp : (uint8_t)'?';
    }
    bytes[0] = (uint8_t)n;
    st = pc_buf_append(b, bytes, n + 1, err);
    if (st == PC_OK && (n + 1) % 2 == 1) {
        st = pc_buf_append_byte(b, 0, err);
    }
    return st;
}

/* UTF-16BE with a BOM; LF (and CR LF) written as CR. */
static pc_status encode_text(const pc_str *s, pc_buf *out, pc_error *err)
{
    size_t i = 0;
    pc_status st = put16(out, 0xFEFF, err);
    while (st == PC_OK && i < s->len) {
        uint32_t cp = 0xFFFD;
        if (!pc_utf8_next((const uint8_t *)s->ptr, s->len, &i, &cp)) {
            i++;
        }
        if (cp == '\r' && i < s->len && s->ptr[i] == '\n') {
            continue; /* CR LF -> LF, written below as CR */
        }
        if (cp == '\n') {
            cp = '\r';
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            st = put16(out, (uint16_t)(0xD800 + (cp >> 10)), err);
            if (st == PC_OK) {
                st = put16(out, (uint16_t)(0xDC00 + (cp & 0x3FF)), err);
            }
        } else {
            st = put16(out, (uint16_t)cp, err);
        }
    }
    return st;
}

static uint32_t i32_bits(int32_t v)
{
    return (uint32_t)(int64_t)v;
}

static pc_status write_note(const pc_note *n, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_buf text = PC_BUF_INIT;
    int32_t x = pc_f64_to_i32(round(n->position[0]));
    int32_t y = pc_f64_to_i32(round(n->position[1]));
    double pp[4];
    float rgb[3];
    pc_str empty = PC_STR_INIT;
    pc_status st = pc_buf_append(&b, "txtA", 4, err);
    bool zero = n->popup[0] == 0.0 && n->popup[1] == 0.0 && n->popup[2] == 0.0 && n->popup[3] == 0.0;
    if (st == PC_OK) {
        st = pc_buf_append_byte(&b, n->open ? 1 : 0, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append_byte(&b, 0x1C, err);
    }
    if (st == PC_OK) {
        st = put16(&b, 1, err);
    }
    /* The position can sit anywhere in i32 (issue #939): the icon's far edge saturates. */
    if (st == PC_OK) {
        int32_t icon[4] = {y, x, pc_i32_sat_add(y, 20), pc_i32_sat_add(x, 16)};
        for (size_t i = 0; i < 4 && st == PC_OK; i++) {
            st = put32(&b, i32_bits(icon[i]), err);
        }
    }
    if (zero) {
        pp[0] = n->position[0] + 20.0;
        pp[1] = n->position[1] + 20.0;
        pp[2] = n->position[0] + 260.0;
        pp[3] = n->position[1] + 160.0;
    } else {
        memcpy(pp, n->popup, sizeof pp);
    }
    {
        static const size_t order[4] = {1, 0, 3, 2};
        for (size_t i = 0; i < 4 && st == PC_OK; i++) {
            st = put32(&b, i32_bits(pc_f64_to_i32(round(pp[order[i]]))), err);
        }
    }
    if (st == PC_OK) {
        st = put16(&b, 0, err);
    }
    pc_color_to_rgb(&n->color, rgb);
    for (size_t i = 0; i < 3 && st == PC_OK; i++) {
        st = put16(&b, pc_f32_to_u16(roundf(pc_clampf(rgb[i], 0.0f, 1.0f) * 65535.0f)), err);
    }
    if (st == PC_OK) {
        st = put16(&b, 0, err);
    }
    if (st == PC_OK) {
        st = put_pascal(&b, &n->author, err);
    }
    if (st == PC_OK) {
        st = put_pascal(&b, &empty, err);
    }
    if (st == PC_OK) {
        st = put_pascal(&b, &n->modified, err);
    }
    if (st == PC_OK) {
        st = encode_text(&n->text, &text, err);
    }
    if (st == PC_OK) {
        st = put32(&b, (uint32_t)(text.len + 12), err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(&b, "txtC", 4, err);
    }
    if (st == PC_OK) {
        st = put32(&b, (uint32_t)text.len, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(&b, text.data, text.len, err);
    }
    if (st == PC_OK) {
        st = put32(out, (uint32_t)(b.len + 4), err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(out, b.data, b.len, err);
    }
    pc_buf_fini(&b);
    pc_buf_fini(&text);
    return st;
}

pc_status pc_io_write_anno(const pc_note *notes, size_t n, const pc_io_byte_span *extra, size_t nextra,
                           pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status st = put16(&b, 2, err);
    if (st == PC_OK) {
        st = put16(&b, 1, err);
    }
    if (st == PC_OK) {
        st = put32(&b, (uint32_t)(n + nextra), err);
    }
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        st = write_note(&notes[i], &b, err);
    }
    for (size_t i = 0; i < nextra && st == PC_OK; i++) {
        st = pc_buf_append(&b, extra[i].data, extra[i].len, err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

/* ---- document level ---- */

static const pc_doc_psd_global_block *anno_block(const pc_document *doc)
{
    for (size_t i = 0; i < doc->metadata.npsd_global_blocks; i++) {
        if (memcmp(doc->metadata.psd_global_blocks[i].key, ANNO, 4) == 0) {
            return &doc->metadata.psd_global_blocks[i];
        }
    }
    return NULL;
}

pc_status pc_io_notes_from_blocks(const pc_document *doc, pc_note **out, size_t *n, pc_error *err)
{
    const pc_doc_psd_global_block *b = anno_block(doc);
    bool ok = false;
    pc_status st;
    if (b != NULL) {
        st = pc_io_parse_anno(pc_blob_data(b->data), pc_blob_len(b->data), &ok, out, n, err);
        if (st != PC_OK || ok) {
            return st;
        }
    }
    *out = NULL;
    *n = 0;
    return PC_OK;
}

bool pc_io_anno_unreadable(const pc_document *doc)
{
    const pc_doc_psd_global_block *b = anno_block(doc);
    entry *e = NULL;
    size_t n = 0;
    bool ok = true;
    if (b == NULL) {
        return false;
    }
    /* Out of memory counts as readable: the notes are then only missing from the panel. */
    if (parse_entries(pc_blob_data(b->data), pc_blob_len(b->data), &ok, &e, &n, NULL) != PC_OK) {
        return false;
    }
    if (ok) {
        entries_free(e, n);
    }
    return !ok;
}

/* Notes compared in stored precision (colours 16-bit, positions whole pixels). */
static bool same_notes(const pc_note *a, size_t na, const pc_note *b, size_t nb)
{
    if (na != nb) {
        return false;
    }
    for (size_t i = 0; i < na; i++) {
        const pc_note *x = &a[i];
        const pc_note *y = &b[i];
        float cx[3];
        float cy[3];
        if (!pc_str_eq(&x->author, &y->author) || !pc_str_eq(&x->text, &y->text) ||
            x->position[0] != y->position[0] || x->position[1] != y->position[1] || x->open != y->open ||
            !pc_str_eq(&x->modified, &y->modified)) {
            return false;
        }
        for (size_t k = 0; k < 4; k++) {
            if (x->popup[k] != y->popup[k]) {
                return false;
            }
        }
        pc_color_to_rgb(&x->color, cx);
        pc_color_to_rgb(&y->color, cy);
        for (size_t k = 0; k < 3; k++) {
            if (!(fabsf(cx[k] - cy[k]) < 1.0f / 512.0f)) {
                return false;
            }
        }
    }
    return true;
}

static void notes_free(pc_note *v, size_t n)
{
    for (size_t i = 0; v != NULL && i < n; i++) {
        pc_note_fini(&v[i]);
    }
    free(v);
}

static void blocks_free(pc_doc_psd_global_block *b, size_t n)
{
    for (size_t i = 0; b != NULL && i < n; i++) {
        pc_blob_release(b[i].data);
    }
    free(b);
}

pc_status pc_io_annotation_export_blocks(const pc_document *doc, pc_doc_psd_global_block *blocks, size_t n,
                                         pc_doc_psd_global_block **out, size_t *nout, pc_error *err)
{
    size_t at = 0;
    bool has_at = false;
    entry *e = NULL;
    size_t ne = 0;
    bool parsed = false;
    pc_note *want = NULL;
    size_t nwant = 0;
    bool keep = false;
    pc_buf b = PC_BUF_INIT;
    pc_io_byte_span *extra = NULL;
    size_t nextra = 0;
    pc_blob *fresh = NULL;
    pc_status st = PC_OK;
    for (size_t i = 0; i < n && !has_at; i++) {
        if (memcmp(blocks[i].key, ANNO, 4) == 0) {
            has_at = true;
            at = i;
        }
    }
    if (has_at) {
        st =
            parse_entries(pc_blob_data(blocks[at].data), pc_blob_len(blocks[at].data), &parsed, &e, &ne, err);
    }
    /* Compare in stored precision: what the document's notes read back as. */
    if (st == PC_OK) {
        st = pc_io_write_anno(doc->notes, doc->nnotes, NULL, 0, &b, err);
    }
    if (st == PC_OK) {
        bool ok = false;
        st = pc_io_parse_anno(b.data, b.len, &ok, &want, &nwant, err);
        if (st == PC_OK && parsed && ok) {
            pc_note *p = NULL;
            size_t np = 0;
            st = notes_of(e, ne, &p, &np, err);
            if (st == PC_OK) {
                keep = same_notes(p, np, want, nwant);
                notes_free(p, np);
            }
        }
    }
    pc_buf_fini(&b);
    notes_free(want, nwant);
    /* With no notes, a block we can't read is kept as is: dropping it would delete the
     * annotations it holds (issue #944). */
    if (st == PC_OK && (keep || (!parsed && doc->nnotes == 0))) {
        entries_free(e, ne);
        *out = blocks;
        *nout = n;
        return PC_OK;
    }
    if (st == PC_OK && ne > 0) {
        extra = pc_malloc_array(ne, sizeof *extra);
        if (extra == NULL) {
            st = pc_io_nomem(err, "writing notes");
        }
        for (size_t i = 0; extra != NULL && i < ne; i++) {
            if (!e[i].is_text) {
                extra[nextra].data = e[i].raw;
                extra[nextra++].len = e[i].raw_len;
            }
        }
    }
    if (st == PC_OK && (doc->nnotes > 0 || nextra > 0)) {
        st = pc_io_write_anno(doc->notes, doc->nnotes, extra, nextra, &b, err);
        if (st == PC_OK) {
            st = pc_blob_new(b.data, b.len, &fresh, err);
        }
        pc_buf_fini(&b);
    }
    free(extra);
    entries_free(e, ne);
    if (st != PC_OK) {
        blocks_free(blocks, n);
        return st;
    }
    if (has_at && fresh != NULL) {
        pc_blob_release(blocks[at].data);
        blocks[at].data = fresh;
    } else if (has_at) {
        pc_blob_release(blocks[at].data);
        memmove(&blocks[at], &blocks[at + 1], (n - at - 1) * sizeof *blocks);
        n--;
    } else if (fresh != NULL) {
        pc_doc_psd_global_block *v = realloc(blocks, (n + 1) * sizeof *v);
        if (v == NULL) {
            pc_blob_release(fresh);
            blocks_free(blocks, n);
            return pc_io_nomem(err, "writing notes");
        }
        blocks = v;
        memcpy(blocks[n].signature, "8BIM", 4);
        memcpy(blocks[n].key, ANNO, 4);
        blocks[n].data = fresh;
        n++;
    }
    *out = blocks;
    *nout = n;
    return PC_OK;
}

/* ---- measurement scale (resource 1074) ---- */

static bool anum(const pc_psd_value *v, double *out)
{
    if (v != NULL && v->kind == PC_PSD_V_LARGE_INTEGER) {
        *out = (double)v->l;
        return true;
    }
    return pc_io4_num(v, out);
}

/* The first of keys in d, else in an object one level down. */
static const pc_psd_value *find(const pc_psd_desc *d, const char *const *keys, size_t nk)
{
    for (size_t k = 0; k < nk; k++) {
        const pc_psd_value *v = pc_psd_desc_get(d, keys[k]);
        if (v != NULL) {
            return v;
        }
    }
    for (size_t i = 0; i < d->n; i++) {
        const pc_psd_value *o = &d->items[i].value;
        if (o->kind != PC_PSD_V_DESCRIPTOR) {
            continue;
        }
        for (size_t k = 0; k < nk; k++) {
            const pc_psd_value *v = pc_psd_desc_get(o->desc, keys[k]);
            if (v != NULL) {
                return v;
            }
        }
    }
    return NULL;
}

pc_status pc_io_scale_from_resource(const uint8_t *data, size_t len, bool *found, pc_measurement_scale *out,
                                    pc_error *err)
{
    static const char *const pl[2] = {"pixelLength", "PxlL"};
    static const char *const ll[2] = {"logicalLength", "LglL"};
    static const char *const lu[2] = {"logicalUnits", "LglU"};
    pc_psd_desc d;
    bool ok = false;
    pc_measurement_scale s;
    const pc_psd_value *u;
    pc_status st = pc_io4_vdesc_prefix(data, len, &ok, &d, err);
    if (st != PC_OK || !ok) {
        if (st == PC_OK) {
            *found = false;
        }
        return st;
    }
    memset(&s, 0, sizeof s);
    if (!anum(find(&d, pl, 2), &s.pixel_length) || !anum(find(&d, ll, 2), &s.logical_length)) {
        pc_psd_desc_fini(&d);
        *found = false;
        return PC_OK;
    }
    u = find(&d, lu, 2);
    if (u != NULL && u->kind == PC_PSD_V_TEXT) {
        st = pc_io4_ustr_lossy(&u->text, &s.units, err);
    } else {
        st = pc_str_set_c(&s.units, "pixels", err);
    }
    pc_psd_desc_fini(&d);
    if (st != PC_OK) {
        pc_measurement_scale_fini(&s);
        return st;
    }
    *out = s;
    *found = true;
    return PC_OK;
}

pc_status pc_io_write_scale_resource(const pc_measurement_scale *s, pc_buf *out, pc_error *err)
{
    pc_status st = PC_OK;
    pc_psd_desc d;
    pc_psd_bytes bytes;
    pc_buf b = PC_BUF_INIT;
    pc_io4_desc(&d, "null", &st, err);
    pc_io4_put(&d, "pixelLength", pc_psd_value_double(s->pixel_length), &st, err);
    pc_io4_put(&d, "logicalLength", pc_psd_value_double(s->logical_length), &st, err);
    pc_io4_put_text(&d, "logicalUnits", s->units.ptr, s->units.len, false, &st, err);
    if (st == PC_OK) {
        st = pc_psd_vdesc_to_bytes(&d, &bytes, err);
        if (st == PC_OK) {
            st = pc_buf_append(&b, bytes.data, bytes.len, err);
            pc_psd_bytes_fini(&bytes);
        }
        if (st == PC_OK && b.len % 2 == 1) {
            st = pc_buf_append_byte(&b, 0, err);
        }
    }
    pc_psd_desc_fini(&d);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

static const pc_doc_psd_resource *scale_res(const pc_document *doc)
{
    for (size_t i = 0; i < doc->metadata.npsd_resources; i++) {
        if (doc->metadata.psd_resources[i].id == PC_IO_MEASUREMENT_SCALE) {
            return &doc->metadata.psd_resources[i];
        }
    }
    return NULL;
}

bool pc_io_raw_scale(const pc_document *doc, pc_measurement_scale *out)
{
    const pc_doc_psd_resource *r = scale_res(doc);
    bool ok = false;
    /* Out of memory reads as "unreadable" here (the predicate has no status). */
    return r != NULL &&
           pc_io_scale_from_resource(pc_blob_data(r->data), pc_blob_len(r->data), &ok, out, NULL) == PC_OK &&
           ok;
}

/* keep_raw_scale: the preserved 1074 still describes the scale (or doesn't parse and the scale
 * is the default). */
static bool keep_raw_scale(const pc_document *doc)
{
    pc_measurement_scale s;
    if (pc_io_raw_scale(doc, &s)) {
        bool same = pc_measurement_scale_eq(&s, &doc->measurement.scale);
        pc_measurement_scale_fini(&s);
        return same;
    }
    return pc_measurement_scale_is_default(&doc->measurement.scale);
}

pc_status pc_io_fresh_scale_resource(const pc_document *doc, bool *has, pc_buf *out, pc_error *err)
{
    pc_status st;
    if (scale_res(doc) != NULL && keep_raw_scale(doc)) {
        *has = false;
        return PC_OK;
    }
    if (pc_measurement_scale_is_default(&doc->measurement.scale)) {
        *has = false;
        return PC_OK;
    }
    st = pc_io_write_scale_resource(&doc->measurement.scale, out, err);
    if (st == PC_OK) {
        *has = true;
    }
    return st;
}

bool pc_io_keep_resource(const pc_document *doc, uint16_t id)
{
    switch (id) {
    case PC_IO_MEASUREMENT_SCALE:
        return keep_raw_scale(doc);
    case COUNT_INFO:
        /* Our count markers can't be written into Photoshop's (unpublished) layout; a stale copy
         * would contradict them, so it is dropped once the document has markers of its own. */
        return pc_measurement_count_total(&doc->measurement) == 0;
    default:
        return true;
    }
}
