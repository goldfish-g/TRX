#include <trx/game/shell.h>

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

void Shell_LoadModGameData(const char *const mod_name)
{
    (void)mod_name;
}

void Shell_InitIDBFS(void)
{
}

void Shell_WaitForUserInput(void)
{
}

void Shell_ShowProfileSelector(
    char *const mod_buf, const int32_t mod_buf_size, int32_t *const engine_out)
{
    (void)mod_buf;
    (void)mod_buf_size;
    (void)engine_out;
}

void Shell_PersistConfigToIDBFS(void)
{
}

void Shell_PersistSavesToIDBFS(void)
{
}

bool Shell_HasTouchSupport(void)
{
    return SDL_GetNumTouchDevices() > 0;
}

uint32_t Shell_GetWindowExtraFlags(void)
{
    return 0;
}

void Shell_PostSDLInit(void)
{
}

void Shell_SetupGLContextVersion(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
}
