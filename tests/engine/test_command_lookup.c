/* Registry lookup must agree with the public spec slice: find(id) returns the first linear match,
 * every id is unique, and non-ids resolve to nothing (port of crates/engine/tests/command_lookup.rs,
 * widened with the uniqueness and bulk-resolution checks the C port's acceptance gate wants).
 *
 * The reference's menu-catalogue cross-check (crates/ui-egui/src/menu_catalog.rs) has no C
 * equivalent yet: the egui shell (L6) is unported, so there is no menu catalogue to match against.
 * command.c builds ids to match crates/engine byte-for-byte; that is what command_lookup guards. */
#include <stdlib.h>
#include <string.h>

#include "engine/engine.h"
#include "harness.h"

/* find(id) must return the same spec as the first linear match in the public slice, for every id,
 * whether the id is a static string or a freshly allocated copy (control/MCP owns its input). */
static void test_every_lookup_matches_the_first_linear_match(void)
{
    size_t n = 0;
    const pc_engine_command *all = pc_engine_commands(&n);
    CHECK(all != NULL && n > 0);
    for (size_t i = 0; i < n; i++) {
        /* The first spec in the slice carrying this id (first registration wins). */
        size_t first = i;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(all[j].id, all[i].id) == 0) {
                first = j;
                break;
            }
        }
        const pc_engine_command *got = pc_engine_command_find(all[i].id);
        if (got != &all[first]) {
            PC_T_FAIL("find(\"%s\") is not the first linear match", all[i].id);
        }
        /* A non-static copy of the id resolves to the very same spec. */
        char *owned = malloc(strlen(all[i].id) + 1);
        CHECK(owned != NULL);
        if (owned != NULL) {
            memcpy(owned, all[i].id, strlen(all[i].id) + 1);
            if (pc_engine_command_find(owned) != &all[first]) {
                PC_T_FAIL("find(owned \"%s\") differs from find(static)", all[i].id);
            }
            free(owned);
        }
    }
}

/* Every registered id is unique (the task's hard requirement; first-wins would otherwise mask a
 * second registration). Reports each clashing id. */
static void test_ids_are_unique(void)
{
    size_t n = 0;
    const pc_engine_command *all = pc_engine_commands(&n);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (strcmp(all[i].id, all[j].id) == 0) {
                PC_T_FAIL("duplicate command id \"%s\" (specs %zu and %zu)", all[i].id, i, j);
            }
        }
    }
}

/* Every spec carries the non-NULL metadata the frontends dereference, and find() agrees with the
 * public slice on each id's presence. */
static void test_specs_are_well_formed(void)
{
    size_t n = 0;
    const pc_engine_command *all = pc_engine_commands(&n);
    for (size_t i = 0; i < n; i++) {
        if (all[i].id == NULL || all[i].label == NULL || all[i].params == NULL ||
            all[i].enabled == NULL || all[i].run == NULL) {
            PC_T_FAIL("spec %zu (\"%s\") has a NULL required field", i, all[i].id ? all[i].id : "(null)");
        }
        /* A menu path with no entries must not claim a length. */
        if (all[i].menu_len > 0) {
            CHECK(all[i].menu != NULL);
        }
    }
}

/* Non-ids resolve to nothing, exactly as the reference requires. */
static void test_non_ids_resolve_to_nothing(void)
{
    static const char *const missing[] = {"", "missing.command", "FILE.NEW", "file.new ",
                                           " file.new", "\xf0\x9f\x92\xbe" /* 💾 */};
    for (size_t i = 0; i < sizeof missing / sizeof missing[0]; i++) {
        if (pc_engine_command_find(missing[i]) != NULL) {
            PC_T_FAIL("find(\"%s\") unexpectedly resolved", missing[i]);
        }
    }
    /* A couple of known-good ids still resolve (sanity that the slice isn't empty of them). */
    CHECK(pc_engine_command_find("edit.undo") != NULL);
    CHECK(pc_engine_command_find("file.new") != NULL);
}

int main(int argc, char **argv)
{
    RUN(test_every_lookup_matches_the_first_linear_match);
    RUN(test_ids_are_unique);
    RUN(test_specs_are_well_formed);
    RUN(test_non_ids_resolve_to_nothing);
    return pc_t_finish();
}
