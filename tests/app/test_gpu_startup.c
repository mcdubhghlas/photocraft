/* gpu_startup: backend planning, marker round-trip, preference reading, the crash sentinel. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/gpu_startup.h"
#include "harness.h"

static char *make_tmp(void)
{
    char *base = getenv("TMPDIR");
    static char path[256];
    (void)snprintf(path, sizeof path, "%s/pc-gpu-XXXXXX", (base && base[0]) ? base : "/tmp");
    return mkdtemp(path);
}

static pc_app_gpu_marker crashed(const char *backend, const char *adapter_backend)
{
    pc_app_gpu_marker m;
    memset(&m, 0, sizeof m);
    (void)snprintf(m.backend, sizeof m.backend, "%s", backend);
    (void)snprintf(m.adapter, sizeof m.adapter, "Intel(R) UHD Graphics");
    (void)snprintf(m.adapter_backend, sizeof m.adapter_backend, "%s", adapter_backend);
    return m;
}

static void test_no_marker_uses_preference(void)
{
    pc_app_gpu_plan p;
    pc_app_gpu_compute_plan(PC_APP_GPU_AUTO, NULL, NULL, false, PC_APP_GPU_OS_OTHER, &p);
    CHECK_EQ_INT(p.backend, PC_APP_GPU_AUTO);
    CHECK(!p.remember);
    pc_app_gpu_compute_plan(PC_APP_GPU_GL, NULL, NULL, false, PC_APP_GPU_OS_OTHER, &p);
    CHECK_EQ_INT(p.backend, PC_APP_GPU_GL);
    /* A backend the platform lacks becomes auto. */
    pc_app_gpu_compute_plan(PC_APP_GPU_DX12, NULL, NULL, false, PC_APP_GPU_OS_OTHER, &p);
    CHECK_EQ_INT(p.backend, PC_APP_GPU_AUTO);
}

static void test_crash_moves_down_chain(void)
{
    pc_app_gpu_plan p;
    pc_app_gpu_marker m = crashed("auto", "vulkan");
    pc_app_gpu_compute_plan(PC_APP_GPU_AUTO, &m, NULL, false, PC_APP_GPU_OS_OTHER, &p);
    CHECK_EQ_INT(p.backend, PC_APP_GPU_GL); /* Linux: Vulkan -> GL */
    CHECK(p.remember);
    pc_app_gpu_marker g = crashed("gl", "gl");
    pc_app_gpu_compute_plan(PC_APP_GPU_GL, &g, NULL, false, PC_APP_GPU_OS_OTHER, &p);
    CHECK_EQ_INT(p.backend, PC_APP_GPU_CPU);
    /* The CPU path stays put and is not remembered again. */
    pc_app_gpu_marker c = crashed("cpu", "gl");
    pc_app_gpu_compute_plan(PC_APP_GPU_CPU, &c, NULL, false, PC_APP_GPU_OS_OTHER, &p);
    CHECK_EQ_INT(p.backend, PC_APP_GPU_CPU);
    CHECK(!p.remember);
}

static void test_safe_gpu_and_env(void)
{
    pc_app_gpu_plan p;
    pc_app_gpu_compute_plan(PC_APP_GPU_VULKAN, NULL, NULL, true, PC_APP_GPU_OS_OTHER, &p);
    CHECK_EQ_INT(p.backend, PC_APP_GPU_CPU);
    CHECK(!p.remember);
    pc_app_gpu_compute_plan(PC_APP_GPU_AUTO, NULL, "vulkan", false, PC_APP_GPU_OS_OTHER, &p);
    CHECK(p.has_env);
    CHECK_EQ_INT(p.backend, PC_APP_GPU_AUTO);
    /* Empty env is unset. */
    pc_app_gpu_compute_plan(PC_APP_GPU_AUTO, NULL, "   ", false, PC_APP_GPU_OS_OTHER, &p);
    CHECK(!p.has_env);
}

static void test_mode_overrides(void)
{
    pc_app_gpu_plan p;
    pc_app_gpu_plan_with_mode(PC_APP_GPU_VULKAN, PC_APP_RENDER_CPU, NULL, "vulkan", false, PC_APP_GPU_OS_OTHER, &p);
    CHECK_EQ_INT(p.backend, PC_APP_GPU_CPU);
    CHECK(!p.has_env);
    pc_app_gpu_plan_with_mode(PC_APP_GPU_CPU, PC_APP_RENDER_GPU, NULL, NULL, false, PC_APP_GPU_OS_OTHER, &p);
    CHECK_EQ_INT(p.backend, PC_APP_GPU_AUTO);
}

static void test_marker_round_trip(void)
{
    pc_app_gpu_marker m;
    memset(&m, 0, sizeof m);
    (void)snprintf(m.backend, sizeof m.backend, "auto");
    (void)snprintf(m.adapter, sizeof m.adapter, "A \"quoted\" GPU");
    (void)snprintf(m.adapter_backend, sizeof m.adapter_backend, "vulkan");
    char json[1024];
    pc_app_gpu_marker_to_json(&m, json, sizeof json);
    pc_app_gpu_marker back;
    pc_app_gpu_marker_parse(json, strlen(json), &back);
    CHECK_EQ_STR(back.adapter, "A \"quoted\" GPU");
    CHECK_EQ_STR(back.adapter_backend, "vulkan");
    /* Lenient: garbage parses to an empty marker whose tried() is Auto. */
    pc_app_gpu_marker junk;
    pc_app_gpu_marker_parse("{garbage", 8, &junk);
    pc_app_gpu_backend t;
    CHECK(pc_app_gpu_marker_tried(&junk, &t));
    CHECK_EQ_INT(t, PC_APP_GPU_AUTO);
    /* env: markers are not counted. */
    pc_app_gpu_marker env;
    memset(&env, 0, sizeof env);
    (void)snprintf(env.backend, sizeof env.backend, "env:vulkan");
    CHECK(!pc_app_gpu_marker_tried(&env, &t));
}

static void test_read_rendering_prefs(void)
{
    char *dir = make_tmp();
    CHECK(dir != NULL);
    char path[300];
    (void)snprintf(path, sizeof path, "%s/preferences.json", dir);
    pc_app_gpu_backend b;
    pc_app_render_mode m;
    pc_app_gpu_read_rendering_prefs(NULL, &b, &m);
    CHECK_EQ_INT(b, PC_APP_GPU_AUTO);
    CHECK_EQ_INT(m, PC_APP_RENDER_AUTO);
    FILE *f = fopen(path, "wb");
    if (f) {
        (void)fputs("{\"performance\":{\"gpuBackend\":\"gl\",\"useGpu\":false}}", f);
        (void)fclose(f);
    }
    pc_app_gpu_read_rendering_prefs(path, &b, &m);
    CHECK_EQ_INT(b, PC_APP_GPU_GL);
    CHECK_EQ_INT(m, PC_APP_RENDER_CPU); /* useGpu:false => cpu */
    f = fopen(path, "wb");
    if (f) {
        (void)fputs("{not json", f);
        (void)fclose(f);
    }
    pc_app_gpu_read_rendering_prefs(path, &b, &m);
    CHECK_EQ_INT(b, PC_APP_GPU_AUTO);
    CHECK_EQ_INT(m, PC_APP_RENDER_AUTO);
}

static void test_sentinel_cycle(void)
{
    char *dir = make_tmp();
    CHECK(dir != NULL);
    char sub[300];
    (void)snprintf(sub, sizeof sub, "%s/cfg", dir);
    pc_app_gpu_sentinel s;
    bool created = false;
    pc_app_gpu_marker prev;
    pc_app_gpu_prev p = pc_app_gpu_sentinel_begin(sub, &s, &created, &prev);
    CHECK_EQ_INT(p, PC_APP_GPU_PREV_CLEAN);
    CHECK(created);
    pc_app_gpu_marker m;
    memset(&m, 0, sizeof m);
    (void)snprintf(m.backend, sizeof m.backend, "auto");
    (void)snprintf(m.adapter_backend, sizeof m.adapter_backend, "vulkan");
    (void)snprintf(m.adapter, sizeof m.adapter, "GPU");
    pc_error err;
    CHECK_EQ_INT(pc_app_gpu_sentinel_write(&s, &m, &err), PC_OK);
    /* The process "dies": the lock goes, the marker stays. */
    pc_app_gpu_sentinel_release(&s);
    pc_app_gpu_sentinel s2;
    pc_app_gpu_prev p2 = pc_app_gpu_sentinel_begin(sub, &s2, &created, &prev);
    CHECK_EQ_INT(p2, PC_APP_GPU_PREV_CRASHED);
    CHECK_EQ_STR(prev.adapter, "GPU");
    pc_app_gpu_backend tried;
    CHECK(pc_app_gpu_marker_tried(&prev, &tried));
    CHECK_EQ_INT(tried, PC_APP_GPU_VULKAN);
    /* This start succeeds: the next finds nothing. */
    pc_app_gpu_sentinel_finish(&s2);
    pc_app_gpu_sentinel s3;
    pc_app_gpu_prev p3 = pc_app_gpu_sentinel_begin(sub, &s3, &created, &prev);
    CHECK_EQ_INT(p3, PC_APP_GPU_PREV_CLEAN);
    pc_app_gpu_sentinel_finish(&s3);
}

int main(int argc, char **argv)
{
    RUN(test_no_marker_uses_preference);
    RUN(test_crash_moves_down_chain);
    RUN(test_safe_gpu_and_env);
    RUN(test_mode_overrides);
    RUN(test_marker_round_trip);
    RUN(test_read_rendering_prefs);
    RUN(test_sentinel_cycle);
    return pc_t_finish();
}
