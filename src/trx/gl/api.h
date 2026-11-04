#pragma once

// OpenGL as the platform offers it. Each platform says what it can do through
// the TRX_GL_HAS_* flags, and how a shader starts through
// TRX_GL_SHADER_PRELUDE, so that the renderer keeps away from what one lacks
// without fencing itself off by platform.
#ifdef __EMSCRIPTEN__
    #include <trx/gl/api_emscripten.h>
#else
    #include <trx/gl/api_generic.h>
#endif
