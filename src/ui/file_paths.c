#include "file_paths.h"
#include <ctype.h>
#include <string.h>

/* Lowercase an ASCII byte; leaves non-letters and non-ASCII bytes alone. */
static unsigned char lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Compare two zero-stripped digit runs s[0,sn) and t[0,tn): shorter value first, then by bytes. */
static int cmp_digits(const unsigned char *s, size_t sn, const unsigned char *t, size_t tn)
{
    while (sn && *s == '0') {
        s++;
        sn--;
    }
    while (tn && *t == '0') {
        t++;
        tn--;
    }
    if (sn != tn) {
        return sn < tn ? -1 : 1;
    }
    for (size_t i = 0; i < sn; i++) {
        if (s[i] != t[i]) {
            return s[i] < t[i] ? -1 : 1;
        }
    }
    return 0;
}

int pc_ui_natural_cmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)(a ? a : "");
    const unsigned char *y = (const unsigned char *)(b ? b : "");
    size_t i = 0, j = 0;
    for (;;) {
        unsigned char c = x[i], d = y[j];
        if (!c && !d) {
            return strcmp((const char *)x, (const char *)y);
        }
        if (!c) {
            return -1;
        }
        if (!d) {
            return 1;
        }
        if (isdigit(c) && isdigit(d)) {
            size_t si = i, sj = j;
            while (isdigit(x[i])) {
                i++;
            }
            while (isdigit(y[j])) {
                j++;
            }
            int r = cmp_digits(x + si, i - si, y + sj, j - sj);
            if (r) {
                return r;
            }
        } else {
            unsigned char lc = lower(c), ld = lower(d);
            if (lc != ld) {
                return lc < ld ? -1 : 1;
            }
            i++;
            j++;
        }
    }
}

const char *pc_ui_display_name(const char *path)
{
    if (!path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    return *name ? name : path;
}

bool pc_ui_lowercase_extension(const char *path, char *out, size_t cap)
{
    if (!path || !out || !cap) {
        return false;
    }
    size_t len = strlen(path);
    if (len + 1 > cap) {
        return false;
    }
    memcpy(out, path, len + 1);
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    /* No extension when there is no '.', or the only '.' starts the name (".bashrc"). */
    if (!dot || dot == base) {
        return false;
    }
    size_t ext_off = (size_t)(dot + 1 - path);
    bool changed = false;
    for (size_t i = ext_off; i < len; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = (char)lower((unsigned char)out[i]);
            changed = true;
        }
    }
    return changed;
}

bool pc_ui_last_used_dir(const char *first, char *out, size_t cap)
{
    if (!first || !*first || !out || !cap) {
        return false;
    }
    size_t len = strlen(first);
    if (len > 65536) {
        return false;
    }
    const char *slash = strrchr(first, '/');
    if (!slash) {
        return false; /* a bare file name has an empty parent */
    }
    if (slash == first) {
        if (first[1] == 0) {
            return false; /* "/" has no parent */
        }
        if (cap < 2) {
            return false;
        }
        out[0] = '/';
        out[1] = 0;
        return true;
    }
    size_t dir_len = (size_t)(slash - first);
    if (dir_len + 1 > cap) {
        return false;
    }
    memcpy(out, first, dir_len);
    out[dir_len] = 0;
    return true;
}

bool pc_ui_is_tiff_path(const char *path)
{
    if (!path) {
        return false;
    }
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) {
        return false; /* no extension, or an empty stem */
    }
    const char *ext = dot + 1;
    size_t n = strlen(ext);
    if (n == 3) {
        return lower((unsigned char)ext[0]) == 't' && lower((unsigned char)ext[1]) == 'i' &&
               lower((unsigned char)ext[2]) == 'f';
    }
    if (n == 4) {
        return lower((unsigned char)ext[0]) == 't' && lower((unsigned char)ext[1]) == 'i' &&
               lower((unsigned char)ext[2]) == 'f' && lower((unsigned char)ext[3]) == 'f';
    }
    return false;
}
