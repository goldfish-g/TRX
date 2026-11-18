#include <trx/game/output/scene_compositor.h>

#include <trx/config.h>
#include <trx/core/utils.h>
#include <trx/core/vector.h>
#include <trx/debug.h>
#include <trx/game/output.h>
#include <trx/game/output/lights/priv.h>
#include <trx/game/output/overlay.h>
#include <trx/game/output/shaders/mesh.h>
#include <trx/game/output/shaders/ui.h>
#include <trx/game/output/textures.h>
#include <trx/game/output/textures_gl.h>
#include <trx/game/output/uniforms.h>
#include <trx/game/shell.h>
#include <trx/game/viewport.h>
#include <trx/gl/context.h>
#include <trx/gl/gpu_timer.h>
#include <trx/gl/renderer.h>
#include <trx/gl/utils.h>
#include <trx/vr/session.h>

#include <math.h>

#define M_PROCESS_SOURCES(p, func, ...)                                        \
    do {                                                                       \
        for (int32_t i = 0; i < p->sources->count; i++) {                      \
            const SCENE_SOURCE *const source =                                 \
                *(SCENE_SOURCE **)Vector_Get(p->sources, i);                   \
            if (source->func != nullptr) {                                     \
                source->func(source, ##__VA_ARGS__);                           \
            }                                                                  \
        }                                                                      \
    } while (0)

typedef struct {
    VECTOR *sources;
    GLuint sampler_id;
    int32_t capture_depth;
    // Whether the world is drawn for the two eyes of a VR headset, and the
    // framebuffers it is drawn into while it is.
    bool is_stereo;
    bool is_distant;
    int32_t eye;
    VR_SCREEN eye_target;
    // How far the world loses its colors once it is drawn, as it does behind
    // a menu, and what it is tinted with.
    float tint_amount;
    RGB_F tint;
    // How far the world fades to black once it is drawn, where a screen
    // would have a black sheet laid over it.
    float fade;
    // Whether the eye's picture is held, between the world being drawn into
    // it and the end of the frame, for what the frame still does to it.
    bool has_eye;
    struct {
        GLuint program;
        GLuint vao;
        GLuint buffer;
        // Which eye's corners the buffer holds, and where they were read
        // from, to tell when they change.
        int32_t eye;
        const float *corners;
        int32_t corner_count;
    } hider;
    struct {
        GLuint program;
        GLuint texture;
        GLuint vao;
        GLint loc_amount;
        GLint loc_tint;
        GLint loc_fade;
        int32_t width;
        int32_t height;
    } tinter;
} M_PRIV;

static M_PRIV m_Priv = {};

_Static_assert(
    SCENE_PASS_COUNT <= TRX_GL_GPU_TIMER_SLOTS,
    "The GPU timer has no slot for every scene pass");

static void M_SetSamplerFilter(
    const GLuint sampler, const TEXTURE_FILTER filter)
{
    const GLenum gl_filter =
        filter == TEXTURE_FILTER_BILINEAR ? GL_LINEAR : GL_NEAREST;
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, gl_filter);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, gl_filter);
}

static void M_SetupScene(const M_PRIV *const p)
{
    Output_MeshShader_Bind(Output_GetMeshShader());
    Output_Uniforms_UploadViewMatrix(Output_GetUniforms(), &g_ViewMatrix);
    Output_Lights_PrepareScene();
    glEnable(GL_BLEND);
    glBlendFunc(
        GL_ONE,
        g_Config.rendering.enable_wireframe ? GL_ZERO : GL_ONE_MINUS_SRC_ALPHA);
    M_SetSamplerFilter(p->sampler_id, g_Config.rendering.texture_filter);
}

static void M_SetupUI(const M_PRIV *const p)
{
    Output_UIShader_Bind(Output_GetUIShader());
    Output_Uniforms_UploadOrthoMatrix(Output_GetUniforms());
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    M_SetSamplerFilter(p->sampler_id, g_Config.rendering.ui_filter);
    glClear(GL_DEPTH_BUFFER_BIT);
}

static void M_RenderSourcePass(const M_PRIV *const p, const SCENE_PASS pass)
{
    TRX_GL_GpuTimer_Begin(pass);
    for (int32_t i = 0; i < p->sources->count; i++) {
        const SCENE_SOURCE *const source =
            *(SCENE_SOURCE **)Vector_Get(p->sources, i);
        if (source->is_dirty != nullptr && source->is_dirty(source, pass)) {
            ASSERT(source->render_pass != nullptr);
            source->render_pass(source, pass);
        }
    }
    TRX_GL_GpuTimer_End();
}

static bool M_IsSourceDirty(const M_PRIV *const p, const SCENE_PASS pass)
{
    for (int32_t i = 0; i < p->sources->count; i++) {
        const SCENE_SOURCE *const source =
            *(SCENE_SOURCE **)Vector_Get(p->sources, i);
        if (source->is_dirty != nullptr && source->is_dirty(source, pass)) {
            return true;
        }
    }
    return false;
}

static bool M_IsAnySourceDirty(const M_PRIV *const p)
{
    for (SCENE_PASS pass = 0; pass < SCENE_PASS_COUNT; pass++) {
        if (M_IsSourceDirty(p, pass)) {
            return true;
        }
    }
    return false;
}

static void M_PrepareScene(const M_PRIV *const p)
{
#ifndef __APPLE__
    const float line_width = g_Config.rendering.enable_wireframe
        ? g_Config.rendering.wireframe_width
        : 1.0f;
    glLineWidth(line_width * Viewport_GetSupersamplingFactor());
    TRX_GL_CheckError();
#endif

    if (GLEW_EXT_texture_filter_anisotropic
        || GLEW_ARB_texture_filter_anisotropic) {
        glSamplerParameterf(
            p->sampler_id, GL_TEXTURE_MAX_ANISOTROPY_EXT,
            g_Config.rendering.anisotropy_filter);
        TRX_GL_CheckError();
    }

    Output_Uniforms_UploadGeneral(Output_GetUniforms());
    Output_Uniforms_UploadDynamicLights(Output_GetUniforms());
    Output_SetCurrentRoom(nullptr);
}

// Draws the passes that make up the world, from the background to what is
// blended over it, with the view that is uploaded.
static void M_RenderWorldPasses(const M_PRIV *const p)
{
    glDisable(GL_DEPTH_TEST);
    if (M_IsSourceDirty(p, SCENE_PASS_BACKGROUND)) {
        M_RenderSourcePass(p, SCENE_PASS_BACKGROUND);
    }

    OUTPUT_MESH_SHADER *const shader = Output_GetMeshShader();
    Output_MeshShader_Bind(shader);

    glPolygonMode(
        GL_FRONT_AND_BACK,
        g_Config.rendering.enable_wireframe ? GL_LINE : GL_FILL);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);

    if (M_IsSourceDirty(p, SCENE_PASS_OPAQUE)
        || M_IsSourceDirty(p, SCENE_PASS_TRANSPARENT)
        || M_IsSourceDirty(p, SCENE_PASS_BLEND_SUB)
        || M_IsSourceDirty(p, SCENE_PASS_BLEND_ADD)) {
        glEnable(GL_CULL_FACE);
        Output_MeshShader_UploadAlphaDiscard(shader, true);
        M_RenderSourcePass(p, SCENE_PASS_OPAQUE);
        Output_MeshShader_UploadAlphaDiscard(shader, false);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        M_RenderSourcePass(p, SCENE_PASS_TRANSPARENT);
        SceneCompositor_SetSubtractiveBlend();
        M_RenderSourcePass(p, SCENE_PASS_BLEND_SUB);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        M_RenderSourcePass(p, SCENE_PASS_BLEND_ADD);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
    }
}

// Marks the parts of the eye's picture that its lens never shows as being
// right in front of the eye, so that nothing of the world is drawn there.
// The player cannot see them, and they are a good share of the picture. The
// eye's framebuffer is bound when this is called, with its depth just
// cleared.
static void M_HideUnseenParts(M_PRIV *const p)
{
    const float *corners = nullptr;
    const int32_t corner_count = VR_Session_GetHiddenArea(p->eye, &corners);
    if (corner_count == 0) {
        return;
    }

    if (p->hider.program == 0) {
        const char *const vertex = TRX_GL_SHADER_PRELUDE
            "layout(location = 0) in vec2 inPos;\n"
            "void main() {\n"
            "    gl_Position = vec4(inPos, -1.0, 1.0);\n"
            "}\n";
        const char *const fragment = TRX_GL_SHADER_PRELUDE
            "out vec4 outColor;\n"
            "void main() {\n"
            "    outColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "}\n";
        const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vertex, nullptr);
        glCompileShader(vs);
        const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fragment, nullptr);
        glCompileShader(fs);
        p->hider.program = glCreateProgram();
        glAttachShader(p->hider.program, vs);
        glAttachShader(p->hider.program, fs);
        glLinkProgram(p->hider.program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glGenVertexArrays(1, &p->hider.vao);
        glGenBuffers(1, &p->hider.buffer);
        p->hider.eye = -1;
    }

    // The rest of the renderer keeps track of what it has bound, and must
    // find all of it as it left it.
    GLint old_program = 0;
    GLint old_vao = 0;
    GLint old_buffer = 0;
    GLint old_depth_func = GL_LESS;
    GLint old_polygon_mode[2] = { GL_FILL, GL_FILL };
    GLboolean old_depth_mask = GL_TRUE;
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
    glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func);
    glGetIntegerv(GL_POLYGON_MODE, old_polygon_mode);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &old_depth_mask);
    const GLboolean had_depth_test = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean had_blend = glIsEnabled(GL_BLEND);
    const GLboolean had_cull_face = glIsEnabled(GL_CULL_FACE);
    const GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);

    glBindVertexArray(p->hider.vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->hider.buffer);
    if (p->hider.eye != p->eye || p->hider.corners != corners
        || p->hider.corner_count != corner_count) {
        glBufferData(
            GL_ARRAY_BUFFER, corner_count * 2 * sizeof(float), corners,
            GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        p->hider.eye = p->eye;
        p->hider.corners = corners;
        p->hider.corner_count = corner_count;
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(p->hider.program);
    glDrawArrays(GL_TRIANGLES, 0, corner_count);

    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_buffer);
    glBindVertexArray((GLuint)old_vao);
    glUseProgram((GLuint)old_program);
    glPolygonMode(GL_FRONT_AND_BACK, (GLenum)old_polygon_mode[0]);
    glDepthFunc((GLenum)old_depth_func);
    glDepthMask(old_depth_mask);
    if (!had_depth_test) {
        glDisable(GL_DEPTH_TEST);
    }
    if (had_blend) {
        glEnable(GL_BLEND);
    }
    if (had_cull_face) {
        glEnable(GL_CULL_FACE);
    }
    if (had_scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
}

// Draws the world for the eye the frame is drawn for, as that eye sees it
// from the player's head.
static void M_RenderWorldToEye(M_PRIV *const p)
{
    const VR_VIEW *const head = VR_Session_GetView();
    ASSERT(head != nullptr);
    SceneCompositor_BindTextures();
    glBindFramebuffer(GL_FRAMEBUFFER, p->eye_target.fbo);
    glViewport(0, 0, p->eye_target.width, p->eye_target.height);
    M_SetupScene(p);
    VR_EYE view = head->eyes[p->eye];
    if (p->is_distant) {
        view.offset[0] = 0.0f;
        view.offset[1] = 0.0f;
        view.offset[2] = 0.0f;
    }
    Output_Uniforms_UploadEyeMatrix(Output_GetUniforms(), &g_ViewMatrix, &view);
    M_RenderWorldPasses(p);
    // Each flush stands in front of the last, as on a screen.
    glClear(GL_DEPTH_BUFFER_BIT);
    M_HideUnseenParts(p);
    TRX_GL_Renderer_BindGeometryFbo();
    TRX_GL_Context_SwitchToViewport(VIEWPORT_GAME);
}

// Draws the flat pictures: the backgrounds of the menus, and what is laid
// over the world. On the screen of a VR headset they have no depth of their
// own, and are put as far back as anything gets, so that every model drawn
// over them is in front of them.
static void M_RenderOverlayPass(const M_PRIV *const p)
{
    const int32_t eye = VR_Session_GetDrawnEye();
    if (eye < 0 || Output_Overlay_IsWorldAround()) {
        M_RenderSourcePass(p, SCENE_PASS_OVERLAY);
        return;
    }
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    const float shift =
        (eye == 0 ? -1.0f : 1.0f) * VR_SCREEN_PARALLAX * viewport[2] / 2.0f;
    glViewport(
        viewport[0] + (GLint)lroundf(shift), viewport[1], viewport[2],
        viewport[3]);
    M_RenderSourcePass(p, SCENE_PASS_OVERLAY);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

static void M_RenderScenePasses(M_PRIV *const p)
{
    if (!M_IsAnySourceDirty(p)) {
        return;
    }

    if (p->is_stereo) {
        M_RenderWorldToEye(p);
        // What is left is drawn flat over the scene, which has no place
        // between the eyes yet.
        return;
    }

    Output_ApplyPendingClear();
    TRX_GL_Renderer_InvalidateScene();
    SceneCompositor_BindTextures();
    M_SetupScene(p);
    M_RenderWorldPasses(p);

    if (M_IsSourceDirty(p, SCENE_PASS_OVERLAY)) {
        M_RenderOverlayPass(p);
    }

    if (M_IsSourceDirty(p, SCENE_PASS_UI)) {
        M_SetupUI(p);
        // On the panel around a player in VR the text comes forward, by
        // being moved towards the other eye's side.
        const int32_t shift = SceneCompositor_GetPanelTextShift();
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        glViewport(viewport[0] + shift, viewport[1], viewport[2], viewport[3]);
        M_RenderSourcePass(p, SCENE_PASS_UI);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }
}

static bool M_IsActive(void)
{
    return !Output_IsHeadless() || m_Priv.capture_depth > 0
        || Shell_GetArgs()->debug_render_performance
        || TRX_GL_Context_GetScheduledScreenshotPath() != nullptr;
}

// Takes the colors out of the picture in the given framebuffer where it
// stands, and tints what is left. The picture is copied aside first, since
// it cannot be read while it is drawn into.
static void M_Tint(
    M_PRIV *const p, const GLuint fbo, const int32_t width,
    const int32_t height)
{
    if (p->tinter.program == 0) {
        const char *const vertex = TRX_GL_SHADER_PRELUDE
            "out vec2 vertUV;\n"
            "void main() {\n"
            "    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    vertUV = pos;\n"
            "    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        const char *const fragment = TRX_GL_SHADER_PRELUDE
            "uniform sampler2D uTexture;\n"
            "uniform float uAmount;\n"
            "uniform vec3 uTint;\n"
            "uniform float uFade;\n"
            "in vec2 vertUV;\n"
            "out vec4 outColor;\n"
            "void main() {\n"
            "    vec3 color = texture(uTexture, vertUV).rgb;\n"
            "    float gray = dot(color, vec3(0.299, 0.587, 0.114));\n"
            "    outColor = vec4(mix(color, vec3(gray), uAmount) * uTint "
            "* (1.0 - uFade), 1.0);\n"
            "}\n";
        const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vertex, nullptr);
        glCompileShader(vs);
        const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fragment, nullptr);
        glCompileShader(fs);
        p->tinter.program = glCreateProgram();
        glAttachShader(p->tinter.program, vs);
        glAttachShader(p->tinter.program, fs);
        glLinkProgram(p->tinter.program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        p->tinter.loc_amount =
            glGetUniformLocation(p->tinter.program, "uAmount");
        p->tinter.loc_tint = glGetUniformLocation(p->tinter.program, "uTint");
        p->tinter.loc_fade = glGetUniformLocation(p->tinter.program, "uFade");
        glGenVertexArrays(1, &p->tinter.vao);
        glGenTextures(1, &p->tinter.texture);
    }

    // The rest of the renderer keeps track of what it has bound, and must
    // find all of it as it left it.
    GLint old_program = 0;
    GLint old_vao = 0;
    GLint old_texture = 0;
    GLint old_sampler = 0;
    GLint old_active = 0;
    GLint old_polygon_mode[2] = { GL_FILL, GL_FILL };
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glGetIntegerv(GL_POLYGON_MODE, old_polygon_mode);
    const GLboolean had_depth_test = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean had_blend = glIsEnabled(GL_BLEND);
    const GLboolean had_cull_face = glIsEnabled(GL_CULL_FACE);
    const GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGetIntegerv(GL_SAMPLER_BINDING, &old_sampler);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, p->tinter.texture);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (p->tinter.width != width || p->tinter.height != height) {
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
            GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p->tinter.width = width;
        p->tinter.height = height;
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);

    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(p->tinter.program);
    glUniform1f(p->tinter.loc_amount, p->tint_amount);
    if (p->tint_amount > 0.0f) {
        glUniform3f(p->tinter.loc_tint, p->tint.r, p->tint.g, p->tint.b);
    } else {
        glUniform3f(p->tinter.loc_tint, 1.0f, 1.0f, 1.0f);
    }
    glUniform1f(p->tinter.loc_fade, p->fade);
    glBindVertexArray(p->tinter.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray((GLuint)old_vao);
    glUseProgram((GLuint)old_program);
    glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
    glBindSampler(0, (GLuint)old_sampler);
    glActiveTexture((GLenum)old_active);
    glPolygonMode(GL_FRONT_AND_BACK, (GLenum)old_polygon_mode[0]);
    if (had_depth_test) {
        glEnable(GL_DEPTH_TEST);
    }
    if (had_blend) {
        glEnable(GL_BLEND);
    }
    if (had_cull_face) {
        glEnable(GL_CULL_FACE);
    }
    if (had_scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    TRX_GL_Renderer_BindGeometryFbo();
    TRX_GL_Context_SwitchToViewport(VIEWPORT_GAME);
}

void SceneCompositor_SetWorldTint(const float amount, const RGB_F tint)
{
    m_Priv.tint_amount = amount;
    m_Priv.tint = tint;
}

void SceneCompositor_AddWorldFade(const float opacity)
{
    M_PRIV *const p = &m_Priv;
    p->fade = MIN(MAX(p->fade, opacity), 1.0f);
}

void SceneCompositor_TintScene(void)
{
    M_PRIV *const p = &m_Priv;
    if (p->has_eye || p->tint_amount <= 0.0f) {
        return;
    }
    const VIEWPORT_RECT game = Viewport_GetRect(VIEWPORT_GAME);
    M_Tint(p, TRX_GL_Renderer_GetGeometryFboId(), game.width, game.height);
    TRX_GL_Renderer_InvalidateScene();
    p->tint_amount = 0.0f;
}

int32_t SceneCompositor_GetPanelTextShift(void)
{
    const int32_t eye = VR_Session_GetDrawnEye();
    if (eye < 0 || !Output_Overlay_IsWorldBehind()) {
        return 0;
    }
    const float half_width = Viewport_GetWidth(VIEWPORT_UI) / 2.0f;
    return (int32_t)lroundf(
        (eye == 0 ? 1.0f : -1.0f) * VR_PANEL_TEXT_PARALLAX * half_width);
}

bool SceneCompositor_IsStereo(void)
{
    return m_Priv.is_stereo;
}

void SceneCompositor_BeginWorld(void)
{
    M_PRIV *const p = &m_Priv;
    const int32_t eye = VR_Session_GetDrawnEye();
    // A capture is a picture of the game as a screen shows it.
    if (p->is_stereo || p->has_eye || !M_IsActive() || p->capture_depth > 0
        || eye < 0 || VR_Session_GetMode() != VR_MODE_INVOLVED
        || VR_Session_GetView() == nullptr
        || !VR_Session_AcquireEye(eye, &p->eye_target)) {
        return;
    }
    const RGBA_F background = Output_GetBackgroundColor();
    glBindFramebuffer(GL_FRAMEBUFFER, p->eye_target.fbo);
    glViewport(0, 0, p->eye_target.width, p->eye_target.height);
    glClearBufferfv(GL_COLOR, 0, &background.r);
    glClear(GL_DEPTH_BUFFER_BIT);
    p->eye = eye;
    M_HideUnseenParts(p);
    TRX_GL_Renderer_BindGeometryFbo();
    TRX_GL_Context_SwitchToViewport(VIEWPORT_GAME);
    p->is_stereo = true;
}

void SceneCompositor_EndWorld(void)
{
    M_PRIV *const p = &m_Priv;
    if (!p->is_stereo) {
        return;
    }
    // The picture stays in hand: the rest of the frame may still ask for it
    // to lose its colors or to fade.
    p->is_stereo = false;
    p->has_eye = true;
}

void SceneCompositor_FinishEye(void)
{
    M_PRIV *const p = &m_Priv;
    if (!p->has_eye) {
        p->fade = 0.0f;
        return;
    }
    if (p->tint_amount > 0.0f || p->fade > 0.0f) {
        M_Tint(p, p->eye_target.fbo, p->eye_target.width, p->eye_target.height);
    }
    p->tint_amount = 0.0f;
    p->fade = 0.0f;
    VR_Session_ReleaseEye(p->eye);
    p->has_eye = false;
}

void SceneCompositor_BeginCapture(void)
{
    m_Priv.capture_depth++;
}

void SceneCompositor_EndCapture(void)
{
    ASSERT(m_Priv.capture_depth > 0);
    m_Priv.capture_depth--;
}

void SceneCompositor_Init(void)
{
    M_PRIV *const p = &m_Priv;
    TRX_GL_GpuTimer_Init(Shell_GetArgs()->debug_render_performance);
    p->sources = Vector_Create(sizeof(SCENE_SOURCE *));
    glGenSamplers(1, &p->sampler_id);
    glSamplerParameteri(p->sampler_id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(p->sampler_id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(p->sampler_id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(p->sampler_id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    TRX_GL_CheckError();
}

void SceneCompositor_Shutdown(void)
{
    M_PRIV *const p = &m_Priv;
    if (p->sources != nullptr) {
        Vector_Free(p->sources);
        p->sources = nullptr;
    }
    if (p->sampler_id != 0) {
        glDeleteSamplers(1, &p->sampler_id);
        p->sampler_id = 0;
    }
    TRX_GL_GpuTimer_Shutdown();
}

void SceneCompositor_BeginScene(void)
{
    M_PRIV *const p = &m_Priv;
    if (!M_IsActive()) {
        return;
    }
    M_PrepareScene(p);
    M_PROCESS_SOURCES(p, render_begin);
}

void SceneCompositor_Flush(void)
{
    M_PRIV *const p = &m_Priv;
    if (!M_IsActive()) {
        M_PROCESS_SOURCES(p, render_begin);
        return;
    }
    M_RenderScenePasses(p);
    M_PROCESS_SOURCES(p, render_end);
    M_PROCESS_SOURCES(p, render_begin);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void SceneCompositor_FlushDistant(void)
{
    M_PRIV *const p = &m_Priv;
    // On a screen, both eyes get the same picture of it, set as far apart
    // as anything goes: the camera does not move to the eye's side for it.
    const float parallax = Output_GetStereoParallax();
    const float convergence = Output_GetStereoConvergence();
    Output_SetStereoDepth(parallax, 0.0f);
    p->is_distant = true;
    Output_Uniforms_SetDistant(true);
    SceneCompositor_Flush();
    Output_Uniforms_SetDistant(false);
    p->is_distant = false;
    Output_SetStereoDepth(parallax, convergence);
}

void SceneCompositor_EndScene(void)
{
    M_PRIV *const p = &m_Priv;
    if (!M_IsActive()) {
        M_PROCESS_SOURCES(p, render_begin);
        return;
    }
    M_RenderScenePasses(p);
    M_PROCESS_SOURCES(p, render_end);
}

void SceneCompositor_AnimateTextures(void)
{
    M_PRIV *const p = &m_Priv;
    M_PROCESS_SOURCES(p, animate_textures);
}

void SceneCompositor_AddSource(const SCENE_SOURCE *const source)
{
    M_PRIV *const p = &m_Priv;
    Vector_Add(p->sources, &source);
}

void SceneCompositor_BindTextures(void)
{
    const M_PRIV *const p = &m_Priv;
    glBindSampler(0, p->sampler_id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, Output_Textures_GetAtlasTexture());
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, Output_Textures_GetEnvMapTexture());
}

void SceneCompositor_SetSamplerFilter(const TEXTURE_FILTER filter)
{
    M_PRIV *const p = &m_Priv;
    M_SetSamplerFilter(p->sampler_id, filter);
}

void SceneCompositor_SetSubtractiveBlend(void)
{
    if (g_Config.rendering.enable_ps1_blending) {
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glBlendFunc(GL_ONE, GL_ONE);
    } else {
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
    }
}
