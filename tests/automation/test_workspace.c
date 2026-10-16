/* Capability sandbox: path validation, read/write confinement, symlink escape, and the command
 * authorization policy (port of workspace.rs tests). */
#include "harness.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "automation/workspace.h"
#include "base/buf.h"
#include "base/json.h"

static void rmrf(const char *path)
{
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

static void test_read_write_and_rejects(void)
{
    const char *tmp = getenv("TMPDIR");
    char base[512];
    snprintf(base, sizeof base, "%s/pc-ws-%d", tmp ? tmp : "/tmp", (int)getpid());
    char inside[600], outside[600];
    snprintf(inside, sizeof inside, "%s/inside", base);
    snprintf(outside, sizeof outside, "%s/outside", base);
    rmrf(base);
    (void)mkdir(base, 0755);
    (void)mkdir(inside, 0755);
    (void)mkdir(outside, 0755);

    pc_auto_workspace ws;
    CHECK(pc_auto_workspace_init(&ws, inside, inside, NULL) == PC_OK);

    /* A valid write then read round-trips. */
    CHECK(pc_auto_workspace_write(&ws, "out.txt", "created", 7, NULL) == PC_OK);
    pc_buf got = PC_BUF_INIT;
    CHECK(pc_auto_workspace_read(&ws, "out.txt", &got, NULL) == PC_OK);
    CHECK_EQ_UINT(got.len, 7u);
    CHECK(memcmp(got.data, "created", 7) == 0);
    pc_buf_fini(&got);

    /* Hostile paths are refused for both read and write, without touching anything. */
    const char *bad[] = {"",         "/absolute", "../escape", "a/../escape", "a\\b",
                         "C:/escape", "a//b",      "./a",       "name:stream", "NUL",
                         "con.txt",  "folder/COM1.log"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pc_buf b = PC_BUF_INIT;
        CHECK(pc_auto_workspace_read(&ws, bad[i], &b, NULL) != PC_OK);
        pc_buf_fini(&b);
        CHECK(pc_auto_workspace_write(&ws, bad[i], "x", 1, NULL) != PC_OK);
    }

    /* A symlink pointing outside the root cannot be traversed. */
    char link[700];
    snprintf(link, sizeof link, "%s/link", inside);
    if (symlink(outside, link) == 0) {
        pc_buf b = PC_BUF_INIT;
        CHECK(pc_auto_workspace_read(&ws, "link/secret.txt", &b, NULL) != PC_OK);
        pc_buf_fini(&b);
        CHECK(pc_auto_workspace_write(&ws, "link/new.txt", "blocked", 7, NULL) != PC_OK);
        char leaked[800];
        snprintf(leaked, sizeof leaked, "%s/new.txt", outside);
        CHECK(access(leaked, F_OK) != 0);
    }

    pc_auto_workspace_fini(&ws);
    rmrf(base);
}

static void test_separate_authority(void)
{
    /* No authority fails closed for both operations. */
    pc_auto_workspace none;
    CHECK(pc_auto_workspace_init(&none, NULL, NULL, NULL) == PC_OK);
    pc_buf b = PC_BUF_INIT;
    CHECK(pc_auto_workspace_read(&none, "x.txt", &b, NULL) != PC_OK);
    pc_buf_fini(&b);
    CHECK(pc_auto_workspace_write(&none, "x.txt", "y", 1, NULL) != PC_OK);
    pc_auto_workspace_fini(&none);
}

static pc_status authz(const char *id, const char *pjson)
{
    yyjson_doc *pd = NULL;
    (void)pc_json_parse(pjson, strlen(pjson), &pd, NULL);
    pc_status rc = pc_auto_authorize_engine_command(id, pd ? yyjson_doc_get_root(pd) : NULL, NULL);
    yyjson_doc_free(pd);
    return rc;
}

static pc_status authz_desktop(const char *id, const char *pjson)
{
    yyjson_doc *pd = NULL;
    (void)pc_json_parse(pjson, strlen(pjson), &pd, NULL);
    pc_status rc = pc_auto_authorize_desktop_engine_command(id, pd ? yyjson_doc_get_root(pd) : NULL, NULL);
    yyjson_doc_free(pd);
    return rc;
}

static void test_authorize_policy(void)
{
    const char *denied[] = {"file.open",
                            "file.openAs",
                            "file.save",
                            "file.saveAs",
                            "file.saveACopy",
                            "file.export.saveForWebLegacy",
                            "pattern.import",
                            "layer.smartObjects.exportContents",
                            "layer.smartObjects.replaceContents",
                            "measurementLog.export",
                            "edit.colorSettings"};
    for (size_t i = 0; i < sizeof denied / sizeof denied[0]; i++) {
        CHECK(authz(denied[i], "{}") != PC_OK);
    }
    CHECK(authz("file.new", "{}") == PC_OK);
    CHECK(authz("file.automate.cropAndStraightenPhotos", "{}") == PC_OK);
    CHECK(authz("image.mode.cmyk", "{}") == PC_OK);
    CHECK(authz_desktop("image.mode.cmyk", "{}") != PC_OK);
    CHECK(authz("image.mode.rgb", "{\"profile\":\"/outside/profile.icc\"}") != PC_OK);
    CHECK(authz("image.mode.rgb", "{\"profile\":\"srgb\"}") == PC_OK);
    CHECK(authz("filter.distort.displace", "{\"mapPath\":\"outside.png\"}") != PC_OK);
    CHECK(authz("prefs.set", "{\"path\":\"colorSettings.workingRgb\",\"value\":\"outside.icc\"}") != PC_OK);
    CHECK(authz("prefs.set", "{\"path\":\"interface.language\",\"value\":\"fr\"}") == PC_OK);
    CHECK(authz("brush.presets.importAbr", "{\"path\":\"/outside/set.abr\"}") != PC_OK);
    CHECK(authz("brush.presets.importAbr", "{\"data\":\"QUJD\"}") == PC_OK);
    CHECK(authz("plugin.reload", "{}") != PC_OK);
}

int main(int argc, char **argv)
{
    RUN(test_read_write_and_rejects);
    RUN(test_separate_authority);
    RUN(test_authorize_policy);
    return pc_t_finish();
}
