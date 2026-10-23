/* Edit > Presets > Migrate Presets: edit.presets.migratePresets (migrate_cmds.c).
 *
 * Merging a presets file appends only groups the library doesn't already have (by name); it is
 * idempotent; a bad or missing path is a graceful error (never a crash, REWRITE_PLAN.md rule 9). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/json.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/migrate_cmds.h"
#include "engine/presets.h"
#include "harness.h"

static pc_status run_json(pc_engine_session *s, const char *id, const char *params, int64_t *migrated)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    if (rc == PC_OK && migrated != NULL) {
        yyjson_mut_val *root = pc_json_value_root(&out);
        yyjson_mut_val *m = root != NULL ? yyjson_mut_obj_get(root, "migrated") : NULL;
        *migrated = m != NULL ? yyjson_mut_get_sint(m) : -1;
    }
    pc_json_value_fini(&out);
    return rc;
}

/* Write `text` to a temp file; returns the path (static buffer). */
static const char *write_temp(const char *text)
{
    static char path[256];
    (void)snprintf(path, sizeof path, "/dev/shm/pc-gap3/pc-migrate-test-%ld.json", (long)getpid());
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        (void)fwrite(text, 1, strlen(text), f);
        (void)fclose(f);
    }
    return path;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_migrate_specs(&n);
    CHECK_EQ_UINT(n, 1);
    CHECK(pc_engine_command_find("edit.presets.migratePresets") != NULL);
}

static void test_merges_new_only(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    size_t before = s->presets.gradients.n;
    CHECK(before > 0);
    /* One new gradient group and one that already exists (the first built-in group's name). */
    const char *existing = s->presets.gradients.v[0].name;
    char json[512];
    (void)snprintf(json, sizeof json,
                   "{\"presets\":{\"gradients\":[{\"name\":\"My Imported Set\",\"items\":[]},"
                   "{\"name\":\"%s\",\"items\":[]}]}}",
                   existing);
    const char *path = write_temp(json);
    char params[384];
    (void)snprintf(params, sizeof params, "{\"path\":\"%s\"}", path);
    int64_t migrated = -1;
    uint64_t rev0 = s->presets.rev;
    CHECK(run_json(s, "edit.presets.migratePresets", params, &migrated) == PC_OK);
    CHECK_EQ_INT((int)migrated, 1);
    CHECK_EQ_UINT(s->presets.gradients.n, before + 1);
    CHECK(s->presets.rev > rev0);
    bool found = false;
    for (size_t i = 0; i < s->presets.gradients.n; i++) {
        if (strcmp(s->presets.gradients.v[i].name, "My Imported Set") == 0) {
            found = true;
        }
    }
    CHECK(found);
    /* Idempotent: a second migration adds nothing and leaves the revision alone. */
    uint64_t rev1 = s->presets.rev;
    migrated = -1;
    CHECK(run_json(s, "edit.presets.migratePresets", params, &migrated) == PC_OK);
    CHECK_EQ_INT((int)migrated, 0);
    CHECK_EQ_UINT(s->presets.gradients.n, before + 1);
    CHECK_EQ_UINT(s->presets.rev, rev1);
    (void)remove(path);
    pc_engine_session_free(s);
}

static void test_tool_presets_and_patterns(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    size_t ntp = s->presets.ntool_presets;
    size_t npg = s->presets.pattern_groups.n;
    const char *path = write_temp(
        "{\"tool_presets\":[{\"name\":\"Imported tool\",\"tool\":\"brush\",\"options\":{\"size\":17}}],"
        "\"pattern_groups\":[{\"name\":\"Imported patterns\",\"items\":[\"Bricks\"]}]}");
    char params[384];
    (void)snprintf(params, sizeof params, "{\"path\":\"%s\"}", path);
    int64_t migrated = -1;
    CHECK(run_json(s, "edit.presets.migratePresets", params, &migrated) == PC_OK);
    CHECK_EQ_INT((int)migrated, 2);
    CHECK_EQ_UINT(s->presets.ntool_presets, ntp + 1);
    CHECK_EQ_UINT(s->presets.pattern_groups.n, npg + 1);
    (void)remove(path);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_json(s, "edit.presets.migratePresets", "{\"path\":\"/no/such/file.json\"}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.presets.migratePresets", "{}", NULL) != PC_OK);
    /* A file that isn't JSON. */
    const char *path = write_temp("not json at all {{{");
    char params[384];
    (void)snprintf(params, sizeof params, "{\"path\":\"%s\"}", path);
    CHECK(run_json(s, "edit.presets.migratePresets", params, NULL) != PC_OK);
    (void)remove(path);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_merges_new_only);
    RUN(test_tool_presets_and_patterns);
    RUN(test_graceful);
    return pc_t_finish();
}
