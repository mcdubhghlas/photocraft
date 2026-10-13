#include "codecs/format.h"

#include <string.h>

#include "codecs/internal.h"

const pc_casymmetric PC_ASYMMETRIC_EXCEPTIONS[] = {
    {PC_CF_PCX, "PCX is supported for opening legacy paint and clipart assets. New files should use PNG; "
                "writing obsolete indexed and planar variants is intentionally not provided."},
    {PC_CF_AVIF, "AVIF is neither read nor written by this build: decoding would need an AV1 decoder and "
                 "encoding an AV1 encoder, and the format is listed only so files are recognised."},
    {PC_CF_HEIF, "HEIC decode uses libheif (with an HEVC decoder plugin), so iPhone and Mac photos open; "
                 "writing needs an HEVC encoder, so write stays unsupported. Without libheif HEIF is "
                 "detected but neither read nor written."},
};
const size_t PC_ASYMMETRIC_EXCEPTIONS_LEN = sizeof PC_ASYMMETRIC_EXCEPTIONS / sizeof PC_ASYMMETRIC_EXCEPTIONS[0];

static const pc_csample D_U8[] = {PC_CS_U8};
static const pc_csample D_U8_U16[] = {PC_CS_U8, PC_CS_U16};
static const pc_csample D_INT_F32[] = {PC_CS_U8, PC_CS_U16, PC_CS_F32};
static const pc_csample D_FLOAT[] = {PC_CS_F16, PC_CS_F32};
static const pc_csample D_F32[] = {PC_CS_F32};
static const pc_clayout L_RGB_RGBA[] = {PC_CL_RGB, PC_CL_RGBA};
static const pc_clayout L_RGB_GRAY[] = {PC_CL_GRAY, PC_CL_GRAYA, PC_CL_RGB, PC_CL_RGBA};
static const pc_clayout L_ALL[] = {PC_CL_GRAY, PC_CL_GRAYA, PC_CL_RGB, PC_CL_RGBA, PC_CL_CMYK, PC_CL_CMYKA};
static const pc_clayout L_JPEG[] = {PC_CL_GRAY, PC_CL_RGB, PC_CL_CMYK};
static const pc_clayout L_RGBA[] = {PC_CL_RGBA};
static const pc_clayout L_RGB[] = {PC_CL_RGB};
static const pc_clayout L_GRAY_RGB[] = {PC_CL_GRAY, PC_CL_RGB};

#define ARR(a) (a), (sizeof(a) / sizeof((a)[0]))

pc_ccaps pc_cformat_caps(pc_cformat f)
{
    pc_ccaps c;
    memset(&c, 0, sizeof c);
    c.read = true;
    c.write = true;
    c.depths = D_U8;
    c.ndepths = 1;
    c.layouts = L_RGB_RGBA;
    c.nlayouts = 2;
    c.alpha = true;
    switch (f) {
    case PC_CF_PNG:
        c.depths = D_U8_U16;
        c.ndepths = 2;
        c.layouts = L_RGB_GRAY;
        c.nlayouts = 4;
        c.icc = c.exif = c.xmp = c.dpi = c.text = c.animation = true;
        break;
    case PC_CF_JPEG:
        c.layouts = L_JPEG;
        c.nlayouts = 3;
        c.alpha = false;
        c.icc = c.exif = c.xmp = c.dpi = c.lossy = true;
        break;
    case PC_CF_TIFF:
        c.depths = D_INT_F32;
        c.ndepths = 3;
        c.layouts = L_ALL;
        c.nlayouts = 6;
        c.icc = c.xmp = c.dpi = c.text = true;
        break;
    case PC_CF_WEBP:
        c.icc = c.exif = c.xmp = c.animation = true;
        break;
    case PC_CF_GIF:
        c.layouts = L_RGBA;
        c.nlayouts = 1;
        c.animation = c.lossy = true;
        break;
    case PC_CF_BMP:
    case PC_CF_QOI:
        break;
    case PC_CF_TGA:
        c.layouts = L_RGB_GRAY;
        c.nlayouts = 4;
        break;
    case PC_CF_ICO:
        c.layouts = L_RGBA;
        c.nlayouts = 1;
        break;
    case PC_CF_PNM:
        c.depths = D_INT_F32;
        c.ndepths = 3;
        c.layouts = L_ALL;
        c.nlayouts = 6;
        break;
    case PC_CF_EXR:
        c.read = c.write = cx_exr_available();
        c.depths = D_FLOAT;
        c.ndepths = 2;
        c.layouts = L_RGB_GRAY;
        c.nlayouts = 4;
        break;
    case PC_CF_HDR:
        c.depths = D_F32;
        c.ndepths = 1;
        c.layouts = L_RGB;
        c.nlayouts = 1;
        c.alpha = false;
        c.lossy = true;
        break;
    case PC_CF_PCX:
        c.write = false;
        c.layouts = L_RGB;
        c.nlayouts = 1;
        c.alpha = false;
        break;
    case PC_CF_FITS:
        c.depths = D_INT_F32;
        c.ndepths = 3;
        c.layouts = L_GRAY_RGB;
        c.nlayouts = 2;
        c.alpha = false;
        c.text = true;
        break;
    case PC_CF_AVIF:
        c.read = false;
        c.write = false;
        c.lossy = true;
        break;
    default: /* HEIF */
        c.read = cx_heif_available();
        c.write = false;
        c.depths = D_U8_U16;
        c.ndepths = 2;
        c.icc = c.exif = c.xmp = c.lossy = true;
    }
    return c;
}

bool pc_cformat_is_enabled(pc_cformat f)
{
    pc_ccaps c = pc_cformat_caps(f);
    return c.read || c.write;
}

const char *pc_cformat_name(pc_cformat f)
{
    static const char *const N[PC_CF_COUNT] = {"PNG",          "JPEG", "TIFF",   "WebP", "GIF", "BMP",
                                               "TGA",          "ICO",  "Netpbm", "PCX",  "QOI", "OpenEXR",
                                               "Radiance HDR", "AVIF", "HEIF",   "FITS"};
    return (unsigned)f < PC_CF_COUNT ? N[f] : "?";
}

const char *pc_cformat_debug(pc_cformat f)
{
    static const char *const N[PC_CF_COUNT] = {"Png", "Jpeg", "Tiff", "WebP", "Gif", "Bmp",
                                               "Tga", "Ico",  "Pnm",  "Pcx",  "Qoi", "OpenExr",
                                               "Hdr", "Avif", "Heif", "Fits"};
    return (unsigned)f < PC_CF_COUNT ? N[f] : "?";
}

const char *const *pc_cformat_extensions(pc_cformat f)
{
    static const char *const PNG[] = {"png", "apng", NULL};
    static const char *const JPEG[] = {"jpg", "jpeg", "jpe", "jfif", NULL};
    static const char *const TIFF[] = {"tif", "tiff", NULL};
    static const char *const WEBP[] = {"webp", NULL};
    static const char *const GIF[] = {"gif", NULL};
    static const char *const BMP[] = {"bmp", "dib", NULL};
    static const char *const TGA[] = {"tga", "icb", "vda", "vst", NULL};
    static const char *const ICO[] = {"ico", NULL};
    static const char *const PNM[] = {"pnm", "pbm", "pgm", "ppm", "pam", "pfm", NULL};
    static const char *const QOI[] = {"qoi", NULL};
    static const char *const EXR[] = {"exr", NULL};
    static const char *const HDR[] = {"hdr", NULL};
    static const char *const AVIF[] = {"avif", NULL};
    static const char *const HEIF[] = {"heic", "heif", "hif", NULL};
    static const char *const PCX[] = {"pcx", NULL};
    static const char *const FITS[] = {"fits", "fit", "fts", NULL};
    static const char *const *const ALL[PC_CF_COUNT] = {PNG, JPEG, TIFF, WEBP, GIF, BMP,  TGA,  ICO,
                                                        PNM, PCX,  QOI,  EXR,  HDR, AVIF, HEIF, FITS};
    return (unsigned)f < PC_CF_COUNT ? ALL[f] : HEIF + 3;
}

const char *pc_cformat_mime(pc_cformat f)
{
    static const char *const N[PC_CF_COUNT] = {
        "image/png",          "image/jpeg",   "image/tiff",
        "image/webp",         "image/gif",    "image/bmp",
        "image/x-tga",        "image/x-icon", "image/x-portable-anymap",
        "image/x-pcx",        "image/qoi",    "image/x-exr",
        "image/vnd.radiance", "image/avif",   "image/heif",
        "image/fits"};
    return (unsigned)f < PC_CF_COUNT ? N[f] : "image/unknown";
}

bool pc_cformat_max_dimensions(pc_cformat f, uint32_t *w, uint32_t *h)
{
    switch (f) {
    case PC_CF_JPEG:
    case PC_CF_GIF:
        *w = *h = 65535;
        return true;
    case PC_CF_WEBP:
        *w = *h = 16384;
        return true;
    case PC_CF_ICO:
        *w = *h = 256;
        return true;
    default:
        return false;
    }
}

/* ---- detection ---- */

static bool starts(const uint8_t *b, size_t n, const void *m, size_t k) { return n >= k && memcmp(b, m, k) == 0; }

/* The ftyp brands: the major brand, then the compatible ones (the minor version is skipped). */
static bool ftyp_has(const uint8_t *b, size_t n, const char *const *brands)
{
    size_t size = cx_be32(b);
    size_t end = n < 64 ? n : 64;
    size_t i;
    if (size < 12) {
        size = 12;
    }
    if (size > end) {
        size = end;
    }
    for (i = 0; 8 + 4 * i + 4 <= size; i++) {
        size_t k;
        if (i == 1) {
            continue;
        }
        for (k = 0; brands[k] != NULL; k++) {
            if (memcmp(b + 8 + 4 * i, brands[k], 4) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool is_space(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

static bool looks_like_tga(const uint8_t *b, size_t n)
{
    uint8_t cmap_type;
    uint8_t img_type;
    uint16_t cmap_len;
    uint8_t cmap_depth;
    uint16_t w;
    uint16_t h;
    uint8_t depth;
    uint8_t desc;
    bool type_ok;
    bool cmap_ok;
    bool depth_ok;
    if (n < 18) {
        return false;
    }
    if (n >= 26 && memcmp(b + n - 18, "TRUEVISION-XFILE.\0", 18) == 0) {
        return true;
    }
    cmap_type = b[1];
    img_type = b[2];
    cmap_len = cx_le16(b + 5);
    cmap_depth = b[7];
    w = cx_le16(b + 12);
    h = cx_le16(b + 14);
    depth = b[16];
    desc = b[17];
    type_ok = img_type == 1 || img_type == 2 || img_type == 3 || img_type == 9 || img_type == 10 || img_type == 11;
    if (cmap_type == 0) {
        cmap_ok = cmap_len == 0 && cmap_depth == 0 && img_type != 1 && img_type != 9;
    } else if (cmap_type == 1) {
        cmap_ok = (img_type == 1 || img_type == 9) && cmap_len > 0 &&
                  (cmap_depth == 15 || cmap_depth == 16 || cmap_depth == 24 || cmap_depth == 32);
    } else {
        cmap_ok = false;
    }
    if (img_type == 1 || img_type == 9) {
        depth_ok = depth == 8;
    } else if (img_type == 3 || img_type == 11) {
        depth_ok = depth == 8 || depth == 16;
    } else {
        depth_ok = depth == 15 || depth == 16 || depth == 24 || depth == 32;
    }
    return type_ok && cmap_ok && depth_ok && w > 0 && h > 0 && (desc & 0xC0) == 0;
}

bool pc_cformat_detect(const uint8_t *b, size_t n, pc_cformat *out)
{
    static const char *const AVIF[] = {"avif", "avis", NULL};
    static const char *const HEIF[] = {"heic", "heix", "heim", "heis", "hevc", "hevx", "mif1", "msf1", "miaf", NULL};
    pc_cformat f;
    if (starts(b, n, "\x89PNG\r\n\x1a\n", 8)) {
        f = PC_CF_PNG;
    } else if (starts(b, n, "\xFF\xD8\xFF", 3)) {
        f = PC_CF_JPEG;
    } else if (starts(b, n, "II*\0", 4) || starts(b, n, "MM\0*", 4) || starts(b, n, "II+\0", 4) ||
               starts(b, n, "MM\0+", 4)) {
        f = PC_CF_TIFF;
    } else if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) {
        f = PC_CF_WEBP;
    } else if (starts(b, n, "GIF87a", 6) || starts(b, n, "GIF89a", 6)) {
        f = PC_CF_GIF;
    } else if (starts(b, n, "qoif", 4)) {
        f = PC_CF_QOI;
    } else if (starts(b, n, "\x76\x2F\x31\x01", 4)) {
        f = PC_CF_EXR;
    } else if (starts(b, n, "#?RADIANCE", 10) || starts(b, n, "#?RGBE", 6)) {
        f = PC_CF_HDR;
    } else if (n >= 12 && memcmp(b + 4, "ftyp", 4) == 0 && ftyp_has(b, n, AVIF)) {
        f = PC_CF_AVIF;
    } else if (n >= 12 && memcmp(b + 4, "ftyp", 4) == 0 && ftyp_has(b, n, HEIF)) {
        f = PC_CF_HEIF;
    } else if (n >= 14 && memcmp(b, "BM", 2) == 0) {
        f = PC_CF_BMP;
    } else if (n >= 6 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0 && (b[4] != 0 || b[5] != 0)) {
        f = PC_CF_ICO;
    } else if (n >= 3 && b[0] == 'P' && ((b[1] >= '1' && b[1] <= '7') || b[1] == 'F' || b[1] == 'f') &&
               is_space(b[2])) {
        f = PC_CF_PNM;
    } else if (starts(b, n, "SIMPLE  =", 9)) {
        f = PC_CF_FITS;
    } else if (cx_pcx_looks_like(b, n)) {
        f = PC_CF_PCX;
    } else if (looks_like_tga(b, n)) {
        f = PC_CF_TGA;
    } else {
        return false;
    }
    *out = f;
    return true;
}

bool pc_cformat_from_extension(const char *ext, pc_cformat *out)
{
    const char *start = ext;
    const char *p;
    char low[16];
    size_t n;
    size_t i;
    int f;
    for (p = ext; *p != '\0'; p++) {
        if (*p == '.' || *p == '/' || *p == '\\') {
            start = p + 1;
        }
    }
    n = strlen(start);
    if (n == 0 || n >= sizeof low) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = start[i];
        low[i] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    low[n] = '\0';
    for (f = 0; f < PC_CF_COUNT; f++) {
        const char *const *e = pc_cformat_extensions((pc_cformat)f);
        for (i = 0; e[i] != NULL; i++) {
            if (strcmp(e[i], low) == 0) {
                *out = (pc_cformat)f;
                return true;
            }
        }
    }
    return false;
}
