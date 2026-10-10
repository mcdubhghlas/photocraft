/* Layout result data (see layout_data.h). */
#include "text/layout_data.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/rmath.h"

void pc_text_layout_init(pc_text_layout *l)
{
    memset(l, 0, sizeof *l);
}

void pc_text_layout_fini(pc_text_layout *l)
{
    size_t i;
    if (l == NULL) {
        return;
    }
    for (i = 0; i < l->nfaces; i++) {
        pc_font_release(l->faces[i].font);
        free(l->faces[i].coords);
    }
    for (i = 0; i < l->nstyles; i++) {
        pc_char_style_fini(&l->styles[i]);
    }
    free(l->faces);
    free(l->glyphs);
    free(l->decorations);
    free(l->lines);
    free(l->clusters);
    free(l->styles);
    memset(l, 0, sizeof *l);
}

void pc_text_layout_to_text(const pc_text_layout *l, float x, float y, float *ox, float *oy)
{
    if (l->vertical) {
        *ox = -y;
        *oy = x;
    } else {
        *ox = x;
        *oy = y;
    }
}

void pc_text_layout_to_line(const pc_text_layout *l, float x, float y, float *ox, float *oy)
{
    if (l->vertical) {
        *ox = y;
        *oy = -x;
    } else {
        *ox = x;
        *oy = y;
    }
}

bool pc_text_layout_line_bounds(const pc_text_layout *l, float out[4])
{
    float a[4];
    size_t i;
    if (l->nlines == 0) {
        return false;
    }
    for (i = 0; i < l->nlines; i++) {
        const pc_line_info *li = &l->lines[i];
        float r[4] = {li->x0, li->baseline - li->ascent, li->x1, li->baseline + li->descent};
        if (i == 0) {
            memcpy(a, r, sizeof a);
        } else {
            a[0] = pc_rs_minf(a[0], r[0]);
            a[1] = pc_rs_minf(a[1], r[1]);
            a[2] = pc_rs_maxf(a[2], r[2]);
            a[3] = pc_rs_maxf(a[3], r[3]);
        }
    }
    memcpy(out, a, sizeof a);
    return true;
}

bool pc_text_layout_bounds(const pc_text_layout *l, float out[4])
{
    float b[4];
    if (!pc_text_layout_line_bounds(l, b)) {
        return false;
    }
    if (l->vertical) {
        out[0] = -b[3];
        out[1] = b[0];
        out[2] = -b[1];
        out[3] = b[2];
    } else {
        memcpy(out, b, sizeof b);
    }
    return true;
}

pc_status pc_text_layout_add_face(pc_text_layout *l, pc_font *font, const int16_t *coords, size_t ncoords,
                                  float size_px, bool embolden, float skew_deg, uint32_t *index,
                                  pc_error *err)
{
    pc_glyph_face *faces;
    int16_t *c = NULL;
    if (font == NULL || (ncoords > 0 && coords == NULL) || l->nfaces >= UINT32_MAX) {
        return pc_fail(err, PC_ERR_INVALID, "layout: invalid face");
    }
    {
        size_t cap = l->nfaces;
        faces = pc_grow_array(l->faces, &cap, l->nfaces + 1, sizeof *faces);
    }
    if (faces == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "layout: out of memory");
    }
    l->faces = faces;
    if (ncoords > 0) {
        c = pc_malloc_array(ncoords, sizeof *c);
        if (c == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "layout: out of memory");
        }
        memcpy(c, coords, ncoords * sizeof *c);
    }
    faces[l->nfaces] = (pc_glyph_face){pc_font_retain(font), c, ncoords, size_px, embolden, skew_deg};
    if (index != NULL) {
        *index = (uint32_t)l->nfaces;
    }
    l->nfaces++;
    return PC_OK;
}
