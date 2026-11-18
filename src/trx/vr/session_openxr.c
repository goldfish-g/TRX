// realpath and posix_spawn are POSIX, which the C standard alone leaves out.
#define _GNU_SOURCE

#include <trx/core/log.h>
#include <trx/core/utils.h>
#include <trx/game/clock/const.h>
#include <trx/gl/api.h>
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
// The same for the panel that carries the interface while the player is
// inside the game. It looks as large, and hangs nearer, where fewer walls
// come between it and the player.
// How much larger than the headset asks for each eye's picture is drawn, on
// each side. The game's textures are coarse and its edges are not smoothed,
// so the extra is what the headset has to make them steady with. The frame
// is cheap enough to leave room for it.
#define M_EYE_SCALE 1.3f
// Scenes differ a great deal in what they cost to draw, so the eyes are
// drawn into as much of their pictures as there is time for, between the
// size the headset asks for and all of it. A frame whose drawing takes the
// graphics card more than the upper share of the time a frame has makes the
// next ones smaller at once; a run of frames under the lower share lets
// them grow again, slowly, so that the size does not swing.
#define M_EYE_SHARE_MIN (1.0f / M_EYE_SCALE)
#define M_EYE_BUSY 0.64
#define M_EYE_IDLE 0.52
#define M_EYE_SHRINK 0.93f
#define M_EYE_GROW 1.03f
#define M_EYE_CALM_FRAMES 20
#define M_PANEL_SIZE 1.8f
#define M_PANEL_DISTANCE 1.4f

#define M_MUST(call_, what_)                                                   \
    do {                                                                       \
        const XrResult xr_result_ = (call_);                                   \
        FAIL_IF(                                                               \
            XR_FAILED(xr_result_), "%s: %s", (what_),                          \
            M_DescribeResult(xr_result_));                                     \
    } while (0)

// A chain of pictures that the headset shows, and the framebuffer the one in
// turn is drawn through.
typedef struct {
    XrSwapchain swapchain;
    XrSwapchainImageOpenGLKHR images[M_MAX_IMAGES];
    uint32_t image_count;
    GLuint fbo;
    GLuint depth;
    int32_t width;
    int32_t height;
    // How much of the picture the frame that is open draws into.
    int32_t used_width;
    int32_t used_height;
    // Whether the frame that is open holds a picture of the chain, and
    // whether that picture was drawn and given back.
    bool is_acquired;
    bool is_drawn;
} M_TARGET;

typedef struct {
    XrInstance instance;
    XrSystemId system_id;
    XrSession session;
    // Where the headset started, the player's head, and where the screen
    // hangs. The last is the first, moved to where the player last looked.
    XrSpace local_space;
    XrSpace view_space;
    XrSpace space;
    // What the game draws into: the screen, and the two eyes.
    M_TARGET screens[2];
    M_TARGET eyes[2];
    VR_MODE mode;
    int32_t theater_eye;
    int64_t format;

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
    // Whether a frame is open, and whether the headset will show it.
    bool is_frame_open;
    bool should_render;
    XrTime display_time;
    // Where the eyes are in the frame that is open, as the headset gives
    // them and as the game takes them.
    XrView xr_views[2];
    VR_VIEW view;
    bool has_view;
    // The share of each side of the eyes' pictures that is drawn into, when
    // the frame that is open began, how long a frame has, and for how many
    // frames in a row there has been time to spare.
    float eye_share;
    Uint64 frame_start;
    double frame_period;
    int32_t calm_frames;
    // Asks the graphics card how long it spent on a frame's drawing, which
    // it answers a frame or two later.
    GLuint timer;
    bool is_timing;
    bool has_timing;
    double draw_time;
    double draw_total;
    int32_t draw_frames;
    // How long the frames took to prepare and then to finish drawing, added
    // up, to say in the log when the session ends.
    double work_total;
    double wait_total;
    int32_t timed_frames;
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

static RESULT M_CreateTarget(
    M_TARGET *const target, const int32_t width, const int32_t height,
    const bool has_depth)
{
    M_PRIV *const p = &m_Priv;
    target->width = width;
    target->height = height;
    const XrSwapchainCreateInfo info = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
            | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
        .format = p->format,
        .sampleCount = 1,
        .width = width,
        .height = height,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    M_MUST(
        xrCreateSwapchain(p->session, &info, &target->swapchain),
        "the headset gives no pictures to draw into");
    for (int32_t i = 0; i < M_MAX_IMAGES; i++) {
        target->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
    }
    M_MUST(
        xrEnumerateSwapchainImages(
            target->swapchain, M_MAX_IMAGES, &target->image_count,
            (XrSwapchainImageBaseHeader *)target->images),
        "the headset gives no pictures to draw into");

    glGenFramebuffers(1, &target->fbo);
    if (has_depth) {
        // The headset is only handed the colors, so the depth the scene is
        // drawn with stays the game's own.
        glGenRenderbuffers(1, &target->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, target->depth);
        glRenderbufferStorage(
            GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
        glFramebufferRenderbuffer(
            GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
            target->depth);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    return OK;
}

static void M_DestroyTarget(M_TARGET *const target)
{
    if (target->fbo != 0) {
        glDeleteFramebuffers(1, &target->fbo);
    }
    if (target->depth != 0) {
        glDeleteRenderbuffers(1, &target->depth);
    }
    if (target->swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(target->swapchain);
    }
    *target = (M_TARGET) {};
}

// Takes the picture of the chain that is in turn and puts it behind the
// framebuffer of the target.
static bool M_AcquireTarget(M_TARGET *const target, VR_SCREEN *const out_screen)
{
    M_PRIV *const p = &m_Priv;
    *out_screen = (VR_SCREEN) {};
    if (!p->is_frame_open || !p->should_render || target->is_acquired
        || target->is_drawn) {
        return false;
    }
    uint32_t index = 0;
    const XrSwapchainImageWaitInfo wait_info = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION,
    };
    if (XR_FAILED(
            xrAcquireSwapchainImage(target->swapchain, nullptr, &index))) {
        return false;
    }
    target->is_acquired = true;
    if (XR_FAILED(xrWaitSwapchainImage(target->swapchain, &wait_info))) {
        xrReleaseSwapchainImage(target->swapchain, nullptr);
        target->is_acquired = false;
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        target->images[index].image, 0);
    out_screen->fbo = target->fbo;
    out_screen->width = target->used_width;
    out_screen->height = target->used_height;
    return true;
}

// Marks the picture as drawn. It is given back to the headset with the
// others at the end of the frame, once the drawing of all of them is seen
// through.
static void M_ReleaseTarget(M_TARGET *const target)
{
    if (target->is_acquired) {
        target->is_drawn = true;
    }
}

static RESULT M_CreateTargets(void)
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
    p->format = formats[0];
    for (uint32_t i = 0; i < format_count; i++) {
        if (formats[i] == GL_SRGB8_ALPHA8) {
            p->format = formats[i];
            break;
        }
    }

    XrViewConfigurationView config[2] = {
        { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW },
        { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW },
    };
    uint32_t view_count = 0;
    M_MUST(
        xrEnumerateViewConfigurationViews(
            p->instance, p->system_id,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &view_count, config),
        "the headset does not say how large its eyes are");

    for (int32_t i = 0; i < 2; i++) {
        MUST(M_CreateTarget(
            &p->screens[i], M_SCREEN_WIDTH, M_SCREEN_HEIGHT, false));
        MUST(M_CreateTarget(
            &p->eyes[i],
            (int32_t)(config[i].recommendedImageRectWidth * M_EYE_SCALE),
            (int32_t)(config[i].recommendedImageRectHeight * M_EYE_SCALE),
            true));
    }
    LOG_INFO(
        "the headset is drawn at %dx%d an eye", p->eyes[0].width,
        p->eyes[0].height);
    M_PickRefreshRate();
    return OK;
}

// Works out where the head and the eyes are when the frame that is open is
// shown, in the space the screen hangs in.
static void M_LocateViews(void)
{
    M_PRIV *const p = &m_Priv;
    p->has_view = false;
    XrViewState state = { .type = XR_TYPE_VIEW_STATE };
    const XrViewLocateInfo info = {
        .type = XR_TYPE_VIEW_LOCATE_INFO,
        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        .displayTime = p->display_time,
        .space = p->space,
    };
    p->xr_views[0] = (XrView) { .type = XR_TYPE_VIEW };
    p->xr_views[1] = (XrView) { .type = XR_TYPE_VIEW };
    uint32_t count = 0;
    const XrViewStateFlags needed =
        XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_FAILED(
            xrLocateViews(p->session, &info, &state, 2, &count, p->xr_views))
        || count != 2 || (state.viewStateFlags & needed) != needed) {
        return;
    }

    // The head is between the eyes and looks the way they do.
    const XrVector3f a = p->xr_views[0].pose.position;
    const XrVector3f b = p->xr_views[1].pose.position;
    const XrQuaternionf q = p->xr_views[0].pose.orientation;
    VR_VIEW *const view = &p->view;
    view->pos[0] = (a.x + b.x) / 2.0f;
    view->pos[1] = (a.y + b.y) / 2.0f;
    view->pos[2] = (a.z + b.z) / 2.0f;
    view->rot[0][0] = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    view->rot[0][1] = 2.0f * (q.x * q.y - q.z * q.w);
    view->rot[0][2] = 2.0f * (q.x * q.z + q.y * q.w);
    view->rot[1][0] = 2.0f * (q.x * q.y + q.z * q.w);
    view->rot[1][1] = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    view->rot[1][2] = 2.0f * (q.y * q.z - q.x * q.w);
    view->rot[2][0] = 2.0f * (q.x * q.z - q.y * q.w);
    view->rot[2][1] = 2.0f * (q.y * q.z + q.x * q.w);
    view->rot[2][2] = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);

    for (int32_t i = 0; i < 2; i++) {
        const XrVector3f eye = p->xr_views[i].pose.position;
        const float d[3] = {
            eye.x - view->pos[0],
            eye.y - view->pos[1],
            eye.z - view->pos[2],
        };
        // Into the axes of the head, which the rows of its rotation turn
        // back to.
        for (int32_t j = 0; j < 3; j++) {
            view->eyes[i].offset[j] = view->rot[0][j] * d[0]
                + view->rot[1][j] * d[1] + view->rot[2][j] * d[2];
        }
        const XrFovf fov = p->xr_views[i].fov;
        view->eyes[i].tan_left = tanf(fov.angleLeft);
        view->eyes[i].tan_right = tanf(fov.angleRight);
        view->eyes[i].tan_up = tanf(fov.angleUp);
        view->eyes[i].tan_down = tanf(fov.angleDown);
    }
    p->has_view = true;
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

// Takes the graphics card's answer on how long a frame's drawing took, once
// it has one, and sets how much of the eyes' pictures the next frames draw
// into. Waiting for the drawing says nothing about it: on the Steam Frame
// the wait ends at the same point of every frame, busy or not.
static void M_CollectDrawTime(void)
{
    M_PRIV *const p = &m_Priv;
    if (!p->has_timing) {
        return;
    }
    GLint is_ready = 0;
    glGetQueryObjectiv(p->timer, GL_QUERY_RESULT_AVAILABLE, &is_ready);
    if (!is_ready) {
        return;
    }
    GLuint64 nanoseconds = 0;
    glGetQueryObjectui64v(p->timer, GL_QUERY_RESULT, &nanoseconds);
    p->has_timing = false;
    // Single frames differ a good deal, by whether the game moved a tick on
    // in them for one, and the size should follow what the scene costs, not
    // each of them. So the times are evened out over a few frames.
    const double draw_time = nanoseconds / 1e9;
    p->draw_time =
        p->draw_time <= 0.0 ? draw_time : p->draw_time * 0.9 + draw_time * 0.1;
    p->draw_total += draw_time;
    p->draw_frames++;
    // Only frames that drew the eyes say something about their size.
    if (!p->eyes[0].is_drawn || p->frame_period <= 0.0) {
        return;
    }
    if (p->draw_time > p->frame_period * M_EYE_BUSY) {
        p->eye_share = fmaxf(p->eye_share * M_EYE_SHRINK, M_EYE_SHARE_MIN);
        p->calm_frames = 0;
        // The evened-out time still tells of the larger size for a while,
        // and would shrink it again and again for the one overrun.
        p->draw_time *= M_EYE_SHRINK * M_EYE_SHRINK;
    } else if (p->draw_time < p->frame_period * M_EYE_IDLE) {
        if (++p->calm_frames >= M_EYE_CALM_FRAMES) {
            p->eye_share = fminf(p->eye_share * M_EYE_GROW, 1.0f);
            p->calm_frames = 0;
        }
    } else {
        p->calm_frames = 0;
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
    m_Priv.theater_eye = -1;
    RESULT result = M_CreateInstance();
    if (IS_OK(result)) {
        result = M_CreateSession(window);
    }
    if (IS_OK(result)) {
        result = M_CreateTargets();
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
    if (p->session != XR_NULL_HANDLE && p->eye_share > 0.0f) {
        LOG_INFO(
            "the eyes were last drawn at %d%% of their largest size; a "
            "frame took %.1f ms to prepare, %.1f ms of the graphics card "
            "and %.1f ms of waiting for it",
            (int32_t)(p->eye_share * 100.0f),
            1000.0 * p->work_total / MAX(p->timed_frames, 1),
            1000.0 * p->draw_total / MAX(p->draw_frames, 1),
            1000.0 * p->wait_total / MAX(p->timed_frames, 1));
    }
    VR_Gamepad_Shutdown();
    if (p->timer != 0) {
        if (p->is_timing) {
            glEndQuery(GL_TIME_ELAPSED);
        }
        glDeleteQueries(1, &p->timer);
    }
    M_DestroyTarget(&p->screens[0]);
    M_DestroyTarget(&p->screens[1]);
    M_DestroyTarget(&p->eyes[0]);
    M_DestroyTarget(&p->eyes[1]);
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
    *p = (M_PRIV) { .theater_eye = -1 };
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

bool VR_Session_BeginFrame(void)
{
    M_PRIV *const p = &m_Priv;
    if (!VR_Session_IsActive()) {
        return false;
    }
    if (p->is_frame_open) {
        return true;
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
    p->is_frame_open = true;
    p->should_render = frame_state.shouldRender;
    p->display_time = frame_state.predictedDisplayTime;
    p->frame_start = SDL_GetPerformanceCounter();
    p->frame_period = frame_state.predictedDisplayPeriod / 1e9;
    if (p->eye_share <= 0.0f) {
        p->eye_share = 1.0f;
    }
    M_CollectDrawTime();
    if (p->should_render && !p->has_timing) {
        if (p->timer == 0) {
            glGenQueries(1, &p->timer);
        }
        glBeginQuery(GL_TIME_ELAPSED, p->timer);
        p->is_timing = true;
    }
    for (int32_t i = 0; i < 2; i++) {
        // Whole pixels, and an even number of them.
        p->eyes[i].used_width = (int32_t)(p->eyes[i].width * p->eye_share) & ~1;
        p->eyes[i].used_height =
            (int32_t)(p->eyes[i].height * p->eye_share) & ~1;
        p->screens[i].used_width = p->screens[i].width;
        p->screens[i].used_height = p->screens[i].height;
    }
    p->has_view = false;
    p->screens[0].is_drawn = false;
    p->screens[1].is_drawn = false;
    p->eyes[0].is_drawn = false;
    p->eyes[1].is_drawn = false;
    VR_Gamepad_Update(p->session);
    if (!p->should_render) {
        return true;
    }
    if (p->needs_recenter && M_Recenter(p->display_time)) {
        p->needs_recenter = false;
    }
    M_LocateViews();
    return true;
}

const VR_VIEW *VR_Session_GetView(void)
{
    const M_PRIV *const p = &m_Priv;
    return p->is_frame_open && p->should_render && p->has_view ? &p->view
                                                               : nullptr;
}

bool VR_Session_AcquireEye(const int32_t eye, VR_SCREEN *const out_screen)
{
    return M_AcquireTarget(&m_Priv.eyes[eye], out_screen);
}

void VR_Session_ReleaseEye(const int32_t eye)
{
    M_ReleaseTarget(&m_Priv.eyes[eye]);
}

bool VR_Session_IsEyeDrawn(const int32_t eye)
{
    const M_PRIV *const p = &m_Priv;
    return p->is_frame_open
        && (p->eyes[eye].is_drawn || p->eyes[eye].is_acquired);
}

bool VR_Session_HasEyes(void)
{
    const M_PRIV *const p = &m_Priv;
    return p->is_frame_open && p->eyes[0].is_drawn && p->eyes[1].is_drawn;
}

bool VR_Session_AcquireScreen(const int32_t eye, VR_SCREEN *const out_screen)
{
    return M_AcquireTarget(&m_Priv.screens[eye], out_screen);
}

VR_MODE VR_Session_GetMode(void)
{
    return m_Priv.mode;
}

void VR_Session_ToggleMode(void)
{
    M_PRIV *const p = &m_Priv;
    p->mode = p->mode == VR_MODE_THEATER ? VR_MODE_INVOLVED : VR_MODE_THEATER;
    // Either way the player starts out looking straight at it.
    p->needs_recenter = true;
    LOG_INFO(
        "the headset shows the game %s",
        p->mode == VR_MODE_THEATER ? "on a screen" : "around the player");
}

bool VR_Session_IsStereo(void)
{
    const M_PRIV *const p = &m_Priv;
    return VR_Session_IsActive() && p->is_running;
}

void VR_Session_SetDrawnEye(const int32_t eye)
{
    m_Priv.theater_eye = eye;
}

int32_t VR_Session_GetDrawnEye(void)
{
    return VR_Session_IsActive() ? m_Priv.theater_eye : -1;
}

void VR_Session_EndFrame(const float aspect_ratio)
{
    M_PRIV *const p = &m_Priv;
    if (!p->is_frame_open) {
        return;
    }
    // The headset shows a picture from the moment it is given back, and
    // the driver may still be drawing it: OpenGL only queues what it is told
    // to draw. Shown early, a picture is partly the color it was cleared
    // to, which flashes in an eye for a frame. So the drawing is seen
    // through first, once for all the pictures of the frame, which lets the
    // game prepare one while the last is still being drawn.
    M_TARGET *const targets[] = {
        &p->screens[0],
        &p->screens[1],
        &p->eyes[0],
        &p->eyes[1],
    };
    bool is_any_acquired = false;
    for (int32_t i = 0; i < 4; i++) {
        is_any_acquired |= targets[i]->is_acquired;
    }
    if (p->is_timing) {
        glEndQuery(GL_TIME_ELAPSED);
        p->is_timing = false;
        p->has_timing = true;
    }
    if (is_any_acquired) {
        const Uint64 wait_start = SDL_GetPerformanceCounter();
        // A fence is passed when everything before it is drawn, and waiting
        // on it also sends off what was still queued.
        const GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000);
        glDeleteSync(fence);
        p->wait_total += (double)(SDL_GetPerformanceCounter() - wait_start)
            / (double)SDL_GetPerformanceFrequency();
        p->work_total += (double)(wait_start - p->frame_start)
            / (double)SDL_GetPerformanceFrequency();
        p->timed_frames++;
    }
    for (int32_t i = 0; i < 4; i++) {
        if (targets[i]->is_acquired) {
            xrReleaseSwapchainImage(targets[i]->swapchain, nullptr);
            targets[i]->is_acquired = false;
            targets[i]->is_drawn = true;
        }
    }
    const bool has_eyes = p->eyes[0].is_drawn && p->eyes[1].is_drawn;
    // A screen drawn for each eye has depth; one drawn once is flat, and
    // both eyes are shown the same picture.
    const bool has_depth = p->screens[0].is_drawn && p->screens[1].is_drawn;

    XrCompositionLayerProjectionView eye_views[2] = {};
    for (int32_t i = 0; i < 2; i++) {
        eye_views[i] = (XrCompositionLayerProjectionView) {
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
            .pose = p->xr_views[i].pose,
            .fov = p->xr_views[i].fov,
            .subImage = {
                .swapchain = p->eyes[i].swapchain,
                .imageRect = {
                    .extent = {
                        p->eyes[i].used_width,
                        p->eyes[i].used_height,
                    },
                },
            },
        };
    }
    const XrCompositionLayerProjection projection = {
        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .space = p->space,
        .viewCount = 2,
        .views = eye_views,
    };
    XrCompositionLayerQuad quads[2];
    for (int32_t i = 0; i < 2; i++) {
        quads[i] = (XrCompositionLayerQuad) {
            .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
            // Over the scene the screen only carries the interface, and
            // lets the scene through where the interface leaves it clear.
            .layerFlags = has_eyes
                ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                : 0,
            .space = p->space,
            .eyeVisibility = !has_depth ? XR_EYE_VISIBILITY_BOTH
                : i == 0 ? XR_EYE_VISIBILITY_LEFT
                         : XR_EYE_VISIBILITY_RIGHT,
            .subImage = {
                .swapchain = p->screens[i].swapchain,
                .imageRect = {
                    .extent = { p->screens[i].width, p->screens[i].height },
                },
            },
            .pose = {
                .orientation = { .w = 1.0f },
                .position = {
                    .z = has_eyes ? -M_PANEL_DISTANCE : -M_SCREEN_DISTANCE,
                },
            },
            .size = {
                has_eyes ? M_PANEL_SIZE : M_SCREEN_SIZE,
                (has_eyes ? M_PANEL_SIZE : M_SCREEN_SIZE) / aspect_ratio,
            },
        };
    }
    const XrCompositionLayerBaseHeader *layers[3];
    uint32_t layer_count = 0;
    if (has_eyes) {
        layers[layer_count++] =
            (const XrCompositionLayerBaseHeader *)&projection;
    }
    if (p->screens[0].is_drawn) {
        layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&quads[0];
    }
    if (has_depth) {
        layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&quads[1];
    }
    const XrFrameEndInfo info = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = p->display_time,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = layer_count,
        .layers = layers,
    };
    xrEndFrame(p->session, &info);
    p->is_frame_open = false;
}
