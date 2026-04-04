#include <trx/game/camera/common.h>

#include <trx/config.h>
#include <trx/core/math/const.h>
#include <trx/core/math/trig.h>
#include <trx/core/utils.h>
#include <trx/game/camera.h>
#include <trx/game/input.h>
#include <trx/game/input/analog.h>
#include <trx/game/input/backends/keyboard.h>
#include <trx/game/lara.h>
#include <trx/game/lara/modern.h>
#include <trx/game/matrix.h>
#include <trx/game/random.h>
#include <trx/game/rooms.h>

#include <SDL2/SDL.h>

#define M_CHASE_ELEVATION (WALL_L * 3 / 2) // = 1536

static CAMERA_STRATEGY m_Strategies[CAMERA_MODE_NUMBER_OF] = {};

// Camera speed option ranges from 1-10, so index 0 is unused.
static const double m_ManualCameraMultiplier[11] = {
    1.0, .5, .625, .75, .875, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0,
};

static bool m_IsChunky = false;
static bool m_LastInputWasMouse = false;

#define M_ROLL_ARC_PULL 40 // % to reduce distance at arc midpoint
static bool m_IsInitialised = false;

static void M_OffsetAdditionalAngle(const int16_t delta)
{
    g_Camera.additional_angle += delta;
}

static void M_OffsetAdditionalElevation(const int16_t delta)
{
    // Do not allow elevation to overflow.
    int32_t new_elevation = g_Camera.additional_elevation + delta;
    CLAMP(new_elevation, INT16_MIN, INT16_MAX);
    g_Camera.additional_elevation = new_elevation;
}

static void M_OffsetReset(void)
{
    g_Camera.additional_angle = 0;
    g_Camera.additional_elevation = 0;
}

static const CAMERA_STRATEGY *M_GetStrategy(void)
{
    return &m_Strategies[g_Config.visuals.camera_mode];
}

const CAMERA_LOOK_SETTINGS *Camera_GetLookSettings(const bool on_surface)
{
    return M_GetStrategy()->get_look_settings_func(on_surface);
}

void Camera_RegisterStrategy(
    const CAMERA_MODE mode, const CAMERA_STRATEGY strategy)
{
    m_Strategies[mode] = strategy;
}

bool Camera_IsChunky(void)
{
    return m_IsChunky;
}

void Camera_SetChunky(const bool is_chunky)
{
    m_IsChunky = is_chunky;
}

void Camera_Initialise(void)
{
    m_IsInitialised = false;
    Matrix_ResetStack();
    g_Camera.last = NO_CAMERA;
    g_Camera.underwater = false;
    Camera_ResetPosition();
    Camera_Update();
    m_IsInitialised = true;
}

void Camera_ResetPosition(void)
{
    const CAMERA_STRATEGY *const strategy = M_GetStrategy();
    strategy->reset_func();

    g_Camera.roll = 0;
    g_Camera.target_distance = CAMERA_DEFAULT_DISTANCE;
    g_Camera.item = nullptr;
    g_Camera.speed = 1;
    g_Camera.flags = CF_NORMAL;
    g_Camera.bounce = 0;
    g_Camera.num = NO_CAMERA;
    g_Camera.fixed_camera = false;
    g_Camera.additional_angle = 0;
    g_Camera.additional_elevation = 0;
    g_Camera.modern_cam_angle = Lara_GetItem()->rot.y;
    g_Camera.modern_roll_active = false;
    const LARA_INFO *const lara_info = Lara_GetLaraInfo();
    if (!lara_info->extra_anim) {
        g_Camera.type = CAM_CHASE;
    }
}

void Camera_Reset(void)
{
    g_Camera.mic_pos.room_num = NO_ROOM;
    g_Camera.pos.room_num = NO_ROOM;
}

void Camera_ApplyBounce(void)
{
    if (g_Camera.bounce > 0) {
        g_Camera.pos.y += g_Camera.bounce;
        g_Camera.target.y += g_Camera.bounce;
        g_Camera.bounce = 0;
    } else if (g_Camera.bounce < 0) {
        const XYZ_32 shake = {
            .x = g_Camera.bounce * (Random_GetControl() - 0x4000) / 0x7FFF,
            .y = g_Camera.bounce * (Random_GetControl() - 0x4000) / 0x7FFF,
            .z = g_Camera.bounce * (Random_GetControl() - 0x4000) / 0x7FFF,
        };
        g_Camera.pos.x += shake.x;
        g_Camera.pos.y += shake.y;
        g_Camera.pos.z += shake.z;
        g_Camera.target.y += shake.x;
        g_Camera.target.y += shake.y;
        g_Camera.target.z += shake.z;
        g_Camera.bounce += 5;
    }
}

void Camera_ClampInterpResult(void)
{
    if (g_Camera.type == CAM_PHOTO_MODE) {
        Room_GetSector(
            (XYZ_32) {
                g_Camera.interp.result.pos.x,
                g_Camera.interp.result.pos.y + g_Camera.interp.result.shift,
                g_Camera.interp.result.pos.z,
            },
            &g_Camera.interp.room_num);
        return;
    }

    const CAMERA_STRATEGY *const strategy = M_GetStrategy();
    strategy->clamp_result_func();
}

void Camera_Update(void)
{
    if (g_Camera.type == CAM_PHOTO_MODE) {
        Camera_PhotoMode_Update();
        Camera_EnsureEnvironment();
        return;
    }

    if (g_Camera.type == CAM_CINEMATIC) {
        Camera_LoadCutsceneFrame();
        Camera_EnsureEnvironment();
        return;
    }

    if (g_Camera.flags != CF_NO_CHUNKY) {
        Camera_SetChunky(true);
    }

    const bool fixed_camera = g_Camera.item != nullptr
        && (g_Camera.type == CAM_FIXED || g_Camera.type == CAM_HEAVY);
    const ITEM *const item = fixed_camera ? g_Camera.item : Lara_GetItem();

    const BOUNDS_16 *const bounds = Item_GetBoundsAccurate(item);
    int32_t y = item->pos.y;
    if (fixed_camera) {
        y += (bounds->min.y + bounds->max.y) / 2;
    } else {
        y += bounds->max.y
            + (((int32_t)(bounds->min.y - bounds->max.y)) * 3 >> 2);
    }

    const CAMERA_STRATEGY *const strategy = M_GetStrategy();
    strategy->update_func(item, fixed_camera, y);

    g_Camera.last = g_Camera.num;
    g_Camera.fixed_camera = fixed_camera;

    switch (g_Camera.type) {
    case CAM_LOOK:
    case CAM_CINEMATIC:
    case CAM_COMBAT:
    case CAM_FIXED:
        g_Camera.additional_angle = 0;
        g_Camera.additional_elevation = 0;
        break;

    default:
        break;
    }

    if (g_Camera.type != CAM_HEAVY || g_Camera.timer == -1) {
        // Re-sync modern camera angle when returning to chase mode
        // from fixed/look/combat cameras to prevent snapping.
        if (g_Config.gameplay.enable_modern_controls
            && g_Camera.type != CAM_CHASE) {
            g_Camera.modern_cam_angle = Math_Atan(
                g_Camera.target.z - g_Camera.pos.z,
                g_Camera.target.x - g_Camera.pos.x);
        }
        g_Camera.type = CAM_CHASE;
        g_Camera.num = NO_CAMERA;
        g_Camera.last_item = g_Camera.item;
        g_Camera.item = nullptr;
        g_Camera.target_angle = g_Camera.additional_angle;
        g_Camera.target_elevation = g_Camera.additional_elevation;
        g_Camera.target_distance = M_CHASE_ELEVATION;
        g_Camera.flags = CF_NORMAL;
        if (g_Config.visuals.camera_mode != CAMERA_MODE_TR1) {
            g_Camera.speed = strategy->get_chase_speed_func();
        }
    }
    Camera_SetChunky(false);
    if (m_IsInitialised) {
        Camera_EnsureEnvironment();
    }
}

void Camera_MoveManual(void)
{
    if (g_Input.camera_reset) {
        M_OffsetReset();
    }

    if (!g_Config.gameplay.enable_manual_camera) {
        return;
    }

    const int16_t camera_delta = (const int32_t)(DEG_90 / LOGIC_FPS)
        * (double)m_ManualCameraMultiplier[g_Config.gameplay.camera_speed];

    if (g_Input.camera_left) {
        M_OffsetAdditionalAngle(camera_delta);
    } else if (g_Input.camera_right) {
        M_OffsetAdditionalAngle(-camera_delta);
    }
    if (g_Input.camera_forward) {
        M_OffsetAdditionalElevation(-camera_delta);
    } else if (g_Input.camera_back) {
        M_OffsetAdditionalElevation(camera_delta);
    }
}

static bool m_MouseCaptured = false;

static void M_EnsureMouseCaptured(const bool capture)
{
    if (capture != m_MouseCaptured) {
        SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE);
        m_MouseCaptured = capture;
        // Drain any stale mouse delta from the mode switch
        if (capture) {
            int32_t dx, dy;
            Input_Keyboard_GetMouseDelta(&dx, &dy);
        }
    }
}

// Read mouse motion delta and apply to yaw/elevation.
// Returns true if mouse moved (sets m_LastInputWasMouse).
static bool M_ProcessMouse(int16_t *yaw, int16_t *elev)
{
    int32_t mouse_dx = 0;
    int32_t mouse_dy = 0;
    Input_Keyboard_GetMouseDelta(&mouse_dx, &mouse_dy);
    if (mouse_dx == 0 && mouse_dy == 0) {
        return false;
    }
    m_LastInputWasMouse = true;
    const int32_t sens = g_Config.gameplay.mouse_sensitivity;
    *yaw = (int16_t)(mouse_dx * sens * DEG_1 / 16);
    if (g_Config.gameplay.invert_camera_x) {
        *yaw = -*yaw;
    }
    *elev = (int16_t)(mouse_dy * sens * DEG_1 / 16);
    if (g_Config.gameplay.invert_camera_y) {
        *elev = -*elev;
    }
    return true;
}

// Apply analog stick input to yaw/elevation.
static void M_ProcessStick(
    const int16_t stick_x, const int16_t stick_y, const int16_t speed,
    int16_t *yaw, int16_t *elev)
{
    if (stick_x != 0) {
        *yaw += (int16_t)((int32_t)stick_x * speed >> 7);
    }
    if (stick_y != 0) {
        *elev += (int16_t)((int32_t)stick_y * speed >> 7);
    }
}

void Camera_MoveModern(void)
{
    M_EnsureMouseCaptured(true);

    const int16_t camera_speed = (int32_t)(DEG_90 / LOGIC_FPS)
        * (double)m_ManualCameraMultiplier[g_Config.gameplay.camera_speed];

    // Look mode: both sticks + mouse orbit the camera freely.
    // Don't touch modern_cam_angle — it resumes when look is released.
    if (g_Input.look) {
        int16_t yaw_delta = 0;
        int16_t elev_delta = 0;

        int16_t mouse_yaw = 0, mouse_elev = 0;
        if (M_ProcessMouse(&mouse_yaw, &mouse_elev)) {
            yaw_delta -= mouse_yaw;
            elev_delta -= mouse_elev;
        }

        // Left stick as camera
        M_ProcessStick(
            g_AnalogInput.stick_x, g_AnalogInput.stick_y, camera_speed,
            &yaw_delta, &elev_delta);

        // Right stick as camera
        M_ProcessStick(
            g_AnalogCamInput.stick_x, g_AnalogCamInput.stick_y, camera_speed,
            &yaw_delta, &elev_delta);

        if (yaw_delta != 0) {
            M_OffsetAdditionalAngle(-yaw_delta);
        }
        if (elev_delta != 0) {
            M_OffsetAdditionalElevation(-elev_delta);
        }
        return;
    }

    // Re-sync modern_cam_angle after look mode ends so the camera
    // doesn't snap back to the pre-look position.
    // Detect by checking if additional_angle was set by look mode.
    // (Camera_Update resets additional_angle for CAM_LOOK each frame,
    // so after look ends the type returns to CAM_CHASE.)

    if (g_Input.camera_reset) {
        g_Camera.modern_cam_angle = Lara_GetItem()->rot.y;
        g_Camera.additional_angle = 0;
        g_Camera.additional_elevation = 0;
        g_Camera.modern_roll_active = false;
        return;
    }

    // Mouse camera control
    int16_t mouse_yaw = 0, mouse_elev = 0;
    if (M_ProcessMouse(&mouse_yaw, &mouse_elev)) {
        g_Camera.modern_cam_angle += mouse_yaw;
        int32_t new_elev = g_Camera.additional_elevation - mouse_elev;
        CLAMP(new_elev, INT16_MIN, INT16_MAX);
        g_Camera.additional_elevation = (int16_t)new_elev;
    }

    // Right stick yaw
    if (g_AnalogCamInput.stick_x != 0) {
        m_LastInputWasMouse = false;
        int16_t yaw =
            (int16_t)((int32_t)g_AnalogCamInput.stick_x * camera_speed >> 7);
        if (g_Config.gameplay.invert_camera_x) {
            yaw = -yaw;
        }
        g_Camera.modern_cam_angle += yaw;
    }

    // Right stick elevation (inverted by default: stick up = look up)
    if (g_AnalogCamInput.stick_y != 0) {
        m_LastInputWasMouse = false;
        int16_t elev =
            (int16_t)((int32_t)g_AnalogCamInput.stick_y * camera_speed >> 7);
        if (g_Config.gameplay.invert_camera_y) {
            elev = -elev;
        }
        int32_t new_elev = g_Camera.additional_elevation - elev;
        CLAMP(new_elev, INT16_MIN, INT16_MAX);
        g_Camera.additional_elevation = (int16_t)new_elev;
    }

    // Roll arc: proportionally sweep modern_cam_angle toward a target
    // set by M_Turn180. Closes 1/6th of the remaining gap each frame
    // (ease-out: fast start, smooth finish). Camera distance is reduced
    // at the midpoint for a shallower arc path. Speed=1 + target_angle
    // sync ensure the chase camera tracks the arc exactly on each frame.
    if (g_Camera.modern_roll_active) {
        const int16_t delta =
            g_Camera.modern_roll_target - g_Camera.modern_cam_angle;
        if (ABS(delta) < DEG_1) {
            g_Camera.modern_cam_angle = g_Camera.modern_roll_target;
            g_Camera.modern_roll_active = false;
        } else {
            int16_t step = delta / 6;
            if (step == 0) {
                step = (delta > 0) ? 1 : -1;
            }
            g_Camera.modern_cam_angle += step;

            // Triangle distance profile: pull camera closer at midpoint.
            // Approximate progress as 0..1 using remaining delta vs 180°.
            const int32_t abs_delta = ABS(delta);
            const int32_t half = (int32_t)DEG_90;
            int32_t pull;
            if (abs_delta > half) {
                pull = (int32_t)DEG_180 - abs_delta; // 0 → DEG_90
            } else {
                pull = abs_delta; // DEG_90 → 0
            }
            g_Camera.target_distance = CAMERA_DEFAULT_DISTANCE
                - CAMERA_DEFAULT_DISTANCE * M_ROLL_ARC_PULL / 100 * pull
                    / half;
        }
        g_Camera.speed = 1;
        g_Camera.target_angle =
            g_Camera.modern_cam_angle - Lara_GetItem()->rot.y;
    }

    // Camera creep: drift toward Lara's facing direction.
    // When stick is off-axis (target != camera forward), use stronger
    // proportional creep so the camera follows even after Lara has
    // finished turning. turn_rate component gives responsive following
    // during active turns.
    if (Lara_GetItem()->speed > 0 && !m_LastInputWasMouse
        && g_AnalogCamInput.stick_x == 0 && g_AnalogCamInput.stick_y == 0) {
        const LARA_INFO *const lara = Lara_GetLaraInfo();
        const int16_t cam_delta =
            Lara_GetItem()->rot.y - g_Camera.modern_cam_angle;
        int16_t step = lara->turn_rate / 3;

        const int32_t target32 = Lara_ModernGetTargetAngle();
        if (target32 != MODERN_ANGLE_NONE) {
            // Stick active and off-axis: stronger proportional creep.
            // Pure proportional — no hard cap, so there's no flat-rate
            // region that would feel jerky.
            step += cam_delta / 10;
        } else {
            // No stick input: gentle convergence only
            step += cam_delta / 192;
        }
        g_Camera.modern_cam_angle += step;
    }

    // Decouple chase camera orbit from Lara's rotation.
    // Chase orbit = additional_angle + lara_rot_y (in M_GetIdeal).
    // By setting additional_angle = modern_cam_angle - lara_rot_y,
    // the orbit becomes: (modern_cam_angle - lara_rot_y) + lara_rot_y
    //                  = modern_cam_angle (player-controlled, stable).
    g_Camera.additional_angle =
        g_Camera.modern_cam_angle - Lara_GetItem()->rot.y;
}

void Camera_Apply(void)
{
    Matrix_LookAt(
        g_Camera.interp.result.pos.x,
        g_Camera.interp.result.pos.y + g_Camera.interp.result.shift,
        g_Camera.interp.result.pos.z, g_Camera.interp.result.target.x,
        g_Camera.interp.result.target.y, g_Camera.interp.result.target.z,
        g_Camera.roll);
}
