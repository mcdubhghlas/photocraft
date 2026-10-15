/* Persistent brush-preset store (port of a subset of crates/engine/src/preset_store/tests.rs, E2b).
 * Covers the tip codec, file-name safety, the directory round trip (save -> reopen), on-demand tip
 * loading and graceful handling of corrupt/oversized files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/mem.h"
#include "base/str.h"
#include "engine/brush_cmds.h"
#include "engine/engine_internal.h"
#include "engine/preset_store.h"
#include "harness.h"

/* ---- tiles ---- */
static pc_paint_gray_tile tile8(uint32_t w, uint32_t h, uint32_t seed)
{
    size_t n = (size_t)w * h;
    uint16_t *v = pc_malloc_array(n, sizeof *v);
    for (size_t i = 0; i < n; i++) {
        uint32_t x = (uint32_t)(i % w), y = (uint32_t)(i / w);
        v[i] = (uint16_t)(((x * 7 + y * 13 + seed) % 256u) * 257u); /* multiples of 257 -> 8-bit */
    }
    pc_paint_gray_tile t = PC_PAINT_GRAY_TILE_INIT;
    CHECK(pc_paint_gray_tile_from_u16(w, h, v, n, &t, NULL) == PC_OK);
    free(v);
    return t;
}
static pc_paint_gray_tile tile16(uint32_t w, uint32_t h, uint32_t seed)
{
    size_t n = (size_t)w * h;
    uint16_t *v = pc_malloc_array(n, sizeof *v);
    for (size_t i = 0; i < n; i++) {
        v[i] = (uint16_t)(((uint32_t)i * 2654435761u + seed) >> 16);
    }
    pc_paint_gray_tile t = PC_PAINT_GRAY_TILE_INIT;
    CHECK(pc_paint_gray_tile_from_u16(w, h, v, n, &t, NULL) == PC_OK);
    free(v);
    return t;
}

static void add_user_preset(pc_engine_session *s, const char *name, const char *group,
                            const pc_paint_gray_tile *tile)
{
    pc_paint_preset p;
    memset(&p, 0, sizeof p);
    CHECK(pc_str_set_c(&p.name, name, NULL) == PC_OK);
    pc_paint_brush_default(&p.brush);
    p.brush.size = 42.0f;
    pc_paint_tip_fini(&p.brush.tip);
    memset(&p.brush.tip, 0, sizeof p.brush.tip);
    p.brush.tip.kind = PC_PAINT_TIP_SAMPLED;
    pc_paint_gray_tile_copy(&p.brush.tip.tile, tile);
    p.builtin = false;
    CHECK(pc_str_set_c(&p.group, group, NULL) == PC_OK);
    pc_paint_preset_captures_default(&p.captures);
    pc_engine_tool_state *t = &s->tools;
    pc_paint_preset *grown = pc_malloc_array(t->npresets + 1, sizeof *grown);
    CHECK(grown != NULL);
    if (t->npresets > 0) {
        memcpy(grown, t->presets, t->npresets * sizeof *grown);
    }
    free(t->presets);
    t->presets = grown;
    t->presets[t->npresets++] = p;
}

/* ---- temp directory ---- */
static void tmp_dir(char *out, size_t cap, const char *tag)
{
    static unsigned ctr = 0;
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    snprintf(out, cap, "%s/pc-preset-%d-%s-%u", base, (int)getpid(), tag, ctr++);
    pc_mkdir_p(out, NULL);
}

static void rmrf(const char *dir)
{
    pc_preset_backend *be = pc_preset_dir_backend_new(dir);
    if (be != NULL) {
        pc_preset_file *files = NULL;
        size_t n = 0;
        if (be->list(be, &files, &n, NULL) == PC_OK) {
            for (size_t i = 0; i < n; i++) {
                be->remove(be, files[i].name, NULL);
                free(files[i].name);
            }
            free(files);
        }
        be->destroy(be);
    }
    pc_buf sub = PC_BUF_INIT;
    pc_buf_appendf(&sub, NULL, "%s/%s", dir, PC_PRESET_TIPS_DIR);
    rmdir(pc_buf_cstr(&sub));
    pc_buf_fini(&sub);
    rmdir(dir);
}

static size_t count_files(const char *dir, const char *suffix_or_prefix, bool prefix)
{
    pc_preset_backend *be = pc_preset_dir_backend_new(dir);
    pc_preset_file *files = NULL;
    size_t n = 0, got = 0;
    if (be != NULL && be->list(be, &files, &n, NULL) == PC_OK) {
        for (size_t i = 0; i < n; i++) {
            size_t fl = strlen(files[i].name), sl = strlen(suffix_or_prefix);
            bool match = prefix ? (fl >= sl && strncmp(files[i].name, suffix_or_prefix, sl) == 0)
                                : (fl >= sl && strcmp(files[i].name + fl - sl, suffix_or_prefix) == 0);
            got += match ? 1 : 0;
            free(files[i].name);
        }
        free(files);
    }
    if (be != NULL) {
        be->destroy(be);
    }
    return got;
}

/* ============================================================ tests */

static void test_tip_codec_round_trips(void)
{
    pc_paint_gray_tile t8 = tile8(37, 21, 3), t16 = tile16(40, 33, 9);
    pc_buf e8 = PC_BUF_INIT, e16 = PC_BUF_INIT;
    CHECK(pc_preset_encode_tip(&t8, &e8, NULL) == PC_OK);
    CHECK(pc_preset_encode_tip(&t16, &e16, NULL) == PC_OK);
    CHECK_EQ_INT(e8.data[6], 8);
    CHECK_EQ_INT(e16.data[6], 16);
    pc_paint_gray_tile d8 = PC_PAINT_GRAY_TILE_INIT, d16 = PC_PAINT_GRAY_TILE_INIT;
    CHECK(pc_preset_decode_tip(e8.data, e8.len, &d8, NULL) == PC_OK);
    CHECK(pc_preset_decode_tip(e16.data, e16.len, &d16, NULL) == PC_OK);
    CHECK(pc_paint_gray_tile_eq(&d8, &t8));
    CHECK(pc_paint_gray_tile_eq(&d16, &t16));
    pc_paint_gray_tile_fini(&t8);
    pc_paint_gray_tile_fini(&t16);
    pc_paint_gray_tile_fini(&d8);
    pc_paint_gray_tile_fini(&d16);
    pc_buf_fini(&e8);
    pc_buf_fini(&e16);
}

static void test_decoder_rejects_garbage(void)
{
    pc_paint_gray_tile t = tile16(16, 16, 1);
    pc_buf good = PC_BUF_INIT;
    CHECK(pc_preset_encode_tip(&t, &good, NULL) == PC_OK);
    pc_paint_gray_tile_fini(&t);
    /* Every truncation is rejected, never a crash. */
    for (size_t k = 0; k < good.len; k++) {
        pc_paint_gray_tile o = PC_PAINT_GRAY_TILE_INIT;
        CHECK(pc_preset_decode_tip(good.data, k, &o, NULL) != PC_OK);
        pc_paint_gray_tile_fini(&o);
    }
    /* Bad depth, and a header claiming a huge bitmap. */
    uint8_t *bad = pc_malloc_array(good.len, 1);
    memcpy(bad, good.data, good.len);
    bad[6] = 12;
    pc_paint_gray_tile o = PC_PAINT_GRAY_TILE_INIT;
    CHECK(pc_preset_decode_tip(bad, good.len, &o, NULL) != PC_OK);
    pc_paint_gray_tile_fini(&o);
    memcpy(bad, good.data, good.len);
    memset(bad + 7, 0xff, 4); /* width = u32::MAX */
    CHECK(pc_preset_decode_tip(bad, good.len, &o, NULL) != PC_OK);
    pc_paint_gray_tile_fini(&o);
    free(bad);
    /* Random fuzz (raw, and magic + random): no crash. */
    uint32_t rng = 0x12345678u;
    for (int it = 0; it < 200; it++) {
        size_t len = rng % 64;
        uint8_t buf[64 + 6];
        for (size_t i = 0; i < len; i++) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            buf[i] = (uint8_t)rng;
        }
        pc_paint_gray_tile a = PC_PAINT_GRAY_TILE_INIT;
        (void)pc_preset_decode_tip(buf, len, &a, NULL);
        pc_paint_gray_tile_fini(&a);
        uint8_t magicbuf[64 + 6];
        memcpy(magicbuf, "PCTIP1", 6);
        memcpy(magicbuf + 6, buf, len);
        pc_paint_gray_tile b = PC_PAINT_GRAY_TILE_INIT;
        (void)pc_preset_decode_tip(magicbuf, len + 6, &b, NULL);
        pc_paint_gray_tile_fini(&b);
    }
    pc_buf_fini(&good);
}

static void test_names(void)
{
    pc_preset_backend *mem = pc_preset_mem_backend_new();
    const char *bad[] = {"../x.pcbrushes", "a/b.pcbrushes", "tips/../x", ".hidden", "", "tips/"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(mem->write(mem, bad[i], (const uint8_t *)"x", 1, NULL) != PC_OK);
    }
    mem->destroy(mem);
    pc_buf a = PC_BUF_INIT, b = PC_BUF_INIT, c = PC_BUF_INIT;
    CHECK(pc_preset_group_file_name("../../etc/passwd", &a, NULL) == PC_OK);
    CHECK(pc_preset_valid_name(pc_buf_cstr(&a)));
    CHECK(pc_preset_group_file_name("", &b, NULL) == PC_OK);
    CHECK(pc_preset_valid_name(pc_buf_cstr(&b)));
    CHECK(pc_preset_group_file_name("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", &c, NULL) == PC_OK); /* 日本語 */
    CHECK(pc_preset_valid_name(pc_buf_cstr(&c)));
    pc_buf x = PC_BUF_INIT, y = PC_BUF_INIT;
    pc_preset_group_file_name("a/b", &x, NULL);
    pc_preset_group_file_name("a_b", &y, NULL);
    CHECK(strcmp(pc_buf_cstr(&x), pc_buf_cstr(&y)) != 0); /* different names never share a file */
    pc_buf_fini(&a);
    pc_buf_fini(&b);
    pc_buf_fini(&c);
    pc_buf_fini(&x);
    pc_buf_fini(&y);
}

static void test_dir_round_trip(void)
{
    char dir[256];
    tmp_dir(dir, sizeof dir, "roundtrip");

    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_engine_preset_opened opened;
    pc_engine_preset_store_open_dir(dir, &opened);
    pc_preset_strs warn = {0};
    CHECK(pc_engine_session_attach_preset_store(s, &opened, &warn, NULL) == PC_OK);
    CHECK_EQ_UINT(warn.len, 0u);
    pc_preset_strs_fini(&warn);
    /* Attaching an empty store writes nothing. */
    CHECK_EQ_UINT(count_files(dir, "." PC_PRESET_GROUP_EXT, false), 0u);

    pc_paint_gray_tile a = tile8(64, 48, 1), b = tile16(50, 70, 2);
    add_user_preset(s, "Eight", "Inks", &a);
    add_user_preset(s, "Sixteen", "Inks", &b);
    pc_paint_gray_tile_fini(&a);
    pc_paint_gray_tile_fini(&b);
    pc_engine_brush_presets_changed(s); /* bumps rev and syncs */

    CHECK_EQ_UINT(count_files(dir, "." PC_PRESET_GROUP_EXT, false), 1u);
    CHECK(count_files(dir, PC_PRESET_TIPS_DIR "/", true) >= 2u);
    /* The session keeps previews, not the full bitmaps (#1843). */
    const pc_paint_preset *ep = pc_paint_presets_find(s->tools.presets, s->tools.npresets, "Eight", 5);
    CHECK(ep != NULL && ep->brush.tip.kind == PC_PAINT_TIP_STORED && !ep->brush.tip.stored.loaded);

    /* Reopen from a fresh store on the same directory. */
    pc_engine_session *s2 = NULL;
    CHECK(pc_engine_session_new(&s2, NULL) == PC_OK);
    pc_engine_preset_opened op2;
    pc_engine_preset_store_open_dir(dir, &op2);
    pc_preset_strs warn2 = {0};
    CHECK(pc_engine_session_attach_preset_store(s2, &op2, &warn2, NULL) == PC_OK);
    CHECK_EQ_UINT(warn2.len, 0u);
    pc_preset_strs_fini(&warn2);
    const pc_paint_preset *r8 = pc_paint_presets_find(s2->tools.presets, s2->tools.npresets, "Eight", 5);
    const pc_paint_preset *r16 = pc_paint_presets_find(s2->tools.presets, s2->tools.npresets, "Sixteen", 7);
    CHECK(r8 != NULL && !r8->builtin && r8->brush.tip.kind == PC_PAINT_TIP_STORED);
    CHECK(r16 != NULL && !r16->builtin && r16->brush.tip.kind == PC_PAINT_TIP_STORED);

    /* Loading the stored tip gives back the exact original bitmap. */
    pc_paint_brush loaded;
    memset(&loaded, 0, sizeof loaded);
    pc_paint_brush_copy(&loaded, &r8->brush);
    CHECK(pc_engine_session_load_brush_tips(s2, &loaded, NULL) == PC_OK);
    CHECK(loaded.tip.kind == PC_PAINT_TIP_STORED && loaded.tip.stored.loaded);
    pc_paint_gray_tile orig = tile8(64, 48, 1);
    CHECK(pc_paint_gray_tile_eq(pc_paint_stored_tile_bitmap(&loaded.tip.stored), &orig));
    pc_paint_gray_tile_fini(&orig);
    pc_paint_brush_fini(&loaded);

    pc_engine_session_free(s);
    pc_engine_session_free(s2);
    rmrf(dir);
}

static void test_corrupt_files_skipped(void)
{
    char dir[256];
    tmp_dir(dir, sizeof dir, "corrupt");
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_engine_preset_opened opened;
    pc_engine_preset_store_open_dir(dir, &opened);
    pc_preset_strs warn = {0};
    CHECK(pc_engine_session_attach_preset_store(s, &opened, &warn, NULL) == PC_OK);
    pc_preset_strs_fini(&warn);
    pc_paint_gray_tile a = tile8(8, 8, 1);
    add_user_preset(s, "Good", "Fine", &a);
    pc_paint_gray_tile_fini(&a);
    pc_engine_brush_presets_changed(s);
    pc_engine_session_free(s);

    /* Write a junk group file and a truncated index into the directory. */
    pc_preset_backend *be = pc_preset_dir_backend_new(dir);
    CHECK(be->write(be, "junk-00000000." PC_PRESET_GROUP_EXT, (const uint8_t *)"{not json", 9, NULL) == PC_OK);
    be->destroy(be);

    /* Reopening skips the junk file with a warning and still loads "Good"; no crash. */
    pc_engine_session *s2 = NULL;
    CHECK(pc_engine_session_new(&s2, NULL) == PC_OK);
    pc_engine_preset_opened op2;
    pc_engine_preset_store_open_dir(dir, &op2);
    pc_preset_strs warn2 = {0};
    CHECK(pc_engine_session_attach_preset_store(s2, &op2, &warn2, NULL) == PC_OK);
    CHECK(warn2.len >= 1u);
    pc_preset_strs_fini(&warn2);
    CHECK(pc_paint_presets_find(s2->tools.presets, s2->tools.npresets, "Good", 4) != NULL);
    pc_engine_session_free(s2);
    rmrf(dir);
}

static void test_oversized_refused(void)
{
    /* An oversized group file is refused without reading it whole (MemBackend). */
    pc_preset_backend *mem = pc_preset_mem_backend_new();
    size_t big = (size_t)PC_PRESET_MAX_GROUP_BYTES + 1;
    uint8_t *blob = pc_malloc_array(big, 1);
    CHECK(blob != NULL);
    memset(blob, ' ', big);
    CHECK(mem->write(mem, "big-00000000." PC_PRESET_GROUP_EXT, blob, big, NULL) == PC_OK);
    free(blob);
    CHECK(mem->write(mem, PC_PRESET_INDEX_FILE, (const uint8_t *)"[1,2", 4, NULL) == PC_OK);
    pc_engine_preset_opened o;
    pc_engine_preset_store_open(mem, &o); /* takes ownership of mem */
    CHECK_EQ_UINT(o.presets.len, 0u);
    CHECK(o.warnings.len >= 2u);
    pc_engine_preset_opened_fini(&o);
}

static void test_headless_has_no_store(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(s->preset_store == NULL); /* headless sessions stay hermetic */
    pc_engine_brush_presets_changed(s);
    CHECK(s->preset_store == NULL);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_tip_codec_round_trips);
    RUN(test_decoder_rejects_garbage);
    RUN(test_names);
    RUN(test_dir_round_trip);
    RUN(test_corrupt_files_skipped);
    RUN(test_oversized_refused);
    RUN(test_headless_has_no_store);
    return pc_t_finish();
}
