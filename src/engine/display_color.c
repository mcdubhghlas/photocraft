#include "display_color.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "base/mem.h"
#include "cms/builtin.h"
#include "cms/gamut.h"
#include "engine_internal.h"

static const pc_proof_view *proof(const pc_engine_session *s, pc_doc_id id)
{
    for (size_t i = 0; i < s->color.nproofs; i++) {
        if (s->color.proofs[i].doc == id) {
            return &s->color.proofs[i].view;
        }
    }
    return NULL;
}
static pc_hdr_preview hdr(const pc_engine_session *s, const pc_document *doc)
{
    if (doc->depth == PC_SAMPLE_TYPE_F32) {
        for (size_t i = 0; i < s->color.nhdr; i++) {
            if (s->color.hdr[i].doc == doc->id) {
                return s->color.hdr[i].view;
            }
        }
    }
    return (pc_hdr_preview){.gamma = 1};
}
static uint64_t hash_bytes(uint64_t key, const void *data, size_t size)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < size; i++) {
        key = (key ^ p[i]) * UINT64_C(1099511628211);
    }
    return key;
}
static uint64_t hash_string(uint64_t key, const char *s)
{
    return s ? hash_bytes(key, s, strlen(s)) : key;
}
uint64_t pc_engine_display_key(const pc_engine_session *s)
{
    const pc_engine_doc_state *st = s ? pc_engine_session_active(s) : NULL;
    if (!st) {
        return 0;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    uint64_t key = hash_bytes(UINT64_C(14695981039346656037), &doc->id, sizeof doc->id);
    key = hash_bytes(key, &st->revision, sizeof st->revision);
    key = hash_string(key, s->color.settings.monitor_profile);
    const pc_proof_view *pv = proof(s, doc->id);
    if (pv) {
        key = hash_string(key, pv->setup.name);
        key = hash_bytes(key, &pv->setup.profile.hash, sizeof pv->setup.profile.hash);
        key = hash_bytes(key, &pv->setup.kind, sizeof pv->setup.kind);
        key = hash_bytes(key, &pv->setup.intent, sizeof pv->setup.intent);
        bool flags[] = {pv->enabled, pv->gamut_warning, pv->setup.bpc, pv->setup.simulate_paper};
        key = hash_bytes(key, flags, sizeof flags);
        key = hash_bytes(key, &pv->gamut_threshold, sizeof pv->gamut_threshold);
    }
    pc_hdr_preview h = hdr(s, doc);
    key = hash_bytes(key, &h.highlight_compression, sizeof h.highlight_compression);
    key = hash_bytes(key, &h.exposure, sizeof h.exposure);
    key = hash_bytes(key, &h.gamma, sizeof h.gamma);
    yyjson_mut_val *prefs =
        yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->prefs.values), "transparencyAndGamut");
    key = hash_string(key, yyjson_mut_get_str(yyjson_mut_obj_get(prefs, "gamutWarningColor")));
    double opacity = yyjson_mut_get_num(yyjson_mut_obj_get(prefs, "gamutWarningOpacity"));
    return hash_bytes(key, &opacity, sizeof opacity);
}
void pc_engine_display_fini(pc_engine_display *d)
{
    if (d) {
        pc_lut3d_fini(&d->lut);
        *d = (pc_engine_display){0};
    }
}
static float decode(float v)
{
    return v <= .04045f ? v / 12.92f : powf((v + .055f) / 1.055f, 2.4f);
}
static float encode(float v)
{
    v = fminf(1, fmaxf(0, v));
    return v <= .0031308f ? v * 12.92f : 1.055f * powf(v, 1.f / 2.4f) - .055f;
}
static void mul(const float m[3][3], const float v[3], float out[3])
{
    for (size_t i = 0; i < 3; i++) {
        out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    }
}
void pc_engine_display_dichromat(const float rgb[3], bool protan, float out[3])
{
    static const float a[3][3] = {
        {17.8824f, 43.5161f, 4.11935f}, {3.45565f, 27.1554f, 3.86714f}, {.0299566f, .184309f, 1.46709f}};
    static const float b[3][3] = {{.08094445f, -.13050441f, .11672107f},
                                  {-.010248533f, .054019327f, -.11361471f},
                                  {-.000365297f, -.0041216147f, .6935114f}};
    float v[3], lms[3];
    for (size_t i = 0; i < 3; i++) {
        v[i] = .992052f * rgb[i] + .003974f;
    }
    mul(a, v, lms);
    if (protan) {
        lms[0] = 2.02344f * lms[1] - 2.52581f * lms[2];
    } else {
        lms[1] = .494207f * lms[0] + 1.24827f * lms[2];
    }
    mul(b, lms, out);
    for (size_t i = 0; i < 3; i++) {
        out[i] = fminf(1, fmaxf(0, out[i]));
    }
}
static bool linear(const pc_profile *p)
{
    if (p->space != PC_CS_RGB || !p->has_trc) {
        return false;
    }
    return pc_cms_curve_is_identity(&p->trc[0]) && pc_cms_curve_is_identity(&p->trc[1]) &&
           pc_cms_curve_is_identity(&p->trc[2]);
}
pc_status pc_engine_display_build(const pc_engine_session *s, size_t size, pc_engine_display *out,
                                  pc_error *err)
{
    const pc_engine_doc_state *st = s ? pc_engine_session_active(s) : NULL;
    if (!st || !out) {
        return pc_fail(err, PC_ERR_INVALID, "no display document/output");
    }
    if (size < 2 || size > 65) {
        return pc_fail(err, PC_ERR_LIMIT, "display LUT size must be 2..65");
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_proof_view *pv = proof(s, doc->id);
    int kind = pv && pv->enabled ? pv->setup.kind : 0;
    pc_profile src = {0}, mon = {0};
    pc_transform *base = NULL, *second = NULL;
    pc_gamut_check *gamut = NULL;
    pc_engine_display next = {
        .hdr = hdr(s, doc), .warning = {128.f / 255, 128.f / 255, 128.f / 255}, .warning_opacity = 1};
    yyjson_mut_val *prefs =
        yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->prefs.values), "transparencyAndGamut");
    const char *warning_pref = yyjson_mut_get_str(yyjson_mut_obj_get(prefs, "gamutWarningColor"));
    if (warning_pref && strlen(warning_pref) == 7 && warning_pref[0] == '#') {
        bool valid = true;
        unsigned rgb[3] = {0};
        for (size_t i = 0; i < 6; i++) {
            char ch = warning_pref[i + 1];
            int n = ch >= '0' && ch <= '9'   ? ch - '0'
                    : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                    : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                             : -1;
            if (n < 0) {
                valid = false;
                break;
            }
            rgb[i / 2] = rgb[i / 2] * 16 + (unsigned)n;
        }
        if (valid) {
            for (size_t i = 0; i < 3; i++) {
                next.warning[i] = (float)rgb[i] / 255.f;
            }
        }
    }
    yyjson_mut_val *opacity = yyjson_mut_obj_get(prefs, "gamutWarningOpacity");
    if (yyjson_mut_is_num(opacity)) {
        next.warning_opacity = fminf(1, fmaxf(.01f, (float)yyjson_mut_get_num(opacity) / 100.f));
    }
    pc_status rc = pc_engine_composite_profile(doc, &src, err);
    if (rc != PC_OK) {
        goto done;
    }
    next.encode_linear = linear(&src);
    if (next.encode_linear) {
        for (size_t i = 0; i < 3; i++) {
            pc_cms_curve_fini(&src.trc[i]);
            src.trc[i] = pc_cms_srgb_trc();
        }
        free(src.bytes);
        src.bytes = NULL;
        src.nbytes = 0;
        src.hash = 0;
    }
    rc = pc_engine_display_monitor(s, &mon, err);
    if (rc != PC_OK) {
        goto done;
    }
    const pc_profile *srgb = pc_builtin_profile(PC_BUILTIN_SRGB);
    if (kind >= 1 && kind <= 5) {
        if (pc_profile_channels(&pv->setup.profile) != 4) {
            rc = pc_fail(err, PC_ERR_INVALID, "plate previews need a CMYK proof profile");
            goto done;
        }
        rc = pc_transform_new(&src, &pv->setup.profile, pv->setup.intent, pv->setup.bpc, &base, err);
        if (rc == PC_OK) {
            rc = pc_transform_new(&pv->setup.profile, &mon, PC_INTENT_RELATIVE, true, &second, err);
        }
    } else if (kind == 6) {
        rc = pc_transform_new(srgb, &mon, PC_INTENT_RELATIVE, true, &base, err);
    } else if (kind == 7 || kind == 8) {
        rc = pc_transform_new(&src, srgb, PC_INTENT_RELATIVE, true, &base, err);
        if (rc == PC_OK) {
            rc = pc_transform_new(srgb, &mon, PC_INTENT_RELATIVE, true, &second, err);
        }
    } else if (pv && pv->enabled) {
        rc = pc_transform_proof(&src, &pv->setup.profile, &mon, pv->setup.intent, pv->setup.bpc,
                                pv->setup.simulate_paper, &base, err);
    } else {
        rc = pc_transform_new(&src, &mon, PC_INTENT_RELATIVE, true, &base, err);
    }
    if (rc != PC_OK) {
        goto done;
    }
    if (pv && pv->gamut_warning) {
        rc = pc_gamut_check_new(&src, &pv->setup.profile, pv->gamut_threshold, &gamut, err);
        if (rc != PC_OK) {
            goto done;
        }
        next.gamut_warning = true;
    }
    next.lut.size = size;
    next.lut.data = pc_malloc_array(size * size * size * 4, sizeof(float));
    if (!next.lut.data) {
        rc = pc_fail(err, PC_ERR_NOMEM, "display LUT");
        goto done;
    }
    float scale = (float)(size - 1);
    for (size_t z = 0; z < size; z++) {
        for (size_t y = 0; y < size; y++) {
            for (size_t x = 0; x < size; x++) {
                float v[3] = {(float)x / scale, (float)y / scale, (float)z / scale}, shown[16] = {0};
                bool warning = gamut && pc_gamut_check_out_of_gamut(gamut, v);
                if (kind >= 1 && kind <= 5) {
                    float ink[16] = {0}, plate[4] = {0};
                    pc_transform_eval(base, v, ink);
                    if (kind == 5) {
                        plate[0] = ink[0];
                        plate[1] = ink[1];
                        plate[2] = ink[2];
                    } else {
                        plate[3] = ink[(size_t)kind - 1];
                    }
                    pc_transform_eval(second, plate, shown);
                } else if (kind == 6) {
                    for (size_t i = 0; i < 3; i++) {
                        v[i] = encode(powf(fmaxf(0, v[i]), 1.8f));
                    }
                    pc_transform_eval(base, v, shown);
                } else if (kind == 7 || kind == 8) {
                    float rgb[16] = {0}, sim[3];
                    pc_transform_eval(base, v, rgb);
                    for (size_t i = 0; i < 3; i++) {
                        rgb[i] = decode(rgb[i]);
                    }
                    pc_engine_display_dichromat(rgb, kind == 7, sim);
                    for (size_t i = 0; i < 3; i++) {
                        sim[i] = encode(sim[i]);
                    }
                    pc_transform_eval(second, sim, shown);
                } else {
                    pc_transform_eval(base, v, shown);
                }
                float *p = next.lut.data + 4 * (x + y * size + z * size * size);
                for (size_t i = 0; i < 3; i++) {
                    p[i] = isfinite(shown[i]) ? shown[i] : 0;
                }
                p[3] = warning ? 1.f : 0.f;
            }
        }
    }
    pc_engine_display_fini(out);
    *out = next;
    next = (pc_engine_display){0};
done:
    pc_engine_display_fini(&next);
    pc_profile_fini(&src);
    pc_profile_fini(&mon);
    pc_transform_release(base);
    pc_transform_release(second);
    pc_gamut_check_free(gamut);
    return rc;
}
void pc_engine_display_apply(const pc_engine_display *d, const float rgb[3], float out[3])
{
    float v[3];
    bool active = !d->hdr.highlight_compression && (d->hdr.exposure != 0 || d->hdr.gamma != 1);
    for (size_t i = 0; i < 3; i++) {
        float value = isfinite(rgb[i]) ? rgb[i] : 0;
        if (d->encode_linear) {
            float linear_value = fminf(65504, fmaxf(0, value));
            value = linear_value <= .0031308f ? linear_value * 12.92f
                                              : 1.055f * powf(linear_value, 1.f / 2.4f) - .055f;
        }
        if (active) {
            value =
                encode(fminf(1, powf(fmaxf(0, decode(value) * exp2f(d->hdr.exposure)), 1.f / d->hdr.gamma)));
        }
        v[i] = value;
    }
    pc_lut3d_sample(&d->lut, v, out);
    if (d->gamut_warning && d->lut.data && d->lut.size >= 2) {
        size_t lo[3];
        float part[3], coverage = 0;
        size_t n = d->lut.size;
        for (size_t i = 0; i < 3; i++) {
            float at = fminf(1, fmaxf(0, v[i])) * (float)(n - 1);
            lo[i] = (size_t)at;
            if (lo[i] >= n - 1) {
                lo[i] = n - 2;
            }
            part[i] = at - (float)lo[i];
        }
        for (unsigned corner = 0; corner < 8; corner++) {
            size_t pos[3];
            float weight = 1;
            for (unsigned axis = 0; axis < 3; axis++) {
                bool upper = (corner & (1u << axis)) != 0;
                pos[axis] = lo[axis] + (upper ? 1 : 0);
                weight *= upper ? part[axis] : 1 - part[axis];
            }
            coverage += weight * d->lut.data[4 * (pos[0] + pos[1] * n + pos[2] * n * n) + 3];
        }
        if (coverage > .5f) {
            for (size_t i = 0; i < 3; i++) {
                out[i] = out[i] * (1 - d->warning_opacity) + d->warning[i] * d->warning_opacity;
            }
        }
    }
}
