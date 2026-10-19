/* Color Range shares the selection sampler and atomic combine seam in selection_cmds.c. */
static bool cr_color(yyjson_val *v, float rgb[3])
{
    if (yyjson_is_arr(v) && yyjson_arr_size(v) >= 3) {
        for (size_t i = 0; i < 3; i++) {
            yyjson_val *n = yyjson_arr_get(v, i);
            double x = yyjson_get_num(n);
            if (!yyjson_is_num(n) || !isfinite(x))
                return false;
            rgb[i] = (float)fmin(1, fmax(0, x));
        }
        return true;
    }
    if (yyjson_is_str(v) && yyjson_get_len(v) == 7 && yyjson_get_str(v)[0] == '#') {
        const char *s = yyjson_get_str(v);
        for (size_t i = 0; i < 3; i++) {
            unsigned value = 0;
            for (size_t j = 0; j < 2; j++) {
                char c = s[1 + 2 * i + j];
                unsigned digit = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                                 : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a') + 10
                                 : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A') + 10
                                                        : 16;
                if (digit > 15)
                    return false;
                value = value * 16 + digit;
            }
            rgb[i] = (float)value / 255;
        }
        return true;
    }
    return false;
}
static void cr_lab(const pc_transform *transform, const float rgb[3], bool grid, float lab[3])
{
    float source[3];
    for (size_t i = 0; i < 3; i++)
        source[i] = isfinite(rgb[i]) ? fminf(1, fmaxf(0, rgb[i])) : 0;
    float result[3] = {0};
    pc_transform_eval(transform, source, result);
    lab[0] = result[0] * 100;
    lab[1] = result[1] * 255 - 128;
    lab[2] = result[2] * 255 - 128;
    if (grid) {
        float quantized[3];
        pc_algo_quantize_lab8(lab, quantized);
        memcpy(lab, quantized, sizeof quantized);
    }
}
static pc_status cr_points(yyjson_val *p, const char *key, const pc_document *doc, const float (*pixels)[4],
                           pc_rect area, pc_algo_selection_mode mode, float (**colors)[3],
                           float (**positions)[2], size_t *count, pc_error *err)
{
    yyjson_val *array = pc_json_get(p, key);
    if (!array)
        return PC_OK;
    if (!yyjson_is_arr(array) || yyjson_arr_size(array) > 4096)
        return pc_fail(err, PC_ERR_INVALID, "Color Range %s needs at most 4096 [x,y] points", key);
    *count = yyjson_arr_size(array);
    *colors = pc_malloc_array(*count ? *count : 1, sizeof **colors);
    *positions = pc_malloc_array(*count ? *count : 1, sizeof **positions);
    if (!*colors || !*positions)
        return pc_fail(err, PC_ERR_NOMEM, "Color Range samples");
    size_t width = pc_rect_width(area);
    for (size_t i = 0; i < *count; i++) {
        yyjson_val *point = yyjson_arr_get(array, i), *xv = yyjson_arr_get(point, 0),
                   *yv = yyjson_arr_get(point, 1);
        double x = floor(yyjson_get_num(xv)), y = floor(yyjson_get_num(yv));
        if (!yyjson_is_arr(point) || yyjson_arr_size(point) != 2 || !yyjson_is_num(xv) ||
            !yyjson_is_num(yv) || !isfinite(x) || !isfinite(y) || x < area.x0 || x >= area.x1 ||
            y < area.y0 || y >= area.y1)
            return pc_fail(err, PC_ERR_INVALID, "Color Range %s point is outside the canvas", key);
        int32_t ix = (int32_t)x, iy = (int32_t)y;
        if (mode == PC_ALGO_SELECTION_REPLACE && doc->has_selection &&
            pc_surface_sample_channel(&doc->selection, ix, iy, 0) <= 0)
            return pc_fail(err, PC_ERR_INVALID, "Color Range point is outside the active selection");
        size_t index = (size_t)(iy - area.y0) * width + (size_t)(ix - area.x0);
        memcpy((*colors)[i], pixels[index], sizeof(*colors)[i]);
        (*positions)[i][0] = (float)(ix - area.x0);
        (*positions)[i][1] = (float)(iy - area.y0);
    }
    return PC_OK;
}
static pc_status cr_sampled(pc_engine_session *s, yyjson_val *p, const pc_document *doc, float (*pixels)[4],
                            size_t count, pc_rect area, float *mask, pc_error *err)
{
    float (*plus)[3] = NULL, (*minus)[3] = NULL, (*at)[2] = NULL, (*unused)[2] = NULL;
    size_t np = 0, nm = 0;
    pc_algo_selection_mode mode = pmode(p);
    pc_status status =
        cr_points(p, "points", doc, (const float (*)[4])pixels, area, mode, &plus, &at, &np, err);
    if (status == PC_OK)
        status = cr_points(p, "subtractPoints", doc, (const float (*)[4])pixels, area, mode, &minus, &unused,
                           &nm, err);
    yyjson_val *order = pc_json_get(p, "order");
    if (status == PC_OK && order && (!yyjson_is_arr(order) || yyjson_arr_size(order) != np + nm))
        status = pc_fail(err, PC_ERR_INVALID, "Color Range order must list each sample once");
    bool localized = pb(p, "localized", false);
    if (status == PC_OK && localized && !np)
        status = pc_fail(err, PC_ERR_INVALID, "localized clusters need eyedropper points");
    pc_profile source = {0};
    pc_transform *transform = NULL;
    if (status == PC_OK)
        status = pc_engine_composite_profile(doc, &source, err);
    if (status == PC_OK)
        status = pc_transform_cached(&source, pc_builtin_profile(PC_BUILTIN_LAB_D50),
                                     pc_transform_options_default(), &transform, err);
    pc_profile_fini(&source);
    bool grid = doc->depth == PC_SAMPLE_TYPE_U8, has_range = false;
    pc_algo_lab_range range = {0};
    float foreground[3] = {0};
    if (status == PC_OK)
        cr_lab(transform, s->tools.foreground, grid, foreground);
    float fuzz = fmaxf(0, pf(p, "fuzziness", 40));
    yyjson_val *fixed = pc_json_get(p, "colors"), *single = pc_json_get(p, "color");
    if (status == PC_OK && fixed && (!yyjson_is_arr(fixed) || yyjson_arr_size(fixed) > 4096))
        status = pc_fail(err, PC_ERR_INVALID, "Color Range colors must be a bounded list");
    size_t nfixed = yyjson_arr_size(fixed) + (single ? 1u : 0u);
    for (size_t i = 0; status == PC_OK && i < nfixed; i++) {
        float rgb[3], lab[3];
        yyjson_val *v = i < yyjson_arr_size(fixed) ? yyjson_arr_get(fixed, i) : single;
        if (!cr_color(v, rgb)) {
            status = pc_fail(err, PC_ERR_INVALID, "invalid Color Range colour");
            break;
        }
        cr_lab(transform, rgb, grid, lab);
        range = has_range ? pc_algo_lab_range_grow(range, lab) : pc_algo_lab_range_point(lab);
        has_range = true;
    }
    size_t pi = 0, mi = 0;
    for (size_t i = 0; status == PC_OK && i < np + nm; i++) {
        bool add = i < np;
        if (order) {
            yyjson_val *v = yyjson_arr_get(order, i);
            const char *text = yyjson_get_str(v);
            if (!text || yyjson_get_len(v) != 1 || (*text != '+' && *text != '-')) {
                status = pc_fail(err, PC_ERR_INVALID, "Color Range order entries must be + or -");
                break;
            }
            add = *text == '+';
        }
        if ((add && pi >= np) || (!add && mi >= nm)) {
            status = pc_fail(err, PC_ERR_INVALID, "Color Range order must list each sample once");
            break;
        }
        float lab[3];
        cr_lab(transform, add ? plus[pi++] : minus[mi++], grid, lab);
        if (add)
            range = has_range ? pc_algo_lab_range_grow(range, lab) : pc_algo_lab_range_point(lab);
        else
            range = pc_algo_lab_range_subtract(has_range ? range : pc_algo_lab_range_point(foreground), lab,
                                               fuzz);
        has_range = true;
    }
    if (status == PC_OK) {
        if (!has_range)
            range = pc_algo_lab_range_point(foreground);
        for (size_t i = 0; i < count; i++) {
            float lab[3];
            cr_lab(transform, pixels[i], grid, lab);
            memcpy(pixels[i], lab, sizeof lab);
        }
        float radius = fminf(100, fmaxf(0, pf(p, "range", 100))) / 100 *
                       (float)fmax(pc_rect_width(area), pc_rect_height(area));
        status = pc_algo_selection_color_range_lab((const float (*)[4])pixels, count, pc_rect_width(area),
                                                   &range, fuzz, grid, localized, radius,
                                                   (const float (*)[2])at, np, mask, err);
    }
    pc_transform_release(transform);
    free(plus);
    free(minus);
    free(at);
    free(unused);
    return status;
}
static pc_status run_color_range(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    if (!state)
        return pc_engine_err_no_document(err);
    const pc_document *doc = pc_doc_arc_get(state->doc);
    yyjson_val *select = pc_json_get(p, "select");
    const char *preset = select ? yyjson_get_str(select) : "sampledColors";
    if (!preset || (select && strlen(preset) != yyjson_get_len(select)))
        return pc_fail(err, PC_ERR_INVALID, "Color Range select must be a string");
    pc_rect area = pc_document_bounds(doc);
    uint64_t size = (uint64_t)pc_rect_width(area) * pc_rect_height(area);
    if (size > 64000000)
        return pc_fail(err, PC_ERR_LIMIT, "Color Range supports at most 64 million pixels");
    float (*pixels)[4] = NULL, *mask = NULL;
    size_t count = 0;
    pc_cmyk_space *space = pc_engine_doc_cmyk_space(s), *previous = space ? pc_cmyk_space_enter(space) : NULL;
    pc_status status = sample_pixels(s, !strcmp(preset, "outOfGamut") || pb(p, "sampleAllLayers", true),
                                     &area, &pixels, &count, err);
    if (space) {
        pc_cmyk_space_leave(previous);
        pc_cmyk_space_release(space);
    }
    if (status == PC_OK) {
        mask = pc_malloc_array(count ? count : 1, sizeof *mask);
        if (!mask)
            status = pc_fail(err, PC_ERR_NOMEM, "Color Range mask");
    }
    if (status == PC_OK && !strcmp(preset, "sampledColors"))
        status = cr_sampled(s, p, doc, pixels, count, area, mask, err);
    else if (status == PC_OK &&
             (!strcmp(preset, "highlights") || !strcmp(preset, "midtones") || !strcmp(preset, "shadows"))) {
        bool shadow = !strcmp(preset, "shadows"), highlight = !strcmp(preset, "highlights");
        float low = shadow ? 0 : highlight ? 190 : 105, high = shadow ? 65 : highlight ? 255 : 150;
        yyjson_val *tonal = pc_json_get(p, "tonalRange");
        if (tonal) {
            yyjson_val *a = shadow || highlight ? tonal : yyjson_arr_get(tonal, 0),
                       *b = shadow || highlight ? tonal : yyjson_arr_get(tonal, 1);
            double x = yyjson_get_num(a), y = yyjson_get_num(b);
            if (!yyjson_is_num(a) || !yyjson_is_num(b) || !isfinite(x) || !isfinite(y) || x < 0 || y > 255 ||
                x > y || (!shadow && !highlight && (!yyjson_is_arr(tonal) || yyjson_arr_size(tonal) != 2)))
                status = pc_fail(err, PC_ERR_INVALID, "invalid Color Range tonalRange");
            else {
                if (!shadow)
                    low = (float)x;
                if (!highlight)
                    high = (float)y;
            }
        }
        if (status == PC_OK)
            pc_algo_selection_tone_range((const float (*)[4])pixels, count, low, high,
                                         fminf(100, fmaxf(0, pf(p, "fuzziness", 20))) / 100 * 255, mask);
    } else if (status == PC_OK && !strcmp(preset, "outOfGamut")) {
        const pc_profile *proof = pc_builtin_profile(PC_BUILTIN_COATED_CMYK);
        float threshold = PC_GAMUT_DEFAULT_THRESHOLD;
        for (size_t i = 0; i < s->color.nproofs; i++)
            if (s->color.proofs[i].doc == doc->id) {
                proof = &s->color.proofs[i].view.setup.profile;
                threshold = s->color.proofs[i].view.gamut_threshold;
                break;
            }
        pc_profile source = {0};
        pc_gamut_check *check = NULL;
        status = pc_engine_composite_profile(doc, &source, err);
        if (status == PC_OK)
            status = pc_gamut_check_new(&source, proof, threshold, &check, err);
        if (status == PC_OK)
            for (size_t i = 0; i < count; i++)
                mask[i] = pixels[i][3] > 0 && pc_gamut_check_out_of_gamut(check, pixels[i]) ? 1 : 0;
        pc_gamut_check_free(check);
        pc_profile_fini(&source);
    } else if (status == PC_OK) {
        const char *names[] = {"reds", "yellows", "greens", "cyans", "blues", "magentas"};
        size_t hue = 0;
        while (hue < 6 && strcmp(preset, names[hue]))
            hue++;
        if (hue == 6)
            status = pc_fail(err, PC_ERR_INVALID, "unknown Color Range select");
        else
            pc_algo_selection_hue_range((const float (*)[4])pixels, count, (float)hue * 60, mask);
    }
    if (status == PC_OK) {
        bool invert = pb(p, "invert", false);
        pc_algo_selection_mode mode = pmode(p);
        size_t width = pc_rect_width(area);
        for (size_t i = 0; i < count; i++) {
            if (invert)
                mask[i] = 1 - mask[i];
            if (mode == PC_ALGO_SELECTION_REPLACE && doc->has_selection)
                mask[i] =
                    fminf(mask[i], pc_surface_sample_channel(&doc->selection, area.x0 + (int32_t)(i % width),
                                                             area.y0 + (int32_t)(i / width), 0));
        }
        status = set_mask_selection(s, "Color Range", area, mask, count, mode, out, err);
    }
    free(pixels);
    free(mask);
    return status;
}
