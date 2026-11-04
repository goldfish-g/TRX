#include <trx/core/file.h>
#include <trx/core/filesystem.h>
#include <trx/core/memory.h>
#include <trx/core/strings.h>
#include <trx/debug.h>
#include <trx/game/shell/host.h>
#include <trx/version.h>

#include <SDL2/SDL.h>
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

// Each game keeps its files under a directory of its own, which the page backs
// with storage that outlives the session, such as IndexedDB.
#define M_STORAGE_ROOT "/persist"

static char *m_StorageDir = nullptr;

// The browser lets the page play sound for good after the player's first
// gesture, so a game started again, as after switching games, carries on.
static bool m_HasStarted = false;

// The page answers through the adapters it hands the module (see
// tools/shared/emscripten/trx-adapter.d.ts). Awaiting one suspends the game
// through Asyncify until its promise settles; a rejected promise is reported
// and the game carries on.
// clang-format off
EM_ASYNC_JS(void, Shell_Host_AwaitAdapter, (const char *name, const char *arg), {
    const adapter = UTF8ToString(name);
    try {
        await Module.trxAdapters[adapter](arg ? UTF8ToString(arg) : undefined);
    } catch (error) {
        err('trxAdapters.' + adapter + ' failed: ' + error);
    }
})

EM_JS(void, Shell_Host_CallAdapter, (const char *name), {
    Module.trxAdapters[UTF8ToString(name)]();
})
// clang-format on

// Where the storage keeps a file the game reads and writes elsewhere, or
// nullptr where the storage holds the file itself or is not open.
static char *M_GetStoredPath(const char *const path)
{
    if (m_StorageDir == nullptr
        || strncmp(path, m_StorageDir, strlen(m_StorageDir)) == 0) {
        return nullptr;
    }
    return String_Format("%s/cfg/%s", m_StorageDir, FS_GetBaseName(path));
}

static void M_CopyFile(const char *const src, const char *const dst)
{
    char *data = nullptr;
    size_t size = 0;
    if (!SHOULD(FS_Load(src, &data, &size))) {
        return;
    }
    TRX_FILE *fp = nullptr;
    if (SHOULD(FS_EnsureParentDirectories(dst))
        && SHOULD(File_OpenPath(dst, FILE_OPEN_WRITE, &fp))) {
        File_WriteData(fp, data, size);
        File_Close(fp);
    }
    Memory_FreePointer(&data);
}

// Lets the page find the version of the engine it loaded.
EMSCRIPTEN_KEEPALIVE const char *trx_get_version(void)
{
    return g_TRXVersion;
}

void Shell_Host_SetupSDL(void)
{
    // Keys typed into the rest of the page stay with the page.
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
}

uint32_t Shell_Host_GetWindowFlags(void)
{
    return SDL_WINDOW_ALLOW_HIGHDPI;
}

SHELL_SIZE Shell_Host_GetWindowSize(SDL_Window *const window)
{
    // On a high density display the canvas holds more pixels than the page
    // gives it room for, and the viewport has to cover all of them.
    SHELL_SIZE result;
    SDL_GL_GetDrawableSize(window, &result.w, &result.h);
    return result;
}

bool Shell_Host_OwnsWindow(void)
{
    return false;
}

void Shell_Host_OpenStorage(const SHELL_MOD *const mod)
{
    if (mod == nullptr) {
        return;
    }
    Memory_FreePointer(&m_StorageDir);
    m_StorageDir = String_Format("%s/%s", M_STORAGE_ROOT, mod->name);
    Shell_Host_AwaitAdapter("mountPersistence", m_StorageDir);
    Shell_Host_AwaitAdapter("syncPersistenceIn", nullptr);

    // The saves live in the storage outright.
    char *saves_dir = String_Format("%s/saves", m_StorageDir);
    setenv("TRX_SAVES_DIR", saves_dir, true);
    Memory_FreePointer(&saves_dir);
}

void Shell_Host_RestoreFile(const char *const path)
{
    char *stored_path = M_GetStoredPath(path);
    if (stored_path != nullptr && FS_Exists(stored_path)) {
        M_CopyFile(stored_path, path);
    }
    Memory_FreePointer(&stored_path);
}

void Shell_Host_KeepFile(const char *const path)
{
    char *stored_path = M_GetStoredPath(path);
    if (stored_path != nullptr) {
        M_CopyFile(path, stored_path);
    }
    Memory_FreePointer(&stored_path);
    Shell_Host_CallAdapter("flushPersistence");
}

void Shell_Host_WaitForStart(void)
{
    if (!m_HasStarted) {
        Shell_Host_AwaitAdapter("startGate", nullptr);
        m_HasStarted = true;
    }
}
