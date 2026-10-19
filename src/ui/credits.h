#ifndef PC_UI_CREDITS_H
#define PC_UI_CREDITS_H
#include "base/json.h"
typedef enum pc_ui_credit_name_mode {
    PC_CREDIT_USERNAME,
    PC_CREDIT_DISPLAY_NAME,
    PC_CREDIT_REAL_NAME
} pc_ui_credit_name_mode;
typedef enum pc_ui_credit_sort {
    PC_CREDIT_NAME,
    PC_CREDIT_PRS,
    PC_CREDIT_COMMITS,
    PC_CREDIT_ADDED,
    PC_CREDIT_DELETED,
    PC_CREDIT_DELTA,
    PC_CREDIT_BINARY_ADDED,
    PC_CREDIT_BINARY_DELETED,
    PC_CREDIT_FIRST,
    PC_CREDIT_LAST,
    PC_CREDIT_SORT_COUNT
} pc_ui_credit_sort;
typedef struct pc_ui_contributor {
    const char *login, *first, *last;
    char *display_name, *real_name;
    uint64_t prs, commits, added, deleted, binary_added, binary_deleted;
} pc_ui_contributor;
typedef struct pc_ui_credit_model {
    const char *company, *model, *version;
    uint64_t commits, added, deleted;
} pc_ui_credit_model;
typedef struct pc_ui_credits {
    yyjson_doc *source;
    pc_ui_contributor *contributors;
    pc_ui_credit_model *models;
    size_t count, nmodels;
    uint64_t total_commits;
} pc_ui_credits;
void pc_ui_credits_fini(pc_ui_credits *credits);
pc_status pc_ui_credits_parse(const char *json, size_t length, const char *people, size_t people_length,
                              pc_ui_credits *out, pc_error *err);
pc_status pc_ui_credits_embedded(pc_ui_credits *out, pc_error *err);
const char *pc_ui_credit_chosen_name(const pc_ui_contributor *row, pc_ui_credit_name_mode mode);
/* Missing chosen names display as @login; ties always use name A-Z, then exact login. */
int pc_ui_credit_compare(const pc_ui_contributor *a, const pc_ui_contributor *b, pc_ui_credit_name_mode names,
                         pc_ui_credit_sort key, bool ascending);
pc_status pc_ui_credits_order(const pc_ui_credits *credits, pc_ui_credit_name_mode names,
                              pc_ui_credit_sort key, bool ascending, size_t **out, pc_error *err);
extern const unsigned char pc_ui_contributors_json[], pc_ui_people_toml[];
extern const size_t pc_ui_contributors_json_len, pc_ui_people_toml_len;
#endif
