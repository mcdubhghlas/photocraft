/* Fonts for SVG text as usvg 0.45.1 reads them through ttf-parser 0.25 and rustybuzz 0.20:
 * ResolvedFont metrics (text/layout.rs load_font), glyph outlines (text/flatten.rs outline: glyf
 * decoded in float exactly like ttf-parser, so implied on-curve points are float midpoints; CFF
 * outlines come from FreeType in font units, see REWRITE_PLAN.md section 7), and shaping
 * (shape_text_with_font: unicode-bidi visual runs of the first paragraph, each shaped by
 * HarfBuzz with rustybuzz's segment properties: the run's direction, the script of its first
 * character that has one, no language).
 *
 * Fonts are untrusted: every read is bounds-checked, composite glyphs nest at most 32 deep (as
 * ttf-parser), and nothing here recurses deeper than that. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <fribidi.h>
#include <hb.h>
#include <pthread.h>

#include "base/blob.h"
#include "base/num.h"
#include "io/svg_text.h"
#include "text/font.h"
#include "text/sfnt.h"

/* HarfBuzz faces are shared by every import (the process-wide font database). Creating fonts on
 * them and shaping fill the faces' lazy caches with lock-free atomics that sanitizers can't see
 * inside the system library; one lock around those calls keeps them strictly ordered. */
static pthread_mutex_t hb_lock = PTHREAD_MUTEX_INITIALIZER;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

static int16_t bes16(const uint8_t *p)
{
    uint16_t u = be16(p);
    return u < 0x8000u ? (int16_t)u : (int16_t)((int32_t)u - 65536);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool table(const pc_sfnt *f, uint32_t tag, const uint8_t **p, size_t *n)
{
    return pc_sfnt_table(f, tag, p, n);
}

/* ---------------------------------------------------------------- ResolvedFont */

bool svt_rfont_load(const svf_db *db, size_t id, svt_rfont *out)
{
    pc_sfnt f;
    const uint8_t *data;
    size_t len;
    uint32_t index;
    const uint8_t *head, *hhea, *maxp, *os2 = NULL, *post = NULL;
    size_t nhead, nhhea, nmaxp, nos2 = 0, npost = 0;
    uint16_t upem;
    int16_t asc, desc;
    bool has_os2 = false;
    bool use_typo = false;
    bool has_post = false;
    uint16_t os2v = 0;
    svt_rfont r;
    hb_face_t *hf;
    if (!svf_db_data(db, id, &data, &len, &index) || !pc_sfnt_open(data, len, index, &f)) {
        return false;
    }
    if (!table(&f, PC_SFNT_TAG('h', 'e', 'a', 'd'), &head, &nhead) || nhead < 54 ||
        !table(&f, PC_SFNT_TAG('h', 'h', 'e', 'a'), &hhea, &nhhea) || nhhea < 36 ||
        !table(&f, PC_SFNT_TAG('m', 'a', 'x', 'p'), &maxp, &nmaxp) || nmaxp < 6) {
        return false;
    }
    upem = be16(head + 18);
    if (upem < 16 || upem > 16384 || be16(head + 50) > 1) {
        return false;
    }
    if ((be32(maxp) != 0x00005000u && be32(maxp) != 0x00010000u) || be16(maxp + 4) == 0) {
        return false;
    }
    memset(&r, 0, sizeof r);
    if (table(&f, PC_SFNT_TAG('O', 'S', '/', '2'), &os2, &nos2) && nos2 >= 2) {
        static const size_t LENS[6] = {78, 86, 96, 96, 96, 100};
        os2v = be16(os2);
        has_os2 = os2v <= 5 && nos2 >= LENS[os2v];
        use_typo = has_os2 && (be16(os2 + 62) & 0x80u) != 0;
    }
    if (table(&f, PC_SFNT_TAG('p', 'o', 's', 't'), &post, &npost) && npost >= 32) {
        uint32_t v = be32(post);
        has_post =
            v == 0x00010000u || v == 0x00020000u || v == 0x00025000u || v == 0x00030000u || v == 0x00040000u;
    }
    /* Face::ascender / descender */
    if (use_typo) {
        asc = bes16(os2 + 68);
        desc = bes16(os2 + 70);
    } else {
        asc = bes16(hhea + 4);
        desc = bes16(hhea + 6);
        if (asc == 0 && has_os2) {
            asc = bes16(os2 + 68);
            if (asc == 0) {
                asc = bes16(os2 + 74);
            }
        }
        if (desc == 0 && has_os2) {
            desc = bes16(os2 + 70);
            if (desc == 0) {
                desc = (int16_t)(-(int32_t)bes16(os2 + 76) == 32768 ? -32768 : -(int32_t)bes16(os2 + 76));
            }
        }
    }
    r.id = id;
    r.upem = upem;
    r.ascent = asc;
    r.descent = desc;
    /* x height: OS/2 v2+, positive, else 45 % of the height */
    {
        int32_t xh = has_os2 && os2v >= 2 ? bes16(os2 + 86) : -1;
        if (xh > 0) {
            r.x_height = (uint16_t)xh;
        } else {
            int16_t h16 = (int16_t)(uint16_t)((uint32_t)(uint16_t)asc - (uint32_t)(uint16_t)desc);
            int32_t v = pc_f32_to_i32((float)h16 * 0.45f);
            if (v <= 0 || v > 65535) {
                return false;
            }
            r.x_height = (uint16_t)v;
        }
    }
    if (has_os2) {
        r.line_through_pos = bes16(os2 + 28);
    } else {
        r.line_through_pos =
            (int16_t)(r.x_height <= 32767 ? (int16_t)r.x_height / 2 : (int16_t)(r.x_height - 65536) / 2);
    }
    if (has_post) {
        int16_t th = bes16(post + 10);
        r.underline_pos = bes16(post + 8);
        r.underline_thick = th > 0 ? (uint16_t)th : (uint16_t)(upem / 12);
    } else {
        r.underline_pos = (int16_t)(-(int32_t)upem / 9);
        r.underline_thick = (uint16_t)(upem / 12);
    }
    {
        float sub = roundf((float)upem / 0.2f);
        float sup = roundf((float)upem / 0.4f);
        r.sub_off = (int16_t)(sub > 32767.0f ? 32767 : (int32_t)sub);
        r.super_off = (int16_t)(sup > 32767.0f ? 32767 : (int32_t)sup);
        if (has_os2) {
            r.sub_off = bes16(os2 + 16);
            r.super_off = bes16(os2 + 24);
        }
    }
    r.data = data;
    r.len = len;
    r.index = index;
    r.glyf = pc_sfnt_has_table(&f, PC_SFNT_TAG('g', 'l', 'y', 'f'));
    hf = svf_db_hb_face(db, id);
    if (hf == NULL) {
        return false;
    }
    (void)pthread_mutex_lock(&hb_lock);
    r.hb_font = hb_font_create(hf);
    (void)pthread_mutex_unlock(&hb_lock);
    if (r.hb_font == NULL || r.hb_font == hb_font_get_empty()) {
        return false;
    }
    *out = r;
    return true;
}

void svt_rfont_fini(svt_rfont *f)
{
    if (f->hb_font != NULL) {
        (void)pthread_mutex_lock(&hb_lock);
        hb_font_destroy(f->hb_font);
        (void)pthread_mutex_unlock(&hb_lock);
        f->hb_font = NULL;
    }
    if (f->ft != NULL) {
        pc_font_release(f->ft);
        f->ft = NULL;
    }
}

/* ---------------------------------------------------------------- glyf outlines (ttf-parser) */

typedef struct ts6 {
    float a, b, c, d, e, f;
} ts6;

typedef struct gpt {
    float x, y;
} gpt;

typedef struct gbuild {
    sv_pb *pb;
    ts6 t;
    bool def;
    float xmin, ymin, xmax, ymax;
    bool has_first_on, has_first_off, has_last_off;
    gpt first_on, first_off, last_off;
} gbuild;

static void gb_init(gbuild *g, sv_pb *pb, ts6 t, float xmin, float ymin, float xmax, float ymax)
{
    memset(g, 0, sizeof *g);
    g->pb = pb;
    g->t = t;
    g->def = t.a == 1.0f && t.b == 0.0f && t.c == 0.0f && t.d == 1.0f && t.e == 0.0f && t.f == 0.0f;
    g->xmin = xmin;
    g->ymin = ymin;
    g->xmax = xmax;
    g->ymax = ymax;
}

static void gb_apply(const gbuild *g, float *x, float *y)
{
    if (!g->def) {
        float tx = *x;
        float ty = *y;
        *x = g->t.a * tx + g->t.c * ty + g->t.e;
        *y = g->t.b * tx + g->t.d * ty + g->t.f;
    }
}

static void gb_extend(gbuild *g, float x, float y)
{
    g->xmin = fminf(g->xmin, x);
    g->ymin = fminf(g->ymin, y);
    g->xmax = fmaxf(g->xmax, x);
    g->ymax = fmaxf(g->ymax, y);
}

static void gb_move(gbuild *g, float x, float y)
{
    gb_apply(g, &x, &y);
    gb_extend(g, x, y);
    pb_move_to(g->pb, x, y);
}

static void gb_line(gbuild *g, float x, float y)
{
    gb_apply(g, &x, &y);
    gb_extend(g, x, y);
    pb_line_to(g->pb, x, y);
}

static void gb_quad(gbuild *g, float x1, float y1, float x, float y)
{
    gb_apply(g, &x1, &y1);
    gb_apply(g, &x, &y);
    gb_extend(g, x1, y1);
    gb_extend(g, x, y);
    pb_quad_to(g->pb, x1, y1, x, y);
}

static gpt lerp(gpt a, gpt b)
{
    gpt r;
    r.x = a.x + 0.5f * (b.x - a.x);
    r.y = a.y + 0.5f * (b.y - a.y);
    return r;
}

static void gb_finish_contour(gbuild *g)
{
    if (g->has_first_off && g->has_last_off) {
        gpt mid = lerp(g->last_off, g->first_off);
        g->has_last_off = false;
        gb_quad(g, g->last_off.x, g->last_off.y, mid.x, mid.y);
    }
    if (g->has_first_on && g->has_first_off) {
        gb_quad(g, g->first_off.x, g->first_off.y, g->first_on.x, g->first_on.y);
    } else if (g->has_first_on && g->has_last_off) {
        gb_quad(g, g->last_off.x, g->last_off.y, g->first_on.x, g->first_on.y);
    } else if (g->has_first_on) {
        gb_line(g, g->first_on.x, g->first_on.y);
    }
    g->has_first_on = g->has_first_off = g->has_last_off = false;
    pb_close(g->pb);
}

static void gb_push(gbuild *g, float x, float y, bool on, bool last)
{
    gpt p = {x, y};
    if (!g->has_first_on) {
        if (on) {
            g->first_on = p;
            g->has_first_on = true;
            gb_move(g, p.x, p.y);
        } else if (g->has_first_off) {
            gpt mid = lerp(g->first_off, p);
            g->first_on = mid;
            g->has_first_on = true;
            g->last_off = p;
            g->has_last_off = true;
            gb_move(g, mid.x, mid.y);
        } else {
            g->first_off = p;
            g->has_first_off = true;
        }
    } else if (g->has_last_off && on) {
        gpt o = g->last_off;
        g->has_last_off = false;
        gb_quad(g, o.x, o.y, p.x, p.y);
    } else if (g->has_last_off) {
        gpt o = g->last_off;
        gpt mid = lerp(o, p);
        g->last_off = p;
        gb_quad(g, o.x, o.y, mid.x, mid.y);
    } else if (on) {
        gb_line(g, p.x, p.y);
    } else {
        g->last_off = p;
        g->has_last_off = true;
    }
    if (last) {
        gb_finish_contour(g);
    }
}

typedef struct glyf_ctx {
    const uint8_t *glyf;
    size_t nglyf;
    const uint8_t *loca;
    size_t nloca; /* entries */
    bool long_loca;
} glyf_ctx;

static bool glyph_range(const glyf_ctx *c, uint32_t gid, size_t *s, size_t *e)
{
    if (gid >= 0xFFFFu || gid + 1 >= c->nloca) {
        return false;
    }
    if (c->long_loca) {
        *s = be32(c->loca + (size_t)gid * 4);
        *e = be32(c->loca + (size_t)(gid + 1) * 4);
    } else {
        *s = (size_t)be16(c->loca + (size_t)gid * 2) * 2;
        *e = (size_t)be16(c->loca + (size_t)(gid + 1) * 2) * 2;
    }
    return *s < *e && *e <= c->nglyf;
}

/* A byte stream with ttf-parser's unwrap_or(0) reads. */
typedef struct bs {
    const uint8_t *p;
    size_t n, o;
} bs;

static bool bs_u8(bs *s, uint8_t *v)
{
    if (s->o + 1 > s->n) {
        return false;
    }
    *v = s->p[s->o++];
    return true;
}

static bool bs_u16(bs *s, uint16_t *v)
{
    if (s->o + 2 > s->n) {
        return false;
    }
    *v = be16(s->p + s->o);
    s->o += 2;
    return true;
}

static bool simple_glyph(gbuild *g, const uint8_t *d, size_t n, uint16_t ncont)
{
    size_t eo = 10;
    uint16_t total;
    uint16_t ilen;
    size_t fo;
    uint32_t left;
    uint32_t xl = 0, yl = 0;
    bs st;
    bs fl, xs, ys;
    uint16_t idx = 1, eleft;
    uint8_t flags = 0, repeats = 0;
    int16_t px = 0, py = 0;
    uint16_t points_left;
    if (eo + (size_t)ncont * 2 > n) {
        return false;
    }
    total = be16(d + eo + ((size_t)ncont - 1) * 2);
    if (total == 0xFFFFu) {
        return false;
    }
    total = (uint16_t)(total + 1);
    if (total == 1) {
        return true;
    }
    st.p = d;
    st.n = n;
    st.o = eo + (size_t)ncont * 2;
    if (!bs_u16(&st, &ilen)) {
        return false;
    }
    st.o += ilen;
    fo = st.o;
    left = total;
    while (left > 0) {
        uint8_t f;
        uint32_t rep = 1;
        if (!bs_u8(&st, &f)) {
            return false;
        }
        if (f & 0x08) {
            uint8_t r;
            if (!bs_u8(&st, &r)) {
                return false;
            }
            rep = (uint32_t)r + 1;
        }
        if (rep > left) {
            return false;
        }
        xl += (f & 0x02) != 0 ? rep : 0;
        xl += (f & (0x02 | 0x10)) == 0 ? rep * 2 : 0;
        yl += (f & 0x04) != 0 ? rep : 0;
        yl += (f & (0x04 | 0x20)) == 0 ? rep * 2 : 0;
        left -= rep;
    }
    if (st.o > n || xl > n - st.o || yl > n - st.o - xl) {
        return false;
    }
    fl.p = d + fo;
    fl.n = st.o - fo;
    fl.o = 0;
    xs.p = d + st.o;
    xs.n = xl;
    xs.o = 0;
    ys.p = d + st.o + xl;
    ys.n = yl;
    ys.o = 0;
    eleft = be16(d + eo);
    points_left = total;
    while (points_left > 0) {
        bool last;
        int16_t nx = 0, ny = 0;
        points_left--;
        if (eleft == 0) {
            if (idx < ncont) {
                uint16_t end = be16(d + eo + (size_t)idx * 2);
                uint16_t prev = be16(d + eo + (size_t)(idx - 1) * 2);
                eleft = end > prev ? (uint16_t)(end - prev) : 0;
                eleft = eleft > 0 ? (uint16_t)(eleft - 1) : 0;
            }
            if (idx < 0xFFFFu) {
                idx++;
            }
            last = true;
        } else {
            eleft--;
            last = false;
        }
        if (repeats == 0) {
            uint8_t f = 0;
            (void)bs_u8(&fl, &f);
            flags = f;
            if (flags & 0x08) {
                uint8_t r = 0;
                (void)bs_u8(&fl, &r);
                repeats = r;
            }
        } else {
            repeats--;
        }
        if (flags & 0x02) {
            uint8_t v = 0;
            (void)bs_u8(&xs, &v);
            nx = (flags & 0x10) ? (int16_t)v : (int16_t)-(int16_t)v;
        } else if (!(flags & 0x10)) {
            uint16_t v = 0;
            (void)bs_u16(&xs, &v);
            nx = v < 0x8000u ? (int16_t)v : (int16_t)((int32_t)v - 65536);
        }
        if (flags & 0x04) {
            uint8_t v = 0;
            (void)bs_u8(&ys, &v);
            ny = (flags & 0x20) ? (int16_t)v : (int16_t)-(int16_t)v;
        } else if (!(flags & 0x20)) {
            uint16_t v = 0;
            (void)bs_u16(&ys, &v);
            ny = v < 0x8000u ? (int16_t)v : (int16_t)((int32_t)v - 65536);
        }
        px = (int16_t)(uint16_t)((uint32_t)(uint16_t)px + (uint32_t)(uint16_t)nx);
        py = (int16_t)(uint16_t)((uint32_t)(uint16_t)py + (uint32_t)(uint16_t)ny);
        gb_push(g, (float)px, (float)py, (flags & 0x01) != 0, last);
    }
    return true;
}

static float f2dot14(uint16_t v)
{
    int16_t s = v < 0x8000u ? (int16_t)v : (int16_t)((int32_t)v - 65536);
    return (float)s / 16384.0f;
}

/* outline_impl: false is ttf-parser's None (the whole outline is dropped). */
static bool outline_impl(const glyf_ctx *c, const uint8_t *d, size_t n, unsigned depth, gbuild *g)
{
    int16_t ncont;
    if (depth >= 32 || n < 2) {
        return false;
    }
    ncont = bes16(d);
    if (n < 10) {
        return ncont == 0;
    }
    if (ncont > 0) {
        return simple_glyph(g, d, n, (uint16_t)ncont);
    }
    if (ncont < 0) {
        bs s = {d, n, 10};
        for (;;) {
            uint16_t flags;
            uint16_t gid;
            ts6 t = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
            size_t gs, ge;
            if (!bs_u16(&s, &flags) || !bs_u16(&s, &gid)) {
                break;
            }
            if (flags & 0x0002) {
                if (flags & 0x0001) {
                    uint16_t a, b;
                    if (!bs_u16(&s, &a) || !bs_u16(&s, &b)) {
                        break;
                    }
                    t.e = (float)(a < 0x8000u ? (int32_t)a : (int32_t)a - 65536);
                    t.f = (float)(b < 0x8000u ? (int32_t)b : (int32_t)b - 65536);
                } else {
                    uint8_t a, b;
                    if (!bs_u8(&s, &a) || !bs_u8(&s, &b)) {
                        break;
                    }
                    t.e = (float)(a < 0x80u ? (int32_t)a : (int32_t)a - 256);
                    t.f = (float)(b < 0x80u ? (int32_t)b : (int32_t)b - 256);
                }
            }
            if (flags & 0x0080) {
                uint16_t a, b, cc, dd;
                if (!bs_u16(&s, &a) || !bs_u16(&s, &b) || !bs_u16(&s, &cc) || !bs_u16(&s, &dd)) {
                    break;
                }
                t.a = f2dot14(a);
                t.b = f2dot14(b);
                t.c = f2dot14(cc);
                t.d = f2dot14(dd);
            } else if (flags & 0x0040) {
                uint16_t a, dd;
                if (!bs_u16(&s, &a) || !bs_u16(&s, &dd)) {
                    break;
                }
                t.a = f2dot14(a);
                t.d = f2dot14(dd);
            } else if (flags & 0x0008) {
                uint16_t a;
                if (!bs_u16(&s, &a)) {
                    break;
                }
                t.a = f2dot14(a);
                t.d = t.a;
            }
            if (glyph_range(c, gid, &gs, &ge)) {
                const ts6 p = g->t;
                ts6 comb;
                gbuild sub;
                comb.a = p.a * t.a + p.c * t.b;
                comb.b = p.b * t.a + p.d * t.b;
                comb.c = p.a * t.c + p.c * t.d;
                comb.d = p.b * t.c + p.d * t.d;
                comb.e = p.a * t.e + p.c * t.f + p.e;
                comb.f = p.b * t.e + p.d * t.f + p.f;
                gb_init(&sub, g->pb, comb, g->xmin, g->ymin, g->xmax, g->ymax);
                if (!outline_impl(c, c->glyf + gs, ge - gs, depth + 1, &sub)) {
                    return false;
                }
                g->xmin = sub.xmin;
                g->ymin = sub.ymin;
                g->xmax = sub.xmax;
                g->ymax = sub.ymax;
            }
            if (!(flags & 0x0020)) {
                break;
            }
        }
    }
    return true;
}

/* TryNumFrom<f32> for i16. */
static bool fits_i16(float v)
{
    return v > -2147483904.0f && v < 2147483648.0f && truncf(v) >= -32768.0f && truncf(v) <= 32767.0f;
}

static const sv_path *glyf_outline(sv_arena *a, const svt_rfont *f, uint32_t gid)
{
    pc_sfnt sf;
    const uint8_t *head, *loca, *glyf, *maxp;
    size_t nhead, nloca, nglyf, nmaxp;
    glyf_ctx c;
    size_t s, e;
    uint32_t total;
    sv_pb pb;
    gbuild g;
    ts6 id = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    if (!pc_sfnt_open(f->data, f->len, f->index, &sf) ||
        !table(&sf, PC_SFNT_TAG('h', 'e', 'a', 'd'), &head, &nhead) ||
        !table(&sf, PC_SFNT_TAG('l', 'o', 'c', 'a'), &loca, &nloca) ||
        !table(&sf, PC_SFNT_TAG('g', 'l', 'y', 'f'), &glyf, &nglyf) ||
        !table(&sf, PC_SFNT_TAG('m', 'a', 'x', 'p'), &maxp, &nmaxp) || nhead < 54 || nmaxp < 6) {
        return NULL;
    }
    c.long_loca = be16(head + 50) == 1;
    total = be16(maxp + 4);
    total = total == 0xFFFFu ? total : total + 1;
    c.nloca = c.long_loca ? nloca / 4 : nloca / 2;
    if (c.nloca > 0xFFFFu) {
        return NULL;
    }
    if (c.nloca > total) {
        c.nloca = total;
    }
    c.loca = loca;
    c.glyf = glyf;
    c.nglyf = nglyf;
    if (!glyph_range(&c, gid, &s, &e)) {
        return NULL;
    }
    pb_init(&pb, a);
    gb_init(&g, &pb, id, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    if (!outline_impl(&c, glyf + s, e - s, 0, &g)) {
        return NULL;
    }
    if (g.xmin == FLT_MAX && g.ymin == FLT_MAX && g.xmax == -FLT_MAX && g.ymax == -FLT_MAX) {
        return NULL;
    }
    if (!fits_i16(g.xmin) || !fits_i16(g.ymin) || !fits_i16(g.xmax) || !fits_i16(g.ymax)) {
        return NULL;
    }
    return pb_finish(&pb);
}

/* ---------------------------------------------------------------- CFF through FreeType */

static void pen_move(void *ctx, float x, float y)
{
    pb_move_to(ctx, x, y);
}

static void pen_line(void *ctx, float x, float y)
{
    pb_line_to(ctx, x, y);
}

static void pen_quad(void *ctx, float cx, float cy, float x, float y)
{
    pb_quad_to(ctx, cx, cy, x, y);
}

static void pen_cubic(void *ctx, float c0x, float c0y, float c1x, float c1y, float x, float y)
{
    pb_cubic_to(ctx, c0x, c0y, c1x, c1y, x, y);
}

static void pen_close(void *ctx)
{
    pb_close(ctx);
}

static const sv_path *cff_outline(sv_arena *a, const svt_rfont *cf, uint32_t gid)
{
    svt_rfont *f = (svt_rfont *)(uintptr_t)cf;
    sv_pb pb;
    pc_outline_pen pen;
    if (f->ft == NULL) {
        pc_blob *b = NULL;
        pc_font *font = NULL;
        if (pc_blob_new(f->data, f->len, &b, NULL) != PC_OK) {
            return NULL;
        }
        if (pc_font_new(b, f->index, &font, NULL) != PC_OK) {
            pc_blob_release(b);
            return NULL;
        }
        pc_blob_release(b);
        f->ft = font;
    }
    pb_init(&pb, a);
    pen.move_to = pen_move;
    pen.line_to = pen_line;
    pen.quad_to = pen_quad;
    pen.curve_to = pen_cubic;
    pen.close = pen_close;
    pen.ctx = &pb;
    if (pc_font_draw(f->ft, gid, NULL, NULL, 0, &pen, NULL) != PC_OK) {
        return NULL;
    }
    return pb_finish(&pb);
}

const sv_path *svt_outline(sv_arena *a, const svt_rfont *f, uint32_t gid)
{
    return f->glyf ? glyf_outline(a, f, gid) : cff_outline(a, f, gid);
}

/* ---------------------------------------------------------------- shaping */

static size_t utf8_len(unsigned char c)
{
    return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

static uint32_t utf8_at(const char *s, size_t n, size_t i, size_t *l)
{
    const unsigned char *p = (const unsigned char *)s + i;
    size_t k = utf8_len(p[0]);
    if (k > n - i) {
        k = 1;
    }
    *l = k;
    switch (k) {
    case 1:
        return p[0];
    case 2:
        return (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    case 3:
        return (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    default:
        return (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 |
               (p[3] & 0x3F);
    }
}

typedef struct brun {
    size_t s, e;
} brun;

/* unicode-bidi: levels of the first paragraph per byte (rule L1 applied) and its visual runs. */
static bool bidi_runs(sv_arena *a, const char *text, size_t len, uint8_t **lv_out, brun **runs_out,
                      size_t *nruns_out)
{
    size_t nch = 0;
    size_t i;
    size_t plen;
    FriBidiChar *cps;
    FriBidiCharType *types;
    FriBidiBracketType *br;
    FriBidiLevel *lev;
    FriBidiParType base = FRIBIDI_PAR_LTR;
    size_t *off;
    uint8_t *blev;
    brun *runs;
    size_t nr = 0;
    uint8_t minl, maxl;
    /* the first paragraph ends after the first paragraph separator */
    plen = len;
    for (i = 0; i < len;) {
        size_t l;
        uint32_t c = utf8_at(text, len, i, &l);
        i += l;
        nch++;
        if (fribidi_get_bidi_type(c) == FRIBIDI_TYPE_BS) {
            plen = i;
            break;
        }
    }
    cps = sv_alloc_array(a, nch + 1, sizeof *cps);
    types = sv_alloc_array(a, nch + 1, sizeof *types);
    br = sv_alloc_array(a, nch + 1, sizeof *br);
    lev = sv_alloc_array(a, nch + 1, sizeof *lev);
    off = sv_alloc_array(a, nch + 1, sizeof *off);
    blev = sv_alloc(a, len + 1);
    runs = sv_alloc_array(a, plen + 1, sizeof *runs);
    if (cps == NULL || types == NULL || br == NULL || lev == NULL || off == NULL || blev == NULL ||
        runs == NULL) {
        return false;
    }
    for (i = 0, nch = 0; i < plen;) {
        size_t l;
        off[nch] = i;
        cps[nch++] = utf8_at(text, plen, i, &l);
        i += l;
    }
    off[nch] = plen;
    fribidi_get_bidi_types(cps, (FriBidiStrIndex)nch, types);
    fribidi_get_bracket_types(cps, (FriBidiStrIndex)nch, types, br);
    if (fribidi_get_par_embedding_levels_ex(types, br, (FriBidiStrIndex)nch, &base, lev) == 0) {
        for (i = 0; i < nch; i++) {
            lev[i] = 0;
        }
    }
    /* reorder_levels (L1), paragraph level 0 */
    {
        bool has_from = true;
        size_t from = 0;
        bool has_to = false;
        size_t to = 0;
        FriBidiLevel prev = 0;
        size_t k;
        for (i = 0; i < nch; i++) {
            FriBidiCharType t = types[i];
            if (t == FRIBIDI_TYPE_BS || t == FRIBIDI_TYPE_SS) {
                has_to = true;
                to = i + 1;
                if (!has_from) {
                    has_from = true;
                    from = i;
                }
            } else if (t == FRIBIDI_TYPE_WS || t == FRIBIDI_TYPE_FSI || t == FRIBIDI_TYPE_LRI ||
                       t == FRIBIDI_TYPE_RLI || t == FRIBIDI_TYPE_PDI) {
                if (!has_from) {
                    has_from = true;
                    from = i;
                }
            } else if (t == FRIBIDI_TYPE_RLE || t == FRIBIDI_TYPE_LRE || t == FRIBIDI_TYPE_RLO ||
                       t == FRIBIDI_TYPE_LRO || t == FRIBIDI_TYPE_PDF || t == FRIBIDI_TYPE_BN) {
                if (!has_from) {
                    has_from = true;
                    from = i;
                }
                lev[i] = prev;
            } else {
                has_from = false;
            }
            if (has_from && has_to) {
                for (k = from; k < to; k++) {
                    lev[k] = 0;
                }
                has_from = has_to = false;
            }
            prev = lev[i];
        }
        if (has_from) {
            for (k = from; k < nch; k++) {
                lev[k] = 0;
            }
        }
    }
    memset(blev, 0, len + 1);
    for (i = 0; i < nch; i++) {
        size_t b;
        for (b = off[i]; b < off[i + 1]; b++) {
            blev[b] = (uint8_t)lev[i];
        }
    }
    /* visual_runs_for_line over the paragraph's bytes */
    if (plen == 0) {
        *lv_out = blev;
        *runs_out = runs;
        *nruns_out = 0;
        return true;
    }
    {
        size_t start = 0;
        uint8_t rl = blev[0];
        minl = maxl = rl;
        for (i = 1; i < plen; i++) {
            if (blev[i] != rl) {
                runs[nr].s = start;
                runs[nr++].e = i;
                start = i;
                rl = blev[i];
                minl = rl < minl ? rl : minl;
                maxl = rl > maxl ? rl : maxl;
            }
        }
        runs[nr].s = start;
        runs[nr++].e = plen;
    }
    minl = (minl & 1) != 0 ? minl : (uint8_t)(minl + 1);
    while (maxl >= minl) {
        size_t ss = 0;
        while (ss < nr) {
            size_t se;
            size_t x, y;
            if (blev[runs[ss].s] < maxl) {
                ss++;
                continue;
            }
            se = ss + 1;
            while (se < nr && blev[runs[se].s] >= maxl) {
                se++;
            }
            for (x = ss, y = se - 1; x < y; x++, y--) {
                brun t = runs[x];
                runs[x] = runs[y];
                runs[y] = t;
            }
            ss = se;
        }
        if (maxl == 0) {
            break;
        }
        maxl--;
    }
    *lv_out = blev;
    *runs_out = runs;
    *nruns_out = nr;
    return true;
}

bool svt_shape(sv_arena *a, const char *text, size_t len, const svt_rfont *font, bool small_caps,
               bool kerning, svt_glyph **out, size_t *nout)
{
    uint8_t *lv;
    brun *runs;
    size_t nr;
    size_t r;
    svt_glyph *gl = NULL;
    size_t ng = 0;
    size_t cap = 0;
    hb_feature_t feats[2];
    unsigned nf = 0;
    hb_buffer_t *buf;
    *nout = 0;
    if (len > INT32_MAX || !bidi_runs(a, text, len, &lv, &runs, &nr)) {
        return false;
    }
    if (small_caps) {
        feats[nf].tag = HB_TAG('s', 'm', 'c', 'p');
        feats[nf].value = 1;
        feats[nf].start = HB_FEATURE_GLOBAL_START;
        feats[nf++].end = HB_FEATURE_GLOBAL_END;
    }
    if (!kerning) {
        feats[nf].tag = HB_TAG('k', 'e', 'r', 'n');
        feats[nf].value = 0;
        feats[nf].start = HB_FEATURE_GLOBAL_START;
        feats[nf++].end = HB_FEATURE_GLOBAL_END;
    }
    buf = hb_buffer_create();
    if (!hb_buffer_allocation_successful(buf)) {
        hb_buffer_destroy(buf);
        return false;
    }
    for (r = 0; r < nr; r++) {
        const char *sub = text + runs[r].s;
        size_t sl = runs[r].e - runs[r].s;
        bool ltr = (lv[runs[r].s] & 1) == 0;
        hb_script_t script = HB_SCRIPT_INVALID;
        size_t i;
        unsigned n;
        hb_glyph_info_t *gi;
        hb_glyph_position_t *gp;
        if (sl == 0) {
            continue;
        }
        hb_buffer_clear_contents(buf);
        hb_buffer_add_utf8(buf, sub, (int)sl, 0, (int)sl);
        hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
        hb_buffer_set_direction(buf, ltr ? HB_DIRECTION_LTR : HB_DIRECTION_RTL);
        for (i = 0; i < sl;) {
            size_t l;
            uint32_t c = utf8_at(sub, sl, i, &l);
            hb_script_t s = hb_unicode_script(hb_unicode_funcs_get_default(), c);
            i += l;
            if (s != HB_SCRIPT_COMMON && s != HB_SCRIPT_INHERITED && s != HB_SCRIPT_UNKNOWN) {
                script = s;
                break;
            }
        }
        hb_buffer_set_script(buf, script);
        (void)pthread_mutex_lock(&hb_lock);
        hb_shape(font->hb_font, buf, feats, nf);
        (void)pthread_mutex_unlock(&hb_lock);
        if (!hb_buffer_allocation_successful(buf)) {
            hb_buffer_destroy(buf);
            return false;
        }
        gi = hb_buffer_get_glyph_infos(buf, &n);
        gp = hb_buffer_get_glyph_positions(buf, NULL);
        {
            void *v = gl;
            if (!sv_grow(a, &v, &cap, ng + n, sizeof(svt_glyph))) {
                hb_buffer_destroy(buf);
                return false;
            }
            gl = v;
        }
        for (i = 0; i < n; i++) {
            size_t start = gi[i].cluster;
            size_t end = sl;
            if (ltr && i + 1 < n) {
                end = gi[i + 1].cluster;
            } else if (!ltr && i > 0) {
                end = gi[i - 1].cluster;
            }
            gl[ng].id = gi[i].codepoint & 0xFFFFu;
            gl[ng].byte_idx = runs[r].s + start;
            gl[ng].cluster_len = end > start ? end - start : 0;
            gl[ng].dx = gp[i].x_offset;
            gl[ng].dy = gp[i].y_offset;
            gl[ng].width = gp[i].x_advance;
            gl[ng].font = font;
            ng++;
        }
    }
    hb_buffer_destroy(buf);
    *out = gl;
    *nout = ng;
    return true;
}
