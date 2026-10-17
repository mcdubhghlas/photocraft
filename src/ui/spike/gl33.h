/* The slice of OpenGL 3.3 core the UI renderer uses, loaded at run time through SDL.
 *
 * Types and enum values are the fixed Khronos ABI, declared here so no platform GL header is
 * needed (macOS has no glcorearb.h; Linux headers vary). Load once per context with
 * gl33_load() after SDL_GL_MakeCurrent; the table is then read-only. */
#ifndef PC_UI_GL33_H
#define PC_UI_GL33_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;

#define GL_NO_ERROR 0
#define GL_TRIANGLES 0x0004
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_VERSION 0x1F02
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGBA8 0x8058
#define GL_RGBA32F 0x8814
#define GL_FUNC_ADD 0x8006
#define GL_TEXTURE0 0x84C0
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_NEAREST 0x2600

/* X(return type, name, parameter list) for every function we load. */
#define GL33_FUNCS(X)                                                                             \
    X(void, glGetIntegerv, (GLenum name, GLint *value))                                             \
    X(GLenum, glGetError, (void))                                                                 \
    X(const GLubyte *, glGetString, (GLenum name))                                                \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                 \
    X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                  \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                           \
    X(void, glClear, (GLbitfield mask))                                                           \
    X(void, glEnable, (GLenum cap))                                                               \
    X(void, glDisable, (GLenum cap))                                                              \
    X(void, glBlendEquation, (GLenum mode))                                                       \
    X(void, glBlendFunc, (GLenum s, GLenum d))                                                    \
    X(void, glPixelStorei, (GLenum pname, GLint param))                                           \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p))  \
    X(GLuint, glCreateShader, (GLenum type))                                                      \
    X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar *const *src, const GLint *len))    \
    X(void, glCompileShader, (GLuint s))                                                          \
    X(void, glGetShaderiv, (GLuint s, GLenum pname, GLint *out))                                  \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei *len, GLchar *log))               \
    X(void, glDeleteShader, (GLuint s))                                                           \
    X(GLuint, glCreateProgram, (void))                                                            \
    X(void, glAttachShader, (GLuint p, GLuint s))                                                 \
    X(void, glLinkProgram, (GLuint p))                                                            \
    X(void, glGetProgramiv, (GLuint p, GLenum pname, GLint *out))                                 \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei *len, GLchar *log))              \
    X(void, glDeleteProgram, (GLuint p))                                                          \
    X(void, glUseProgram, (GLuint p))                                                             \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar *name))                                \
    X(GLint, glGetAttribLocation, (GLuint p, const GLchar *name))                                 \
    X(void, glUniform1i, (GLint loc, GLint v))                                                    \
    X(void, glUniform2f, (GLint loc, GLfloat x, GLfloat y))                                         \
    X(void, glUniform4f, (GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w))                    \
    X(void, glUniformMatrix4fv, (GLint loc, GLsizei n, GLboolean t, const GLfloat *v))           \
    X(void, glGenBuffers, (GLsizei n, GLuint *out))                                               \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint *b))                                        \
    X(void, glBindBuffer, (GLenum target, GLuint b))                                              \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))       \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *out))                                          \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *v))                                   \
    X(void, glBindVertexArray, (GLuint v))                                                        \
    X(void, glEnableVertexAttribArray, (GLuint i))                                                \
    X(void, glVertexAttribPointer, (GLuint i, GLint n, GLenum t, GLboolean norm, GLsizei stride,  \
                                    const void *off))                                             \
    X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *off))          \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                                \
    X(void, glGenTextures, (GLsizei n, GLuint *out))                                              \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *t))                                       \
    X(void, glBindTexture, (GLenum target, GLuint t))                                             \
    X(void, glActiveTexture, (GLenum unit))                                                       \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint v))                              \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint b, \
                           GLenum fmt, GLenum type, const void *data))                            \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *out))                                          \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *f))                                   \
    X(void, glBindFramebuffer, (GLenum target, GLuint f))                                         \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum att, GLenum textarget, GLuint t,      \
                                     GLint level))                                                \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))

#define GL33_DECLARE(ret, name, params) ret(*name) params;
typedef struct gl33 {
    GL33_FUNCS(GL33_DECLARE)
} gl33;
#undef GL33_DECLARE

/* Load every function for the current context. On failure returns false and sets *missing to
 * the first name that could not be loaded. */
bool gl33_load(gl33 *gl, const char **missing);

#endif
