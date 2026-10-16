/* Community and project links (port of crates/ui-egui/src/links.rs). */
#include "ui/links.h"

#include <string.h>

const pc_ui_link PC_UI_LINK_COMMANDS[5] = {
    {"help.discord", PC_UI_LINK_DISCORD},
    {"help.website", PC_UI_LINK_APP_PAGE},
    {"help.artcraftWebsite", PC_UI_LINK_ARTCRAFT_WEBSITE},
    {"help.github", PC_UI_LINK_GITHUB},
    {"help.reportIssue", PC_UI_LINK_ISSUES},
};

const char *pc_ui_link_url_for(const char *id)
{
    if (!id) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof PC_UI_LINK_COMMANDS / sizeof PC_UI_LINK_COMMANDS[0]; i++) {
        if (strcmp(PC_UI_LINK_COMMANDS[i].id, id) == 0) {
            return PC_UI_LINK_COMMANDS[i].url;
        }
    }
    return NULL;
}
