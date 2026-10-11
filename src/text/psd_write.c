/* PSD type layers, writing side: pc_text_layer -> EngineData and TySh (port of the writing half
 * of crates/text/src/psd.rs). See psd.h.
 *
 * Kerning in EngineData, as observed in Photoshop 2026 (writing, and reading files without
 * Txt2): a manual kern k (1/1000 em) after character c is written on character c + 1 as
 * `AutoKerning false` + `Kerning k`; everything else is `true` + 0. Photoshop's reader takes
 * `Kerning k` on character j as the kern after j - 1 (making j - 1 manual), and
 * `AutoKerning false` on j as "character j - 2 is manually kerned". The automatic mode itself
 * (Metrics or Optical) isn't in EngineData: Photoshop keeps it in the document's Txt2
 * (pc_txt2_apply). We write and read the same way, so Photoshop reads our kerning as we do and
 * its own fields round-trip unchanged. (The last character's mode has no slot and reads back as
 * Metrics.) */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "text/fonts.h"
#include "text/psd_internal.h"

const char *const PC_TEXT_OPENTYPE_KEYS[PC_TEXT_OPENTYPE_KEY_COUNT][2] = {{"ContextualLigatures", "calt"},
                                                                          {"Swash", "swsh"},
                                                                          {"OldStyle", "onum"},
                                                                          {"StylisticAlternates", "salt"},
                                                                          {"Titling", "titl"},
                                                                          {"Ornaments", "ornm"},
                                                                          {"Ordinals", "ordn"},
                                                                          {"Fractions", "frac"}};

/* ---- EngineData building helpers ---- */

static pc_status set_val(pc_ed_value *d, const char *key, pc_ed_value v, pc_error *err)
{
    pc_status st = pc_ed_set(d, key, &v, err);
    if (st != PC_OK) {
        pc_ed_fini(&v);
    }
    return st;
}

pc_status psd_set_int(pc_ed_value *d, const char *key, int64_t i, pc_error *err)
{
    return set_val(d, key, pc_ed_int(i), err);
}

pc_status psd_set_bool(pc_ed_value *d, const char *key, bool b, pc_error *err)
{
    return set_val(d, key, pc_ed_bool(b), err);
}

pc_status psd_set_real(pc_ed_value *d, const char *key, double r, pc_error *err)
{
    return set_val(d, key, pc_ed_real(r), err);
}

pc_status psd_set_str(pc_ed_value *d, const char *key, const char *s, size_t len, pc_error *err)
{
    pc_ed_value v;
    pc_status st = pc_ed_string(s, len, &v, err);
    if (st != PC_OK) {
        return st;
    }
    return set_val(d, key, v, err);
}

/* Sets key to *v (taken over, also on failure). */
static pc_status set_take(pc_ed_value *d, const char *key, pc_ed_value *v, pc_error *err)
{
    pc_status st = pc_ed_set(d, key, v, err);
    pc_ed_fini(v);
    return st;
}

static pc_status push_take(pc_ed_value *a, pc_ed_value *v, pc_error *err)
{
    pc_status st = pc_ed_push(a, v, err);
    pc_ed_fini(v);
    return st;
}

static pc_status push_real(pc_ed_value *a, double r, pc_error *err)
{
    pc_ed_value v = pc_ed_real(r);
    return pc_ed_push(a, &v, err);
}

static double real(float v)
{
    return (double)v;
}

/* Manual kerning as Photoshop stores it: a whole number of 1/1000 em (non-finite writes 0). */
static int64_t kern_units(float v)
{
    return isfinite(v) ? pc_f32_to_i64(pc_clampf(roundf(v), -10000.0f, 10000.0f)) : 0;
}

static bool manual(const pc_char_style *s)
{
    return s->kerning == PC_KERNING_OFF || kern_units(s->kern) != 0;
}

pc_status pc_text_postscript_for(const pc_char_style *s, pc_str *out, pc_error *err)
{
    const char *fam;
    size_t flen, k;
    const char *w;
    pc_buf b = PC_BUF_INIT;
    pc_status st = PC_OK;
    uint8_t *data;
    size_t n;
    if (s->has_postscript_name) {
        return pc_str_clone(&s->postscript_name, out, err);
    }
    fam = s->font_family.len == 0 ? PC_TEXT_DEFAULT_FAMILY : pc_str_c(&s->font_family);
    flen = s->font_family.len == 0 ? strlen(PC_TEXT_DEFAULT_FAMILY) : s->font_family.len;
    w = s->weight <= 150   ? "Thin"
        : s->weight <= 250 ? "ExtraLight"
        : s->weight <= 350 ? "Light"
        : s->weight <= 450 ? "Regular"
        : s->weight <= 550 ? "Medium"
        : s->weight <= 650 ? "SemiBold"
        : s->weight <= 750 ? "Bold"
        : s->weight <= 850 ? "ExtraBold"
                           : "Black";
    for (k = 0; k < flen && st == PC_OK; k++) {
        if (fam[k] != ' ') {
            st = pc_buf_append_byte(&b, (uint8_t)fam[k], err);
        }
    }
    if (st == PC_OK) {
        st = pc_buf_append_byte(&b, '-', err);
    }
    if (st == PC_OK) {
        if (s->italic) {
            st = pc_buf_append_str(&b, strcmp(w, "Regular") == 0 ? "Italic" : w, err);
            if (st == PC_OK && strcmp(w, "Regular") != 0) {
                st = pc_buf_append_str(&b, "Italic", err);
            }
        } else {
            st = pc_buf_append_str(&b, w, err);
        }
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    data = pc_buf_take(&b, &n);
    *out = pc_str_adopt((char *)data, n);
    return PC_OK;
}

pc_status psd_style_sheet_data(const pc_char_style *s, size_t font, float k, pc_ed_value *out, pc_error *err)
{
    const pc_color *c = &s->color;
    pc_ed_value d = pc_ed_dict(), values = pc_ed_array(), fill = pc_ed_dict();
    int64_t color_type = c->mode == PC_COLOR_MODE_CMYK ? 2 : c->mode == PC_COLOR_MODE_GRAYSCALE ? 0 : 1;
    int64_t caps = s->caps == PC_CAPS_SMALL_CAPS ? 1 : s->caps == PC_CAPS_ALL_CAPS ? 2 : 0;
    pc_status st = push_real(&values, real(c->alpha), err);
    size_t f, g;
    if (c->mode == PC_COLOR_MODE_CMYK) {
        for (f = 0; f < 4 && st == PC_OK; f++) {
            st = push_real(&values, real(c->c[f]), err);
        }
    } else if (c->mode == PC_COLOR_MODE_GRAYSCALE) {
        if (st == PC_OK) {
            st = push_real(&values, real(c->c[0]), err);
        }
    } else {
        float rgb[3];
        pc_color_to_rgb(c, rgb);
        for (f = 0; f < 3 && st == PC_OK; f++) {
            st = push_real(&values, real(rgb[f]), err);
        }
    }
    if (st == PC_OK) {
        st = psd_set_int(&d, "Font", (int64_t)font, err);
    }
    if (st == PC_OK) {
        st = psd_set_real(&d, "FontSize", real(s->size_pt / k), err);
    }
    if (st == PC_OK) {
        st = psd_set_bool(&d, "FauxBold", s->faux_bold, err);
    }
    if (st == PC_OK) {
        st = psd_set_bool(&d, "FauxItalic", s->faux_italic, err);
    }
    if (st == PC_OK) {
        st = psd_set_bool(&d, "AutoLeading", !s->has_leading, err);
    }
    if (st == PC_OK) {
        st = psd_set_real(&d, "Leading", real((s->has_leading ? s->leading_pt : 0.0f) / k), err);
    }
    if (st == PC_OK) {
        st = psd_set_real(&d, "HorizontalScale", real(s->horizontal_scale), err);
    }
    if (st == PC_OK) {
        st = psd_set_real(&d, "VerticalScale", real(s->vertical_scale), err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&d, "Tracking", pc_f32_to_i64(roundf(s->tracking)), err);
    }
    if (st == PC_OK) {
        st = psd_set_bool(&d, "AutoKerning", s->kerning != PC_KERNING_OFF, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&d, "Kerning", kern_units(s->kern), err);
    }
    if (st == PC_OK) {
        st = psd_set_real(&d, "BaselineShift", real(s->baseline_shift_pt / k), err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&d, "FontCaps", caps, err);
    }
    if (st == PC_OK) {
        st = psd_set_bool(&d, "Underline", s->underline, err);
    }
    if (st == PC_OK) {
        st = psd_set_bool(&d, "Strikethrough", s->strikethrough, err);
    }
    if (st == PC_OK) {
        st = psd_set_bool(&d, "Ligatures", s->ligatures, err);
    }
    if (st == PC_OK) {
        st = psd_set_bool(&d, "DLigatures", s->discretionary_ligatures, err);
    }
    /* Photopea lays out edited text without painting it unless its fill is enabled. */
    if (st == PC_OK) {
        st = psd_set_bool(&d, "FillFlag", true, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&fill, "Type", color_type, err);
    }
    if (st == PC_OK) {
        st = set_take(&fill, "Values", &values, err);
    }
    if (st == PC_OK) {
        st = set_take(&d, "FillColor", &fill, err);
    }
    for (f = 0; f < PC_TEXT_OPENTYPE_KEY_COUNT && st == PC_OK; f++) {
        bool on = false;
        for (g = 0; g < s->nfeatures; g++) {
            if (pc_str_eq_c(&s->features[g].tag, PC_TEXT_OPENTYPE_KEYS[f][1]) && s->features[g].value > 0) {
                on = true;
            }
        }
        if (on) {
            st = psd_set_bool(&d, PC_TEXT_OPENTYPE_KEYS[f][0], true, err);
        }
    }
    pc_ed_fini(&values);
    pc_ed_fini(&fill);
    if (st != PC_OK) {
        pc_ed_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

pc_status psd_paragraph_properties(const pc_paragraph_style *p, float k, pc_ed_value *out, pc_error *err)
{
    pc_ed_value d = pc_ed_dict();
    int64_t j;
    pc_status st;
    switch (p->align) {
    case PC_TEXT_ALIGN_RIGHT:
        j = 1;
        break;
    case PC_TEXT_ALIGN_CENTER:
        j = 2;
        break;
    case PC_TEXT_ALIGN_JUSTIFY_LEFT:
        j = 3;
        break;
    case PC_TEXT_ALIGN_JUSTIFY_RIGHT:
        j = 4;
        break;
    case PC_TEXT_ALIGN_JUSTIFY_CENTER:
        j = 5;
        break;
    case PC_TEXT_ALIGN_JUSTIFY_ALL:
        j = 6;
        break;
    case PC_TEXT_ALIGN_LEFT:
    default:
        j = 0;
        break;
    }
    st = psd_set_int(&d, "Justification", j, err);
    if (st == PC_OK) {
        st = psd_set_real(&d, "FirstLineIndent", real(p->first_line_indent_pt / k), err);
    }
    if (st == PC_OK) {
        st = psd_set_real(&d, "StartIndent", real(p->start_indent_pt / k), err);
    }
    if (st == PC_OK) {
        st = psd_set_real(&d, "EndIndent", real(p->end_indent_pt / k), err);
    }
    if (st == PC_OK) {
        st = psd_set_real(&d, "SpaceBefore", real(p->space_before_pt / k), err);
    }
    if (st == PC_OK) {
        st = psd_set_real(&d, "SpaceAfter", real(p->space_after_pt / k), err);
    }
    if (st == PC_OK) {
        st = psd_set_bool(&d, "AutoHyphenate", p->hyphenate, err);
    }
    if (st == PC_OK) {
        st = psd_set_real(&d, "AutoLeading", real(p->auto_leading), err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&d, "LeadingType", 0, err);
    }
    if (st != PC_OK) {
        pc_ed_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

pc_status psd_font_entry(const pc_str *name, pc_ed_value *out, pc_error *err)
{
    pc_ed_value d = pc_ed_dict();
    pc_status st = psd_set_str(&d, "Name", pc_str_c(name), name->len, err);
    if (st == PC_OK) {
        st = psd_set_int(&d, "Script", 0, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&d, "FontType", 1, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&d, "Synthetic", 0, err);
    }
    if (st != PC_OK) {
        pc_ed_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

pc_status psd_font_set(const psd_names *fonts, pc_ed_value *out, pc_error *err)
{
    pc_ed_value a = pc_ed_array();
    pc_status st = PC_OK;
    size_t k;
    for (k = 0; k < fonts->n && st == PC_OK; k++) {
        pc_ed_value e;
        st = psd_font_entry(&fonts->v[k], &e, err);
        if (st == PC_OK) {
            st = push_take(&a, &e, err);
        }
    }
    if (st != PC_OK) {
        pc_ed_fini(&a);
        return st;
    }
    *out = a;
    return PC_OK;
}

/* ---- pair runs ---- */

typedef struct pair_run {
    size_t ri;    /* model run */
    bool autok;   /* AutoKerning */
    int64_t kern; /* Kerning */
    size_t n;     /* UTF-16 length */
} pair_run;

/* pair_runs: EngineData style runs for `ed_text` (the layer text with \r breaks and the
 * trailing \r), split where the model run or the (AutoKerning, Kerning) pair changes. */
static pc_status pair_runs(const pc_str *ed_text, const pc_text_run *runs, size_t nruns, pair_run **out,
                           size_t *nout, pc_error *err)
{
    const uint8_t *s = (const uint8_t *)pc_str_c(ed_text);
    size_t *own_ri = NULL, *own_n = NULL, nown = 0, b, ri = 0, run_end, j, n = 0;
    pair_run *r = NULL;
    pc_status st = PC_OK;
    own_ri = pc_malloc_array(ed_text->len + 1, sizeof *own_ri);
    own_n = pc_malloc_array(ed_text->len + 1, sizeof *own_n);
    r = pc_malloc_array(ed_text->len + 1, sizeof *r);
    if (own_ri == NULL || own_n == NULL || r == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory writing PSD text");
        goto done;
    }
    /* Model run of each character (the trailing \r takes the last run). */
    run_end = nruns > 0 ? runs[0].len : 0;
    for (b = 0; b < ed_text->len;) {
        size_t at = b;
        uint32_t cp = psd_next_char(s, ed_text->len, &b);
        while (at >= run_end && ri + 1 < nruns) {
            ri++;
            run_end = run_end > SIZE_MAX - runs[ri].len ? SIZE_MAX : run_end + runs[ri].len;
        }
        own_ri[nown] = ri;
        own_n[nown] = psd_utf16_len(cp);
        nown++;
    }
    for (j = 0; j < nown; j++) {
        const pc_char_style *prev = j >= 1 && own_ri[j - 1] < nruns ? &runs[own_ri[j - 1]].style : NULL;
        int64_t k = prev != NULL ? kern_units(prev->kern) : 0;
        bool off;
        /* `false` marks character j - 2 manual: written when it is and its own kern (on j - 1)
         * doesn't say so already; harmless (and Photoshop's own form) next to a kern on j. */
        if (j >= 2) {
            const pc_char_style *p2 = own_ri[j - 2] < nruns ? &runs[own_ri[j - 2]].style : NULL;
            off = p2 != NULL && manual(p2) && (kern_units(p2->kern) == 0 || k != 0);
        } else {
            off = k != 0;
        }
        if (n > 0 && r[n - 1].ri == own_ri[j] && r[n - 1].autok == !off && r[n - 1].kern == k) {
            r[n - 1].n += own_n[j];
        } else {
            r[n].ri = own_ri[j];
            r[n].autok = !off;
            r[n].kern = k;
            r[n].n = own_n[j];
            n++;
        }
    }
done:
    free(own_ri);
    free(own_n);
    if (st != PC_OK) {
        free(r);
        return st;
    }
    *out = r;
    *nout = n;
    return PC_OK;
}

static int64_t utf16_count(const char *text, size_t len)
{
    const uint8_t *s = (const uint8_t *)text;
    size_t i = 0;
    int64_t n = 0;
    while (i < len) {
        n += (int64_t)psd_utf16_len(psd_next_char(s, len, &i));
    }
    return n;
}

/* ---- build_engine_data ---- */

static pc_status style_runs(const pc_str *ed_text, const pc_text_run *runs, size_t nruns, psd_names *fonts,
                            float k, pc_ed_value *sruns, pc_ed_value *slens, pc_error *err)
{
    pair_run *pr = NULL;
    size_t npr = 0, i;
    pc_status st = pair_runs(ed_text, runs, nruns, &pr, &npr, err);
    for (i = 0; i < npr && st == PC_OK; i++) {
        const pc_text_run *r;
        pc_str ps = PC_STR_INIT;
        size_t fi = 0;
        pc_ed_value data, sheet = pc_ed_dict(), entry = pc_ed_dict(), len;
        if (pr[i].ri >= nruns) {
            continue;
        }
        r = &runs[pr[i].ri];
        st = pc_text_postscript_for(&r->style, &ps, err);
        if (st == PC_OK) {
            st = psd_names_index(fonts, &ps, &fi, err);
        }
        pc_str_fini(&ps);
        if (st == PC_OK) {
            st = psd_style_sheet_data(&r->style, fi, k, &data, err);
        }
        if (st != PC_OK) {
            break;
        }
        st = psd_set_bool(&data, "AutoKerning", pr[i].autok, err);
        if (st == PC_OK) {
            st = psd_set_int(&data, "Kerning", pr[i].kern, err);
        }
        if (st == PC_OK) {
            st = set_take(&sheet, "StyleSheetData", &data, err);
        } else {
            pc_ed_fini(&data);
        }
        if (st == PC_OK) {
            st = set_take(&entry, "StyleSheet", &sheet, err);
        } else {
            pc_ed_fini(&sheet);
        }
        if (st == PC_OK) {
            st = push_take(sruns, &entry, err);
        } else {
            pc_ed_fini(&entry);
        }
        if (st == PC_OK) {
            len = pc_ed_int((int64_t)pr[i].n);
            st = pc_ed_push(slens, &len, err);
        }
    }
    free(pr);
    return st;
}

static pc_status para_runs(const pc_str *text, const pc_paragraph_run *paras, size_t nparas, float k,
                           pc_ed_value *pruns, pc_ed_value *plens, pc_error *err)
{
    size_t i, at = 0;
    pc_status st = PC_OK;
    for (i = 0; i < nparas && st == PC_OK; i++) {
        /* Normalised runs always fit the text; clamp anyway so a bad run can't read past it. */
        size_t start = at < text->len ? at : text->len;
        size_t plen = paras[i].len < text->len - start ? paras[i].len : text->len - start;
        int64_t n = utf16_count(pc_str_c(text) + start, plen);
        pc_ed_value props, sheet = pc_ed_dict(), entry = pc_ed_dict(), len;
        at = start + plen;
        if (i + 1 == nparas) {
            n += 1;
        }
        st = psd_set_int(&sheet, "DefaultStyleSheet", 0, err);
        if (st == PC_OK) {
            st = psd_paragraph_properties(&paras[i].style, k, &props, err);
            if (st == PC_OK) {
                st = set_take(&sheet, "Properties", &props, err);
            }
        }
        if (st == PC_OK) {
            st = set_take(&entry, "ParagraphSheet", &sheet, err);
        } else {
            pc_ed_fini(&sheet);
        }
        if (st == PC_OK) {
            st = push_take(pruns, &entry, err);
        } else {
            pc_ed_fini(&entry);
        }
        if (st == PC_OK) {
            len = pc_ed_int(n);
            st = pc_ed_push(plens, &len, err);
        }
    }
    return st;
}

/* `d[key]` cloned, or an empty dictionary. */
static pc_status get_or_dict(const pc_ed_value *d, const char *key, pc_ed_value *out, pc_error *err)
{
    const pc_ed_value *v = pc_ed_get(d, key);
    if (v == NULL) {
        *out = pc_ed_dict();
        return PC_OK;
    }
    return pc_ed_clone(v, out, err);
}

static pc_status real_pair(double a, double b, pc_ed_value *out, pc_error *err)
{
    pc_ed_value v = pc_ed_array();
    pc_status st = push_real(&v, a, err);
    if (st == PC_OK) {
        st = push_real(&v, b, err);
    }
    if (st != PC_OK) {
        pc_ed_fini(&v);
        return st;
    }
    *out = v;
    return PC_OK;
}

/* EngineDict/Rendered for the layer's shape and orientation. */
static pc_status rendered(const pc_text_layer *layer, pc_ed_value *out, pc_error *err)
{
    int64_t shape_type = layer->shape.is_box ? 1 : 0;
    int64_t writing = layer->orientation == PC_ORIENT_VERTICAL ? 2 : 0;
    pc_ed_value photoshop = pc_ed_dict(), base = pc_ed_dict(), child = pc_ed_dict(), lines = pc_ed_dict();
    pc_ed_value cookie = pc_ed_dict(), shapes = pc_ed_dict(), children = pc_ed_array(), r = pc_ed_dict();
    pc_ed_value tmp = pc_ed_array();
    pc_status st = psd_set_int(&photoshop, "ShapeType", shape_type, err);
    if (st == PC_OK && !layer->shape.is_box) {
        st = real_pair(real(0.0f), real(0.0f), &tmp, err);
        if (st == PC_OK) {
            st = set_take(&photoshop, "PointBase", &tmp, err);
        }
    } else if (st == PC_OK) {
        const pc_text_shape *s = &layer->shape;
        float b[4] = {s->x, s->y, s->x + s->width, s->y + s->height};
        int i;
        for (i = 0; i < 4 && st == PC_OK; i++) {
            st = push_real(&tmp, real(b[i]), err);
        }
        if (st == PC_OK) {
            st = set_take(&photoshop, "BoxBounds", &tmp, err);
        }
    }
    if (st == PC_OK) {
        st = psd_set_int(&base, "ShapeType", shape_type, err);
    }
    if (st == PC_OK) {
        st = real_pair(1.0, 0.0, &tmp, err);
        if (st == PC_OK) {
            st = set_take(&base, "TransformPoint0", &tmp, err);
        }
    }
    if (st == PC_OK) {
        st = real_pair(0.0, 1.0, &tmp, err);
        if (st == PC_OK) {
            st = set_take(&base, "TransformPoint1", &tmp, err);
        }
    }
    if (st == PC_OK) {
        st = real_pair(0.0, 0.0, &tmp, err);
        if (st == PC_OK) {
            st = set_take(&base, "TransformPoint2", &tmp, err);
        }
    }
    if (st == PC_OK) {
        st = set_take(&photoshop, "Base", &base, err);
    }
    /* EngineData writing direction and procession, as Photoshop writes vertical type: 2 and 1
     * (horizontal: 0 and 0). The descriptor's `Ornt` agrees. */
    if (st == PC_OK) {
        st = psd_set_int(&child, "ShapeType", shape_type, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&child, "Procession", writing == 2 ? 1 : 0, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&lines, "WritingDirection", writing, err);
    }
    if (st == PC_OK) {
        tmp = pc_ed_array();
        st = set_take(&lines, "Children", &tmp, err);
    }
    if (st == PC_OK) {
        st = set_take(&child, "Lines", &lines, err);
    }
    if (st == PC_OK) {
        st = set_take(&cookie, "Photoshop", &photoshop, err);
    }
    if (st == PC_OK) {
        st = set_take(&child, "Cookie", &cookie, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&shapes, "WritingDirection", writing, err);
    }
    if (st == PC_OK) {
        st = push_take(&children, &child, err);
    }
    if (st == PC_OK) {
        st = set_take(&shapes, "Children", &children, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&r, "Version", 1, err);
    }
    if (st == PC_OK) {
        st = set_take(&r, "Shapes", &shapes, err);
    }
    pc_ed_fini(&photoshop);
    pc_ed_fini(&base);
    pc_ed_fini(&child);
    pc_ed_fini(&lines);
    pc_ed_fini(&cookie);
    pc_ed_fini(&shapes);
    pc_ed_fini(&children);
    pc_ed_fini(&tmp);
    if (st != PC_OK) {
        pc_ed_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

/* Updates a run dictionary (ParagraphRun / StyleRun) of EngineDict. */
static pc_status set_run(pc_ed_value *dict, const char *key, pc_ed_value *arr, pc_ed_value *lens,
                         int64_t joinable, pc_error *err)
{
    pc_ed_value r;
    pc_status st = get_or_dict(dict, key, &r, err);
    if (st != PC_OK) {
        return st;
    }
    st = set_take(&r, "RunArray", arr, err);
    if (st == PC_OK) {
        st = set_take(&r, "RunLengthArray", lens, err);
    }
    if (st == PC_OK && pc_ed_get(&r, "IsJoinable") == NULL) {
        st = psd_set_int(&r, "IsJoinable", joinable, err);
    }
    if (st == PC_OK) {
        return set_take(dict, key, &r, err);
    }
    pc_ed_fini(&r);
    return st;
}

static pc_status fill_engine_dict(pc_ed_value *dict, const pc_text_layer *layer, const pc_str *ed_text,
                                  pc_ed_value *pruns, pc_ed_value *plens, pc_ed_value *sruns,
                                  pc_ed_value *slens, pc_error *err)
{
    pc_ed_value editor, rend;
    int64_t aa;
    pc_status st = get_or_dict(dict, "Editor", &editor, err);
    if (st != PC_OK) {
        return st;
    }
    st = psd_set_str(&editor, "Text", pc_str_c(ed_text), ed_text->len, err);
    if (st == PC_OK) {
        st = set_take(dict, "Editor", &editor, err);
    } else {
        pc_ed_fini(&editor);
    }
    if (st == PC_OK) {
        st = set_run(dict, "ParagraphRun", pruns, plens, 1, err);
    }
    if (st == PC_OK) {
        st = set_run(dict, "StyleRun", sruns, slens, 2, err);
    }
    switch (layer->antialias) {
    case PC_AA_NONE:
        aa = 0;
        break;
    case PC_AA_SHARP:
        aa = 1;
        break;
    case PC_AA_CRISP:
        aa = 2;
        break;
    case PC_AA_STRONG:
        aa = 3;
        break;
    case PC_AA_SMOOTH:
        aa = 4;
        break;
    /* EngineData has no platform modes; the `AntA` descriptor carries the exact value. */
    case PC_AA_WINDOWS:
    case PC_AA_WINDOWS_LCD:
    default:
        aa = 1;
        break;
    }
    if (st == PC_OK) {
        st = psd_set_int(dict, "AntiAlias", aa, err);
    }
    if (st == PC_OK && pc_ed_get(dict, "UseFractionalGlyphWidths") == NULL) {
        st = psd_set_bool(dict, "UseFractionalGlyphWidths", true, err);
    }
    if (st == PC_OK) {
        st = rendered(layer, &rend, err);
        if (st == PC_OK) {
            st = set_take(dict, "Rendered", &rend, err);
        }
    }
    return st;
}

/* FontSet and the default sheets of one resource dictionary. */
static pc_status fill_resources(pc_ed_value *rd, const pc_ed_value *fontset, const pc_text_run *runs,
                                size_t nruns, float k, pc_error *err)
{
    pc_ed_value v;
    pc_status st = pc_ed_clone(fontset, &v, err);
    if (st == PC_OK) {
        st = set_take(rd, "FontSet", &v, err);
    }
    if (st == PC_OK && pc_ed_get(rd, "StyleSheetSet") == NULL) {
        pc_char_style def;
        pc_ed_value set = pc_ed_array(), sheet = pc_ed_dict(), data;
        pc_char_style_default(&def);
        st = psd_set_str(&sheet, "Name", "Normal RGB", 10, err);
        if (st == PC_OK) {
            st = psd_style_sheet_data(nruns > 0 ? &runs[0].style : &def, 0, k, &data, err);
            if (st == PC_OK) {
                st = set_take(&sheet, "StyleSheetData", &data, err);
            }
        }
        if (st == PC_OK) {
            st = push_take(&set, &sheet, err);
        } else {
            pc_ed_fini(&sheet);
        }
        if (st == PC_OK) {
            st = set_take(rd, "StyleSheetSet", &set, err);
        } else {
            pc_ed_fini(&set);
        }
    }
    if (st == PC_OK && pc_ed_get(rd, "ParagraphSheetSet") == NULL) {
        pc_paragraph_style def = pc_paragraph_style_default();
        pc_ed_value set = pc_ed_array(), sheet = pc_ed_dict(), props;
        st = psd_set_str(&sheet, "Name", "Normal RGB", 10, err);
        if (st == PC_OK) {
            st = psd_set_int(&sheet, "DefaultStyleSheet", 0, err);
        }
        if (st == PC_OK) {
            st = psd_paragraph_properties(&def, k, &props, err);
            if (st == PC_OK) {
                st = set_take(&sheet, "Properties", &props, err);
            }
        }
        if (st == PC_OK) {
            st = push_take(&set, &sheet, err);
        } else {
            pc_ed_fini(&sheet);
        }
        if (st == PC_OK) {
            st = set_take(rd, "ParagraphSheetSet", &set, err);
        } else {
            pc_ed_fini(&set);
        }
    }
    if (st == PC_OK && pc_ed_get(rd, "TheNormalStyleSheet") == NULL) {
        st = psd_set_int(rd, "TheNormalStyleSheet", 0, err);
    }
    if (st == PC_OK && pc_ed_get(rd, "TheNormalParagraphSheet") == NULL) {
        st = psd_set_int(rd, "TheNormalParagraphSheet", 0, err);
    }
    return st;
}

/* A fresh tree: << /EngineDict << >> /ResourceDict << >> >>. */
static pc_status fresh(pc_ed_value *out, pc_error *err)
{
    pc_ed_value e = pc_ed_dict();
    pc_status st = set_val(&e, "EngineDict", pc_ed_dict(), err);
    if (st == PC_OK) {
        st = set_val(&e, "ResourceDict", pc_ed_dict(), err);
    }
    if (st != PC_OK) {
        pc_ed_fini(&e);
        return st;
    }
    *out = e;
    return PC_OK;
}

pc_status pc_text_build_engine_data(const pc_text_layer *layer, pc_ed_value *tmpl, float dpi,
                                    pc_ed_value *out, pc_error *err)
{
    float k = psd_k_of(dpi);
    pc_ed_value e = pc_ed_dict();
    pc_ed_value sruns = pc_ed_array(), slens = pc_ed_array(), pruns = pc_ed_array(), plens = pc_ed_array();
    pc_ed_value fontset = pc_ed_array();
    pc_text_run *runs = NULL;
    pc_paragraph_run *paras = NULL;
    size_t nruns = 0, nparas = 0;
    pc_str text = PC_STR_INIT, ed_text = PC_STR_INIT;
    psd_names fonts = {NULL, 0, 0};
    pc_ed_value *dict, *rd;
    const pc_ed_value *ed;
    pc_status st;
    /* The template comes from the PSD: it may not be a dictionary or may lack `EngineDict`. */
    if (tmpl != NULL && tmpl->kind == PC_ED_DICT) {
        e = *tmpl;
        *tmpl = pc_ed_dict();
        st = PC_OK;
    } else {
        pc_ed_fini(tmpl);
        st = fresh(&e, err);
    }
    ed = pc_ed_get(&e, "EngineDict");
    if (st == PC_OK && (ed == NULL || ed->kind != PC_ED_DICT)) {
        st = set_val(&e, "EngineDict", pc_ed_dict(), err);
    }
    if (st == PC_OK) {
        st = pc_text_layer_char_runs(layer, &runs, &nruns, err);
    }
    if (st == PC_OK) {
        st = pc_text_layer_paragraph_runs(layer, &paras, &nparas, err);
    }
    if (st == PC_OK) {
        st = psd_replace_byte(pc_str_c(&layer->text), layer->text.len, '\n', '\r', &text, err);
    }
    if (st == PC_OK) {
        st = pc_str_clone(&text, &ed_text, err);
    }
    if (st == PC_OK) {
        st = pc_str_append(&ed_text, "\r", 1, err);
    }
    /* Fonts: keep the template's font list and append new PostScript names. */
    if (st == PC_OK) {
        st = psd_names_of(pc_ed_path(&e, "ResourceDict", "FontSet", (const char *)NULL), &fonts, err);
    }
    if (st == PC_OK) {
        st = style_runs(&ed_text, runs, nruns, &fonts, k, &sruns, &slens, err);
    }
    if (st == PC_OK) {
        st = para_runs(&text, paras, nparas, k, &pruns, &plens, err);
    }
    if (st == PC_OK) {
        st = psd_font_set(&fonts, &fontset, err);
    }
    if (st == PC_OK) {
        dict = pc_ed_get_mut(&e, "EngineDict");
        if (dict == NULL) { /* unreachable: set above */
            pc_ed_fini(&e);
            st = fresh(&e, err);
        } else {
            st = fill_engine_dict(dict, layer, &ed_text, &pruns, &plens, &sruns, &slens, err);
            if (st == PC_OK) {
                rd = pc_ed_get_mut(&e, "ResourceDict");
                if (rd != NULL) {
                    st = fill_resources(rd, &fontset, runs, nruns, k, err);
                }
            }
            if (st == PC_OK && pc_ed_get(&e, "DocumentResources") == NULL) {
                pc_ed_value copy;
                st = get_or_dict(&e, "ResourceDict", &copy, err);
                if (st == PC_OK) {
                    st = set_take(&e, "DocumentResources", &copy, err);
                }
            } else if (st == PC_OK) {
                rd = pc_ed_get_mut(&e, "DocumentResources");
                if (rd != NULL) {
                    st = fill_resources(rd, &fontset, runs, nruns, k, err);
                }
            }
        }
    }
    pc_ed_fini(&sruns);
    pc_ed_fini(&slens);
    pc_ed_fini(&pruns);
    pc_ed_fini(&plens);
    pc_ed_fini(&fontset);
    pc_text_runs_free(runs, nruns);
    free(paras);
    pc_str_fini(&text);
    pc_str_fini(&ed_text);
    psd_names_fini(&fonts);
    if (st != PC_OK) {
        pc_ed_fini(&e);
        return st;
    }
    *out = e;
    return PC_OK;
}

/* ---- build_tysh ---- */

/* Id::new over bytes: four bytes are a code, anything else an explicit string. */
static pc_status id_from(const char *s, size_t len, pc_psd_id *out, pc_error *err)
{
    pc_psd_id id;
    memset(&id, 0, sizeof id);
    if (len == 4) {
        id.is_code = true;
        memcpy(id.code, s, 4);
    } else {
        pc_status st = pc_psd_bytes_set(&id.str, s, len, err);
        if (st != PC_OK) {
            return st;
        }
    }
    *out = id;
    return PC_OK;
}

/* Sets the first item `key` of d (or appends one), taking over *v (also on failure). */
static pc_status desc_set(pc_psd_desc *d, const char *key, pc_psd_value *v, pc_error *err)
{
    size_t k;
    for (k = 0; k < d->n; k++) {
        if (pc_psd_id_is(&d->items[k].key, key)) {
            pc_psd_value_fini(&d->items[k].value);
            d->items[k].value = *v;
            memset(v, 0, sizeof *v);
            return PC_OK;
        }
    }
    return pc_psd_desc_add(d, key, v, err);
}

static pc_status desc_set_enum(pc_psd_desc *d, const char *key, const char *type_id, const char *value,
                               pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_enum(type_id, value, &v, err);
    if (st != PC_OK) {
        return st;
    }
    return desc_set(d, key, &v, err);
}

static pc_status rect_value(const char *cls, const float b[4], pc_psd_value *out, pc_error *err)
{
    static const char *const keys[4] = {"Left", "Top ", "Rght", "Btom"};
    pc_psd_desc d;
    pc_status st = pc_psd_desc_init(&d, cls, err);
    int i;
    for (i = 0; i < 4 && st == PC_OK; i++) {
        pc_psd_value v;
        memset(&v, 0, sizeof v);
        v.kind = PC_PSD_V_UNIT_FLOAT;
        memcpy(v.unit, "#Pnt", 4);
        v.d = (double)b[i];
        st = pc_psd_desc_add(&d, keys[i], &v, err);
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    return pc_psd_value_object(&d, out, err);
}

static pc_status new_warp(const pc_text_warp *w, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_value v;
    pc_status st = pc_psd_desc_init(&d, "warp", err);
    if (st == PC_OK) {
        memset(&v, 0, sizeof v);
        v.kind = PC_PSD_V_ENUMERATED;
        st = pc_psd_id_make("warpStyle", &v.type_id, err);
        if (st == PC_OK) {
            st = id_from(pc_str_c(&w->style), w->style.len, &v.value, err);
        }
        if (st == PC_OK) {
            st = pc_psd_desc_add(&d, "warpStyle", &v, err);
        } else {
            pc_psd_value_fini(&v);
        }
    }
    if (st == PC_OK) {
        v = pc_psd_value_double((double)w->value);
        st = pc_psd_desc_add(&d, "warpValue", &v, err);
    }
    if (st == PC_OK) {
        v = pc_psd_value_double((double)w->horizontal_distortion);
        st = pc_psd_desc_add(&d, "warpPerspective", &v, err);
    }
    if (st == PC_OK) {
        v = pc_psd_value_double((double)w->vertical_distortion);
        st = pc_psd_desc_add(&d, "warpPerspectiveOther", &v, err);
    }
    if (st == PC_OK) {
        st = pc_psd_value_enum("Ornt", w->horizontal ? "Hrzn" : "Vrtc", &v, err);
        if (st == PC_OK) {
            st = pc_psd_desc_add(&d, "warpRotate", &v, err);
        }
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

static const char *antialias_id(pc_antialias aa)
{
    switch (aa) {
    case PC_AA_NONE:
        return "Anno";
    case PC_AA_SHARP:
        return "antiAliasSharp";
    case PC_AA_CRISP:
        return "AnCr";
    case PC_AA_STRONG:
        return "AnSt";
    case PC_AA_WINDOWS:
        return "antiAliasPlatformGray";
    case PC_AA_WINDOWS_LCD:
        return "antiAliasPlatformLCD";
    case PC_AA_SMOOTH:
    default:
        return "AnSm";
    }
}

/* Whether the old warp's warpStyle is the enumeration "warpNone". */
static bool warp_is_none(const pc_psd_desc *w)
{
    const pc_psd_value *v = pc_psd_desc_get(w, "warpStyle");
    const uint8_t *b;
    size_t n = 0;
    if (v == NULL || v->kind != PC_PSD_V_ENUMERATED) {
        return false;
    }
    b = pc_psd_id_bytes(&v->value, &n);
    return n == 8 && memcmp(b, "warpNone", 8) == 0;
}

/* The text descriptor items build_tysh patches. */
static pc_status patch_text(pc_psd_desc *text, const pc_text_layer *layer, const float b[4], const pc_buf *ed,
                            pc_error *err)
{
    pc_psd_value v;
    pc_str cr = PC_STR_INIT;
    pc_status st = psd_replace_byte(pc_str_c(&layer->text), layer->text.len, '\n', '\r', &cr, err);
    if (st == PC_OK) {
        memset(&v, 0, sizeof v);
        v.kind = PC_PSD_V_TEXT;
        st = pc_psd_ustr_from_utf8n(pc_str_c(&cr), cr.len, true, &v.text, err);
        if (st == PC_OK) {
            st = desc_set(text, "Txt ", &v, err);
        }
    }
    pc_str_fini(&cr);
    if (st == PC_OK && pc_psd_desc_get(text, "textGridding") == NULL) {
        st = desc_set_enum(text, "textGridding", "textGridding", "None", err);
    }
    if (st == PC_OK) {
        st = desc_set_enum(text, "Ornt", "Ornt", layer->orientation == PC_ORIENT_VERTICAL ? "Vrtc" : "Hrzn",
                           err);
    }
    if (st == PC_OK) {
        st = desc_set_enum(text, "AntA", "Annt", antialias_id(layer->antialias), err);
    }
    if (st == PC_OK) {
        st = rect_value("bounds", b, &v, err);
        if (st == PC_OK) {
            st = desc_set(text, "bounds", &v, err);
        }
    }
    if (st == PC_OK) {
        st = rect_value("boundingBox", b, &v, err);
        if (st == PC_OK) {
            st = desc_set(text, "boundingBox", &v, err);
        }
    }
    if (st == PC_OK && pc_psd_desc_get(text, "TextIndex") == NULL) {
        v = pc_psd_value_int(0);
        st = desc_set(text, "TextIndex", &v, err);
    }
    if (st == PC_OK) {
        memset(&v, 0, sizeof v);
        v.kind = PC_PSD_V_RAW_DATA;
        st = pc_psd_bytes_set(&v.raw, ed->data, ed->len, err);
        if (st == PC_OK) {
            st = desc_set(text, "EngineData", &v, err);
        }
    }
    return st;
}

pc_status pc_text_build_tysh(const pc_text_layer *layer, float dpi, const float *ink, pc_psd_bytes *out,
                             pc_error *err)
{
    pc_tysh old, t;
    bool has_old = false, has_tmpl = false;
    pc_ed_value tmpl = pc_ed_dict(), e = pc_ed_dict();
    pc_buf ed = PC_BUF_INIT;
    pc_error tmp;
    float b[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    pc_status st = PC_OK;
    memset(&old, 0, sizeof old);
    memset(&t, 0, sizeof t);
    if (layer->psd_raw != NULL) {
        st = pc_tysh_parse(pc_blob_data(layer->psd_raw), pc_blob_len(layer->psd_raw), &old, &tmp);
        if (st == PC_OK) {
            has_old = true;
        } else if (st == PC_ERR_NOMEM) {
            if (err != NULL) {
                *err = tmp;
            }
            return st;
        } else {
            st = PC_OK;
        }
    }
    if (has_old) {
        st = pc_tysh_engine_data(&old.text, &tmpl, &tmp);
        if (st == PC_OK) {
            has_tmpl = true;
        } else if (st == PC_ERR_NOMEM) {
            if (err != NULL) {
                *err = tmp;
            }
        } else {
            st = PC_OK;
        }
    }
    if (st == PC_OK) {
        st = pc_text_build_engine_data(layer, has_tmpl ? &tmpl : NULL, dpi, &e, err);
    }
    if (st == PC_OK) {
        st = pc_ed_write(&e, &ed, err);
    }
    if (st == PC_OK) {
        if (has_old) {
            t.text = old.text;
            memset(&old.text, 0, sizeof old.text);
        } else {
            st = pc_psd_desc_init(&t.text, "TxLr", err);
        }
    }
    if (layer->shape.is_box) {
        b[0] = layer->shape.x;
        b[1] = layer->shape.y;
        b[2] = layer->shape.x + layer->shape.width;
        b[3] = layer->shape.y + layer->shape.height;
    } else if (ink != NULL) {
        memcpy(b, ink, sizeof b);
    }
    if (st == PC_OK) {
        st = patch_text(&t.text, layer, b, &ed, err);
    }
    if (st == PC_OK) {
        if (layer->has_warp) {
            st = new_warp(&layer->warp, &t.warp, err);
            t.has_warp = st == PC_OK;
        } else if (has_old && old.has_warp && warp_is_none(&old.warp)) {
            t.warp = old.warp;
            memset(&old.warp, 0, sizeof old.warp);
            t.has_warp = true;
        }
    }
    if (st == PC_OK) {
        t.transform = layer->transform;
        t.bounds[0] = pc_f32_to_i32(floorf(b[0]));
        t.bounds[1] = pc_f32_to_i32(floorf(b[1]));
        t.bounds[2] = pc_f32_to_i32(ceilf(b[2]));
        t.bounds[3] = pc_f32_to_i32(ceilf(b[3]));
        st = pc_tysh_write(&t, out, err);
    }
    pc_tysh_fini(&t);
    pc_tysh_fini(&old);
    pc_ed_fini(&tmpl);
    pc_ed_fini(&e);
    pc_buf_fini(&ed);
    return st;
}
