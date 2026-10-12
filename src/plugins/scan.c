/* Section walk and start-function rewrite for plug-in modules (see scan.h). */
#include "plugins/scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"

/* wasmi 2.0 EnforcedLimits::strict(). */
enum {
    MAX_GLOBALS = 1000,
    MAX_FUNCTIONS = 10000,
    MAX_TABLES = 100,
    MAX_ELEMENT_SEGMENTS = 1000,
    MAX_MEMORIES = 1,
    MAX_DATA_SEGMENTS = 1000,
    MAX_PARAMS = 32,
    MAX_RESULTS = 32,
    AVG_REQ_FUNCS_BYTES = 1000,
    MIN_AVG_BYTES_PER_FUNCTION = 40
};

typedef struct rd {
    const uint8_t *b;
    size_t pos, end;
    bool bad;
} rd;

static uint64_t leb_u(rd *r, unsigned max_bits)
{
    uint64_t v = 0;
    unsigned shift = 0;
    for (;;) {
        uint8_t byte;
        if (r->pos >= r->end || shift >= max_bits) {
            r->bad = true;
            return 0;
        }
        byte = r->b[r->pos++];
        v |= (uint64_t)(byte & 0x7Fu) << shift;
        shift += 7;
        if ((byte & 0x80u) == 0) {
            return v;
        }
    }
}

static uint32_t leb32(rd *r)
{
    return (uint32_t)leb_u(r, 35);
}

static uint8_t byte(rd *r)
{
    if (r->pos >= r->end) {
        r->bad = true;
        return 0;
    }
    return r->b[r->pos++];
}

static void skip(rd *r, size_t n)
{
    if (n > r->end - r->pos) {
        r->bad = true;
        r->pos = r->end;
        return;
    }
    r->pos += n;
}

/* A value type; reference types may carry a heap type (s33). */
static void valtype(rd *r)
{
    uint8_t t = byte(r);
    if (t == 0x63 || t == 0x64) {
        (void)leb_u(r, 35);
    }
}

static void set_limit(pc_wasm_scan *s, const char *msg)
{
    if (s->limit_msg[0] == '\0') {
        (void)snprintf(s->limit_msg, sizeof s->limit_msg, "%s", msg);
    }
}

static void scan_types(rd *r, pc_wasm_scan *s)
{
    uint32_t n = leb32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        uint32_t np, nr;
        if (byte(r) != 0x60) {
            return; /* GC types: the validator decides */
        }
        np = leb32(r);
        for (uint32_t k = 0; k < np && !r->bad; k++) {
            valtype(r);
        }
        nr = leb32(r);
        for (uint32_t k = 0; k < nr && !r->bad; k++) {
            valtype(r);
        }
        if (r->bad) {
            return;
        }
        if (np > MAX_PARAMS) {
            set_limit(s, "a function type exceeds the limit of 32 parameters");
            return;
        }
        if (nr > MAX_RESULTS) {
            set_limit(s, "a function type exceeds the limit of 32 results");
            return;
        }
    }
}

static void scan_import(rd *r, pc_wasm_scan *s)
{
    uint32_t n = leb32(r), ml, nl;
    if (n == 0 || r->bad) {
        return;
    }
    ml = leb32(r);
    s->imp_module_off = r->pos;
    s->imp_module_len = ml;
    skip(r, ml);
    nl = leb32(r);
    s->imp_name_off = r->pos;
    s->imp_name_len = nl;
    skip(r, nl);
    s->has_import = !r->bad;
}

static void scan_tables(rd *r, pc_wasm_scan *s)
{
    uint32_t n = leb32(r);
    if (r->bad) {
        return;
    }
    if (n > MAX_TABLES) {
        set_limit(s, "the Wasm module exceeds the limit of 100 tables");
    }
    s->table_count += n;
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        uint8_t flags;
        uint64_t min;
        if (r->pos < r->end && r->b[r->pos] == 0x40) {
            return; /* a table with an initialiser expression: the validator decides */
        }
        valtype(r);
        flags = byte(r);
        min = leb_u(r, (flags & 4u) != 0 ? 70 : 35);
        if ((flags & 1u) != 0) {
            (void)leb_u(r, (flags & 4u) != 0 ? 70 : 35);
        }
        if (!r->bad && min > s->table_max_min) {
            s->table_max_min = min;
        }
    }
}

static void scan_memories(rd *r, pc_wasm_scan *s)
{
    uint32_t n = leb32(r);
    if (r->bad) {
        return;
    }
    if (n > MAX_MEMORIES) {
        set_limit(s, "the Wasm module exceeds the limit of 1 memories");
    }
    if (n > 0 && s->memory_count == 0) {
        uint8_t flags = byte(r);
        uint64_t min = leb_u(r, (flags & 4u) != 0 ? 70 : 35);
        if (!r->bad) {
            s->memory_min_pages = min;
            s->memory64 = (flags & 4u) != 0;
        }
    }
    s->memory_count += n;
}

static void scan_count(rd *r, pc_wasm_scan *s, uint32_t limit, const char *msg)
{
    uint32_t n = leb32(r);
    if (!r->bad && n > limit) {
        set_limit(s, msg);
    }
}

static bool is_auto_export(const uint8_t *name, size_t len)
{
    return (len == 17 && memcmp(name, "__wasm_call_ctors", 17) == 0) ||
           (len == 18 && memcmp(name, "__post_instantiate", 18) == 0);
}

static void scan_exports(rd *r, pc_wasm_scan *s)
{
    uint32_t n = leb32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        uint32_t nl = leb32(r);
        size_t at = r->pos;
        skip(r, nl);
        (void)byte(r);
        (void)leb32(r);
        if (!r->bad && is_auto_export(r->b + at, nl)) {
            s->needs_rewrite = true;
        }
    }
}

/* Position of a known section id in the required order (0 = custom, -1 = unknown). */
static int section_rank(uint8_t id)
{
    static const int8_t RANK[14] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 11, 6};
    return id < 14 ? RANK[id] : -1;
}

void pc_wasm_scan_module(const uint8_t *b, size_t len, pc_wasm_scan *out)
{
    pc_wasm_scan s;
    size_t pos = 8;
    int last_rank = 0;
    memset(&s, 0, sizeof s);
    if (b == NULL || len < 8 || memcmp(b, "\0asm\x01\0\0\0", 8) != 0) {
        *out = s;
        return;
    }
    s.walked = true;
    while (pos < len) {
        rd h = {b, pos, len, false};
        uint8_t id = byte(&h);
        uint32_t size = leb32(&h);
        rd r;
        if (h.bad || size > len - h.pos) {
            s.walked = false;
            break;
        }
        /* Out of order, repeated or unknown sections: invalid (WAMR rejects them too; never
         * rewrite such a module). */
        if (section_rank(id) < 0 || (section_rank(id) > 0 && section_rank(id) <= last_rank)) {
            s.walked = false;
            break;
        }
        if (section_rank(id) > 0) {
            last_rank = section_rank(id);
        }
        r = (rd){b, h.pos, h.pos + size, false};
        switch (id) {
        case 1:
            scan_types(&r, &s);
            break;
        case 2:
            scan_import(&r, &s);
            break;
        case 3:
            scan_count(&r, &s, MAX_FUNCTIONS, "the Wasm modules exceeds the limit of 10000 functions");
            break;
        case 4:
            scan_tables(&r, &s);
            break;
        case 5:
            scan_memories(&r, &s);
            break;
        case 6:
            scan_count(&r, &s, MAX_GLOBALS, "the Wasm module exceeds the limit of 1000 global variables");
            break;
        case 7:
            s.has_export = true;
            s.export_off = pos;
            s.export_end = h.pos + size;
            s.export_payload = h.pos;
            scan_exports(&r, &s);
            break;
        case 8:
            s.start_func = leb32(&r);
            s.has_start = !r.bad;
            s.start_off = pos;
            s.start_end = h.pos + size;
            s.needs_rewrite = s.needs_rewrite || s.has_start;
            break;
        case 9:
            scan_count(&r, &s, MAX_ELEMENT_SEGMENTS,
                       "the Wasm module exceeds the limit of 1000 active element segments");
            break;
        case 10: {
            uint32_t n = leb32(&r);
            uint32_t rest = (uint32_t)(r.end - r.pos);
            if (r.bad) {
                break;
            }
            if (n > MAX_FUNCTIONS) {
                set_limit(&s, "the Wasm modules exceeds the limit of 10000 functions");
            } else if (rest >= AVG_REQ_FUNCS_BYTES && n > 0 && rest / n < MIN_AVG_BYTES_PER_FUNCTION) {
                char m[96];
                (void)snprintf(
                    m, sizeof m,
                    "the Wasm module failed to meet the minimum average bytes per function of 40: avg=%u",
                    (unsigned)(rest / n));
                set_limit(&s, m);
            }
            break;
        }
        case 11:
        case 12:
            scan_count(&r, &s, MAX_DATA_SEGMENTS,
                       "the Wasm module exceeds the limit of 1000 active data segments");
            break;
        default:
            break;
        }
        pos = h.pos + size;
    }
    *out = s;
}

/* ---- rewrite ---- */

static pc_status put_leb(pc_buf *o, uint64_t v, pc_error *err)
{
    do {
        uint8_t byte_ = (uint8_t)(v & 0x7Fu);
        v >>= 7;
        if (v != 0) {
            byte_ |= 0x80u;
        }
        if (pc_buf_append_byte(o, byte_, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    } while (v != 0);
    return PC_OK;
}

static pc_status put_name(pc_buf *o, const char *prefix, const uint8_t *name, size_t len, pc_error *err)
{
    size_t pl = strlen(prefix);
    if (put_leb(o, pl + len, err) != PC_OK || pc_buf_append(o, prefix, pl, err) != PC_OK ||
        pc_buf_append(o, name, len, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

/* The export section's payload with renames and the start export appended. false when the
 * section can't be parsed (the module is then handed over unchanged and WAMR rejects it). */
static bool new_exports(const uint8_t *b, const pc_wasm_scan *s, pc_buf *payload, pc_status *st,
                        pc_error *err)
{
    rd r = {b, s->export_payload, s->export_end, false};
    uint32_t n = s->has_export ? leb32(&r) : 0;
    pc_buf entries = PC_BUF_INIT;
    *st = PC_OK;
    for (uint32_t i = 0; i < n && !r.bad && *st == PC_OK; i++) {
        uint32_t nl = leb32(&r);
        size_t at = r.pos, tail;
        skip(&r, nl);
        tail = r.pos;
        (void)byte(&r);
        (void)leb32(&r);
        if (r.bad) {
            break;
        }
        *st = put_name(&entries, is_auto_export(b + at, nl) ? "\x01photocraft:" : "", b + at, nl, err);
        if (*st == PC_OK) {
            *st = pc_buf_append(&entries, b + tail, r.pos - tail, err);
        }
    }
    if (r.bad || (s->has_export && r.pos != r.end)) {
        pc_buf_fini(&entries);
        return false;
    }
    if (*st == PC_OK && s->has_start) {
        uint8_t start_kind = 0; /* func */
        *st = put_name(&entries, PC_WASM_START_EXPORT, NULL, 0, err);
        if (*st == PC_OK) {
            *st = pc_buf_append(&entries, &start_kind, 1, err);
        }
        if (*st == PC_OK) {
            *st = put_leb(&entries, s->start_func, err);
        }
        n++;
    }
    if (*st == PC_OK) {
        *st = put_leb(payload, n, err);
    }
    if (*st == PC_OK) {
        *st = pc_buf_append(payload, entries.data, entries.len, err);
    }
    pc_buf_fini(&entries);
    return *st == PC_OK;
}

pc_status pc_wasm_rewrite(const uint8_t *b, size_t len, const pc_wasm_scan *s, uint8_t **out, size_t *out_len,
                          pc_error *err)
{
    pc_buf o = PC_BUF_INIT, payload = PC_BUF_INIT;
    pc_status st = PC_OK;
    bool rewrite = s->walked && s->needs_rewrite;
    if (rewrite && !new_exports(b, s, &payload, &st, err)) {
        pc_buf_fini(&payload);
        if (st != PC_OK) {
            return st;
        }
        /* A start function must never reach WAMR, which would run it unmetered. */
        return pc_fail(err, PC_ERR_FORMAT, "malformed export section");
    }
    if (!s->walked && s->has_start) {
        return pc_fail(err, PC_ERR_FORMAT, "malformed module with a start section");
    }
    if (!rewrite) {
        st = pc_buf_append(&o, b, len, err);
    } else {
        /* The export section goes where it was, or where the start section was. */
        size_t sec_at = s->has_export ? s->export_off : s->start_off;
        size_t sec_end = s->has_export ? s->export_end : s->start_off;
        uint8_t id = 7;
        st = pc_buf_append(&o, b, sec_at, err);
        if (st == PC_OK) {
            st = pc_buf_append(&o, &id, 1, err);
        }
        if (st == PC_OK) {
            st = put_leb(&o, payload.len, err);
        }
        if (st == PC_OK) {
            st = pc_buf_append(&o, payload.data, payload.len, err);
        }
        if (s->has_start) {
            /* Copy what lies between, then skip the start section. */
            if (st == PC_OK && s->start_off > sec_end) {
                st = pc_buf_append(&o, b + sec_end, s->start_off - sec_end, err);
            }
            if (st == PC_OK) {
                st = pc_buf_append(&o, b + s->start_end, len - s->start_end, err);
            }
        } else if (st == PC_OK) {
            st = pc_buf_append(&o, b + sec_end, len - sec_end, err);
        }
    }
    pc_buf_fini(&payload);
    if (st != PC_OK) {
        pc_buf_fini(&o);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory preparing a plug-in module");
    }
    if (o.data == NULL) {
        o.data = malloc(1);
        if (o.data == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory preparing a plug-in module");
        }
    }
    *out_len = o.len;
    *out = pc_buf_take(&o, NULL);
    return PC_OK;
}
