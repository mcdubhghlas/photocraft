#include "vector_preview.h"
#include "shell_internal.h"
#include "base/hash.h"
#include "base/num.h"
#include "base/mem.h"
#include "ui/widgets.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
void pc_ui_vector_preview_fini(pc_ui_vector_preview *p)
{
    pc_vec_compiled_shape_fini(&p->compiled);
    pc_vec_polylines_free(p->paths, p->npaths);
    memset(p, 0, sizeof *p);
}
static pc_status prepare(pc_ui_shell_app *app, pc_ui_vector_preview *p, const pc_ui_canvas_xform *xf,
                         const pc_ui_tool_gesture *g, pc_error *err)
{
    double area = (double)xf->width * xf->height;
    float scale = (float)fmin(pc_ui_shell_app_ui_scale(app), sqrt(2097152. / fmax(1, area)));
    if (!isfinite(scale) || scale <= 0 || !isfinite(area) || area <= 0) {
        return pc_fail(err, PC_ERR_INVALID, "invalid vector preview viewport");
    }
    float origin[2], x[2], y[2];
    if (pc_ui_canvas_to_screen(xf, 0, 0, origin) != PC_OK || pc_ui_canvas_to_screen(xf, 1, 0, x) != PC_OK ||
        pc_ui_canvas_to_screen(xf, 0, 1, y) != PC_OK) {
        return pc_fail(err, PC_ERR_INVALID, "invalid vector preview transform");
    }
    pc_affine affine = {{(x[0] - origin[0]) * scale, (x[1] - origin[1]) * scale, (y[0] - origin[0]) * scale,
                         (y[1] - origin[1]) * scale, origin[0] * scale, origin[1] * scale}};
    pc_rect clip =
        pc_rect_make(pc_f64_to_i32(floor((double)xf->x * scale)), pc_f64_to_i32(floor((double)xf->y * scale)),
                     pc_f64_to_i32(ceil(((double)xf->x + xf->width) * scale)),
                     pc_f64_to_i32(ceil(((double)xf->y + xf->height) * scale)));
    struct {
        uint64_t tag, generation, domain, document;
        double affine[6];
        int32_t clip[4];
        float scale;
    } signature = {0};
    signature.tag = 0x7665637072657631ULL;
    signature.generation = g->preview_revision;
    signature.domain = g->preview_domain;
    signature.document = g->document;
    memcpy(signature.affine, affine.m, sizeof signature.affine);
    signature.clip[0] = clip.x0;
    signature.clip[1] = clip.y0;
    signature.clip[2] = clip.x1;
    signature.clip[3] = clip.y1;
    signature.scale = scale;
    uint8_t key[32];
    pc_blake3(&signature, sizeof signature, key);
    if (p->valid && !memcmp(p->key, key, 32)) {
        return PC_OK;
    }
    pc_shape_layer shape = {0};
    pc_status status = pc_shape_layer_clone(&g->shape_preview, &shape, err);
    pc_path path = {0};
    if (status == PC_OK) {
        status = pc_path_transform(&shape.path, affine, &path, err);
    }
    pc_ui_vector_preview next = {.scale = scale, .clip = clip};
    if (status == PC_OK) {
        pc_path_fini(&shape.path);
        shape.path = path;
        shape.has_live = false;
        if (shape.has_stroke) {
            shape.stroke.width *= xf->zoom * scale;
        }
        status = pc_vec_compiled_shape_new(&shape, .25, clip, &next.compiled, err);
    }
    if (status == PC_OK) {
        status = pc_vec_flatten_path(&shape.path, .4, &next.paths, &next.npaths, err);
    }
    pc_shape_layer_fini(&shape);
    if (status != PC_OK) {
        pc_ui_vector_preview_fini(&next);
        return status;
    }
    memcpy(next.key, key, 32);
    next.valid = true;
    pc_ui_vector_preview_fini(p);
    *p = next;
    return PC_OK;
}
static pc_status tile(pc_ui_shell_app *app, const pc_ui_vector_preview *p, pc_rect rect, struct nk_image *out,
                      pc_error *err)
{
    struct {
        uint8_t preview[32];
        int32_t rect[4];
    } signature = {0};
    memcpy(signature.preview, p->key, 32);
    signature.rect[0] = rect.x0;
    signature.rect[1] = rect.y0;
    signature.rect[2] = rect.x1;
    signature.rect[3] = rect.y1;
    uint8_t key[32];
    pc_blake3(&signature, sizeof signature, key);
    uint16_t w = (uint16_t)pc_rect_width(rect), h = (uint16_t)pc_rect_height(rect);
    pc_status status = pc_ui_shell_bitmap(app, key, w, h, NULL, out, err);
    if (status == PC_OK) {
        return status;
    }
    float (*pixels)[4] = NULL;
    size_t count = 0;
    status = pc_vec_compiled_shape_render_rgba(&p->compiled, rect, &pixels, &count, err);
    uint8_t *rgba = NULL;
    if (status == PC_OK) {
        if (count != (size_t)w * h) {
            status = pc_fail(err, PC_ERR_INVALID, "invalid vector preview tile");
        } else {
            rgba = pc_malloc_array(count, 4);
            if (!rgba) {
                status = pc_fail(err, PC_ERR_NOMEM, "vector preview tile");
            }
        }
    }
    if (status == PC_OK) {
        for (size_t i = 0; i < count; i++) {
            for (size_t ch = 0; ch < 4; ch++) {
                float v = pixels[i][ch];
                rgba[i * 4 + ch] = (uint8_t)lrintf((isfinite(v) ? fminf(1, fmaxf(0, v)) : 0) * 255);
            }
        }
        status = pc_ui_shell_bitmap(app, key, w, h, rgba, out, err);
    }
    free(rgba);
    free(pixels);
    return status;
}
pc_status pc_ui_shell_vector_preview(pc_ui_shell_app *app, pc_ui_vector_preview *p,
                                     const pc_ui_canvas_xform *xf, const pc_ui_tool_gesture *g, pc_error *err)
{
    if (!g->active || !g->has_shape_preview) {
        if (p->valid) {
            pc_ui_vector_preview_fini(p);
        }
        return PC_OK;
    }
    pc_status status = prepare(app, p, xf, g, err);
    if (status != PC_OK) {
        return status;
    }
    struct nk_command_buffer *canvas = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    struct nk_rect saved = canvas->clip;
    nk_push_scissor(canvas, nk_rect(xf->x, xf->y, xf->width, xf->height));
    pc_rect bounds;
    if (pc_vec_compiled_shape_bounds(&p->compiled, &bounds)) {
        bounds = pc_rect_intersect(bounds, p->clip);
        for (int32_t y = bounds.y0; status == PC_OK && y < bounds.y1;) {
            int32_t bottom = y + ((int64_t)bounds.y1 - y > 256 ? 256 : bounds.y1 - y);
            for (int32_t x = bounds.x0; status == PC_OK && x < bounds.x1;) {
                int32_t right = x + ((int64_t)bounds.x1 - x > 256 ? 256 : bounds.x1 - x);
                struct nk_image image;
                status = tile(app, p, pc_rect_make(x, y, right, bottom), &image, err);
                if (status == PC_OK) {
                    nk_draw_image(canvas,
                                  nk_rect((float)x / p->scale, (float)y / p->scale,
                                          (float)(right - x) / p->scale, (float)(bottom - y) / p->scale),
                                  &image, nk_rgba(255, 255, 255, 255));
                }
                x = right;
            }
            y = bottom;
        }
    }
    struct nk_color accent = pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent);
    for (size_t i = 0; i < p->npaths; i++) {
        const pc_vec_polyline *line = &p->paths[i];
        size_t count = line->len;
        for (size_t j = 0; j < count; j++) {
            size_t next = j + 1;
            if (next == count) {
                if (!line->closed) {
                    break;
                }
                next = 0;
            }
            float x0 = (float)(line->pts[j][0] / p->scale), y0 = (float)(line->pts[j][1] / p->scale),
                  x1 = (float)(line->pts[next][0] / p->scale), y1 = (float)(line->pts[next][1] / p->scale);
            if (isfinite(x0) && isfinite(y0) && isfinite(x1) && isfinite(y1)) {
                nk_stroke_line(canvas, x0, y0, x1, y1, 3, nk_rgba(0, 0, 0, 210));
                nk_stroke_line(canvas, x0, y0, x1, y1, 1, accent);
            }
        }
    }
    nk_push_scissor(canvas, saved);
    return status;
}
