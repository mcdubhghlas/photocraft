/* photocraft-ui-shell: the desktop frame loop (SDL3 + OpenGL 3.3 + Nuklear). Phase 9 (P1) wraps
 * the PhotocraftApp with OS services and the control server; until then this entry point runs the
 * app interactively or renders one offscreen snapshot. SDL input translation lives in input.c.
 *
 *   photocraft-ui-shell [--scale S] [--theme ID] [--font PATH]
 *   photocraft-ui-shell --snapshot OUT.png [--scale S] [--theme ID] [--font PATH]
 *
 * With SDL_VIDEO_DRIVER=offscreen the snapshot renders headless (D23), as the U1 tool does. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/mem.h"
#include "base/num.h"
#include "io/io.h"
#include "codecs/codecs.h"
#include "codecs/image.h"
#include "shell_internal.h"
#include "ui/state.h"
#include "ui/theme.h"

static int fail(const char *what, const char *detail)
{
    (void)fprintf(stderr, "photocraft-ui-shell: %s: %s\n", what, detail ? detail : "");
    return 1;
}

static int write_snapshot(pc_ui_shell_app *a, const char *out)
{
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    pc_error err;
    if (pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) != PC_OK) {
        return fail("snapshot", err.msg);
    }
    pc_cimage img;
    pc_status rc = pc_cimage_from_copy((uint32_t)w, (uint32_t)h, PC_CL_RGBA, PC_CS_U8, rgba,
                                       (size_t)w * (size_t)h * 4, &img, &err);
    free(rgba);
    if (rc != PC_OK) {
        return fail("snapshot", err.msg);
    }
    pc_buf png = PC_BUF_INIT;
    pc_cencode_opts opts = pc_cencode_opts_default();
    rc = pc_cencode(&img, PC_CF_PNG, &opts, &png, &err);
    pc_cimage_fini(&img);
    if (rc == PC_OK) {
        rc = pc_file_write_atomic(out, png.data, png.len, &err);
    }
    pc_buf_fini(&png);
    if (rc != PC_OK) {
        return fail("snapshot", err.msg);
    }
    (void)printf("wrote %s (%dx%d)\n", out, w, h);
    return 0;
}

int main(int argc, char **argv)
{
    const char *snapshot = NULL, *font_path = "assets/fonts/Inter-Regular.ttf";
    const char *open_path = NULL;
    float scale = 0.0f;
    pc_ui_theme_kind theme = PC_UI_THEME_DEFAULT;
    bool set_theme = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            snapshot = argv[++i];
        } else if (strcmp(argv[i], "--open") == 0 && i + 1 < argc) {
            open_path = argv[++i];
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = pc_f64_to_f32(SDL_strtod(argv[++i], NULL));
            if (!(scale >= 0.5f && scale <= 4.0f)) {
                return fail("--scale", "must be 0.5..4");
            }
        } else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            if (!pc_ui_theme_kind_from_name(argv[++i], &theme)) {
                return fail("--theme", "unknown theme");
            }
            set_theme = true;
        } else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            font_path = argv[++i];
        } else {
            (void)fprintf(
                stderr,
                "usage: %s [--snapshot OUT.png] [--scale S] [--theme ID] [--font PATH] [--open FILE]\n",
                argv[0]);
            return 2;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return fail("SDL_Init", SDL_GetError());
    }
    pc_ui_shell_app *a = NULL;
    pc_error err;
    int rc = 1;
    if (pc_ui_shell_app_new(&a, &err) != PC_OK) {
        (void)fail("startup", err.msg);
        goto done;
    }
    if (open_path) {
        pc_buf bytes = PC_BUF_INIT;
        pc_io_import_result imported = {0};
        pc_status status = pc_file_read(open_path, PC_MAX_ALLOC, &bytes, &err);
        if (status == PC_OK) {
            status = pc_io_import(open_path, bytes.data, bytes.len, &imported, &err);
        }
        if (status == PC_OK) {
            size_t index;
            status = pc_engine_session_add_document(pc_ui_shell_app_session(a), &imported.document, open_path,
                                                    &index, &err);
        }
        if (status == PC_OK) {
            pc_engine_session_active_mut(pc_ui_shell_app_session(a))->source_read_only =
                imported.source_read_only;
        }
        pc_buf_fini(&bytes);
        pc_io_import_result_fini(&imported);
        if (status != PC_OK) {
            (void)fail("open", err.msg);
            goto done;
        }
    }
    if (set_theme) {
        pc_ui_shell_app_state(a)->theme = theme;
        pc_ui_shell_app_apply_theme(a);
    }
    if (pc_ui_shell_app_open_window(a, font_path, scale, snapshot != NULL, &err) != PC_OK) {
        (void)fail("window", err.msg);
        goto done;
    }
    if (snapshot != NULL) {
        rc = write_snapshot(a, snapshot);
    } else {
        rc = pc_ui_shell_app_run(a);
    }
done:
    pc_ui_shell_app_free(a);
    pc_ui_shell_input_cleanup();
    SDL_Quit();
    return rc;
}
