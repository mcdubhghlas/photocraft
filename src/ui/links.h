/* Community and project links (port of crates/ui-egui/src/links.rs). The rendering (Help menu,
 * About dialog, start screen) is reimplemented by the chunks that draw those; U1 owns the URL data
 * and the id->url lookup the control channel and menus use. No GUI dependency. */
#ifndef PC_UI_LINKS_H
#define PC_UI_LINKS_H

#define PC_UI_LINK_DISCORD "https://discord.gg/artcraft"
#define PC_UI_LINK_ARTCRAFT_WEBSITE "https://getartcraft.com"
#define PC_UI_LINK_APP_PAGE "https://getartcraft.com/apps/photocraft"
#define PC_UI_LINK_GITHUB "https://github.com/storytold/photocraft"
#define PC_UI_LINK_ISSUES "https://github.com/storytold/photocraft/issues"

typedef struct pc_ui_link {
    const char *id;
    const char *url;
} pc_ui_link;

/* Help-menu link commands: (id, url). Labels live with the menu definitions. */
extern const pc_ui_link PC_UI_LINK_COMMANDS[5];

/* The URL for a help-link command id, or NULL. */
const char *pc_ui_link_url_for(const char *id);

#endif
