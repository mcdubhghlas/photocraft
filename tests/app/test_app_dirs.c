/* app_dirs: directory resolution (override, portable, platform, warnings, missing env). */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/app_dirs.h"
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
            return e->vals[i][0] ? e->vals[i] : NULL; /* empty => unset, as the reference does */
        }
    }
    return NULL;
}

static void put(fake_env *e, const char *k, const char *v)
{
    e->keys[e->n] = k;
    e->vals[e->n] = v;
    e->n++;
}

static char *make_tmp(void)
{
    char tmpl[] = "/dev/shm/pc-app-dirs-XXXXXX";
    char *base = getenv("TMPDIR");
    static char path[256];
    if (base && base[0]) {
        (void)snprintf(path, sizeof path, "%s/pc-app-dirs-XXXXXX", base);
    } else {
        (void)snprintf(path, sizeof path, "%s", tmpl);
    }
    return mkdtemp(path);
}

static void test_override_wins(void)
{
    char *root = make_tmp();
    CHECK(root != NULL);
    char custom[300];
    (void)snprintf(custom, sizeof custom, "%s/custom", root);
    fake_env e = {{0}, {0}, 0};
    put(&e, "PHOTOCRAFT_CONFIG_DIR", custom);
    put(&e, "HOME", root);
    pc_app_dirs d;
    pc_app_dirs_resolve(&d, env_lookup, &e, root);
    CHECK(d.has_dir);
    CHECK_EQ_INT(d.mode, PC_APP_DIR_OVERRIDE);
    CHECK_EQ_STR(d.dir, custom);
}

static void test_platform_linux(void)
{
    char *root = make_tmp();
    CHECK(root != NULL);
    char home[300];
    (void)snprintf(home, sizeof home, "%s/home", root);
    fake_env e = {{0}, {0}, 0};
    put(&e, "HOME", home);
    pc_app_dirs d;
    pc_app_dirs_resolve(&d, env_lookup, &e, root); /* no marker in root */
    CHECK_EQ_INT(d.mode, PC_APP_DIR_PLATFORM);
    CHECK(!d.has_warning);
#if !defined(__APPLE__) && !defined(_WIN32)
    char want[400];
    (void)snprintf(want, sizeof want, "%s/.config/photocraft", home);
    CHECK(d.has_dir);
    CHECK_EQ_STR(d.dir, want);
    /* XDG_CONFIG_HOME overrides ~/.config. */
    fake_env x = {{0}, {0}, 0};
    put(&x, "HOME", home);
    put(&x, "XDG_CONFIG_HOME", root);
    pc_app_dirs d2;
    pc_app_dirs_resolve(&d2, env_lookup, &x, root);
    (void)snprintf(want, sizeof want, "%s/photocraft", root);
    CHECK_EQ_STR(d2.dir, want);
#endif
}

static void test_portable_marker(void)
{
    char *root = make_tmp();
    CHECK(root != NULL);
    char marker[300];
    (void)snprintf(marker, sizeof marker, "%s/portable.txt", root);
    FILE *f = fopen(marker, "wb");
    CHECK(f != NULL);
    if (f) { (void)fclose(f); }
    char home[300];
    (void)snprintf(home, sizeof home, "%s/home", root);
    fake_env e = {{0}, {0}, 0};
    put(&e, "HOME", home);
    pc_app_dirs d;
    pc_app_dirs_resolve(&d, env_lookup, &e, root);
    CHECK_EQ_INT(d.mode, PC_APP_DIR_PORTABLE);
    CHECK(d.has_dir);
    char want[400];
    (void)snprintf(want, sizeof want, "%s/%s", root, PC_APP_PORTABLE_DATA_DIR);
    CHECK_EQ_STR(d.dir, want);
    struct stat st;
    CHECK(stat(want, &st) == 0 && S_ISDIR(st.st_mode));
}

static void test_unwritable_portable_falls_back(void)
{
    char *root = make_tmp();
    CHECK(root != NULL);
    char marker[300];
    (void)snprintf(marker, sizeof marker, "%s/PhotoCraft.portable", root);
    FILE *f = fopen(marker, "wb");
    if (f) { (void)fclose(f); }
    /* A regular file where the data folder should go: it cannot be created as a directory. */
    char blocker[320];
    (void)snprintf(blocker, sizeof blocker, "%s/%s", root, PC_APP_PORTABLE_DATA_DIR);
    f = fopen(blocker, "wb");
    if (f) { (void)fclose(f); }
    char home[300];
    (void)snprintf(home, sizeof home, "%s/home", root);
    fake_env e = {{0}, {0}, 0};
    put(&e, "HOME", home);
    pc_app_dirs d;
    pc_app_dirs_resolve(&d, env_lookup, &e, root);
    CHECK_EQ_INT(d.mode, PC_APP_DIR_PLATFORM);
    CHECK(d.has_warning);
    CHECK_CONTAINS(d.warning, "portable");
    CHECK_CONTAINS(d.warning, "isn't writable");
}

static void test_missing_env_never_crashes(void)
{
    pc_app_dirs d;
    pc_app_dirs_resolve(&d, env_lookup, &(fake_env){{0}, {0}, 0}, NULL);
    CHECK_EQ_INT(d.mode, PC_APP_DIR_PLATFORM);
    CHECK(!d.has_dir); /* no HOME/APPDATA => no directory, no crash */
}

int main(int argc, char **argv)
{
    RUN(test_override_wins);
    RUN(test_platform_linux);
    RUN(test_portable_marker);
    RUN(test_unwritable_portable_falls_back);
    RUN(test_missing_env_never_crashes);
    return pc_t_finish();
}
