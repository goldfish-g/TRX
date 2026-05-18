#include <trx/config.h>
#include <trx/core/utils.h>
#include <trx/game/camera.h>
#include <trx/game/input.h>
#include <trx/game/input/analog.h>
#include <trx/game/lara.h>
#include <trx/game/lara/modern.h>
#include <trx/game/lara/util.h>

// clang-format off
#define M_FRICTION       6
#define M_LEAN_RATE      (2 * LARA_LEAN_RATE) // = 546
#define M_TURN_RATE      (2 * DEG_1)          // = 364
#define M_MAX_SURF_SPEED 60
#define M_MAX_SWIM_SPEED 200
// clang-format on

// Camera-relative underwater aim, modeled on the remaster's
// ModernControlsSwim: the stick picks a world-direction, Lara rotates
// toward it, and pitch tracks the camera elevation. Two differences:
//   - We rate-cap rot.y at LARA_MED_TURN/frame (the original engine's
//     underwater yaw cap). The remaster snaps instantly; TRX won't allow
//     turn rates that weren't possible in the original.
//   - Returns true while modern controls are engaged so the legacy
//     classic turn doesn't also run.
// Lean is always zeroed — matches remaster behavior.
static bool M_SwimTurnModern(ITEM *const item)
{
    if (!g_Config.gameplay.enable_modern_controls) {
        return false;
    }

    // While modern is on, suppress classic turn handling regardless of
    // whether the stick is currently held — releasing the stick should
    // leave Lara coasting, not snap into classic turn-by-digital-input.
    item->rot.z = 0;

    if (g_AnalogInput.magnitude == 0) {
        return true;
    }

    const int32_t target_yaw32 = Lara_ModernGetTargetAngle();
    if (target_yaw32 == MODERN_ANGLE_NONE) {
        return true;
    }

    const int16_t target_yaw = (int16_t)target_yaw32;
    const int16_t yaw_delta = target_yaw - item->rot.y;
    if (yaw_delta > LARA_MED_TURN) {
        item->rot.y += LARA_MED_TURN;
    } else if (yaw_delta < -LARA_MED_TURN) {
        item->rot.y -= LARA_MED_TURN;
    } else {
        item->rot.y += yaw_delta;
    }

    // Pitch tracks camera elevation (right stick / mouse Y), same as
    // remaster's separate-analog-field pitch source.
    const int16_t target_pitch = g_Camera.additional_elevation;
    const int16_t pitch_delta = target_pitch - item->rot.x;
    if (pitch_delta > M_TURN_RATE) {
        item->rot.x += M_TURN_RATE;
    } else if (pitch_delta < -M_TURN_RATE) {
        item->rot.x -= M_TURN_RATE;
    } else {
        item->rot.x += pitch_delta;
    }

    return true;
}

static void M_SwimTurn(ITEM *const item)
{
    if (M_SwimTurnModern(item)) {
        return;
    }

    if (g_Input.forward) {
        item->rot.x -= M_TURN_RATE;
    } else if (g_Input.back) {
        item->rot.x += M_TURN_RATE;
    }

    if (g_Config.gameplay.enable_tr2_swimming) {
        if (!Lara_ModernTurn(LARA_TURN_RATE, LARA_MED_TURN)) {
            LARA_INFO *const lara = Lara_GetLaraInfo();
            if (g_Input.left) {
                lara->turn_rate -= LARA_TURN_RATE;
                CLAMPL(lara->turn_rate, -LARA_MED_TURN);
                item->rot.z -= M_LEAN_RATE;
            } else if (g_Input.right) {
                lara->turn_rate += LARA_TURN_RATE;
                CLAMPG(lara->turn_rate, LARA_MED_TURN);
                item->rot.z += M_LEAN_RATE;
            }
        }
    } else {
        if (!Lara_ModernTurn(LARA_TURN_RATE, LARA_MED_TURN)) {
            if (g_Input.left) {
                item->rot.y -= LARA_MED_TURN;
                item->rot.z -= M_LEAN_RATE;
            } else if (g_Input.right) {
                item->rot.y += LARA_MED_TURN;
                item->rot.z += M_LEAN_RATE;
            }
        }
    }
}

static bool M_StickActive(void)
{
    return g_Config.gameplay.enable_modern_controls
        && g_AnalogInput.magnitude > 0;
}

static void M_Tread(ITEM *const item, COLL_INFO *const coll)
{
    if (item->hit_points <= 0) {
        item->goal_anim_state = LS(LS_UW_DEATH);
        return;
    }

    coll->enable_hit = 0;

    if (g_Config.gameplay.enable_uw_roll && g_Input.roll) {
        item->current_anim_state = LS(LS_WATER_ROLL);
        Item_SwitchToAnim(item, LA(LA_UNDERWATER_ROLL_START), 0);
        return;
    }

    if (g_Config.gameplay.look_mode != LOOK_MODE_RESTRICTED && g_Input.look) {
        Lara_Look_UpDown();
    }

    // Modern controls: stick held → straight into LS_SWIM (the swim
    // handler does the rotation each frame, rate-capped). This matches
    // the remaster's tread behavior (stick = synthetic JUMP).
    M_SwimTurn(item);
    if (g_Input.jump || M_StickActive()) {
        item->goal_anim_state = LS(LS_SWIM);
    }
    item->fall_speed -= M_FRICTION;
    CLAMPL(item->fall_speed, 0);

    LARA_INFO *const lara = Lara_GetLaraInfo();
    if (lara->gun_status == LGS_HANDS_BUSY) {
        lara->gun_status = LGS_ARMLESS;
    }
}

static void M_Swim(ITEM *const item, COLL_INFO *const coll)
{
    if (item->hit_points <= 0) {
        item->goal_anim_state = LS(LS_UW_DEATH);
        return;
    }

    coll->enable_hit = 0;

    if (g_Config.gameplay.enable_uw_roll && g_Input.roll) {
        item->current_anim_state = LS(LS_WATER_ROLL);
        Item_SwitchToAnim(item, LA(LA_UNDERWATER_ROLL_START), 0);
        return;
    }

    M_SwimTurn(item);
    item->fall_speed += 8;
    LARA_INFO *const lara = Lara_GetLaraInfo();
    if (lara->water_status == LWS_CHEAT) {
        CLAMPG(item->fall_speed, M_MAX_SWIM_SPEED * 2);
    } else {
        CLAMPG(item->fall_speed, M_MAX_SWIM_SPEED);
    }

    if (!g_Input.jump && !M_StickActive()) {
        item->goal_anim_state =
            LS(g_Config.gameplay.enable_tr2_swim_cancel
                       && Lara_State_IsResponsive(LA_UNDERWATER_SWIM_FORWARD)
                   ? LS_RESPONSIVE
                   : LS_GLIDE);
    }
}

static void M_Glide(ITEM *item, COLL_INFO *coll)
{
    if (item->hit_points <= 0) {
        item->goal_anim_state = LS(LS_UW_DEATH);
        return;
    }

    coll->enable_hit = 0;

    if (g_Config.gameplay.enable_uw_roll && g_Input.roll) {
        item->current_anim_state = LS(LS_WATER_ROLL);
        Item_SwitchToAnim(item, LA(LA_UNDERWATER_ROLL_START), 0);
        return;
    }

    M_SwimTurn(item);
    if (g_Input.jump || M_StickActive()) {
        item->goal_anim_state = LS(LS_SWIM);
    }
    item->fall_speed -= M_FRICTION;
    CLAMPL(item->fall_speed, 0);
    if (item->fall_speed <= M_MAX_SWIM_SPEED * 2 / 3) {
        item->goal_anim_state = LS(LS_TREAD);
    }
}

static void M_TreadSurface(ITEM *const item, COLL_INFO *const coll)
{
    item->fall_speed -= 4;
    CLAMPL(item->fall_speed, 0);

    if (item->hit_points <= 0) {
        item->goal_anim_state = LS(LS_UW_DEATH);
        return;
    }

    coll->enable_hit = 0;

    if (g_Input.look) {
        Lara_Look_UpDown();
        return;
    }

    const int32_t surf_target = Lara_ModernGetTargetAngle();
    if (surf_target != MODERN_ANGLE_NONE) {
        const int16_t surf_delta = (int16_t)surf_target - item->rot.y;
        if (surf_delta > LARA_SLOW_TURN) {
            item->rot.y += LARA_SLOW_TURN;
        } else if (surf_delta < -LARA_SLOW_TURN) {
            item->rot.y -= LARA_SLOW_TURN;
        } else {
            item->rot.y += surf_delta;
        }
        // Camera-relative: any stick input means swim forward
        item->goal_anim_state = LS(LS_SURF_SWIM);
    } else {
        if (g_Input.left) {
            item->rot.y -= LARA_SLOW_TURN;
        } else if (g_Input.right) {
            item->rot.y += LARA_SLOW_TURN;
        }

        if (g_Input.forward) {
            item->goal_anim_state = LS(LS_SURF_SWIM);
        } else if (g_Input.back) {
            item->goal_anim_state = LS(LS_SURF_BACK);
        }

        if (g_Input.step_left) {
            item->goal_anim_state = LS(LS_SURF_LEFT);
        } else if (g_Input.step_right) {
            item->goal_anim_state = LS(LS_SURF_RIGHT);
        }
    }

    LARA_INFO *const lara = Lara_GetLaraInfo();
    if (g_Input.jump) {
        lara->dive_timer++;
        if (lara->dive_timer == LARA_DIVE_WAIT) {
            Item_SwitchToAnim(item, LA(LA_ONWATER_DIVE), 0);
            item->goal_anim_state = LS(LS_SWIM);
            item->current_anim_state = LS(LS_DIVE);
            item->rot.x = -45 * DEG_1;
            item->fall_speed = 80;
            lara->water_status = LWS_UNDERWATER;
        }
    } else {
        lara->dive_timer = 0;
    }
}

static void M_ForwardSurface(ITEM *const item, COLL_INFO *const coll)
{
    if (item->hit_points <= 0) {
        item->goal_anim_state = LS(LS_UW_DEATH);
        return;
    }

    coll->enable_hit = 0;

    LARA_INFO *const lara = Lara_GetLaraInfo();
    lara->dive_timer = 0;
    {
        const int32_t fwd_target = Lara_ModernGetTargetAngle();
        if (fwd_target != MODERN_ANGLE_NONE) {
            const int16_t fwd_delta = (int16_t)fwd_target - item->rot.y;
            if (fwd_delta > LARA_SLOW_TURN) {
                item->rot.y += LARA_SLOW_TURN;
            } else if (fwd_delta < -LARA_SLOW_TURN) {
                item->rot.y -= LARA_SLOW_TURN;
            } else {
                item->rot.y += fwd_delta;
            }
            // Camera-relative: keep swimming while stick is active
            if (g_Input.jump) {
                item->goal_anim_state = LS(LS_SURF_TREAD);
            }
        } else {
            if (g_Input.left) {
                item->rot.y -= LARA_SLOW_TURN;
            } else if (g_Input.right) {
                item->rot.y += LARA_SLOW_TURN;
            }
            if (!g_Input.forward || g_Input.jump) {
                item->goal_anim_state = LS(LS_SURF_TREAD);
            }
        }
    }
    item->fall_speed += 8;
    CLAMPG(item->fall_speed, M_MAX_SURF_SPEED);
}

static void M_SideBackSurface(ITEM *const item, COLL_INFO *const coll)
{
    if (item->hit_points <= 0) {
        item->goal_anim_state = LS(LS_UW_DEATH);
        return;
    }

    coll->enable_hit = 0;

    LARA_INFO *const lara = Lara_GetLaraInfo();
    lara->dive_timer = 0;

    {
        const int32_t sb_target = Lara_ModernGetTargetAngle();
        if (sb_target != MODERN_ANGLE_NONE) {
            const int16_t sb_delta = (int16_t)sb_target - item->rot.y;
            if (sb_delta > M_TURN_RATE) {
                item->rot.y += M_TURN_RATE;
            } else if (sb_delta < -M_TURN_RATE) {
                item->rot.y -= M_TURN_RATE;
            } else {
                item->rot.y += sb_delta;
            }
            // Camera-relative: exit to tread (which will start forward swim)
            item->goal_anim_state = LS(LS_SURF_TREAD);
        } else {
            if (g_Input.left) {
                item->rot.y -= M_TURN_RATE;
            } else if (g_Input.right) {
                item->rot.y += M_TURN_RATE;
            }

            bool stop = false;
            switch (LS_U(item->current_anim_state)) {
            case LS_SURF_BACK:
                stop = !g_Input.back;
                break;
            case LS_SURF_LEFT:
                stop = !g_Input.step_left;
                break;
            case LS_SURF_RIGHT:
                stop = !g_Input.step_right;
                break;
            default:
                break;
            }

            if (stop) {
                item->goal_anim_state = LS(LS_SURF_TREAD);
            }
        }
    }

    item->fall_speed += 8;
    CLAMPG(item->fall_speed, M_MAX_SURF_SPEED);
}

static void M_Dive(ITEM *const item, COLL_INFO *const coll)
{
    if (g_Input.forward) {
        item->rot.x -= DEG_1;
    }
}

static void M_UWDeath(ITEM *const item, COLL_INFO *const coll)
{
    coll->enable_hit = 0;
    item->gravity = false;
    item->fall_speed -= 8;
    CLAMPL(item->fall_speed, 0);

    if (item->rot.x >= -M_TURN_RATE && item->rot.x <= M_TURN_RATE) {
        item->rot.x = 0;
    } else if (item->rot.x >= 0) {
        item->rot.x -= M_TURN_RATE;
    } else {
        item->rot.x += M_TURN_RATE;
    }
}

static void M_WaterOut(ITEM *const item, COLL_INFO *const coll)
{
    coll->enable_hit = 0;
    coll->enable_baddie_push = 0;
    if (g_Config.gameplay.enable_modern_controls) {
        // The chase camera's vertical offset is target_distance *
        // sin(target_elevation), and target_elevation comes from
        // additional_elevation (right stick / mouse Y). Persistent
        // pitch from underwater swim places the camera below Lara
        // post-climbout — sometimes under the floor. Decay it toward
        // level so the camera frames her normally on dry land. Use
        // about 1/3 per frame so the level-out completes inside the
        // ~1s climbout anim without an obvious pop.
        g_Camera.additional_elevation -= g_Camera.additional_elevation / 3;
    } else {
        g_Camera.flags = CF_FOLLOW_CENTRE;
    }
}

static void M_UWTwist(ITEM *const item, COLL_INFO *const coll)
{
    item->fall_speed = 0;
    item->goal_anim_state = LS(LS_TREAD);
}

// clang-format off
REGISTER_LARA_STATE(LS_TREAD,      M_Tread)
REGISTER_LARA_STATE(LS_SWIM,       M_Swim)
REGISTER_LARA_STATE(LS_GLIDE,      M_Glide)
REGISTER_LARA_STATE(LS_SURF_TREAD, M_TreadSurface)
REGISTER_LARA_STATE(LS_SURF_SWIM,  M_ForwardSurface)
REGISTER_LARA_STATE(LS_DIVE,       M_Dive)
REGISTER_LARA_STATE(LS_UW_DEATH,   M_UWDeath)
REGISTER_LARA_STATE(LS_SURF_BACK,  M_SideBackSurface)
REGISTER_LARA_STATE(LS_SURF_LEFT,  M_SideBackSurface)
REGISTER_LARA_STATE(LS_SURF_RIGHT, M_SideBackSurface)
REGISTER_LARA_STATE(LS_WATER_OUT,  M_WaterOut)
REGISTER_LARA_STATE(LS_WATER_ROLL, M_UWTwist)
// clang-format on
