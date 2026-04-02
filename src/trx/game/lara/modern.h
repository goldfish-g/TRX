#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MODERN_ANGLE_NONE 0x7FFFFFFF
#define MODERN_FORWARD_ZONE (182 * 60) // ±60° in engine angle units
#define MODERN_STRAIGHT_ZONE (182 * 10) // ±10° dead zone for running straight
#define MODERN_BACK_ZONE (182 * 160)   // ±160° — backstep only within 20° of back

// Returns camera-relative world angle from stick, or MODERN_ANGLE_NONE.
int32_t Lara_ModernGetTargetAngle(void);

// Turn toward target using classic turn_rate accumulation.
// Returns true if modern controls handled the turn.
bool Lara_ModernTurn(int16_t rate, int16_t max_turn);

// Turn toward target with lean (z_rot) applied.
// Returns true if modern controls handled the turn.
bool Lara_ModernTurnWithLean(
    int16_t rate, int16_t max_turn, int16_t lean_rate, int16_t lean_max);

// Remap g_Input directional flags based on analog stick zones.
// Must be called after g_Input is populated and before state handlers run.
void Lara_ModernRemapInput(void);
