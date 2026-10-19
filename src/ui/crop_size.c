#include "crop_size.h"
#include <string.h>
#include <math.h>
#include <ctype.h>
static bool number(const char **text, double *out)
{
    const char *p = *text;
    while (*p == ' ' || *p == '\t')
        p++;
    bool digit = false, dot = false;
    double value = 0, fraction = .1;
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            digit = true;
            if (dot) {
                value += (*p - '0') * fraction;
                fraction *= .1;
            } else
                value = value * 10 + (*p - '0');
            if (value > 1e9)
                return false;
        } else if ((*p == '.' || *p == ',') && !dot)
            dot = true;
        else
            break;
    }
    while (*p == ' ' || *p == '\t')
        p++;
    if (!digit || !isfinite(value) || value <= 0)
        return false;
    *text = p;
    *out = value;
    return true;
}
bool pc_ui_crop_pixels(const char *text, double ppi, double *pixels)
{
    if (!text || !pixels || strnlen(text, 65) > 64 || !isfinite(ppi) || ppi <= 0)
        return false;
    const char *p = text;
    double value;
    if (!number(&p, &value))
        return false;
    char suffix[16];
    size_t length = strlen(p);
    while (length && isspace((unsigned char)p[length - 1]))
        length--;
    if (length >= sizeof suffix)
        return false;
    for (size_t i = 0; i < length; i++)
        suffix[i] = (char)tolower((unsigned char)p[i]);
    suffix[length] = 0;
    double factor = 1;
    if (!*suffix || !strcmp(suffix, "px") || !strcmp(suffix, "pixels"))
        factor = 1;
    else if (!strcmp(suffix, "in") || !strcmp(suffix, "inch") || !strcmp(suffix, "inches") ||
             !strcmp(suffix, "\""))
        factor = ppi;
    else if (!strcmp(suffix, "cm"))
        factor = ppi / 2.54;
    else if (!strcmp(suffix, "mm"))
        factor = ppi / 25.4;
    else if (!strcmp(suffix, "pt"))
        factor = ppi / 72;
    else if (!strcmp(suffix, "pc") || !strcmp(suffix, "pica"))
        factor = ppi / 6;
    else
        return false;
    value *= factor;
    if (!isfinite(value) || value <= 0 || value > 1e9)
        return false;
    *pixels = value;
    return true;
}
bool pc_ui_crop_resolution(const char *text, const char *unit, double *ppi)
{
    if (!text || !unit || !ppi || strnlen(text, 65) > 64)
        return false;
    const char *p = text;
    double value;
    if (!number(&p, &value) || *p)
        return false;
    if (!strcmp(unit, "px/cm"))
        value *= 2.54;
    else if (strcmp(unit, "px/in"))
        return false;
    if (!isfinite(value) || value > 1e9)
        return false;
    *ppi = value;
    return true;
}
