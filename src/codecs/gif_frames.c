/* Every frame of an animated GIF (port of crates/codecs/src/codecs/gif_frames.rs). pc_cdecode
 * keeps the first frame only; this walks the whole animation for callers that can hold it (video
 * layers). Each frame is composited onto the logical screen with the disposal methods applied
 * the way the reference's image crate does it, so every returned frame is a full picture. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "codecs/internal.h"

#define F PC_CF_GIF

void pc_canim_frames_free(pc_canim_frame *frames, size_t n)
{
    size_t i;
    for (i = 0; frames != NULL && i < n; i++) {
        pc_cimage_fini(&frames[i].image);
    }
    free(frames);
}

/* Draws pixel cur over prev (a transparent pixel shows what was there) and updates prev by the
 * disposal method: kept (0, 1), cleared (2), or left as it was (3). */
static void blend_and_dispose(int disposal, uint8_t *prev, uint8_t *cur)
{
    if (cur[3] == 0) {
        memcpy(cur, prev, 4);
    }
    if (disposal == 2) {
        memset(prev, 0, 4);
    } else if (disposal != 3) {
        memcpy(prev, cur, 4);
    }
}

pc_status pc_cdecode_gif_frames(const uint8_t *b, size_t n, const pc_cdecode_opts *o, pc_canim_frame **frames,
                                size_t *nframes, pc_error *err)
{
    const pc_climits *l = &o->limits;
    cx_gif g;
    uint8_t *prev = NULL;
    pc_canim_frame *out = NULL;
    size_t nout = 0;
    size_t cap = 0;
    bool damaged = false;
    uint64_t screen;
    uint64_t remaining;
    size_t px;
    pc_status st;
    *frames = NULL;
    *nframes = 0;
    if ((st = cx_gif_open(b, n, &g, err)) != PC_OK) {
        return st;
    }
    if (g.sw > l->max_width || g.sh > l->max_height) {
        return cx_limit(err, "Image size exceeds limit");
    }
    if ((st = cx_check_bytes(l, g.sw, g.sh, 4, err)) != PC_OK) {
        return st;
    }
    px = (size_t)g.sw * g.sh;
    screen = (uint64_t)px * 4;
    /* The frame kept between frames is the persistent allocation. */
    remaining = l->max_alloc - screen;
    prev = calloc(px * 4, 1);
    if (prev == NULL) {
        return cx_nomem(err);
    }
    for (;;) {
        cx_gif_frame f;
        bool done = false;
        bool full;
        uint8_t *img;
        pc_canim_frame *grow;
        pc_error e;
        size_t x;
        size_t y;
        st = cx_gif_next(&g, remaining, false, &f, &done, &e);
        if (st == PC_OK && done) {
            break;
        }
        full = st == PC_OK && f.left == 0 && f.top == 0 && f.w == g.sw && f.h == g.sh;
        if (st == PC_OK && !full && screen > remaining - (uint64_t)f.w * f.h * 4) {
            free(f.rgba);
            st = cx_limit(&e, "Memory limit exceeded");
        }
        if (st != PC_OK) {
            if (nout == 0) {
                if (err != NULL) {
                    *err = e;
                }
                free(prev);
                return st;
            }
            damaged = true;
            break;
        }
        if (nout >= PC_MAX_GIF_FRAMES) {
            free(f.rgba);
            st = cx_limit(err, "more than %d frames", PC_MAX_GIF_FRAMES);
            goto fail;
        }
        if (screen * (nout + 1) > l->max_alloc) {
            free(f.rgba);
            st = cx_limit(err, "%zu frames of %ux%u exceed max_alloc %llu", nout + 1, (unsigned)g.sw,
                          (unsigned)g.sh, (unsigned long long)l->max_alloc);
            goto fail;
        }
        if (full) {
            for (x = 0; x < px; x++) {
                blend_and_dispose(f.disposal, prev + 4 * x, f.rgba + 4 * x);
            }
            img = f.rgba;
        } else {
            img = malloc(px * 4);
            if (img == NULL) {
                free(f.rgba);
                st = cx_nomem(err);
                goto fail;
            }
            for (y = 0; y < g.sh; y++) {
                for (x = 0; x < g.sw; x++) {
                    size_t p = (y * g.sw + x) * 4;
                    uint32_t fx = (uint32_t)x - f.left;
                    uint32_t fy = (uint32_t)y - f.top;
                    if (fx < f.w && fy < f.h) {
                        uint8_t pixel[4];
                        memcpy(pixel, f.rgba + ((size_t)fy * f.w + fx) * 4, 4);
                        blend_and_dispose(f.disposal, prev + p, pixel);
                        memcpy(img + p, pixel, 4);
                    } else {
                        memcpy(img + p, prev + p, 4); /* outside the frame: the previous frame */
                    }
                }
            }
            free(f.rgba);
        }
        grow = pc_grow_array(out, &cap, nout + 1, sizeof *out);
        if (grow == NULL) {
            free(img);
            st = cx_nomem(err);
            goto fail;
        }
        out = grow;
        if ((st = pc_cimage_from_raw(g.sw, g.sh, PC_CL_RGBA, PC_CS_U8, img, px * 4, &out[nout].image, err)) !=
            PC_OK) {
            goto fail;
        }
        out[nout].delay_ms = (uint32_t)f.delay_cs * 10;
        nout++;
    }
    free(prev);
    if (damaged && nout > 0) {
        pc_cwarning w;
        memset(&w, 0, sizeof w);
        w.kind = PC_CW_TRUNCATED;
        w.format = F;
        if ((st = pc_cimage_add_warning(&out[0].image, w, err)) != PC_OK) {
            pc_canim_frames_free(out, nout);
            return st;
        }
    }
    *frames = out;
    *nframes = nout;
    return PC_OK;
fail:
    free(prev);
    pc_canim_frames_free(out, nout);
    return st;
}
