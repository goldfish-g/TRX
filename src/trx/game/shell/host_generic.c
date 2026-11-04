#include <trx/game/shell/host.h>

void Shell_Host_SetupSDL(void)
{
}

uint32_t Shell_Host_GetWindowFlags(void)
{
    return 0;
}

SHELL_SIZE Shell_Host_GetWindowSize(SDL_Window *const window)
{
    SHELL_SIZE result;
    SDL_GetWindowSize(window, &result.w, &result.h);
    return result;
}

bool Shell_Host_OwnsWindow(void)
{
    return true;
}

void Shell_Host_OpenStorage(const SHELL_MOD *const mod)
{
}

void Shell_Host_RestoreFile(const char *const path)
{
}

void Shell_Host_KeepFile(const char *const path)
{
}

void Shell_Host_WaitForStart(void)
{
}
