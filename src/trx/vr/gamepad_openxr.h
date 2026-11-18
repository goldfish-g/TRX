#pragma once

#include <trx/core/result.h>

#include <openxr/openxr.h>

// The controllers of the headset, offered to the game as a gamepad. The game
// reads a gamepad already, with bindings the player can change, so the
// buttons and sticks of the two controllers are laid out as the ones of a
// gamepad and attached as a device of SDL.

// Declares the buttons and sticks to the runtime. A session takes them once,
// before its first frame.
RESULT VR_Gamepad_Init(XrInstance instance, XrSession session);
void VR_Gamepad_Shutdown(void);

// Reads the controllers and moves the gamepad along. The runtime only says
// what the controllers do while the game has the player's attention.
void VR_Gamepad_Update(XrSession session);
