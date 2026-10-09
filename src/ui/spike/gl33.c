#include "gl33.h"

#include <SDL3/SDL.h>

bool gl33_load(gl33 *gl, const char **missing)
{
    /* SDL returns a generic function pointer; converting between function pointer types is
     * well-defined in C, and each is called only through its real type. */
#define GL33_LOAD(ret, name, params)                                                              \
    gl->name = (ret(*) params)SDL_GL_GetProcAddress(#name);                                       \
    if (gl->name == NULL) {                                                                       \
        *missing = #name;                                                                         \
        return false;                                                                             \
    }
    GL33_FUNCS(GL33_LOAD)
#undef GL33_LOAD
    return true;
}
