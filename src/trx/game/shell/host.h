#pragma once

#include <trx/game/shell/common.h>
#include <trx/game/shell/mod.h>

#include <SDL2/SDL_video.h>
#include <stdint.h>

// What the game asks of the environment that hosts it. On the desktop the game
// owns its window and its files outright, and these mostly do nothing. In the
// browser it shares both with the page, which answers through the adapter
// contract in tools/shared/emscripten/trx-adapter.d.ts.

// Called before SDL starts, to give it the hints the host needs.
void Shell_Host_SetupSDL(void);

// Extra flags for the game window.
uint32_t Shell_Host_GetWindowFlags(void);

// The size of the window in pixels.
SHELL_SIZE Shell_Host_GetWindowSize(SDL_Window *window);

// Whether the game sizes and places its own window. In the browser the page
// lays the canvas out, and the game follows.
bool Shell_Host_OwnsWindow(void);

// Opens the storage the host keeps between sessions for the given game, and
// brings its files back.
void Shell_Host_OpenStorage(const SHELL_MOD *mod);

// Brings back a file the host kept from an earlier session, before the game
// reads it.
void Shell_Host_RestoreFile(const char *path);

// Has the host keep a file the game has just written, for later sessions.
void Shell_Host_KeepFile(const char *path);

// Holds the game until the host lets it start, such as on the first click
// that lets a browser play sound.
void Shell_Host_WaitForStart(void);
