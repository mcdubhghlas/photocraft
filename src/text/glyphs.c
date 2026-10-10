/* Glyphs panel support (port of crates/text/src/glyphs.rs; see glyphs.h). */
#include "text/glyphs.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "text/render.h"
#include "text/unicode.h"

const char *const PC_GLYPH_CATEGORIES[14] = {
    "Entire Font", "Basic Latin", "Latin 1",  "Latin Extended-A", "Latin Extended-B", "Greek",  "Cyrillic",
    "Punctuation", "Numbers",     "Currency", "Symbols",          "Math Symbols",     "Arrows", "Ornaments",
};

static bool in(uint32_t u, uint32_t a, uint32_t b)
{
    return u >= a && u <= b;
}

static bool ascii_punct(uint32_t c)
{
    return in(c, 0x21, 0x2F) || in(c, 0x3A, 0x40) || in(c, 0x5B, 0x60) || in(c, 0x7B, 0x7E);
}

bool pc_glyph_in_category(uint32_t u, const char *category)
{
    const char *k = category != NULL ? category : "";
    if (strcmp(k, "Basic Latin") == 0) {
        return in(u, 0x20, 0x7E);
    }
    if (strcmp(k, "Latin 1") == 0) {
        return in(u, 0xA0, 0xFF);
    }
    if (strcmp(k, "Latin Extended-A") == 0) {
        return in(u, 0x100, 0x17F);
    }
    if (strcmp(k, "Latin Extended-B") == 0) {
        return in(u, 0x180, 0x24F);
    }
    if (strcmp(k, "Greek") == 0) {
        return in(u, 0x370, 0x3FF) || in(u, 0x1F00, 0x1FFF);
    }
    if (strcmp(k, "Cyrillic") == 0) {
        return in(u, 0x400, 0x52F);
    }
    if (strcmp(k, "Punctuation") == 0) {
        return (ascii_punct(u) && !(u == '$' || u == '+' || u == '<' || u == '=' || u == '>' || u == '^' ||
                                    u == '`' || u == '|' || u == '~')) ||
               u == 0xA1 || u == 0xA7 || u == 0xAB || u == 0xB6 || u == 0xB7 || u == 0xBB || u == 0xBF ||
               in(u, 0x2010, 0x2027) || in(u, 0x2030, 0x205E) || in(u, 0x2E00, 0x2E7F) ||
               in(u, 0x3000, 0x303F);
    }
    if (strcmp(k, "Numbers") == 0) {
        return pc_text_is_numeric(u) || in(u, 0x2150, 0x218F);
    }
    if (strcmp(k, "Currency") == 0) {
        return u == '$' || u == 0xA2 || u == 0xA3 || u == 0xA4 || u == 0xA5 || in(u, 0x20A0, 0x20CF);
    }
    if (strcmp(k, "Symbols") == 0) {
        return u == 0xA9 || u == 0xAE || u == 0xB0 || u == 0xA6 || u == 0xA8 || u == 0xAF || u == 0xB4 ||
               u == 0xB8 || u == 0x2122 || u == 0x2120 || u == 0x2116 || u == 0x212E || u == '^' ||
               u == '`' || u == '|' || u == '~' || in(u, 0x2100, 0x214F) || in(u, 0x2300, 0x23FF) ||
               in(u, 0x2500, 0x25FF);
    }
    if (strcmp(k, "Math Symbols") == 0) {
        return u == '+' || u == '<' || u == '=' || u == '>' || u == 0xB1 || u == 0xD7 || u == 0xF7 ||
               u == 0xAC || u == 0xB5 || u == 0xB9 || u == 0xB2 || u == 0xB3 || u == 0xBC || u == 0xBD ||
               u == 0xBE || in(u, 0x2070, 0x209F) || in(u, 0x2200, 0x22FF) || in(u, 0x27C0, 0x27EF) ||
               in(u, 0x2980, 0x2AFF);
    }
    if (strcmp(k, "Arrows") == 0) {
        return in(u, 0x2190, 0x21FF) || in(u, 0x27F0, 0x27FF) || in(u, 0x2900, 0x297F) ||
               in(u, 0x2B00, 0x2BFF);
    }
    if (strcmp(k, "Ornaments") == 0) {
        return in(u, 0x2600, 0x27BF) || in(u, 0x1F300, 0x1FAFF) || in(u, 0xE000, 0xF8FF);
    }
    return true;
}

uint32_t pc_glyph_preview_size(uint32_t px)
{
    return px < 4 ? 4 : px > 512 ? 512 : px;
}

pc_status pc_glyph_preview_from_layout(const pc_text_layout *layout, uint32_t px, uint32_t *out_px,
                                       uint8_t **alpha, pc_error *err)
{
    float b[4], w = 0.0f, x0 = 0.0f, tx;
    pc_affine t;
    pc_rendered r;
    uint8_t (*rgba)[4] = NULL;
    uint8_t *a;
    size_t n, i;
    pc_status st;
    pc_pixel_format fmt = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true};
    if (layout == NULL || out_px == NULL || alpha == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "glyph preview: invalid arguments");
    }
    px = pc_glyph_preview_size(px);
    if (pc_text_layout_bounds(layout, b)) {
        w = b[2] - b[0];
        x0 = b[0];
    }
    tx = ((float)px - w) / 2.0f - x0;
    t = (pc_affine){{1.0, 0.0, 0.0, 1.0, (double)tx, (double)((float)px * 0.78f)}};
    st = pc_text_rasterize(layout, &t, fmt, PC_AA_SMOOTH, NULL, &r, err);
    if (st != PC_OK) {
        return st;
    }
    n = (size_t)px * px;
    rgba = pc_malloc_array(n, sizeof *rgba);
    a = pc_malloc_array(n, 1);
    if (rgba == NULL || a == NULL) {
        free(rgba);
        free(a);
        pc_rendered_fini(&r);
        return pc_fail(err, PC_ERR_NOMEM, "glyph preview: out of memory");
    }
    st = pc_surface_read_rgba8_into(&r.surface, pc_rect_make(0, 0, (int32_t)px, (int32_t)px), rgba, n, err);
    pc_rendered_fini(&r);
    if (st != PC_OK) {
        free(rgba);
        free(a);
        return st;
    }
    for (i = 0; i < n; i++) {
        a[i] = rgba[i][3];
    }
    free(rgba);
    *out_px = px;
    *alpha = a;
    return PC_OK;
}
