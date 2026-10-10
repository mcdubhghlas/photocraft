/* Filter Gallery: the filter table, the parameter notation, effect layers and their serde, and
 * the FilterParams variant (port of crates/algo/src/artistic.rs). */
#include "algo/artistic.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/artistic_fx.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

typedef struct gal_row {
    const char *key, *name, *category, *doc;
} gal_row;

static const gal_row ROWS[PC_ALGO_GALLERY_FILTERS] = {
    {"coloredPencil", "Colored Pencil", "Artistic",
     "{\"pencilWidth\":1..24=4,\"strokePressure\":0..15=8,\"paperBrightness\":0..50=25}"},
    {"cutout", "Cutout", "Artistic",
     "{\"numberOfLevels\":2..8=4,\"edgeSimplicity\":0..10=4,\"edgeFidelity\":1..3=2}"},
    {"dryBrush", "Dry Brush", "Artistic",
     "{\"brushSize\":0..10=2,\"brushDetail\":0..10=8,\"texture\":1..3=1}"},
    {"filmGrain", "Film Grain", "Artistic",
     "{\"grain\":0..20=4,\"highlightArea\":0..20=0,\"intensity\":0..10=10}"},
    {"fresco", "Fresco", "Artistic", "{\"brushSize\":0..10=2,\"brushDetail\":0..10=8,\"texture\":1..3=1}"},
    {"neonGlow", "Neon Glow", "Artistic",
     "{\"glowSize\":-24..24=5,\"glowBrightness\":0..50=15,\"glowColor\":json}"},
    {"paintDaubs", "Paint Daubs", "Artistic",
     "{\"brushSize\":1..50=8,\"sharpness\":0..40=7,"
     "\"brushType\":\"simple|lightRough|darkRough|wideSharp|wideBlurry|sparkle\"}"},
    {"paletteKnife", "Palette Knife", "Artistic",
     "{\"strokeSize\":1..50=25,\"strokeDetail\":1..3=3,\"softness\":0..10=0}"},
    {"plasticWrap", "Plastic Wrap", "Artistic",
     "{\"highlightStrength\":0..20=15,\"detail\":1..15=9,\"smoothness\":1..15=7}"},
    {"posterEdges", "Poster Edges", "Artistic",
     "{\"edgeThickness\":0..10=2,\"edgeIntensity\":0..10=1,\"posterization\":0..6=2}"},
    {"roughPastels", "Rough Pastels", "Artistic",
     "{\"strokeLength\":0..40=6,\"strokeDetail\":1..20=4,\"texture\":\"canvas|brick|burlap|sandstone\","
     "\"scaling\":50..200=100,\"relief\":0..50=20,"
     "\"light\":\"bottom|bottomLeft|left|topLeft|top|topRight|right|bottomRight\",\"invert\":bool}"},
    {"smudgeStick", "Smudge Stick", "Artistic",
     "{\"strokeLength\":0..10=2,\"highlightArea\":0..20=0,\"intensity\":0..10=10}"},
    {"sponge", "Sponge", "Artistic",
     "{\"brushSize\":0..10=2,\"definition\":0..25=12,\"smoothness\":1..15=5}"},
    {"underpainting", "Underpainting", "Artistic",
     "{\"brushSize\":0..40=6,\"textureCoverage\":0..40=16,\"texture\":\"canvas|brick|burlap|sandstone\","
     "\"scaling\":50..200=100,\"relief\":0..50=4,"
     "\"light\":\"top|topRight|right|bottomRight|bottom|bottomLeft|left|topLeft\",\"invert\":bool}"},
    {"watercolor", "Watercolor", "Artistic",
     "{\"brushDetail\":1..14=9,\"shadowIntensity\":0..10=1,\"texture\":1..3=1}"},
    {"accentedEdges", "Accented Edges", "Brush Strokes",
     "{\"edgeWidth\":1..14=2,\"edgeBrightness\":0..50=38,\"smoothness\":1..15=5}"},
    {"angledStrokes", "Angled Strokes", "Brush Strokes",
     "{\"directionBalance\":0..100=50,\"strokeLength\":3..50=15,\"sharpness\":0..10=3}"},
    {"crosshatch", "Crosshatch", "Brush Strokes",
     "{\"strokeLength\":3..50=9,\"sharpness\":0..20=6,\"strength\":1..3=1}"},
    {"darkStrokes", "Dark Strokes", "Brush Strokes",
     "{\"balance\":0..10=5,\"blackIntensity\":0..10=6,\"whiteIntensity\":0..10=2}"},
    {"inkOutlines", "Ink Outlines", "Brush Strokes",
     "{\"strokeLength\":1..50=4,\"darkIntensity\":0..50=20,\"lightIntensity\":0..50=10}"},
    {"spatter", "Spatter", "Brush Strokes", "{\"sprayRadius\":0..25=10,\"smoothness\":1..15=5}"},
    {"sprayedStrokes", "Sprayed Strokes", "Brush Strokes",
     "{\"strokeLength\":0..20=12,\"sprayRadius\":0..25=7,"
     "\"strokeDirection\":\"rightDiagonal|horizontal|leftDiagonal|vertical\"}"},
    {"sumiE", "Sumi-e", "Brush Strokes",
     "{\"strokeWidth\":3..15=10,\"strokePressure\":0..15=2,\"contrast\":0..40=16}"},
    {"diffuseGlow", "Diffuse Glow", "Distort",
     "{\"graininess\":0..10=6,\"glowAmount\":0..20=10,\"clearAmount\":0..20=15,\"background\":json}"},
    {"glass", "Glass", "Distort",
     "{\"distortion\":0..20=5,\"smoothness\":1..15=3,\"texture\":\"frosted|blocks|canvas|tinyLens\","
     "\"scaling\":50..200=100,\"invert\":bool}"},
    {"oceanRipple", "Ocean Ripple", "Distort", "{\"rippleSize\":1..15=9,\"rippleMagnitude\":0..20=9}"},
    {"basRelief", "Bas Relief", "Sketch",
     "{\"detail\":1..15=13,\"smoothness\":1..15=3,"
     "\"light\":\"bottom|bottomLeft|left|topLeft|top|topRight|right|bottomRight\",\"foreground\":json,"
     "\"background\":json}"},
    {"chalkCharcoal", "Chalk & Charcoal", "Sketch",
     "{\"charcoalArea\":0..20=6,\"chalkArea\":0..20=6,\"strokePressure\":0..5=1,\"foreground\":json,"
     "\"background\":json}"},
    {"charcoal", "Charcoal", "Sketch",
     "{\"charcoalThickness\":1..7=1,\"detail\":0..5=5,\"lightDarkBalance\":0..100=50,\"foreground\":json,"
     "\"background\":json}"},
    {"chrome", "Chrome", "Sketch", "{\"detail\":0..10=4,\"smoothness\":0..10=7}"},
    {"conteCrayon", "Cont\xc3\xa9 Crayon", "Sketch",
     "{\"foregroundLevel\":1..15=11,\"backgroundLevel\":1..15=7,\"texture\":\"canvas|brick|burlap|"
     "sandstone\","
     "\"scaling\":50..200=100,\"relief\":0..50=4,"
     "\"light\":\"top|topRight|right|bottomRight|bottom|bottomLeft|left|topLeft\",\"invert\":bool,"
     "\"foreground\":json,\"background\":json}"},
    {"graphicPen", "Graphic Pen", "Sketch",
     "{\"strokeLength\":1..15=15,\"lightDarkBalance\":0..100=50,"
     "\"strokeDirection\":\"rightDiagonal|horizontal|leftDiagonal|vertical\",\"foreground\":json,"
     "\"background\":json}"},
    {"halftonePattern", "Halftone Pattern", "Sketch",
     "{\"size\":1..12=1,\"contrast\":0..50=5,\"patternType\":\"dot|circle|line\",\"foreground\":json,"
     "\"background\":json}"},
    {"notePaper", "Note Paper", "Sketch",
     "{\"imageBalance\":0..50=25,\"graininess\":0..20=10,\"relief\":0..25=11,\"foreground\":json,"
     "\"background\":json}"},
    {"photocopy", "Photocopy", "Sketch",
     "{\"detail\":1..24=7,\"darkness\":1..50=8,\"foreground\":json,\"background\":json}"},
    {"plaster", "Plaster", "Sketch",
     "{\"imageBalance\":0..50=20,\"smoothness\":1..15=2,"
     "\"light\":\"top|topRight|right|bottomRight|bottom|bottomLeft|left|topLeft\",\"foreground\":json,"
     "\"background\":json}"},
    {"reticulation", "Reticulation", "Sketch",
     "{\"density\":0..50=12,\"foregroundLevel\":0..50=40,\"backgroundLevel\":0..50=5,\"foreground\":json,"
     "\"background\":json}"},
    {"stamp", "Stamp", "Sketch",
     "{\"lightDarkBalance\":0..50=25,\"smoothness\":1..50=5,\"foreground\":json,\"background\":json}"},
    {"tornEdges", "Torn Edges", "Sketch",
     "{\"imageBalance\":0..50=25,\"smoothness\":1..15=11,\"contrast\":1..25=17,\"foreground\":json,"
     "\"background\":json}"},
    {"waterPaper", "Water Paper", "Sketch",
     "{\"fiberLength\":3..50=15,\"brightness\":0..100=60,\"contrast\":0..100=80}"},
    {"glowingEdges", "Glowing Edges", "Stylize",
     "{\"edgeWidth\":1..14=2,\"edgeBrightness\":0..20=6,\"smoothness\":1..15=5}"},
    {"craquelure", "Craquelure", "Texture",
     "{\"crackSpacing\":2..100=15,\"crackDepth\":0..10=6,\"crackBrightness\":0..10=9}"},
    {"grain", "Grain", "Texture",
     "{\"intensity\":0..100=40,\"contrast\":0..100=50,"
     "\"grainType\":\"regular|soft|sprinkles|clumped|contrasty|enlarged|stippled|horizontal|vertical|"
     "speckle\","
     "\"foreground\":json,\"background\":json}"},
    {"mosaicTiles", "Mosaic Tiles", "Texture",
     "{\"tileSize\":2..100=12,\"groutWidth\":1..15=3,\"lightenGrout\":0..10=9}"},
    {"patchwork", "Patchwork", "Texture", "{\"squareSize\":0..10=4,\"relief\":0..25=8}"},
    {"stainedGlass", "Stained Glass", "Texture",
     "{\"cellSize\":2..50=10,\"borderThickness\":1..20=4,\"lightIntensity\":0..10=3,\"foreground\":json}"},
    {"texturizer", "Texturizer", "Texture",
     "{\"texture\":\"canvas|brick|burlap|sandstone\",\"scaling\":50..200=100,\"relief\":0..50=4,"
     "\"light\":\"top|topRight|right|bottomRight|bottom|bottomLeft|left|topLeft\",\"invert\":bool}"},
};

static const char *const KEYS[PC_ALGO_GALLERY_FILTERS] = {
    "coloredPencil", "cutout",        "dryBrush",        "filmGrain",      "fresco",        "neonGlow",
    "paintDaubs",    "paletteKnife",  "plasticWrap",     "posterEdges",    "roughPastels",  "smudgeStick",
    "sponge",        "underpainting", "watercolor",      "accentedEdges",  "angledStrokes", "crosshatch",
    "darkStrokes",   "inkOutlines",   "spatter",         "sprayedStrokes", "sumiE",         "diffuseGlow",
    "glass",         "oceanRipple",   "basRelief",       "chalkCharcoal",  "charcoal",      "chrome",
    "conteCrayon",   "graphicPen",    "halftonePattern", "notePaper",      "photocopy",     "plaster",
    "reticulation",  "stamp",         "tornEdges",       "waterPaper",     "glowingEdges",  "craquelure",
    "grain",         "mosaicTiles",   "patchwork",       "stainedGlass",   "texturizer",
};

const pc_algo_enum_desc PC_ALGO_ENUM_GALLERY_FILTER = {"GalleryFilter", KEYS, PC_ALGO_GALLERY_FILTERS, 0};

const char *const pc_algo_gallery_categories[6] = {"Artistic", "Brush Strokes", "Distort",
                                                   "Sketch",   "Stylize",       "Texture"};
const float pc_algo_neon_default[4] = {0.0f, 0.62f, 1.0f, 1.0f};

static bool valid(pc_algo_gallery_filter f)
{
    return (int)f >= 0 && f < PC_ALGO_GALLERY_FILTERS;
}

const char *pc_algo_gallery_filter_key(pc_algo_gallery_filter f)
{
    return valid(f) ? ROWS[f].key : "";
}

const char *pc_algo_gallery_filter_command_id(pc_algo_gallery_filter f)
{
    /* "filter.gallery." + the longest key (15) fits. */
    static const char *const IDS[PC_ALGO_GALLERY_FILTERS] = {
        "filter.gallery.coloredPencil",  "filter.gallery.cutout",        "filter.gallery.dryBrush",
        "filter.gallery.filmGrain",      "filter.gallery.fresco",        "filter.gallery.neonGlow",
        "filter.gallery.paintDaubs",     "filter.gallery.paletteKnife",  "filter.gallery.plasticWrap",
        "filter.gallery.posterEdges",    "filter.gallery.roughPastels",  "filter.gallery.smudgeStick",
        "filter.gallery.sponge",         "filter.gallery.underpainting", "filter.gallery.watercolor",
        "filter.gallery.accentedEdges",  "filter.gallery.angledStrokes", "filter.gallery.crosshatch",
        "filter.gallery.darkStrokes",    "filter.gallery.inkOutlines",   "filter.gallery.spatter",
        "filter.gallery.sprayedStrokes", "filter.gallery.sumiE",         "filter.gallery.diffuseGlow",
        "filter.gallery.glass",          "filter.gallery.oceanRipple",   "filter.gallery.basRelief",
        "filter.gallery.chalkCharcoal",  "filter.gallery.charcoal",      "filter.gallery.chrome",
        "filter.gallery.conteCrayon",    "filter.gallery.graphicPen",    "filter.gallery.halftonePattern",
        "filter.gallery.notePaper",      "filter.gallery.photocopy",     "filter.gallery.plaster",
        "filter.gallery.reticulation",   "filter.gallery.stamp",         "filter.gallery.tornEdges",
        "filter.gallery.waterPaper",     "filter.gallery.glowingEdges",  "filter.gallery.craquelure",
        "filter.gallery.grain",          "filter.gallery.mosaicTiles",   "filter.gallery.patchwork",
        "filter.gallery.stainedGlass",   "filter.gallery.texturizer",
    };
    return valid(f) ? IDS[f] : "";
}

const char *pc_algo_gallery_filter_name(pc_algo_gallery_filter f)
{
    return valid(f) ? ROWS[f].name : "";
}

const char *pc_algo_gallery_filter_category(pc_algo_gallery_filter f)
{
    return valid(f) ? ROWS[f].category : "";
}

const char *pc_algo_gallery_filter_params_doc(pc_algo_gallery_filter f)
{
    return valid(f) ? ROWS[f].doc : "";
}

static bool ascii_eq_ci(const char *a, size_t alen, const char *b)
{
    size_t i, blen = strlen(b);
    if (alen != blen) {
        return false;
    }
    for (i = 0; i < alen; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x < 128 && y < 128) {
            if (tolower(x) != tolower(y)) {
                return false;
            }
        } else if (x != y) {
            return false;
        }
    }
    return true;
}

bool pc_algo_gallery_filter_from_key(const char *k, size_t len, pc_algo_gallery_filter *out)
{
    static const char prefix[] = "filter.gallery.";
    size_t i, pl = sizeof prefix - 1;
    if (len >= pl && memcmp(k, prefix, pl) == 0) {
        k += pl;
        len -= pl;
    }
    for (i = 0; i < PC_ALGO_GALLERY_FILTERS; i++) {
        if ((strlen(ROWS[i].key) == len && memcmp(ROWS[i].key, k, len) == 0) ||
            ascii_eq_ci(k, len, ROWS[i].name)) {
            *out = (pc_algo_gallery_filter)i;
            return true;
        }
    }
    return false;
}

bool pc_algo_gallery_filter_uses_colours(pc_algo_gallery_filter f)
{
    const char *doc = pc_algo_gallery_filter_params_doc(f);
    return f == PC_ALGO_GAL_COLORED_PENCIL || f == PC_ALGO_GAL_NEON_GLOW ||
           strstr(doc, "\"foreground\"") != NULL || strstr(doc, "\"background\"") != NULL;
}

/* ---- the parameter notation ---- */

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* str::trim. */
static void trim(const char **s, size_t *n)
{
    while (*n > 0 && is_ws(**s)) {
        (*s)++, (*n)--;
    }
    while (*n > 0 && is_ws((*s)[*n - 1])) {
        (*n)--;
    }
}

static void copy_name(char *dst, const char *s, size_t n)
{
    if (n >= PC_ALGO_GALLERY_NAME_MAX) {
        n = PC_ALGO_GALLERY_NAME_MAX - 1;
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/* `s.trim().parse::<f32>().unwrap_or(0.0)` for the notation's plain decimal numbers. */
static float parse_num(const char *s, size_t n)
{
    char tmp[32];
    char *end;
    float v;
    trim(&s, &n);
    if (n == 0 || n >= sizeof tmp) {
        return 0.0f;
    }
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    v = strtof(tmp, &end);
    return *end == '\0' ? v : 0.0f;
}

static const char *find_in(const char *s, size_t n, const char *pat)
{
    size_t i, pn = strlen(pat);
    for (i = 0; i + pn <= n; i++) {
        if (memcmp(s + i, pat, pn) == 0) {
            return s + i;
        }
    }
    return NULL;
}

size_t pc_algo_gallery_params(pc_algo_gallery_filter f, pc_algo_gallery_param *out, size_t cap)
{
    const char *doc = pc_algo_gallery_filter_params_doc(f), *inner = doc, *part;
    size_t n = strlen(doc), count = 0;
    trim(&inner, &n);
    while (n > 0 && *inner == '{') {
        inner++, n--;
    }
    while (n > 0 && inner[n - 1] == '}') {
        n--;
    }
    part = inner;
    while (part <= inner + n) {
        const char *end = memchr(part, ',', (size_t)(inner + n - part)), *colon, *k, *v;
        size_t plen = end != NULL ? (size_t)(end - part) : (size_t)(inner + n - part), kn, vn;
        pc_algo_gallery_param p;
        colon = memchr(part, ':', plen);
        if (colon != NULL && count < cap) {
            memset(&p, 0, sizeof p);
            k = part;
            kn = (size_t)(colon - part);
            v = colon + 1;
            vn = plen - kn - 1;
            trim(&k, &kn);
            while (kn > 0 && *k == '"') {
                k++, kn--;
            }
            while (kn > 0 && k[kn - 1] == '"') {
                kn--;
            }
            trim(&v, &vn);
            copy_name(p.key, k, kn);
            if (vn > 0 && *v == '"') {
                const char *body = v + 1, *c;
                size_t bn = vn - 1;
                while (bn > 0 && body[bn - 1] == '"') {
                    bn--;
                }
                p.kind = PC_ALGO_GALLERY_CHOICE;
                c = body;
                while (p.nchoices < PC_ALGO_GALLERY_MAX_CHOICES) {
                    const char *bar = memchr(c, '|', (size_t)(body + bn - c));
                    size_t cn = bar != NULL ? (size_t)(bar - c) : (size_t)(body + bn - c);
                    copy_name(p.choices[p.nchoices++], c, cn);
                    if (bar == NULL) {
                        break;
                    }
                    c = bar + 1;
                }
                out[count++] = p;
            } else if (vn == 4 && memcmp(v, "bool", 4) == 0) {
                p.kind = PC_ALGO_GALLERY_BOOL;
                out[count++] = p;
            } else {
                const char *eq = memchr(v, '=', vn);
                if (eq != NULL) {
                    const char *dots = find_in(v, (size_t)(eq - v), "..");
                    p.kind = PC_ALGO_GALLERY_RANGE;
                    if (dots != NULL) {
                        p.min = parse_num(v, (size_t)(dots - v));
                        p.max = parse_num(dots + 2, (size_t)(eq - dots - 2));
                    } else {
                        p.min = 0.0f;
                        p.max = 1.0f;
                    }
                    p.def = parse_num(eq + 1, (size_t)(v + vn - eq - 1));
                    out[count++] = p;
                }
                /* else: json (colours) */
            }
        }
        if (end == NULL) {
            break;
        }
        part = end + 1;
    }
    return count;
}

static bool param_of(pc_algo_gallery_filter f, const char *key, pc_algo_gallery_param *out)
{
    pc_algo_gallery_param ps[PC_ALGO_GALLERY_MAX_PARAMS];
    size_t n = pc_algo_gallery_params(f, ps, PC_ALGO_GALLERY_MAX_PARAMS), i;
    for (i = 0; i < n; i++) {
        if (strcmp(ps[i].key, key) == 0) {
            *out = ps[i];
            return true;
        }
    }
    return false;
}

/* ---- effects ---- */

static int key_cmp(const char *a, size_t an, const char *b, size_t bn)
{
    size_t m = an < bn ? an : bn;
    int c = m > 0 ? memcmp(a, b, m) : 0;
    if (c != 0) {
        return c;
    }
    return an < bn ? -1 : (an > bn ? 1 : 0);
}

/* Index of key in the sorted values, or where it would go (*found false). */
static size_t find_value(const pc_algo_gallery_effect *e, const char *key, size_t len, bool *found)
{
    size_t lo = 0, hi = e->nvalues;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = key_cmp(e->values[mid].key, e->values[mid].key_len, key, len);
        if (c == 0) {
            *found = true;
            return mid;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = false;
    return lo;
}

pc_status pc_algo_gallery_effect_put(pc_algo_gallery_effect *e, const char *key, size_t len, float v,
                                     pc_error *err)
{
    bool found;
    size_t i = find_value(e, key, len, &found);
    pc_algo_gallery_value *nv;
    char *k;
    if (found && e->values != NULL) {
        e->values[i].v = v;
        return PC_OK;
    }
    k = malloc(len + 1);
    nv = pc_malloc_array(e->nvalues + 1, sizeof *nv);
    if (k == NULL || nv == NULL) {
        free(k);
        free(nv);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a gallery value");
    }
    if (len > 0) {
        memcpy(k, key, len);
    }
    k[len] = '\0';
    if (e->nvalues > 0 && e->values != NULL) {
        memcpy(nv, e->values, i * sizeof *nv);
        memcpy(nv + i + 1, e->values + i, (e->nvalues - i) * sizeof *nv);
    }
    free(e->values);
    e->values = nv;
    e->values[i].key = k;
    e->values[i].key_len = len;
    e->values[i].v = v;
    e->nvalues++;
    return PC_OK;
}

static void effect_init(pc_algo_gallery_effect *e, pc_algo_gallery_filter f)
{
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f}, white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    memset(e, 0, sizeof *e);
    e->filter = f;
    memcpy(e->foreground, black, sizeof black);
    memcpy(e->background, white, sizeof white);
    memcpy(e->color, pc_algo_neon_default, sizeof e->color);
}

pc_status pc_algo_gallery_effect_new(pc_algo_gallery_filter f, pc_algo_gallery_effect *out, pc_error *err)
{
    pc_algo_gallery_param ps[PC_ALGO_GALLERY_MAX_PARAMS];
    size_t n = pc_algo_gallery_params(f, ps, PC_ALGO_GALLERY_MAX_PARAMS), i;
    pc_algo_gallery_effect e;
    effect_init(&e, f);
    for (i = 0; i < n; i++) {
        float v = ps[i].kind == PC_ALGO_GALLERY_RANGE  ? ps[i].def
                  : ps[i].kind == PC_ALGO_GALLERY_BOOL ? (ps[i].flag ? 1.0f : 0.0f)
                                                       : 0.0f;
        pc_status st = pc_algo_gallery_effect_put(&e, ps[i].key, strlen(ps[i].key), v, err);
        if (st != PC_OK) {
            pc_algo_gallery_effect_fini(&e);
            return st;
        }
    }
    *out = e;
    return PC_OK;
}

void pc_algo_gallery_effect_fini(pc_algo_gallery_effect *e)
{
    size_t i;
    if (e == NULL) {
        return;
    }
    for (i = 0; i < e->nvalues; i++) {
        free(e->values[i].key);
    }
    free(e->values);
    e->values = NULL;
    e->nvalues = 0;
}

pc_status pc_algo_gallery_effect_clone(const pc_algo_gallery_effect *e, pc_algo_gallery_effect *out,
                                       pc_error *err)
{
    pc_algo_gallery_effect c = *e;
    size_t i;
    c.values = NULL;
    c.nvalues = 0;
    if (e->nvalues > 0) {
        c.values = pc_calloc_array(e->nvalues, sizeof *c.values);
        if (c.values == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a gallery effect");
        }
    }
    for (i = 0; i < e->nvalues; i++) {
        char *k = malloc(e->values[i].key_len + 1);
        if (k == NULL) {
            pc_algo_gallery_effect_fini(&c);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a gallery effect");
        }
        memcpy(k, e->values[i].key, e->values[i].key_len + 1);
        c.values[i] = e->values[i];
        c.values[i].key = k;
        c.nvalues = i + 1;
    }
    *out = c;
    return PC_OK;
}

pc_status pc_algo_gallery_effect_set(pc_algo_gallery_effect *e, const char *key, float v, pc_error *err)
{
    pc_algo_gallery_param p;
    if (!param_of(e->filter, key, &p)) {
        return PC_OK;
    }
    switch (p.kind) {
    case PC_ALGO_GALLERY_RANGE:
        v = pc_clampf(v, p.min, p.max);
        break;
    case PC_ALGO_GALLERY_CHOICE:
        v = pc_clampf(roundf(v), 0.0f, (float)p.nchoices - 1.0f);
        break;
    default:
        v = v != 0.0f ? 1.0f : 0.0f;
        break;
    }
    return pc_algo_gallery_effect_put(e, key, strlen(key), v, err);
}

pc_status pc_algo_gallery_effect_set_choice(pc_algo_gallery_effect *e, const char *key, const char *name,
                                            bool *ok, pc_error *err)
{
    pc_algo_gallery_param p;
    size_t i;
    *ok = false;
    if (!param_of(e->filter, key, &p) || p.kind != PC_ALGO_GALLERY_CHOICE) {
        return PC_OK;
    }
    for (i = 0; i < p.nchoices; i++) {
        if (ascii_eq_ci(name, strlen(name), p.choices[i])) {
            pc_status st = pc_algo_gallery_effect_put(e, key, strlen(key), (float)i, err);
            if (st == PC_OK) {
                *ok = true;
            }
            return st;
        }
    }
    return PC_OK;
}

float pc_algo_gallery_effect_get(const pc_algo_gallery_effect *e, const char *key)
{
    pc_algo_gallery_param p;
    bool found;
    size_t i = find_value(e, key, strlen(key), &found);
    if (found) {
        return e->values[i].v;
    }
    if (!param_of(e->filter, key, &p)) {
        return 0.0f;
    }
    if (p.kind == PC_ALGO_GALLERY_RANGE) {
        return p.def;
    }
    if (p.kind == PC_ALGO_GALLERY_BOOL) {
        return p.flag ? 1.0f : 0.0f;
    }
    return 0.0f;
}

bool pc_algo_gallery_effect_flag(const pc_algo_gallery_effect *e, const char *key)
{
    return pc_algo_gallery_effect_get(e, key) != 0.0f;
}

const char *pc_algo_gallery_effect_choice(const pc_algo_gallery_effect *e, const char *key,
                                          char buf[PC_ALGO_GALLERY_NAME_MAX])
{
    pc_algo_gallery_param p;
    buf[0] = '\0';
    if (param_of(e->filter, key, &p) && p.kind == PC_ALGO_GALLERY_CHOICE && p.nchoices > 0) {
        size_t i = pc_f32_to_size(pc_rs_maxf_k(pc_algo_gallery_effect_get(e, key), 0.0f));
        (void)snprintf(buf, PC_ALGO_GALLERY_NAME_MAX, "%s", p.choices[i < p.nchoices ? i : 0]);
    }
    return buf;
}

/* ---- serde ---- */

/* values: BTreeMap<String, f32>. */
static pc_status read_values(yyjson_val *v, void *slot, pc_error *err)
{
    pc_algo_gallery_effect *e =
        (pc_algo_gallery_effect *)((char *)slot - offsetof(pc_algo_gallery_effect, values));
    yyjson_obj_iter it;
    yyjson_val *k;
    if (!yyjson_is_obj(v)) {
        return pc_algo_jfail_type(v, "a map", err);
    }
    yyjson_obj_iter_init(v, &it);
    while ((k = yyjson_obj_iter_next(&it)) != NULL) {
        float f;
        pc_status st = pc_algo_jread_f32(yyjson_obj_iter_get_val(k), &f, err);
        if (st == PC_OK) {
            st = pc_algo_gallery_effect_put(e, yyjson_get_str(k), yyjson_get_len(k), f, err);
        }
        if (st != PC_OK) {
            pc_algo_gallery_effect_fini(e);
            return st;
        }
    }
    return PC_OK;
}

static yyjson_mut_val *write_f32(yyjson_mut_doc *d, float v)
{
    return isfinite(v) ? yyjson_mut_float(d, v) : yyjson_mut_null(d);
}

static yyjson_mut_val *write_values(yyjson_mut_doc *d, const void *slot)
{
    const pc_algo_gallery_effect *e =
        (const pc_algo_gallery_effect *)(const void *)((const char *)slot -
                                                       offsetof(pc_algo_gallery_effect, values));
    yyjson_mut_val *o = yyjson_mut_obj(d);
    size_t i;
    if (o == NULL) {
        return NULL;
    }
    for (i = 0; i < e->nvalues; i++) {
        yyjson_mut_val *k = yyjson_mut_strncpy(d, e->values[i].key, e->values[i].key_len), *v;
        v = write_f32(d, e->values[i].v);
        if (k == NULL || v == NULL || !yyjson_mut_obj_add(o, k, v)) {
            return NULL;
        }
    }
    return o;
}

/* The values live in the effect itself (read_values reaches back from the slot), so the generic
 * fini/copy aren't used for this field: the effect's own fini/clone handle it. */
static const pc_algo_jfield EFFECT_F[] = {
    PC_ALGO_JFE("filter", pc_algo_gallery_effect, filter, PC_ALGO_ENUM_GALLERY_FILTER, false),
    {"values", PC_ALGO_JF_CUSTOM, offsetof(pc_algo_gallery_effect, values), true, NULL, 0, read_values,
     write_values, NULL, NULL},
    PC_ALGO_JFA("foreground", pc_algo_gallery_effect, foreground, 4, true),
    PC_ALGO_JFA("background", pc_algo_gallery_effect, background, 4, true),
    PC_ALGO_JFA("color", pc_algo_gallery_effect, color, 4, true),
};

pc_status pc_algo_gallery_effect_from_json(yyjson_val *v, pc_algo_gallery_effect *out, pc_error *err)
{
    pc_algo_gallery_effect e;
    pc_status st;
    effect_init(&e, PC_ALGO_GAL_COLORED_PENCIL);
    st = pc_algo_jfields_read(EFFECT_F, sizeof EFFECT_F / sizeof EFFECT_F[0], v, &e, "struct GalleryEffect",
                              NULL, err);
    if (st != PC_OK) {
        pc_algo_gallery_effect_fini(&e);
        return st;
    }
    *out = e;
    return PC_OK;
}

yyjson_mut_val *pc_algo_gallery_effect_to_json(yyjson_mut_doc *d, const pc_algo_gallery_effect *e)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (o == NULL || !pc_algo_jfields_write(EFFECT_F, sizeof EFFECT_F / sizeof EFFECT_F[0], d, o, e)) {
        return NULL;
    }
    return o;
}

static void effects_fini(void *slot)
{
    pc_algo_gallery_effects *es = slot;
    size_t i;
    for (i = 0; i < es->n; i++) {
        pc_algo_gallery_effect_fini(&es->v[i]);
    }
    free(es->v);
    es->v = NULL;
    es->n = 0;
}

static pc_status effects_read(yyjson_val *v, void *slot, pc_error *err)
{
    pc_algo_gallery_effects es = {NULL, 0};
    yyjson_arr_iter it;
    yyjson_val *el;
    size_t n;
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, "a sequence", err);
    }
    n = yyjson_arr_size(v);
    if (n > 0) {
        es.v = pc_calloc_array(n, sizeof *es.v);
        if (es.v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory reading %zu gallery effects", n);
        }
    }
    yyjson_arr_iter_init(v, &it);
    while ((el = yyjson_arr_iter_next(&it)) != NULL && es.n < n) {
        pc_status st = pc_algo_gallery_effect_from_json(el, &es.v[es.n], err);
        if (st != PC_OK) {
            effects_fini(&es);
            return st;
        }
        es.n++;
    }
    effects_fini(slot);
    *(pc_algo_gallery_effects *)slot = es;
    return PC_OK;
}

static yyjson_mut_val *effects_write(yyjson_mut_doc *d, const void *slot)
{
    const pc_algo_gallery_effects *es = slot;
    yyjson_mut_val *a = yyjson_mut_arr(d);
    size_t i;
    if (a == NULL) {
        return NULL;
    }
    for (i = 0; i < es->n; i++) {
        yyjson_mut_val *o = pc_algo_gallery_effect_to_json(d, &es->v[i]);
        if (o == NULL || !yyjson_mut_arr_append(a, o)) {
            return NULL;
        }
    }
    return a;
}

static pc_status effects_copy(void *dst, const void *src, pc_error *err)
{
    const pc_algo_gallery_effects *s = src;
    pc_algo_gallery_effects c = {NULL, 0};
    size_t i;
    if (s->n > 0) {
        c.v = pc_calloc_array(s->n, sizeof *c.v);
        if (c.v == NULL) {
            memset(dst, 0, sizeof c);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying gallery effects");
        }
    }
    for (i = 0; i < s->n; i++) {
        pc_status st = pc_algo_gallery_effect_clone(&s->v[i], &c.v[i], err);
        if (st != PC_OK) {
            effects_fini(&c);
            memset(dst, 0, sizeof c);
            return st;
        }
        c.n = i + 1;
    }
    *(pc_algo_gallery_effects *)dst = c;
    return PC_OK;
}

int32_t pc_algo_filter_gallery_reach(const pc_algo_gallery_effects *effects)
{
    int32_t r = 0;
    size_t i;
    for (i = 0; i < effects->n; i++) {
        r = pc_i32_wrapping_add(r, pc_algo_gallery_effect_reach(&effects->v[i]));
    }
    return r;
}

static pc_algo_halo gallery_halo(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_radius(
        pc_algo_filter_gallery_reach(&((const pc_algo_filter_gallery_params *)p)->effects));
}

static const char *gallery_label(const void *p)
{
    const pc_algo_filter_gallery_params *g = p;
    return g->effects.n == 1 ? pc_algo_gallery_filter_name(g->effects.v[0].filter) : "Filter Gallery";
}

static pc_status gallery_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                           const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    (void)prep;
    return pc_algo_gallery_run(&((const pc_algo_filter_gallery_params *)p)->effects, src, out, ctx, dst, err);
}

static const pc_algo_jfield GALLERY_F[] = {
    {"effects", PC_ALGO_JF_CUSTOM, offsetof(pc_algo_filter_gallery_params, effects), false, NULL, 0,
     effects_read, effects_write, effects_fini, effects_copy},
};

const pc_algo_filter_def pc_algo_def_filter_gallery = {"filterGallery",
                                                       sizeof(pc_algo_filter_gallery_params),
                                                       GALLERY_F,
                                                       1,
                                                       NULL,
                                                       NULL,
                                                       gallery_halo,
                                                       gallery_label,
                                                       NULL,
                                                       NULL,
                                                       gallery_k,
                                                       NULL,
                                                       0};
