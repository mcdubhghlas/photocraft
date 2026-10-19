#include "ui/direct_state.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>
void pc_ui_direct_cancel(pc_ui_direct *s)
{
    pc_path_fini(&s->base);
    pc_path_fini(&s->preview);
    s->active = s->has_preview = false;
}
void pc_ui_direct_fini(pc_ui_direct *s)
{
    pc_ui_direct_cancel(s);
    pc_str_fini(&s->name);
    free(s->anchors);
    memset(s, 0, sizeof *s);
}
pc_status pc_ui_direct_to_json(const pc_ui_direct *s, yyjson_mut_doc *d, yyjson_mut_val **out, pc_error *err)
{
    yyjson_mut_val *o = yyjson_mut_obj(d), *a = yyjson_mut_arr(d);
    bool ok = o && a && yyjson_mut_obj_add_strcpy(d, o, "name", pc_str_c(&s->name)) &&
              yyjson_mut_obj_add_uint(d, o, "layer", s->layer);
    for (size_t i = 0; ok && i < s->count; i++) {
        yyjson_mut_val *p = yyjson_mut_arr(d);
        ok = p && yyjson_mut_arr_add_uint(d, p, s->anchors[i].sub) &&
             yyjson_mut_arr_add_uint(d, p, s->anchors[i].knot) && yyjson_mut_arr_add_val(a, p);
    }
    ok = ok && yyjson_mut_obj_add_val(d, o, "anchors", a);
    yyjson_mut_val *segment = yyjson_mut_null(d);
    if (s->has_segment) {
        segment = yyjson_mut_arr(d);
        ok = ok && segment && yyjson_mut_arr_add_uint(d, segment, s->segment.sub) &&
             yyjson_mut_arr_add_uint(d, segment, s->segment.knot);
    }
    ok = ok && segment && yyjson_mut_obj_add_val(d, o, "segment", segment);
    if (!ok)
        return pc_fail(err, PC_ERR_NOMEM, "direct selection state");
    *out = o;
    return PC_OK;
}
static bool pair(yyjson_val *v, pc_vec_knot_ref *out)
{
    uint64_t a, b;
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 2 || !pc_json_as_u64(yyjson_arr_get(v, 0), &a) ||
        !pc_json_as_u64(yyjson_arr_get(v, 1), &b) || a >= 4096 || b >= 4096)
        return false;
    *out = (pc_vec_knot_ref){(size_t)a, (size_t)b};
    return true;
}
pc_status pc_ui_direct_from_json(pc_ui_direct *s, yyjson_val *v, pc_error *err)
{
    pc_ui_direct next = {0};
    if (!yyjson_is_obj(v))
        return pc_fail(err, PC_ERR_INVALID, "direct selection must be an object");
    yyjson_val *name = pc_json_get(v, "name"), *anchors = pc_json_get(v, "anchors"),
               *segment = pc_json_get(v, "segment"), *layer = pc_json_get(v, "layer");
    size_t len = 0;
    const char *text = pc_json_as_str(name, &len);
    uint64_t id = 0;
    if ((name && (!text || len > 4096)) || (layer && !pc_json_as_u64(layer, &id)) ||
        (anchors && (!yyjson_is_arr(anchors) || yyjson_arr_size(anchors) > 4096)))
        return pc_fail(err, PC_ERR_INVALID, "invalid direct selection");
    pc_status status = text ? pc_str_set(&next.name, text, len, err) : PC_OK;
    next.layer = id;
    if (status == PC_OK && anchors) {
        next.count = yyjson_arr_size(anchors);
        next.anchors = pc_malloc_array(next.count ? next.count : 1, sizeof *next.anchors);
        if (!next.anchors)
            status = pc_fail(err, PC_ERR_NOMEM, "selected anchors");
    }
    for (size_t i = 0; status == PC_OK && i < next.count; i++)
        if (!pair(yyjson_arr_get(anchors, i), &next.anchors[i]))
            status = pc_fail(err, PC_ERR_INVALID, "invalid selected anchor");
    if (status == PC_OK && segment && !yyjson_is_null(segment)) {
        next.has_segment = pair(segment, &next.segment);
        if (!next.has_segment)
            status = pc_fail(err, PC_ERR_INVALID, "invalid selected segment");
    }
    if (status != PC_OK) {
        pc_ui_direct_fini(&next);
        return status;
    }
    pc_ui_direct_fini(s);
    *s = next;
    return PC_OK;
}
