#include "builtin.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "math.h"
#include "synth.h"

static const char *const IDS[PC_BUILTIN_COUNT] = {
    "srgb", "display-p3", "adobe-rgb-compat", "prophoto-compat", "linear-srgb",
    "rec2020", "gray-gamma-2.2", "sgray", "lab-d50", "coated-cmyk",
};

static const char *const DESCRIPTIONS[PC_BUILTIN_COUNT] = {
    "sRGB IEC61966-2.1",
    "Display P3",
    "Adobe RGB (1998)-compatible (Photocraft)",
    "ProPhoto RGB-compatible (ROMM primaries, Photocraft)",
    "Linear sRGB (Photocraft)",
    "Rec. ITU-R BT.2020 (Photocraft)",
    "Gray Gamma 2.2 (Photocraft)",
    "sGray (sRGB tone curve, Photocraft)",
    "Lab D50 identity (Photocraft)",
    PC_SYNTH_DESCRIPTION,
};

const char *pc_builtin_id(pc_builtin b)
{
    return (unsigned)b < PC_BUILTIN_COUNT ? IDS[b] : IDS[0];
}

const char *pc_builtin_description(pc_builtin b)
{
    return (unsigned)b < PC_BUILTIN_COUNT ? DESCRIPTIONS[b] : DESCRIPTIONS[0];
}

static bool ascii_ieq_n(const char *a, size_t alen, const char *b)
{
    size_t i;
    if (strlen(b) != alen) {
        return false;
    }
    for (i = 0; i < alen; i++) {
        unsigned char x = (unsigned char)a[i];
        unsigned char y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') { x = (unsigned char)(x + 32); }
        if (y >= 'A' && y <= 'Z') { y = (unsigned char)(y + 32); }
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool pc_builtin_from_id(const char *s, size_t len, pc_builtin *out)
{
    static const struct {
        const char *alias;
        pc_builtin b;
    } aliases[] = {
        {"srgb", PC_BUILTIN_SRGB}, {"srgbiec6196621", PC_BUILTIN_SRGB}, {"srgbiec61966", PC_BUILTIN_SRGB},
        {"p3", PC_BUILTIN_DISPLAY_P3}, {"displayp3", PC_BUILTIN_DISPLAY_P3},
        {"adobergb", PC_BUILTIN_ADOBE_RGB_COMPAT}, {"adobergb1998", PC_BUILTIN_ADOBE_RGB_COMPAT},
        {"adobergbcompat", PC_BUILTIN_ADOBE_RGB_COMPAT}, {"clayrgb", PC_BUILTIN_ADOBE_RGB_COMPAT},
        {"prophoto", PC_BUILTIN_PROPHOTO_COMPAT}, {"prophotorgb", PC_BUILTIN_PROPHOTO_COMPAT},
        {"prophotocompat", PC_BUILTIN_PROPHOTO_COMPAT}, {"romm", PC_BUILTIN_PROPHOTO_COMPAT},
        {"rommrgb", PC_BUILTIN_PROPHOTO_COMPAT},
        {"linearsrgb", PC_BUILTIN_LINEAR_SRGB}, {"srgblinear", PC_BUILTIN_LINEAR_SRGB},
        {"linear", PC_BUILTIN_LINEAR_SRGB},
        {"rec2020", PC_BUILTIN_REC2020}, {"bt2020", PC_BUILTIN_REC2020},
        {"gray", PC_BUILTIN_GRAY_GAMMA22}, {"graygamma22", PC_BUILTIN_GRAY_GAMMA22},
        {"gamma22", PC_BUILTIN_GRAY_GAMMA22},
        {"sgray", PC_BUILTIN_SGRAY}, {"graysrgb", PC_BUILTIN_SGRAY},
        {"lab", PC_BUILTIN_LAB_D50}, {"labd50", PC_BUILTIN_LAB_D50}, {"cielab", PC_BUILTIN_LAB_D50},
        {"cmyk", PC_BUILTIN_COATED_CMYK}, {"coatedcmyk", PC_BUILTIN_COATED_CMYK},
        {"defaultcmyk", PC_BUILTIN_COATED_CMYK}, {"photocraftcoatedcmyk", PC_BUILTIN_COATED_CMYK},
    };
    char key[64];
    size_t n = 0;
    size_t i;
    bool fits = true;
    if (s == NULL) {
        return false;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + 32);
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (n + 1 >= sizeof key) {
                fits = false;
                break;
            }
            key[n++] = (char)c;
        }
    }
    key[n] = '\0';
    for (i = 0; fits && i < sizeof aliases / sizeof aliases[0]; i++) {
        if (strcmp(key, aliases[i].alias) == 0) {
            *out = aliases[i].b;
            return true;
        }
    }
    for (i = 0; i < PC_BUILTIN_COUNT; i++) {
        if (ascii_ieq_n(s, len, DESCRIPTIONS[i])) {
            *out = (pc_builtin)i;
            return true;
        }
    }
    return false;
}

/* Colorant matrix adapted to D50 and quantized to s15Fixed16, each row nudged so RGB (1, 1, 1)
 * maps exactly to the stored D50 white (keeps neutrals neutral). */
static bool colorants(const double prim[3][2], const double white[3], pc_mat3 *q, bool *has_chad, pc_mat3 *chad)
{
    pc_mat3 m;
    pc_mat3 m50;
    pc_mat3 adapt;
    double d50[3];
    bool do_adapt;
    int i;
    int j;
    if (!pc_rgb_to_xyz_matrix(prim[0], prim[1], prim[2], white, &m)) {
        return false;
    }
    pc_d50_quantized(d50);
    do_adapt = !(fabs(white[0] - PC_D50[0]) < 1e-3 && fabs(white[2] - PC_D50[2]) < 1e-3);
    if (do_adapt) {
        adapt = pc_bradford(white, PC_D50);
        m50 = pc_mat3_mul(&adapt, &m);
    } else {
        /* White already (nearly) D50: scale rows onto the exact PCS white. */
        static const double ones[3] = {1.0, 1.0, 1.0};
        double s[3];
        pc_mat3_apply(&m, ones, s);
        m50 = m;
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                m50.m[i][j] *= PC_D50[i] / s[i];
            }
        }
    }
    for (i = 0; i < 3; i++) {
        double sum = -0.0; /* Rust's f64 Sum */
        double err;
        int k = 0;
        for (j = 0; j < 3; j++) {
            q->m[i][j] = pc_s15f16_round(m50.m[i][j]);
            sum += q->m[i][j];
        }
        err = d50[i] - sum;
        /* max_by on |v| returns the last of equal maxima. */
        for (j = 1; j < 3; j++) {
            if (fabs(q->m[i][j]) >= fabs(q->m[i][k])) {
                k = j;
            }
        }
        q->m[i][k] = pc_s15f16_round(q->m[i][k] + err);
    }
    *has_chad = do_adapt;
    if (do_adapt) {
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                chad->m[i][j] = pc_s15f16_round(adapt.m[i][j]);
            }
        }
    }
    return true;
}

static pc_status set_text(pc_profile *p, const char *desc, pc_error *err)
{
    char *d = pc_strndup(desc, strlen(desc));
    char *c = pc_strndup(PC_BUILTIN_COPYRIGHT, strlen(PC_BUILTIN_COPYRIGHT));
    if (d == NULL || c == NULL) {
        free(d);
        free(c);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    free(p->description);
    free(p->copyright);
    p->description = d;
    p->description_len = strlen(d);
    p->copyright = c;
    p->copyright_len = strlen(c);
    return PC_OK;
}

static pc_status base_profile(pc_profile *p, const char *desc, uint32_t cls, uint32_t space, pc_pcs pcs,
                              pc_error *err)
{
    pc_status s = pc_profile_init_empty(p, err);
    if (s != PC_OK) {
        return s;
    }
    p->version_major = 4;
    p->version_minor = 0x30;
    p->class_sig = cls;
    p->space = space;
    p->pcs = pcs;
    p->rendering_intent = PC_INTENT_PERCEPTUAL;
    pc_d50_quantized(p->white_point);
    s = set_text(p, desc, err);
    if (s != PC_OK) {
        pc_profile_fini(p);
    }
    return s;
}

static pc_status rgb(pc_profile *p, pc_builtin b, const double prim[3][2], const double white[3], pc_cms_curve trc,
                     pc_error *err)
{
    pc_status s = base_profile(p, DESCRIPTIONS[b], PC_CLASS_DISPLAY, PC_CS_RGB, PC_PCS_XYZ, err);
    int k;
    if (s != PC_OK) {
        return s;
    }
    if (!colorants(prim, white, &p->matrix, &p->has_chad, &p->chad)) {
        pc_profile_fini(p);
        return pc_fail(err, PC_ERR_INVALID, "built-in primaries are not independent");
    }
    p->has_matrix = true;
    p->has_trc = true;
    for (k = 0; k < 3; k++) {
        p->trc[k] = trc; /* parametric/gamma curves own no memory */
    }
    s = pc_profile_encode_bytes(p, err);
    if (s != PC_OK) {
        pc_profile_fini(p);
    }
    return s;
}

static pc_status gray(pc_profile *p, const char *description, pc_cms_curve trc, pc_error *err)
{
    pc_status s = base_profile(p, description, PC_CLASS_DISPLAY, PC_CS_GRAY, PC_PCS_XYZ, err);
    if (s != PC_OK) {
        return s;
    }
    p->has_gray_trc = true;
    p->gray_trc = trc;
    s = pc_profile_encode_bytes(p, err);
    if (s != PC_OK) {
        pc_profile_fini(p);
    }
    return s;
}

static pc_status ident_lut(pc_lut **out, pc_error *err)
{
    pc_lut *l = calloc(1, sizeof *l);
    pc_cms_curve *c = pc_calloc_array(3, sizeof *c);
    pc_cms_stage st;
    pc_status s;
    if (l == NULL || c == NULL) {
        free(l);
        free(c);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    c[0] = c[1] = c[2] = pc_cms_curve_identity();
    l->kind = PC_LUT_AB;
    l->inputs = 3;
    l->outputs = 3;
    st = pc_cms_stage_curves(PC_STAGE_CURVES, c, 3);
    s = pc_cms_stages_push(&l->stages, &st, err);
    if (s != PC_OK) {
        free(l);
        return s;
    }
    *out = l;
    return PC_OK;
}

static pc_status lab(pc_profile *p, pc_error *err)
{
    pc_status s = base_profile(p, DESCRIPTIONS[PC_BUILTIN_LAB_D50], PC_CLASS_COLOR_SPACE, PC_CS_LAB, PC_PCS_LAB, err);
    if (s != PC_OK) {
        return s;
    }
    s = ident_lut(&p->a2b[0], err);
    if (s == PC_OK) {
        s = ident_lut(&p->b2a[0], err);
    }
    if (s == PC_OK) {
        s = pc_profile_encode_bytes(p, err);
    }
    if (s != PC_OK) {
        pc_profile_fini(p);
    }
    return s;
}

static pc_status build(pc_builtin b, pc_profile *p, pc_error *err)
{
    static const double srgb_p[3][2] = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}};
    static const double p3[3][2] = {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}};
    static const double adobe[3][2] = {{0.64, 0.33}, {0.21, 0.71}, {0.15, 0.06}};
    static const double romm[3][2] = {{0.7347, 0.2653}, {0.1596, 0.8404}, {0.0366, 0.0001}};
    static const double bt2020[3][2] = {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};
    double d65[3];
    pc_xy_to_xyz(PC_D65_XY, d65);
    switch (b) {
    case PC_BUILTIN_SRGB: return rgb(p, b, srgb_p, d65, pc_cms_srgb_trc(), err);
    case PC_BUILTIN_DISPLAY_P3: return rgb(p, b, p3, d65, pc_cms_srgb_trc(), err);
    case PC_BUILTIN_ADOBE_RGB_COMPAT: return rgb(p, b, adobe, d65, pc_cms_curve_gamma(563.0 / 256.0), err);
    case PC_BUILTIN_PROPHOTO_COMPAT: return rgb(p, b, romm, PC_D50, pc_cms_curve_gamma(1.8), err);
    case PC_BUILTIN_LINEAR_SRGB: return rgb(p, b, srgb_p, d65, pc_cms_curve_gamma(1.0), err);
    case PC_BUILTIN_REC2020: {
        /* Rec. 709 OETF inverse: V = 1.099 L^0.45 - 0.099 (L >= 0.018), 4.5 L below. */
        const double a = 1.099;
        const double prm[7] = {1.0 / 0.45, 1.0 / a, (a - 1.0) / a, 1.0 / 4.5, 0.081, 0.0, 0.0};
        return rgb(p, b, bt2020, d65, pc_cms_curve_parametric(3, prm), err);
    }
    case PC_BUILTIN_GRAY_GAMMA22:
        return gray(p, DESCRIPTIONS[b], pc_cms_curve_gamma(2.2), err);
    case PC_BUILTIN_SGRAY:
        return gray(p, DESCRIPTIONS[b], pc_cms_srgb_trc(), err);
    case PC_BUILTIN_LAB_D50: return lab(p, err);
    case PC_BUILTIN_COATED_CMYK:
        /* The shipped bytes are synth's output (checked by tests); rebuild them rather than
         * fail if they ever don't parse. */
        if (pc_profile_parse(pc_coated_cmyk_icc, pc_coated_cmyk_icc_len, p, NULL) == PC_OK) {
            return PC_OK;
        }
        return pc_synth_coated_cmyk(p, err);
    case PC_BUILTIN_COUNT: break;
    }
    return pc_fail(err, PC_ERR_INVALID, "unknown built-in profile");
}

static pc_profile profiles[PC_BUILTIN_COUNT];
static bool built[PC_BUILTIN_COUNT];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void build_all(void)
{
    int i;
    for (i = 0; i < PC_BUILTIN_COUNT; i++) {
        built[i] = build((pc_builtin)i, &profiles[i], NULL) == PC_OK;
    }
}

const pc_profile *pc_builtin_profile(pc_builtin b)
{
    if ((unsigned)b >= PC_BUILTIN_COUNT || pthread_once(&once, build_all) != 0) {
        return NULL;
    }
    return built[b] ? &profiles[b] : NULL;
}

const pc_profile *pc_builtin_default_for(uint32_t space)
{
    switch (space) {
    case PC_CS_RGB: return pc_builtin_profile(PC_BUILTIN_SRGB);
    case PC_CS_GRAY: return pc_builtin_profile(PC_BUILTIN_SGRAY);
    case PC_CS_CMYK: return pc_builtin_profile(PC_BUILTIN_COATED_CMYK);
    case PC_CS_LAB: return pc_builtin_profile(PC_BUILTIN_LAB_D50);
    default: return NULL;
    }
}

static pc_profile linear_gray_profile;
static bool linear_gray_built;
static pthread_once_t linear_gray_once = PTHREAD_ONCE_INIT;

static void build_linear_gray(void)
{
    pc_status s = gray(&linear_gray_profile, "Linear Gray (Photocraft)", pc_cms_curve_gamma(1.0), NULL);
    linear_gray_built = s == PC_OK;
}

const pc_profile *pc_builtin_linear_gray(void)
{
    if (pthread_once(&linear_gray_once, build_linear_gray) != 0) {
        return NULL;
    }
    return linear_gray_built ? &linear_gray_profile : NULL;
}
