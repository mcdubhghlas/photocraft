/* See app_icon.h. */
#include "app_icon.h"

#include "ui/brand.h"

pc_status pc_app_window_icon_rgba(uint8_t **out, uint32_t *w, uint32_t *h, pc_error *err)
{
    return pc_ui_brand_icon_rgba(out, w, h, err);
}
