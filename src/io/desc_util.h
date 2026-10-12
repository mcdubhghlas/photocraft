/* Small helpers shared by chunk I2's files (blocks.c, adjust_map.c, effects_map.c, psd_import.c):
 * Rust's f32::total_cmp, a stable index sort (Rust's sort_by), descriptor value constructors and
 * text accessors. Internal to src/io; everything is static inline. Thread-safe. */
#ifndef PC_IO_DESC_UTIL_H
#define PC_IO_DESC_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/mem.h"
#include "base/str.h"
#include "base/utf8.h"
#include "psd/descriptor.h"

/* f32::total_cmp: -NaN < -inf < ... < -0 < +0 < ... < +inf < +NaN. */
static inline int pc_io_total_cmp_f32(float a, float b)
{
    int32_t ia;
    int32_t ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    ia ^= (int32_t)((uint32_t)(ia >> 31) >> 1);
    ib ^= (int32_t)((uint32_t)(ib >> 31) >> 1);
    return ia < ib ? -1 : ia > ib ? 1 : 0;
}

/* Stable sort of idx[0..n) by key[idx[i]] (total order), as Rust's slice::sort_by: a merge sort,
 * so equal keys keep their order and large inputs stay O(n log n). false on out of memory (idx
 * unchanged). */
static inline bool pc_io_sort_idx_f32(size_t *idx, size_t n, const float *key)
{
    size_t *tmp;
    if (n < 2) {
        return true;
    }
    tmp = pc_malloc_array(n, sizeof *tmp);
    if (tmp == NULL) {
        return false;
    }
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo;
            size_t j = mid;
            size_t k = lo;
            while (i < mid && j < hi) {
                tmp[k++] = pc_io_total_cmp_f32(key[idx[j]], key[idx[i]]) < 0 ? idx[j++] : idx[i++];
            }
            while (i < mid) {
                tmp[k++] = idx[i++];
            }
            while (j < hi) {
                tmp[k++] = idx[j++];
            }
        }
        memcpy(idx, tmp, n * sizeof *idx);
        if (width > SIZE_MAX / 4) {
            break;
        }
    }
    free(tmp);
    return true;
}

/* UnicodeString::to_string_lossy (trailing NULs stripped, lone surrogates -> U+FFFD), keeping
 * interior U+0000. *out replaced on success. */
static inline pc_status pc_io_ustr_lossy(const pc_psd_ustr *s, pc_str *out, pc_error *err)
{
    size_t end = s->n;
    pc_buf be = PC_BUF_INIT;
    pc_buf utf8 = PC_BUF_INIT;
    pc_status st = PC_OK;
    while (end > 0 && s->u[end - 1] == 0) {
        end--;
    }
    if (end > 0) {
        st = pc_buf_reserve(&be, end * 2, err);
        for (size_t i = 0; i < end && st == PC_OK; i++) {
            uint8_t b2[2] = {(uint8_t)(s->u[i] >> 8), (uint8_t)(s->u[i] & 0xff)};
            st = pc_buf_append(&be, b2, 2, err);
        }
        if (st == PC_OK) {
            st = pc_utf16be_lossy(be.data, end, &utf8, err);
        }
    }
    if (st == PC_OK) {
        st = pc_str_set(out, (const char *)utf8.data, utf8.len, err);
    }
    pc_buf_fini(&be);
    pc_buf_fini(&utf8);
    return st;
}

/* The Text value under key, lossy; *found false (out untouched) when absent or not text. */
static inline pc_status pc_io_desc_text(const pc_psd_desc *d, const char *key, bool *found, pc_str *out,
                                        pc_error *err)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, key) : NULL;
    if (v == NULL || v->kind != PC_PSD_V_TEXT) {
        *found = false;
        return PC_OK;
    }
    *found = true;
    return pc_io_ustr_lossy(&v->text, out, err);
}

/* Value::UnitFloat { unit, value }. */
static inline pc_psd_value pc_io_unit_float(const char unit[4], double v)
{
    pc_psd_value x;
    memset(&x, 0, sizeof x);
    x.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(x.unit, unit, 4);
    x.d = v;
    return x;
}

/* Value::Text(UnicodeString::new_nul(s)) for a length-carrying string. */
static inline pc_status pc_io_text_value(const char *s, size_t len, pc_psd_value *out, pc_error *err)
{
    pc_psd_value x;
    memset(&x, 0, sizeof x);
    x.kind = PC_PSD_V_TEXT;
    {
        pc_status st = pc_psd_ustr_from_utf8n(s != NULL ? s : "", len, true, &x.text, err);
        if (st != PC_OK) {
            return st;
        }
    }
    *out = x;
    return PC_OK;
}

#endif
