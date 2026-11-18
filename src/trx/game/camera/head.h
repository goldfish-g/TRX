#pragma once

#include <trx/vr/session.h>

#include <stdint.h>

// The camera of a player in VR.

// Puts the camera on the head of a player who is inside the game, following
// Lara by rules of its own. Sets the view the way Camera_Apply does for the
// game's own camera.
void Camera_ApplyHead(const VR_VIEW *head);

// Gives the view of the game's own camera its exact form, for a headset
// that shows it on a screen. Called once the view is set.
void Camera_RefineView(void);

// The room the player's head is in while the camera is on it, which is
// where finding what is visible starts from.
int16_t Camera_GetHeadRoom(void);

// Whether the camera is on the head of a player in VR at the moment, and
// not where the game's own camera is.
bool Camera_IsOnHead(void);
