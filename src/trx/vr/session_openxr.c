// realpath and posix_spawn are POSIX, which the C standard alone leaves out.
#define _GNU_SOURCE

#include <trx/core/log.h>
#include <trx/game/clock/const.h>
#include <trx/vr/gamepad_openxr.h>
#include <trx/vr/session.h>

#include <GL/glx.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <X11/Xlib.h>
#include <math.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define M_MAX_IMAGES 8
#define M_SCREEN_WIDTH 1920
#define M_SCREEN_HEIGHT 1080
// The screen in the headset, in meters: how wide it is, and how far in front
// of the player it hangs.
#define M_SCREEN_SIZE 3.2f
#define M_SCREEN_DISTANCE 2.5f

#define M_MUST(call_, what_)                                                   \
    do {                                                                       \
        const XrResult xr_result_ = (call_);                                   \
        FAIL_IF(                                                               \
            XR_FAILED(xr_result_), "%s: %s", (what_),                          \
            M_DescribeResult(xr_result_));                                     \
    } while (0)

typedef struct {
    XrInstance instance;
    XrSystemId system_id;
    XrSession session;
    // Where the headset started, the player's head, and where the screen
    // hangs. The last is the first, moved to where the player last looked.
    XrSpace local_space;
    XrSpace view_space;
    XrSpace space;
    XrSwapchain swapchain;
    XrSwapchainImageOpenGLKHR images[M_MAX_IMAGES];
    uint32_t image_count;
    GLuint fbo;

    // Whether the headset takes frames at the moment.
    bool is_running;
    // Whether the session is over, and is closed before the next frame.
    bool is_over;
    bool needs_recenter;
    // Whether the runtime is SteamVR, and whether its menu was already asked
    // to step aside.
    bool is_steamvr;
    bool has_refresh_rates;
    bool has_presence;
    bool is_player_away;
    bool is_menu_dismissed;
    // Whether the frame that is open has an image of the swapchain to give
    // back.
    bool has_image;
    XrTime display_time;
} M_PRIV;

static M_PRIV m_Priv = {};

static const char *M_DescribeResult(const XrResult result)
{
    static char name[XR_MAX_RESULT_STRING_SIZE];
    if (m_Priv.instance == XR_NULL_HANDLE
        || XR_FAILED(xrResultToString(m_Priv.instance, result, name))) {
        snprintf(name, sizeof(name), "error %d", result);
    }
    return name;
}

static bool M_HasExtension(const char *const name)
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
        found |= strcmp(props[i].extensionName, name) == 0;
    }
    free(props);
    return found;
}

// SteamVR keeps its menu open over a game that was started from it, and the
// game has neither the picture nor the controllers until the player closes
// it. OpenXR has no way to ask for the menu to go, so SteamVR's own command
// line tool is sent to do it, which sits next to the runtime.
static void M_DismissSteamVRMenu(void)
{
    const char *const home = getenv("HOME");
    char manifest[512];
    char runtime_dir[512];
    if (home == nullptr) {
        return;
    }
    snprintf(
        manifest, sizeof(manifest), "%s/.config/openxr/1/active_runtime.json",
        home);
    if (realpath(manifest, runtime_dir) == nullptr) {
        return;
    }
    char *const slash = strrchr(runtime_dir, '/');
    if (slash == nullptr) {
        return;
    }
    *slash = '\0';

#if defined(__aarch64__)
    const char *const arch = "linuxarm64";
#else
    const char *const arch = "linux64";
#endif
    char tool[600];
    snprintf(tool, sizeof(tool), "%s/bin/%s/vrcmd", runtime_dir, arch);
    char *const argv[] = { tool, "--hidedashboard", nullptr };
    extern char **environ;
    pid_t pid;
    if (posix_spawn(&pid, tool, nullptr, nullptr, argv, environ) != 0) {
        LOG_WARNING("the SteamVR menu could not be asked to close");
    }
}

static RESULT M_CreateInstance(void)
{
    M_PRIV *const p = &m_Priv;
    // The controllers of the Steam Frame are a kind of their own, which only
    // a runtime that knows them takes bindings for.
    const char *extensions[4] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    uint32_t extension_count = 1;
    // Lets the headset say when the player takes it off.
    p->has_presence = M_HasExtension(XR_EXT_USER_PRESENCE_EXTENSION_NAME);
    if (p->has_presence) {
        extensions[extension_count++] = XR_EXT_USER_PRESENCE_EXTENSION_NAME;
    }
    p->has_refresh_rates =
        M_HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (p->has_refresh_rates) {
        extensions[extension_count++] =
            XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    }
    if (M_HasExtension("XR_VALVE_frame_controller_interaction")) {
        extensions[extension_count++] = "XR_VALVE_frame_controller_interaction";
    }
    const XrInstanceCreateInfo info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {
            .applicationName = "TRX",
            .applicationVersion = 1,
            .engineName = "TRX",
            .apiVersion = XR_MAKE_VERSION(1, 0, 0),
        },
        .enabledExtensionCount = extension_count,
        .enabledExtensionNames = extensions,
    };
    M_MUST(
        xrCreateInstance(&info, &p->instance),
        "no OpenXR runtime that draws with OpenGL was found");

    XrInstanceProperties props = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(p->instance, &props))) {
        p->is_steamvr = strncmp(props.runtimeName, "SteamVR", 7) == 0;
        LOG_INFO(
            "OpenXR runtime: %s %d.%d.%d", props.runtimeName,
            XR_VERSION_MAJOR(props.runtimeVersion),
            XR_VERSION_MINOR(props.runtimeVersion),
            XR_VERSION_PATCH(props.runtimeVersion));
    }

    const XrSystemGetInfo system_info = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    M_MUST(
        xrGetSystem(p->instance, &system_info, &p->system_id),
        "no headset was found");
    return OK;
}

static RESULT M_CreateSession(SDL_Window *const window)
{
    M_PRIV *const p = &m_Priv;

    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    FAIL_IF(
        !SDL_GetWindowWMInfo(window, &wm_info)
            || wm_info.subsystem != SDL_SYSWM_X11,
        "the headset needs the game window to be an X11 one");
    Display *const display = wm_info.info.x11.display;
    const GLXContext context = glXGetCurrentContext();
    FAIL_IF(context == nullptr, "there is no OpenGL context to share");

    // The runtime has to be asked what it needs before a session is made,
    // and the loader exports only the core functions, so the instance hands
    // out the one that asks.
    XrResult (*get_requirements)(
        XrInstance, XrSystemId, XrGraphicsRequirementsOpenGLKHR *) = nullptr;
    M_MUST(
        xrGetInstanceProcAddr(
            p->instance, "xrGetOpenGLGraphicsRequirementsKHR",
            (PFN_xrVoidFunction *)&get_requirements),
        "the OpenXR runtime does not say what OpenGL it needs");
    XrGraphicsRequirementsOpenGLKHR requirements = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR,
    };
    M_MUST(
        get_requirements(p->instance, p->system_id, &requirements),
        "the OpenXR runtime does not say what OpenGL it needs");
    LOG_INFO(
        "OpenXR runtime takes OpenGL %d.%d to %d.%d",
        XR_VERSION_MAJOR(requirements.minApiVersionSupported),
        XR_VERSION_MINOR(requirements.minApiVersionSupported),
        XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
        XR_VERSION_MINOR(requirements.maxApiVersionSupported));

    int fb_config_id = 0;
    glXQueryContext(display, context, GLX_FBCONFIG_ID, &fb_config_id);
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
        .glxContext = context,
    };
    const XrSessionCreateInfo info = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &binding,
        .systemId = p->system_id,
    };
    const XrResult result = xrCreateSession(p->instance, &info, &p->session);
    if (fb_configs != nullptr) {
        XFree(fb_configs);
    }
    M_MUST(result, "the headset does not take the game's OpenGL context");

    XrReferenceSpaceCreateInfo space_info = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = { .orientation = { .w = 1.0f } },
    };
    M_MUST(
        xrCreateReferenceSpace(p->session, &space_info, &p->local_space),
        "the headset has no space to place the screen in");
    M_MUST(
        xrCreateReferenceSpace(p->session, &space_info, &p->space),
        "the headset has no space to place the screen in");
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    M_MUST(
        xrCreateReferenceSpace(p->session, &space_info, &p->view_space),
        "the headset does not say where the player looks");
    p->needs_recenter = true;
    return OK;
}

// Hangs the screen in front of where the player looks at the given time. It
// stays upright: only the way the player faces is taken, not how the head
// tilts. Returns false where the headset does not know where the head is.
static bool M_Recenter(const XrTime time)
{
    M_PRIV *const p = &m_Priv;
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
        | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (XR_FAILED(xrLocateSpace(p->view_space, p->local_space, time, &location))
        || (location.locationFlags & needed) != needed) {
        return false;
    }

    const XrQuaternionf q = location.pose.orientation;
    const float yaw = atan2f(
        2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const XrReferenceSpaceCreateInfo info = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = {
            .orientation = { .y = sinf(yaw / 2.0f), .w = cosf(yaw / 2.0f) },
            .position = location.pose.position,
        },
    };
    XrSpace space;
    if (XR_FAILED(xrCreateReferenceSpace(p->session, &info, &space))) {
        return false;
    }
    xrDestroySpace(p->space);
    p->space = space;
    return true;
}

// Asks the headset to show as many frames a second as a whole number of them
// fits into a tick of the game. What only moves once a tick, such as the
// pose of an animation, is then held for the same number of frames every
// time; at any other rate the number alternates, which shows as a stutter.
static void M_PickRefreshRate(void)
{
    M_PRIV *const p = &m_Priv;
    if (!p->has_refresh_rates) {
        return;
    }
    XrResult (*enumerate)(XrSession, uint32_t, uint32_t *, float *) = nullptr;
    XrResult (*request)(XrSession, float) = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(
            p->instance, "xrEnumerateDisplayRefreshRatesFB",
            (PFN_xrVoidFunction *)&enumerate))
        || XR_FAILED(xrGetInstanceProcAddr(
            p->instance, "xrRequestDisplayRefreshRateFB",
            (PFN_xrVoidFunction *)&request))) {
        return;
    }

    float rates[16];
    uint32_t count = 0;
    if (XR_FAILED(enumerate(p->session, 16, &count, rates))) {
        return;
    }
    // The lowest such rate that is still smooth to look at.
    const float wanted = 3.0f * LOGIC_FPS;
    float best = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        const float ticks = rates[i] / LOGIC_FPS;
        const bool is_whole = fabsf(ticks - roundf(ticks)) < 0.02f;
        if (is_whole && rates[i] >= wanted - 0.5f
            && (best == 0.0f || rates[i] < best)) {
            best = rates[i];
        }
    }
    if (best == 0.0f) {
        LOG_INFO("the headset has no refresh rate that fits the game");
        return;
    }
    const XrResult result = request(p->session, best);
    LOG_INFO(
        "asked the headset for %.0f Hz: %s", best,
        XR_SUCCEEDED(result) ? "taken" : M_DescribeResult(result));
}

static RESULT M_CreateScreen(void)
{
    M_PRIV *const p = &m_Priv;

    int64_t formats[64];
    uint32_t format_count = 0;
    M_MUST(
        xrEnumerateSwapchainFormats(p->session, 64, &format_count, formats),
        "the headset names no picture formats");
    FAIL_IF(format_count == 0, "the headset names no picture formats");
    // The game draws colors as a display takes them, which is what the sRGB
    // format says of its texels. The runtime lists the formats it prefers
    // first, so that is what is left where it has no sRGB one.
    int64_t format = formats[0];
    for (uint32_t i = 0; i < format_count; i++) {
        if (formats[i] == GL_SRGB8_ALPHA8) {
            format = formats[i];
            break;
        }
    }

    const XrSwapchainCreateInfo info = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
            | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
        .format = format,
        .sampleCount = 1,
        .width = M_SCREEN_WIDTH,
        .height = M_SCREEN_HEIGHT,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    M_MUST(
        xrCreateSwapchain(p->session, &info, &p->swapchain),
        "the headset gives no pictures to draw into");
    for (int32_t i = 0; i < M_MAX_IMAGES; i++) {
        p->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
    }
    M_MUST(
        xrEnumerateSwapchainImages(
            p->swapchain, M_MAX_IMAGES, &p->image_count,
            (XrSwapchainImageBaseHeader *)p->images),
        "the headset gives no pictures to draw into");

    glGenFramebuffers(1, &p->fbo);
    M_PickRefreshRate();
    return OK;
}

static void M_HandleStateChange(const XrSessionState state)
{
    M_PRIV *const p = &m_Priv;
    LOG_INFO("OpenXR session state: %d", state);
    if (state == XR_SESSION_STATE_VISIBLE && p->is_steamvr
        && !p->is_menu_dismissed) {
        p->is_menu_dismissed = true;
        M_DismissSteamVRMenu();
    }
    if (state == XR_SESSION_STATE_FOCUSED) {
        // The player comes back to the game, from the menu of the headset
        // or from starting it, and may have turned since the screen was
        // placed.
        p->needs_recenter = true;
    }
    if (state == XR_SESSION_STATE_READY) {
        const XrSessionBeginInfo info = {
            .type = XR_TYPE_SESSION_BEGIN_INFO,
            .primaryViewConfigurationType =
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        };
        const XrResult result = xrBeginSession(p->session, &info);
        if (XR_FAILED(result)) {
            LOG_ERROR(
                "the headset does not take frames: %s",
                M_DescribeResult(result));
        } else {
            p->is_running = true;
        }
    } else if (state == XR_SESSION_STATE_STOPPING) {
        xrEndSession(p->session);
        p->is_running = false;
    } else if (state == XR_SESSION_STATE_EXITING) {
        // The player closed the game from the headset.
        p->is_running = false;
        p->is_over = true;
        SDL_Event quit = { .type = SDL_QUIT };
        SDL_PushEvent(&quit);
    } else if (state == XR_SESSION_STATE_LOSS_PENDING) {
        LOG_WARNING("the headset is gone, and the game stays in its window");
        p->is_running = false;
        p->is_over = true;
    }
}

static void M_PollEvents(void)
{
    M_PRIV *const p = &m_Priv;
    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(p->instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *const changed =
                (const XrEventDataSessionStateChanged *)&event;
            M_HandleStateChange(changed->state);
        } else if (event.type == XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT) {
            const XrEventDataUserPresenceChangedEXT *const changed =
                (const XrEventDataUserPresenceChangedEXT *)&event;
            p->is_player_away = !changed->isUserPresent;
            LOG_INFO(
                "the player %s the headset",
                p->is_player_away ? "took off" : "put on");
        } else if (
            event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // The player recentered through the headset, which moves the
            // space the screen hangs in. The screen follows to where the
            // player looks now.
            p->needs_recenter = true;
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            LOG_WARNING(
                "the OpenXR runtime is gone, and the game stays in its "
                "window");
            p->is_running = false;
            p->is_over = true;
        }
        event = (XrEventDataBuffer) { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
}

void VR_Session_PrepareSDL(void)
{
    // The runtime shares the game's OpenGL context through X11.
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
    // The game window may be hidden, and never has the keyboard then. SDL
    // drops what gamepads say while no window has it, the controllers of the
    // headset among them.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
}

RESULT VR_Session_Start(void *const window)
{
    if (VR_Session_IsActive()) {
        return OK;
    }
    RESULT result = M_CreateInstance();
    if (IS_OK(result)) {
        result = M_CreateSession(window);
    }
    if (IS_OK(result)) {
        result = M_CreateScreen();
    }
    if (!IS_OK(result)) {
        VR_Session_Stop();
        return result;
    }
    // Without its controllers the headset still shows the game, which other
    // devices can play.
    SHOULD(
        VR_Gamepad_Init(m_Priv.instance, m_Priv.session),
        "the controllers of the headset will not work");
    // The headset says when a frame is due, so the window waits for nothing.
    SDL_GL_SetSwapInterval(0);
    return OK;
}

void VR_Session_Stop(void)
{
    M_PRIV *const p = &m_Priv;
    VR_Gamepad_Shutdown();
    if (p->fbo != 0) {
        glDeleteFramebuffers(1, &p->fbo);
    }
    if (p->swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(p->swapchain);
    }
    if (p->space != XR_NULL_HANDLE) {
        xrDestroySpace(p->space);
    }
    if (p->view_space != XR_NULL_HANDLE) {
        xrDestroySpace(p->view_space);
    }
    if (p->local_space != XR_NULL_HANDLE) {
        xrDestroySpace(p->local_space);
    }
    if (p->session != XR_NULL_HANDLE) {
        xrDestroySession(p->session);
    }
    if (p->instance != XR_NULL_HANDLE) {
        xrDestroyInstance(p->instance);
    }
    *p = (M_PRIV) {};
}

void VR_Session_GetScreenSize(
    int32_t *const out_width, int32_t *const out_height)
{
    *out_width = M_SCREEN_WIDTH;
    *out_height = M_SCREEN_HEIGHT;
}

bool VR_Session_IsActive(void)
{
    return m_Priv.session != XR_NULL_HANDLE;
}

bool VR_Session_PacesFrames(void)
{
    return m_Priv.is_running;
}

bool VR_Session_IsPlayerAway(void)
{
    return VR_Session_IsActive() && m_Priv.is_player_away;
}

void VR_Session_Recenter(void)
{
    m_Priv.needs_recenter = true;
}

bool VR_Session_BeginFrame(VR_SCREEN *const out_screen)
{
    M_PRIV *const p = &m_Priv;
    *out_screen = (VR_SCREEN) {};
    if (!VR_Session_IsActive()) {
        return false;
    }
    M_PollEvents();
    if (p->is_over) {
        VR_Session_Stop();
        return false;
    }
    if (!p->is_running) {
        return false;
    }

    XrFrameState frame_state = { .type = XR_TYPE_FRAME_STATE };
    XrResult result = xrWaitFrame(p->session, nullptr, &frame_state);
    if (XR_SUCCEEDED(result)) {
        result = xrBeginFrame(p->session, nullptr);
    }
    if (XR_FAILED(result)) {
        // A session that is lost says so here before any event does.
        if (result == XR_ERROR_SESSION_LOST
            || result == XR_ERROR_INSTANCE_LOST) {
            LOG_WARNING(
                "the headset is gone, and the game stays in its window");
            VR_Session_Stop();
        }
        return false;
    }
    p->display_time = frame_state.predictedDisplayTime;
    p->has_image = false;
    VR_Gamepad_Update(p->session);
    if (!frame_state.shouldRender) {
        return true;
    }
    if (p->needs_recenter && M_Recenter(p->display_time)) {
        p->needs_recenter = false;
    }

    uint32_t index = 0;
    const XrSwapchainImageWaitInfo wait_info = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION,
    };
    if (XR_FAILED(xrAcquireSwapchainImage(p->swapchain, nullptr, &index))) {
        return true;
    }
    p->has_image = true;
    if (XR_FAILED(xrWaitSwapchainImage(p->swapchain, &wait_info))) {
        return true;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, p->fbo);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        p->images[index].image, 0);
    out_screen->fbo = p->fbo;
    out_screen->width = M_SCREEN_WIDTH;
    out_screen->height = M_SCREEN_HEIGHT;
    return true;
}

void VR_Session_EndFrame(const float aspect_ratio)
{
    M_PRIV *const p = &m_Priv;
    if (p->has_image) {
        xrReleaseSwapchainImage(p->swapchain, nullptr);
    }

    const XrCompositionLayerQuad quad = {
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .space = p->space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = p->swapchain,
            .imageRect = {
                .extent = { M_SCREEN_WIDTH, M_SCREEN_HEIGHT },
            },
        },
        .pose = {
            .orientation = { .w = 1.0f },
            .position = { .z = -M_SCREEN_DISTANCE },
        },
        .size = { M_SCREEN_SIZE, M_SCREEN_SIZE / aspect_ratio },
    };
    const XrCompositionLayerBaseHeader *const layers[] = {
        (const XrCompositionLayerBaseHeader *)&quad,
    };
    const XrFrameEndInfo info = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = p->display_time,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = p->has_image ? 1 : 0,
        .layers = layers,
    };
    xrEndFrame(p->session, &info);
    p->has_image = false;
}
