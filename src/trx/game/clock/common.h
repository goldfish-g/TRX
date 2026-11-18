#pragma once

#include <stddef.h>
#include <stdint.h>

// Holds the game for the given number of milliseconds. Where the platform
// runs the game in turns with others, as the browser does, it also gives them
// their turn, even when asked to hold for none.
void Clock_Delay(int32_t ms);

// Disables any kind of waiting in Clock_WaitTick
void Clock_DisableWait(void);

// Restores the waiting Clock_DisableWait turned off.
void Clock_EnableWait(void);

// Reports whether frames are paced to real time. Frames run as fast as possible
// when pacing is disabled.
bool Clock_IsRealTime(void);

// Counts time in frames: every Clock_WaitTick moves the clock on by 1/fps. Zero
// goes back to real time, carrying on from where the frame count reached.
void Clock_EnableFixedFPS(int32_t fps);

void Clock_SyncTick(void);
int32_t Clock_WaitTick(void);

// Counts the logic ticks that came due since the last call, without waiting
// for one. For a caller whose frames are paced by something else, such as a
// VR headset, and who draws as often as that allows. The fraction is how far
// the time has moved from the last tick towards the next one.
int32_t Clock_TakeTicks(double *out_fraction);

size_t Clock_GetDateTime(char *buffer, size_t size);

int32_t Clock_GetFrameAdvance(void);
int32_t Clock_GetCurrentFPS(void);

void Clock_SetSimSpeed(double new_speed);
double Clock_GetRealTime(void);
double Clock_GetSimTime(void);
