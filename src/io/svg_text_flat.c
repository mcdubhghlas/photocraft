/* Flattened text (port of usvg 0.45.1 text/flatten.rs and text/mod.rs): every span's glyph
 * outlines become one path (decorations: one path each, underline and overline before the
 * glyphs, line-through after), grouped and measured. Also the kurbo 0.11 arc-length code text on
 * a path uses (CubicBez::arclen / inv_arclen, solve_itp) and two Unicode properties
 * (unicode-vo's Upright orientation, the scripts letter-spacing skips).
 *
 * Gaps (REWRITE_PLAN.md section 7): COLR, SVG-in-OpenType and bitmap (CBDT/sbix) glyphs are drawn
 * from their outlines (usually none) instead of as colour groups or images. Arc lengths subdivide at
 * most 20 levels deep, as kurbo's. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <hb.h>

#include "io/svg_text.h"

/* ---------------------------------------------------------------- Unicode */

static const uint32_t UPRIGHT[123][2] = {
    {0xA7, 0xA7},       {0xA9, 0xA9},       {0xAE, 0xAE},         {0xB1, 0xB1},       {0xBC, 0xBE},
    {0xD7, 0xD7},       {0xF7, 0xF7},       {0x2EA, 0x2EB},       {0x1100, 0x11FF},   {0x1401, 0x167F},
    {0x18B0, 0x18FF},   {0x2016, 0x2016},   {0x2020, 0x2021},     {0x2030, 0x2031},   {0x203B, 0x203C},
    {0x2042, 0x2042},   {0x2047, 0x2049},   {0x2051, 0x2051},     {0x2065, 0x2065},   {0x20DD, 0x20E0},
    {0x20E2, 0x20E4},   {0x2100, 0x2101},   {0x2103, 0x2109},     {0x210F, 0x210F},   {0x2113, 0x2114},
    {0x2116, 0x2117},   {0x211E, 0x2123},   {0x2125, 0x2125},     {0x2127, 0x2127},   {0x2129, 0x2129},
    {0x212E, 0x212E},   {0x2135, 0x213F},   {0x2145, 0x214A},     {0x214C, 0x214D},   {0x214F, 0x2189},
    {0x218C, 0x218F},   {0x221E, 0x221E},   {0x2234, 0x2235},     {0x2300, 0x2307},   {0x230C, 0x231F},
    {0x2324, 0x2328},   {0x232B, 0x232B},   {0x237D, 0x239A},     {0x23BE, 0x23CD},   {0x23CF, 0x23CF},
    {0x23D1, 0x23DB},   {0x23E2, 0x2422},   {0x2424, 0x24FF},     {0x25A0, 0x2619},   {0x2620, 0x2767},
    {0x2776, 0x2793},   {0x2B12, 0x2B2F},   {0x2B50, 0x2B59},     {0x2BB8, 0x2BEB},   {0x2BF0, 0x2BFF},
    {0x2E80, 0x3000},   {0x3003, 0x3007},   {0x3012, 0x3013},     {0x3020, 0x302F},   {0x3031, 0x3040},
    {0x3042, 0x3042},   {0x3044, 0x3044},   {0x3046, 0x3046},     {0x3048, 0x3048},   {0x304A, 0x3062},
    {0x3064, 0x3082},   {0x3084, 0x3084},   {0x3086, 0x3086},     {0x3088, 0x308D},   {0x308F, 0x3094},
    {0x3097, 0x309A},   {0x309D, 0x309F},   {0x30A2, 0x30A2},     {0x30A4, 0x30A4},   {0x30A6, 0x30A6},
    {0x30A8, 0x30A8},   {0x30AA, 0x30C2},   {0x30C4, 0x30E2},     {0x30E4, 0x30E4},   {0x30E6, 0x30E6},
    {0x30E8, 0x30ED},   {0x30EF, 0x30F4},   {0x30F7, 0x30FB},     {0x30FD, 0x3126},   {0x3128, 0x31EF},
    {0x3200, 0x32FF},   {0x3358, 0x337A},   {0x3380, 0xA4CF},     {0xA960, 0xA97F},   {0xAC00, 0xD7FF},
    {0xE000, 0xFAFF},   {0xFE10, 0xFE1F},   {0xFE30, 0xFE48},     {0xFE53, 0xFE57},   {0xFE5F, 0xFE62},
    {0xFE67, 0xFE6F},   {0xFF02, 0xFF07},   {0xFF0A, 0xFF0B},     {0xFF0F, 0xFF19},   {0xFF20, 0xFF3A},
    {0xFF3C, 0xFF3C},   {0xFF3E, 0xFF3E},   {0xFF40, 0xFF5A},     {0xFFE0, 0xFFE2},   {0xFFE4, 0xFFE7},
    {0xFFF0, 0xFFF8},   {0xFFFC, 0xFFFD},   {0x10980, 0x1099F},   {0x11580, 0x115FF}, {0x13000, 0x1342F},
    {0x14400, 0x1467F}, {0x16FE0, 0x18AFF}, {0x1B000, 0x1B0FF},   {0x1D000, 0x1D1FF}, {0x1D300, 0x1D37F},
    {0x1D800, 0x1DAAF}, {0x1F000, 0x1F1FF}, {0x1F202, 0x1F7FF},   {0x1F900, 0x1F9FF}, {0x20000, 0x2FFFD},
    {0x30000, 0x3FFFD}, {0xF0000, 0xFFFFD}, {0x100000, 0x10FFFD},
};

bool svt_is_upright(uint32_t c)
{
    size_t lo = 0;
    size_t hi = sizeof UPRIGHT / sizeof UPRIGHT[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c < UPRIGHT[mid][0]) {
            hi = mid;
        } else if (c > UPRIGHT[mid][1]) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

bool svt_script_supports_letter_spacing(uint32_t c)
{
    switch (hb_unicode_script(hb_unicode_funcs_get_default(), c)) {
    case HB_SCRIPT_ARABIC:
    case HB_SCRIPT_SYRIAC:
    case HB_SCRIPT_NKO:
    case HB_SCRIPT_MANICHAEAN:
    case HB_SCRIPT_PSALTER_PAHLAVI:
    case HB_SCRIPT_MANDAIC:
    case HB_SCRIPT_MONGOLIAN:
    case HB_SCRIPT_PHAGS_PA:
    case HB_SCRIPT_DEVANAGARI:
    case HB_SCRIPT_BENGALI:
    case HB_SCRIPT_GURMUKHI:
    case HB_SCRIPT_MODI:
    case HB_SCRIPT_SHARADA:
    case HB_SCRIPT_SYLOTI_NAGRI:
    case HB_SCRIPT_TIRHUTA:
    case HB_SCRIPT_OGHAM:
        return false;
    default:
        return true;
    }
}

/* ---------------------------------------------------------------- kurbo arc length */

static const double GL_8[8][2] = {
    {0.3626837833783620, -0.1834346424956498}, {0.3626837833783620, 0.1834346424956498},
    {0.3137066458778873, -0.5255324099163290}, {0.3137066458778873, 0.5255324099163290},
    {0.2223810344533745, -0.7966664774136267}, {0.2223810344533745, 0.7966664774136267},
    {0.1012285362903763, -0.9602898564975363}, {0.1012285362903763, 0.9602898564975363},
};
static const double GL_8_HALF[4][2] = {
    {0.3626837833783620, 0.1834346424956498},
    {0.3137066458778873, 0.5255324099163290},
    {0.2223810344533745, 0.7966664774136267},
    {0.1012285362903763, 0.9602898564975363},
};
static const double GL_16_HALF[8][2] = {
    {0.1894506104550685, 0.0950125098376374}, {0.1826034150449236, 0.2816035507792589},
    {0.1691565193950025, 0.4580167776572274}, {0.1495959888165767, 0.6178762444026438},
    {0.1246289712555339, 0.7554044083550030}, {0.0951585116824928, 0.8656312023878318},
    {0.0622535239386479, 0.9445750230732326}, {0.0271524594117541, 0.9894009349916499},
};
static const double GL_24_HALF[12][2] = {
    {0.1279381953467522, 0.0640568928626056}, {0.1258374563468283, 0.1911188674736163},
    {0.1216704729278034, 0.3150426796961634}, {0.1155056680537256, 0.4337935076260451},
    {0.1074442701159656, 0.5454214713888396}, {0.0976186521041139, 0.6480936519369755},
    {0.0861901615319533, 0.7401241915785544}, {0.0733464814110803, 0.8200019859739029},
    {0.0592985849154368, 0.8864155270044011}, {0.0442774388174198, 0.9382745520027328},
    {0.0285313886289337, 0.9747285559713095}, {0.0123412297999872, 0.9951872199970213},
};

/* f64::powi as compiler-rt's __powidf2 (square-and-multiply). */
static double powi_d(double a, int b)
{
    double r = 1.0;
    int e = b < 0 ? -b : b;
    for (;;) {
        if ((e & 1) != 0) {
            r *= a;
        }
        e /= 2;
        if (e == 0) {
            break;
        }
        a *= a;
    }
    return b < 0 ? 1.0 / r : r;
}

static double fmin_k(double a, double k)
{
    return isnan(a) ? k : (a < k ? a : k);
}

static double quad_core(const double (*co)[2], size_t n, double dmx, double dmy, double dm1x, double dm1y,
                        double dm2x, double dm2y)
{
    double sum = -0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        double wi = co[i][0];
        double xi = co[i][1];
        double dx = dmx + dm2x * (xi * xi);
        double dy = dmy + dm2y * (xi * xi);
        double dpx = hypot(dx + dm1x * xi, dy + dm1y * xi);
        double dmxx = hypot(dx - dm1x * xi, dy - dm1y * xi);
        sum += (1.5 * wi) * (dpx + dmxx);
    }
    return sum;
}

static void subdivide(const svt_cubic *c, svt_cubic *a, svt_cubic *b)
{
    double mt = 0.5;
    double t = 0.5;
    double pmx =
        c->x0 * (mt * mt * mt) + (c->x1 * (mt * mt * 3.0) + (c->x2 * (mt * 3.0) + c->x3 * t) * t) * t;
    double pmy =
        c->y0 * (mt * mt * mt) + (c->y1 * (mt * mt * 3.0) + (c->y2 * (mt * 3.0) + c->y3 * t) * t) * t;
    a->x0 = c->x0;
    a->y0 = c->y0;
    a->x1 = 0.5 * (c->x0 + c->x1);
    a->y1 = 0.5 * (c->y0 + c->y1);
    a->x2 = (c->x0 + c->x1 * 2.0 + c->x2) * 0.25;
    a->y2 = (c->y0 + c->y1 * 2.0 + c->y2) * 0.25;
    a->x3 = pmx;
    a->y3 = pmy;
    b->x0 = pmx;
    b->y0 = pmy;
    b->x1 = (c->x1 + c->x2 * 2.0 + c->x3) * 0.25;
    b->y1 = (c->y1 + c->y2 * 2.0 + c->y3) * 0.25;
    b->x2 = 0.5 * (c->x2 + c->x3);
    b->y2 = 0.5 * (c->y2 + c->y3);
    b->x3 = c->x3;
    b->y3 = c->y3;
}

static double arclen_rec(const svt_cubic *c, double accuracy, unsigned depth)
{
    double d03x = c->x3 - c->x0, d03y = c->y3 - c->y0;
    double d01x = c->x1 - c->x0, d01y = c->y1 - c->y0;
    double d12x = c->x2 - c->x1, d12y = c->y2 - c->y1;
    double d23x = c->x3 - c->x2, d23y = c->y3 - c->y2;
    double lp_lc = hypot(d01x, d01y) + hypot(d12x, d12y) + hypot(d23x, d23y) - hypot(d03x, d03y);
    double dd1x = d12x - d01x, dd1y = d12y - d01y;
    double dd2x = d23x - d12x, dd2y = d23y - d12y;
    double dmx = 0.25 * (d01x + d23x) + 0.5 * d12x;
    double dmy = 0.25 * (d01y + d23y) + 0.5 * d12y;
    double dm1x = 0.5 * (dd2x + dd1x), dm1y = 0.5 * (dd2y + dd1y);
    double dm2x = 0.25 * (dd2x - dd1x), dm2y = 0.25 * (dd2y - dd1y);
    double est = -0.0;
    size_t i;
    svt_cubic a, b;
    for (i = 0; i < 8; i++) {
        double wi = GL_8[i][0];
        double xi = GL_8[i][1];
        double dx = dmx + dm1x * xi + dm2x * (xi * xi);
        double dy = dmy + dm1y * xi + dm2y * (xi * xi);
        double ddx = dm1x + dm2x * (2.0 * xi);
        double ddy = dm1y + dm2y * (2.0 * xi);
        est += wi * ((ddx * ddx + ddy * ddy) / (dx * dx + dy * dy));
    }
    if (fmin_k(powi_d(est, 3) * 2.5e-6, 3e-2) * lp_lc < accuracy) {
        return quad_core(GL_8_HALF, 4, dmx, dmy, dm1x, dm1y, dm2x, dm2y);
    }
    if (fmin_k(powi_d(est, 6) * 1.5e-11, 9e-3) * lp_lc < accuracy) {
        return quad_core(GL_16_HALF, 8, dmx, dmy, dm1x, dm1y, dm2x, dm2y);
    }
    if (fmin_k(powi_d(est, 9) * 3.5e-16, 3.5e-3) * lp_lc < accuracy || depth >= 20) {
        return quad_core(GL_24_HALF, 12, dmx, dmy, dm1x, dm1y, dm2x, dm2y);
    }
    subdivide(c, &a, &b);
    return arclen_rec(&a, accuracy * 0.5, depth + 1) + arclen_rec(&b, accuracy * 0.5, depth + 1);
}

double svt_cubic_arclen(const svt_cubic *c, double accuracy)
{
    return arclen_rec(c, accuracy, 0);
}

static void eval(const svt_cubic *c, double t, double *x, double *y)
{
    double mt = 1.0 - t;
    *x = c->x0 * (mt * mt * mt) + (c->x1 * (mt * mt * 3.0) + (c->x2 * (mt * 3.0) + c->x3 * t) * t) * t;
    *y = c->y0 * (mt * mt * mt) + (c->y1 * (mt * mt * 3.0) + (c->y2 * (mt * 3.0) + c->y3 * t) * t) * t;
}

static void deriv_eval(const svt_cubic *c, double t, double *x, double *y)
{
    double p0x = 3.0 * (c->x1 - c->x0), p0y = 3.0 * (c->y1 - c->y0);
    double p1x = 3.0 * (c->x2 - c->x1), p1y = 3.0 * (c->y2 - c->y1);
    double p2x = 3.0 * (c->x3 - c->x2), p2y = 3.0 * (c->y3 - c->y2);
    double mt = 1.0 - t;
    *x = p0x * (mt * mt) + (p1x * (mt * 2.0) + p2x * t) * t;
    *y = p0y * (mt * mt) + (p1y * (mt * 2.0) + p2y * t) * t;
}

static void subsegment(const svt_cubic *c, double t0, double t1, svt_cubic *o)
{
    double dx, dy;
    double scale = (t1 - t0) * (1.0 / 3.0);
    eval(c, t0, &o->x0, &o->y0);
    eval(c, t1, &o->x3, &o->y3);
    deriv_eval(c, t0, &dx, &dy);
    o->x1 = o->x0 + scale * dx;
    o->y1 = o->y0 + scale * dy;
    deriv_eval(c, t1, &dx, &dy);
    o->x2 = o->x3 - scale * dx;
    o->y2 = o->y3 - scale * dy;
}

double svt_cubic_inv_arclen(const svt_cubic *c, double arclen, double accuracy)
{
    double total;
    double t_last = 0.0;
    double arclen_last = 0.0;
    double epsilon;
    double n;
    double inner;
    double a = 0.0, b = 1.0, ya, yb;
    double scaled;
    unsigned nmax;
    double n12;
    unsigned guard = 0;
    if (arclen <= 0.0) {
        return 0.0;
    }
    total = svt_cubic_arclen(c, accuracy);
    if (arclen >= total) {
        return 1.0;
    }
    epsilon = accuracy / total;
    n = 1.0 - fmin_k(ceil(log2(epsilon)), 0.0);
    inner = accuracy / n;
    ya = -arclen;
    yb = total - arclen;
    n12 = ceil(log2((b - a) / epsilon)) - 1.0;
    n12 = isnan(n12) || n12 < 0.0 ? 0.0 : n12;
    nmax = 1u + (unsigned)(n12 > 4096.0 ? 4096.0 : n12);
    scaled = epsilon * (double)((uint64_t)1 << (nmax & 63u));
    while (b - a > 2.0 * epsilon && guard++ < 4096) {
        double x12 = 0.5 * (a + b);
        double r = scaled - 0.5 * (b - a);
        double xf = (yb * a - ya * b) / (yb - ya);
        double sigma = x12 - xf;
        double delta = 0.2 * ((b - a) * (b - a));
        double xt = delta <= fabs(x12 - xf) ? xf + copysign(delta, sigma) : x12;
        double xitp = fabs(xt - x12) <= r ? xt : x12 - copysign(r, sigma);
        double yitp;
        svt_cubic sub;
        double arc;
        if (xitp > t_last) {
            subsegment(c, t_last, xitp, &sub);
            arc = svt_cubic_arclen(&sub, inner);
            arclen_last += arc;
        } else {
            subsegment(c, xitp, t_last, &sub);
            arc = svt_cubic_arclen(&sub, inner);
            arclen_last += arc * -1.0;
        }
        t_last = xitp;
        yitp = arclen_last - arclen;
        if (yitp > 0.0) {
            b = xitp;
            yb = yitp;
        } else if (yitp < 0.0) {
            a = xitp;
            ya = yitp;
        } else {
            return xitp;
        }
        scaled *= 0.5;
    }
    return 0.5 * (a + b);
}

/* ---------------------------------------------------------------- path segments as cubics */

static void line_cubic(float px, float py, float x, float y, svt_cubic *c)
{
    double x0 = (double)px, y0 = (double)py, x1 = (double)x, y1 = (double)y;
    c->x0 = x0;
    c->y0 = y0;
    c->x1 = x0 + (x1 - x0) * 0.33;
    c->y1 = y0 + (y1 - y0) * 0.33;
    c->x2 = x0 + (x1 - x0) * 0.66;
    c->y2 = y0 + (y1 - y0) * 0.66;
    c->x3 = x1;
    c->y3 = y1;
}

void svt_path_cubics(const sv_path *p, void (*f)(void *ctx, const svt_cubic *c), void *ctx)
{
    float pmx, pmy, px, py;
    sv_piter it;
    sv_pseg seg;
    if (p == NULL || p->npts == 0) {
        return;
    }
    pmx = p->pts[0].x;
    pmy = p->pts[0].y;
    px = pmx;
    py = pmy;
    piter_init(&it, p);
    while (piter_next(&it, &seg)) {
        svt_cubic c;
        switch (seg.verb) {
        case V_MOVE:
            pmx = seg.p[0].x;
            pmy = seg.p[0].y;
            px = pmx;
            py = pmy;
            continue;
        case V_LINE:
            line_cubic(px, py, seg.p[0].x, seg.p[0].y, &c);
            break;
        case V_QUAD: {
            double x0 = (double)px, y0 = (double)py;
            double x1 = (double)seg.p[0].x, y1 = (double)seg.p[0].y;
            double x2 = (double)seg.p[1].x, y2 = (double)seg.p[1].y;
            c.x0 = x0;
            c.y0 = y0;
            c.x1 = x0 + (2.0 / 3.0) * (x1 - x0);
            c.y1 = y0 + (2.0 / 3.0) * (y1 - y0);
            c.x2 = x2 + (2.0 / 3.0) * (x1 - x2);
            c.y2 = y2 + (2.0 / 3.0) * (y1 - y2);
            c.x3 = x2;
            c.y3 = y2;
            break;
        }
        case V_CUBIC:
            c.x0 = (double)px;
            c.y0 = (double)py;
            c.x1 = (double)seg.p[0].x;
            c.y1 = (double)seg.p[0].y;
            c.x2 = (double)seg.p[1].x;
            c.y2 = (double)seg.p[1].y;
            c.x3 = (double)seg.p[2].x;
            c.y3 = (double)seg.p[2].y;
            break;
        case V_CLOSE:
        default:
            line_cubic(px, py, pmx, pmy, &c);
            break;
        }
        f(ctx, &c);
        px = (float)c.x3;
        py = (float)c.y3;
    }
}

/* ---------------------------------------------------------------- flatten */

static bool push_child(sv_conv *c, sv_unode *g, sv_unode *n)
{
    if (!sv_push_child(c->a, g, n)) {
        c->nomem = true;
        return false;
    }
    return true;
}

static bool pb_append(sv_pb *b, const sv_path *p)
{
    void *v = b->verbs;
    void *q;
    if (!sv_grow(b->a, &v, &b->capverbs, b->nverbs + p->nverbs, 1)) {
        b->failed = true;
        return false;
    }
    b->verbs = v;
    q = b->pts;
    if (!sv_grow(b->a, &q, &b->cappts, b->npts + p->npts, sizeof(sv_pt))) {
        b->failed = true;
        return false;
    }
    b->pts = q;
    b->last_move_to = b->npts;
    memcpy(b->verbs + b->nverbs, p->verbs, p->nverbs);
    memcpy(b->pts + b->npts, p->pts, p->npts * sizeof(sv_pt));
    b->nverbs += p->nverbs;
    b->npts += p->npts;
    return true;
}

sv_unode *svt_flatten(sv_conv *c, const svt_text *t, const svt_lspan *spans, size_t n, sv_rect *stroke_bbox)
{
    sv_unode *g = sv_group_empty(c->a);
    size_t s;
    if (g == NULL) {
        c->nomem = true;
        return NULL;
    }
    g->id = t->id;
    for (s = 0; s < n; s++) {
        const svt_lspan *sp = &spans[s];
        sv_pb pb;
        size_t i;
        sv_path *data;
        if (sp->overline != NULL && !push_child(c, g, sp->overline)) {
            return NULL;
        }
        if (sp->underline != NULL && !push_child(c, g, sp->underline)) {
            return NULL;
        }
        pb_init(&pb, c->a);
        for (i = 0; i < sp->nglyphs; i++) {
            const svt_pglyph *pg = &sp->glyphs[i];
            const sv_path *o = svt_outline(c->a, pg->font, pg->id);
            if (o == NULL) {
                if (c->a->nomem) {
                    c->nomem = true;
                    return NULL;
                }
                continue;
            }
            o = path_transform(c->a, o, svt_outline_ts(pg));
            if (o == NULL) {
                if (c->a->nomem) {
                    c->nomem = true;
                    return NULL;
                }
                continue;
            }
            if (!pb_append(&pb, o)) {
                c->nomem = true;
                return NULL;
            }
        }
        data = pb_finish(&pb);
        if (data != NULL) {
            sv_unode *u =
                sv_new_path(c, "", sp->visible, sp->fill, sp->stroke, sp->stroke_first, data, ts_identity());
            if (u != NULL) {
                u->crisp = t->crisp;
                if (!push_child(c, g, u)) {
                    return NULL;
                }
            } else if (c->nomem) {
                return NULL;
            }
        } else if (pb.failed) {
            c->nomem = true;
            return NULL;
        }
        if (sp->line_through != NULL && !push_child(c, g, sp->line_through)) {
            return NULL;
        }
    }
    (void)sv_group_bboxes(g);
    if (!rect_to_nz(g->stroke_bbox, stroke_bbox)) {
        return NULL;
    }
    return g;
}
