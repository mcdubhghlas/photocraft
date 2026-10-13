/* The `phry` (preset hierarchy) section of v6+ brush files: Brushes panel folders (port of the
 * hierarchy part of crates/psd/src/abr.rs). The section is a versioned descriptor whose key
 * `hierarchy` lists objects read as a token stream: `Grup` { `Nm  ` name, `zuid` uuid } opens a
 * folder, `preset` {} is the next preset of `desc` (in order), `groupEnd` {} closes the current
 * folder. A damaged hierarchy never fails the parse: mismatches become warnings and the presets
 * they affect land at the top level. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/utf8.h"
#include "psd/abr.h"
#include "psd/internal.h"

/* Longest folder name kept (characters). */
#define MAX_FOLDER_NAME 255u
/* Most bytes of folder names held across all presets' paths; each name also counts the 24 bytes
 * of the reference's `String`, so the cut-off lands on the same preset. */
#define MAX_FOLDER_BYTES ((size_t)16 << 20)
#define STRING_SIZE 24u

void pc_psd_abr_folder_fini(pc_psd_abr_folder *f)
{
    size_t i;
    if (f == NULL) {
        return;
    }
    for (i = 0; i < f->n; i++) {
        pc_psd_str_fini(&f->names[i]);
    }
    free(f->names);
    f->names = NULL;
    f->n = 0;
}

void pc_psd_abr_folders_free(pc_psd_abr_folder *f, size_t n)
{
    size_t i;
    for (i = 0; f != NULL && i < n; i++) {
        pc_psd_abr_folder_fini(&f[i]);
    }
    free(f);
}

bool pc_psd_abr_folder_eq(const pc_psd_abr_folder *a, const pc_psd_abr_folder *b)
{
    size_t i;
    if (a->n != b->n) {
        return false;
    }
    for (i = 0; i < a->n; i++) {
        if (a->names[i].len != b->names[i].len ||
            memcmp(pc_psd_str_c(&a->names[i]), pc_psd_str_c(&b->names[i]), a->names[i].len) != 0) {
            return false;
        }
    }
    return true;
}

static pc_status path_clone(const pc_psd_str *names, size_t n, pc_psd_abr_folder *out, pc_error *err)
{
    size_t i;
    memset(out, 0, sizeof *out);
    if (n == 0) {
        return PC_OK;
    }
    out->names = pc_calloc_array(n, sizeof *out->names);
    if (out->names == NULL) {
        return psd_nomem(err);
    }
    for (i = 0; i < n; i++) {
        pc_status st = pc_psd_str_set(&out->names[i], names[i].s, names[i].len, err);
        if (st != PC_OK) {
            pc_psd_abr_folder_fini(out);
            return st;
        }
        out->n++;
    }
    return PC_OK;
}

/* A folder name: lossy text, trailing NULs dropped, trimmed, at most MAX_FOLDER_NAME characters;
 * "Group" when nothing is left. */
static pc_status folder_name(const pc_psd_desc *d, pc_psd_str *out, pc_error *err)
{
    const pc_psd_value *v = pc_psd_desc_get(d, "Nm  ");
    pc_psd_str full = {NULL, 0};
    const char *text = NULL;
    size_t start = 0;
    size_t len = 0;
    size_t end;
    size_t chars = 0;
    pc_status st;
    if (v != NULL && v->kind == PC_PSD_V_TEXT) {
        size_t n = v->text.n;
        while (n > 0 && v->text.u[n - 1] == 0) {
            n--;
        }
        st = psd_str_from_units(v->text.u, n, &full, err);
        if (st != PC_OK) {
            return st;
        }
        text = pc_psd_str_c(&full);
        len = pc_utf8_trim(text, full.len, &start);
    }
    /* Characters are counted on lead bytes (the text is valid UTF-8). */
    for (end = 0; end < len; end++) {
        if (((uint8_t)text[start + end] & 0xC0u) != 0x80u) {
            if (chars == MAX_FOLDER_NAME) {
                break;
            }
            chars++;
        }
    }
    st = end == 0 ? pc_psd_str_set(out, "Group", 5, err) : pc_psd_str_set(out, text + start, end, err);
    pc_psd_str_fini(&full);
    return st;
}

typedef enum { TOK_OTHER, TOK_GROUP, TOK_END, TOK_PRESET } tok_kind;

static tok_kind token(const pc_psd_value *v)
{
    if (v->kind != PC_PSD_V_DESCRIPTOR || v->desc == NULL) {
        return TOK_OTHER;
    }
    if (pc_psd_id_is(&v->desc->class_id, "Grup")) {
        return TOK_GROUP;
    }
    if (pc_psd_id_is(&v->desc->class_id, "groupEnd")) {
        return TOK_END;
    }
    return pc_psd_id_is(&v->desc->class_id, "preset") ? TOK_PRESET : TOK_OTHER;
}

/* Walks the token stream into one path per preset (`presets` of them). */
static pc_status walk(const pc_psd_value *items, size_t nitems, size_t presets, pc_psd_warnings *w,
                      pc_psd_abr_folder *paths, pc_error *err)
{
    pc_psd_str stack[PC_PSD_ABR_MAX_FOLDER_DEPTH];
    size_t depth = 0;
    size_t overflow = 0; /* folders opened past the depth limit: their groupEnds close nothing */
    size_t out = 0;
    size_t listed = 0;
    size_t bytes = 0;
    bool unbalanced = false;
    bool too_deep = false;
    bool too_big = false;
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < nitems && st == PC_OK; i++) {
        switch (token(&items[i])) {
        case TOK_GROUP:
            if (depth >= PC_PSD_ABR_MAX_FOLDER_DEPTH) {
                overflow++;
                too_deep = true;
                break;
            }
            memset(&stack[depth], 0, sizeof stack[depth]);
            st = folder_name(items[i].desc, &stack[depth], err);
            if (st == PC_OK) {
                depth++;
            }
            break;
        case TOK_END:
            if (overflow > 0) {
                overflow--;
            } else if (depth == 0) {
                unbalanced = true;
            } else {
                pc_psd_str_fini(&stack[--depth]);
            }
            break;
        case TOK_PRESET: {
            size_t size = 0;
            size_t k;
            listed = listed == SIZE_MAX ? listed : listed + 1;
            if (out >= presets) {
                break;
            }
            for (k = 0; k < depth; k++) {
                size_t one = stack[k].len + STRING_SIZE;
                size = size > SIZE_MAX - one ? SIZE_MAX : size + one;
            }
            if (!too_big && size <= MAX_FOLDER_BYTES - bytes) {
                bytes += size;
                st = path_clone(stack, depth, &paths[out], err);
            } else {
                too_big = true;
            }
            out++;
            break;
        }
        case TOK_OTHER:
            break;
        }
    }
    while (depth > 0) {
        pc_psd_str_fini(&stack[--depth]);
    }
    if (st == PC_OK && unbalanced) {
        st = psd_warn(w, err, "brush folders (phry): a folder end without a folder was ignored");
    }
    if (st == PC_OK && too_deep) {
        st = psd_warn(w, err, "brush folders nested deeper than %u levels were merged into their parent",
                      (unsigned)PC_PSD_ABR_MAX_FOLDER_DEPTH);
    }
    if (st == PC_OK && too_big) {
        st = psd_warn(w, err,
                      "brush folders (phry) too large; the remaining brushes were imported at the top level");
    }
    if (st == PC_OK && listed != presets) {
        st = psd_warn(
            w, err, "brush folders (phry) list %zu brushes but the file holds %zu; %s", listed, presets,
            listed > presets ? "the extra entries were ignored" : "the rest were imported at the top level");
    }
    return st;
}

pc_status psd_abr_read_phry(const uint8_t *data, size_t len, size_t presets, pc_psd_warnings *w,
                            pc_psd_abr_folder **out, size_t *nout, pc_error *err)
{
    pc_psd_desc vd;
    size_t used;
    pc_error e;
    const pc_psd_value *h;
    pc_psd_abr_folder *paths = NULL;
    bool any = false;
    size_t i;
    pc_status st = pc_psd_vdesc_parse_prefix(data, len, &vd, &used, &e);
    *out = NULL;
    *nout = 0;
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", e.msg);
    }
    if (st != PC_OK) {
        return psd_warn(w, err, "brush folders (phry) unreadable: %s; brushes imported without folders",
                        e.msg);
    }
    h = pc_psd_desc_get(&vd, "hierarchy");
    if (h == NULL || h->kind != PC_PSD_V_LIST) {
        st = psd_warn(w, err, "brush folders (phry) hold no hierarchy; brushes imported without folders");
        pc_psd_desc_fini(&vd);
        return st;
    }
    if (presets > 0) {
        paths = pc_calloc_array(presets, sizeof *paths);
        if (paths == NULL) {
            pc_psd_desc_fini(&vd);
            return psd_nomem(err);
        }
    }
    st = walk(h->list, h->nlist, presets, w, paths, err);
    pc_psd_desc_fini(&vd);
    for (i = 0; st == PC_OK && i < presets; i++) {
        any = any || paths[i].n > 0;
    }
    if (st != PC_OK || !any) {
        pc_psd_abr_folders_free(paths, presets);
        return st;
    }
    *out = paths;
    *nout = presets;
    return PC_OK;
}

/* ---- writing ---- */

static uint64_t fnv_path(const pc_psd_str *const *open, size_t n, uint64_t seq, uint64_t seed)
{
    uint64_t h = (uint64_t)0xcbf29ce484222325u ^ seed;
    size_t i;
    size_t k;
    for (i = 0; i < n; i++) {
        for (k = 0; k <= open[i]->len; k++) {
            uint8_t b = k < open[i]->len ? (uint8_t)open[i]->s[k] : 0;
            h ^= b;
            h *= (uint64_t)0x00000100000001b3u;
        }
    }
    for (k = 0; k < 8; k++) {
        h ^= (seq >> (8 * k)) & 0xffu;
        h *= (uint64_t)0x00000100000001b3u;
    }
    return h;
}

/* A stable uuid-shaped folder id (`zuid`) from the folder's path and position. */
static void folder_uuid(const pc_psd_str *const *open, size_t n, uint64_t seq, char out[40])
{
    uint64_t a = fnv_path(open, n, seq, 1);
    uint64_t b = fnv_path(open, n, seq, 2);
    (void)snprintf(out, 40, "%08" PRIx64 "-%04" PRIx64 "-%04" PRIx64 "-%04" PRIx64 "-%012" PRIx64, a >> 32,
                   (a >> 16) & 0xffffu, a & 0xffffu, b >> 48, b & (uint64_t)0xffffffffffffu);
}

static pc_status push_token(pc_psd_value *list, pc_psd_desc *d, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_object(d, &v, err);
    if (st == PC_OK) {
        st = pc_psd_value_list_push(list, &v, err);
    }
    return st;
}

static pc_status push_simple(pc_psd_value *list, const char *class_id, pc_error *err)
{
    pc_psd_desc d;
    pc_status st = pc_psd_desc_init(&d, class_id, err);
    return st == PC_OK ? push_token(list, &d, err) : st;
}

static pc_status push_group(pc_psd_value *list, const pc_psd_str *const *open, size_t n, uint64_t seq,
                            pc_error *err)
{
    char uuid[40];
    pc_psd_desc d;
    pc_psd_value v;
    pc_status st = pc_psd_desc_init(&d, "Grup", err);
    if (st != PC_OK) {
        return st;
    }
    folder_uuid(open, n, seq, uuid);
    st = pc_psd_value_text(pc_psd_str_c(open[n - 1]), true, &v, err);
    if (st == PC_OK) {
        st = pc_psd_desc_add(&d, "Nm  ", &v, err);
    }
    if (st == PC_OK) {
        st = pc_psd_value_text(uuid, true, &v, err);
    }
    if (st == PC_OK) {
        st = pc_psd_desc_add(&d, "zuid", &v, err);
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    return push_token(list, &d, err);
}

static bool str_eq(const pc_psd_str *a, const pc_psd_str *b)
{
    return a->len == b->len && memcmp(pc_psd_str_c(a), pc_psd_str_c(b), a->len) == 0;
}

pc_status psd_abr_phry_bytes(const pc_psd_abr_folder *folders, size_t n, pc_psd_bytes *out, pc_error *err)
{
    const pc_psd_str *open[PC_PSD_ABR_MAX_FOLDER_DEPTH];
    size_t nopen = 0;
    uint64_t seq = 0;
    pc_psd_value list;
    pc_psd_desc root;
    size_t i;
    pc_status st;
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    st = PC_OK;
    for (i = 0; i < n && st == PC_OK; i++) {
        size_t depth =
            folders[i].n < PC_PSD_ABR_MAX_FOLDER_DEPTH ? folders[i].n : PC_PSD_ABR_MAX_FOLDER_DEPTH;
        size_t common = 0;
        size_t k;
        while (common < nopen && common < depth && str_eq(open[common], &folders[i].names[common])) {
            common++;
        }
        while (st == PC_OK && nopen > common) {
            nopen--;
            st = push_simple(&list, "groupEnd", err);
        }
        for (k = common; st == PC_OK && k < depth; k++) {
            open[nopen++] = &folders[i].names[k];
            seq++;
            st = push_group(&list, open, nopen, seq, err);
        }
        if (st == PC_OK) {
            st = push_simple(&list, "preset", err);
        }
    }
    while (st == PC_OK && nopen > 0) {
        nopen--;
        st = push_simple(&list, "groupEnd", err);
    }
    if (st != PC_OK) {
        pc_psd_value_fini(&list);
        return st;
    }
    st = pc_psd_desc_init(&root, "null", err);
    if (st != PC_OK) {
        pc_psd_value_fini(&list);
        return st;
    }
    st = pc_psd_desc_add(&root, "hierarchy", &list, err);
    if (st == PC_OK) {
        st = pc_psd_vdesc_to_bytes(&root, out, err);
    }
    pc_psd_desc_fini(&root);
    return st;
}
