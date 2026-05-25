// setenv() is POSIX, not C standard — request the POSIX namespace
// before any libc headers are pulled in transitively.
#ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
#endif

#include <trx/core/webgl_log.h>
#include <trx/game/clock.h>
#include <trx/game/shell.h>
#include <trx/version.h>

#include <SDL2/SDL.h>
#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Bridge pattern:
//
// The JS adapter API is fully promise-based (see trx-adapter.d.ts).
// Internally, each C-side blocking operation kicks off its adapter
// method, parks a `done` flag on a per-call Module._trxState.* slot,
// and polls that flag from C via Clock_Delay. This approach uses
// Asyncify's well-tested frame-timing yield path rather than
// EM_ASYNC_JS's rewind machinery, which is fragile in the presence of
// our function-pointer-heavy game-flow dispatch.
//
// The Module._trxState object exists only inside the wasm<->JS bridge.
// Frontends never see it.

// clang-format off
EM_JS(void, js_start_gate, (void), {
    Module._trxState = Module._trxState || {};
    Module._trxState.startGateDone = false;
    Module.trxAdapters.startGate().then(function() {
        Module._trxState.startGateDone = true;
    });
})

EM_JS(int, js_is_start_gate_done, (void), {
    return Module._trxState && Module._trxState.startGateDone ? 1 : 0;
})

EM_JS(void, js_mount_persistence, (const char *mount_path), {
    Module._trxState = Module._trxState || {};
    Module._trxState.mountDone = false;
    Module.trxAdapters.mountPersistence(UTF8ToString(mount_path)).then(function() {
        Module._trxState.mountDone = true;
    });
})

EM_JS(int, js_is_mount_done, (void), {
    return Module._trxState && Module._trxState.mountDone ? 1 : 0;
})

EM_JS(void, js_sync_persistence_in, (void), {
    Module._trxState = Module._trxState || {};
    Module._trxState.syncInDone = false;
    Module.trxAdapters.syncPersistenceIn().then(function() {
        Module._trxState.syncInDone = true;
    });
})

EM_JS(int, js_is_sync_in_done, (void), {
    return Module._trxState && Module._trxState.syncInDone ? 1 : 0;
})

EM_JS(void, js_sync_persistence_out, (const char *src, const char *dst), {
    Module.trxAdapters.syncPersistenceOut(
        UTF8ToString(src), UTF8ToString(dst));
})

EM_JS(void, js_flush_persistence, (void), {
    Module.trxAdapters.flushPersistence();
})

EM_JS(void, js_load_mod_data, (const char *mod_name), {
    Module._trxState = Module._trxState || {};
    Module._trxState.loadModDone = false;
    Module.trxAdapters.loadModData(UTF8ToString(mod_name)).then(function() {
        Module._trxState.loadModDone = true;
    });
})

EM_JS(int, js_is_load_mod_done, (void), {
    return Module._trxState && Module._trxState.loadModDone ? 1 : 0;
})

EM_JS(void, js_select_profile, (void), {
    Module._trxState = Module._trxState || {};
    Module._trxState.selectDone = false;
    Module._trxState.selectedMod = "";
    Module._trxState.selectedEngine = 0;
    Module.trxAdapters.selectProfile().then(function(result) {
        Module._trxState.selectedMod =
            (result && typeof result.mod === 'string') ? result.mod : "";
        Module._trxState.selectedEngine =
            (result && Number.isInteger(result.engine)) ? result.engine : 0;
        Module._trxState.selectDone = true;
    });
})

EM_JS(int, js_is_select_done, (void), {
    return Module._trxState && Module._trxState.selectDone ? 1 : 0;
})

EM_JS(void, js_get_selected_mod, (char *buf, int bufsize), {
    stringToUTF8(
        (Module._trxState && Module._trxState.selectedMod) || "",
        buf, bufsize);
})

EM_JS(int, js_get_selected_engine, (void), {
    return (Module._trxState && Module._trxState.selectedEngine) | 0;
})

EM_JS(int, js_has_touch_support, (void), {
    return navigator.maxTouchPoints > 0 ? 1 : 0;
})
// clang-format on

EMSCRIPTEN_KEEPALIVE const char *trx_get_version(void)
{
    return g_TRXVersion;
}

void Shell_LoadModGameData(const char *const mod_name)
{
    js_load_mod_data(mod_name);
    while (!js_is_load_mod_done()) {
        Clock_Delay(10);
    }
}

void Shell_InitIDBFS(void)
{
    const SHELL_ARGS *const args = Shell_GetArgs();
    const char *const mod_name = args->mod != nullptr ? args->mod->name : "trx";

    char mount_path[64];
    snprintf(mount_path, sizeof(mount_path), "/persist/%s", mod_name);
    js_mount_persistence(mount_path);
    while (!js_is_mount_done()) {
        Clock_Delay(10);
    }

    js_sync_persistence_in();
    while (!js_is_sync_in_done()) {
        Clock_Delay(10);
    }
    WEBGL_LOG("[WEBGL] IDBFS ready at /persist/%s", mod_name);

    // Redirect saves directory to IDBFS-backed path.
    char saves_dir[64];
    snprintf(saves_dir, sizeof(saves_dir), "/persist/%s/saves", mod_name);
    setenv("TRX_SAVES_DIR", saves_dir, 1);
}

void Shell_WaitForUserInput(void)
{
    WEBGL_LOG("[WEBGL] Waiting for user interaction...");
    js_start_gate();
    while (!js_is_start_gate_done()) {
        Clock_Delay(50);
    }
    WEBGL_LOG("[WEBGL] User interaction received, proceeding.");
}

void Shell_ShowProfileSelector(
    char *const mod_buf, const int32_t mod_buf_size, int32_t *const engine_out)
{
    js_select_profile();
    while (!js_is_select_done()) {
        Clock_Delay(50);
    }
    js_get_selected_mod(mod_buf, mod_buf_size);
    *engine_out = js_get_selected_engine();
}

void Shell_PersistConfigToIDBFS(void)
{
    const SHELL_ARGS *const args = Shell_GetArgs();
    const char *const mod_name = args->mod != nullptr ? args->mod->name : "trx";
    const int ver = args->engine_version;
    char src[64];
    char dst[64];
    snprintf(src, sizeof(src), "/cfg/TR%dX.json5", ver);
    snprintf(dst, sizeof(dst), "/persist/%s/cfg/TR%dX.json5", mod_name, ver);
    js_sync_persistence_out(src, dst);
}

void Shell_PersistSavesToIDBFS(void)
{
    js_flush_persistence();
}

bool Shell_HasTouchSupport(void)
{
    return js_has_touch_support();
}

uint32_t Shell_GetWindowExtraFlags(void)
{
    return SDL_WINDOW_ALLOW_HIGHDPI;
}

void Shell_PostSDLInit(void)
{
    // Restrict SDL keyboard handling to the canvas element so that HTML
    // inputs elsewhere on the page receive keystrokes normally.
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
}

void Shell_SetupGLContextVersion(void)
{
    // WebGL 2.0 = OpenGL ES 3.0
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
}
