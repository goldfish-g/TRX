#include <trx/game/lara/modern.h>

#include <trx/config.h>
#include <trx/core/math/const.h>
#include <trx/core/utils.h>
#include <trx/game/camera.h>
#include <trx/game/input/analog.h>
#include <trx/game/input/common.h>
#include <trx/game/lara.h>

int32_t Lara_ModernGetTargetAngle(void)
{
    if (!g_Config.gameplay.enable_modern_controls) {
        return MODERN_ANGLE_NONE;
    }
    if (g_AnalogInput.magnitude == 0) {
        return MODERN_ANGLE_NONE;
    }
    if (g_Camera.type != CAM_CHASE && g_Camera.type != CAM_COMBAT) {
        return MODERN_ANGLE_NONE;
    }
    return (int32_t)(int16_t)(
        g_AnalogInput.direction + g_Camera.modern_cam_angle);
}

bool Lara_ModernTurn(const int16_t rate, const int16_t max_turn)
{
    const int32_t target32 = Lara_ModernGetTargetAngle();
    if (target32 == MODERN_ANGLE_NONE) {
        return false;
    }

    ITEM *const item = Lara_GetItem();
    LARA_INFO *const lara = Lara_GetLaraInfo();
    const int16_t target = (int16_t)target32;
    const int16_t delta = target - item->rot.y;

    // Straight-ahead dead zone: suppress turning within ±6°
    if (ABS(delta) < MODERN_STRAIGHT_ZONE) {
        lara->turn_rate = 0;
        return true;
    }

    // Scale max turn by stick magnitude (0..256)
    int16_t scaled_max = (int16_t)((int32_t)max_turn * g_AnalogInput.magnitude >> 8);
    if (scaled_max < rate) {
        scaled_max = rate;
    }

    if (delta > 0) {
        lara->turn_rate += rate;
        CLAMPG(lara->turn_rate, scaled_max);
    } else if (delta < 0) {
        lara->turn_rate -= rate;
        CLAMPL(lara->turn_rate, -scaled_max);
    }

    // Anti-oscillation: snap when remaining delta is smaller than turn_rate
    if (ABS(delta) < ABS(lara->turn_rate)) {
        lara->turn_rate = delta;
    }

    return true;
}

bool Lara_ModernTurnWithLean(
    const int16_t rate, const int16_t max_turn, const int16_t lean_rate,
    const int16_t lean_max)
{
    const int32_t target32 = Lara_ModernGetTargetAngle();
    if (target32 == MODERN_ANGLE_NONE) {
        return false;
    }

    ITEM *const item = Lara_GetItem();
    LARA_INFO *const lara = Lara_GetLaraInfo();
    const int16_t target = (int16_t)target32;
    const int16_t delta = target - item->rot.y;

    // Straight-ahead dead zone: suppress turning within ±6°
    if (ABS(delta) < MODERN_STRAIGHT_ZONE) {
        lara->turn_rate = 0;
        return true;
    }

    // Scale maximums by stick magnitude
    int16_t scaled_turn_max =
        (int16_t)((int32_t)max_turn * g_AnalogInput.magnitude >> 8);
    if (scaled_turn_max < rate) {
        scaled_turn_max = rate;
    }
    int16_t scaled_lean_max =
        (int16_t)((int32_t)lean_max * g_AnalogInput.magnitude >> 8);

    // Turn
    if (delta > 0) {
        lara->turn_rate += rate;
        CLAMPG(lara->turn_rate, scaled_turn_max);
    } else if (delta < 0) {
        lara->turn_rate -= rate;
        CLAMPL(lara->turn_rate, -scaled_turn_max);
    }

    // Anti-oscillation
    if (ABS(delta) < ABS(lara->turn_rate)) {
        lara->turn_rate = delta;
    }

    // Lean: positional, proportional to angular delta.
    // When delta is 0 (facing target), lean returns to 0.
    // When delta is DEG_90, lean reaches lean_max.
    int16_t target_lean =
        (int16_t)((int32_t)delta * lean_max / (int32_t)DEG_90);
    target_lean =
        (int16_t)((int32_t)target_lean * g_AnalogInput.magnitude >> 8);
    CLAMPG(target_lean, lean_max);
    CLAMPL(target_lean, -lean_max);

    if (item->rot.z < target_lean) {
        item->rot.z += lean_rate;
        CLAMPG(item->rot.z, target_lean);
    } else if (item->rot.z > target_lean) {
        item->rot.z -= lean_rate;
        CLAMPL(item->rot.z, target_lean);
    }

    return true;
}

// Positive list of states where input remapping is allowed.
// Uses LS_U() to convert the game-specific state ID back to the internal
// TRX enum, so we can use compile-time enum constants as case labels.
static bool M_IsRemappableState(const int16_t state)
{
    switch (LS_U(state)) {
    case LS_STOP:
    case LS_POSE:
    case LS_WALK:
    case LS_RUN:
    case LS_SPRINT:
    case LS_WADE:
    case LS_WALK_BACK:
    case LS_FAST_BACK:
    case LS_TURN_LEFT:
    case LS_TURN_RIGHT:
    case LS_FAST_TURN:
    case LS_STEP_LEFT:
    case LS_STEP_RIGHT:
    case LS_ROLL:
    case LS_ROLL_CONT:
    case LS_LAND:
    case LS_SLIDE:
    case LS_SLIDE_BACK:
    case LS_COMPRESS:
        return true;
    default:
        return false;
    }
}

// Threshold for raw stick-to-digital conversion (~37% of axis range).
// Prevents minor cross-axis bleed from triggering unwanted inputs.
#define M_DIGITAL_THRESHOLD 48

void Lara_ModernRemapInput(void)
{
    if (!g_Config.gameplay.enable_modern_controls) {
        return;
    }

    if (g_AnalogInput.magnitude == 0) {
        return;
    }

    const ITEM *const item = Lara_GetItem();

    // For non-remappable states (hang, shimmy, climb, swim, etc.),
    // pass through raw stick direction as digital inputs.
    // The left stick is mapped to camera roles in controller.def,
    // so without this, these states receive no directional input
    // from the analog stick.
    if (!M_IsRemappableState(item->current_anim_state)) {
        if (g_AnalogInput.stick_y < -M_DIGITAL_THRESHOLD) {
            g_Input.forward = 1;
        } else if (g_AnalogInput.stick_y > M_DIGITAL_THRESHOLD) {
            g_Input.back = 1;
        }
        if (g_AnalogInput.stick_x < -M_DIGITAL_THRESHOLD) {
            g_Input.left = 1;
        } else if (g_AnalogInput.stick_x > M_DIGITAL_THRESHOLD) {
            g_Input.right = 1;
        }
        return;
    }

    // Camera-relative remapping for movement states
    const int32_t target32 = Lara_ModernGetTargetAngle();
    if (target32 == MODERN_ANGLE_NONE) {
        return;
    }

    const int16_t delta = (int16_t)target32 - item->rot.y;
    const int16_t abs_delta = ABS(delta);

    // Clear directional inputs — we set the appropriate ones below
    g_Input.forward = 0;
    g_Input.back = 0;
    g_Input.left = 0;
    g_Input.right = 0;

    if (g_Input.slow) {
        // Walk mode: side zones = sidestep
        if (abs_delta < MODERN_FORWARD_ZONE) {
            g_Input.forward = 1;
        } else if (abs_delta > MODERN_BACK_ZONE) {
            g_Input.back = 1;
        } else if (delta > 0) {
            g_Input.step_right = 1;
        } else {
            g_Input.step_left = 1;
        }
    } else {
        // Run mode: side zones = turn in place
        if (abs_delta < MODERN_FORWARD_ZONE) {
            g_Input.forward = 1;
        } else if (abs_delta > MODERN_BACK_ZONE) {
            g_Input.back = 1;
        } else if (delta > 0) {
            g_Input.right = 1;
        } else {
            g_Input.left = 1;
        }
    }
}
