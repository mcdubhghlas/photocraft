#include "cms.h"

#include <stdio.h>
#include <string.h>

bool pc_intent_from_u32(uint32_t v, pc_intent *out)
{
    if (v > 3) {
        return false;
    }
    *out = (pc_intent)v;
    return true;
}

bool pc_intent_parse(const char *s, size_t len, pc_intent *out)
{
    static const struct {
        const char *name;
        pc_intent intent;
    } names[] = {
        {"perceptual", PC_INTENT_PERCEPTUAL},
        {"0", PC_INTENT_PERCEPTUAL},
        {"relative", PC_INTENT_RELATIVE},
        {"relativecolorimetric", PC_INTENT_RELATIVE},
        {"colorimetric", PC_INTENT_RELATIVE},
        {"1", PC_INTENT_RELATIVE},
        {"saturation", PC_INTENT_SATURATION},
        {"2", PC_INTENT_SATURATION},
        {"absolute", PC_INTENT_ABSOLUTE},
        {"absolutecolorimetric", PC_INTENT_ABSOLUTE},
        {"3", PC_INTENT_ABSOLUTE},
    };
    char key[32];
    size_t n = 0;
    size_t i;
    if (s == NULL) {
        return false;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + 32);
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (n + 1 >= sizeof key) {
                return false;
            }
            key[n++] = (char)c;
        }
    }
    key[n] = '\0';
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(key, names[i].name) == 0) {
            *out = names[i].intent;
            return true;
        }
    }
    return false;
}

const char *pc_intent_id(pc_intent i)
{
    switch (i) {
    case PC_INTENT_PERCEPTUAL: return "perceptual";
    case PC_INTENT_RELATIVE: return "relative";
    case PC_INTENT_SATURATION: return "saturation";
    case PC_INTENT_ABSOLUTE: return "absolute";
    }
    return "relative";
}

/* n of an `nCLR` signature (2..15), or 0. */
static unsigned clr_count(uint32_t s)
{
    unsigned d = s >> 24;
    if ((s & 0xFFFFFFu) != PC_SIG(0, 'C', 'L', 'R')) {
        return 0;
    }
    if (d >= '2' && d <= '9') {
        return d - '0';
    }
    if (d >= 'A' && d <= 'F') {
        return d - 'A' + 10;
    }
    return 0;
}

size_t pc_cs_channels(uint32_t s)
{
    switch (s) {
    case PC_CS_GRAY: return 1;
    case PC_CS_CMYK: return 4;
    case PC_CS_XYZ:
    case PC_CS_LAB:
    case PC_CS_LUV:
    case PC_CS_YCBCR:
    case PC_CS_YXY:
    case PC_CS_RGB:
    case PC_CS_HSV:
    case PC_CS_HLS:
    case PC_CS_CMY: return 3;
    default: return clr_count(s);
    }
}

void pc_cs_debug_name(uint32_t s, char out[32])
{
    const char *n = NULL;
    switch (s) {
    case PC_CS_XYZ: n = "Xyz"; break;
    case PC_CS_LAB: n = "Lab"; break;
    case PC_CS_LUV: n = "Luv"; break;
    case PC_CS_YCBCR: n = "YCbCr"; break;
    case PC_CS_YXY: n = "Yxy"; break;
    case PC_CS_RGB: n = "Rgb"; break;
    case PC_CS_GRAY: n = "Gray"; break;
    case PC_CS_HSV: n = "Hsv"; break;
    case PC_CS_HLS: n = "Hls"; break;
    case PC_CS_CMYK: n = "Cmyk"; break;
    case PC_CS_CMY: n = "Cmy"; break;
    default: break;
    }
    if (n != NULL) {
        (void)snprintf(out, 32, "%s", n);
    } else if (clr_count(s) != 0) {
        (void)snprintf(out, 32, "Color(%u)", clr_count(s));
    } else {
        (void)snprintf(out, 32, "Unknown(%lu)", (unsigned long)s);
    }
}

void pc_class_debug_name(uint32_t c, char out[32])
{
    const char *n = NULL;
    switch (c) {
    case PC_CLASS_INPUT: n = "Input"; break;
    case PC_CLASS_DISPLAY: n = "Display"; break;
    case PC_CLASS_OUTPUT: n = "Output"; break;
    case PC_CLASS_DEVICE_LINK: n = "DeviceLink"; break;
    case PC_CLASS_COLOR_SPACE: n = "ColorSpace"; break;
    case PC_CLASS_ABSTRACT: n = "Abstract"; break;
    case PC_CLASS_NAMED_COLOR: n = "NamedColor"; break;
    default: break;
    }
    if (n != NULL) {
        (void)snprintf(out, 32, "%s", n);
    } else {
        (void)snprintf(out, 32, "Unknown(%lu)", (unsigned long)c);
    }
}
