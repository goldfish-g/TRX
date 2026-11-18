// Reports what an OpenXR runtime offers to an OpenGL application, and proves
// it by opening a session on a desktop OpenGL 3.3 core context and drawing
// each eye a chequered picture in a colour of its own for a few seconds: the
// left eye green, the right eye magenta.
//
// It answers whether TRX can keep its OpenGL renderer on a given headset.
//
// The picture is costly for every pixel, and the probe reports how long the
// graphics card took over it, which shows whether foveated rendering takes
// hold. It asks the runtime for a foveated swapchain unless told "plain",
// and on the Steam Frame the saving comes from Valve's Vulkan layer:
//   VK_INSTANCE_LAYERS=VK_LAYER_VALVE_fdm_injection \
//     FDM_DEBUG=enable,synth,hi ./xr_probe 5 plain
// PROBE_SCALE=1.3 and PROBE_DEPTH=1 make the eyes larger and give them a
// depth buffer, as a game has.
//
// Build, on any Linux with SDL2 and the OpenXR loader:
//   cc xr_probe.c -o xr_probe $(pkg-config --cflags --libs sdl2) \
//     -lopenxr_loader -lGL -lX11 -lstdc++ -lm

#define GL_GLEXT_PROTOTYPES
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <X11/Xlib.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_MAX_IMAGES 8
#define M_DEFAULT_SECONDS 5
// How long the session may sit without drawing before the probe gives up.
#define M_IDLE_TIMEOUT_MS 60000

typedef struct {
    XrSwapchain handle;
    int32_t width;
    int32_t height;
    uint32_t image_count;
    XrSwapchainImageOpenGLKHR images[M_MAX_IMAGES];
} M_EYE;

static XrInstance m_Instance = XR_NULL_HANDLE;

static void M_Check(const XrResult result, const char *const what)
{
    if (XR_SUCCEEDED(result)) {
        return;
    }
    char name[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (m_Instance != XR_NULL_HANDLE) {
        xrResultToString(m_Instance, result, name);
    }
    printf("FAILED: %s: %s (%d)\n", what, name, result);
    exit(1);
}

static bool M_ListExtensions(const char *const wanted)
{
    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    XrExtensionProperties *const props = calloc(count, sizeof(*props));
    for (uint32_t i = 0; i < count; i++) {
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props);
    bool found = false;
    printf("== extensions the runtime offers (%u)\n", count);
    for (uint32_t i = 0; i < count; i++) {
        printf("  %s\n", props[i].extensionName);
        found |= strcmp(props[i].extensionName, wanted) == 0;
    }
    free(props);
    return found;
}

static bool M_IsOffered(const char *const wanted)
{
    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    XrExtensionProperties *const props = calloc(count, sizeof(*props));
    for (uint32_t i = 0; i < count; i++) {
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props);
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        found |= strcmp(props[i].extensionName, wanted) == 0;
    }
    free(props);
    return found;
}

static const char *M_ResultName(const XrResult result)
{
    static char name[XR_MAX_RESULT_STRING_SIZE];
    strcpy(name, "?");
    xrResultToString(m_Instance, result, name);
    return name;
}

// A picture that costs the GPU something for every pixel, so that drawing
// fewer of them shows in the time it takes.
static GLuint M_MakeHeavyProgram(void)
{
    const char *const vs =
        "#version 330 core\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "  uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        "  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    const char *const fs =
        "#version 330 core\n"
        "in vec2 uv;\n"
        "out vec4 color;\n"
        "uniform vec3 tint;\n"
        "void main() {\n"
        "  float a = 0.0;\n"
        "  for (int i = 0; i < 96; i++) {\n"
        "    a += sin(uv.x * float(i) + a) * cos(uv.y * float(i) - a);\n"
        "  }\n"
        "  vec2 g = floor(gl_FragCoord.xy);\n"
        "  float c = mod(g.x + g.y, 2.0);\n"
        "  color = vec4(tint * (0.3 + 0.7 * c) + a * 0.0001, 1.0);\n"
        "}\n";
    const GLuint program = glCreateProgram();
    const GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    const char *const sources[2] = { vs, fs };
    for (int32_t i = 0; i < 2; i++) {
        const GLuint shader = glCreateShader(types[i]);
        glShaderSource(shader, 1, &sources[i], nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            printf("FAILED: shader %d: %s\n", i, log);
            exit(1);
        }
        glAttachShader(program, shader);
    }
    glLinkProgram(program);
    return program;
}

// Reads the picture back and prints how sharp it is in each part of a grid,
// from 0 to 9: the chequers are crisp where every pixel was drawn, and
// smeared where the graphics card drew fewer and stretched them. The top
// row of the print is the top of the picture.
static void M_PrintSharpness(
    const int32_t eye, const int32_t width, const int32_t height)
{
    enum { GRID = 12 };
    uint8_t *const pixels = malloc((size_t)width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    double sums[GRID][GRID] = {};
    double max = 1.0;
    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x + 1 < width; x++) {
            const uint8_t *const p = &pixels[((size_t)y * width + x) * 4];
            const int32_t d = abs(p[0] + p[1] - p[4] - p[5]);
            sums[y * GRID / height][x * GRID / width] += d;
        }
    }
    for (int32_t y = 0; y < GRID; y++) {
        for (int32_t x = 0; x < GRID; x++) {
            max = sums[y][x] > max ? sums[y][x] : max;
        }
    }
    printf("== eye %d sharpness (max %.0f):\n", eye, max);
    for (int32_t y = GRID - 1; y >= 0; y--) {
        printf("   ");
        for (int32_t x = 0; x < GRID; x++) {
            printf(" %d", (int)(sums[y][x] * 9.0 / max + 0.5));
        }
        printf("\n");
    }
    free(pixels);
}

// Reports the part of each eye's picture that the lens never shows: how
// many triangles cover it, and what share of the picture they take up.
static void M_PrintMask(const XrSession session, const XrView *const views)
{
    XrResult (*get_mask)(
        XrSession, XrViewConfigurationType, uint32_t, XrVisibilityMaskTypeKHR,
        XrVisibilityMaskKHR *) = nullptr;
    xrGetInstanceProcAddr(
        m_Instance, "xrGetVisibilityMaskKHR", (PFN_xrVoidFunction *)&get_mask);
    for (uint32_t i = 0; i < 2 && get_mask != nullptr; i++) {
        XrVisibilityMaskKHR mask = { .type = XR_TYPE_VISIBILITY_MASK_KHR };
        XrResult result = get_mask(
            session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, i,
            XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);
        if (XR_FAILED(result) || mask.indexCountOutput == 0) {
            printf(
                "== eye %u hidden mask: %s, %u indices\n", i,
                M_ResultName(result), mask.indexCountOutput);
            continue;
        }
        mask.vertexCapacityInput = mask.vertexCountOutput;
        mask.indexCapacityInput = mask.indexCountOutput;
        mask.vertices = calloc(mask.vertexCountOutput, sizeof(XrVector2f));
        mask.indices = calloc(mask.indexCountOutput, sizeof(uint32_t));
        result = get_mask(
            session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, i,
            XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);
        double area = 0.0;
        float min_x = 9, max_x = -9, min_y = 9, max_y = -9;
        for (uint32_t j = 0; j + 2 < mask.indexCountOutput; j += 3) {
            const XrVector2f a = mask.vertices[mask.indices[j]];
            const XrVector2f b = mask.vertices[mask.indices[j + 1]];
            const XrVector2f c = mask.vertices[mask.indices[j + 2]];
            const double cross =
                (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
            area += (cross < 0 ? -cross : cross) / 2.0;
        }
        for (uint32_t j = 0; j < mask.vertexCountOutput; j++) {
            const XrVector2f v = mask.vertices[j];
            min_x = v.x < min_x ? v.x : min_x;
            max_x = v.x > max_x ? v.x : max_x;
            min_y = v.y < min_y ? v.y : min_y;
            max_y = v.y > max_y ? v.y : max_y;
        }
        const XrFovf fov = views[i].fov;
        const double whole = (tan(fov.angleRight) - tan(fov.angleLeft))
            * (tan(fov.angleUp) - tan(fov.angleDown));
        printf(
            "== eye %u hidden mask: %s, %u triangles, x %.3f..%.3f, y "
            "%.3f..%.3f, %.1f%% of the picture (tangents %.3f..%.3f, "
            "%.3f..%.3f)\n",
            i, M_ResultName(result), mask.indexCountOutput / 3, min_x, max_x,
            min_y, max_y, 100.0 * area / whole, tan(fov.angleLeft),
            tan(fov.angleRight), tan(fov.angleDown), tan(fov.angleUp));
        free(mask.vertices);
        free(mask.indices);
    }
}

static bool M_HasGLExtension(const char *const name)
{
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; i++) {
        if (strcmp((const char *)glGetStringi(GL_EXTENSIONS, i), name) == 0) {
            return true;
        }
    }
    return false;
}

int main(const int argc, char **const argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // The first argument is how many seconds to keep the picture up for. A
    // second one of "plain" leaves foveation alone, to compare against.
    const uint32_t draw_ms =
        1000 * (argc > 1 ? atoi(argv[1]) : M_DEFAULT_SECONDS);
    const bool want_foveation = argc <= 2 || strcmp(argv[2], "plain") != 0;

    const bool has_gl = M_ListExtensions(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    printf("== XR_KHR_opengl_enable offered: %s\n", has_gl ? "yes" : "NO");
    if (!has_gl) {
        return 1;
    }

    const char *extensions[8] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    uint32_t extension_count = 1;
    const bool has_foveation = want_foveation
        && M_IsOffered(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME)
        && M_IsOffered(XR_FB_FOVEATION_EXTENSION_NAME)
        && M_IsOffered(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
    const bool has_eye_foveation = has_foveation
        && M_IsOffered(XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME);
    const bool has_gaze =
        M_IsOffered(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    if (has_foveation) {
        extensions[extension_count++] =
            XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME;
        extensions[extension_count++] = XR_FB_FOVEATION_EXTENSION_NAME;
        extensions[extension_count++] =
            XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME;
    }
    if (has_eye_foveation) {
        extensions[extension_count++] =
            XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME;
    }
    if (has_gaze) {
        extensions[extension_count++] =
            XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME;
    }
    const bool has_mask = M_IsOffered(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);
    if (has_mask) {
        extensions[extension_count++] = XR_KHR_VISIBILITY_MASK_EXTENSION_NAME;
    }
    printf(
        "== foveation: asked %s, fixed %s, eye-tracked %s, gaze %s\n",
        want_foveation ? "yes" : "no", has_foveation ? "yes" : "no",
        has_eye_foveation ? "yes" : "no", has_gaze ? "yes" : "no");
    XrInstanceCreateInfo instance_info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {
            .applicationName = "trx_xr_probe",
            .applicationVersion = 1,
            .apiVersion = XR_MAKE_VERSION(1, 0, 0),
        },
        .enabledExtensionCount = extension_count,
        .enabledExtensionNames = extensions,
    };
    M_Check(xrCreateInstance(&instance_info, &m_Instance), "xrCreateInstance");

    XrInstanceProperties instance_props = { .type =
                                                XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(m_Instance, &instance_props);
    printf(
        "== runtime: %s %u.%u.%u\n", instance_props.runtimeName,
        XR_VERSION_MAJOR(instance_props.runtimeVersion),
        XR_VERSION_MINOR(instance_props.runtimeVersion),
        XR_VERSION_PATCH(instance_props.runtimeVersion));

    XrSystemId system_id;
    const XrSystemGetInfo system_info = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    M_Check(xrGetSystem(m_Instance, &system_info, &system_id), "xrGetSystem");

    XrSystemEyeGazeInteractionPropertiesEXT gaze_props = {
        .type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT,
    };
    XrSystemFoveationEyeTrackedPropertiesMETA eye_foveation_props = {
        .type = XR_TYPE_SYSTEM_FOVEATION_EYE_TRACKED_PROPERTIES_META,
    };
    XrSystemProperties system_props = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    if (has_gaze) {
        gaze_props.next = system_props.next;
        system_props.next = &gaze_props;
    }
    if (has_eye_foveation) {
        eye_foveation_props.next = system_props.next;
        system_props.next = &eye_foveation_props;
    }
    xrGetSystemProperties(m_Instance, system_id, &system_props);
    printf(
        "== system says: eye gaze %s, eye-tracked foveation %s\n",
        gaze_props.supportsEyeGazeInteraction ? "yes" : "no",
        eye_foveation_props.supportsFoveationEyeTracked ? "yes" : "no");
    printf(
        "== system: %s, max swapchain %ux%u, max layers %u\n",
        system_props.systemName,
        system_props.graphicsProperties.maxSwapchainImageWidth,
        system_props.graphicsProperties.maxSwapchainImageHeight,
        system_props.graphicsProperties.maxLayerCount);

    XrViewConfigurationView views_info[2] = {
        { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW },
        { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW },
    };
    uint32_t view_count = 0;
    M_Check(
        xrEnumerateViewConfigurationViews(
            m_Instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
            &view_count, views_info),
        "xrEnumerateViewConfigurationViews");
    for (uint32_t i = 0; i < view_count; i++) {
        printf(
            "== eye %u: recommended %ux%u, max %ux%u, samples %u (max %u)\n", i,
            views_info[i].recommendedImageRectWidth,
            views_info[i].recommendedImageRectHeight,
            views_info[i].maxImageRectWidth, views_info[i].maxImageRectHeight,
            views_info[i].recommendedSwapchainSampleCount,
            views_info[i].maxSwapchainSampleCount);
    }

    // The same context TRX asks for: OpenGL 3.3 core, through SDL on X11.
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("FAILED: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *const window = SDL_CreateWindow(
        "trx_xr_probe", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640,
        360, SDL_WINDOW_OPENGL);
    if (window == nullptr) {
        printf("FAILED: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    const SDL_GLContext context = SDL_GL_CreateContext(window);
    if (context == nullptr) {
        printf("FAILED: SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(0);
    printf("== GL_VERSION: %s\n", (const char *)glGetString(GL_VERSION));
    printf("== GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf(
        "== GL_OVR_multiview2: %s, GL_EXT_memory_object_fd: %s\n",
        M_HasGLExtension("GL_OVR_multiview2") ? "yes" : "no",
        M_HasGLExtension("GL_EXT_memory_object_fd") ? "yes" : "no");

    XrGraphicsRequirementsOpenGLKHR requirements = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR
    };
    // The loader exports only the core functions, so the instance hands out
    // the ones that extensions add.
    XrResult (*get_requirements)(
        XrInstance, XrSystemId, XrGraphicsRequirementsOpenGLKHR *) = nullptr;
    M_Check(
        xrGetInstanceProcAddr(
            m_Instance, "xrGetOpenGLGraphicsRequirementsKHR",
            (PFN_xrVoidFunction *)&get_requirements),
        "xrGetInstanceProcAddr");
    M_Check(
        get_requirements(m_Instance, system_id, &requirements),
        "xrGetOpenGLGraphicsRequirementsKHR");
    printf(
        "== runtime wants OpenGL %u.%u to %u.%u\n",
        XR_VERSION_MAJOR(requirements.minApiVersionSupported),
        XR_VERSION_MINOR(requirements.minApiVersionSupported),
        XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
        XR_VERSION_MINOR(requirements.maxApiVersionSupported));

    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    SDL_GetWindowWMInfo(window, &wm_info);
    Display *const display = wm_info.info.x11.display;
    const GLXContext glx_context = glXGetCurrentContext();

    int fb_config_id = 0;
    glXQueryContext(display, glx_context, GLX_FBCONFIG_ID, &fb_config_id);
    const int fb_attribs[] = { GLX_FBCONFIG_ID, fb_config_id, None };
    int fb_count = 0;
    GLXFBConfig *const fb_configs = glXChooseFBConfig(
        display, DefaultScreen(display), fb_attribs, &fb_count);
    int visual_id = 0;
    if (fb_count > 0) {
        glXGetFBConfigAttrib(display, fb_configs[0], GLX_VISUAL_ID, &visual_id);
    }

    const XrGraphicsBindingOpenGLXlibKHR binding = {
        .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR,
        .xDisplay = display,
        .visualid = (uint32_t)visual_id,
        .glxFBConfig = fb_count > 0 ? fb_configs[0] : nullptr,
        .glxDrawable = glXGetCurrentDrawable(),
        .glxContext = glx_context,
    };
    const XrSessionCreateInfo session_info = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &binding,
        .systemId = system_id,
    };
    XrSession session;
    M_Check(
        xrCreateSession(m_Instance, &session_info, &session),
        "xrCreateSession with an OpenGL context");
    printf("== OpenGL session created\n");

    int64_t formats[64];
    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(session, 64, &format_count, formats);
    int64_t format = format_count > 0 ? formats[0] : GL_RGBA8;
    printf("== swapchain formats:");
    for (uint32_t i = 0; i < format_count; i++) {
        printf(" 0x%04X", (unsigned)formats[i]);
    }
    printf("\n");
    for (uint32_t i = 0; i < format_count; i++) {
        if (formats[i] == GL_SRGB8_ALPHA8) {
            format = formats[i];
            break;
        }
    }
    printf("== using format 0x%04X\n", (unsigned)format);

    const XrReferenceSpaceCreateInfo space_info = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = { .orientation = { .w = 1.0f } },
    };
    XrSpace space;
    M_Check(
        xrCreateReferenceSpace(session, &space_info, &space),
        "xrCreateReferenceSpace");

    XrResult (*create_profile)(
        XrSession, const XrFoveationProfileCreateInfoFB *,
        XrFoveationProfileFB *) = nullptr;
    XrResult (*update_swapchain)(
        XrSwapchain, const XrSwapchainStateBaseHeaderFB *) = nullptr;
    XrResult (*get_eye_state)(XrSession, XrFoveationEyeTrackedStateMETA *) =
        nullptr;
    if (has_foveation) {
        xrGetInstanceProcAddr(
            m_Instance, "xrCreateFoveationProfileFB",
            (PFN_xrVoidFunction *)&create_profile);
        xrGetInstanceProcAddr(
            m_Instance, "xrUpdateSwapchainFB",
            (PFN_xrVoidFunction *)&update_swapchain);
    }
    if (has_eye_foveation) {
        xrGetInstanceProcAddr(
            m_Instance, "xrGetFoveationEyeTrackedStateMETA",
            (PFN_xrVoidFunction *)&get_eye_state);
    }

    M_EYE eyes[2] = {};
    for (int32_t i = 0; i < 2; i++) {
        M_EYE *const eye = &eyes[i];
        // PROBE_SCALE draws the eyes larger than the headset recommends, and
        // PROBE_DEPTH gives them a depth buffer, as a game has.
        const float scale = getenv("PROBE_SCALE") != nullptr
            ? atof(getenv("PROBE_SCALE"))
            : 1.0f;
        eye->width = views_info[i].recommendedImageRectWidth * scale;
        eye->height = views_info[i].recommendedImageRectHeight * scale;
        const XrSwapchainCreateInfo swapchain_info = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
                | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
            .format = format,
            .sampleCount = 1,
            .width = eye->width,
            .height = eye->height,
            .faceCount = 1,
            .arraySize = 1,
            .mipCount = 1,
        };
        bool is_foveated = false;
        if (has_foveation) {
            // The two ways a runtime can thin out the pixels, and then
            // neither named.
            const XrSwapchainCreateFoveationFlagsFB tries[3] = {
                XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB,
                XR_SWAPCHAIN_CREATE_FOVEATION_SCALED_BIN_BIT_FB,
                0,
            };
            for (int32_t j = 0; j < 3 && !is_foveated; j++) {
                const XrSwapchainCreateInfoFoveationFB foveation_info = {
                    .type = XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB,
                    .flags = tries[j],
                };
                XrSwapchainCreateInfo info = swapchain_info;
                info.next = &foveation_info;
                const XrResult result =
                    xrCreateSwapchain(session, &info, &eye->handle);
                printf(
                    "== eye %d foveated swapchain, flags %d: %s\n", i,
                    (int)tries[j], M_ResultName(result));
                is_foveated = XR_SUCCEEDED(result);
            }
        }
        if (!is_foveated) {
            M_Check(
                xrCreateSwapchain(session, &swapchain_info, &eye->handle),
                "xrCreateSwapchain");
        }
        if (is_foveated && create_profile != nullptr
            && update_swapchain != nullptr) {
            const XrFoveationEyeTrackedProfileCreateInfoMETA eye_info = {
                .type = XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META,
            };
            const XrFoveationLevelProfileCreateInfoFB level_info = {
                .type = XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB,
                .next = has_eye_foveation ? &eye_info : nullptr,
                .level = XR_FOVEATION_LEVEL_HIGH_FB,
                .dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB,
            };
            const XrFoveationProfileCreateInfoFB profile_info = {
                .type = XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB,
                .next = &level_info,
            };
            XrFoveationProfileFB profile = XR_NULL_HANDLE;
            XrResult result = create_profile(session, &profile_info, &profile);
            printf(
                "== eye %d foveation profile: %s\n", i, M_ResultName(result));
            if (XR_SUCCEEDED(result)) {
                const XrSwapchainStateFoveationFB state = {
                    .type = XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB,
                    .profile = profile,
                };
                result = update_swapchain(
                    eye->handle, (const XrSwapchainStateBaseHeaderFB *)&state);
                printf(
                    "== eye %d foveation applied: %s\n", i,
                    M_ResultName(result));
            }
        }
        for (int32_t j = 0; j < M_MAX_IMAGES; j++) {
            eye->images[j].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        }
        M_Check(
            xrEnumerateSwapchainImages(
                eye->handle, M_MAX_IMAGES, &eye->image_count,
                (XrSwapchainImageBaseHeader *)eye->images),
            "xrEnumerateSwapchainImages");
    }
    printf(
        "== swapchains created: %dx%d, %u images each\n", eyes[0].width,
        eyes[0].height, eyes[0].image_count);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    GLuint depth = 0;
    if (getenv("PROBE_DEPTH") != nullptr) {
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(
            GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eyes[0].width,
            eyes[0].height);
    }
    const GLuint program = M_MakeHeavyProgram();
    const GLint tint_loc = glGetUniformLocation(program, "tint");
    GLuint timer;
    glGenQueries(1, &timer);
    double gpu_ms_total = 0.0;
    int32_t gpu_samples = 0;

    bool running = false;
    bool done = false;
    int32_t frames = 0;
    bool exit_requested = false;
    uint32_t first_draw = 0;
    uint32_t last_draw = SDL_GetTicks();
    while (!done) {
        if (SDL_GetTicks() - last_draw > M_IDLE_TIMEOUT_MS) {
            printf("FAILED: timed out after %d frames\n", frames);
            return 1;
        }
        SDL_Event sdl_event;
        while (SDL_PollEvent(&sdl_event)) {}

        XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(m_Instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const XrEventDataSessionStateChanged *const changed =
                    (const XrEventDataSessionStateChanged *)&event;
                printf("== session state %d\n", changed->state);
                if (changed->state == XR_SESSION_STATE_READY) {
                    const XrSessionBeginInfo begin_info = {
                        .type = XR_TYPE_SESSION_BEGIN_INFO,
                        .primaryViewConfigurationType =
                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                    };
                    M_Check(
                        xrBeginSession(session, &begin_info), "xrBeginSession");
                    running = true;
                } else if (changed->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                } else if (
                    changed->state == XR_SESSION_STATE_EXITING
                    || changed->state == XR_SESSION_STATE_LOSS_PENDING) {
                    done = true;
                }
            }
            event = (XrEventDataBuffer) { .type = XR_TYPE_EVENT_DATA_BUFFER };
        }
        if (!running) {
            SDL_Delay(10);
            continue;
        }

        XrFrameState frame_state = { .type = XR_TYPE_FRAME_STATE };
        M_Check(xrWaitFrame(session, nullptr, &frame_state), "xrWaitFrame");
        M_Check(xrBeginFrame(session, nullptr), "xrBeginFrame");

        XrView views[2] = { { .type = XR_TYPE_VIEW },
                            { .type = XR_TYPE_VIEW } };
        XrCompositionLayerProjectionView layer_views[2] = {};
        XrCompositionLayerProjection layer = {
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            .space = space,
            .viewCount = 2,
            .views = layer_views,
        };
        const XrCompositionLayerBaseHeader *layers[1] = {
            (const XrCompositionLayerBaseHeader *)&layer,
        };
        uint32_t layer_count = 0;

        if (frame_state.shouldRender) {
            XrViewState view_state = { .type = XR_TYPE_VIEW_STATE };
            const XrViewLocateInfo locate_info = {
                .type = XR_TYPE_VIEW_LOCATE_INFO,
                .viewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                .displayTime = frame_state.predictedDisplayTime,
                .space = space,
            };
            uint32_t located = 0;
            M_Check(
                xrLocateViews(
                    session, &locate_info, &view_state, 2, &located, views),
                "xrLocateViews");
            if (frames == 0 && has_mask) {
                M_PrintMask(session, views);
            }
            if (frames == 0) {
                for (int32_t i = 0; i < 2; i++) {
                    printf(
                        "== eye %d fov (rad): left %.3f right %.3f up %.3f "
                        "down %.3f, orientation %.3f %.3f %.3f %.3f\n",
                        i, views[i].fov.angleLeft, views[i].fov.angleRight,
                        views[i].fov.angleUp, views[i].fov.angleDown,
                        views[i].pose.orientation.x,
                        views[i].pose.orientation.y,
                        views[i].pose.orientation.z,
                        views[i].pose.orientation.w);
                }
            }

            if (frames == 30 && get_eye_state != nullptr) {
                XrFoveationEyeTrackedStateMETA state = {
                    .type = XR_TYPE_FOVEATION_EYE_TRACKED_STATE_META,
                };
                const XrResult result = get_eye_state(session, &state);
                printf(
                    "== eye-tracked foveation state: %s, flags %d, centers "
                    "%.3f %.3f / %.3f %.3f\n",
                    M_ResultName(result), (int)state.flags,
                    state.foveationCenter[0].x, state.foveationCenter[0].y,
                    state.foveationCenter[1].x, state.foveationCenter[1].y);
            }

            glBeginQuery(GL_TIME_ELAPSED, timer);
            for (int32_t i = 0; i < 2; i++) {
                M_EYE *const eye = &eyes[i];
                uint32_t index = 0;
                M_Check(
                    xrAcquireSwapchainImage(eye->handle, nullptr, &index),
                    "xrAcquireSwapchainImage");
                const XrSwapchainImageWaitInfo wait_info = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
                    .timeout = XR_INFINITE_DURATION,
                };
                M_Check(
                    xrWaitSwapchainImage(eye->handle, &wait_info),
                    "xrWaitSwapchainImage");

                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(
                    GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                    eye->images[index].image, 0);
                if (depth != 0) {
                    glFramebufferRenderbuffer(
                        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                        depth);
                    glClear(GL_DEPTH_BUFFER_BIT);
                }
                if (frames == 0) {
                    printf(
                        "== eye %d framebuffer status 0x%04X\n", i,
                        glCheckFramebufferStatus(GL_FRAMEBUFFER));
                }
                glViewport(0, 0, eye->width, eye->height);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(program);
                if (i == 0) {
                    glUniform3f(tint_loc, 0.0f, 0.5f, 0.1f);
                } else {
                    glUniform3f(tint_loc, 0.5f, 0.0f, 0.5f);
                }
                glBindVertexArray(vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                if (frames == 60 || frames == 120) {
                    M_PrintSharpness(i, eye->width, eye->height);
                }
                glBindFramebuffer(GL_FRAMEBUFFER, 0);

                M_Check(
                    xrReleaseSwapchainImage(eye->handle, nullptr),
                    "xrReleaseSwapchainImage");

                layer_views[i] = (XrCompositionLayerProjectionView) {
                    .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
                    .pose = views[i].pose,
                    .fov = views[i].fov,
                    .subImage = {
                        .swapchain = eye->handle,
                        .imageRect = {
                            .extent = { eye->width, eye->height },
                        },
                    },
                };
            }
            glEndQuery(GL_TIME_ELAPSED);
            GLuint64 elapsed = 0;
            glGetQueryObjectui64v(timer, GL_QUERY_RESULT, &elapsed);
            if (frames >= 30) {
                gpu_ms_total += elapsed / 1e6;
                gpu_samples++;
            }
            layer_count = 1;
            frames++;
            last_draw = SDL_GetTicks();
            if (first_draw == 0) {
                first_draw = last_draw;
            }
        }

        const XrFrameEndInfo end_info = {
            .type = XR_TYPE_FRAME_END_INFO,
            .displayTime = frame_state.predictedDisplayTime,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
            .layerCount = layer_count,
            .layers = layers,
        };
        M_Check(xrEndFrame(session, &end_info), "xrEndFrame");

        if (!exit_requested && first_draw != 0
            && last_draw - first_draw >= draw_ms) {
            printf(
                "== drew %d frames in %u ms, GL error 0x%04X\n", frames,
                last_draw - first_draw, glGetError());
            printf(
                "== GPU time for both eyes: %.2f ms on average\n",
                gpu_samples > 0 ? gpu_ms_total / gpu_samples : 0.0);
            xrRequestExitSession(session);
            exit_requested = true;
        }
    }

    printf("== RESULT: OpenGL 3.3 core rendering through OpenXR works\n");
    xrDestroySession(session);
    xrDestroyInstance(m_Instance);
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
