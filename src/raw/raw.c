/* Camera raw recognition and the top-level API (port of crates/raw/src/lib.rs). */
#include <stdlib.h>
#include <string.h>

#include "raw/internal.h"

#define PHOTOMETRIC_CFA 32803u
#define PHOTOMETRIC_LINEAR_RAW 34892u

const char *const PC_RAW_EXTENSIONS[PC_RAW_NEXTENSIONS] = {"dng", "cr2", "cr3", "nef", "nrw", "arw",
                                                           "srf", "sr2", "pef", "orf", "rw2", "raf"};

const char *pc_raw_format_name(pc_raw_format f)
{
    switch (f) {
    case PC_RAW_DNG:
        return "DNG";
    case PC_RAW_CR2:
        return "CR2";
    case PC_RAW_CR3:
        return "CR3";
    case PC_RAW_NEF:
        return "NEF";
    case PC_RAW_ARW:
        return "ARW";
    case PC_RAW_PEF:
        return "PEF";
    case PC_RAW_ORF:
        return "ORF";
    case PC_RAW_RW2:
        return "RW2";
    case PC_RAW_RAF:
        return "RAF";
    default:
        return "TIFF/EP raw";
    }
}

pc_raw_limits pc_raw_limits_default(void)
{
    pc_raw_limits l;
    l.max_width = 1u << 18;
    l.max_height = 1u << 18;
    l.max_pixels = (uint64_t)1 << 28;
    l.max_alloc = (uint64_t)2 << 30;
    return l;
}

pc_status pc_raw_limits_check(const pc_raw_limits *l, uint64_t width, uint64_t height, uint64_t bpp, pc_error *err)
{
    uint64_t pixels;
    uint64_t bytes;
    if (width == 0 || height == 0) {
        return rx_malformed(err, "zero-sized image %llux%llu", (unsigned long long)width, (unsigned long long)height);
    }
    if (width > l->max_width || height > l->max_height) {
        return rx_limit(err, "dimensions %llux%llu exceed %ux%u", (unsigned long long)width,
                        (unsigned long long)height, (unsigned)l->max_width, (unsigned)l->max_height);
    }
    pixels = width > UINT64_MAX / height ? UINT64_MAX : width * height;
    if (pixels > l->max_pixels) {
        return rx_limit(err, "%llu pixels exceed %llu", (unsigned long long)pixels,
                        (unsigned long long)l->max_pixels);
    }
    bytes = bpp != 0 && pixels > UINT64_MAX / bpp ? UINT64_MAX : pixels * bpp;
    if (bytes > l->max_alloc || bytes > SIZE_MAX) {
        return rx_limit(err, "%llu bytes exceed %llu", (unsigned long long)bytes, (unsigned long long)l->max_alloc);
    }
    return PC_OK;
}

static bool starts_upper(const char *s, const char *prefix)
{
    size_t i;
    for (i = 0; prefix[i] != '\0'; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != prefix[i]) {
            return false;
        }
    }
    return true;
}

static pc_raw_format make_format(const char *make)
{
    const char *m = make != NULL ? make : "";
    if (starts_upper(m, "NIKON")) {
        return PC_RAW_NEF;
    }
    if (starts_upper(m, "SONY")) {
        return PC_RAW_ARW;
    }
    if (starts_upper(m, "PENTAX") || starts_upper(m, "RICOH")) {
        return PC_RAW_PEF;
    }
    if (starts_upper(m, "OLYMPUS") || starts_upper(m, "OM DIGITAL")) {
        return PC_RAW_ORF;
    }
    return PC_RAW_TIFF_EP;
}

bool pc_raw_identify(const uint8_t *b, size_t n, pc_raw_format *out)
{
    rx_tiff t;
    rx_ifd ifds[RX_MAX_IFDS];
    size_t nifd;
    size_t i;
    if (n >= 12 && memcmp(b + 4, "ftypcrx ", 8) == 0) {
        *out = PC_RAW_CR3;
        return true;
    }
    if (n >= 15 && memcmp(b, RX_RAF_MAGIC, 15) == 0) {
        *out = PC_RAW_RAF;
        return true;
    }
    if (!rx_tiff_new(b, n, &t)) {
        return false;
    }
    if (memcmp(b, "IIRO", 4) == 0 || memcmp(b, "IIRS", 4) == 0 || memcmp(b, "MMOR", 4) == 0) {
        *out = PC_RAW_ORF;
        return true;
    }
    if (memcmp(b, "IIU\0", 4) == 0) {
        *out = PC_RAW_RW2;
        return true;
    }
    if (n >= 11 && memcmp(b + 8, "CR\x02", 3) == 0) {
        *out = PC_RAW_CR2;
        return true;
    }
    nifd = rx_all_ifds(&t, ifds);
    for (i = 0; i < nifd; i++) {
        if (rx_ifd_has(&t, &ifds[i], RX_DNG_VERSION)) {
            *out = PC_RAW_DNG;
            return true;
        }
    }
    for (i = 0; i < nifd; i++) {
        uint32_t p;
        if (rx_tag_uint(&t, &ifds[i], RX_PHOTOMETRIC, &p) && (p == PHOTOMETRIC_CFA || p == PHOTOMETRIC_LINEAR_RAW)) {
            bool nomem = false;
            char *make = rx_find_ascii(&t, ifds, nifd, RX_MAKE, &nomem);
            *out = make_format(make);
            free(make);
            return true;
        }
    }
    if (rx_is_sony_non_cfa(&t, ifds, nifd)) {
        *out = PC_RAW_ARW;
        return true;
    }
    return false;
}

bool pc_raw_is_raw(const uint8_t *b, size_t n)
{
    pc_raw_format f;
    return pc_raw_identify(b, n, &f);
}

pc_status pc_raw_decode(const uint8_t *b, size_t n, const pc_raw_limits *limits, pc_raw_sensor *out, pc_error *err)
{
    pc_raw_format f;
    rx_tiff t;
    if (!pc_raw_identify(b, n, &f)) {
        return rx_notraw(err);
    }
    if (f == PC_RAW_CR3) {
        return rx_unsupported(err, "Canon CR3 (ISO BMFF / CRX) is not decoded yet");
    }
    if (f == PC_RAW_RAF) {
        return rx_raf_decode(b, n, limits, out, err);
    }
    if (!rx_tiff_new(b, n, &t)) {
        return rx_notraw(err);
    }
    switch (f) {
    case PC_RAW_DNG:
        return rx_dng_decode(&t, limits, out, err);
    case PC_RAW_CR2:
        return rx_cr2_decode(&t, limits, out, err);
    case PC_RAW_RW2:
        return rx_rw2_decode(&t, limits, out, err);
    case PC_RAW_ORF:
        return rx_orf_decode(&t, limits, out, err);
    default:
        return rx_tiffep_decode(&t, f, limits, out, err);
    }
}

pc_status pc_raw_decode_lossless_jpeg(const uint8_t *b, size_t n, size_t max_samples, size_t *width,
                                      size_t *height, size_t *components, uint16_t **samples, size_t *nsamples,
                                      pc_error *err)
{
    rx_frame f;
    pc_status st = rx_ljpeg_decode(b, n, max_samples, &f, samples, nsamples, err);
    if (st == PC_OK) {
        *width = f.width;
        *height = f.height;
        *components = f.components;
    }
    return st;
}

char *pc_raw_dump_structure(const uint8_t *b, size_t n) { return rx_dump(b, n); }

pc_status pc_raw_develop(const uint8_t *b, size_t n, const pc_raw_develop_options *o, pc_raw_developed *out,
                         pc_error *err)
{
    pc_raw_sensor s;
    pc_status st = pc_raw_decode(b, n, &o->limits, &s, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_raw_develop_sensor(&s, o, out, err);
    pc_raw_sensor_fini(&s);
    return st;
}

const char *pc_raw_demosaic_id(pc_raw_demosaic d)
{
    switch (d) {
    case PC_RAW_BILINEAR:
        return "bilinear";
    case PC_RAW_MHC:
        return "mhc";
    default:
        return "ahd";
    }
}

static bool eq_lower(const char *s, const char *want)
{
    size_t i;
    for (i = 0; s[i] != '\0' && want[i] != '\0'; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != want[i]) {
            return false;
        }
    }
    return s[i] == '\0' && want[i] == '\0';
}

bool pc_raw_demosaic_from_id(const char *s, pc_raw_demosaic *out)
{
    if (eq_lower(s, "bilinear") || eq_lower(s, "fast")) {
        *out = PC_RAW_BILINEAR;
    } else if (eq_lower(s, "mhc") || eq_lower(s, "malvar")) {
        *out = PC_RAW_MHC;
    } else if (eq_lower(s, "ahd") || eq_lower(s, "best")) {
        *out = PC_RAW_AHD;
    } else {
        return false;
    }
    return true;
}

pc_raw_develop_options pc_raw_develop_options_default(void)
{
    pc_raw_develop_options o;
    memset(&o, 0, sizeof o);
    o.demosaic = PC_RAW_AHD;
    o.white_balance.kind = PC_RAW_WB_AS_SHOT;
    o.exposure = 0.0;
    o.orient = true;
    o.limits = pc_raw_limits_default();
    return o;
}

void pc_raw_developed_fini(pc_raw_developed *d)
{
    if (d == NULL) {
        return;
    }
    free(d->rgb);
    free(d->info.make);
    free(d->info.model);
    pc_raw_strings_fini(&d->warnings);
    memset(d, 0, sizeof *d);
}
