#include <trx/game/input/backends/touch.h>

#include <trx/config.h>
#include <trx/config/common.h>
#include <trx/game/input/common.h>
#include <trx/game/ui/touch_overlay.h>

#include <SDL2/SDL_events.h>
#include <stdbool.h>
#include <stdint.h>

static bool m_State[INPUT_ROLE_NUMBER_OF] = {};

void Touch_SetState(const INPUT_ROLE role, const bool pressed)
{
    if (role >= 0 && role < INPUT_ROLE_NUMBER_OF) {
        m_State[role] = pressed;
    }
}

static void M_Init(void)
{
    TouchOverlay_Init();
}

static void M_Shutdown(void)
{
    TouchOverlay_Shutdown();
}

static void M_ProcessEvent(const SDL_Event *const event)
{
    if (event->type == SDL_FINGERDOWN
        && !g_Config.input.enable_touch_controls) {
        g_Config.input.enable_touch_controls = true;
        TouchOverlay_SetVisible(true);
        Config_Write();
    }
    TouchOverlay_ProcessEvent(event);
}

static bool M_IsPressed(const INPUT_LAYOUT layout, const INPUT_ROLE role)
{
    (void)layout;
    if (role >= 0 && role < INPUT_ROLE_NUMBER_OF) {
        return m_State[role];
    }
    return false;
}

static bool M_CustomUpdate(INPUT_STATE *const result, const INPUT_LAYOUT layout)
{
    (void)layout;
    result->menu_confirm |= result->action;
    result->menu_back |= result->jump;
    result->menu_skip = result->menu_confirm || result->menu_back;
    return true;
}

INPUT_BACKEND_IMPL g_Input_Touch = {
    .init = M_Init,
    .shutdown = M_Shutdown,
    .discover = nullptr,
    .custom_update = M_CustomUpdate,
    .process_event = M_ProcessEvent,
    .is_pressed = M_IsPressed,
    .is_role_conflicted = nullptr,
    .get_name = nullptr,
    .unassign_role = nullptr,
    .assign_from_json_object = nullptr,
    .assign_to_json_object = nullptr,
    .reset_layout = nullptr,
    .read_and_assign = nullptr,
    .resolve_combos = nullptr,
};
