#include <trx/game/input/backends/base.h>

#include <SDL2/SDL_gamecontroller.h>
#include <stdint.h>

extern INPUT_BACKEND_IMPL g_Input_Controller;

// Returns the raw SDL axis value (-32768..32767) for the given axis.
// Returns 0 if no controller is connected.
int16_t Input_Controller_GetRawAxis(SDL_GameControllerAxis axis);
void Input_Controller_SetScheme(bool modern);
