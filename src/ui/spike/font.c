#include "font.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <fribidi.h>
#include "base/file.h"
#include "base/mem.h"
#include "text/fonts.h"

enum { ATLAS_SIDE = 2048, MAX_GLYPHS = 8192, MAX_FACES = 24, MAX_FONT_BYTES = 64 << 20 };
enum { MAX_SHAPE_TEXT = 16384, MAX_SHAPE_RUNES = 4096, MAX_SHAPE_CACHE = 64 };
typedef struct ui_positioned_glyph {
    ui_glyph glyph;
    float x, y;
} ui_positioned_glyph;
typedef struct ui_shaped_text {
    char *text;
    int length;
    ui_positioned_glyph *glyphs;
    size_t count, capacity, runs, missing;
    float width;
    uint64_t used;
} ui_shaped_text;
struct ui_font_runtime {
    FT_Library library;
    FT_Face primary, faces[MAX_FACES];
    pc_font *fonts[MAX_FACES];
    size_t nfaces;
    pc_fontdb *database;
    pc_strlist installed_families;
    bool tried_database;
    int pen_x, pen_y, row_height;
    ui_shaped_text shaped[MAX_SHAPE_CACHE];
    uint64_t shape_clock;
    hb_font_t *hbfonts[MAX_FACES + 1];
};
static int compare(const void *key, const void *value)
{
    uint32_t cp = *(const uint32_t *)key, g = ((const ui_glyph *)value)->cp;
    return cp < g ? -1 : cp > g ? 1 : 0;
}
static ui_glyph *lookup(ui_font *font, uint32_t cp)
{
    return bsearch(&cp, font->glyphs, font->nglyphs, sizeof *font->glyphs, compare);
}
static FT_Face face_for(ui_font *font, uint32_t cp)
{
    struct ui_font_runtime *r = font->runtime;
    if (FT_Get_Char_Index(r->primary, cp)) {
        return r->primary;
    }
    for (size_t i = 0; i < r->nfaces; i++) {
        if (FT_Get_Char_Index(r->faces[i], cp)) {
            return r->faces[i];
        }
    }
    if (!r->tried_database) {
        r->tried_database = true;
        (void)pc_fontdb_with_system_fonts(&r->database, NULL);
        if (r->database)
            (void)pc_fontdb_families(r->database, &r->installed_families, NULL);
    }
    if (!r->database || r->nfaces >= MAX_FACES) {
        return NULL;
    }
    size_t n = pc_fontdb_fallback_count(r->database);
    pc_font_query query = {.width = 1, .weight = 400};
    size_t installed = r->installed_families.n < 4096 ? r->installed_families.n : 4096;
    for (size_t i = 0; i < n + installed; i++) {
        const char *family = i < n ? pc_fontdb_fallback(r->database, i) : r->installed_families.v[i - n];
        if (!family) {
            continue;
        }
        pc_font_match match = {0};
        if (pc_fontdb_match(r->database, family, strlen(family), &query, false, &match, NULL) != PC_OK) {
            continue;
        }
        bool already = false;
        for (size_t f = 0; f < r->nfaces; f++) {
            if (pc_font_id(r->fonts[f]) == pc_font_id(match.font)) {
                already = true;
                break;
            }
        }
        if (already) {
            pc_font_release(match.font);
            continue;
        }
        size_t len = 0;
        const uint8_t *bytes = pc_font_data(match.font, &len);
        FT_Face face = NULL;
        if (len > (size_t)MAX_FONT_BYTES || len > (size_t)LONG_MAX ||
            FT_New_Memory_Face(r->library, bytes, (FT_Long)len, (FT_Long)pc_font_index(match.font), &face)) {
            pc_font_release(match.font);
            continue;
        }
        if (!FT_Get_Char_Index(face, cp) ||
            FT_Set_Pixel_Sizes(face, 0, (FT_UInt)lroundf(font->size * font->scale))) {
            (void)FT_Done_Face(face);
            pc_font_release(match.font);
            continue;
        }
        r->faces[r->nfaces] = face;
        r->fonts[r->nfaces++] = match.font;
        return face;
    }
    return NULL;
}
static unsigned glyph_alpha(const FT_Bitmap *bitmap, const unsigned char *row, unsigned x)
{
    switch (bitmap->pixel_mode) {
    case FT_PIXEL_MODE_MONO:
        return (row[x / 8] & (128u >> (x % 8))) ? 255 : 0;
    case FT_PIXEL_MODE_GRAY2:
        return (((unsigned)row[x / 4] >> (6 - 2 * (x % 4))) & 3u) * 85;
    case FT_PIXEL_MODE_GRAY4:
        return (((unsigned)row[x / 2] >> (4 - 4 * (x % 2))) & 15u) * 17;
    case FT_PIXEL_MODE_BGRA:
        return row[4 * x + 3];
    default: {
        unsigned levels = bitmap->num_grays;
        return levels > 1 && levels <= 256 ? (unsigned)row[x] * 255 / (levels - 1) : row[x];
    }
    }
}
static bool glyph_bitmap_valid(const FT_Bitmap *bitmap)
{
    if (!bitmap->width || !bitmap->rows)
        return true;
    size_t bytes;
    switch (bitmap->pixel_mode) {
    case FT_PIXEL_MODE_MONO:
        bytes = ((size_t)bitmap->width + 7) / 8;
        break;
    case FT_PIXEL_MODE_GRAY2:
        bytes = ((size_t)bitmap->width + 3) / 4;
        break;
    case FT_PIXEL_MODE_GRAY4:
        bytes = ((size_t)bitmap->width + 1) / 2;
        break;
    case FT_PIXEL_MODE_GRAY:
        bytes = bitmap->width;
        break;
    case FT_PIXEL_MODE_BGRA:
        bytes = (size_t)bitmap->width * 4;
        break;
    default:
        return false;
    }
    int64_t pitch = bitmap->pitch;
    return bitmap->buffer && (uint64_t)(pitch < 0 ? -pitch : pitch) >= bytes;
}
static ui_glyph *resolve_index(ui_font *font, uint32_t cp, FT_Face face, FT_UInt index)
{
    ui_glyph *existing = lookup(font, cp);
    if (existing) {
        return existing;
    }
    if (!font->runtime) {
        return NULL;
    }
    ui_glyph glyph = {.cp = cp};
    bool usable = false;
    if (face && !FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) {
        FT_GlyphSlot slot = face->glyph;
        unsigned width = slot->bitmap.width, height = slot->bitmap.rows;
        if (width < (unsigned)ATLAS_SIDE - 2 && height < (unsigned)ATLAS_SIDE - 2 &&
            glyph_bitmap_valid(&slot->bitmap)) {
            struct ui_font_runtime *r = font->runtime;
            int x = r->pen_x, y = r->pen_y, row = r->row_height;
            if (x + (int)width + 1 > ATLAS_SIDE) {
                x = 1;
                y += row + 1;
                row = 0;
            }
            if (y + (int)height + 1 <= ATLAS_SIDE && font->nglyphs < MAX_GLYPHS) {
                for (unsigned h = 0; h < height; h++) {
                    const unsigned char *source = slot->bitmap.buffer + (ptrdiff_t)h * slot->bitmap.pitch;
                    uint8_t *dest = font->atlas + ((size_t)(y + (int)h) * ATLAS_SIDE + (size_t)x) * 4;
                    for (unsigned w = 0; w < width; w++) {
                        dest[w * 4] = 255;
                        dest[w * 4 + 1] = 255;
                        dest[w * 4 + 2] = 255;
                        dest[w * 4 + 3] = (uint8_t)glyph_alpha(&slot->bitmap, source, w);
                    }
                }
                glyph.u0 = (float)x / (float)ATLAS_SIDE;
                glyph.v0 = (float)y / (float)ATLAS_SIDE;
                glyph.u1 = (float)(x + (int)width) / (float)ATLAS_SIDE;
                glyph.v1 = (float)(y + (int)height) / (float)ATLAS_SIDE;
                glyph.w = (float)width / font->scale;
                glyph.h = (float)height / font->scale;
                glyph.ox = (float)slot->bitmap_left / font->scale;
                glyph.oy = font->ascent - (float)slot->bitmap_top / font->scale;
                glyph.adv = (float)slot->advance.x / 64 / font->scale;
                r->pen_x = x + (int)width + 1;
                r->pen_y = y;
                r->row_height = row > (int)height ? row : (int)height;
                if (!font->dirty || y < font->dirty_top) {
                    font->dirty_top = y;
                }
                if (!font->dirty || y + (int)height > font->dirty_bottom) {
                    font->dirty_bottom = y + (int)height;
                }
                font->dirty = true;
                usable = true;
            }
        }
    }
    if (!usable) {
        ui_glyph *replacement = lookup(font, 0xfffd);
        if (!replacement) {
            replacement = lookup(font, '?');
        }
        if (!replacement) {
            return NULL;
        }
        glyph = *replacement;
        glyph.cp = cp;
        glyph.missing = true;
    }
    if (font->nglyphs >= MAX_GLYPHS) {
        return lookup(font, '?');
    }
    size_t at = 0;
    while (at < font->nglyphs && font->glyphs[at].cp < cp) {
        at++;
    }
    memmove(font->glyphs + at + 1, font->glyphs + at, (font->nglyphs - at) * sizeof *font->glyphs);
    font->glyphs[at] = glyph;
    font->nglyphs++;
    return &font->glyphs[at];
}
static ui_glyph *resolve(ui_font *font, uint32_t cp)
{
    ui_glyph *existing = cp <= 0x10ffff ? lookup(font, cp) : NULL;
    if (existing)
        return existing;
    FT_Face face = cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff) ? face_for(font, cp) : NULL;
    return resolve_index(font, cp, face, face ? FT_Get_Char_Index(face, cp) : 0);
}
bool ui_font_has_glyph(ui_font *font, uint32_t cp)
{
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return false;
    ui_glyph *glyph = font && font->runtime ? resolve(font, cp) : NULL;
    return glyph && glyph->cp == cp && !glyph->missing;
}
typedef struct ui_shape_run {
    size_t start, count, face;
    int level;
    hb_script_t script;
} ui_shape_run;
typedef struct ui_shape_work {
    FriBidiChar chars[MAX_SHAPE_RUNES];
    FriBidiCharType types[MAX_SHAPE_RUNES];
    FriBidiBracketType brackets[MAX_SHAPE_RUNES];
    FriBidiLevel levels[MAX_SHAPE_RUNES];
    size_t faces[MAX_SHAPE_RUNES];
    hb_script_t scripts[MAX_SHAPE_RUNES];
    ui_shape_run runs[MAX_SHAPE_RUNES];
} ui_shape_work;
static bool needs_shaping(const char *text, int length)
{
    if (!text || length <= 0 || length > MAX_SHAPE_TEXT)
        return false;
    for (int i = 0; i < length;) {
        nk_rune cp = 0;
        int step = nk_utf_decode(text + i, &cp, length - i);
        if (step <= 0 || cp == NK_UTF_INVALID)
            return false;
        if (cp >= 0x500 || (cp >= 0x300 && cp <= 0x36f))
            return true;
        i += step;
    }
    return false;
}
static size_t shape_bytes(const ui_shaped_text *text)
{
    return text->text ? (size_t)text->length + 1 + text->capacity * sizeof *text->glyphs : 0;
}
static void discard_shape(ui_shaped_text *text)
{
    free(text->text);
    free(text->glyphs);
    *text = (ui_shaped_text){0};
}
static ui_shaped_text *shape_text(ui_font *font, const char *text, int length)
{
    if (!font || !font->runtime || !needs_shaping(text, length))
        return NULL;
    struct ui_font_runtime *runtime = font->runtime;
    if (runtime->shape_clock == UINT64_MAX) {
        runtime->shape_clock = 0;
        for (size_t i = 0; i < MAX_SHAPE_CACHE; i++)
            runtime->shaped[i].used = 0;
    }
    uint64_t clock = ++runtime->shape_clock;
    size_t slot = 0;
    for (size_t i = 0; i < MAX_SHAPE_CACHE; i++) {
        ui_shaped_text *cached = &runtime->shaped[i];
        if (cached->text && cached->length == length && !memcmp(cached->text, text, (size_t)length)) {
            cached->used = clock;
            return cached;
        }
        if (!cached->text || cached->used < runtime->shaped[slot].used)
            slot = i;
    }
    ui_shape_work *work = pc_calloc_array(1, sizeof *work);
    if (!work)
        return NULL;
    size_t count = 0;
    for (int at = 0; at < length;) {
        nk_rune cp = 0;
        int step = nk_utf_decode(text + at, &cp, length - at);
        if (step <= 0 || cp == NK_UTF_INVALID || count == MAX_SHAPE_RUNES) {
            free(work);
            return NULL;
        }
        work->chars[count] = cp;
        FT_Face face = face_for(font, cp);
        size_t face_index = 0;
        if (face && face != runtime->primary)
            for (size_t i = 0; i < runtime->nfaces; i++)
                if (runtime->faces[i] == face) {
                    face_index = i + 1;
                    break;
                }
        hb_script_t script = hb_unicode_script(hb_unicode_funcs_get_default(), cp);
        if (count && (script == HB_SCRIPT_INHERITED || cp == 0x200c || cp == 0x200d)) {
            size_t previous = work->faces[count - 1];
            FT_Face previous_face = previous ? runtime->faces[previous - 1] : runtime->primary;
            if (FT_Get_Char_Index(previous_face, cp) || cp == 0x200c || cp == 0x200d)
                face_index = previous;
        }
        work->faces[count] = face_index;
        work->scripts[count] = hb_unicode_script(hb_unicode_funcs_get_default(), cp);
        count++;
        at += step;
    }
    if (!count) {
        free(work);
        return NULL;
    }
    fribidi_get_bidi_types(work->chars, (FriBidiStrIndex)count, work->types);
    fribidi_get_bracket_types(work->chars, (FriBidiStrIndex)count, work->types, work->brackets);
    FriBidiParType direction = FRIBIDI_PAR_ON;
    if (!fribidi_get_par_embedding_levels_ex(work->types, work->brackets, (FriBidiStrIndex)count, &direction,
                                             work->levels)) {
        free(work);
        return NULL;
    }
    hb_script_t preceding = HB_SCRIPT_UNKNOWN;
    for (size_t i = 0; i < count; i++) {
        hb_script_t script = work->scripts[i];
        if (script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_UNKNOWN) {
            if (preceding == HB_SCRIPT_UNKNOWN)
                for (size_t j = i + 1; j < count; j++)
                    if (work->scripts[j] != HB_SCRIPT_COMMON && work->scripts[j] != HB_SCRIPT_INHERITED &&
                        work->scripts[j] != HB_SCRIPT_UNKNOWN) {
                        preceding = work->scripts[j];
                        break;
                    }
            work->scripts[i] = preceding;
        } else
            preceding = script;
    }
    size_t nruns = 0;
    int max_level = 0, lowest_odd = 256;
    for (size_t i = 0; i < count;) {
        size_t end = i + 1;
        while (end < count && work->faces[end] == work->faces[i] && work->levels[end] == work->levels[i] &&
               work->scripts[end] == work->scripts[i])
            end++;
        int level = work->levels[i];
        work->runs[nruns++] = (ui_shape_run){i, end - i, work->faces[i], level, work->scripts[i]};
        if (level > max_level)
            max_level = level;
        if ((level & 1) && level < lowest_odd)
            lowest_odd = level;
        i = end;
    }
    /* Unicode bidi L2: reverse contiguous runs from the highest level to the lowest odd level. */
    for (int level = max_level; level >= lowest_odd; level--)
        for (size_t i = 0; i < nruns;) {
            if (work->runs[i].level < level) {
                i++;
                continue;
            }
            size_t end = i + 1;
            while (end < nruns && work->runs[end].level >= level)
                end++;
            for (size_t a = i, b = end - 1; a < b; a++, b--) {
                ui_shape_run swap = work->runs[a];
                work->runs[a] = work->runs[b];
                work->runs[b] = swap;
            }
            i = end;
        }
    ui_shaped_text result = {.length = length, .used = clock, .runs = nruns};
    result.text = pc_strndup(text, (size_t)length);
    bool ok = result.text != NULL;
    for (size_t i = 0; ok && i < nruns; i++) {
        ui_shape_run *run = &work->runs[i];
        FT_Face face = run->face ? runtime->faces[run->face - 1] : runtime->primary;
        hb_font_t *hbfont = runtime->hbfonts[run->face];
        if (!hbfont) {
            hbfont = hb_ft_font_create_referenced(face);
            hb_ft_font_set_load_flags(hbfont, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT);
            runtime->hbfonts[run->face] = hbfont;
        }
        hb_buffer_t *buffer = hb_buffer_create();
        hb_buffer_add_utf32(buffer, work->chars, (int)count, (unsigned)run->start, (int)run->count);
        hb_buffer_set_direction(buffer, (run->level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buffer, run->script);
        hb_buffer_set_language(buffer, hb_language_get_default());
        hb_shape(hbfont, buffer, NULL, 0);
        unsigned n = 0;
        hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buffer, &n);
        hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, NULL);
        if (!hb_buffer_allocation_successful(buffer) || !n || n > MAX_SHAPE_RUNES * 4 ||
            result.count > MAX_SHAPE_RUNES * 4 - n)
            ok = false;
        if (ok) {
            ui_positioned_glyph *grown =
                pc_grow_array(result.glyphs, &result.capacity, result.count + n, sizeof *result.glyphs);
            if (!grown)
                ok = false;
            else
                result.glyphs = grown;
        }
        for (unsigned g = 0; ok && g < n; g++) {
            if (infos[g].codepoint > 65535) {
                ok = false;
                break;
            }
            uint32_t key = 0x110000 + (uint32_t)run->face * 65536 + infos[g].codepoint;
            ui_glyph *glyph = resolve_index(font, key, face, infos[g].codepoint);
            if (!glyph) {
                ok = false;
                break;
            }
            if (!infos[g].codepoint || glyph->missing || glyph->cp != key)
                result.missing++;
            float divisor = 64 * font->scale;
            result.glyphs[result.count++] =
                (ui_positioned_glyph){*glyph, result.width + (float)positions[g].x_offset / divisor,
                                      -(float)positions[g].y_offset / divisor};
            result.width += (float)positions[g].x_advance / divisor;
        }
        hb_buffer_destroy(buffer);
    }
    free(work);
    if (!ok || !isfinite(result.width) || result.width < 0 || shape_bytes(&result) > 4 * 1024 * 1024) {
        discard_shape(&result);
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; i < MAX_SHAPE_CACHE; i++)
        used += shape_bytes(&runtime->shaped[i]);
    used -= shape_bytes(&runtime->shaped[slot]);
    discard_shape(&runtime->shaped[slot]);
    while (used + shape_bytes(&result) > 4 * 1024 * 1024) {
        size_t oldest = MAX_SHAPE_CACHE;
        for (size_t i = 0; i < MAX_SHAPE_CACHE; i++)
            if (runtime->shaped[i].text &&
                (oldest == MAX_SHAPE_CACHE || runtime->shaped[i].used < runtime->shaped[oldest].used))
                oldest = i;
        if (oldest == MAX_SHAPE_CACHE)
            break;
        used -= shape_bytes(&runtime->shaped[oldest]);
        discard_shape(&runtime->shaped[oldest]);
    }
    runtime->shaped[slot] = result;
    return &runtime->shaped[slot];
}
bool ui_font_shape_metrics(ui_font *font, const char *text, int length, ui_font_shape_info *out)
{
    ui_shaped_text *shaped = shape_text(font, text, length);
    if (!shaped)
        return false;
    if (out)
        *out = (ui_font_shape_info){shaped->count, shaped->runs, shaped->missing, shaped->width};
    return true;
}
static nk_bool draw_shaped(nk_handle handle, struct nk_draw_list *list, struct nk_rect rect, const char *text,
                           int length, float height, struct nk_color color)
{
    ui_font *font = handle.ptr;
    ui_shaped_text *shaped = shape_text(font, text, length);
    (void)height;
    if (!shaped)
        return nk_false;
    color.a = (nk_byte)((float)color.a * list->config.global_alpha);
    for (size_t i = 0; i < shaped->count; i++) {
        const ui_positioned_glyph *g = &shaped->glyphs[i];
        if (g->glyph.w <= 0 || g->glyph.h <= 0)
            continue;
        struct nk_image image =
            nk_subimage_handle(font->nk.texture, (nk_ushort)font->atlas_w, (nk_ushort)font->atlas_h,
                               nk_rect(g->glyph.u0 * (float)font->atlas_w, g->glyph.v0 * (float)font->atlas_h,
                                       g->glyph.w * font->scale, g->glyph.h * font->scale));
        nk_draw_list_add_image(
            list, image,
            nk_rect(rect.x + g->x + g->glyph.ox, rect.y + g->y + g->glyph.oy, g->glyph.w, g->glyph.h), color);
    }
    return nk_true;
}

static float text_width(nk_handle handle, float height, const char *text, int len)
{
    ui_font *font = handle.ptr;
    float width = 0;
    (void)height;
    ui_shaped_text *shaped = shape_text(font, text, len);
    if (shaped)
        return shaped->width;
    for (int i = 0; i < len;) {
        nk_rune cp = 0;
        int n = nk_utf_decode(text + i, &cp, len - i);
        if (n <= 0) {
            break;
        }
        ui_glyph *glyph = resolve(font, cp);
        if (glyph) {
            width += glyph->adv;
        }
        i += n;
    }
    return width;
}
static void query_glyph(nk_handle handle, float height, struct nk_user_font_glyph *out, nk_rune cp,
                        nk_rune next)
{
    ui_glyph *glyph = resolve(handle.ptr, cp);
    (void)height;
    (void)next;
    memset(out, 0, sizeof *out);
    if (!glyph) {
        return;
    }
    out->uv[0] = nk_vec2(glyph->u0, glyph->v0);
    out->uv[1] = nk_vec2(glyph->u1, glyph->v1);
    out->offset = nk_vec2(glyph->ox, glyph->oy);
    out->width = glyph->w;
    out->height = glyph->h;
    out->xadvance = glyph->adv;
}
void ui_font_fini(ui_font *font)
{
    if (!font) {
        return;
    }
    struct ui_font_runtime *r = font->runtime;
    if (r) {
        for (size_t i = 0; i < MAX_SHAPE_CACHE; i++) {
            free(r->shaped[i].text);
            free(r->shaped[i].glyphs);
        }
        for (size_t i = 0; i <= MAX_FACES; i++)
            if (r->hbfonts[i])
                hb_font_destroy(r->hbfonts[i]);

        for (size_t i = 0; i < r->nfaces; i++) {
            (void)FT_Done_Face(r->faces[i]);
            pc_font_release(r->fonts[i]);
        }
        if (r->primary) {
            (void)FT_Done_Face(r->primary);
        }
        pc_strlist_fini(&r->installed_families);
        pc_fontdb_free(r->database);
        if (r->library) {
            (void)FT_Done_FreeType(r->library);
        }
        free(r);
    }
    free(font->glyphs);
    free(font->atlas);
    pc_buf_fini(&font->file);
    memset(font, 0, sizeof *font);
}
pc_status ui_font_init(ui_font *font, const char *path, float size, float scale, pc_error *err)
{
    memset(font, 0, sizeof *font);
    if (!path || !isfinite(size) || !isfinite(scale) || size <= 0 || size >= 512 || scale <= 0 || scale > 8) {
        return pc_fail(err, PC_ERR_INVALID, "invalid UI font size or scale");
    }
    font->size = size;
    font->scale = scale;
    font->atlas_w = font->atlas_h = ATLAS_SIDE;
    font->runtime = pc_calloc_array(1, sizeof *font->runtime);
    font->glyphs = pc_calloc_array(MAX_GLYPHS, sizeof *font->glyphs);
    font->atlas = pc_calloc_array((size_t)ATLAS_SIDE * ATLAS_SIDE, 4);
    pc_status status = PC_OK;
    if (!font->runtime || !font->glyphs || !font->atlas) {
        status = pc_fail(err, PC_ERR_NOMEM, "UI font atlas");
        goto fail;
    }
    status = pc_file_read(path, MAX_FONT_BYTES, &font->file, err);
    if (status != PC_OK) {
        goto fail;
    }
    struct ui_font_runtime *r = font->runtime;
    if (FT_Init_FreeType(&r->library)) {
        status = pc_fail(err, PC_ERR_UNSUPPORTED, "FreeType initialization failed");
        goto fail;
    }
    if (font->file.len > (size_t)LONG_MAX ||
        FT_New_Memory_Face(r->library, font->file.data, (FT_Long)font->file.len, 0, &r->primary)) {
        status = pc_fail(err, PC_ERR_FORMAT, "cannot read UI font");
        goto fail;
    }
    FT_UInt pixels = (FT_UInt)lroundf(size * scale);
    if (!pixels) {
        pixels = 1;
    }
    if (FT_Set_Pixel_Sizes(r->primary, 0, pixels)) {
        status = pc_fail(err, PC_ERR_FORMAT, "cannot size UI font");
        goto fail;
    }
    font->ascent = (float)r->primary->size->metrics.ascender / 64 / scale;
    font->nk.height =
        (float)(r->primary->size->metrics.ascender - r->primary->size->metrics.descender) / 64 / scale;
    for (size_t y = 1; y < 3; y++) {
        for (size_t x = 1; x < 3; x++) {
            memset(font->atlas + (y * ATLAS_SIDE + x) * 4, 255, 4);
        }
    }
    font->white_uv = nk_vec2(2.f / (float)ATLAS_SIDE, 2.f / (float)ATLAS_SIDE);
    r->pen_x = 5;
    r->pen_y = 1;
    r->row_height = 2;
    if (!ui_font_has_glyph(font, '?')) {
        status = pc_fail(err, PC_ERR_FORMAT, "UI font has no replacement glyph");
        goto fail;
    }
    (void)resolve(font, 0xfffd);
    for (uint32_t cp = 0x20; cp <= 0x7e; cp++) {
        (void)resolve(font, cp);
    }
    font->nk.userdata = nk_handle_ptr(font);
    font->nk.width = text_width;
    font->nk.query = query_glyph;
    font->nk.shape = draw_shaped;
    return PC_OK;
fail:
    ui_font_fini(font);
    return status;
}
