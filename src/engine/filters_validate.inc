/* Validate new core-filter invocations (port of crates/engine/src/filters/validation.rs, E9).
 * Included by filters_cmds.c; all symbols static. Returns PC_OK for ids with no rules (non-filter
 * commands and Last Filter), matching the reference's gallery fallback for everything this chunk
 * does not register. */

typedef enum { VR_NUM, VR_FINITE, VR_UNS, VR_CHOICE, VR_BOOL } vrkind;
typedef struct vrule {
    const char *name;
    vrkind kind;
    double min, max;      /* VR_NUM */
    uint64_t umax;        /* VR_UNS */
    const char *choices;  /* VR_CHOICE: pipe-separated */
} vrule;

static bool vr_choice_has(const char *choices, const char *s, size_t slen)
{
    const char *c = choices;
    while (*c != '\0') {
        const char *e = c;
        while (*e != '\0' && *e != '|') {
            e++;
        }
        size_t len = (size_t)(e - c);
        if (len == slen && memcmp(c, s, slen) == 0) {
            return true;
        }
        c = (*e == '|') ? e + 1 : e;
    }
    return false;
}

static bool vr_accepts(const vrule *r, yyjson_val *v)
{
    switch (r->kind) {
    case VR_NUM: {
        double n;
        return pc_json_as_f64(v, &n) && isfinite(n) && n >= r->min && n <= r->max;
    }
    case VR_FINITE: {
        double n;
        return pc_json_as_f64(v, &n) && isfinite(n);
    }
    case VR_UNS:
        return yyjson_is_uint(v) && yyjson_get_uint(v) <= r->umax;
    case VR_CHOICE: {
        if (!yyjson_is_str(v)) {
            return false;
        }
        return vr_choice_has(r->choices, yyjson_get_str(v), yyjson_get_len(v));
    }
    case VR_BOOL:
        return yyjson_is_bool(v);
    }
    return false;
}

static void vr_expected(const vrule *r, char *buf, size_t cap)
{
    switch (r->kind) {
    case VR_FINITE:
        (void)snprintf(buf, cap, "a finite number");
        break;
    case VR_NUM:
        (void)snprintf(buf, cap, "a number in %g..%g", r->min, r->max);
        break;
    case VR_UNS:
        (void)snprintf(buf, cap, "an integer in 0..%llu", (unsigned long long)r->umax);
        break;
    case VR_CHOICE:
        (void)snprintf(buf, cap, "one of %s", r->choices);
        break;
    case VR_BOOL:
        (void)snprintf(buf, cap, "a boolean");
        break;
    }
}

#define N(nm, lo, hi) {nm, VR_NUM, (lo), (hi), 0, NULL}
#define FIN(nm) {nm, VR_FINITE, 0, 0, 0, NULL}
#define U32(nm) {nm, VR_UNS, 0, 0, 4294967295ULL, NULL}
#define CH(nm, c) {nm, VR_CHOICE, 0, 0, 0, (c)}
#define BL(nm) {nm, VR_BOOL, 0, 0, 0, NULL}

/* The core filters' field tables (filters/validation.rs::fields). */
static const vrule F_RADIUS1000[] = {N("radius", 0.1, 1000.0)};
static const vrule F_BOX[] = {N("radius", 1.0, 2000.0)};
static const vrule F_MOTION[] = {N("angle", -360.0, 360.0), N("distance", 1.0, 2000.0)};
static const vrule F_RADIAL[] = {N("amount", 1.0, 100.0), CH("method", "spin|zoom"),
                                 CH("quality", "draft|good|best"), N("centerX", 0.0, 1.0),
                                 N("centerY", 0.0, 1.0)};
static const vrule F_SURFACE[] = {N("radius", 1.0, 100.0), N("threshold", 2.0, 255.0)};
static const vrule F_UNSHARP[] = {N("amount", 1.0, 500.0), N("radius", 0.1, 1000.0),
                                  N("threshold", 0.0, 255.0)};
static const vrule F_SMART_SHARPEN[] = {N("amount", 1.0, 500.0), N("radius", 0.1, 64.0),
                                        N("reduceNoise", 0.0, 100.0)};
static const vrule F_ADD_NOISE[] = {N("amount", 0.1, 400.0), CH("distribution", "uniform|gaussian"),
                                    BL("monochromatic"), U32("seed")};
static const vrule F_MEDIAN[] = {N("radius", 1.0, 500.0)};
static const vrule F_DUST[] = {N("radius", 1.0, 500.0), N("threshold", 0.0, 255.0)};
static const vrule F_MOSAIC[] = {N("cellSize", 2.0, 200.0)};
static const vrule F_EMBOSS[] = {N("angle", -180.0, 180.0), N("height", 1.0, 100.0),
                                 N("amount", 1.0, 500.0)};
static const vrule F_TWIRL[] = {N("angle", -999.0, 999.0)};
static const vrule F_PINCH[] = {N("amount", -100.0, 100.0)};
static const vrule F_SPHERIZE[] = {N("amount", -100.0, 100.0),
                                   CH("mode", "normal|horizontalOnly|verticalOnly")};
static const vrule F_WAVE[] = {N("generators", 1.0, 999.0), N("wavelengthMin", 1.0, 998.0),
                               N("wavelengthMax", 2.0, 999.0), N("amplitudeMin", 1.0, 998.0),
                               N("amplitudeMax", 1.0, 999.0), CH("type", "sine|triangle|square"),
                               CH("undefinedAreas", "wrap|repeat"), U32("seed")};
static const vrule F_RIPPLE[] = {N("amount", -999.0, 999.0), CH("size", "small|medium|large")};
static const vrule F_POLAR[] = {CH("mode", "rectangularToPolar|polarToRectangular")};
static const vrule F_MINMAX[] = {N("radius", 0.2, 500.0), CH("preserve", "squareness|roundness")};
static const vrule F_OFFSET[] = {FIN("horizontal"), FIN("vertical"),
                                 CH("undefinedAreas", "wrap|repeat|transparent")};
static const vrule F_EMPTY[1] = {{NULL, VR_BOOL, 0, 0, 0, NULL}};

#undef N
#undef FIN
#undef U32
#undef CH
#undef BL

/* fields(id): the rule table and its length, or false when id has none (non-core). */
static bool vr_fields(const char *id, const vrule **out, size_t *n)
{
#define SET(arr) \
    do {                                                 \
        *out = (arr);                                    \
        *n = sizeof(arr) / sizeof((arr)[0]);             \
        return true;                                     \
    } while (0)
    if (strcmp(id, "filter.blur.gaussianBlur") == 0 || strcmp(id, "filter.other.highPass") == 0) {
        SET(F_RADIUS1000);
    }
    if (strcmp(id, "filter.blur.boxBlur") == 0) {
        SET(F_BOX);
    }
    if (strcmp(id, "filter.blur.motionBlur") == 0) {
        SET(F_MOTION);
    }
    if (strcmp(id, "filter.blur.radialBlur") == 0) {
        SET(F_RADIAL);
    }
    if (strcmp(id, "filter.blur.surfaceBlur") == 0) {
        SET(F_SURFACE);
    }
    if (strcmp(id, "filter.sharpen.unsharpMask") == 0) {
        SET(F_UNSHARP);
    }
    if (strcmp(id, "filter.sharpen.smartSharpen") == 0) {
        SET(F_SMART_SHARPEN);
    }
    if (strcmp(id, "filter.noise.addNoise") == 0) {
        SET(F_ADD_NOISE);
    }
    if (strcmp(id, "filter.noise.median") == 0) {
        SET(F_MEDIAN);
    }
    if (strcmp(id, "filter.noise.dustAndScratches") == 0) {
        SET(F_DUST);
    }
    if (strcmp(id, "filter.pixelate.mosaic") == 0) {
        SET(F_MOSAIC);
    }
    if (strcmp(id, "filter.stylize.emboss") == 0) {
        SET(F_EMBOSS);
    }
    if (strcmp(id, "filter.distort.twirl") == 0) {
        SET(F_TWIRL);
    }
    if (strcmp(id, "filter.distort.pinch") == 0) {
        SET(F_PINCH);
    }
    if (strcmp(id, "filter.distort.spherize") == 0) {
        SET(F_SPHERIZE);
    }
    if (strcmp(id, "filter.distort.wave") == 0) {
        SET(F_WAVE);
    }
    if (strcmp(id, "filter.distort.ripple") == 0) {
        SET(F_RIPPLE);
    }
    if (strcmp(id, "filter.distort.polarCoordinates") == 0) {
        SET(F_POLAR);
    }
    if (strcmp(id, "filter.other.minimum") == 0 || strcmp(id, "filter.other.maximum") == 0) {
        SET(F_MINMAX);
    }
    if (strcmp(id, "filter.other.offset") == 0) {
        SET(F_OFFSET);
    }
    if (strcmp(id, "filter.blur.blur") == 0 || strcmp(id, "filter.blur.blurMore") == 0 ||
        strcmp(id, "filter.sharpen.sharpen") == 0 || strcmp(id, "filter.sharpen.sharpenMore") == 0 ||
        strcmp(id, "filter.sharpen.sharpenEdges") == 0 || strcmp(id, "filter.noise.despeckle") == 0 ||
        strcmp(id, "filter.stylize.findEdges") == 0 || strcmp(id, "filter.stylize.solarize") == 0) {
        *out = F_EMPTY;
        *n = 0;
        return true;
    }
#undef SET
    return false;
}

/* The shared-field "target" value: "pixels"/"mask"/"quickMask" or {"channel": index}. */
static bool vr_target_ok(yyjson_val *v)
{
    if (yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        return s != NULL && (strcmp(s, "pixels") == 0 || strcmp(s, "mask") == 0 ||
                             strcmp(s, "quickMask") == 0);
    }
    if (yyjson_is_obj(v) && yyjson_obj_size(v) == 1) {
        yyjson_val *ch = pc_json_get(v, "channel");
        return ch != NULL && yyjson_is_uint(ch);
    }
    return false;
}

static pc_status filters_validate_params(const char *id, yyjson_val *params, pc_error *err)
{
    /* Filter Gallery ids: an unknown choice name is a bad-params error (gallery_cmds.c). */
    if (strcmp(id, "filter.filterGallery") == 0 || strncmp(id, "filter.gallery.", 15) == 0) {
        return pc_engine_gallery_validate(id, params, err);
    }
    const vrule *fields = NULL;
    size_t nf = 0;
    if (!vr_fields(id, &fields, &nf)) {
        return PC_OK; /* non-core id: nothing to check here */
    }
    if (params == NULL || yyjson_is_null(params)) {
        return PC_OK;
    }
    if (!yyjson_is_obj(params)) {
        return pc_engine_err_bad_params(err, id, "params must be a JSON object");
    }
    yyjson_obj_iter it;
    yyjson_obj_iter_init(params, &it);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&it)) != NULL) {
        const char *k = yyjson_get_str(key);
        size_t klen = yyjson_get_len(key);
        yyjson_val *v = yyjson_obj_iter_get_val(key);
        const vrule *rule = NULL;
        for (size_t i = 0; i < nf; i++) {
            if (strlen(fields[i].name) == klen && memcmp(fields[i].name, k, klen) == 0) {
                rule = &fields[i];
                break;
            }
        }
        bool valid;
        char expected[96];
        if (rule != NULL) {
            valid = vr_accepts(rule, v);
            vr_expected(rule, expected, sizeof expected);
        } else if (klen == 5 && memcmp(k, "layer", 5) == 0) {
            valid = yyjson_is_uint(v);
            (void)snprintf(expected, sizeof expected, "an unsigned layer id");
        } else if (klen == 8 && memcmp(k, "coalesce", 8) == 0) {
            valid = yyjson_is_str(v);
            (void)snprintf(expected, sizeof expected, "a string");
        } else if (klen == 6 && memcmp(k, "target", 6) == 0) {
            valid = vr_target_ok(v);
            (void)snprintf(expected, sizeof expected, "pixels, mask, quickMask or {\"channel\":index}");
        } else {
            return pc_engine_err_bad_params(err, id, "unknown parameter `%.*s`", (int)klen, k);
        }
        if (!valid) {
            return pc_engine_err_bad_params(err, id, "`%.*s` must be %s", (int)klen, k, expected);
        }
    }
    return PC_OK;
}
