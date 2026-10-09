/* DNG opcode lists (DNG 1.7 chapter 7), a port of crates/raw/src/opcodes.rs. Big-endian whatever
 * the file's byte order: a count, then per opcode its ID, version, flags, parameter size and
 * parameters. Only GainMap (ID 9, lens shading in OpcodeList2) is applied; others are reported. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "raw/internal.h"

static bool be32(const uint8_t *b, size_t n, size_t at, uint32_t *v)
{
    if (at > n || n - at < 4) {
        return false;
    }
    *v = (uint32_t)b[at] << 24 | (uint32_t)b[at + 1] << 16 | (uint32_t)b[at + 2] << 8 | b[at + 3];
    return true;
}

static bool be64f(const uint8_t *b, size_t n, size_t at, double *v)
{
    uint32_t hi;
    uint32_t lo;
    uint64_t u;
    double d;
    if (!be32(b, n, at, &hi) || !be32(b, n, at + 4, &lo)) {
        return false;
    }
    u = (uint64_t)hi << 32 | lo;
    memcpy(&d, &u, 8);
    if (!isfinite(d)) {
        return false;
    }
    *v = d;
    return true;
}

static bool has_exact(const pc_raw_strings *s, const char *v)
{
    size_t j;
    for (j = 0; j < s->n; j++) {
        if (strcmp(s->v[j], v) == 0) {
            return true;
        }
    }
    return false;
}

static void name(uint32_t id, char *out, size_t cap)
{
    static const char *const N[15] = {NULL,
                                      "WarpRectilinear",
                                      "WarpFisheye",
                                      "FixVignetteRadial",
                                      "FixBadPixelsConstant",
                                      "FixBadPixelsList",
                                      "TrimBounds",
                                      "MapTable",
                                      "MapPolynomial",
                                      "GainMap",
                                      "DeltaPerRow",
                                      "DeltaPerColumn",
                                      "ScalePerRow",
                                      "ScalePerColumn",
                                      "WarpRectilinear2"};
    if (id >= 1 && id <= 14) {
        (void)snprintf(out, cap, "%s", N[id]);
    } else {
        (void)snprintf(out, cap, "opcode %u", (unsigned)id);
    }
}

/* One GainMap opcode's parameters, or false (unusable). Sets *nomem on allocation failure. */
static bool gain_map(const uint8_t *p, size_t n, pc_raw_gain_map *out, bool *nomem)
{
    uint32_t u[19];
    pc_raw_gain_map m;
    size_t i;
    size_t cnt;
    static const int IDX[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 18};
    for (i = 0; i < 11; i++) {
        if (!be32(p, n, 4 * (size_t)IDX[i], &u[IDX[i]])) {
            return false;
        }
    }
    memset(&m, 0, sizeof m);
    m.top = u[0];
    m.left = u[1];
    m.bottom = u[2];
    m.right = u[3];
    m.plane = u[4];
    m.planes = u[5];
    m.row_pitch = u[6];
    m.col_pitch = u[7];
    m.points_v = u[8];
    m.points_h = u[9];
    if (!be64f(p, n, 40, &m.spacing_v) || !be64f(p, n, 48, &m.spacing_h) || !be64f(p, n, 56, &m.origin_v) ||
        !be64f(p, n, 64, &m.origin_h)) {
        return false;
    }
    m.map_planes = u[18];
    if ((m.points_h != 0 && m.points_v > SIZE_MAX / m.points_h) ||
        (m.map_planes != 0 && m.points_v * m.points_h > SIZE_MAX / m.map_planes)) {
        return false;
    }
    cnt = m.points_v * m.points_h * m.map_planes;
    if (cnt == 0 || cnt > ((size_t)1 << 20) || m.row_pitch == 0 || m.col_pitch == 0 || m.spacing_v <= 0.0 ||
        m.spacing_h <= 0.0 || m.bottom <= m.top || m.right <= m.left) {
        return false;
    }
    m.gains = pc_malloc_array(cnt, sizeof *m.gains);
    if (m.gains == NULL) {
        *nomem = true;
        return false;
    }
    for (i = 0; i < cnt; i++) {
        uint32_t bitsv;
        float g;
        if (!be32(p, n, 76 + 4 * i, &bitsv)) {
            free(m.gains);
            return false;
        }
        memcpy(&g, &bitsv, 4);
        if (!isfinite(g)) {
            free(m.gains);
            return false;
        }
        m.gains[i] = g;
    }
    m.ngains = cnt;
    *out = m;
    return true;
}

pc_status rx_opcodes_parse(const uint8_t *list, size_t n, pc_raw_gain_map **maps, size_t *nmaps,
                           pc_raw_strings *skipped, pc_error *err)
{
    uint32_t count;
    size_t at = 4;
    uint32_t k;
    pc_raw_gain_map *v = NULL;
    size_t nv = 0;
    pc_raw_strings sk = {NULL, 0};
    if (!be32(list, n, 0, &count)) {
        *maps = NULL;
        *nmaps = 0;
        *skipped = sk;
        return PC_OK;
    }
    for (k = 0; k < (count < 4096 ? count : 4096); k++) {
        uint32_t id;
        uint32_t size;
        size_t start;
        size_t end;
        pc_raw_gain_map m;
        bool nomem = false;
        if (!be32(list, n, at, &id) || !be32(list, n, at > SIZE_MAX - 12 ? SIZE_MAX : at + 12, &size)) {
            break;
        }
        start = at > SIZE_MAX - 16 ? SIZE_MAX : at + 16;
        end = start > SIZE_MAX - size ? SIZE_MAX : start + size;
        if (start > n || end > n) {
            break;
        }
        if (id == 9 && gain_map(list + start, size, &m, &nomem)) {
            pc_raw_gain_map *g = realloc(v, (nv + 1) * sizeof *g);
            if (g == NULL) {
                rx_gain_map_fini(&m);
                goto oom;
            }
            v = g;
            v[nv++] = m;
        } else {
            char nm[32];
            if (nomem) {
                goto oom;
            }
            name(id, nm, sizeof nm);
            if (!has_exact(&sk, nm)) {
                if (rx_warn(&sk, err, "%s", nm) != PC_OK) {
                    goto oom;
                }
            }
        }
        at = end;
    }
    *maps = v;
    *nmaps = nv;
    *skipped = sk;
    return PC_OK;
oom:
    while (nv > 0) {
        rx_gain_map_fini(&v[--nv]);
    }
    free(v);
    pc_raw_strings_fini(&sk);
    return rx_nomem(err);
}

void rx_gain_map_row(const pc_raw_gain_map *m, double rv, size_t mp, float *out)
{
    size_t mpc = m->map_planes > 0 ? m->map_planes - 1 : 0;
    double fv;
    size_t i0;
    size_t i1;
    float t;
    size_t j;
    if (mp > mpc) {
        mp = mpc;
    }
    fv = pc_clampd((rv - m->origin_v) / m->spacing_v, 0.0, (double)(m->points_v - 1));
    i0 = pc_f64_to_size(floor(fv));
    if (i0 > m->points_v - 1) {
        i0 = m->points_v - 1;
    }
    i1 = i0 + 1 < m->points_v - 1 ? i0 + 1 : m->points_v - 1;
    t = (float)(fv - (double)i0);
    for (j = 0; j < m->points_h; j++) {
        size_t ia = (i0 * m->points_h + j) * m->map_planes + mp;
        size_t ib = (i1 * m->points_h + j) * m->map_planes + mp;
        float a = ia < m->ngains ? m->gains[ia] : 1.0f;
        float b = ib < m->ngains ? m->gains[ib] : 1.0f;
        out[j] = a + (b - a) * t;
    }
}

float rx_gain_map_at(const pc_raw_gain_map *m, const float *row, double rh)
{
    double fh = pc_clampd((rh - m->origin_h) / m->spacing_h, 0.0, (double)(m->points_h - 1));
    size_t j0 = pc_f64_to_size(floor(fh));
    size_t j1;
    float t;
    float a;
    float b;
    if (j0 > m->points_h - 1) {
        j0 = m->points_h - 1;
    }
    j1 = j0 + 1 < m->points_h - 1 ? j0 + 1 : m->points_h - 1;
    t = (float)(fh - (double)j0);
    a = j0 < m->points_h ? row[j0] : 1.0f;
    b = j1 < m->points_h ? row[j1] : 1.0f;
    return a + (b - a) * t;
}

bool rx_gain_map_covers(const pc_raw_gain_map *m, size_t ax, size_t ay)
{
    return ay >= m->top && ay < m->bottom && ax >= m->left && ax < m->right && (ay - m->top) % m->row_pitch == 0 &&
           (ax - m->left) % m->col_pitch == 0;
}
