#include <trx/gl/sampler.h>

#include <trx/debug.h>
#include <trx/gl/utils.h>

void TRX_GL_Sampler_Init(TRX_GL_SAMPLER *sampler)
{
    ASSERT(sampler != nullptr);
    glGenSamplers(1, &sampler->id);
    TRX_GL_CheckError();
    sampler->initialized = true;
}

void TRX_GL_Sampler_Close(TRX_GL_SAMPLER *sampler)
{
    ASSERT(sampler != nullptr);
    if (sampler->initialized) {
        glDeleteSamplers(1, &sampler->id);
        TRX_GL_CheckError();
    }
    sampler->initialized = false;
}

void TRX_GL_Sampler_Bind(TRX_GL_SAMPLER *sampler, GLuint unit)
{
    ASSERT(sampler != nullptr);
    ASSERT(sampler->initialized);
    glBindSampler(unit, sampler->id);
    TRX_GL_CheckError();
}

GLuint TRX_GL_Sampler_GetBound(const GLuint unit)
{
    // GL_SAMPLER_BINDING has no indexed form; it reads the active unit.
    GLint active_unit = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_unit);
    glActiveTexture(GL_TEXTURE0 + unit);
    GLint sampler = 0;
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glActiveTexture(active_unit);
    TRX_GL_CheckError();
    return (GLuint)sampler;
}

void TRX_GL_Sampler_Parameteri(
    TRX_GL_SAMPLER *sampler, GLenum pname, GLint param)
{
    ASSERT(sampler != nullptr);
    ASSERT(sampler->initialized);
    glSamplerParameteri(sampler->id, pname, param);
    TRX_GL_CheckError();
}

void TRX_GL_Sampler_Parameterf(
    TRX_GL_SAMPLER *sampler, GLenum pname, GLfloat param)
{
    ASSERT(sampler != nullptr);
    ASSERT(sampler->initialized);
    glSamplerParameterf(sampler->id, pname, param);
    TRX_GL_CheckError();
}
