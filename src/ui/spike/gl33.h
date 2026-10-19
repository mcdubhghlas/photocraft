/* SDL-owned loader for the platform-independent OpenGL 3.3 function table. */
#ifndef PC_UI_GL33_H
#define PC_UI_GL33_H
#include "gpu/gl33.h"
bool gl33_load(gl33 *gl, const char **missing);
#endif
