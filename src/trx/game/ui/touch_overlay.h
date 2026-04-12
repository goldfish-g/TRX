#pragma once

#include <trx/game/input/common.h>

#include <SDL2/SDL_events.h>
#include <stdbool.h>
#include <stdint.h>

void TouchOverlay_Init(void);
void TouchOverlay_Shutdown(void);

void TouchOverlay_SetVisible(bool visible);
bool TouchOverlay_IsVisible(void);

void TouchOverlay_Draw(void);

// Returns true if the event was consumed by the touch overlay.
bool TouchOverlay_ProcessEvent(const SDL_Event *event);

// Returns true if any finger is currently touching the screen.
bool TouchOverlay_HasAnyFingerDown(void);

// --- Touch position system for remapping ---
// Positions identify assignable touch locations:
//   0-3: D-pad directions (UP, DOWN, LEFT, RIGHT)
//   4+:  Regular buttons (mapped from button def indices)

#define TOUCH_POS_DPAD_UP    0
#define TOUCH_POS_DPAD_DOWN  1
#define TOUCH_POS_DPAD_LEFT  2
#define TOUCH_POS_DPAD_RIGHT 3
#define TOUCH_POS_BUTTON_BASE 4

int32_t TouchOverlay_GetPositionCount(void);
uint8_t TouchOverlay_GetPositionEngineMask(int32_t position);
INPUT_ROLE TouchOverlay_GetPositionDefaultRole(int32_t position);
const char *TouchOverlay_GetPositionName(int32_t position);

// Selection mode for touch remap listen phase.
// In selection mode, finger-down events record which position was tapped
// instead of triggering input actions.
void TouchOverlay_EnterSelectionMode(void);
void TouchOverlay_ExitSelectionMode(void);
int32_t TouchOverlay_GetSelectedPosition(void); // returns -1 if none selected
