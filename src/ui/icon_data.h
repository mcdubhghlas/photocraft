#ifndef PC_UI_ICON_DATA_H
#define PC_UI_ICON_DATA_H
#include <stddef.h>
#include <stdint.h>
typedef struct pc_ui_embedded_icon {
    const char *name;
    const uint8_t *svg;
    size_t len;
} pc_ui_embedded_icon;
extern const pc_ui_embedded_icon pc_ui_embedded_icons[];
extern const size_t pc_ui_embedded_icons_count;
#endif
