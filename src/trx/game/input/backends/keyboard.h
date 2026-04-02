#include <trx/game/input/backends/base.h>

#include <stdint.h>

extern INPUT_BACKEND_IMPL g_Input_Keyboard;

void Input_Keyboard_GetMouseDelta(int32_t *dx, int32_t *dy);
void Input_Keyboard_SetScheme(bool modern);
