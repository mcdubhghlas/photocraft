/* Reject params a command does not document (port of crates/engine/src/param_check.rs, E4, #4336).
 *
 * This file owns the pc_engine_hook_param_check seam declared in engine_internal.h (its E1-safe
 * default -- "always PC_OK" -- was deleted from hooks.c when this module landed). The dispatcher
 * calls the hook once per command, for every transport.
 *
 * Only the commands in STRICT are checked, because many `params` docs are free-form text and not
 * yet complete. For a STRICT command, every key the command reads must be named "key":... in its
 * params doc; a doc with a top-level ellipsis is treated as open-ended and never checked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_internal.h"

/* Keys any command accepts: the history-coalescing key every command honours and the Channels
 * panel "target" that pixel commands follow (param_check::UNIVERSAL). */
static const char *const UNIVERSAL[] = {"coalesce", "target"};

/* The commands whose unknown params are rejected: their params doc is complete (param_check::STRICT). */
static const char *const STRICT[] = {"layer.hideLayers", "layer.showLayers"};

static bool in_list(const char *const *list, size_t n, const char *s, size_t len)
{
    for (size_t i = 0; i < n; i++) {
        if (strlen(list[i]) == len && memcmp(list[i], s, len) == 0) {
            return true;
        }
    }
    return false;
}

/* One documented key: a borrowed slice [a, b) of the params doc. */
typedef struct {
    size_t a, b;
} span;

/* The keys a params doc names, or (via *open=true) that the doc is open-ended
 * (param_check::documented_keys). *out holds *n quoted spans that are keys; the caller passes a
 * buffer of cap spans (a doc never names more "words" than its own length). */
static void documented_keys(const char *doc, span *quoted, size_t *nquoted_out, span *keys,
                            size_t *nkeys_out, bool *open)
{
    size_t len = strlen(doc);
    size_t depth = 0;
    bool in_str = false;
    size_t start = 0;
    size_t nq = 0;
    *open = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)doc[i];
        if (in_str) {
            if (b == '"') {
                in_str = false;
                quoted[nq].a = start;
                quoted[nq].b = i;
                nq++;
            }
            continue;
        }
        switch (b) {
        case '"':
            in_str = true;
            start = i + 1;
            break;
        case '{':
        case '[':
            depth++;
            break;
        case '}':
        case ']':
            if (depth > 0) {
                depth--;
            }
            break;
        default:
            break;
        }
        /* An ellipsis ("..." or the UTF-8 "\xE2\x80\xA6") straight inside the top-level braces:
         * the list is not complete. The left side must trim-start to a '{'. */
        if (depth == 1) {
            bool is_ellipsis = (i + 3 <= len && memcmp(doc + i, "...", 3) == 0) ||
                               (i + 3 <= len && (unsigned char)doc[i] == 0xE2 &&
                                (unsigned char)doc[i + 1] == 0x80 && (unsigned char)doc[i + 2] == 0xA6);
            if (is_ellipsis) {
                size_t j = 0;
                while (j < i && (doc[j] == ' ' || doc[j] == '\t' || doc[j] == '\n' || doc[j] == '\r')) {
                    j++;
                }
                if (j < i && doc[j] == '{') {
                    *open = true;
                    *nquoted_out = nq;
                    *nkeys_out = 0;
                    return;
                }
            }
        }
    }
    *nquoted_out = nq;
    size_t nk = 0;
    for (size_t n = 0; n < nq; n++) {
        size_t a = quoted[n].a, b = quoted[n].b;
        if (b <= a) {
            continue;
        }
        bool wordy = true;
        for (size_t k = a; k < b; k++) {
            unsigned char c = (unsigned char)doc[k];
            bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
            if (!ok) {
                wordy = false;
                break;
            }
        }
        if (!wordy) {
            continue;
        }
        /* A key is followed by ':', or by ,"other" pairs that end in one. */
        size_t end = b + 1;
        size_t k = n;
        for (;;) {
            const char *rest = doc + (end <= len ? end : len);
            if (end <= len && rest[0] == ':') {
                keys[nk++] = quoted[n];
                break;
            }
            if (!(end < len && doc[end] == ',')) {
                break;
            }
            if (k + 1 >= nq) {
                break;
            }
            size_t na = quoted[k + 1].a, nb = quoted[k + 1].b;
            /* na == end + 2 ( ',' then '"' ), and the char after ',' is '"'. */
            if (na != end + 2 || !(end + 1 < len && doc[end + 1] == '"')) {
                break;
            }
            end = nb + 1;
            k++;
        }
    }
    *nkeys_out = nk;
}

pc_status pc_engine_hook_param_check(const pc_engine_command *spec, yyjson_val *params, pc_error *err)
{
    if (!in_list(STRICT, sizeof STRICT / sizeof STRICT[0], spec->id, strlen(spec->id))) {
        return PC_OK;
    }
    if (params == NULL || !yyjson_is_obj(params)) {
        return PC_OK;
    }
    const char *doc = spec->params;
    size_t dlen = strlen(doc);
    /* A quoted word takes at least 2 chars ("") so there are at most dlen/2 of them. */
    size_t cap = dlen / 2 + 1;
    span *quoted = malloc(cap * sizeof *quoted);
    span *keys = malloc(cap * sizeof *keys);
    if (quoted == NULL || keys == NULL) {
        free(quoted);
        free(keys);
        return pc_fail(err, PC_ERR_NOMEM, "param check");
    }
    size_t nq = 0, nk = 0;
    bool open = false;
    documented_keys(doc, quoted, &nq, keys, &nk, &open);
    pc_status rc = PC_OK;
    if (!open) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach (params, idx, max, key, val) {
            (void)val;
            size_t klen = 0;
            const char *kstr = pc_json_as_str(key, &klen);
            if (kstr == NULL) {
                continue;
            }
            if (in_list(UNIVERSAL, sizeof UNIVERSAL / sizeof UNIVERSAL[0], kstr, klen)) {
                continue;
            }
            bool known = false;
            for (size_t i = 0; i < nk; i++) {
                if (keys[i].b - keys[i].a == klen && memcmp(doc + keys[i].a, kstr, klen) == 0) {
                    known = true;
                    break;
                }
            }
            if (known) {
                continue;
            }
            /* Build "expected: a, b" or "no parameters". */
            char expected[192];
            if (nk == 0) {
                snprintf(expected, sizeof expected, "no parameters");
            } else {
                size_t w = 0;
                w += (size_t)snprintf(expected, sizeof expected, "expected: ");
                for (size_t i = 0; i < nk && w < sizeof expected - 1; i++) {
                    size_t klen2 = keys[i].b - keys[i].a;
                    w += (size_t)snprintf(expected + w, sizeof expected - w, "%s%.*s", i == 0 ? "" : ", ",
                                          (int)klen2, doc + keys[i].a);
                }
            }
            rc = pc_engine_err_bad_params(err, spec->id, "unknown parameter `%.*s` (%s)", (int)klen, kstr,
                                          expected);
            break;
        }
    }
    free(quoted);
    free(keys);
    return rc;
}
