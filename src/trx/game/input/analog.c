#include <trx/game/input/analog.h>

#include <trx/config.h>
#include <trx/core/math/func.h>
#include <trx/core/math/trig.h>
#include <trx/game/input/backends/controller.h>
#include <trx/game/input/common.h>

#include <SDL2/SDL_gamecontroller.h>

#define M_DEAD_ZONE 4915 // ~15% of 32767

ANALOG_INPUT g_AnalogInput = {};
ANALOG_CAM_INPUT g_AnalogCamInput = {};

void Analog_Update(void)
{
    const int16_t raw_x =
        Input_Controller_GetRawAxis(SDL_CONTROLLER_AXIS_LEFTX);
    const int16_t raw_y =
        Input_Controller_GetRawAxis(SDL_CONTROLLER_AXIS_LEFTY);

    // Circular dead zone: check magnitude of raw values
    const int32_t raw_mag_sq = (int32_t)raw_x * raw_x + (int32_t)raw_y * raw_y;
    const int32_t dead_sq = (int32_t)M_DEAD_ZONE * M_DEAD_ZONE;

    if (raw_mag_sq >= dead_sq) {
        // Controller stick active — use it
        g_AnalogInput.stick_x = raw_x / 256;
        g_AnalogInput.stick_y = raw_y / 256;
        const uint32_t raw_mag = Math_Sqrt((uint32_t)raw_mag_sq);
        g_AnalogInput.magnitude = (int16_t)(raw_mag * 256 / 32767);
        if (g_AnalogInput.magnitude > 256) {
            g_AnalogInput.magnitude = 256;
        }
        g_AnalogInput.direction =
            (int16_t)Math_Atan(-g_AnalogInput.stick_y, g_AnalogInput.stick_x);
        return;
    }

    // No controller stick input — synthesize from keyboard/D-PAD digital
    // inputs when modern controls are active. This lets WASD and D-PAD
    // produce camera-relative movement via the analog system.
    if (g_Config.gameplay.enable_modern_controls) {
        int16_t kb_x = 0;
        int16_t kb_y = 0;
        if (g_Input.forward) {
            kb_y = -128;
        }
        if (g_Input.back) {
            kb_y = 128;
        }
        if (g_Input.left) {
            kb_x = -128;
        }
        if (g_Input.right) {
            kb_x = 128;
        }

        if (kb_x != 0 || kb_y != 0) {
            g_AnalogInput.stick_x = kb_x;
            g_AnalogInput.stick_y = kb_y;
            g_AnalogInput.magnitude = 256;
            g_AnalogInput.direction =
                (int16_t)Math_Atan(-kb_y, kb_x);
            return;
        }
    }

    g_AnalogInput.stick_x = 0;
    g_AnalogInput.stick_y = 0;
    g_AnalogInput.magnitude = 0;
    g_AnalogInput.direction = 0;
}

void Analog_UpdateCamera(void)
{
    const int16_t raw_x =
        Input_Controller_GetRawAxis(SDL_CONTROLLER_AXIS_RIGHTX);
    const int16_t raw_y =
        Input_Controller_GetRawAxis(SDL_CONTROLLER_AXIS_RIGHTY);

    // Circular dead zone (same threshold as left stick)
    const int32_t raw_mag_sq =
        (int32_t)raw_x * raw_x + (int32_t)raw_y * raw_y;
    const int32_t dead_sq = (int32_t)M_DEAD_ZONE * M_DEAD_ZONE;

    if (raw_mag_sq < dead_sq) {
        g_AnalogCamInput.stick_x = 0;
        g_AnalogCamInput.stick_y = 0;
        return;
    }

    g_AnalogCamInput.stick_x = raw_x / 256;
    g_AnalogCamInput.stick_y = raw_y / 256;
}
