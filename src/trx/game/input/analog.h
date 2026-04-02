#pragma once

#include <stdint.h>

typedef struct {
    int16_t stick_x;   // Left stick X: -128..+127
    int16_t stick_y;   // Left stick Y: -128..+127
    int16_t magnitude; // 0..256 fixed-point (256 = full deflection)
    int16_t direction; // Angle 0..65535 (stick direction in screen space)
} ANALOG_INPUT;

extern ANALOG_INPUT g_AnalogInput;

typedef struct {
    int16_t stick_x; // Right stick X: -128..+127
    int16_t stick_y; // Right stick Y: -128..+127
} ANALOG_CAM_INPUT;

extern ANALOG_CAM_INPUT g_AnalogCamInput;

void Analog_Update(void);
void Analog_UpdateCamera(void);
