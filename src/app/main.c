/* photocraft: the desktop application entry point (P1 wiring).
 *
 * Behavioural port of apps/photocraft/src/main.rs onto the SDL3 + OpenGL + Nuklear shell. It
 * initialises logging and the last-resort crash guard, resolves the settings directory, attaches the
 * log file, runs the Linux library preflight, drives the crash-safe GPU startup sentinel, constructs
 * the PhotocraftApp, restores/persists UI state, optionally starts the control server, and runs the
 * frame loop. The existing --snapshot and --open developer paths are kept.
 *
 * Usage: photocraft [--control <port>] [--control-token <64hex> | --control-token-file <path>]
 *                   [--safe-gpu] [--scale S] [--theme ID] [--font PATH] [--snapshot OUT.png]
 *                   [--open FILE | FILES...] [--version]
 *
 * The eframe/wgpu-specific machinery of the Rust main (adapter selection, the in-process CPU retry,
 * macOS Apple events, monitor/tablet/cursor services) is not part of P1 (REWRITE_PLAN.md §7; the OS
 * services are P2). */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "control.h"
#include "shell_internal.h"

#include "app/app_dirs.h"
#include "app/app_icon.h"
#include "app/control_server.h"
#include "app/crash_guard.h"
#include "app/gpu_startup.h"
#include "app/linux_libs.h"
#include "app/logging.h"
#include "app/ui_state.h"
#include "automation/security.h"
#include "base/buf.h"
#include "base/file.h"
#include "base/log.h"
#include "base/mem.h"
#include "base/num.h"
#include "codecs/codecs.h"
#include "codecs/image.h"
#include "engine/engine.h"
#include "io/io.h"
#include "ui/theme.h"

static int fail(const char *what, const char *detail)
{
    (void)fprintf(stderr, "photocraft: %s: %s\n", what, detail ? detail : "");
    return 1;
}

/* ---- the control-server wake event (so a queued request wakes the blocked frame loop) ---- */
static Uint32 g_wake_event = (Uint32)-1;

static void wake_loop(void *ud)
{
    (void)ud;
    if (g_wake_event != (Uint32)-1) {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = g_wake_event;
        (void)SDL_PushEvent(&e);
    }
}

static pc_status control_dispatch(void *ctx, const char *method, yyjson_val *params,
                                  pc_json_value *out, pc_error *err)
{
    return pc_ui_control_handle((pc_ui_shell_app *)ctx, method, params, out, err);
}

/* Parse a control port; returns false and names the value on error. */
static bool parse_port(const char *value, const char *source, uint16_t *out)
{
    if (!value) {
        (void)fprintf(stderr, "photocraft: %s: missing port value\n", source);
        return false;
    }
    char *end = NULL;
    long v = strtol(value, &end, 10);
    while (end && (*end == ' ' || *end == '\t')) {
        end++;
    }
    if (end == value || (end && *end != '\0') || v < 0 || v > 65535) {
        (void)fprintf(stderr, "photocraft: %s: `%s` is not a valid port (0..65535)\n", source, value);
        return false;
    }
    *out = (uint16_t)v;
    return true;
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

static int open_document(pc_ui_shell_app *a, const char *path)
{
    pc_buf bytes = PC_BUF_INIT;
    pc_io_import_result imported = {0};
    pc_error err;
    pc_status status = pc_file_read(path, PC_MAX_ALLOC, &bytes, &err);
    if (status == PC_OK) {
        status = pc_io_import(path, bytes.data, bytes.len, &imported, &err);
    }
    if (status == PC_OK) {
        size_t index;
        status = pc_engine_session_add_document(pc_ui_shell_app_session(a), &imported.document, path,
                                                &index, &err);
    }
    if (status == PC_OK) {
        pc_engine_session_active_mut(pc_ui_shell_app_session(a))->source_read_only =
            imported.source_read_only;
    }
    pc_buf_fini(&bytes);
    pc_io_import_result_fini(&imported);
    return status == PC_OK ? 0 : fail("open", err.msg);
}

/* The interactive frame loop (mirrors pc_ui_shell_app_run, plus the control-server pump). */
static int run_loop(pc_ui_shell_app *a, pc_app_control_server *server)
{
    SDL_Window *win = pc_ui_shell_app_sdl_window(a);
    struct nk_context *nk = pc_ui_shell_app_nk(a);
    if (!win || !nk) {
        return fail("run", "no window");
    }
    while (!pc_ui_shell_exit_requested(a)) {
        SDL_Event e;
        int fw, fh;
        pc_error err;
        nk_input_begin(nk);
        if (!SDL_WaitEventTimeout(&e, 16)) {
            e.type = 0;
        }
        do {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                pc_json_value result = PC_JSON_VALUE_INIT;
                (void)pc_ui_shell_app_invoke(a, "file.exit", NULL, &result, NULL);
                pc_json_value_fini(&result);
            } else if (e.type != 0 && e.type != g_wake_event) {
                pc_ui_shell_input_event(a, &e);
            }
        } while (!pc_ui_shell_exit_requested(a) && SDL_PollEvent(&e));
        nk_input_end(nk);
        if (server) {
            (void)pc_app_control_server_pump(server);
        }
        if (pc_ui_shell_exit_requested(a)) {
            break;
        }
        if (!SDL_GetWindowSizeInPixels(win, &fw, &fh) || fw <= 0 || fh <= 0) {
            continue;
        }
        if (pc_ui_shell_app_render_frame(a, fw, fh, &err) != PC_OK) {
            return fail("render", err.msg);
        }
        SDL_GL_SwapWindow(win);
    }
    return 0;
}

static void set_window_icon(pc_ui_shell_app *a)
{
    SDL_Window *win = pc_ui_shell_app_sdl_window(a);
    if (!win) {
        return;
    }
    uint8_t *rgba = NULL;
    uint32_t w = 0, h = 0;
    pc_error err;
    if (pc_app_window_icon_rgba(&rgba, &w, &h, &err) != PC_OK) {
        return;
    }
    SDL_Surface *surf =
        SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_RGBA32, rgba, (int)(w * 4));
    if (surf) {
        (void)SDL_SetWindowIcon(win, surf);
        SDL_DestroySurface(surf);
    }
    free(rgba);
}

int main(int argc, char **argv)
{
    pc_app_logging_install();
    pc_app_crash_install_hook();

    const char *snapshot = NULL, *font_path = "assets/fonts/Inter-Regular.ttf";
    const char *open_path = NULL, *control_token = NULL, *control_token_file = NULL;
    float scale = 0.0f;
    pc_ui_theme_kind theme = PC_UI_THEME_DEFAULT;
    bool set_theme = false, safe_gpu = false, have_control = false;
    uint16_t control_port = 0;

    const char *env_port = getenv("PHOTOCRAFT_CONTROL_PORT");
    if (env_port && env_port[0]) {
        if (!parse_port(env_port, "PHOTOCRAFT_CONTROL_PORT", &control_port)) {
            return 2;
        }
        have_control = true;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            snapshot = argv[++i];
        } else if (strcmp(argv[i], "--open") == 0 && i + 1 < argc) {
            open_path = argv[++i];
        } else if (strcmp(argv[i], "--control") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], "--control", &control_port)) {
                return 2;
            }
            have_control = true;
        } else if (strcmp(argv[i], "--control-token") == 0 && i + 1 < argc) {
            control_token = argv[++i];
        } else if (strcmp(argv[i], "--control-token-file") == 0 && i + 1 < argc) {
            control_token_file = argv[++i];
        } else if (strcmp(argv[i], "--safe-gpu") == 0) {
            safe_gpu = true;
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
        } else if (strcmp(argv[i], "--version") == 0) {
            char ver[256];
            (void)pc_engine_long_version(ver, sizeof ver);
            (void)printf("photocraft %s\n", ver);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            (void)fprintf(stderr,
                          "usage: %s [--control PORT] [--control-token HEX | --control-token-file "
                          "PATH] [--safe-gpu] [--scale S] [--theme ID] [--font PATH] [--snapshot "
                          "OUT.png] [--open FILE | FILES...] [--version]\n",
                          argv[0]);
            return 2;
        } else if (!open_path) {
            open_path = argv[i]; /* first positional path */
        }
    }

    /* The settings directory and the log file (after arguments, so --version leaves no file). */
    const char *config_dir = pc_app_config_dir();
    if (config_dir) {
        char logs[PC_APP_PATH_MAX];
        if ((size_t)snprintf(logs, sizeof logs, "%s/logs", config_dir) < sizeof logs) {
            char log_path[PC_APP_PATH_MAX];
            pc_error lerr;
            if (pc_app_logging_attach_dir(logs, log_path, sizeof log_path, &lerr) != PC_OK) {
                pc_log(PC_LOG_WARN, "no log file: %s", lerr.msg);
            } else {
                char ver[256];
                (void)pc_engine_long_version(ver, sizeof ver);
                pc_log(PC_LOG_INFO, "PhotoCraft %s, log file %s", ver, log_path);
            }
        }
    }

    /* Linux library preflight (skipped for the headless snapshot path). */
    if (!snapshot) {
        pc_display_session session = pc_linux_session_from_env(NULL, NULL);
        char msg[2048];
        if (pc_linux_preflight(session, msg, sizeof msg) != PC_OK) {
            (void)fputs(msg, stderr);
            return 1;
        }
    }

    /* Resolve the control token now, so a bad token fails before the window opens. */
    char token[65];
    if (have_control) {
        pc_error terr;
        if (pc_auto_server_token(control_token, control_token_file, token, &terr) != PC_OK) {
            return fail("control", terr.msg);
        }
        if (control_token_file) {
            (void)fprintf(stderr, "photocraft: control token file: %s\n", control_token_file);
        } else if (!control_token) {
            (void)fprintf(stderr, "photocraft: control token: %s\n", token);
        } else {
            (void)fprintf(stderr, "photocraft: using supplied control token\n");
        }
    }

    /* Crash-safe GPU startup: read the preference, detect a previous crash, record this attempt. */
    pc_app_gpu_sentinel sentinel;
    bool have_sentinel = false;
    pc_app_gpu_plan plan;
    memset(&plan, 0, sizeof plan);
    if (config_dir && !snapshot) {
        char prefs_path[PC_APP_PATH_MAX];
        pc_app_gpu_backend pref = PC_APP_GPU_AUTO;
        pc_app_render_mode mode = PC_APP_RENDER_AUTO;
        if ((size_t)snprintf(prefs_path, sizeof prefs_path, "%s/preferences.json", config_dir) <
            sizeof prefs_path) {
            pc_app_gpu_read_rendering_prefs(prefs_path, &pref, &mode);
        }
        pc_app_gpu_marker prev;
        bool created = false;
        pc_app_gpu_prev previous = pc_app_gpu_sentinel_begin(config_dir, &sentinel, &created, &prev);
        have_sentinel = created;
        const char *env_backend = getenv("WGPU_BACKEND");
        pc_app_gpu_plan_with_mode(pref, mode, previous == PC_APP_GPU_PREV_CRASHED ? &prev : NULL, env_backend,
                              safe_gpu, pc_app_gpu_os_current(), &plan);
        if (previous == PC_APP_GPU_PREV_CRASHED) {
            pc_log(PC_LOG_WARN, "the previous start didn't finish; %s",
                   plan.has_reason ? plan.reason : "");
        }
        if (have_sentinel) {
            pc_app_gpu_marker marker;
            memset(&marker, 0, sizeof marker);
            (void)snprintf(marker.backend, sizeof marker.backend, "%s",
                           plan.has_env ? "env" : pc_app_gpu_backend_name(plan.backend));
            (void)pc_engine_long_version(marker.version, sizeof marker.version);
            pc_error werr;
            (void)pc_app_gpu_sentinel_write(&sentinel, &marker, &werr);
        }
        pc_log(PC_LOG_INFO, "GPU startup: backend %s%s", pc_app_gpu_backend_name(plan.backend),
               plan.backend == PC_APP_GPU_CPU ? " (software/safe)" : "");
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        if (have_sentinel) {
            pc_app_gpu_sentinel_release(&sentinel); /* window init failed: next start tries safer */
        }
        return fail("SDL_Init", SDL_GetError());
    }
    g_wake_event = SDL_RegisterEvents(1);

    pc_ui_shell_app *a = NULL;
    pc_error err;
    int rc = 1;
    pc_app_control_server *server = NULL;
    if (pc_ui_shell_app_new(&a, &err) != PC_OK) {
        (void)fail("startup", err.msg);
        goto done;
    }
    /* If the startup GPU plan chose the software backend (--safe-gpu, a "cpu" rendering
     * preference, or a crash detected on the previous run), force the shell to composite on the
     * CPU: the canvas view will not create or use the GL compositor. */
    if (plan.backend == PC_APP_GPU_CPU) { /* zeroed plan is AUTO, so this is only the real plan */
        pc_ui_shell_app_set_force_cpu(a, true);
        pc_log(PC_LOG_INFO, "canvas: software compositing forced");
    }
    /* Restore the saved UI layout before the window opens. */
    pc_app_ui_state_load(pc_ui_shell_app_state(a), config_dir);
    if (open_path && open_document(a, open_path) != 0) {
        goto done;
    }
    if (set_theme) {
        char command[96];
        (void)snprintf(command, sizeof command, "window.theme.%s", pc_ui_theme_kind_id(theme));
        pc_json_value result = PC_JSON_VALUE_INIT;
        pc_status status = pc_ui_shell_app_invoke(a, command, NULL, &result, &err);
        pc_json_value_fini(&result);
        if (status != PC_OK) {
            (void)fail("theme", err.msg);
            goto done;
        }
    }
    if (pc_ui_shell_app_open_window(a, font_path, scale, snapshot != NULL, &err) != PC_OK) {
        if (have_sentinel) {
            pc_app_gpu_sentinel_release(&sentinel);
            have_sentinel = false;
        }
        (void)fail("window", err.msg);
        goto done;
    }
    /* The window (and its GL context) came up: the driver survived startup. */
    if (have_sentinel) {
        pc_app_gpu_sentinel_finish(&sentinel);
        have_sentinel = false;
    }

    if (snapshot) {
        rc = write_snapshot(a, snapshot);
        goto done;
    }

    set_window_icon(a);

    if (have_control) {
        uint16_t bound = 0;
        if (pc_app_control_server_start(control_port, token, control_dispatch, a, &server, &bound,
                                        &err) != PC_OK) {
            (void)fail("control server", err.msg);
            goto done;
        }
        pc_app_control_server_on_wake(server, wake_loop, NULL);
        (void)fprintf(stderr, "photocraft: control server listening on 127.0.0.1:%u\n", bound);
    }

    rc = run_loop(a, server);

    /* Persist the UI layout on a clean exit. */
    if (config_dir) {
        pc_error serr;
        (void)pc_app_ui_state_save(pc_ui_shell_app_state(a), config_dir, &serr);
    }

done:
    if (server) {
        pc_app_control_server_stop(server);
    }
    if (have_sentinel) {
        pc_app_gpu_sentinel_finish(&sentinel);
    }
    pc_ui_shell_app_free(a);
    pc_ui_shell_input_cleanup();
    SDL_Quit();
    return rc;
}
