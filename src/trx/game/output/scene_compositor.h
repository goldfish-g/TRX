#pragma once

#include <trx/core/colors.h>
#include <trx/game/output/scene_source.h>
#include <trx/gl/enum.h>

void SceneCompositor_Init(void);
void SceneCompositor_Shutdown(void);
void SceneCompositor_AddSource(const SCENE_SOURCE *source);
void SceneCompositor_BeginScene(void);
void SceneCompositor_EndScene(void);
void SceneCompositor_Flush(void);
// Flushes what is so far away that both eyes of a VR headset see it in the
// same place, such as the sky. Drawn from where each eye is, it would look
// as near as it is modeled.
void SceneCompositor_FlushDistant(void);

// Brackets the drawing of the world, as opposed to the interface and the
// models it shows. Between the two, a VR headset that shows the frame gets
// the world drawn once for each eye, and SceneCompositor_IsStereo says so,
// which is what the camera asks to put itself on the player's head.
void SceneCompositor_BeginWorld(void);
void SceneCompositor_EndWorld(void);
bool SceneCompositor_IsStereo(void);
// How many pixels the interface is moved sideways for the eye being drawn,
// to bring its text forward of the panel it is on in VR. The models of the
// interface undo it for themselves, having depth of their own.
int32_t SceneCompositor_GetPanelTextShift(void);
// Makes the world that is drawn next for an eye of a VR headset lose its
// colors by the given amount and take the given tint, as it does behind a
// menu. Holds for that one drawing.
void SceneCompositor_SetWorldTint(float amount, RGB_F tint);
// Applies that tint to the scene as it stands in its framebuffer, where the
// world was drawn for a screen and not for an eye.
void SceneCompositor_TintScene(void);
// Makes the world an eye of a VR headset is shown fade towards black, as far
// as the strongest call in the frame asks. In place of a black sheet over a
// screen, which around the player would be a black board in the air.
void SceneCompositor_AddWorldFade(float opacity);
// Ends an eye's part of a frame: does to the eye's picture what the frame
// asked for, and hands it back to the headset.
void SceneCompositor_FinishEye(void);

// Renders the scene even where the frame would otherwise be skipped, so a
// capture reads back real content instead of an empty frame. A headless run
// draws nothing on its own.
void SceneCompositor_BeginCapture(void);
void SceneCompositor_EndCapture(void);

void SceneCompositor_AnimateTextures(void);
void SceneCompositor_SetSamplerFilter(TEXTURE_FILTER filter);
// Binds the texture atlas, the environment map and the sampler the sources
// draw with.
void SceneCompositor_BindTextures(void);
// Sets the blend state that the subtractive pass draws with. PlayStation
// blending subtracts the source color from the scene; otherwise the scene is
// multiplied by the inverse of the source color, as on the PC.
void SceneCompositor_SetSubtractiveBlend(void);
