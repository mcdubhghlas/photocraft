#include "harness.h"
#include "ui/credits.h"
#include <stdlib.h>
#include <string.h>
static void test_consent_names_and_synced_fallback(void)
{
    const char json[] =
        "{\"commits\":42,\"contributors\":[{\"login\":\"alice\",\"people_entry\":true,\"github_name\":"
        "\"UNAPPROVED\"},{\"login\":\"bob\",\"people_entry\":false,\"github_name\":\"Private\"},{\"login\":"
        "\"carol\",\"people_entry\":true,\"github_name\":\"Public "
        "Name\"},{\"login\":\"dan\",\"people_entry\":true,\"github_name\":\"Other\"}],\"models\":[]}";
    const char people[] = "[people.alice]\nreal_name = \"  Chosen \\u00c9 \\U0001F338  \"\ndisplay_name = "
                          "'Nickname'\n[people.bob]\nreal_name = \"Never display\"\nsync_github_name = "
                          "true\n[people.carol]\nsync_github_name = true # consent recorded\n";
    pc_ui_credits c = {0};
    CHECK(pc_ui_credits_parse(json, sizeof json - 1, people, sizeof people - 1, &c, NULL) == PC_OK);
    CHECK(c.count == 4 && c.total_commits == 42);
    CHECK_EQ_STR(c.contributors[0].real_name, "Chosen É 🌸");
    CHECK_EQ_STR(c.contributors[0].display_name, "Nickname");
    CHECK(!c.contributors[1].real_name && !c.contributors[1].display_name);
    CHECK_EQ_STR(c.contributors[2].display_name, "Public Name");
    CHECK(!c.contributors[3].real_name && !c.contributors[3].display_name);
    CHECK(!pc_ui_credit_chosen_name(&c.contributors[0], PC_CREDIT_USERNAME));
    CHECK_EQ_STR(pc_ui_credit_chosen_name(&c.contributors[0], PC_CREDIT_REAL_NAME), "Chosen É 🌸");
    pc_ui_credits_fini(&c);
}
static void test_sort_ties_dates_and_full_unsigned_delta(void)
{
    pc_ui_contributor rows[] = {
        {.login = "zeta", .first = "", .last = "", .added = UINT64_MAX, .deleted = 0, .commits = 2},
        {.login = "beta",
         .first = "2026-01-01",
         .last = "2026-01-02",
         .added = 0,
         .deleted = UINT64_MAX,
         .commits = 2},
        {.login = "Alpha",
         .first = "2026-01-01",
         .last = "2026-01-03",
         .added = 4,
         .deleted = 1,
         .commits = 2}};
    pc_ui_credits c = {.contributors = rows, .count = 3};
    size_t *order = NULL;
    CHECK(pc_ui_credits_order(&c, PC_CREDIT_USERNAME, PC_CREDIT_FIRST, true, &order, NULL) == PC_OK);
    CHECK(order[0] == 2 && order[1] == 1 && order[2] == 0);
    free(order);
    CHECK(pc_ui_credits_order(&c, PC_CREDIT_USERNAME, PC_CREDIT_COMMITS, false, &order, NULL) == PC_OK);
    CHECK(order[0] == 2 && order[1] == 1 && order[2] == 0);
    free(order);
    CHECK(pc_ui_credits_order(&c, PC_CREDIT_USERNAME, PC_CREDIT_DELTA, true, &order, NULL) == PC_OK);
    CHECK(order[0] == 1 && order[1] == 2 && order[2] == 0);
    free(order);
    CHECK(pc_ui_credit_compare(&rows[1], &rows[0], PC_CREDIT_USERNAME, PC_CREDIT_DELTA, false) > 0);
    CHECK(pc_ui_credit_compare(&rows[2], &rows[1], PC_CREDIT_USERNAME, PC_CREDIT_LAST, true) > 0);
}
static void test_embedded_tables_and_malformed_input(void)
{
    pc_ui_credits c = {0};
    CHECK(pc_ui_credits_embedded(&c, NULL) == PC_OK);
    CHECK(c.count > 100 && c.nmodels > 10 && c.total_commits > 1000);
    for (size_t i = 0; i < c.count; i++)
        CHECK(c.contributors[i].login && *c.contributors[i].login);
    pc_ui_credits_fini(&c);
    CHECK(pc_ui_credits_parse("{}", 2, NULL, 0, &c, NULL) != PC_OK);
    CHECK(pc_ui_credits_parse("{\"contributors\":null}", 21, NULL, 0, &c, NULL) != PC_OK);
    CHECK(pc_ui_credits_parse(NULL, 0, NULL, 0, &c, NULL) != PC_OK);
    pc_ui_credits_fini(&c);
}
int main(int argc, char **argv)
{
    RUN(test_consent_names_and_synced_fallback);
    RUN(test_sort_ties_dates_and_full_unsigned_delta);
    RUN(test_embedded_tables_and_malformed_input);
    return pc_t_finish();
}
