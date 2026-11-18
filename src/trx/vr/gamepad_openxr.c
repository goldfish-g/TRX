#include <trx/vr/gamepad_openxr.h>

#include <trx/core/log.h>

#include <SDL2/SDL.h>
#include <string.h>

// The far end of an axis of SDL.
#define M_AXIS_MAX 32767

// How far a stick has to lean to count as a direction of the d-pad.
#define M_DPAD_THRESHOLD 0.5f

typedef enum {
    M_KIND_BUTTON,
    M_KIND_TRIGGER,
    M_KIND_STICK,
    // A stick that works as the d-pad as well, which is what the game moves
    // Lara and its menus with, and which not every controller has.
    M_KIND_DPAD,
} M_KIND;

// The kinds of controller the runtime is told about. The first is the one of
// the Steam Frame, which is a gamepad split in two; the others have fewer
// buttons, and leave some of the gamepad out.
typedef enum {
    M_PROFILE_FRAME,
    M_PROFILE_FRAME_VALVE,
    M_PROFILE_TOUCH,
    M_PROFILE_INDEX,
    M_PROFILE_NUMBER_OF,
} M_PROFILE;

static const char *const m_ProfilePaths[M_PROFILE_NUMBER_OF] = {
    // SteamVR knows the controllers of the Steam Frame under two names: the
    // one its documentation gives, and the one it reports them as.
    [M_PROFILE_FRAME] = "/interaction_profiles/valve/frame_controller",
    [M_PROFILE_FRAME_VALVE] =
        "/interaction_profiles/valve/frame_controller_valve",
    [M_PROFILE_TOUCH] = "/interaction_profiles/oculus/touch_controller",
    [M_PROFILE_INDEX] = "/interaction_profiles/valve/index_controller",
};

// One button, trigger or stick of the gamepad, and where the controllers of
// each kind carry it. A path that is nullptr leaves it out for that kind.
typedef struct {
    const char *name;
    const char *label;
    M_KIND kind;
    // The button, or the axis of a trigger, or the horizontal axis of a
    // stick, which the vertical one follows.
    int32_t target;
    const char *paths[M_PROFILE_NUMBER_OF];
} M_INPUT;

static const M_INPUT m_Inputs[] = {
    // clang-format off
    { "a", "A", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_A, {
        [M_PROFILE_FRAME] = "/user/hand/right/input/a/click",
        [M_PROFILE_TOUCH] = "/user/hand/right/input/a/click",
        [M_PROFILE_INDEX] = "/user/hand/right/input/a/click",
    } },
    { "b", "B", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_B, {
        [M_PROFILE_FRAME] = "/user/hand/right/input/b/click",
        [M_PROFILE_TOUCH] = "/user/hand/right/input/b/click",
        [M_PROFILE_INDEX] = "/user/hand/right/input/b/click",
    } },
    { "x", "X", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_X, {
        [M_PROFILE_FRAME] = "/user/hand/right/input/x/click",
        [M_PROFILE_TOUCH] = "/user/hand/left/input/x/click",
        [M_PROFILE_INDEX] = "/user/hand/left/input/a/click",
    } },
    { "y", "Y", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_Y, {
        [M_PROFILE_FRAME] = "/user/hand/right/input/y/click",
        [M_PROFILE_TOUCH] = "/user/hand/left/input/y/click",
        [M_PROFILE_INDEX] = "/user/hand/left/input/b/click",
    } },
    { "back", "Back", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_BACK, {
        [M_PROFILE_FRAME] = "/user/hand/left/input/view/click",
        [M_PROFILE_TOUCH] = nullptr,
        [M_PROFILE_INDEX] = nullptr,
    } },
    { "start", "Start", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_START, {
        [M_PROFILE_FRAME] = "/user/hand/right/input/menu/click",
        [M_PROFILE_TOUCH] = "/user/hand/left/input/menu/click",
        [M_PROFILE_INDEX] = nullptr,
    } },
    { "dpad_up", "D-pad up", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_UP, {
        [M_PROFILE_FRAME] = "/user/hand/left/input/dpad_up/click",
        [M_PROFILE_TOUCH] = nullptr,
        [M_PROFILE_INDEX] = nullptr,
    } },
    { "dpad_down", "D-pad down", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_DOWN, {
        [M_PROFILE_FRAME] = "/user/hand/left/input/dpad_down/click",
        [M_PROFILE_TOUCH] = nullptr,
        [M_PROFILE_INDEX] = nullptr,
    } },
    { "dpad_left", "D-pad left", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_LEFT, {
        [M_PROFILE_FRAME] = "/user/hand/left/input/dpad_left/click",
        [M_PROFILE_TOUCH] = nullptr,
        [M_PROFILE_INDEX] = nullptr,
    } },
    { "dpad_right", "D-pad right", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, {
        [M_PROFILE_FRAME] = "/user/hand/left/input/dpad_right/click",
        [M_PROFILE_TOUCH] = nullptr,
        [M_PROFILE_INDEX] = nullptr,
    } },
    { "left_shoulder", "Left shoulder", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, {
        [M_PROFILE_FRAME] = "/user/hand/left/input/bumper/click",
        [M_PROFILE_TOUCH] = "/user/hand/left/input/squeeze/value",
        [M_PROFILE_INDEX] = "/user/hand/left/input/squeeze/value",
    } },
    { "right_shoulder", "Right shoulder", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, {
        [M_PROFILE_FRAME] = "/user/hand/right/input/bumper/click",
        [M_PROFILE_TOUCH] = "/user/hand/right/input/squeeze/value",
        [M_PROFILE_INDEX] = "/user/hand/right/input/squeeze/value",
    } },
    { "left_grip", "Left grip", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_PADDLE1, {
        [M_PROFILE_FRAME] = "/user/hand/left/input/squeeze/value",
        [M_PROFILE_TOUCH] = nullptr,
        [M_PROFILE_INDEX] = nullptr,
    } },
    { "right_grip", "Right grip", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_PADDLE2, {
        [M_PROFILE_FRAME] = "/user/hand/right/input/squeeze/value",
        [M_PROFILE_TOUCH] = nullptr,
        [M_PROFILE_INDEX] = nullptr,
    } },
    { "left_stick_click", "Left stick click", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_LEFTSTICK, {
        [M_PROFILE_FRAME] = "/user/hand/left/input/thumbstick/click",
        [M_PROFILE_TOUCH] = "/user/hand/left/input/thumbstick/click",
        [M_PROFILE_INDEX] = "/user/hand/left/input/thumbstick/click",
    } },
    { "right_stick_click", "Right stick click", M_KIND_BUTTON, SDL_CONTROLLER_BUTTON_RIGHTSTICK, {
        [M_PROFILE_FRAME] = "/user/hand/right/input/thumbstick/click",
        [M_PROFILE_TOUCH] = "/user/hand/right/input/thumbstick/click",
        [M_PROFILE_INDEX] = "/user/hand/right/input/thumbstick/click",
    } },
    { "left_trigger", "Left trigger", M_KIND_TRIGGER, SDL_CONTROLLER_AXIS_TRIGGERLEFT, {
        [M_PROFILE_FRAME] = "/user/hand/left/input/trigger/value",
        [M_PROFILE_TOUCH] = "/user/hand/left/input/trigger/value",
        [M_PROFILE_INDEX] = "/user/hand/left/input/trigger/value",
    } },
    { "right_trigger", "Right trigger", M_KIND_TRIGGER, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, {
        [M_PROFILE_FRAME] = "/user/hand/right/input/trigger/value",
        [M_PROFILE_TOUCH] = "/user/hand/right/input/trigger/value",
        [M_PROFILE_INDEX] = "/user/hand/right/input/trigger/value",
    } },
    { "left_stick", "Left stick", M_KIND_DPAD, 0, {
        [M_PROFILE_FRAME] = "/user/hand/left/input/thumbstick",
        [M_PROFILE_TOUCH] = "/user/hand/left/input/thumbstick",
        [M_PROFILE_INDEX] = "/user/hand/left/input/thumbstick",
    } },
    { "right_stick", "Right stick", M_KIND_STICK, SDL_CONTROLLER_AXIS_RIGHTX, {
        [M_PROFILE_FRAME] = "/user/hand/right/input/thumbstick",
        [M_PROFILE_TOUCH] = "/user/hand/right/input/thumbstick",
        [M_PROFILE_INDEX] = "/user/hand/right/input/thumbstick",
    } },
    // clang-format on
};

#define M_INPUT_COUNT ((int32_t)(sizeof(m_Inputs) / sizeof(m_Inputs[0])))

static XrActionSet m_ActionSet = XR_NULL_HANDLE;
static XrAction m_Actions[M_INPUT_COUNT] = {};
static XrInstance m_Instance = XR_NULL_HANDLE;
static XrResult m_LastSyncResult = XR_SUCCESS;
static int32_t m_DeviceIndex = -1;
static SDL_Joystick *m_Joystick = nullptr;

static int16_t M_ToAxis(const float value)
{
    const float clamped = value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
    return (int16_t)(clamped * M_AXIS_MAX);
}

// Says in the log which kind of controller the runtime takes each hand for,
// whenever that changes.
static void M_LogProfiles(const XrSession session)
{
    static XrPath last[2] = { XR_NULL_PATH, XR_NULL_PATH };
    const char *const hands[] = { "/user/hand/left", "/user/hand/right" };
    for (int32_t i = 0; i < 2; i++) {
        XrPath hand = XR_NULL_PATH;
        xrStringToPath(m_Instance, hands[i], &hand);
        XrInteractionProfileState state = {
            .type = XR_TYPE_INTERACTION_PROFILE_STATE,
        };
        if (XR_FAILED(xrGetCurrentInteractionProfile(session, hand, &state))
            || state.interactionProfile == last[i]) {
            continue;
        }
        last[i] = state.interactionProfile;
        char name[XR_MAX_PATH_LENGTH] = "none";
        uint32_t length = 0;
        if (state.interactionProfile != XR_NULL_PATH) {
            xrPathToString(
                m_Instance, state.interactionProfile, sizeof(name), &length,
                name);
        }
        LOG_INFO("%s is held as %s", hands[i], name);
    }
}

static RESULT M_SuggestBindings(
    const XrInstance instance, const M_PROFILE profile_id)
{
    const char *const profile = m_ProfilePaths[profile_id];
    XrActionSuggestedBinding bindings[M_INPUT_COUNT];
    uint32_t count = 0;
    for (int32_t i = 0; i < M_INPUT_COUNT; i++) {
        const M_INPUT *const input = &m_Inputs[i];
        const char *const path = input->paths[
            profile_id == M_PROFILE_FRAME_VALVE ? M_PROFILE_FRAME : profile_id];
        if (path == nullptr) {
            continue;
        }
        bindings[count].action = m_Actions[i];
        FAIL_IF(
            XR_FAILED(xrStringToPath(instance, path, &bindings[count].binding)),
            "the runtime does not know the controller path %s", path);
        count++;
    }

    XrInteractionProfileSuggestedBinding suggested = {
        .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .countSuggestedBindings = count,
        .suggestedBindings = bindings,
    };
    FAIL_IF(
        XR_FAILED(
            xrStringToPath(instance, profile, &suggested.interactionProfile)),
        "the runtime does not know the controllers %s", profile);
    FAIL_IF(
        XR_FAILED(xrSuggestInteractionProfileBindings(instance, &suggested)),
        "the runtime does not take the bindings for %s", profile);
    return OK;
}

static RESULT M_AttachDevice(void)
{
    FAIL_IF(
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0,
        "SDL has no gamepads: %s", SDL_GetError());
    const SDL_VirtualJoystickDesc desc = {
        .version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION,
        .type = SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        .naxes = SDL_CONTROLLER_AXIS_MAX,
        .nbuttons = SDL_CONTROLLER_BUTTON_MAX,
        .name = "VR controllers",
    };
    m_DeviceIndex = SDL_JoystickAttachVirtualEx(&desc);
    FAIL_IF(
        m_DeviceIndex < 0, "SDL does not take the VR controllers: %s",
        SDL_GetError());
    // The gamepad is moved through a handle of its own, next to the one the
    // game opens to read it.
    m_Joystick = SDL_JoystickOpen(m_DeviceIndex);
    FAIL_IF(
        m_Joystick == nullptr, "SDL does not open the VR controllers: %s",
        SDL_GetError());
    return OK;
}

RESULT VR_Gamepad_Init(const XrInstance instance, const XrSession session)
{
    m_Instance = instance;
    XrActionSetCreateInfo set_info = {
        .type = XR_TYPE_ACTION_SET_CREATE_INFO,
        .actionSetName = "gamepad",
        .localizedActionSetName = "Gamepad",
    };
    FAIL_IF(
        XR_FAILED(xrCreateActionSet(instance, &set_info, &m_ActionSet)),
        "the runtime takes no controller actions");

    for (int32_t i = 0; i < M_INPUT_COUNT; i++) {
        const M_INPUT *const input = &m_Inputs[i];
        XrActionCreateInfo info = {
            .type = XR_TYPE_ACTION_CREATE_INFO,
            .actionType = input->kind == M_KIND_BUTTON
                ? XR_ACTION_TYPE_BOOLEAN_INPUT
                : input->kind == M_KIND_TRIGGER ? XR_ACTION_TYPE_FLOAT_INPUT
                                                : XR_ACTION_TYPE_VECTOR2F_INPUT,
        };
        strncpy(info.actionName, input->name, sizeof(info.actionName) - 1);
        strncpy(
            info.localizedActionName, input->label,
            sizeof(info.localizedActionName) - 1);
        FAIL_IF(
            XR_FAILED(xrCreateAction(m_ActionSet, &info, &m_Actions[i])),
            "the runtime does not take the controller action %s", input->name);
    }

    MUST(M_SuggestBindings(instance, M_PROFILE_TOUCH));
    // Controllers of a kind the runtime does not know are not an error: the
    // runtime maps the ones it has onto a kind it was told about.
    IGNORE(M_SuggestBindings(instance, M_PROFILE_INDEX));
    IGNORE(M_SuggestBindings(instance, M_PROFILE_FRAME_VALVE));
    const RESULT frame = M_SuggestBindings(instance, M_PROFILE_FRAME);
    LOG_INFO(
        "Steam Frame controllers are %s", IS_OK(frame) ? "known" : "not known");
    IGNORE(frame);

    const XrSessionActionSetsAttachInfo attach_info = {
        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1,
        .actionSets = &m_ActionSet,
    };
    FAIL_IF(
        XR_FAILED(xrAttachSessionActionSets(session, &attach_info)),
        "the headset does not take the controller actions");

    MUST(M_AttachDevice());
    return OK;
}

void VR_Gamepad_Shutdown(void)
{
    if (m_Joystick != nullptr) {
        SDL_JoystickClose(m_Joystick);
        m_Joystick = nullptr;
    }
    if (m_DeviceIndex >= 0) {
        SDL_JoystickDetachVirtual(m_DeviceIndex);
        m_DeviceIndex = -1;
    }
    if (m_ActionSet != XR_NULL_HANDLE) {
        // The actions go with their set.
        xrDestroyActionSet(m_ActionSet);
        m_ActionSet = XR_NULL_HANDLE;
    }
    for (int32_t i = 0; i < M_INPUT_COUNT; i++) {
        m_Actions[i] = XR_NULL_HANDLE;
    }
}

void VR_Gamepad_Update(const XrSession session)
{
    if (m_Joystick == nullptr) {
        return;
    }
    const XrActiveActionSet active = { .actionSet = m_ActionSet };
    const XrActionsSyncInfo sync_info = {
        .type = XR_TYPE_ACTIONS_SYNC_INFO,
        .countActiveActionSets = 1,
        .activeActionSets = &active,
    };
    // Anything but a plain success means the game does not have the
    // controllers at the moment, and then every action reads as let go.
    const XrResult sync_result = xrSyncActions(session, &sync_info);
    const bool is_live = sync_result == XR_SUCCESS;
    if (sync_result != m_LastSyncResult) {
        m_LastSyncResult = sync_result;
        LOG_INFO("the controllers answer with %d", sync_result);
    }
    if (is_live) {
        M_LogProfiles(session);
    }

    // Several inputs can stand for one button of the gamepad, which is down
    // while any of them is.
    bool buttons[SDL_CONTROLLER_BUTTON_MAX] = {};
    for (int32_t i = 0; i < M_INPUT_COUNT; i++) {
        const M_INPUT *const input = &m_Inputs[i];
        const XrActionStateGetInfo get_info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = m_Actions[i],
        };
        if (input->kind == M_KIND_BUTTON) {
            XrActionStateBoolean state = { .type =
                                               XR_TYPE_ACTION_STATE_BOOLEAN };
            const bool is_down = is_live
                && XR_SUCCEEDED(
                    xrGetActionStateBoolean(session, &get_info, &state))
                && state.isActive && state.currentState;
            buttons[input->target] |= is_down;
        } else if (input->kind == M_KIND_TRIGGER) {
            XrActionStateFloat state = { .type = XR_TYPE_ACTION_STATE_FLOAT };
            const bool is_read = is_live
                && XR_SUCCEEDED(
                    xrGetActionStateFloat(session, &get_info, &state))
                && state.isActive;
            // A trigger of SDL rests at the bottom of its range.
            const float value = is_read ? state.currentState : 0.0f;
            SDL_JoystickSetVirtualAxis(
                m_Joystick, input->target, M_ToAxis(value * 2.0f - 1.0f));
        } else {
            XrActionStateVector2f state = { .type =
                                                XR_TYPE_ACTION_STATE_VECTOR2F };
            const bool is_read = is_live
                && XR_SUCCEEDED(
                    xrGetActionStateVector2f(session, &get_info, &state))
                && state.isActive;
            const float x = is_read ? state.currentState.x : 0.0f;
            const float y = is_read ? state.currentState.y : 0.0f;
            if (input->kind == M_KIND_DPAD) {
                buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] |=
                    x < -M_DPAD_THRESHOLD;
                buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] |=
                    x > M_DPAD_THRESHOLD;
                buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] |= y > M_DPAD_THRESHOLD;
                buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] |=
                    y < -M_DPAD_THRESHOLD;
                continue;
            }
            SDL_JoystickSetVirtualAxis(m_Joystick, input->target, M_ToAxis(x));
            // A stick of SDL points down where one of OpenXR points up.
            SDL_JoystickSetVirtualAxis(
                m_Joystick, input->target + 1, M_ToAxis(-y));
        }
    }
    for (int32_t i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        SDL_JoystickSetVirtualButton(m_Joystick, i, buttons[i]);
    }
}
