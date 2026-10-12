/* Camera raw import (port of crates/io/src/raw.rs). */
#include "io/raw_import.h"

#include <ctype.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/camera_raw.h"
#include "base/blob.h"
#include "base/buf.h"
#include "base/num.h"
#include "base/rfmt.h"
#include "base/utf8.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "codecs/orientation.h"
#include "io/flat.h"

bool pc_io_is_raw(const uint8_t *bytes, size_t len)
{
    return pc_raw_is_raw(bytes, len);
}

pc_raw_limits pc_io_raw_limits(void)
{
    pc_climits c = pc_climits_default();
    pc_raw_limits l;
    l.max_width = c.max_width;
    l.max_height = c.max_height;
    l.max_pixels = c.max_pixels;
    l.max_alloc = c.max_alloc;
    return l;
}

static const char *format_name(const uint8_t *bytes, size_t len)
{
    pc_raw_format f;
    return pc_raw_identify(bytes, len, &f) ? pc_raw_format_name(f) : "camera raw";
}

/* Rust's Display of an f32: the shortest decimal that reads back as v, printed like an f64 of
 * that decimal (those digits are also the f64's shortest, having at most 9 significant). */
static void display_f32(float v, char *out, size_t cap)
{
    char e[64];
    char d[PC_RUST_F64_TEXT_MAX];
    int prec = 0;
    if (!isfinite(v)) {
        (void)snprintf(out, cap, "%s", isnan(v) ? "NaN" : v < 0 ? "-inf" : "inf");
        return;
    }
    for (prec = 0; prec < 9; prec++) {
        (void)snprintf(e, sizeof e, "%.*e", prec, (double)v);
        if (strtof(e, NULL) == v) {
            break;
        }
    }
    {
        size_t n = pc_rust_display_f64(strtod(e, NULL), d);
        if (cap == 0) {
            return;
        }
        n = n < cap - 1 ? n : cap - 1;
        memcpy(out, d, n);
        out[n] = '\0';
    }
}

/* Rust's {:+.N} of an f32 ("NaN" carries no sign; printf would print "+nan"). */
static void signed_fixed(float v, int prec, char *out, size_t cap)
{
    if (isnan(v)) {
        (void)snprintf(out, cap, "NaN");
    } else if (isinf(v)) {
        (void)snprintf(out, cap, "%s", v < 0 ? "-inf" : "+inf");
    } else {
        (void)snprintf(out, cap, "%+.*f", prec, (double)v);
    }
}

/* The first white-space separated word of s: [*start, *start + len). */
static size_t first_word(const char *s, size_t *start)
{
    size_t n = strlen(s);
    size_t i = 0;
    size_t b;
    uint32_t cp;
    while (i < n) {
        size_t j = i;
        if (pc_utf8_next((const uint8_t *)s, n, &j, &cp) && pc_unicode_is_space(cp)) {
            i = j;
            continue;
        }
        break;
    }
    b = i;
    while (i < n) {
        size_t j = i;
        if (pc_utf8_next((const uint8_t *)s, n, &j, &cp)) {
            if (pc_unicode_is_space(cp)) {
                break;
            }
            i = j;
        } else {
            i++;
        }
    }
    *start = b;
    return i - b;
}

size_t pc_io_camera_name(const char *make, const char *model, char *out, size_t cap)
{
    int n;
    if (make != NULL && model != NULL) {
        size_t start;
        size_t wl = first_word(make, &start);
        if (wl > 0 && strlen(model) >= wl) {
            bool same = true;
            for (size_t i = 0; i < wl && same; i++) {
                same = tolower((unsigned char)make[start + i]) == tolower((unsigned char)model[i]);
            }
            if (same) {
                n = snprintf(out, cap, "%s", model);
                return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
            }
        }
    }
    if (make != NULL && model != NULL) {
        n = snprintf(out, cap, "%s %s", make, model);
    } else {
        n = snprintf(out, cap, "%s", make != NULL ? make : model != NULL ? model : "");
    }
    return n < 0 || cap == 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

/* A developed raw as a 16-bit ProPhoto document, with a note saying how it was developed.
 * Takes over dev. */
static pc_status developed_document(const char *name, const char *format, pc_raw_developed *dev,
                                    const char *summary, pc_io_import_result *out, pc_error *err)
{
    pc_cimage img;
    pc_io_import_result r;
    const pc_profile *pro = pc_builtin_profile(PC_BUILTIN_PROPHOTO_COMPAT);
    pc_buf icc = PC_BUF_INIT;
    char camera[512];
    pc_status st;
    st = pc_cimage_from_copy(dev->width, dev->height, PC_CL_RGB, PC_CS_U16, dev->rgb,
                             dev->nrgb * sizeof(uint16_t), &img, err);
    if (st != PC_OK) {
        pc_raw_developed_fini(dev);
        return pc_io_err_prefix(st, err, "codec: ");
    }
    st = pc_io_image_to_document(name, &img, &r, err);
    pc_cimage_fini(&img);
    if (st != PC_OK) {
        pc_raw_developed_fini(dev);
        return st;
    }
    st = pro != NULL ? pc_profile_to_bytes(pro, &icc, err) : pc_io_nomem(err, "building ProPhoto");
    if (st == PC_OK) {
        st = pc_io_set_blob(&r.document.icc_profile, icc.data, icc.len, err);
    }
    pc_buf_fini(&icc);
    (void)pc_io_camera_name(dev->info.make, dev->info.model, camera, sizeof camera);
    if (st == PC_OK) {
        st = pc_io_warn(&r.warnings, err, "%s%s%s %s into 16-bit %s", format,
                        camera[0] != '\0' ? " from " : "", camera, summary, PC_RAW_OUTPUT_SPACE);
    }
    if (st == PC_OK) {
        st = pc_io_warnings_extend(&r.warnings, "", dev->warnings.v, dev->warnings.n, err);
    }
    pc_raw_developed_fini(dev);
    if (st != PC_OK) {
        pc_io_import_result_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

pc_status pc_io_import_raw(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                           pc_error *err)
{
    pc_raw_develop_options o = pc_raw_develop_options_default();
    o.limits = pc_io_raw_limits();
    return pc_io_import_raw_with(name, bytes, len, &o, out, err);
}

/* A raw file's embedded JPEG preview, turned upright: its own EXIF orientation wins; otherwise
 * the raw's IFD0 orientation applies. */
static pc_status upright_preview(const uint8_t *raw, size_t raw_len, const uint8_t *jpeg, size_t jpeg_len,
                                 pc_cimage *out, pc_error *err)
{
    pc_cdecode_opts o = pc_cdecode_opts_default();
    pc_cimage img;
    uint16_t own;
    uint16_t orient;
    pc_status st;
    o.keep_orientation = true;
    st = pc_cdecode_as_with(PC_CF_JPEG, jpeg, jpeg_len, &o, &img, err);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "codec: ");
    }
    own = img.meta.exif.has ? pc_exif_orientation(img.meta.exif.data, img.meta.exif.len) : 1;
    orient = own != 1 ? own : pc_exif_orientation(raw, raw_len);
    st = pc_cimage_oriented(&img, orient, out, err);
    pc_cimage_fini(&img);
    return st == PC_OK ? PC_OK : pc_io_err_prefix(st, err, "codec: ");
}

pc_status pc_io_import_raw_with(const char *name, const uint8_t *bytes, size_t len,
                                const pc_raw_develop_options *opts, pc_io_import_result *out, pc_error *err)
{
    const char *format = format_name(bytes, len);
    pc_raw_developed dev;
    pc_raw_preview p;
    pc_error e;
    char summary[256];
    char reason[PC_ERROR_MSG_MAX];
    static const char unsupported[] = "unsupported camera raw: ";
    pc_status st = pc_raw_develop(bytes, len, opts, &dev, &e);
    if (st == PC_OK) {
        (void)snprintf(summary, sizeof summary, "%s (%s demosaic, as-shot white balance)",
                       PC_IO_DEVELOPED_NOTE, pc_raw_demosaic_id(opts->demosaic));
        return developed_document(name, format, &dev, summary, out, err);
    }
    if (st != PC_ERR_UNSUPPORTED || !pc_raw_embedded_preview(bytes, len, &p)) {
        if (err != NULL) {
            *err = e;
        }
        return st;
    }
    (void)snprintf(reason, sizeof reason, "%s",
                   strncmp(e.msg, unsupported, sizeof unsupported - 1) == 0 ? e.msg + sizeof unsupported - 1
                                                                            : e.msg);
    {
        pc_cimage img;
        pc_io_import_result r;
        st = upright_preview(bytes, len, p.jpeg, p.len, &img, err);
        if (st != PC_OK) {
            return st;
        }
        st = pc_io_image_to_document(name, &img, &r, err);
        pc_cimage_fini(&img);
        if (st != PC_OK) {
            return st;
        }
        st = pc_io_warn_at(
            &r.warnings, 0, err,
            "%s: %s is not supported yet; opened the camera's embedded %ux%u JPEG preview instead "
            "(8-bit, not the raw sensor data)",
            format, reason, (unsigned)p.width, (unsigned)p.height);
        if (st != PC_OK) {
            pc_io_import_result_fini(&r);
            return st;
        }
        *out = r;
        return PC_OK;
    }
}

pc_status pc_io_import_raw_tuned(const char *name, const uint8_t *bytes, size_t len,
                                 const pc_io_raw_tuning *tuning, pc_io_import_result *out, bool *wb_applied,
                                 pc_error *err)
{
    const char *format = format_name(bytes, len);
    double exposure = (double)tuning->exposure;
    pc_raw_sensor sensor;
    pc_raw_develop_options o = pc_raw_develop_options_default();
    pc_raw_developed dev;
    pc_io_import_result r;
    bool applied = false;
    char wb[256];
    char summary[512];
    pc_status st;
    if (!isfinite(exposure) || !(exposure >= -10.0 && exposure <= 10.0)) {
        char v[64];
        display_f32(tuning->exposure, v, sizeof v);
        return pc_fail(err, PC_ERR_FORMAT, "malformed camera raw: exposure %s EV is out of range", v);
    }
    o.limits = pc_io_raw_limits();
    st = pc_raw_decode(bytes, len, &o.limits, &sensor, err);
    if (st != PC_OK) {
        return st;
    }
    o.exposure = exposure;
    if (sensor.has_camera_wb) {
        float g[3];
        pc_algo_cr_white_balance_gains(pc_clampf(tuning->temperature, -100.0f, 100.0f),
                                       pc_clampf(tuning->tint, -100.0f, 100.0f), g);
        o.white_balance.kind = PC_RAW_WB_MULTIPLIERS;
        for (size_t i = 0; i < 3; i++) {
            o.white_balance.multipliers[i] = sensor.camera_wb[i] * (double)g[i];
        }
        applied = true;
    }
    st = pc_raw_develop_sensor(&sensor, &o, &dev, err);
    pc_raw_sensor_fini(&sensor);
    if (st != PC_OK) {
        return st;
    }
    if (applied) {
        char t[64];
        char n[64];
        signed_fixed(tuning->temperature, 0, t, sizeof t);
        signed_fixed(tuning->tint, 0, n, sizeof n);
        (void)snprintf(wb, sizeof wb, "as-shot white balance, temperature %s, tint %s", t, n);
    } else {
        (void)snprintf(wb, sizeof wb, "estimated white balance");
    }
    {
        char x[64];
        signed_fixed(tuning->exposure, 2, x, sizeof x);
        (void)snprintf(summary, sizeof summary, "developed in Camera Raw (exposure %s EV, %s)", x, wb);
    }
    st = developed_document(name, format, &dev, summary, &r, err);
    if (st != PC_OK) {
        return st;
    }
    *out = r;
    *wb_applied = applied;
    return PC_OK;
}
