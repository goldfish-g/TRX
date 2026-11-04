#pragma once

// The desktop OpenGL 3.3 core profile, loaded through GLEW.

#include <GL/glew.h>
#include <string.h>

// The context the renderer asks SDL for, and how its shaders start.
#define TRX_GL_CONTEXT_MAJOR_VERSION 3
#define TRX_GL_CONTEXT_MINOR_VERSION 3
#define TRX_GL_CONTEXT_PROFILE SDL_GL_CONTEXT_PROFILE_CORE
#define TRX_GL_SHADER_PRELUDE "#version 330 core\n"

// Batches draw many instances at once, reading their vertices through buffer
// textures and placing them with base vertex draws.
#define TRX_GL_HAS_BATCHING 1
// Affine mapping subdivides the faces in a geometry stage.
#define TRX_GL_HAS_GEOMETRY_SHADERS 1
// Framebuffers smooth their edges through multisample textures.
#define TRX_GL_HAS_MULTISAMPLING 1
#define TRX_GL_HAS_TIMER_QUERIES 1
#define TRX_GL_HAS_DEBUG_OUTPUT 1
// Whether the game picks when the frames it draws are shown.
#define TRX_GL_HAS_SWAP_INTERVAL 1
// Whether glGetError answers without waiting for the GPU, cheaply enough to
// ask after every call.
#define TRX_GL_HAS_CHEAP_ERROR_CHECKS 1
// Whether a write into a buffer can go ahead of the draws that still read
// other parts of it, so that a ring of uploads can share one buffer.
#define TRX_GL_HAS_UNSYNCHRONIZED_WRITES 1

// Writes into a range of a buffer that no draw still reads, without waiting
// for the GPU.
static inline void TRX_GL_WriteBufferRange(
    GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    void *mapped = glMapBufferRange(
        target, offset, size,
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT
            | GL_MAP_UNSYNCHRONIZED_BIT);
    memcpy(mapped, data, size);
    glUnmapBuffer(target);
}
