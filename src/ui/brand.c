/* Brand mark decode (port of crates/ui-egui/src/brand.rs decode()). */
#include "ui/brand.h"

#include <stdlib.h>

#include "codecs/codecs.h"
#include "codecs/image.h"

pc_status pc_ui_brand_icon_rgba(uint8_t **out, uint32_t *w, uint32_t *h, pc_error *err)
{
    pc_cimage img;
    pc_cimage_init(&img);
    pc_status st = pc_cdecode(pc_ui_app_icon_png, pc_ui_app_icon_png_len, &img, err);
    if (st != PC_OK) {
        return st;
    }
    uint8_t *rgba = NULL;
    size_t n = 0;
    st = pc_cimage_to_rgba8(&img, &rgba, &n, err);
    uint32_t iw = img.width, ih = img.height;
    pc_cimage_fini(&img);
    if (st != PC_OK) {
        return st;
    }
    if (n != (size_t)iw * (size_t)ih * 4) {
        free(rgba);
        return pc_fail(err, PC_ERR_FORMAT, "unexpected icon buffer size");
    }
    *out = rgba;
    *w = iw;
    *h = ih;
    return PC_OK;
}
