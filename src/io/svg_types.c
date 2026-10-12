/* SVG attribute value parsers (port of the parts of svgtypes 0.15 usvg uses): numbers, lengths,
 * colours (hex, rgb()/rgba(), hsl()/hsla(), the CSS names), transform lists, paints with
 * fallbacks, IRIs, viewBox, preserveAspectRatio, the simplifying path parser and kurbo 0.11's
 * SVG-arc-to-cubic conversion (both in f64, as the reference). Numbers are converted with
 * yyjson's correctly rounded reader, so the locale never matters. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "base/num.h"
#include "io/svg_internal.h"

/* ---------------------------------------------------------------- stream */

void ss_init(sv_stream *st, const char *s)
{
    st->s = s != NULL ? s : "";
    st->len = strlen(st->s);
    st->pos = 0;
}

bool ss_at_end(const sv_stream *st)
{
    return st->pos >= st->len;
}

static char ss_cur(const sv_stream *st)
{
    return st->pos < st->len ? st->s[st->pos] : '\0';
}

static bool is_sp(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_dig(char c)
{
    return c >= '0' && c <= '9';
}

void ss_skip_spaces(sv_stream *st)
{
    while (!ss_at_end(st) && is_sp(ss_cur(st))) {
        st->pos++;
    }
}

static void skip_digits(sv_stream *st)
{
    while (!ss_at_end(st) && is_dig(ss_cur(st))) {
        st->pos++;
    }
}

static bool starts_with(const sv_stream *st, const char *t)
{
    size_t n = strlen(t);
    return st->len - st->pos >= n && memcmp(st->s + st->pos, t, n) == 0;
}

/* Rust's f64::from_str of s[0..n) (which matched svgtypes' number grammar), finite only. */
static bool to_f64(const char *s, size_t n, double *out)
{
    char buf[512];
    size_t i = 0;
    size_t o = 0;
    size_t int_st;
    size_t int_n;
    size_t frac_st = 0;
    size_t frac_n = 0;
    bool neg = false;
    yyjson_val v;
    const char *end;
    if (n == 0) {
        return false;
    }
    if (s[i] == '+' || s[i] == '-') {
        neg = s[i] == '-';
        i++;
    }
    while (i < n && s[i] == '0' && i + 1 < n && is_dig(s[i + 1])) {
        i++; /* JSON allows no leading zeros */
    }
    int_st = i;
    while (i < n && is_dig(s[i])) {
        i++;
    }
    int_n = i - int_st;
    if (i < n && s[i] == '.') {
        i++;
        frac_st = i;
        while (i < n && is_dig(s[i])) {
            i++;
        }
        frac_n = i - frac_st;
    }
    if (int_n == 0 && frac_n == 0) {
        return false;
    }
    /* Digits beyond the buffer change nothing representable in practice; refuse them. */
    if (int_n + frac_n + 64 > sizeof buf) {
        return false;
    }
    if (neg) {
        buf[o++] = '-';
    }
    if (int_n == 0) {
        buf[o++] = '0';
    } else {
        memcpy(buf + o, s + int_st, int_n);
        o += int_n;
    }
    buf[o++] = '.';
    if (frac_n == 0) {
        buf[o++] = '0';
    } else {
        memcpy(buf + o, s + frac_st, frac_n);
        o += frac_n;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t es;
        i++;
        buf[o++] = 'e';
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            buf[o++] = s[i];
            i++;
        }
        es = i;
        while (i < n && is_dig(s[i])) {
            i++;
        }
        if (i == es || i - es > 40) {
            return false;
        }
        memcpy(buf + o, s + es, i - es);
        o += i - es;
    }
    if (i != n) {
        return false;
    }
    buf[o] = '\0';
    end = yyjson_read_number(buf, &v, 0, NULL, NULL);
    if (end == NULL || *end != '\0') {
        return false;
    }
    if (yyjson_is_real(&v)) {
        *out = yyjson_get_real(&v);
    } else {
        return false;
    }
    return isfinite(*out);
}

bool ss_parse_number(sv_stream *st, double *out)
{
    size_t start;
    char c;
    ss_skip_spaces(st);
    start = st->pos;
    if (ss_at_end(st)) {
        return false;
    }
    c = ss_cur(st);
    if (c == '+' || c == '-') {
        st->pos++;
        if (ss_at_end(st)) {
            return false;
        }
        c = ss_cur(st);
    }
    if (is_dig(c)) {
        skip_digits(st);
    } else if (c != '.') {
        return false;
    }
    if (ss_cur(st) == '.' && !ss_at_end(st)) {
        st->pos++;
        skip_digits(st);
    }
    if (!ss_at_end(st) && (ss_cur(st) == 'e' || ss_cur(st) == 'E')) {
        char c2;
        if (st->pos + 1 >= st->len) {
            return false;
        }
        c2 = st->s[st->pos + 1];
        if (c2 != 'm' && c2 != 'x') {
            st->pos++;
            if (ss_at_end(st)) {
                return false;
            }
            c = ss_cur(st);
            if (c == '+' || c == '-') {
                st->pos++;
                skip_digits(st);
            } else if (is_dig(c)) {
                skip_digits(st);
            } else {
                return false;
            }
        }
    }
    return to_f64(st->s + start, st->pos - start, out);
}

static void list_separator(sv_stream *st)
{
    if (ss_cur(st) == ',' && !ss_at_end(st)) {
        st->pos++;
    }
}

bool ss_parse_list_number(sv_stream *st, double *out)
{
    if (ss_at_end(st) || !ss_parse_number(st, out)) {
        return false;
    }
    ss_skip_spaces(st);
    list_separator(st);
    return true;
}

bool ss_parse_length(sv_stream *st, sv_length *out)
{
    static const struct {
        const char *s;
        sv_unit u;
    } U[] = {{"%", U_PERCENT}, {"em", U_EM}, {"ex", U_EX}, {"px", U_PX}, {"in", U_IN},
             {"cm", U_CM},     {"mm", U_MM}, {"pt", U_PT}, {"pc", U_PC}};
    double n;
    size_t i;
    ss_skip_spaces(st);
    if (!ss_parse_number(st, &n)) {
        return false;
    }
    out->number = n;
    out->unit = U_NONE;
    for (i = 0; i < sizeof U / sizeof U[0]; i++) {
        if (starts_with(st, U[i].s)) {
            out->unit = U[i].u;
            st->pos += strlen(U[i].s);
            break;
        }
    }
    return true;
}

bool ss_parse_list_length(sv_stream *st, sv_length *out)
{
    if (ss_at_end(st) || !ss_parse_length(st, out)) {
        return false;
    }
    ss_skip_spaces(st);
    list_separator(st);
    return true;
}

bool sv_parse_number(const char *s, double *out)
{
    sv_stream st;
    double n;
    ss_init(&st, s);
    if (!ss_parse_number(&st, &n)) {
        return false;
    }
    ss_skip_spaces(&st);
    if (!ss_at_end(&st)) {
        return false;
    }
    *out = n;
    return true;
}

bool sv_parse_length(const char *s, sv_length *out)
{
    sv_stream st;
    sv_length l;
    ss_init(&st, s);
    if (!ss_parse_length(&st, &l)) {
        return false;
    }
    ss_skip_spaces(&st);
    if (!ss_at_end(&st)) {
        return false;
    }
    *out = l;
    return true;
}

/* ---------------------------------------------------------------- colours */

typedef struct named {
    const char *name;
    sv_color c;
} named;

static const named COLORS[] = {
#include "io/svg_colors.inc"
};

static int named_cmp(const void *k, const void *e)
{
    return strcmp((const char *)k, ((const named *)e)->name);
}

static int hexv(char c)
{
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : 0;
}

static bool is_hex(char c)
{
    return is_dig(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_ascii_ident(char c)
{
    return is_dig(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_';
}

static bool number_or_percent(sv_stream *st, double *out)
{
    double n;
    ss_skip_spaces(st);
    if (!ss_parse_number(st, &n)) {
        return false;
    }
    if (ss_cur(st) == '%' && !ss_at_end(st)) {
        st->pos++;
        n = n / 100.0;
    }
    *out = n;
    return true;
}

static bool list_number_or_percent(sv_stream *st, double *out)
{
    if (ss_at_end(st) || !number_or_percent(st, out)) {
        return false;
    }
    ss_skip_spaces(st);
    list_separator(st);
    return true;
}

static uint8_t round_u8(double v)
{
    return pc_f64_to_u8(round(v));
}

static float hue_to_rgb(float t1, float t2, float hue)
{
    if (hue < 0.0f) {
        hue += 6.0f;
    }
    if (hue >= 6.0f) {
        hue -= 6.0f;
    }
    if (hue < 1.0f) {
        return (t2 - t1) * hue + t1;
    }
    if (hue < 3.0f) {
        return t2;
    }
    if (hue < 4.0f) {
        return (t2 - t1) * (4.0f - hue) + t1;
    }
    return t1;
}

static double clamp01(double v)
{
    return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
}

bool ss_parse_color(sv_stream *st, sv_color *out)
{
    sv_color c = {0, 0, 0, 255};
    ss_skip_spaces(st);
    if (ss_at_end(st)) {
        return false;
    }
    if (ss_cur(st) == '#') {
        size_t s0;
        size_t n;
        const char *h;
        st->pos++;
        s0 = st->pos;
        while (!ss_at_end(st) && is_hex(ss_cur(st))) {
            st->pos++;
        }
        n = st->pos - s0;
        h = st->s + s0;
        if (n == 6 || n == 8) {
            c.r = (uint8_t)(hexv(h[0]) << 4 | hexv(h[1]));
            c.g = (uint8_t)(hexv(h[2]) << 4 | hexv(h[3]));
            c.b = (uint8_t)(hexv(h[4]) << 4 | hexv(h[5]));
            if (n == 8) {
                c.a = (uint8_t)(hexv(h[6]) << 4 | hexv(h[7]));
            }
        } else if (n == 3 || n == 4) {
            c.r = (uint8_t)(hexv(h[0]) * 17);
            c.g = (uint8_t)(hexv(h[1]) * 17);
            c.b = (uint8_t)(hexv(h[2]) * 17);
            if (n == 4) {
                c.a = (uint8_t)(hexv(h[3]) * 17);
            }
        } else {
            return false;
        }
    } else {
        char name[32];
        size_t s0 = st->pos;
        size_t n;
        size_t i;
        while (!ss_at_end(st) && is_ascii_ident(ss_cur(st))) {
            st->pos++;
        }
        n = st->pos - s0;
        if (n >= sizeof name) {
            return false;
        }
        for (i = 0; i < n; i++) {
            char ch = st->s[s0 + i];
            name[i] = (char)(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        }
        name[n] = '\0';
        if (strcmp(name, "rgb") == 0 || strcmp(name, "rgba") == 0) {
            double v;
            bool pct = false;
            if (ss_cur(st) != '(') {
                return false;
            }
            st->pos++;
            if (!ss_parse_number(st, &v)) {
                return false;
            }
            if (ss_cur(st) == '%' && !ss_at_end(st)) {
                st->pos++;
                pct = true;
            }
            ss_skip_spaces(st);
            list_separator(st);
            if (pct) {
                double g;
                double b;
                c.r = round_u8((v / 100.0) * 255.0);
                if (!list_number_or_percent(st, &g) || !list_number_or_percent(st, &b)) {
                    return false;
                }
                c.g = round_u8(g * 255.0);
                c.b = round_u8(b * 255.0);
            } else {
                double g;
                double b;
                c.r = round_u8(v);
                if (!ss_parse_list_number(st, &g) || !ss_parse_list_number(st, &b)) {
                    return false;
                }
                c.g = round_u8(g);
                c.b = round_u8(b);
            }
            ss_skip_spaces(st);
            if (!starts_with(st, ")")) {
                double a;
                if (!ss_parse_list_number(st, &a)) {
                    return false;
                }
                c.a = round_u8(a * 255.0);
            }
            ss_skip_spaces(st);
            if (ss_cur(st) != ')' || ss_at_end(st)) {
                return false;
            }
            st->pos++;
        } else if (strcmp(name, "hsl") == 0 || strcmp(name, "hsla") == 0) {
            double hue;
            double sat;
            double light;
            float t1;
            float t2;
            float h;
            float s;
            float l;
            if (ss_cur(st) != '(') {
                return false;
            }
            st->pos++;
            if (!ss_parse_list_number(st, &hue)) {
                return false;
            }
            hue = fmod(fmod(hue, 360.0) + 360.0, 360.0);
            if (!list_number_or_percent(st, &sat) || !list_number_or_percent(st, &light)) {
                return false;
            }
            h = (float)hue / 60.0f;
            s = (float)clamp01(sat);
            l = (float)clamp01(light);
            t2 = l <= 0.5f ? l * (s + 1.0f) : l + s - (l * s);
            t1 = l * 2.0f - t2;
            c.r = pc_f32_to_u8(roundf(hue_to_rgb(t1, t2, h + 2.0f) * 255.0f));
            c.g = pc_f32_to_u8(roundf(hue_to_rgb(t1, t2, h) * 255.0f));
            c.b = pc_f32_to_u8(roundf(hue_to_rgb(t1, t2, h - 2.0f) * 255.0f));
            ss_skip_spaces(st);
            if (!starts_with(st, ")")) {
                double a;
                if (!ss_parse_list_number(st, &a)) {
                    return false;
                }
                c.a = round_u8(a * 255.0);
            }
            ss_skip_spaces(st);
            if (ss_cur(st) != ')' || ss_at_end(st)) {
                return false;
            }
            st->pos++;
        } else {
            const named *nm =
                bsearch(name, COLORS, sizeof COLORS / sizeof COLORS[0], sizeof COLORS[0], named_cmp);
            if (nm == NULL) {
                return false;
            }
            c = nm->c;
        }
    }
    *out = c;
    return true;
}

bool sv_parse_color(const char *s, sv_color *out)
{
    sv_stream st;
    sv_color c;
    ss_init(&st, s);
    if (!ss_parse_color(&st, &c)) {
        return false;
    }
    ss_skip_spaces(&st);
    if (!ss_at_end(&st)) {
        return false;
    }
    *out = c;
    return true;
}

/* ---------------------------------------------------------------- transforms */

static void multiply(double t[6], const double o[6])
{
    double r[6];
    r[0] = t[0] * o[0] + t[2] * o[1];
    r[1] = t[1] * o[0] + t[3] * o[1];
    r[2] = t[0] * o[2] + t[2] * o[3];
    r[3] = t[1] * o[2] + t[3] * o[3];
    r[4] = t[0] * o[4] + t[2] * o[5] + t[4];
    r[5] = t[1] * o[4] + t[3] * o[5] + t[5];
    memcpy(t, r, sizeof r);
}

static double to_radians(double deg)
{
    return deg * (3.14159265358979323846 / 180.0);
}

bool sv_parse_transform(const char *s, double m[6])
{
    sv_stream st;
    double ts[6] = {1, 0, 0, 1, 0, 0};
    ss_init(&st, s);
    for (;;) {
        char name[16] = {0};
        size_t s0;
        size_t n;
        double o[6] = {1, 0, 0, 1, 0, 0};
        ss_skip_spaces(&st);
        if (ss_at_end(&st)) {
            break;
        }
        s0 = st.pos;
        while (!ss_at_end(&st) && is_ascii_ident(ss_cur(&st))) {
            st.pos++;
        }
        n = st.pos - s0;
        if (n >= sizeof name) {
            return false;
        }
        memcpy(name, st.s + s0, n);
        name[n] = '\0';
        ss_skip_spaces(&st);
        if (ss_cur(&st) != '(' || ss_at_end(&st)) {
            return false;
        }
        st.pos++;
        if (strcmp(name, "matrix") == 0) {
            int i;
            for (i = 0; i < 6; i++) {
                if (!ss_parse_list_number(&st, &o[i])) {
                    return false;
                }
            }
            multiply(ts, o);
        } else if (strcmp(name, "translate") == 0) {
            if (!ss_parse_list_number(&st, &o[4])) {
                return false;
            }
            ss_skip_spaces(&st);
            if (ss_cur(&st) != ')' && !ss_parse_list_number(&st, &o[5])) {
                return false;
            }
            multiply(ts, o);
        } else if (strcmp(name, "scale") == 0) {
            if (!ss_parse_list_number(&st, &o[0])) {
                return false;
            }
            ss_skip_spaces(&st);
            o[3] = o[0];
            if (ss_cur(&st) != ')' && !ss_parse_list_number(&st, &o[3])) {
                return false;
            }
            multiply(ts, o);
        } else if (strcmp(name, "rotate") == 0) {
            double a;
            double v;
            double r[6] = {1, 0, 0, 1, 0, 0};
            if (!ss_parse_list_number(&st, &a)) {
                return false;
            }
            ss_skip_spaces(&st);
            v = to_radians(a);
            r[0] = cos(v);
            r[1] = sin(v);
            r[2] = -r[1];
            r[3] = r[0];
            if (ss_cur(&st) != ')') {
                double cx = 0.0;
                double cy = 0.0;
                double t1[6] = {1, 0, 0, 1, 0, 0};
                if (!ss_parse_list_number(&st, &cx) || !ss_parse_list_number(&st, &cy)) {
                    return false;
                }
                t1[4] = cx;
                t1[5] = cy;
                multiply(ts, t1);
                multiply(ts, r);
                t1[4] = -cx;
                t1[5] = -cy;
                multiply(ts, t1);
            } else {
                multiply(ts, r);
            }
        } else if (strcmp(name, "skewX") == 0 || strcmp(name, "skewY") == 0) {
            double a = 0.0;
            if (!ss_parse_list_number(&st, &a)) {
                return false;
            }
            o[name[4] == 'X' ? 2 : 1] = tan(to_radians(a));
            multiply(ts, o);
        } else {
            return false;
        }
        ss_skip_spaces(&st);
        if (ss_cur(&st) != ')' || ss_at_end(&st)) {
            return false;
        }
        st.pos++;
        ss_skip_spaces(&st);
        list_separator(&st);
    }
    memcpy(m, ts, sizeof ts);
    return true;
}

/* ---------------------------------------------------------------- IRIs and paints */

bool sv_parse_iri(const char *s, const char **id, size_t *len)
{
    sv_stream st;
    size_t s0;
    ss_init(&st, s);
    ss_skip_spaces(&st);
    if (ss_cur(&st) != '#' || ss_at_end(&st)) {
        return false;
    }
    st.pos++;
    s0 = st.pos;
    while (!ss_at_end(&st) && ss_cur(&st) != ' ') {
        st.pos++;
    }
    if (st.pos == s0) {
        return false;
    }
    *id = st.s + s0;
    *len = st.pos - s0;
    ss_skip_spaces(&st);
    return ss_at_end(&st);
}

/* Stream::parse_func_iri; the stream is left after ')'. */
static bool func_iri(sv_stream *st, const char **id, size_t *len)
{
    char quote = 0;
    size_t s0;
    size_t e;
    size_t i;
    ss_skip_spaces(st);
    if (!starts_with(st, "url(")) {
        return false;
    }
    st->pos += 4;
    ss_skip_spaces(st);
    if (ss_cur(st) == '\'' || ss_cur(st) == '"') {
        quote = ss_cur(st);
        st->pos++;
        ss_skip_spaces(st);
    }
    if (ss_cur(st) != '#' || ss_at_end(st)) {
        return false;
    }
    st->pos++;
    s0 = st->pos;
    if (quote != 0) {
        while (!ss_at_end(st) && ss_cur(st) != quote) {
            st->pos++;
        }
        e = st->pos;
        while (e > s0 && is_sp(st->s[e - 1])) {
            e--;
        }
    } else {
        while (!ss_at_end(st) && ss_cur(st) != ' ' && ss_cur(st) != ')') {
            st->pos++;
        }
        e = st->pos;
    }
    if (e == s0) {
        return false;
    }
    for (i = s0; i < e; i++) {
        if (st->s[i] == '\'' || st->s[i] == '"') {
            return false;
        }
    }
    ss_skip_spaces(st);
    if (quote != 0) {
        if (ss_cur(st) != quote || ss_at_end(st)) {
            return false;
        }
        st->pos++;
        ss_skip_spaces(st);
    }
    if (ss_cur(st) != ')' || ss_at_end(st)) {
        return false;
    }
    st->pos++;
    *id = st->s + s0;
    *len = e - s0;
    return true;
}

bool sv_parse_func_iri(const char *s, const char **id, size_t *len)
{
    sv_stream st;
    ss_init(&st, s);
    if (!func_iri(&st, id, len)) {
        return false;
    }
    ss_skip_spaces(&st);
    return ss_at_end(&st);
}

static bool eq_trim(const char *s, size_t n, const char *t)
{
    return strlen(t) == n && memcmp(s, t, n) == 0;
}

bool sv_parse_paint(const char *s, sv_spaint *out)
{
    size_t st0 = 0;
    size_t n;
    sv_spaint p;
    memset(&p, 0, sizeof p);
    if (s == NULL) {
        return false;
    }
    n = strlen(s);
    while (st0 < n && is_sp(s[st0])) {
        st0++;
    }
    while (n > st0 && is_sp(s[n - 1])) {
        n--;
    }
    s += st0;
    n -= st0;
    if (eq_trim(s, n, "none")) {
        p.kind = P_NONE;
    } else if (eq_trim(s, n, "inherit")) {
        p.kind = P_INHERIT;
    } else if (eq_trim(s, n, "currentColor")) {
        p.kind = P_CURRENT_COLOR;
    } else if (eq_trim(s, n, "context-fill")) {
        p.kind = P_CONTEXT_FILL;
    } else if (eq_trim(s, n, "context-stroke")) {
        p.kind = P_CONTEXT_STROKE;
    } else {
        sv_stream st;
        st.s = s;
        st.len = n;
        st.pos = 0;
        if (starts_with(&st, "url(")) {
            if (!func_iri(&st, &p.iri, &p.iri_len)) {
                return false;
            }
            p.kind = P_FUNC_IRI;
            p.fallback = PF_NONE_SET;
            ss_skip_spaces(&st);
            if (!ss_at_end(&st)) {
                const char *f = st.s + st.pos;
                size_t fl = st.len - st.pos;
                if (eq_trim(f, fl, "none")) {
                    p.fallback = PF_NONE;
                } else if (eq_trim(f, fl, "currentColor")) {
                    p.fallback = PF_CURRENT_COLOR;
                } else {
                    sv_stream fs;
                    fs.s = f;
                    fs.len = fl;
                    fs.pos = 0;
                    if (!ss_parse_color(&fs, &p.fallback_color)) {
                        return false;
                    }
                    ss_skip_spaces(&fs);
                    if (!ss_at_end(&fs)) {
                        return false;
                    }
                    p.fallback = PF_COLOR;
                }
            }
        } else {
            sv_stream cs;
            cs.s = s;
            cs.len = n;
            cs.pos = 0;
            if (!ss_parse_color(&cs, &p.color)) {
                return false;
            }
            ss_skip_spaces(&cs);
            if (!ss_at_end(&cs)) {
                return false;
            }
            p.kind = P_COLOR;
        }
    }
    *out = p;
    return true;
}

/* ---------------------------------------------------------------- viewBox, aspect */

bool sv_parse_viewbox(const char *s, sv_viewbox *out)
{
    sv_stream st;
    sv_viewbox v;
    ss_init(&st, s);
    if (!ss_parse_list_number(&st, &v.x) || !ss_parse_list_number(&st, &v.y) ||
        !ss_parse_list_number(&st, &v.w) || !ss_parse_list_number(&st, &v.h)) {
        return false;
    }
    if (v.w <= 0.0 || v.h <= 0.0) {
        return false;
    }
    *out = v;
    return true;
}

sv_aspect sv_aspect_default(void)
{
    sv_aspect a = {false, AL_XMIDYMID, false};
    return a;
}

bool sv_parse_aspect(const char *s, sv_aspect *out)
{
    static const char *const AL[] = {"none",     "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid",
                                     "xMidYMid", "xMaxYMid", "xMinYMax", "xMidYMax", "xMaxYMax"};
    sv_stream st;
    sv_aspect a = {false, AL_NONE, false};
    size_t s0;
    size_t n;
    size_t i;
    bool found = false;
    ss_init(&st, s);
    ss_skip_spaces(&st);
    if (starts_with(&st, "defer")) {
        st.pos += 5;
        if (ss_cur(&st) != ' ' || ss_at_end(&st)) {
            return false;
        }
        st.pos++;
        ss_skip_spaces(&st);
        a.defer = true;
    }
    s0 = st.pos;
    while (!ss_at_end(&st) && is_ascii_ident(ss_cur(&st))) {
        st.pos++;
    }
    n = st.pos - s0;
    for (i = 0; i < sizeof AL / sizeof AL[0]; i++) {
        if (eq_trim(st.s + s0, n, AL[i])) {
            a.align = (sv_align)i;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    ss_skip_spaces(&st);
    if (!ss_at_end(&st)) {
        s0 = st.pos;
        while (!ss_at_end(&st) && is_ascii_ident(ss_cur(&st))) {
            st.pos++;
        }
        n = st.pos - s0;
        if (eq_trim(st.s + s0, n, "slice")) {
            a.slice = true;
        } else if (!eq_trim(st.s + s0, n, "meet") && n != 0) {
            return false;
        }
    }
    *out = a;
    return true;
}

/* ---------------------------------------------------------------- arcs (kurbo 0.11) */

#define PI_D 3.14159265358979323846
/* Subdivisions per arc are bounded (kurbo has no bound: an infinite radius never finishes). */
#define MAX_ARC_SEGMENTS 4096.0

static void sample_ellipse(double rx, double ry, double rot, double angle, double *x, double *y)
{
    double u = rx * cos(angle);
    double v = ry * sin(angle);
    double s = sin(rot);
    double c = cos(rot);
    *x = u * c - v * s;
    *y = u * s + v * c;
}

bool sv_arc_to_cubics(double fx, double fy, double rx, double ry, double x_rot_deg, bool large, bool sweep,
                      double tx, double ty, bool (*f)(void *ctx, const double c[6]), void *ctx, bool *ok)
{
    double xr;
    double sin_phi;
    double cos_phi;
    double hd_x;
    double hd_y;
    double hs_x;
    double hs_y;
    double px;
    double py;
    double rf;
    double rxry;
    double rxpy;
    double rypx;
    double sum_of_sq;
    double coe;
    double tcx;
    double tcy;
    double cx;
    double cy;
    double start;
    double sweep_angle;
    double sign;
    double scaled_err;
    double n_err;
    double nf;
    double step;
    double arm;
    double a0;
    double p0x;
    double p0y;
    double x_rot = to_radians(x_rot_deg);
    size_t n;
    size_t i;
    *ok = true;
    rx = fabs(rx);
    ry = fabs(ry);
    if (rx <= 1e-5 || ry <= 1e-5 || (fx == tx && fy == ty)) {
        return false;
    }
    xr = fmod(x_rot, 2.0 * PI_D);
    sin_phi = sin(xr);
    cos_phi = cos(xr);
    hd_x = (fx - tx) * 0.5;
    hd_y = (fy - ty) * 0.5;
    hs_x = (fx + tx) * 0.5;
    hs_y = (fy + ty) * 0.5;
    px = cos_phi * hd_x + sin_phi * hd_y;
    py = -sin_phi * hd_x + cos_phi * hd_y;
    rf = px * px / (rx * rx) + py * py / (ry * ry);
    if (rf > 1.0) {
        double scale = sqrt(rf);
        rx *= scale;
        ry *= scale;
    }
    rxry = rx * ry;
    rxpy = rx * py;
    rypx = ry * px;
    sum_of_sq = rxpy * rxpy + rypx * rypx;
    coe = (large == sweep ? -1.0 : 1.0) * sqrt(fabs((rxry * rxry - sum_of_sq) / sum_of_sq));
    tcx = coe * rxpy / ry;
    tcy = -coe * rypx / rx;
    cx = cos_phi * tcx - sin_phi * tcy + hs_x;
    cy = sin_phi * tcx + cos_phi * tcy + hs_y;
    start = atan2((py - tcy) / ry, (px - tcx) / rx);
    sweep_angle = fmod(atan2((-py - tcy) / ry, (-px - tcx) / rx) - start, 2.0 * PI_D);
    if (sweep && sweep_angle < 0.0) {
        sweep_angle += 2.0 * PI_D;
    } else if (!sweep && sweep_angle > 0.0) {
        sweep_angle -= 2.0 * PI_D;
    }
    /* append_iter(0.1) */
    sign = isnan(sweep_angle) ? sweep_angle : signbit(sweep_angle) ? -1.0 : 1.0;
    scaled_err = (rx > ry || isnan(ry) ? rx : ry) / 0.1;
    {
        double t = pow(1.1163 * scaled_err, 1.0 / 6.0);
        n_err = isnan(t) || t < 3.999999 ? 3.999999 : t;
    }
    nf = ceil(n_err * fabs(sweep_angle) * (1.0 / (2.0 * PI_D)));
    step = sweep_angle / nf;
    if (!(nf >= 0.0)) {
        nf = 0.0;
    }
    if (nf > MAX_ARC_SEGMENTS) {
        nf = MAX_ARC_SEGMENTS;
    }
    n = (size_t)nf;
    arm = (4.0 / 3.0) * tan(fabs(0.25 * step)) * sign;
    a0 = start;
    sample_ellipse(rx, ry, x_rot, a0, &p0x, &p0y);
    for (i = 0; i < n; i++) {
        double a1 = a0 + step;
        double ex;
        double ey;
        double c[6];
        double p3x;
        double p3y;
        sample_ellipse(rx, ry, x_rot, a0 + PI_D / 2.0, &ex, &ey);
        c[0] = cx + (p0x + arm * ex);
        c[1] = cy + (p0y + arm * ey);
        sample_ellipse(rx, ry, x_rot, a1, &p3x, &p3y);
        sample_ellipse(rx, ry, x_rot, a1 + PI_D / 2.0, &ex, &ey);
        c[2] = cx + (p3x - arm * ex);
        c[3] = cy + (p3y - arm * ey);
        c[4] = cx + p3x;
        c[5] = cy + p3y;
        if (!f(ctx, c)) {
            *ok = false;
            return true;
        }
        a0 = a1;
        p0x = p3x;
        p0y = p3y;
    }
    return true;
}

/* ---------------------------------------------------------------- paths */

typedef enum { PS_M, PS_L, PS_H, PS_V, PS_C, PS_S, PS_Q, PS_T, PS_A, PS_Z } pskind;

typedef struct pseg {
    pskind k;
    bool abs;
    double v[7];
    bool large, sweep;
} pseg;

static bool is_cmd(char c)
{
    return strchr("MmZzLlHhVvCcSsQqTtAa", c) != NULL && c != '\0';
}

static bool flag(sv_stream *st, bool *out)
{
    char c;
    ss_skip_spaces(st);
    if (ss_at_end(st)) {
        return false;
    }
    c = ss_cur(st);
    if (c != '0' && c != '1') {
        return false;
    }
    st->pos++;
    if (ss_cur(st) == ',' && !ss_at_end(st)) {
        st->pos++;
    }
    ss_skip_spaces(st);
    *out = c == '1';
    return true;
}

/* PathParser::next: false at the end or on an error (*err). */
static bool next_seg(sv_stream *st, char *prev, pseg *out, bool *err)
{
    char first;
    char cmd;
    bool implicit = false;
    char lc;
    int i;
    int nv = 0;
    *err = false;
    ss_skip_spaces(st);
    if (ss_at_end(st)) {
        return false;
    }
    first = ss_cur(st);
    if (*prev == 0 && !is_cmd(first)) {
        *err = true;
        return false;
    }
    if (*prev == 0 && first != 'M' && first != 'm') {
        *err = true;
        return false;
    }
    if (is_cmd(first)) {
        cmd = first;
        st->pos++;
    } else if ((is_dig(first) || first == '.' || first == '-' || first == '+') && *prev != 0) {
        if (*prev == 'Z' || *prev == 'z') {
            *err = true;
            return false;
        }
        if (*prev == 'M' || *prev == 'm') {
            implicit = true;
            cmd = *prev == 'M' ? 'L' : 'l';
        } else {
            cmd = *prev;
        }
    } else {
        *err = true;
        return false;
    }
    lc = (char)(cmd >= 'A' && cmd <= 'Z' ? cmd - 'A' + 'a' : cmd);
    memset(out, 0, sizeof *out);
    out->abs = cmd >= 'A' && cmd <= 'Z';
    switch (lc) {
    case 'm':
        out->k = PS_M;
        nv = 2;
        break;
    case 'l':
        out->k = PS_L;
        nv = 2;
        break;
    case 'h':
        out->k = PS_H;
        nv = 1;
        break;
    case 'v':
        out->k = PS_V;
        nv = 1;
        break;
    case 'c':
        out->k = PS_C;
        nv = 6;
        break;
    case 's':
        out->k = PS_S;
        nv = 4;
        break;
    case 'q':
        out->k = PS_Q;
        nv = 4;
        break;
    case 't':
        out->k = PS_T;
        nv = 2;
        break;
    case 'a':
        out->k = PS_A;
        nv = 0;
        break;
    default:
        out->k = PS_Z;
        nv = 0;
        break;
    }
    if (out->k == PS_A) {
        if (!ss_parse_list_number(st, &out->v[0]) || !ss_parse_list_number(st, &out->v[1]) ||
            !ss_parse_list_number(st, &out->v[2]) || !flag(st, &out->large) || !flag(st, &out->sweep) ||
            !ss_parse_list_number(st, &out->v[3]) || !ss_parse_list_number(st, &out->v[4])) {
            *err = true;
            return false;
        }
    } else {
        for (i = 0; i < nv; i++) {
            if (!ss_parse_list_number(st, &out->v[i])) {
                *err = true;
                return false;
            }
        }
    }
    *prev = implicit ? (out->abs ? 'M' : 'm') : cmd;
    return true;
}

typedef struct simp {
    double prev_mx, prev_my, prev_tx, prev_ty, prev_x, prev_y;
    pseg prev_seg; /* absolute */
    bool have_prev_simple;
    sv_segkind prev_simple;
    bool (*f)(void *ctx, const sv_seg *seg);
    void *ctx;
    bool last_set;
    sv_seg last;
} simp;

static bool emit(simp *s, sv_seg seg)
{
    s->last = seg;
    s->last_set = true;
    return s->f(s->ctx, &seg);
}

static bool arc_cb(void *ctx, const double c[6])
{
    simp *s = ctx;
    sv_seg seg;
    seg.kind = SEG_CURVE;
    seg.x1 = c[0];
    seg.y1 = c[1];
    seg.x2 = c[2];
    seg.y2 = c[3];
    seg.x = c[4];
    seg.y = c[5];
    return emit(s, seg);
}

bool sv_simplify_path(const char *d, bool (*f)(void *ctx, const sv_seg *seg), void *ctx)
{
    sv_stream st;
    simp s;
    char prev = 0;
    memset(&s, 0, sizeof s);
    s.prev_seg.k = PS_M;
    s.prev_seg.abs = true;
    s.f = f;
    s.ctx = ctx;
    ss_init(&st, d);
    for (;;) {
        pseg seg;
        bool err;
        sv_seg o;
        memset(&o, 0, sizeof o);
        if (!next_seg(&st, &prev, &seg, &err)) {
            return true; /* the end, or an error: usvg stops there */
        }
        s.last_set = false;
        /* A ClosePath followed by anything but MoveTo / ClosePath starts at the last MoveTo. */
        if (s.have_prev_simple && s.prev_simple == SEG_CLOSE && seg.k != PS_M && seg.k != PS_Z) {
            o.kind = SEG_MOVE;
            o.x = s.prev_mx;
            o.y = s.prev_my;
            if (!emit(&s, o)) {
                return false;
            }
            s.prev_simple = SEG_MOVE;
        }
        switch (seg.k) {
        case PS_M: {
            double x = seg.v[0];
            double y = seg.v[1];
            if (!seg.abs) {
                if (s.have_prev_simple && s.prev_simple == SEG_CLOSE) {
                    x += s.prev_mx;
                    y += s.prev_my;
                } else {
                    x += s.prev_x;
                    y += s.prev_y;
                }
            }
            o.kind = SEG_MOVE;
            o.x = x;
            o.y = y;
            if (!emit(&s, o)) {
                return false;
            }
            s.prev_seg = seg;
            break;
        }
        case PS_L:
        case PS_H:
        case PS_V: {
            double x = seg.k == PS_V ? s.prev_x : seg.v[0];
            double y = seg.k == PS_L ? seg.v[1] : seg.k == PS_H ? s.prev_y : seg.v[0];
            if (!seg.abs) {
                if (seg.k != PS_V) {
                    x += s.prev_x;
                }
                if (seg.k != PS_H) {
                    y += s.prev_y;
                }
            }
            o.kind = SEG_LINE;
            o.x = x;
            o.y = y;
            if (!emit(&s, o)) {
                return false;
            }
            s.prev_seg = seg;
            break;
        }
        case PS_C: {
            int i;
            if (!seg.abs) {
                for (i = 0; i < 6; i++) {
                    seg.v[i] += i % 2 == 0 ? s.prev_x : s.prev_y;
                }
            }
            o.kind = SEG_CURVE;
            o.x1 = seg.v[0];
            o.y1 = seg.v[1];
            o.x2 = seg.v[2];
            o.y2 = seg.v[3];
            o.x = seg.v[4];
            o.y = seg.v[5];
            if (!emit(&s, o)) {
                return false;
            }
            seg.abs = true;
            s.prev_seg = seg;
            break;
        }
        case PS_S: {
            double x1;
            double y1;
            int i;
            if (s.prev_seg.k == PS_C) {
                x1 = s.prev_seg.v[4] * 2.0 - s.prev_seg.v[2];
                y1 = s.prev_seg.v[5] * 2.0 - s.prev_seg.v[3];
            } else if (s.prev_seg.k == PS_S) {
                x1 = s.prev_seg.v[2] * 2.0 - s.prev_seg.v[0];
                y1 = s.prev_seg.v[3] * 2.0 - s.prev_seg.v[1];
            } else {
                x1 = s.prev_x;
                y1 = s.prev_y;
            }
            if (!seg.abs) {
                for (i = 0; i < 4; i++) {
                    seg.v[i] += i % 2 == 0 ? s.prev_x : s.prev_y;
                }
            }
            o.kind = SEG_CURVE;
            o.x1 = x1;
            o.y1 = y1;
            o.x2 = seg.v[0];
            o.y2 = seg.v[1];
            o.x = seg.v[2];
            o.y = seg.v[3];
            if (!emit(&s, o)) {
                return false;
            }
            seg.abs = true;
            s.prev_seg = seg;
            break;
        }
        case PS_Q: {
            int i;
            if (!seg.abs) {
                for (i = 0; i < 4; i++) {
                    seg.v[i] += i % 2 == 0 ? s.prev_x : s.prev_y;
                }
            }
            o.kind = SEG_QUAD;
            o.x1 = seg.v[0];
            o.y1 = seg.v[1];
            o.x = seg.v[2];
            o.y = seg.v[3];
            if (!emit(&s, o)) {
                return false;
            }
            seg.abs = true;
            s.prev_seg = seg;
            break;
        }
        case PS_T: {
            double x1;
            double y1;
            if (s.prev_seg.k == PS_Q) {
                x1 = s.prev_seg.v[2] * 2.0 - s.prev_seg.v[0];
                y1 = s.prev_seg.v[3] * 2.0 - s.prev_seg.v[1];
            } else if (s.prev_seg.k == PS_T) {
                x1 = s.prev_seg.v[0] * 2.0 - s.prev_tx;
                y1 = s.prev_seg.v[1] * 2.0 - s.prev_ty;
            } else {
                x1 = s.prev_x;
                y1 = s.prev_y;
            }
            s.prev_tx = x1;
            s.prev_ty = y1;
            if (!seg.abs) {
                seg.v[0] += s.prev_x;
                seg.v[1] += s.prev_y;
            }
            o.kind = SEG_QUAD;
            o.x1 = x1;
            o.y1 = y1;
            o.x = seg.v[0];
            o.y = seg.v[1];
            if (!emit(&s, o)) {
                return false;
            }
            seg.abs = true;
            s.prev_seg = seg;
            break;
        }
        case PS_A: {
            double x = seg.v[3];
            double y = seg.v[4];
            bool ok;
            if (!seg.abs) {
                x += s.prev_x;
                y += s.prev_y;
            }
            if (!sv_arc_to_cubics(s.prev_x, s.prev_y, seg.v[0], seg.v[1], seg.v[2], seg.large, seg.sweep, x,
                                  y, arc_cb, &s, &ok)) {
                o.kind = SEG_LINE;
                o.x = x;
                o.y = y;
                if (!emit(&s, o)) {
                    return false;
                }
            } else if (!ok) {
                return false;
            }
            s.prev_seg = seg;
            break;
        }
        case PS_Z:
        default:
            if (!(s.have_prev_simple && s.prev_simple == SEG_CLOSE)) {
                o.kind = SEG_CLOSE;
                if (!emit(&s, o)) {
                    return false;
                }
            }
            s.prev_seg = seg;
            break;
        }
        /* Remember the last position. */
        if (s.last_set) {
            s.have_prev_simple = true;
            s.prev_simple = s.last.kind;
            switch (s.last.kind) {
            case SEG_MOVE:
                s.prev_x = s.last.x;
                s.prev_y = s.last.y;
                s.prev_mx = s.prev_x;
                s.prev_my = s.prev_y;
                break;
            case SEG_LINE:
            case SEG_CURVE:
            case SEG_QUAD:
                s.prev_x = s.last.x;
                s.prev_y = s.last.y;
                break;
            case SEG_CLOSE:
            default:
                s.prev_x = s.prev_mx;
                s.prev_y = s.prev_my;
                break;
            }
        }
    }
}
