/* The reference's plug-in Error variants as messages (crates/plugins/src/lib.rs), and Rust's
 * `{:?}` quoting for the strings they embed. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "base/utf8.h"
#include "plugins/internal.h"

static const char *const PREFIX[] = {
    [PC_PLUGIN_E_NONE] = "",
    [PC_PLUGIN_E_MODULE] = "not a valid plug-in module: ",
    [PC_PLUGIN_E_ABI] = "plug-in ABI: ",
    [PC_PLUGIN_E_MANIFEST] = "plug-in manifest: ",
    [PC_PLUGIN_E_TRAP] = "plug-in trapped: ",
    [PC_PLUGIN_E_LIMIT] = "plug-in exceeded its ",
    [PC_PLUGIN_E_PARAMS] = "plug-in parameters: ",
    [PC_PLUGIN_E_FAILED] = "plug-in failed: ",
    [PC_PLUGIN_E_NOT_FOUND] = "",
    [PC_PLUGIN_E_IO] = "",
};

static pc_status kind_status(pc_plugin_error_kind kind)
{
    switch (kind) {
    case PC_PLUGIN_E_LIMIT:
        return PC_ERR_LIMIT;
    case PC_PLUGIN_E_PARAMS:
        return PC_ERR_INVALID;
    case PC_PLUGIN_E_NOT_FOUND:
        return PC_ERR_NOT_FOUND;
    case PC_PLUGIN_E_IO:
        return PC_ERR_IO;
    default:
        return PC_ERR_FORMAT;
    }
}

void pc_plugin_fail_set(pc_error *err, pc_plugin_error_kind kind, const char *fmt, ...)
{
    char detail[PC_ERROR_MSG_MAX];
    va_list ap;
    if (err == NULL) {
        return;
    }
    va_start(ap, fmt);
    if (vsnprintf(detail, sizeof detail, fmt, ap) < 0) {
        detail[0] = '\0';
    }
    va_end(ap);
    if ((unsigned)kind >= sizeof PREFIX / sizeof PREFIX[0]) {
        kind = PC_PLUGIN_E_FAILED;
    }
    pc_fail_set(err, kind_status(kind), "%s%s", PREFIX[kind], detail);
}

pc_plugin_error_kind pc_plugin_error_kind_of(const pc_error *err)
{
    if (err == NULL || err->code == PC_OK) {
        return PC_PLUGIN_E_NONE;
    }
    if (err->code == PC_ERR_NOT_FOUND && strncmp(err->msg, "no plug-in ", 11) == 0) {
        return PC_PLUGIN_E_NOT_FOUND;
    }
    if (err->code == PC_ERR_IO) {
        return PC_PLUGIN_E_IO;
    }
    for (int k = PC_PLUGIN_E_MODULE; k <= PC_PLUGIN_E_FAILED; k++) {
        size_t n = strlen(PREFIX[k]);
        if (err->code == kind_status((pc_plugin_error_kind)k) && strncmp(err->msg, PREFIX[k], n) == 0) {
            return (pc_plugin_error_kind)k;
        }
    }
    return PC_PLUGIN_E_NONE;
}

pc_status pc_plugin_fail_not_found(pc_error *err, const char *id, size_t id_len)
{
    pc_buf q = PC_BUF_INIT;
    pc_plugin_debug_quote(id, id_len, &q);
    (void)pc_fail(err, PC_ERR_NOT_FOUND, "no plug-in %s is installed", pc_buf_cstr(&q));
    pc_buf_fini(&q);
    return PC_ERR_NOT_FOUND;
}

/* Characters Rust's escape_debug writes as \u{..}: controls, format characters, separators other
 * than the space, private use, combining marks and variation selectors (an approximation of
 * Rust's is_printable / Grapheme_Extend tables covering what manifests plausibly contain). */
static bool rust_escapes(uint32_t c)
{
    static const uint32_t R[][2] = {
        {0x00, 0x1F},       {0x7F, 0x9F},       {0xA0, 0xA0},        {0xAD, 0xAD},       {0x300, 0x36F},
        {0x378, 0x379},     {0x600, 0x605},     {0x61C, 0x61C},      {0x6DD, 0x6DD},     {0x70F, 0x70F},
        {0x890, 0x891},     {0x8E2, 0x8E2},     {0x1680, 0x1680},    {0x180B, 0x180F},   {0x2000, 0x200F},
        {0x2028, 0x202F},   {0x205F, 0x2064},   {0x2066, 0x206F},    {0x20D0, 0x20F0},   {0x3000, 0x3000},
        {0xD800, 0xF8FF},   {0xFE00, 0xFE0F},   {0xFE20, 0xFE2F},    {0xFEFF, 0xFEFF},   {0xFFF0, 0xFFFB},
        {0xFFFE, 0xFFFF},   {0x110BD, 0x110BD}, {0x110CD, 0x110CD},  {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3},
        {0x1D173, 0x1D17A}, {0xE0000, 0xE01EF}, {0xF0000, 0x10FFFF},
    };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        if (c >= R[i][0] && c <= R[i][1]) {
            return true;
        }
    }
    return false;
}

void pc_plugin_debug_quote(const char *s, size_t len, pc_buf *out)
{
    const uint8_t *u = (const uint8_t *)s;
    size_t i = 0;
    (void)pc_buf_append_byte(out, '"', NULL);
    while (i < len) {
        uint32_t cp = 0;
        size_t start = i;
        if (!pc_utf8_next(u, len, &i, &cp)) {
            /* Not UTF-8 (cannot happen for JSON text): show the replacement character. */
            (void)pc_buf_append_str(out, "\xEF\xBF\xBD", NULL);
            i = start + 1;
            continue;
        }
        switch (cp) {
        case '"':
            (void)pc_buf_append_str(out, "\\\"", NULL);
            break;
        case '\\':
            (void)pc_buf_append_str(out, "\\\\", NULL);
            break;
        case 0:
            (void)pc_buf_append_str(out, "\\0", NULL);
            break;
        case '\t':
            (void)pc_buf_append_str(out, "\\t", NULL);
            break;
        case '\n':
            (void)pc_buf_append_str(out, "\\n", NULL);
            break;
        case '\r':
            (void)pc_buf_append_str(out, "\\r", NULL);
            break;
        default:
            if (rust_escapes(cp)) {
                (void)pc_buf_appendf(out, NULL, "\\u{%x}", (unsigned)cp);
            } else {
                (void)pc_buf_append(out, u + start, i - start, NULL);
            }
        }
    }
    (void)pc_buf_append_byte(out, '"', NULL);
}
