#pragma once

#include <trx/core/colors.h>
#include <trx/game/matrix.h>
#include <trx/game/rooms/types.h>
#include <trx/gl/api.h>
#include <trx/vr/session.h>

typedef struct {
    int32_t ls_adder;
    int32_t ls_divider;
    XYZ_32 ls_vector_view;
    RGB_F tr3_ambient;
    RGB_F tr3_light_color[3];
    XYZ_32 tr3_light_dir_view[3];
    struct {
        // Index into the TR4 per-scene staged light pool; -1 = none.
        int32_t handle;
    } tr4;
} OUTPUT_LIGHT_INFO;

typedef struct {
    // Contiguous: created/destroyed with a single glGenBuffers/glDeleteBuffers
    // call starting at `general`.
    GLuint general;
    GLuint matrices;
    GLuint lights;
    GLuint fog_bulbs;
    void *priv;
} OUTPUT_UNIFORMS;

OUTPUT_UNIFORMS *Output_Uniforms_Create(void);
void Output_Uniforms_Free(OUTPUT_UNIFORMS *uniforms);

void Output_Uniforms_UploadGeneral(const OUTPUT_UNIFORMS *uniforms);
void Output_Uniforms_UploadOrthoMatrix(const OUTPUT_UNIFORMS *uniforms);
void Output_Uniforms_UploadViewMatrix(
    const OUTPUT_UNIFORMS *uniforms, const MATRIX *matrix);
// Uploads the view of one eye of a VR headset: the given view, which is the
// head's, moved to where the eye is, with the eye's own projection.
void Output_Uniforms_UploadEyeMatrix(
    const OUTPUT_UNIFORMS *uniforms, const MATRIX *matrix, const VR_EYE *eye);
// Says that what is drawn next is too far away to have a place of its own,
// such as the sky, and stays put around the camera.
void Output_Uniforms_SetDistant(bool is_distant);
void Output_Uniforms_UploadDynamicLights(const OUTPUT_UNIFORMS *uniforms);

void Output_Uniforms_UploadFogDistance(
    const OUTPUT_UNIFORMS *uniforms, float start, float end);
