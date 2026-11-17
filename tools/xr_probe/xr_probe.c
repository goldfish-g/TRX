// Reports what an OpenXR runtime offers to an OpenGL application, and proves
// it by opening a session on a desktop OpenGL 3.3 core context and clearing
// each eye to a colour of its own for a few seconds: the left eye green, the
// right eye magenta.
//
// It answers whether TRX can keep its OpenGL renderer on a given headset.
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
    // The first argument is how many seconds to keep the colours up for.
    const uint32_t draw_ms =
        1000 * (argc > 1 ? atoi(argv[1]) : M_DEFAULT_SECONDS);

    const bool has_gl = M_ListExtensions(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    printf("== XR_KHR_opengl_enable offered: %s\n", has_gl ? "yes" : "NO");
    if (!has_gl) {
        return 1;
    }

    const char *const extensions[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo instance_info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {
            .applicationName = "trx_xr_probe",
            .applicationVersion = 1,
            .apiVersion = XR_MAKE_VERSION(1, 0, 0),
        },
        .enabledExtensionCount = 1,
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

    XrSystemProperties system_props = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties(m_Instance, system_id, &system_props);
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

    M_EYE eyes[2] = {};
    for (int32_t i = 0; i < 2; i++) {
        M_EYE *const eye = &eyes[i];
        eye->width = views_info[i].recommendedImageRectWidth;
        eye->height = views_info[i].recommendedImageRectHeight;
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
        M_Check(
            xrCreateSwapchain(session, &swapchain_info, &eye->handle),
            "xrCreateSwapchain");
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
                if (frames == 0) {
                    printf(
                        "== eye %d framebuffer status 0x%04X\n", i,
                        glCheckFramebufferStatus(GL_FRAMEBUFFER));
                }
                glViewport(0, 0, eye->width, eye->height);
                if (i == 0) {
                    glClearColor(0.0f, 0.5f, 0.1f, 1.0f);
                } else {
                    glClearColor(0.5f, 0.0f, 0.5f, 1.0f);
                }
                glClear(GL_COLOR_BUFFER_BIT);
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
