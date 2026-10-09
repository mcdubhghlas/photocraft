#include "png.h"

#include "base/buf.h"
#include "base/bytes.h"
#include "base/compress.h"
#include "base/file.h"
#include "base/num.h"

static pc_status chunk(pc_buf *out, const char type[4], const uint8_t *data, size_t len, pc_error *err)
{
    size_t start;
    pc_status s;
    if (len > 0x7FFFFFFFu) {
        return pc_fail(err, PC_ERR_LIMIT, "PNG chunk too large");
    }
    s = pc_buf_put_be32(out, (uint32_t)len, err);
    start = out->len;
    if (s == PC_OK) {
        s = pc_buf_append(out, type, 4, err);
    }
    if (s == PC_OK && len > 0) {
        s = pc_buf_append(out, data, len, err);
    }
    if (s == PC_OK) {
        s = pc_buf_put_be32(out, pc_crc32(0, out->data + start, out->len - start), err);
    }
    return s;
}

pc_status spike_png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t stride, pc_error *err)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    pc_buf raw = PC_BUF_INIT;
    pc_buf z = PC_BUF_INIT;
    pc_buf out = PC_BUF_INIT;
    uint8_t ihdr[13];
    size_t row;
    size_t total;
    uint32_t y;
    pc_status s;

    if (w == 0 || h == 0 || !pc_size_mul(w, 4, &row) || stride < row || !pc_size_mul(row + 1, h, &total)) {
        return pc_fail(err, PC_ERR_INVALID, "bad PNG dimensions %ux%u", (unsigned)w, (unsigned)h);
    }
    s = pc_buf_reserve(&raw, total, err);
    for (y = 0; s == PC_OK && y < h; y++) {
        s = pc_buf_append_byte(&raw, 0, err); /* filter: none */
        if (s == PC_OK) {
            s = pc_buf_append(&raw, rgba + (size_t)y * stride, row, err);
        }
    }
    if (s == PC_OK) {
        s = pc_deflate(PC_Z_ZLIB, PC_Z_DEFAULT, raw.data, raw.len, &z, err);
    }
    pc_store_be32(ihdr, w);
    pc_store_be32(ihdr + 4, h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* RGBA */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* adaptive filtering */
    ihdr[12] = 0; /* no interlace */
    if (s == PC_OK) {
        s = pc_buf_append(&out, sig, sizeof sig, err);
    }
    if (s == PC_OK) {
        s = chunk(&out, "IHDR", ihdr, sizeof ihdr, err);
    }
    if (s == PC_OK) {
        s = chunk(&out, "IDAT", z.data, z.len, err);
    }
    if (s == PC_OK) {
        s = chunk(&out, "IEND", NULL, 0, err);
    }
    if (s == PC_OK) {
        s = pc_file_write_atomic(path, out.data, out.len, err);
    }
    pc_buf_fini(&raw);
    pc_buf_fini(&z);
    pc_buf_fini(&out);
    return s;
}
