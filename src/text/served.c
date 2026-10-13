/* Served fonts: no-op stubs (see served.h; the web build is not ported, D2). */
#include "text/served.h"

pc_status pc_text_served_script_fallback_families(pc_strlist *out, pc_error *err)
{
    (void)err;
    out->v = NULL;
    out->n = 0;
    return PC_OK;
}

void pc_text_served_request(const char *family, size_t len)
{
    (void)family;
    (void)len;
}

void pc_text_served_request_for_text(const char *text, size_t len)
{
    (void)text;
    (void)len;
}
