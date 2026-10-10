/* serde-compatible JSON for Liquify strokes and Puppet Warp pins (the derives on LiquifyStroke,
 * PuppetPin and PuppetWarp in crates/algo/src/{liquify,puppet}.rs, read as the engine reads them:
 * serde_json::from_value). Semantics reproduced:
 *   - structs from maps: keys visited in byte order with the last duplicate winning (a Value is a
 *     BTreeMap), unknown keys ignored, missing required fields reported afterwards in declaration
 *     order ("missing field `src`"); from arrays (serde's visit_seq): fields in declaration order,
 *     missing trailing ones defaulted or "invalid length N, expected struct X with M elements",
 *     extra ones "invalid length N, expected fewer elements in array";
 *   - f64 from any number; i32 with serde's range and type messages; Option<f64> null = None;
 *     [f64; 2] exactly two numbers; enums from a variant name or a one-key map with a null value.
 * Writing emits fields in declaration order (sort with PC_JSON_SORT_KEYS for a Value's order). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/liquify.h"
#include "algo/puppet.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"

/* ---- scalars ---- */

/* serde_json reads "-0" as the float -0.0 (yyjson as the integer 0). */
static bool neg_zero_int(yyjson_val *v)
{
    return yyjson_is_sint(v) && yyjson_get_sint(v) == 0;
}

static pc_status read_f64(yyjson_val *v, double *out, pc_error *err)
{
    if (neg_zero_int(v)) {
        *out = -0.0;
    } else if (yyjson_is_uint(v)) {
        *out = (double)yyjson_get_uint(v);
    } else if (yyjson_is_sint(v)) {
        *out = (double)yyjson_get_sint(v);
    } else if (yyjson_is_real(v)) {
        *out = yyjson_get_real(v);
    } else {
        return pc_algo_jfail_type(v, "f64", err);
    }
    return PC_OK;
}

static pc_status read_i32(yyjson_val *v, int32_t *out, pc_error *err)
{
    if (neg_zero_int(v) || !yyjson_is_int(v)) {
        return pc_algo_jfail_type(v, "i32", err);
    }
    if (yyjson_is_uint(v)) {
        uint64_t u = yyjson_get_uint(v);
        if (u > INT32_MAX) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%llu`, expected i32",
                           (unsigned long long)u);
        }
        *out = (int32_t)u;
        return PC_OK;
    }
    {
        int64_t i = yyjson_get_sint(v);
        if (i < INT32_MIN || i > INT32_MAX) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%lld`, expected i32", (long long)i);
        }
        *out = (int32_t)i;
    }
    return PC_OK;
}

static pc_status read_opt_f64(yyjson_val *v, bool *set, double *out, pc_error *err)
{
    double d = 0.0;
    pc_status st;
    if (yyjson_is_null(v)) {
        *set = false;
        return PC_OK;
    }
    st = read_f64(v, &d, err);
    if (st == PC_OK) {
        *set = true;
        *out = d;
    }
    return st;
}

/* [f64; 2] */
static pc_status read_pair(yyjson_val *v, double out[2], pc_error *err)
{
    size_t n, i;
    double tmp[2] = {0.0, 0.0};
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, "an array of length 2", err);
    }
    n = yyjson_arr_size(v);
    for (i = 0; i < 2; i++) {
        pc_status st;
        if (i >= n) {
            return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected an array of length 2", i);
        }
        st = read_f64(yyjson_arr_get(v, i), &tmp[i], err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (n > 2) {
        return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected fewer elements in array", n);
    }
    out[0] = tmp[0];
    out[1] = tmp[1];
    return PC_OK;
}

static pc_status read_enum(yyjson_val *v, const pc_algo_enum_desc *d, int *out, pc_error *err)
{
    const char *s;
    size_t len;
    yyjson_val *payload = NULL;
    if (yyjson_is_str(v)) {
        s = yyjson_get_str(v);
        len = yyjson_get_len(v);
    } else if (yyjson_is_obj(v)) {
        yyjson_val *key;
        if (yyjson_obj_size(v) != 1) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: map, expected map with a single key");
        }
        {
            yyjson_obj_iter it = yyjson_obj_iter_with(v);
            key = yyjson_obj_iter_next(&it);
            payload = yyjson_obj_iter_get_val(key);
        }
        s = yyjson_get_str(key);
        len = yyjson_get_len(key);
    } else {
        return pc_algo_jfail_type(v, "string or map", err);
    }
    if (!pc_algo_enum_from_name(d, s, len, out)) {
        return pc_algo_jfail_variant(s, len, d->names, d->n, err);
    }
    if (payload != NULL && !yyjson_is_null(payload)) {
        return pc_algo_jfail_type(payload, "unit", err);
    }
    return PC_OK;
}

/* ---- generic struct reader ---- */

typedef pc_status (*field_read)(yyjson_val *v, void *base, pc_error *err);

typedef struct jfield {
    const char *name;
    bool required;
    field_read read;
} jfield;

typedef struct kv {
    const char *key;
    size_t len;
    size_t pos;
    yyjson_val *val;
} kv;

static int cmp_kv(const void *pa, const void *pb)
{
    const kv *a = pa, *b = pb;
    size_t n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->key, b->key, n);
    if (c != 0) {
        return c;
    }
    if (a->len != b->len) {
        return a->len < b->len ? -1 : 1;
    }
    return a->pos < b->pos ? -1 : (a->pos > b->pos ? 1 : 0);
}

/* Reads v into base (prefilled with the defaults). `name` is the Rust struct name. */
static pc_status read_struct(yyjson_val *v, const jfield *f, size_t nf, const char *name, void *base,
                             pc_error *err)
{
    size_t i;
    if (yyjson_is_arr(v)) {
        size_t n = yyjson_arr_size(v);
        for (i = 0; i < nf; i++) {
            pc_status st;
            if (i >= n) {
                if (f[i].required) {
                    return pc_fail(err, PC_ERR_INVALID,
                                   "invalid length %zu, expected struct %s with %zu elements", i, name, nf);
                }
                continue;
            }
            st = f[i].read(yyjson_arr_get(v, i), base, err);
            if (st != PC_OK) {
                return st;
            }
        }
        if (n > nf) {
            return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected fewer elements in array", n);
        }
        return PC_OK;
    }
    if (yyjson_is_obj(v)) {
        size_t n = yyjson_obj_size(v), k = 0;
        bool seen[8] = {false};
        kv *items = pc_malloc_array(n > 0 ? n : 1, sizeof *items);
        yyjson_obj_iter it = yyjson_obj_iter_with(v);
        yyjson_val *key;
        if (items == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        while ((key = yyjson_obj_iter_next(&it)) != NULL) {
            items[k].key = yyjson_get_str(key);
            items[k].len = yyjson_get_len(key);
            items[k].pos = k;
            items[k].val = yyjson_obj_iter_get_val(key);
            k++;
        }
        qsort(items, k, sizeof *items, cmp_kv);
        for (i = 0; i < k; i++) {
            size_t j;
            if (i + 1 < k && items[i + 1].len == items[i].len &&
                memcmp(items[i + 1].key, items[i].key, items[i].len) == 0) {
                continue; /* a later duplicate wins */
            }
            for (j = 0; j < nf; j++) {
                if (strlen(f[j].name) == items[i].len && memcmp(f[j].name, items[i].key, items[i].len) == 0) {
                    pc_status st = f[j].read(items[i].val, base, err);
                    if (st != PC_OK) {
                        free(items);
                        return st;
                    }
                    seen[j] = true;
                    break;
                }
            }
        }
        free(items);
        for (i = 0; i < nf; i++) {
            if (f[i].required && !seen[i]) {
                return pc_fail(err, PC_ERR_INVALID, "missing field `%s`", f[i].name);
            }
        }
        return PC_OK;
    }
    {
        char exp[64];
        (void)snprintf(exp, sizeof exp, "struct %s", name);
        return pc_algo_jfail_type(v, exp, err);
    }
}

/* ---- LiquifyStroke ---- */

static pc_status st_tool(yyjson_val *v, void *b, pc_error *err)
{
    int t = 0;
    pc_status st = read_enum(v, &pc_algo_liquify_tool_desc, &t, err);
    if (st == PC_OK) {
        ((pc_algo_liquify_stroke *)b)->tool = (pc_algo_liquify_tool)t;
    }
    return st;
}
static pc_status st_size(yyjson_val *v, void *b, pc_error *err)
{
    return read_f64(v, &((pc_algo_liquify_stroke *)b)->size, err);
}
static pc_status st_density(yyjson_val *v, void *b, pc_error *err)
{
    return read_f64(v, &((pc_algo_liquify_stroke *)b)->density, err);
}
static pc_status st_pressure(yyjson_val *v, void *b, pc_error *err)
{
    return read_f64(v, &((pc_algo_liquify_stroke *)b)->pressure, err);
}
static pc_status st_rate(yyjson_val *v, void *b, pc_error *err)
{
    return read_f64(v, &((pc_algo_liquify_stroke *)b)->rate, err);
}
static pc_status st_amount(yyjson_val *v, void *b, pc_error *err)
{
    pc_algo_liquify_stroke *s = b;
    return read_opt_f64(v, &s->has_amount, &s->amount, err);
}
static pc_status st_points(yyjson_val *v, void *b, pc_error *err)
{
    pc_algo_liquify_stroke *s = b, tmp = *s;
    size_t i, n;
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, "a sequence", err);
    }
    tmp.coords = NULL;
    tmp.off = NULL;
    tmp.npoints = 0;
    n = yyjson_arr_size(v);
    for (i = 0; i < n; i++) {
        yyjson_val *p = yyjson_arr_get(v, i);
        size_t m, k;
        double *vals;
        pc_status st;
        if (!yyjson_is_arr(p)) {
            pc_algo_liquify_stroke_fini(&tmp);
            return pc_algo_jfail_type(p, "a sequence", err);
        }
        m = yyjson_arr_size(p);
        vals = pc_malloc_array(m > 0 ? m : 1, sizeof *vals);
        if (vals == NULL) {
            pc_algo_liquify_stroke_fini(&tmp);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (k = 0; k < m; k++) {
            st = read_f64(yyjson_arr_get(p, k), &vals[k], err);
            if (st != PC_OK) {
                free(vals);
                pc_algo_liquify_stroke_fini(&tmp);
                return st;
            }
        }
        st = pc_algo_liquify_stroke_push(&tmp, vals, m, err);
        free(vals);
        if (st != PC_OK) {
            pc_algo_liquify_stroke_fini(&tmp);
            return st;
        }
    }
    pc_algo_liquify_stroke_fini(s);
    s->coords = tmp.coords;
    s->off = tmp.off;
    s->npoints = tmp.npoints;
    return PC_OK;
}

static const jfield STROKE_FIELDS[] = {{"tool", false, st_tool},       {"size", false, st_size},
                                       {"density", false, st_density}, {"pressure", false, st_pressure},
                                       {"rate", false, st_rate},       {"points", false, st_points},
                                       {"amount", false, st_amount}};

pc_status pc_algo_liquify_stroke_from_json(yyjson_val *v, pc_algo_liquify_stroke *out, pc_error *err)
{
    /* Field defaults (d_size etc.), not LiquifyStroke::new's. */
    pc_algo_liquify_stroke s = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_FORWARD_WARP, 100.0);
    pc_status st = read_struct(v, STROKE_FIELDS, 7, "LiquifyStroke", &s, err);
    if (st != PC_OK) {
        pc_algo_liquify_stroke_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}

void pc_algo_liquify_strokes_free(pc_algo_liquify_stroke *s, size_t n)
{
    size_t i;
    if (s == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        pc_algo_liquify_stroke_fini(&s[i]);
    }
    free(s);
}

pc_status pc_algo_liquify_strokes_from_json(yyjson_val *v, pc_algo_liquify_stroke **out, size_t *n,
                                            pc_error *err)
{
    size_t i, m;
    pc_algo_liquify_stroke *arr;
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, "a sequence", err);
    }
    m = yyjson_arr_size(v);
    arr = pc_calloc_array(m > 0 ? m : 1, sizeof *arr);
    if (arr == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < m; i++) {
        pc_status st = pc_algo_liquify_stroke_from_json(yyjson_arr_get(v, i), &arr[i], err);
        if (st != PC_OK) {
            pc_algo_liquify_strokes_free(arr, i);
            return st;
        }
    }
    *out = arr;
    *n = m;
    return PC_OK;
}

static yyjson_mut_val *num(yyjson_mut_doc *d, double x)
{
    return isfinite(x) ? yyjson_mut_real(d, x) : yyjson_mut_null(d);
}

yyjson_mut_val *pc_algo_liquify_stroke_to_json(yyjson_mut_doc *d, const pc_algo_liquify_stroke *s)
{
    yyjson_mut_val *o = yyjson_mut_obj(d), *pts = yyjson_mut_arr(d);
    size_t i, k;
    bool ok = o != NULL && pts != NULL;
    const char *tool = pc_algo_enum_name(&pc_algo_liquify_tool_desc, (int)s->tool);
    ok = ok && yyjson_mut_obj_add_str(d, o, "tool", tool);
    ok = ok && yyjson_mut_obj_add_val(d, o, "size", num(d, s->size));
    ok = ok && yyjson_mut_obj_add_val(d, o, "density", num(d, s->density));
    ok = ok && yyjson_mut_obj_add_val(d, o, "pressure", num(d, s->pressure));
    ok = ok && yyjson_mut_obj_add_val(d, o, "rate", num(d, s->rate));
    for (i = 0; ok && i < s->npoints; i++) {
        yyjson_mut_val *p = yyjson_mut_arr(d);
        ok = p != NULL;
        for (k = s->off[i]; ok && k < s->off[i + 1]; k++) {
            ok = yyjson_mut_arr_append(p, num(d, s->coords[k]));
        }
        ok = ok && yyjson_mut_arr_append(pts, p);
    }
    ok = ok && yyjson_mut_obj_add_val(d, o, "points", pts);
    ok = ok && yyjson_mut_obj_add_val(d, o, "amount", s->has_amount ? num(d, s->amount) : yyjson_mut_null(d));
    return ok ? o : NULL;
}

/* ---- PuppetPin / PuppetWarp ---- */

static pc_status pin_src(yyjson_val *v, void *b, pc_error *err)
{
    return read_pair(v, ((pc_algo_puppet_pin *)b)->src, err);
}
static pc_status pin_dst(yyjson_val *v, void *b, pc_error *err)
{
    return read_pair(v, ((pc_algo_puppet_pin *)b)->dst, err);
}
static pc_status pin_rotate(yyjson_val *v, void *b, pc_error *err)
{
    pc_algo_puppet_pin *p = b;
    return read_opt_f64(v, &p->rotate.set, &p->rotate.deg, err);
}
static pc_status pin_depth(yyjson_val *v, void *b, pc_error *err)
{
    return read_i32(v, &((pc_algo_puppet_pin *)b)->depth, err);
}

static const jfield PIN_FIELDS[] = {{"src", true, pin_src},
                                    {"dst", true, pin_dst},
                                    {"rotate", false, pin_rotate},
                                    {"depth", false, pin_depth}};

pc_status pc_algo_puppet_pins_from_json(yyjson_val *v, pc_algo_puppet_pin **pins, size_t *n, pc_error *err)
{
    size_t i, m;
    pc_algo_puppet_pin *arr;
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, "a sequence", err);
    }
    m = yyjson_arr_size(v);
    arr = pc_calloc_array(m > 0 ? m : 1, sizeof *arr);
    if (arr == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < m; i++) {
        pc_status st = read_struct(yyjson_arr_get(v, i), PIN_FIELDS, 4, "PuppetPin", &arr[i], err);
        if (st != PC_OK) {
            free(arr);
            return st;
        }
    }
    *pins = m > 0 ? arr : NULL;
    if (m == 0) {
        free(arr);
    }
    *n = m;
    return PC_OK;
}

static pc_status warp_pins(yyjson_val *v, void *b, pc_error *err)
{
    pc_algo_puppet_warp *w = b;
    pc_algo_puppet_pin *p = NULL;
    size_t n = 0;
    pc_status st = pc_algo_puppet_pins_from_json(v, &p, &n, err);
    if (st == PC_OK) {
        free(w->pins);
        w->pins = p;
        w->npins = n;
    }
    return st;
}
static pc_status warp_mode(yyjson_val *v, void *b, pc_error *err)
{
    int m = 0;
    pc_status st = read_enum(v, &pc_algo_puppet_mode_desc, &m, err);
    if (st == PC_OK) {
        ((pc_algo_puppet_warp *)b)->mode = (pc_algo_puppet_mode)m;
    }
    return st;
}
static pc_status warp_density(yyjson_val *v, void *b, pc_error *err)
{
    int m = 0;
    pc_status st = read_enum(v, &pc_algo_puppet_density_desc, &m, err);
    if (st == PC_OK) {
        ((pc_algo_puppet_warp *)b)->density = (pc_algo_puppet_density)m;
    }
    return st;
}
static pc_status warp_expansion(yyjson_val *v, void *b, pc_error *err)
{
    return read_f64(v, &((pc_algo_puppet_warp *)b)->expansion, err);
}

static const jfield WARP_FIELDS[] = {{"pins", false, warp_pins},
                                     {"mode", false, warp_mode},
                                     {"density", false, warp_density},
                                     {"expansion", false, warp_expansion}};

pc_status pc_algo_puppet_warp_from_json(yyjson_val *v, pc_algo_puppet_warp *out, pc_error *err)
{
    pc_algo_puppet_warp w = pc_algo_puppet_warp_make();
    pc_status st = read_struct(v, WARP_FIELDS, 4, "PuppetWarp", &w, err);
    if (st != PC_OK) {
        pc_algo_puppet_warp_fini(&w);
        return st;
    }
    *out = w;
    return PC_OK;
}

static yyjson_mut_val *pair(yyjson_mut_doc *d, const double p[2])
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    if (a == NULL || !yyjson_mut_arr_append(a, num(d, p[0])) || !yyjson_mut_arr_append(a, num(d, p[1]))) {
        return NULL;
    }
    return a;
}

yyjson_mut_val *pc_algo_puppet_pin_to_json(yyjson_mut_doc *d, const pc_algo_puppet_pin *p)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    bool ok = o != NULL;
    ok = ok && yyjson_mut_obj_add_val(d, o, "src", pair(d, p->src));
    ok = ok && yyjson_mut_obj_add_val(d, o, "dst", pair(d, p->dst));
    if (ok && p->rotate.set) {
        ok = yyjson_mut_obj_add_val(d, o, "rotate", num(d, p->rotate.deg));
    }
    if (ok && p->depth != 0) {
        ok = yyjson_mut_obj_add_sint(d, o, "depth", p->depth);
    }
    return ok ? o : NULL;
}

yyjson_mut_val *pc_algo_puppet_warp_to_json(yyjson_mut_doc *d, const pc_algo_puppet_warp *w)
{
    yyjson_mut_val *o = yyjson_mut_obj(d), *pins = yyjson_mut_arr(d);
    size_t i;
    bool ok = o != NULL && pins != NULL;
    for (i = 0; ok && i < w->npins; i++) {
        ok = yyjson_mut_arr_append(pins, pc_algo_puppet_pin_to_json(d, &w->pins[i]));
    }
    ok = ok && yyjson_mut_obj_add_val(d, o, "pins", pins);
    ok = ok &&
         yyjson_mut_obj_add_str(d, o, "mode", pc_algo_enum_name(&pc_algo_puppet_mode_desc, (int)w->mode));
    ok = ok && yyjson_mut_obj_add_str(d, o, "density",
                                      pc_algo_enum_name(&pc_algo_puppet_density_desc, (int)w->density));
    ok = ok && yyjson_mut_obj_add_val(d, o, "expansion", num(d, w->expansion));
    return ok ? o : NULL;
}
