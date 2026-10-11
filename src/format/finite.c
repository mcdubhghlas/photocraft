#include "format/finite.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "format/format.h"

typedef struct frame {
    bool is_obj;
    yyjson_mut_arr_iter ai;
    yyjson_mut_obj_iter oi;
    size_t index;    /* next array index */
    size_t path_len; /* path length at this container */
} frame;

/* yyjson's iterators take mutable pointers; the walk never modifies the tree. */
static yyjson_mut_val *unconst(const yyjson_mut_val *v)
{
    return (yyjson_mut_val *)(uintptr_t)v;
}

static bool non_finite(const yyjson_mut_val *v)
{
    return yyjson_mut_is_real(unconst(v)) && !isfinite(yyjson_mut_get_real(unconst(v)));
}

/* Append ".<key as serde_json::to_string writes it, surrounding quotes trimmed>". */
static pc_status append_key(pc_buf *path, const yyjson_mut_val *key, pc_error *err)
{
    static const char HEX[] = "0123456789abcdef";
    const char *k = yyjson_mut_get_str(unconst(key));
    size_t n = yyjson_mut_get_len(unconst(key));
    size_t start;
    pc_status s = pc_buf_append_byte(path, '.', err);
    if (s != PC_OK) {
        return s;
    }
    start = path->len;
    for (size_t i = 0; i < n && s == PC_OK; i++) {
        unsigned char c = (unsigned char)k[i];
        switch (c) {
        case '"':
            s = pc_buf_append_str(path, "\\\"", err);
            break;
        case '\\':
            s = pc_buf_append_str(path, "\\\\", err);
            break;
        case '\b':
            s = pc_buf_append_str(path, "\\b", err);
            break;
        case '\f':
            s = pc_buf_append_str(path, "\\f", err);
            break;
        case '\n':
            s = pc_buf_append_str(path, "\\n", err);
            break;
        case '\r':
            s = pc_buf_append_str(path, "\\r", err);
            break;
        case '\t':
            s = pc_buf_append_str(path, "\\t", err);
            break;
        default:
            if (c < 0x20) {
                char u[7] = {'\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 15], '\0'};
                s = pc_buf_append_str(path, u, err);
            } else {
                s = pc_buf_append_byte(path, c, err);
            }
        }
    }
    if (s != PC_OK) {
        return s;
    }
    /* trim_matches('"') on the quoted text: the escaped body never starts with a bare quote, but it
     * may end with one (from an escaped `\"`), which goes too. */
    while (path->len > start && path->data[path->len - 1] == '"') {
        pc_buf_truncate(path, path->len - 1);
    }
    return PC_OK;
}

static pc_status report(const pc_buf *walk, pc_buf *path_out, pc_error *err)
{
    const char *p = pc_buf_cstr(walk);
    size_t n = walk->len;
    while (n > 0 && *p == '.') { /* trim_start_matches('.') */
        p++;
        n--;
    }
    if (path_out != NULL) {
        pc_buf b = PC_BUF_INIT;
        pc_status s = pc_buf_append(&b, p, n, err);
        if (s != PC_OK) {
            return s;
        }
        pc_buf_fini(path_out);
        *path_out = b;
    }
    pc_format_fail_set(
        err, PC_ERR_INVALID, "",
        "`%.*s` is NaN or infinite, which a .pcraft manifest cannot hold; reset that value and save again",
        (int)(n > 4096 ? 4096 : n), p);
    return PC_ERR_INVALID;
}

pc_status pc_format_check_finite(const yyjson_mut_val *root, pc_buf *path_out, pc_error *err)
{
    frame *stack = NULL;
    size_t depth = 0;
    size_t cap = 0;
    pc_buf path = PC_BUF_INIT;
    const yyjson_mut_val *v = root;
    pc_status s = PC_OK;

    for (;;) {
        /* Visit v. */
        if (v != NULL) {
            if (non_finite(v)) {
                s = report(&path, path_out, err);
                goto done;
            }
            if (yyjson_mut_is_ctn(unconst(v)) && yyjson_mut_get_len(unconst(v)) > 0) {
                frame *f;
                frame *grown = pc_grow_array(stack, &cap, depth + 1, sizeof *stack);
                if (grown == NULL) {
                    s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                    goto done;
                }
                stack = grown;
                f = &stack[depth++];
                memset(f, 0, sizeof *f);
                f->is_obj = yyjson_mut_is_obj(unconst(v));
                f->path_len = path.len;
                if (f->is_obj) {
                    (void)yyjson_mut_obj_iter_init(unconst(v), &f->oi);
                } else {
                    (void)yyjson_mut_arr_iter_init(unconst(v), &f->ai);
                }
            }
        }
        /* Next child of the innermost container that has one. */
        v = NULL;
        while (depth > 0 && v == NULL) {
            frame *f = &stack[depth - 1];
            pc_buf_truncate(&path, f->path_len);
            if (f->is_obj) {
                yyjson_mut_val *key = yyjson_mut_obj_iter_next(&f->oi);
                if (key != NULL) {
                    v = yyjson_mut_obj_iter_get_val(key);
                    s = append_key(&path, key, err);
                }
            } else {
                v = yyjson_mut_arr_iter_next(&f->ai);
                if (v != NULL) {
                    s = pc_buf_appendf(&path, err, "[%zu]", f->index++);
                }
            }
            if (s != PC_OK) {
                goto done;
            }
            if (v == NULL) {
                depth--;
            }
        }
        if (v == NULL) {
            break;
        }
    }
done:
    free(stack);
    pc_buf_fini(&path);
    return s;
}
