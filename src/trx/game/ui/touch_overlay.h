#pragma once

#include <SDL2/SDL_events.h>
#include <stdbool.h>

void TouchOverlay_Init(void);
void TouchOverlay_Shutdown(void);

void TouchOverlay_SetVisible(bool visible);
bool TouchOverlay_IsVisible(void);

void TouchOverlay_Draw(void);

// Returns true if the event was consumed by the touch overlay.
bool TouchOverlay_ProcessEvent(const SDL_Event *event);
