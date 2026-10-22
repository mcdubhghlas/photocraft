/* linux_libs: session detection, ldconfig parsing, requirement checking, distro + messages. */
#include <string.h>

#include "app/linux_libs.h"
#include "harness.h"

typedef struct {
    const char *keys[8];
    const char *vals[8];
    size_t n;
} fake_env;

static const char *env_lookup(void *ctx, const char *name)
{
    fake_env *e = ctx;
    for (size_t i = 0; i < e->n; i++) {
        if (strcmp(e->keys[i], name) == 0) {
            return e->vals[i][0] ? e->vals[i] : NULL;
        }
    }
    return NULL;
}

static bool never_exists(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    return false;
}

static void test_session_follows_winit_order(void)
{
    fake_env a = {{"WAYLAND_DISPLAY", "DISPLAY"}, {"wayland-0", ":0"}, 2};
    CHECK_EQ_INT(pc_linux_session_from_env(env_lookup, &a), PC_DISPLAY_WAYLAND);
    fake_env b = {{"WAYLAND_DISPLAY", "DISPLAY"}, {"", ":99"}, 2};
    CHECK_EQ_INT(pc_linux_session_from_env(env_lookup, &b), PC_DISPLAY_X11);
    fake_env c = {{"DISPLAY"}, {""}, 1};
    CHECK_EQ_INT(pc_linux_session_from_env(env_lookup, &c), PC_DISPLAY_NONE);
    fake_env d = {{0}, {0}, 0};
    CHECK_EQ_INT(pc_linux_session_from_env(env_lookup, &d), PC_DISPLAY_NONE);
}

static void test_ldconfig_parsing(void)
{
    const char *cache =
        "1234 libs found in cache `/etc/ld.so.cache'\n"
        "\tlibxkbcommon.so.0 (libc6,x86-64) => /lib/x86_64-linux-gnu/libxkbcommon.so.0\n"
        "\tlibxkbcommon-x11.so.0 (libc6) => /lib/i386-linux-gnu/libxkbcommon-x11.so.0\n";
    CHECK(pc_linux_ldconfig_valid(cache));
    CHECK(!pc_linux_ldconfig_valid("ldconfig: command not found"));
    CHECK(pc_linux_ldconfig_has(cache, "libxkbcommon.so.0", "x86-64"));
    /* 32-bit only does not count for a 64-bit app. */
    CHECK(!pc_linux_ldconfig_has(cache, "libxkbcommon-x11.so.0", "x86-64"));
    CHECK(pc_linux_ldconfig_has(cache, "libxkbcommon-x11.so.0", NULL));
    /* Prefix of another name is not a match. */
    CHECK(!pc_linux_ldconfig_has(cache, "libxkbcommon.so", NULL));
    CHECK(!pc_linux_ldconfig_has("", "libX11.so.6", NULL));
}

static void test_check_and_message(void)
{
    /* X11 cache missing libxkbcommon-x11 is reported, conclusively. */
    const char *names[] = {"libX11.so.6",   "libX11-xcb.so.1", "libxcb.so.1", "libXcursor.so.1",
                           "libXi.so.6",    "libxkbcommon.so.0", "libEGL.so.1"};
    char cache[1024];
    size_t off = (size_t)snprintf(cache, sizeof cache, "%zu libs found in cache `x'\n",
                                  sizeof names / sizeof names[0]);
    const char *tag =
#if defined(__x86_64__)
        "libc6,x86-64";
#elif defined(__aarch64__)
        "libc6,AArch64";
#else
        "libc6";
#endif
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        off += (size_t)snprintf(cache + off, sizeof cache - off, "\t%s (%s) => /usr/lib/%s\n",
                                names[i], tag, names[i]);
    }
    const char *dirs[1] = {0};
    pc_linux_probe probe = {cache, dirs, 0};
    pc_linux_verdict v;
    pc_linux_check(PC_DISPLAY_X11, &probe, never_exists, NULL, &v);
    CHECK(!v.ok);
    CHECK(v.conclusive);
    CHECK_EQ_UINT(v.n_missing, 1u);
    CHECK_EQ_STR(v.missing[0]->libs[0].soname, "libxkbcommon-x11.so.0");
    char msg[1024];
    pc_linux_missing_message(PC_DISPLAY_X11, v.missing, v.n_missing, PC_DISTRO_DEBIAN, msg, sizeof msg);
    CHECK_CONTAINS(msg, "libxkbcommon-x11.so.0");
    CHECK_CONTAINS(msg, "sudo apt install libxkbcommon-x11-0");
    CHECK_CONTAINS(msg, "PHOTOCRAFT_SKIP_LIB_CHECK");
}

static void test_inconclusive_without_cache(void)
{
    /* No cache and nothing found: inconclusive, so the app should start (not reported as fatal). */
    const char *dirs[1] = {0};
    pc_linux_probe probe = {NULL, dirs, 0};
    pc_linux_verdict v;
    pc_linux_check(PC_DISPLAY_WAYLAND, &probe, never_exists, NULL, &v);
    CHECK(!v.ok);
    CHECK(!v.conclusive);
}

static void test_distro_detection(void)
{
    CHECK_EQ_INT(pc_linux_distro_from_os_release("NAME=\"Ubuntu\"\nID=ubuntu\nID_LIKE=debian\n"),
                 PC_DISTRO_DEBIAN);
    CHECK_EQ_INT(pc_linux_distro_from_os_release("ID=fedora\n"), PC_DISTRO_FEDORA);
    CHECK_EQ_INT(pc_linux_distro_from_os_release("ID=endeavouros\nID_LIKE=arch\n"), PC_DISTRO_ARCH);
    CHECK_EQ_INT(pc_linux_distro_from_os_release("ID=\"opensuse-tumbleweed\"\n"), PC_DISTRO_SUSE);
    CHECK_EQ_INT(pc_linux_distro_from_os_release("ID=nixos\n"), PC_DISTRO_UNKNOWN);
    CHECK_EQ_INT(pc_linux_distro_from_os_release(""), PC_DISTRO_UNKNOWN);
}

int main(int argc, char **argv)
{
    RUN(test_session_follows_winit_order);
    RUN(test_ldconfig_parsing);
    RUN(test_check_and_message);
    RUN(test_inconclusive_without_cache);
    RUN(test_distro_detection);
    return pc_t_finish();
}
