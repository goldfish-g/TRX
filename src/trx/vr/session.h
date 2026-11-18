#pragma once

#include <trx/core/result.h>

#include <stdint.h>

// The headset, reached through OpenXR. A build without VR support carries
// the same functions, and they report that there is no headset.
//
// The game is shown on a screen that hangs in front of the player: each
// frame is drawn into the framebuffer that VR_Session_BeginFrame hands out.

// Where a frame for the screen in the headset is drawn.
typedef struct {
    // The OpenGL name of the framebuffer.
    uint32_t fbo;
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

// Where one eye is and how far it sees to each side. The offset is from the
// head, in meters and in the head's own axes: right, up and backwards. The
// tangents are those of the angles from straight ahead, negative to the left
// and downwards.
typedef struct {
    float offset[3];
    float tan_left;
    float tan_right;
    float tan_up;
    float tan_down;
} VR_EYE;

// Where the head is when the frame is shown, in meters from where the player
// last recentered, and the rotation that turns the head's axes into that
// space's: right, up and backwards.
typedef struct {
    float pos[3];
    float rot[3][3];
    VR_EYE eyes[2];
} VR_VIEW;

// How the game is shown in the headset.
typedef enum {
    // On a screen in front of the player, with depth: the game's own camera,
    // seen once for each eye.
    VR_MODE_THEATER,
    // Around the player, with the camera on the player's head.
    VR_MODE_INVOLVED,
} VR_MODE;

VR_MODE VR_Session_GetMode(void);
void VR_Session_ToggleMode(void);

// Whether each frame is to be drawn twice, once for each eye. In the
// theater both go onto the screen; with the player inside the game, the
// world goes around the player and the interface onto the screen.
bool VR_Session_IsStereo(void);

// Which eye the frame is being drawn for: 0 for the left, 1 for the right,
// and -1 where the frame is drawn once for both.
void VR_Session_SetDrawnEye(int32_t eye);
int32_t VR_Session_GetDrawnEye(void);

// How far apart the two pictures on the screen put what is infinitely far
// away, as a share of half the screen's width. Nothing is put further apart
// than that, so nothing looks further away than a flat picture that is put
// there, which is where the backgrounds of the menus go.
#define VR_SCREEN_PARALLAX 0.02f
// How far from the camera, in the game's units, something has to be to sit
// in the plane of the screen, where the text of the interface is. What is
// nearer than that stands out of the screen, in front of the text, so these
// are kept short: one for the world and one for the models of the
// interface, which come close to their camera.
#define VR_WORLD_CONVERGENCE 640.0f
#define VR_UI_CONVERGENCE 128.0f
// The same two for the models of the interface while the level is around
// the player. The panel they are on hangs in the level, in front of Lara,
// so their depth is kept around its plane: not as far forward as the text
// over them, and not as far back as she is.
#define VR_PANEL_PARALLAX 0.012f
#define VR_PANEL_CONVERGENCE 400.0f
// How far the text of the panel is brought forward of its plane, in the same
// measure, which leaves the models room to come forward behind it.
#define VR_PANEL_TEXT_PARALLAX 0.008f

// How many of the game's units a meter of the player's room is.
#define VR_UNITS_PER_METER 512.0f

// Starts a frame for the headset, waiting until the headset wants one.
// Returns false where the headset takes no frames at the moment, and then
// there is nothing to end. Starting a frame that is already open does
// nothing.
bool VR_Session_BeginFrame(void);

// Where the head and the eyes are in the frame that is open, or nullptr
// where the headset does not know or will not show the frame.
const VR_VIEW *VR_Session_GetView(void);

// The parts of one eye's picture that its lens never shows, as triangles:
// two numbers to a corner, three corners to a triangle, with the picture
// reaching from -1 to 1 either way and counting upwards. Returns how many
// corners there are, which is none where the headset does not say.
int32_t VR_Session_GetHiddenArea(int32_t eye, const float **out_corners);

// Hands out the framebuffer one eye's view of the scene is drawn into, with a
// depth buffer of its own, and takes it back once drawn. Returns false where
// the headset will not show the frame.
bool VR_Session_AcquireEye(int32_t eye, VR_SCREEN *out_screen);
void VR_Session_ReleaseEye(int32_t eye);

// Whether the given eye's view of the scene was drawn in the frame that is
// open.
bool VR_Session_IsEyeDrawn(int32_t eye);

// Whether both eyes were drawn in the frame that is open. The screen then
// hangs over the scene and shows only what is drawn into it, clear where
// nothing is.
bool VR_Session_HasEyes(void);

// Hands out the framebuffer the screen is drawn into, for the left eye (0)
// or the right (1). A frame that only draws the left one shows it to both
// eyes; one that draws both shows each eye its own, and the screen then has
// depth the way a 3D film has.
bool VR_Session_AcquireScreen(int32_t eye, VR_SCREEN *out_screen);

// Hands the frame to the headset, with the eyes and screens that were drawn.
// The aspect ratio is the one of the picture on the screen, which the screen
// takes its shape from.
void VR_Session_EndFrame(float aspect_ratio);
