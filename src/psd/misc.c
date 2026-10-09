#include "psd/misc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "psd/file.h"
#include "psd/internal.h"

/* ---- hdr ---- */

bool pc_psd_is_toning_data(const uint8_t *data, size_t len)
{
    return len >= 4 && memcmp(data, PC_PSD_HDR_TONING_KEY, 4) == 0;
}

static void put_f32(psd_w *w, float v)
{
    uint8_t b[4];
    pc_store_be32(b, pc_f32_to_bits(v));
    psd_put(w, b, 4);
}

pc_status pc_psd_default_toning_data(pc_psd_bytes *out, pc_error *err)
{
    static const uint16_t linear[7] = {'L', 'i', 'n', 'e', 'a', 'r', 0};
    static const uint8_t ones[2] = {1, 1};
    psd_w w;
    int i;
    psd_w_init(&w, err);
    /* hdrt, version 3 */
    psd_put(&w, PC_PSD_HDR_TONING_KEY, 4);
    psd_put_u32(&w, 3);
    put_f32(&w, 0.23f);
    psd_put_u32(&w, 2); /* toning method */
    psd_write_units(&w, linear, 7);
    /* Toning curve: two points, (0, 0) and (255, 255), both corners. */
    psd_put_u16(&w, 2);
    psd_put_u16(&w, 2);
    for (i = 0; i < 2; i++) {
        uint16_t v = i == 0 ? 0 : 255;
        psd_put_u16(&w, v); /* output */
        psd_put_u16(&w, v); /* input */
    }
    psd_put(&w, ones, 2);
    psd_put_zeros(&w, 8);
    put_f32(&w, 16.0f);
    psd_put_u32(&w, 1);
    psd_put_u32(&w, 0);
    put_f32(&w, 1.0f);
    /* hdra, version 6 */
    psd_put(&w, PC_PSD_HDR_EXTRA_KEY, 4);
    psd_put_u32(&w, 6);
    psd_put_u32(&w, 0);
    put_f32(&w, 20.0f);
    put_f32(&w, 30.0f);
    psd_put_u32(&w, 0);
    psd_put_u32(&w, 0);
    put_f32(&w, 1.0f);
    psd_put_zeros(&w, 6);
    return psd_w_finish(&w, out);
}

pc_status pc_psd_color_mode_data_for_depth(uint16_t depth, pc_psd_bytes *out, pc_error *err)
{
    if (depth == 32) {
        return pc_psd_default_toning_data(out, err);
    }
    out->data = NULL;
    out->len = 0;
    return PC_OK;
}

/* ---- metadata ---- */

pc_status pc_psd_meta_item_make(const char key[4], const void *data, size_t len, pc_psd_meta_item *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    memcpy(out->signature, "8BIM", 4);
    memcpy(out->key, key, 4);
    if (len % 2 == 1) {
        uint8_t *p = malloc(len + 1);
        if (p == NULL) {
            return psd_nomem(err);
        }
        memcpy(p, data, len);
        p[len] = 0;
        out->data.data = p;
        out->data.len = len + 1;
        return PC_OK;
    }
    return pc_psd_bytes_set(&out->data, data, len, err);
}

void pc_psd_meta_items_free(pc_psd_meta_item *items, size_t n)
{
    size_t i;
    for (i = 0; items != NULL && i < n; i++) {
        pc_psd_bytes_fini(&items[i].data);
    }
    free(items);
}

bool pc_psd_meta_item_eq(const pc_psd_meta_item *a, const pc_psd_meta_item *b)
{
    return memcmp(a->signature, b->signature, 4) == 0 && memcmp(a->key, b->key, 4) == 0 && a->copy == b->copy &&
           memcmp(a->reserved, b->reserved, 3) == 0 && pc_psd_bytes_eq(&a->data, &b->data);
}

pc_status pc_psd_parse_shmd(const uint8_t *data, size_t len, pc_psd_meta_item **items, size_t *n, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pc_psd_meta_item *v = NULL;
    uint32_t count;
    uint32_t i;
    size_t k = 0;
    pc_status st;
    CK(psd_u32(&r, &count, err));
    CK(psd_check_count(&r, count, 16, err));
    if (count > 0) {
        v = pc_calloc_array(count, sizeof *v);
        if (v == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
    }
    for (i = 0; i < count; i++) {
        pc_psd_meta_item *it = &v[k];
        uint8_t copy;
        const uint8_t *res;
        uint32_t dlen;
        CK(psd_arr4(&r, it->signature, err));
        CK(psd_arr4(&r, it->key, err));
        CK(psd_u8(&r, &copy, err));
        it->copy = copy != 0;
        CK(psd_bytes(&r, 3, &res, err));
        memcpy(it->reserved, res, 3);
        CK(psd_u32(&r, &dlen, err));
        CK(psd_take_bytes(&r, dlen, &it->data, err));
        k++;
    }
    if (!psd_empty(&r)) {
        st = psd_invalid(err, "trailing bytes after metadata items");
        goto fail;
    }
    *items = v;
    *n = k;
    return PC_OK;
fail:
    pc_psd_meta_items_free(v, k);
    return st;
}

pc_status pc_psd_write_shmd(const pc_psd_meta_item *items, size_t n, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    size_t i;
    psd_w_init(&w, err);
    psd_put_u32(&w, (uint32_t)n);
    for (i = 0; i < n; i++) {
        psd_put(&w, items[i].signature, 4);
        psd_put(&w, items[i].key, 4);
        psd_put_u8(&w, items[i].copy ? 1 : 0);
        psd_put(&w, items[i].reserved, 3);
        psd_put_u32(&w, (uint32_t)items[i].data.len);
        psd_put(&w, items[i].data.data, items[i].data.len);
    }
    return psd_w_finish(&w, out);
}

/* ---- path ---- */

double pc_psd_fixed_to_f64(int32_t v)
{
    return (double)v / 16777216.0;
}

int32_t pc_psd_f64_to_fixed(double v)
{
    return pc_f64_to_i32(pc_clampd(round(v * 16777216.0), -2147483648.0, 2147483647.0));
}

pc_status pc_psd_path_parse(const uint8_t *data, size_t len, pc_psd_path *out, pc_error *err)
{
    size_t n = len / PC_PSD_PATH_RECORD_LEN;
    size_t i;
    pc_psd_path_record *rec = NULL;
    if (n > 4000000) {
        return psd_limit(err, "path records");
    }
    if (n > 0) {
        rec = pc_malloc_array(n, sizeof *rec);
        if (rec == NULL) {
            return psd_nomem(err);
        }
    }
    for (i = 0; i < n; i++) {
        const uint8_t *c = data + i * PC_PSD_PATH_RECORD_LEN;
        rec[i].selector = pc_load_be16(c);
        memcpy(rec[i].data, c + 2, 24);
    }
    out->records = rec;
    out->n = n;
    return PC_OK;
}

pc_status pc_psd_path_to_bytes(const pc_psd_path *p, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    size_t i;
    psd_w_init(&w, err);
    for (i = 0; i < p->n; i++) {
        psd_put_u16(&w, p->records[i].selector);
        psd_put(&w, p->records[i].data, 24);
    }
    return psd_w_finish(&w, out);
}

void pc_psd_path_fini(pc_psd_path *p)
{
    if (p != NULL) {
        free(p->records);
        p->records = NULL;
        p->n = 0;
    }
}

bool pc_psd_path_initial_fill(const pc_psd_path *p)
{
    size_t i;
    for (i = 0; i < p->n; i++) {
        if (p->records[i].selector == PC_PSD_PATH_INITIAL_FILL) {
            return pc_load_be16(p->records[i].data) != 0;
        }
    }
    return false;
}

void pc_psd_subpaths_free(pc_psd_subpath *s, size_t n)
{
    size_t i;
    for (i = 0; s != NULL && i < n; i++) {
        free(s[i].knots);
    }
    free(s);
}

static bool is_length(uint16_t sel)
{
    return sel == PC_PSD_PATH_CLOSED_LENGTH || sel == PC_PSD_PATH_OPEN_LENGTH;
}

static bool is_knot(uint16_t sel)
{
    return sel == 1 || sel == 2 || sel == 4 || sel == 5;
}

pc_status pc_psd_path_subpaths(const pc_psd_path *p, pc_psd_subpath **out, size_t *n, pc_error *err)
{
    pc_psd_subpath *v = NULL;
    size_t count = 0;
    size_t remaining = 0;
    size_t i;
    /* At most one subpath per record. */
    if (p->n > 0) {
        v = pc_calloc_array(p->n, sizeof *v);
        if (v == NULL) {
            return psd_nomem(err);
        }
    }
    for (i = 0; i < p->n; i++) {
        const pc_psd_path_record *r = &p->records[i];
        if (is_length(r->selector)) {
            remaining = pc_load_be16(r->data);
            v[count].closed = r->selector == PC_PSD_PATH_CLOSED_LENGTH;
            {
                int32_t op = pc_load_be16(r->data + 2);
                v[count].operation = (int16_t)(op > 0x7FFF ? op - 0x10000 : op);
            }
            count++;
        } else if (is_knot(r->selector)) {
            bool closed = r->selector <= 2;
            pc_psd_subpath *s;
            pc_psd_knot k;
            pc_psd_knot *g;
            int j;
            if (remaining == 0 && (count == 0 || v[count - 1].closed != closed)) {
                v[count].closed = closed;
                v[count].operation = -1;
                count++;
            }
            remaining = remaining > 0 ? remaining - 1 : 0;
            k.linked = r->selector == 1 || r->selector == 4;
            for (j = 0; j < 3; j++) {
                double *pt = j == 0 ? k.pre : j == 1 ? k.anchor : k.post;
                pt[0] = pc_psd_fixed_to_f64(pc_u32_as_i32(pc_load_be32(r->data + j * 8 + 4)));
                pt[1] = pc_psd_fixed_to_f64(pc_u32_as_i32(pc_load_be32(r->data + j * 8)));
            }
            s = &v[count - 1];
            g = realloc(s->knots, (s->n + 1) * sizeof *g);
            if (g == NULL) {
                pc_psd_subpaths_free(v, count);
                return psd_nomem(err);
            }
            s->knots = g;
            s->knots[s->n++] = k;
        }
    }
    *out = v;
    *n = count;
    return PC_OK;
}

pc_status pc_psd_path_from_subpaths(const pc_psd_subpath *s, size_t n, bool initial_fill, pc_psd_path *out,
                                    pc_error *err)
{
    size_t total = 2;
    size_t i;
    size_t k;
    size_t at = 0;
    pc_psd_path_record *rec;
    for (i = 0; i < n; i++) {
        total += 1 + (s[i].n < 0xFFFF ? s[i].n : 0xFFFF);
    }
    rec = pc_calloc_array(total, sizeof *rec);
    if (rec == NULL) {
        return psd_nomem(err);
    }
    rec[at++].selector = PC_PSD_PATH_FILL_RULE;
    rec[at].selector = PC_PSD_PATH_INITIAL_FILL;
    pc_store_be16(rec[at++].data, initial_fill ? 1 : 0);
    for (i = 0; i < n; i++) {
        size_t nk = s[i].n < 0xFFFF ? s[i].n : 0xFFFF;
        rec[at].selector = s[i].closed ? PC_PSD_PATH_CLOSED_LENGTH : PC_PSD_PATH_OPEN_LENGTH;
        pc_store_be16(rec[at].data, (uint16_t)nk);
        pc_store_be16(rec[at].data + 2, (uint16_t)s[i].operation);
        pc_store_be16(rec[at].data + 4, 1); /* Photoshop writes 1 here (psd-tools default) */
        at++;
        for (k = 0; k < nk; k++) {
            const pc_psd_knot *kn = &s[i].knots[k];
            int j;
            rec[at].selector = s[i].closed ? (kn->linked ? PC_PSD_PATH_CLOSED_KNOT_LINKED : PC_PSD_PATH_CLOSED_KNOT_UNLINKED)
                                           : (kn->linked ? PC_PSD_PATH_OPEN_KNOT_LINKED : PC_PSD_PATH_OPEN_KNOT_UNLINKED);
            for (j = 0; j < 3; j++) {
                const double *pt = j == 0 ? kn->pre : j == 1 ? kn->anchor : kn->post;
                pc_store_be32(rec[at].data + j * 8, (uint32_t)pc_psd_f64_to_fixed(pt[1]));
                pc_store_be32(rec[at].data + j * 8 + 4, (uint32_t)pc_psd_f64_to_fixed(pt[0]));
            }
            at++;
        }
    }
    out->records = rec;
    out->n = total;
    return PC_OK;
}

pc_status pc_psd_vector_mask_parse(const uint8_t *data, size_t len, pc_psd_vector_mask *out, pc_error *err)
{
    pc_status st;
    if (len < 8) {
        return psd_eof(err, 0, 8 - len);
    }
    st = pc_psd_path_parse(data + 8, len - 8, &out->path, err);
    if (st == PC_OK) {
        out->version = pc_load_be32(data);
        out->flags = pc_load_be32(data + 4);
    }
    return st;
}

pc_status pc_psd_vector_mask_to_bytes(const pc_psd_vector_mask *m, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    size_t i;
    psd_w_init(&w, err);
    psd_put_u32(&w, m->version);
    psd_put_u32(&w, m->flags);
    for (i = 0; i < m->path.n; i++) {
        psd_put_u16(&w, m->path.records[i].selector);
        psd_put(&w, m->path.records[i].data, 24);
    }
    return psd_w_finish(&w, out);
}

/* ---- tree ---- */

void pc_psd_nodes_free(pc_psd_node *nodes, size_t n)
{
    size_t i;
    for (i = 0; nodes != NULL && i < n; i++) {
        pc_psd_nodes_free(nodes[i].children, nodes[i].n);
    }
    free(nodes);
}

size_t pc_psd_node_count(const pc_psd_node *node)
{
    size_t c = 1;
    size_t i;
    for (i = 0; i < node->n; i++) {
        c += pc_psd_node_count(&node->children[i]);
    }
    return c;
}

typedef struct frame {
    bool has_divider;
    size_t divider;
    pc_psd_node *children;
    size_t n;
    size_t cap;
} frame;

static pc_status frame_push(frame *f, const pc_psd_node *node, pc_error *err)
{
    if (f->n == f->cap) {
        pc_psd_node *g = pc_grow_array(f->children, &f->cap, f->n + 1, sizeof *g);
        if (g == NULL) {
            return psd_nomem(err);
        }
        f->children = g;
    }
    f->children[f->n++] = *node;
    return PC_OK;
}

pc_status pc_psd_layer_tree(const pc_psd_file *f, pc_psd_node **roots, size_t *n, pc_error *err)
{
    size_t nl;
    const pc_psd_layer *layers = pc_psd_file_layers(f, &nl);
    frame *stack;
    size_t depth = 1;
    size_t i;
    pc_status st = PC_OK;
    /* The stack is at most one frame per record (dividers) plus the root: no recursion here,
     * and node depth is bounded by the record count. */
    stack = pc_calloc_array(nl + 1, sizeof *stack);
    if (stack == NULL) {
        return psd_nomem(err);
    }
    for (i = 0; i < nl && st == PC_OK; i++) {
        uint32_t t = pc_psd_layer_section_type(&layers[i]);
        pc_psd_node node;
        memset(&node, 0, sizeof node);
        node.index = i;
        if (t == PC_PSD_SECTION_DIVIDER) {
            memset(&stack[depth], 0, sizeof stack[depth]);
            stack[depth].has_divider = true;
            stack[depth].divider = i;
            depth++;
        } else if (t == PC_PSD_SECTION_OPEN || t == PC_PSD_SECTION_CLOSED) {
            node.is_group = true;
            if (depth > 1) {
                depth--;
                node.has_divider = stack[depth].has_divider;
                node.divider = stack[depth].divider;
                node.children = stack[depth].children;
                node.n = stack[depth].n;
                memset(&stack[depth], 0, sizeof stack[depth]);
            }
            st = frame_push(&stack[depth - 1], &node, err);
            if (st != PC_OK) {
                pc_psd_nodes_free(node.children, node.n);
            }
        } else {
            st = frame_push(&stack[depth - 1], &node, err);
        }
    }
    /* Unclosed dividers: hoist their children to the enclosing level. */
    while (st == PC_OK && depth > 1) {
        frame *top = &stack[depth - 1];
        size_t k;
        for (k = 0; k < top->n && st == PC_OK; k++) {
            st = frame_push(&stack[depth - 2], &top->children[k], err);
            if (st == PC_OK) {
                memset(&top->children[k], 0, sizeof top->children[k]);
            }
        }
        if (st == PC_OK) {
            free(top->children);
            memset(top, 0, sizeof *top);
            depth--;
        }
    }
    if (st != PC_OK) {
        for (i = 0; i < depth; i++) {
            pc_psd_nodes_free(stack[i].children, stack[i].n);
        }
        free(stack);
        return st;
    }
    *roots = stack[0].children;
    *n = stack[0].n;
    free(stack);
    return PC_OK;
}
