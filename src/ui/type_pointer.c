#include "type_tool.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
void pc_ui_type_pointer_fini(pc_ui_type_pointer *p)
{
    if (p) {
        pc_ui_type_layout_fini(&p->layout);
        memset(p, 0, sizeof *p);
    }
}
static bool local_point(const pc_text_layer *text, pc_ui_tool_point p, pc_point *out)
{
    pc_affine inverse;
    if (!pc_affine_inverse(text->transform, &inverse)) {
        return false;
    }
    *out = pc_affine_apply(inverse, (pc_point){p.x, p.y});
    return isfinite(out->x) && isfinite(out->y);
}
static pc_status caret_at(pc_ui_type_pointer *p, pc_engine_session *session, pc_ui_state *ui,
                          pc_ui_tool_point point, bool extend, pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    const pc_layer *layer = state ? pc_document_layer(pc_doc_arc_get(state->doc), ui->text_edit.layer) : NULL;
    if (!layer || layer->content.kind != PC_LAYER_TEXT) {
        return pc_fail(err, PC_ERR_INVALID, "text layer changed");
    }
    pc_status status = pc_ui_type_layout_get(&p->layout, state, layer->id, err);
    if (status != PC_OK) {
        return status;
    }
    pc_point local;
    if (!local_point(&layer->content.u.text, point, &local)) {
        return pc_fail(err, PC_ERR_INVALID, "singular type transform");
    }
    size_t caret, line;
    const pc_str *text = &layer->content.u.text.text;
    pc_text_hit_char(&p->layout.layout, pc_str_c(text), text->len, (float)local.x, (float)local.y, &caret,
                     &line);
    ui->text_edit.caret = caret;
    if (!extend) {
        ui->text_edit.anchor = caret;
    }
    return PC_OK;
}
static pc_status hit(pc_ui_type_pointer *p, const pc_engine_doc_state *state, pc_ui_tool_point point,
                     float zoom, pc_layer_id *id, pc_error *err)
{
    const pc_document *doc = pc_doc_arc_get(state->doc);
    pc_walk walk = {0};
    pc_status status = pc_document_walk(doc, &walk, err);
    if (status != PC_OK) {
        return status;
    }
    *id = 0;
    for (size_t i = walk.len; i > 0; i--) {
        const pc_walk_item *item = &walk.items[i - 1];
        const pc_layer *layer = item->layer;
        if (layer->content.kind != PC_LAYER_TEXT || !layer->visible) {
            continue;
        }
        bool visible = true;
        for (size_t depth = 1; depth <= item->depth; depth++) {
            const pc_layer *parent = pc_document_layer_at(doc, walk.paths + item->path_start, depth);
            if (!parent || !parent->visible) {
                visible = false;
                break;
            }
        }
        if (!visible) {
            continue;
        }
        pc_point local;
        if (!local_point(&layer->content.u.text, point, &local)) {
            continue;
        }
        status = pc_ui_type_layout_get(&p->layout, state, layer->id, err);
        if (status != PC_OK) {
            break;
        }
        if (pc_text_point_inside(&p->layout.layout, (float)local.x, (float)local.y, 4 / zoom)) {
            *id = layer->id;
            break;
        }
    }
    pc_walk_fini(&walk);
    return status;
}
static int box_handle(const pc_text_layer *text, pc_ui_tool_point point, float zoom)
{
    if (text->shape.kind != PC_TEXT_SHAPE_BOX) {
        return -1;
    }
    float x = text->shape.x, y = text->shape.y, w = text->shape.width, h = text->shape.height;
    const float spots[8][2] = {{x, y},         {x + w, y},         {x + w, y + h},     {x, y + h},
                               {x + w / 2, y}, {x + w, y + h / 2}, {x + w / 2, y + h}, {x, y + h / 2}};
    for (int i = 0; i < 8; i++) {
        pc_point doc = pc_affine_apply(text->transform, (pc_point){spots[i][0], spots[i][1]});
        if (fabs(doc.x - point.x) <= 6 / zoom && fabs(doc.y - point.y) <= 6 / zoom) {
            return i;
        }
    }
    return -1;
}
static pc_status resize_box(pc_ui_type_pointer *p, pc_engine_session *session, pc_ui_state *ui,
                            pc_ui_tool_point point, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    const pc_layer *layer = state ? pc_document_layer(pc_doc_arc_get(state->doc), ui->text_edit.layer) : NULL;
    if (!layer || layer->content.kind != PC_LAYER_TEXT) {
        return pc_fail(err, PC_ERR_INVALID, "type box changed");
    }
    const pc_text_layer *text = &layer->content.u.text;
    pc_point local;
    if (text->shape.kind != PC_TEXT_SHAPE_BOX || !local_point(text, point, &local)) {
        return pc_fail(err, PC_ERR_INVALID, "type box is unavailable");
    }
    double x0 = text->shape.x, y0 = text->shape.y, x1 = x0 + text->shape.width, y1 = y0 + text->shape.height;
    unsigned handle = ui->text_edit.resize;
    if (handle == 0 || handle == 3 || handle == 7) {
        x0 = fmin(local.x, x1 - 4);
    }
    if (handle == 1 || handle == 2 || handle == 5) {
        x1 = fmax(local.x, x0 + 4);
    }
    if (handle == 0 || handle == 1 || handle == 4) {
        y0 = fmin(local.y, y1 - 4);
    }
    if (handle == 2 || handle == 3 || handle == 6) {
        y1 = fmax(local.y, y0 + 4);
    }
    pc_point origin = pc_affine_apply(text->transform, (pc_point){x0, y0});
    char json[512];
    int n = snprintf(json, sizeof json, "{\"box\":[%.17g,%.17g,%.17g,%.17g]}", origin.x, origin.y, x1 - x0,
                     y1 - y0);
    if (n < 0 || (size_t)n >= sizeof json) {
        return pc_fail(err, PC_ERR_LIMIT, "type box coordinates");
    }
    yyjson_doc *params = NULL;
    pc_status status = pc_json_parse(json, (size_t)n, &params, err);
    if (status == PC_OK) {
        status = pc_ui_type_edit_command(session, ui, "type.edit", yyjson_doc_get_root(params), true, false,
                                         out, err);
    }
    yyjson_doc_free(params);
    if (status == PC_OK) {
        p->revision = pc_engine_session_active(session)->revision;
    }
    return status;
}
pc_status pc_ui_type_pointer_event(pc_ui_type_pointer *p, pc_engine_session *session, pc_ui_state *ui,
                                   pc_ui_pointer_kind kind, pc_ui_tool_point point, unsigned modifiers,
                                   float zoom, pc_json_value *out, pc_error *err)
{
    if (!p || !session || !ui) {
        return pc_fail(err, PC_ERR_INVALID, "missing type pointer context");
    }
    if (kind == PC_UI_POINTER_CANCEL) {
        p->pressed = p->selecting = false;
        ui->text_edit.has_resize = false;
        return PC_OK;
    }
    if (!p->pressed && !pc_ui_tool_is_type(ui->tool)) {
        return PC_ERR_NOT_FOUND;
    }
    if (!isfinite(point.x) || !isfinite(point.y) || !isfinite(zoom) || zoom <= 0) {
        return pc_fail(err, PC_ERR_INVALID, "invalid type pointer");
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state) {
        return pc_fail(err, PC_ERR_INVALID, "no text document");
    }
    pc_doc_id document = pc_doc_arc_get(state->doc)->id;
    if (p->pressed && (p->document != document || p->revision != state->revision)) {
        p->pressed = p->selecting = false;
        return pc_fail(err, PC_ERR_INVALID, "document changed during type gesture");
    }
    if (kind == PC_UI_POINTER_DOWN) {
        p->pressed = p->selecting = false;
        pc_layer_id id = 0;
        pc_status status = PC_OK;
        ui->text_edit.has_resize = false;
        if (ui->has_text_edit && (!ui->text_edit.document || ui->text_edit.document == document)) {
            const pc_layer *layer = pc_document_layer(pc_doc_arc_get(state->doc), ui->text_edit.layer);
            int handle = layer && layer->content.kind == PC_LAYER_TEXT
                             ? box_handle(&layer->content.u.text, point, zoom)
                             : -1;
            if (handle >= 0) {
                ui->text_edit.has_resize = true;
                ui->text_edit.resize = (uint8_t)handle;
                p->pressed = true;
                p->document = document;
                p->revision = state->revision;
                return PC_OK;
            }
        }
        if (!(modifiers & PC_UI_MOD_SHIFT) && !pc_ui_tool_is_type_mask(ui->tool)) {
            status = hit(p, state, point, zoom, &id, err);
            if (status != PC_OK) {
                return status;
            }
        }
        if (ui->has_text_edit && id != ui->text_edit.layer) {
            return pc_ui_type_commit(session, ui, modifiers, out, err);
        }
        if (id) {
            if (!ui->has_text_edit) {
                status = pc_ui_type_begin(session, ui, id, false, err);
            }
            if (status != PC_OK) {
                return status;
            }
            status = caret_at(p, session, ui, point, (modifiers & PC_UI_MOD_SHIFT) != 0, err);
            if (status != PC_OK) {
                return status;
            }
            p->selecting = true;
        }
        p->pressed = true;
        p->document = document;
        p->revision = pc_engine_session_active(session)->revision;
        p->start[0] = point.x;
        p->start[1] = point.y;
        return PC_OK;
    }
    if (!p->pressed) {
        return PC_ERR_NOT_FOUND;
    }
    if (ui->has_text_edit && ui->text_edit.has_resize) {
        pc_status status = resize_box(p, session, ui, point, out, err);
        if (kind == PC_UI_POINTER_UP || status != PC_OK) {
            p->pressed = false;
            ui->text_edit.has_resize = false;
        }
        return status;
    }
    if (p->selecting) {
        pc_status status = caret_at(p, session, ui, point, true, err);
        if (kind == PC_UI_POINTER_UP || status != PC_OK) {
            p->pressed = p->selecting = false;
        }
        return status;
    }
    if (kind == PC_UI_POINTER_UP) {
        p->pressed = false;
        const float end[2] = {point.x, point.y};
        return pc_ui_type_create(session, ui, p->start, end, zoom, out, err);
    }
    return PC_OK;
}
