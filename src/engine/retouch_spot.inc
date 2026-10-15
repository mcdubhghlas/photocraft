/* Spot Healing Brush (port of the Spot Healing part of crates/engine/src/retouch_cmds.rs).
 * Included into retouch_cmds.c. Fills the stroke area with no source, then gradient-domain blends
 * it into its surroundings; Content-Aware takes its texture from a sampling window around it. */

typedef enum spot_type { SPOT_CONTENT_AWARE, SPOT_CREATE_TEXTURE, SPOT_PROXIMITY_MATCH } spot_type;

/* Dilate a bool mask by r pixels (square structuring element). *out owned (free()). */
static pc_status dilate_mask(size_t w, size_t h, const bool *m, size_t r, bool **out, pc_error *err)
{
    size_t npx = w * h;
    bool *tmp = calloc(npx > 0 ? npx : 1, sizeof(bool));
    bool *res = calloc(npx > 0 ? npx : 1, sizeof(bool));
    if (tmp == NULL || res == NULL) {
        free(tmp);
        free(res);
        return pc_fail(err, PC_ERR_NOMEM, "dilate");
    }
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            if (m[y * w + x]) {
                size_t xlo = x > r ? x - r : 0;
                size_t xhi = (x + r + 1) < w ? (x + r + 1) : w;
                for (size_t xx = xlo; xx < xhi; xx++) {
                    tmp[y * w + xx] = true;
                }
            }
        }
    }
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            if (tmp[y * w + x]) {
                size_t ylo = y > r ? y - r : 0;
                size_t yhi = (y + r + 1) < h ? (y + r + 1) : h;
                for (size_t yy = ylo; yy < yhi; yy++) {
                    res[yy * w + x] = true;
                }
            }
        }
    }
    free(tmp);
    *out = res;
    return PC_OK;
}

/* Copy a translated Proximity Match patch from the *pre-healing* pixels. *out owned (free()); NULL
 * (PC_OK) when any copy would leave the grid (proximity_source_patch's None -> caller falls back). */
static pc_status proximity_source_patch(const float *img, size_t w, size_t h, size_t ch, const bool *mask,
                                        int32_t dx, int32_t dy, float **out, pc_error *err)
{
    *out = NULL;
    if (w == 0 || h == 0 || ch == 0) {
        return PC_OK;
    }
    size_t npx = w * h, len = npx * ch;
    float *res = malloc(len * sizeof(float));
    if (res == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "proximity patch");
    }
    memcpy(res, img, len * sizeof(float));
    for (size_t pidx = 0; pidx < npx; pidx++) {
        if (!mask[pidx]) {
            continue;
        }
        int64_t x = (int64_t)(pidx % w) + dx;
        int64_t y = (int64_t)(pidx / w) + dy;
        if (x < 0 || y < 0 || x >= (int64_t)w || y >= (int64_t)h) {
            free(res);
            *out = NULL;
            return PC_OK; /* None: caller falls back to content-aware */
        }
        size_t src = ((size_t)y * w + (size_t)x) * ch;
        memcpy(&res[pidx * ch], &img[src], ch * sizeof(float));
    }
    *out = res;
    return PC_OK;
}

/* Content-aware completion of `domain` with a membrane fallback. *out owned (free()); never NULL on
 * PC_OK. */
static pc_status spot_content_aware(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                    const bool *domain, float **out, pc_error *err)
{
    pc_algo_complete_params cp = pc_algo_complete_params_default();
    float *res = NULL;
    pc_status rc = pc_algo_inpaint_complete(w, h, ch, img, len, domain, w * h, &cp, NULL, &res, err);
    if (rc == PC_OK && res != NULL) {
        *out = res;
        return PC_OK;
    }
    if (rc != PC_OK) {
        return rc;
    }
    return pc_algo_poisson_membrane_fill(w, h, ch, img, len, domain, w * h, NULL, out, err);
}

struct spot_work {
    const pc_paint_stroke *stroke;
    spot_type kind;
    bool all;
};

static pc_status spot_heal_surface(struct spot_work *c, const pc_document *pre, pc_surface *surf,
                                  const pc_surface *sel, bool lock, pc_rect *out_dmg, pc_error *err)
{
    *out_dmg = PC_RECT_EMPTY;
    pc_rect bounds = PC_RECT_EMPTY;
    float *cov = NULL;
    size_t ncov = 0;
    pc_status rc = pc_paint_stroke_coverage(c->stroke, &bounds, &cov, &ncov, err);
    if (rc != PC_OK) {
        return rc;
    }
    float size = c->stroke->brush.size;
    pc_rect region_rect = pc_engine_fill_sampling_window(bounds, pc_document_bounds(pre));
    if (pc_rect_is_empty(region_rect)) {
        free(cov);
        return PC_OK;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    int32_t margin = (int32_t)(((region_rect.x1 - region_rect.x0 > region_rect.y1 - region_rect.y0)
                                    ? (region_rect.x1 - region_rect.x0)
                                    : (region_rect.y1 - region_rect.y0)) /
                               2);
    pc_paint_region img = {0};
    if (c->all) {
        rc = composite_region(pre, false, 0, SAMPLE_ALL, region_rect, fmt, &img, err);
    } else {
        rc = pc_paint_region_read(surf, region_rect, &img, err);
    }
    if (rc != PC_OK) {
        free(cov);
        return rc;
    }
    size_t w = pc_paint_region_width(&img), h = pc_paint_region_height(&img), ch = img.ch;
    size_t npx = w * h;
    size_t bw = (size_t)pc_rect_width(bounds);
    float *stroke_cov = calloc(npx > 0 ? npx : 1, sizeof(float));
    bool *hole = NULL, *domain = NULL, *heal_mask = NULL;
    float *filled = NULL;
    pc_paint_region healed = {0};
    float *dmg_cov = NULL;
    if (stroke_cov == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "spot heal");
        goto done;
    }
    for (size_t i = 0; i < ncov && bw > 0; i++) {
        int32_t x = bounds.x0 + (int32_t)(i % bw), y = bounds.y0 + (int32_t)(i / bw);
        if (pc_rect_contains(region_rect, x, y)) {
            size_t ix = (size_t)(y - region_rect.y0) * w + (size_t)(x - region_rect.x0);
            if (ix < npx) {
                stroke_cov[ix] = cov[i];
            }
        }
    }
    hole = malloc((npx > 0 ? npx : 1) * sizeof(bool));
    if (hole == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "spot heal");
        goto done;
    }
    bool any = false;
    for (size_t i = 0; i < npx; i++) {
        hole[i] = stroke_cov[i] > 0.0f;
        any = any || hole[i];
    }
    if (!any) {
        goto done; /* nothing covered */
    }
    rc = dilate_mask(w, h, hole, 2, &domain, err);
    if (rc == PC_OK) {
        rc = dilate_mask(w, h, hole, 1, &heal_mask, err);
    }
    if (rc != PC_OK) {
        goto done;
    }
    switch (c->kind) {
    case SPOT_CONTENT_AWARE:
        rc = spot_content_aware(w, h, ch, img.data, img.len, domain, &filled, err);
        break;
    case SPOT_CREATE_TEXTURE: {
        float blk = size / 4.0f;
        size_t block = (size_t)clampf(blk, 6.0f, 32.0f);
        rc = pc_algo_inpaint_synthesize(w, h, ch, img.data, img.len, domain, npx, block, 0x5eedu, &filled,
                                        err);
        break;
    }
    case SPOT_PROXIMITY_MATCH: {
        size_t ring = (size_t)clampf(size / 8.0f, 3.0f, 16.0f);
        bool found = false;
        int32_t dx = 0, dy = 0;
        rc = pc_algo_inpaint_best_offset(w, h, ch, img.data, img.len, domain, npx, ring, margin, &found, &dx,
                                         &dy, err);
        if (rc == PC_OK && found) {
            rc = proximity_source_patch(img.data, w, h, ch, domain, dx, dy, &filled, err);
            if (rc == PC_OK && filled == NULL) {
                rc = spot_content_aware(w, h, ch, img.data, img.len, domain, &filled, err);
            }
        } else if (rc == PC_OK) {
            rc = spot_content_aware(w, h, ch, img.data, img.len, domain, &filled, err);
        }
        break;
    }
    }
    if (rc != PC_OK || filled == NULL) {
        goto done;
    }
    {
        pc_paint_region src = {region_rect, ch, filled, npx * ch};
        rc = heal_region(fmt, &src, &img, heal_mask, &healed, err);
    }
    if (rc != PC_OK) {
        goto done;
    }
    {
        pc_rect dmg = pc_rect_intersect(bounds, region_rect);
        if (pc_rect_is_empty(dmg)) {
            goto done;
        }
        size_t dn = (size_t)pc_rect_width(dmg) * (size_t)pc_rect_height(dmg);
        dmg_cov = malloc((dn > 0 ? dn : 1) * sizeof(float));
        if (dmg_cov == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "spot heal");
            goto done;
        }
        size_t dw = (size_t)pc_rect_width(dmg);
        for (int32_t y = dmg.y0; y < dmg.y1; y++) {
            for (int32_t x = dmg.x0; x < dmg.x1; x++) {
                size_t ix = (size_t)(y - region_rect.y0) * w + (size_t)(x - region_rect.x0);
                dmg_cov[(size_t)(y - dmg.y0) * dw + (size_t)(x - dmg.x0)] = (ix < npx) ? stroke_cov[ix] : 0.0f;
            }
        }
        rc = pc_paint_apply_coverage(surf, dmg, dmg_cov, dn, c->stroke->brush.opacity, sel, lock, &healed,
                                     PC_BLEND_NORMAL, out_dmg, err);
    }
done:
    free(cov);
    free(stroke_cov);
    free(hole);
    free(domain);
    free(heal_mask);
    free(filled);
    free(dmg_cov);
    pc_paint_region_fini(&img);
    pc_paint_region_fini(&healed);
    return rc;
}

static pc_status spot_work_fn(void *tool, const pc_document *pre, pc_surface *surf, const pc_surface *sel,
                             bool lock, pc_rect *dmg, pc_error *err)
{
    return spot_heal_surface(tool, pre, surf, sel, lock, dmg, err);
}

static pc_status spot_healing(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "paint.spotHealing";
    pc_paint_stroke stroke = {0};
    bool has_layer = false;
    pc_layer_id layer = 0;
    pc_status rc = parse_brush(s, p, CMD, &stroke, &has_layer, &layer, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t len = 0;
    const char *ty = string_p(p, "type", "contentAware", &len);
    spot_type kind;
    if (len == 12 && memcmp(ty, "contentAware", 12) == 0) {
        kind = SPOT_CONTENT_AWARE;
    } else if (len == 13 && memcmp(ty, "createTexture", 13) == 0) {
        kind = SPOT_CREATE_TEXTURE;
    } else if (len == 14 && memcmp(ty, "proximityMatch", 14) == 0) {
        kind = SPOT_PROXIMITY_MATCH;
    } else {
        pc_paint_stroke_fini(&stroke);
        return pc_engine_err_bad_params(err, CMD,
                                        "unknown type `%.*s` (contentAware|createTexture|proximityMatch)",
                                        (int)len, ty);
    }
    struct spot_work sw = {&stroke, kind, sample_all_layers(p)};
    struct retouch_edit ed = {has_layer, layer, p, &sw, spot_work_fn, PC_RECT_EMPTY};
    pc_rect dmg = PC_RECT_EMPTY;
    rc = run_stroke(s, "Spot Healing Brush", &ed, &dmg, err);
    pc_paint_stroke_fini(&stroke);
    if (rc != PC_OK) {
        return rc;
    }
    return damage_only(out, dmg, err);
}
