#include "font.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "base/file.h"
#include "base/mem.h"
#include "base/num.h"

enum { ATLAS_W = 1024, ATLAS_MAX_H = 2048, PAD = 1, MAX_FONT_BYTES = 64 << 20 };

static const uint32_t RANGES[][2] = {
    {0x20, 0x7E}, {0xA0, 0x17F}, {0x2010, 0x2027}, {0x2030, 0x203A}, {0x20AC, 0x20AC},
    {0x2122, 0x2122}, {0x2190, 0x2193}, {0x2212, 0x2212}, {0xFFFD, 0xFFFD},
};

static int glyph_cmp(const void *key, const void *elem)
{
    uint32_t cp = *(const uint32_t *)key;
    uint32_t g = ((const ui_glyph *)elem)->cp;
    return cp < g ? -1 : (cp > g ? 1 : 0);
}

static const ui_glyph *find_glyph(const ui_font *f, uint32_t cp)
{
    const ui_glyph *g = bsearch(&cp, f->glyphs, f->nglyphs, sizeof *f->glyphs, glyph_cmp);
    if (g == NULL) {
        uint32_t fallback = 0xFFFD;
        g = bsearch(&fallback, f->glyphs, f->nglyphs, sizeof *f->glyphs, glyph_cmp);
    }
    if (g == NULL) {
        uint32_t q = '?';
        g = bsearch(&q, f->glyphs, f->nglyphs, sizeof *f->glyphs, glyph_cmp);
    }
    return g;
}

static float text_width(nk_handle h, float height, const char *text, int len)
{
    const ui_font *f = h.ptr;
    float w = 0.0f;
    int i = 0;
    (void)height;
    while (i < len) {
        nk_rune cp = 0;
        int n = nk_utf_decode(text + i, &cp, len - i);
        const ui_glyph *g;
        if (n <= 0) {
            break;
        }
        g = find_glyph(f, cp);
        if (g != NULL) {
            w += g->adv;
        }
        i += n;
    }
    return w;
}

static void query_glyph(nk_handle h, float height, struct nk_user_font_glyph *out, nk_rune cp, nk_rune next)
{
    const ui_font *f = h.ptr;
    const ui_glyph *g = find_glyph(f, cp);
    (void)height;
    (void)next;
    memset(out, 0, sizeof *out);
    if (g == NULL) {
        return;
    }
    out->uv[0] = nk_vec2(g->u0, g->v0);
    out->uv[1] = nk_vec2(g->u1, g->v1);
    out->offset = nk_vec2(g->ox, g->oy);
    out->width = g->w;
    out->height = g->h;
    out->xadvance = g->adv;
}

void ui_font_fini(ui_font *f)
{
    if (f == NULL) {
        return;
    }
    free(f->glyphs);
    free(f->atlas);
    pc_buf_fini(&f->file);
    memset(f, 0, sizeof *f);
}

static size_t count_codepoints(void)
{
    size_t n = 0;
    size_t i;
    for (i = 0; i < sizeof RANGES / sizeof RANGES[0]; i++) {
        n += RANGES[i][1] - RANGES[i][0] + 1;
    }
    return n;
}

pc_status ui_font_init(ui_font *f, const char *path, float size, float scale, pc_error *err)
{
    FT_Library lib = NULL;
    FT_Face face = NULL;
    pc_status s = PC_OK;
    size_t cap = count_codepoints();
    int pen_x = PAD + 2 + PAD; /* the white texel block sits at (PAD, PAD), 2x2 */
    int pen_y = PAD;
    int row_h = 2;
    size_t r;
    unsigned px;

    memset(f, 0, sizeof *f);
    if (!(size > 0.0f && size < 512.0f && scale > 0.0f && scale <= 8.0f)) {
        return pc_fail(err, PC_ERR_INVALID, "bad font size %g or scale %g", (double)size, (double)scale);
    }
    f->size = size;
    f->scale = scale;
    f->atlas_w = ATLAS_W;
    f->atlas_h = ATLAS_MAX_H;
    f->glyphs = pc_calloc_array(cap, sizeof *f->glyphs);
    f->atlas = pc_calloc_array((size_t)ATLAS_W * ATLAS_MAX_H, 4);
    if (f->glyphs == NULL || f->atlas == NULL) {
        s = pc_fail(err, PC_ERR_NOMEM, "out of memory building the font atlas");
        goto cleanup;
    }
    s = pc_file_read(path, MAX_FONT_BYTES, &f->file, err);
    if (s != PC_OK) {
        goto cleanup;
    }
    if (FT_Init_FreeType(&lib) != 0) {
        s = pc_fail(err, PC_ERR_UNSUPPORTED, "FreeType failed to initialise");
        goto cleanup;
    }
    if (f->file.len > (size_t)LONG_MAX ||
        FT_New_Memory_Face(lib, f->file.data, (FT_Long)f->file.len, 0, &face) != 0) {
        s = pc_fail(err, PC_ERR_FORMAT, "%s: not a font FreeType can read", path);
        goto cleanup;
    }
    px = (unsigned)lroundf(size * scale);
    if (FT_Set_Pixel_Sizes(face, 0, px) != 0) {
        s = pc_fail(err, PC_ERR_FORMAT, "%s: cannot set size %upx", path, px);
        goto cleanup;
    }
    f->ascent = (float)face->size->metrics.ascender / 64.0f / scale;
    f->nk.height = (float)(face->size->metrics.ascender - face->size->metrics.descender) / 64.0f / scale;

    /* Solid white block for untextured shapes. */
    {
        int y;
        int x;
        for (y = PAD; y < PAD + 2; y++) {
            for (x = PAD; x < PAD + 2; x++) {
                memset(f->atlas + ((size_t)y * ATLAS_W + (size_t)x) * 4, 0xFF, 4);
            }
        }
        f->white_uv = nk_vec2((float)(PAD + 1) / (float)ATLAS_W, (float)(PAD + 1) / (float)ATLAS_MAX_H);
    }

    for (r = 0; r < sizeof RANGES / sizeof RANGES[0]; r++) {
        uint32_t cp;
        for (cp = RANGES[r][0]; cp <= RANGES[r][1]; cp++) {
            FT_GlyphSlot slot;
            ui_glyph *g;
            int bw;
            int bh;
            int y;
            if (FT_Get_Char_Index(face, cp) == 0 || FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) {
                continue; /* not in this font */
            }
            slot = face->glyph;
            if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && slot->bitmap.width > 0) {
                continue;
            }
            bw = (int)slot->bitmap.width;
            bh = (int)slot->bitmap.rows;
            if (bw > ATLAS_W - 2 * PAD) {
                continue;
            }
            if (pen_x + bw + PAD > ATLAS_W) { /* next shelf */
                pen_x = PAD;
                pen_y += row_h + PAD;
                row_h = 0;
            }
            if (pen_y + bh + PAD > ATLAS_MAX_H) {
                s = pc_fail(err, PC_ERR_LIMIT, "font atlas full at U+%04X (size %upx)", (unsigned)cp, px);
                goto cleanup;
            }
            for (y = 0; y < bh; y++) {
                const unsigned char *src = slot->bitmap.buffer + (ptrdiff_t)y * slot->bitmap.pitch;
                uint8_t *dst = f->atlas + ((size_t)(pen_y + y) * ATLAS_W + (size_t)pen_x) * 4;
                int x;
                for (x = 0; x < bw; x++) {
                    dst[4 * x] = 0xFF;
                    dst[4 * x + 1] = 0xFF;
                    dst[4 * x + 2] = 0xFF;
                    dst[4 * x + 3] = src[x];
                }
            }
            g = &f->glyphs[f->nglyphs++];
            g->cp = cp;
            g->u0 = (float)pen_x / (float)ATLAS_W;
            g->v0 = (float)pen_y / (float)ATLAS_MAX_H;
            g->u1 = (float)(pen_x + bw) / (float)ATLAS_W;
            g->v1 = (float)(pen_y + bh) / (float)ATLAS_MAX_H;
            g->w = (float)bw / scale;
            g->h = (float)bh / scale;
            g->ox = (float)slot->bitmap_left / scale;
            g->oy = f->ascent - (float)slot->bitmap_top / scale;
            g->adv = (float)slot->advance.x / 64.0f / scale;
            pen_x += bw + PAD;
            if (bh > row_h) {
                row_h = bh;
            }
        }
    }
    if (f->nglyphs == 0) {
        s = pc_fail(err, PC_ERR_FORMAT, "%s: no usable glyphs", path);
        goto cleanup;
    }
    f->nk.userdata = nk_handle_ptr(f);
    f->nk.width = text_width;
    f->nk.query = query_glyph;
cleanup:
    if (face != NULL) {
        (void)FT_Done_Face(face);
    }
    if (lib != NULL) {
        (void)FT_Done_FreeType(lib);
    }
    pc_buf_fini(&f->file); /* glyphs are copied out; FreeType no longer reads it */
    if (s != PC_OK) {
        ui_font_fini(f);
    }
    return s;
}
