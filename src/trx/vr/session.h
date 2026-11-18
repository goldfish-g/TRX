#pragma once

#include <trx/core/result.h>
#include <trx/gl/api.h>

#include <stdint.h>

// The headset, reached through OpenXR. A build without VR support carries
// the same functions, and they report that there is no headset.
//
// The game is shown on a screen that hangs in front of the player: each
// frame is drawn into the framebuffer that VR_Session_BeginFrame hands out.

// Where a frame for the screen in the headset is drawn.
typedef struct {
    GLuint fbo;
    int32_t width;
    int32_t height;
} VR_SCREEN;

// Tells SDL what the headset needs of it. Called before SDL is brought up.
void VR_Session_PrepareSDL(void);

// Opens a session on the OpenGL context that is current, which belongs to
// the given SDL window.
RESULT VR_Session_Start(void *window);
void VR_Session_Stop(void);

// The size of the screen in the headset, in pixels, which is the size the
// game is best drawn at while a session is open.
void VR_Session_GetScreenSize(int32_t *out_width, int32_t *out_height);

// Whether a session is open. The headset paces the frames while one is, so
// the window must not wait for the display.
bool VR_Session_IsActive(void);

// Whether the headset takes frames at the moment, and so holds each one
// back until it is due.
bool VR_Session_PacesFrames(void);

// Whether the player took the headset off. A headset that cannot tell says
// that the player is there.
bool VR_Session_IsPlayerAway(void);

// Moves the screen to where the player is looking, the next time the headset
// knows where that is. A session does this by itself when it opens.
void VR_Session_Recenter(void);

// Starts a frame for the headset, waiting until the headset wants one.
// Returns false where the headset takes no frames at the moment, and then
// there is nothing to end. A frame the headset will not show has no
// framebuffer to draw into, and still has to be ended.
bool VR_Session_BeginFrame(VR_SCREEN *out_screen);

// Hands the frame to the headset. The aspect ratio is the one of the picture
// that was drawn, which the screen takes its shape from.
void VR_Session_EndFrame(float aspect_ratio);
