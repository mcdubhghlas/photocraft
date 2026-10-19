#ifndef PC_UI_CROP_SIZE_H
#define PC_UI_CROP_SIZE_H
#include <stdbool.h>
/* Positive bounded length with px/in/cm/mm/pt/pc suffix; bare numbers are pixels.
 * Decimal comma is accepted. No locale-dependent parsing or partial trailing input. */
bool pc_ui_crop_pixels(const char *text, double ppi, double *pixels);
bool pc_ui_crop_resolution(const char *text, const char *unit, double *ppi);
#endif
