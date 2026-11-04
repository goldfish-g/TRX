#pragma once

// OpenGL ES 3.0, which the browser offers as WebGL 2. It lacks parts of the
// desktop core profile that the renderer calls, so the definitions below
// stand in for them: with the ES way of doing the same where there is one, or
// with nothing where the renderer only calls them behind a TRX_GL_HAS_* flag
// that rules them out here.

#include <GLES3/gl3.h>
#include <string.h>

// The context the renderer asks SDL for, and how its shaders start.
#define TRX_GL_CONTEXT_MAJOR_VERSION 3
#define TRX_GL_CONTEXT_MINOR_VERSION 0
#define TRX_GL_CONTEXT_PROFILE SDL_GL_CONTEXT_PROFILE_ES
#define TRX_GL_SHADER_PRELUDE                                                  \
    "#version 300 es\n"                                                        \
    "precision highp float;\n"                                                 \
    "precision highp int;\n"                                                   \
    "precision highp sampler2D;\n"                                             \
    "precision highp sampler2DArray;\n"                                        \
    "precision highp sampler3D;\n"

// WebGL has neither buffer textures nor base vertex draws to batch with.
#define TRX_GL_HAS_BATCHING 0
#define TRX_GL_HAS_GEOMETRY_SHADERS 0
#define TRX_GL_HAS_MULTISAMPLING 0
#define TRX_GL_HAS_TIMER_QUERIES 0
#define TRX_GL_HAS_DEBUG_OUTPUT 0
// The browser shows each frame in step with the display.
#define TRX_GL_HAS_SWAP_INTERVAL 0
// WebGL answers glGetError only once the GPU has caught up with every call
// before it, which asking after every call cannot afford.
#define TRX_GL_HAS_CHEAP_ERROR_CHECKS 0
// WebGL writes into a buffer only once no draw reads it any more, and on the
// tiled GPUs of phones the wait also splits the frame being drawn in two.
#define TRX_GL_HAS_UNSYNCHRONIZED_WRITES 0

#define GLAPIENTRY GL_APIENTRY
#define GL_DEBUG_SEVERITY_NOTIFICATION 0x826B
#define GL_STACK_OVERFLOW 0x0503
#define GL_STACK_UNDERFLOW 0x0504
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_MAX_TEXTURE_BUFFER_SIZE 0x8C2B
#define GL_TIME_ELAPSED 0x88BF
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_GEOMETRY_SHADER 0x8DD9
#define GL_MULTISAMPLE 0x809D
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100

// WebAssembly is little-endian, so the packed type lays out its bytes as the
// format names them, the same as unsigned bytes do.
#define GL_UNSIGNED_INT_8_8_8_8_REV GL_UNSIGNED_BYTE

// Emscripten links the GL functions directly, so there is nothing to load.
#define GLEW_OK 0
#define GLEW_ARB_texture_filter_anisotropic false
#define GLEW_EXT_texture_filter_anisotropic TRX_GL_ES_HasAnisotropy()

static inline GLenum glewInit(void)
{
    return GLEW_OK;
}

static inline bool TRX_GL_ES_HasAnisotropy(void)
{
    static int has_anisotropy = -1;
    if (has_anisotropy < 0) {
        const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
        has_anisotropy = extensions != nullptr
            && strstr(extensions, "GL_EXT_texture_filter_anisotropic")
                != nullptr;
    }
    return has_anisotropy;
}

// WebGL maps no buffers. Its writes wait for the draws that read the buffer,
// so the callers give it fresh storage to write into.
static inline void TRX_GL_WriteBufferRange(
    GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    glBufferSubData(target, offset, size, data);
}

static inline void glClearDepth(double depth)
{
    glClearDepthf((GLfloat)depth);
}

static inline void glDrawBuffer(GLenum buf)
{
    glDrawBuffers(1, &buf);
}

// A shader with a single output writes it to the first draw buffer.
static inline void glBindFragDataLocation(GLuint, GLuint, const GLchar *)
{
}

static inline void glGetActiveUniformName(
    GLuint program, GLuint index, GLsizei buf_size, GLsizei *length,
    GLchar *name)
{
    GLint size;
    GLenum type;
    glGetActiveUniform(program, index, buf_size, length, &size, &type, name);
}

static inline void glVertexAttribI1ui(GLuint index, GLuint x)
{
    glVertexAttribI4ui(index, x, 0, 0, 1);
}

// Every polygon is drawn filled, and the stand-in for glGetIntegerv says as
// much to whoever asks, so that saving and restoring the mode keeps working.
#define GL_POLYGON_MODE 0x0B40
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02

static inline void glPolygonMode(GLenum, GLenum)
{
}

static inline void TRX_GL_ES_GetIntegerv(GLenum pname, GLint *data)
{
    if (pname == GL_POLYGON_MODE) {
        data[0] = GL_FILL;
        data[1] = GL_FILL;
        return;
    }
    glGetIntegerv(pname, data);
}

#define glGetIntegerv TRX_GL_ES_GetIntegerv

static inline void glTexBuffer(GLenum, GLenum, GLuint)
{
}

static inline void glTexImage2DMultisample(
    GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean)
{
}

static inline void glDrawElementsBaseVertex(
    GLenum, GLsizei, GLenum, const void *, GLint)
{
}

static inline void glMultiDrawElementsBaseVertex(
    GLenum, const GLsizei *, GLenum, const void *const *, GLsizei,
    const GLint *)
{
}

static inline void glGetQueryObjectiv(GLuint, GLenum, GLint *params)
{
    *params = 0;
}

static inline void glGetQueryObjectui64v(GLuint, GLenum, GLuint64 *params)
{
    *params = 0;
}
