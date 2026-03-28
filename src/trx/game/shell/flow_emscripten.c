#include <trx/core/filesystem.h>
#include <trx/core/memory.h>
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

// clang-format off
EM_JS(void, js_show_start_gate, (void), {
    var gate = document.getElementById('start-gate');
    if (!gate) return;
    document.getElementById('loading').classList.add('hidden');
    gate.classList.remove('hidden');
    Module._startGateDismissed = false;

    function dismiss() {
        if (Module._startGateDismissed) return;
        Module._startGateDismissed = true;
        gate.classList.add('hidden');
        document.getElementById('canvas').focus();
        document.removeEventListener('keydown', dismiss, true);
        document.removeEventListener('mousedown', dismiss, true);
        document.removeEventListener('touchstart', dismiss, true);
    }
    Module._dismissStartGate = dismiss;
    document.addEventListener('keydown', dismiss, true);
    document.addEventListener('mousedown', dismiss, true);
    document.addEventListener('touchstart', dismiss, true);

    // Poll for gamepad button presses.
    (function poll() {
        if (Module._startGateDismissed) return;
        var gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
        for (var i = 0; i < gamepads.length; i++) {
            var gp = gamepads[i];
            if (!gp) continue;
            for (var j = 0; j < gp.buttons.length; j++) {
                if (gp.buttons[j].pressed) { dismiss(); return; }
            }
        }
        requestAnimationFrame(poll);
    })();
})

EM_JS(int, js_is_start_gate_dismissed, (void), {
    return Module._startGateDismissed ? 1 : 0;
})

// --- IDBFS persistence ---

EM_JS(void, js_init_idbfs, (const char *mount_path), {
    var path = UTF8ToString(mount_path);
    try { FS.mkdir('/persist'); } catch(e) {}
    try { FS.mkdir(path); } catch(e) {}
    try { FS.mount(IDBFS, {}, path); } catch(e) {}
    try { FS.mkdir(path + '/saves'); } catch(e) {}
    try { FS.mkdir(path + '/cfg'); } catch(e) {}
})

EM_JS(void, js_start_idbfs_sync_from_db, (void), {
    Module._idbfsSyncDone = false;
    FS.syncfs(true, function(err) {
        if (err) console.error('[IDBFS] sync from DB error:', err);
        Module._idbfsSyncDone = true;
    });
})

EM_JS(int, js_is_idbfs_sync_done, (void), {
    return Module._idbfsSyncDone ? 1 : 0;
})

EM_JS(void, js_restore_config, (const char *src, const char *dst), {
    try {
        var data = FS.readFile(UTF8ToString(src));
        FS.writeFile(UTF8ToString(dst), data);
    } catch(e) { /* no persisted config yet */ }
})

EM_JS(void, js_persist_file_and_sync, (const char *src, const char *dst), {
    var d = UTF8ToString(dst);
    try { FS.mkdirTree(d.substring(0, d.lastIndexOf('/'))); } catch(e) {}
    try {
        FS.writeFile(d, FS.readFile(UTF8ToString(src)));
    } catch(e) { console.error('[IDBFS] persist failed:', e); }
    FS.syncfs(false, function(err) {
        if (err) console.error('[IDBFS] sync error:', err);
    });
})

EM_JS(void, js_sync_idbfs_to_db, (void), {
    FS.syncfs(false, function(err) {
        if (err) console.error('[IDBFS] sync error:', err);
    });
})

EM_JS(void, js_set_touch_controls_visible, (int visible), {
    if (Module.setTouchControlsVisible) {
        Module.setTouchControlsVisible(visible);
    }
})

EM_JS(int, js_has_touch_support, (void), {
    return navigator.maxTouchPoints > 0 ? 1 : 0;
})

// --- Mod data loading ---

EM_JS(void, js_load_mod_data, (const char *mod_name), {
    var name = UTF8ToString(mod_name);
    Module._modDataLoaded = false;
    if (Module.loadModData) {
        Module.loadModData(name, function() {
            Module._modDataLoaded = true;
        });
    } else {
        Module._modDataLoaded = true;
    }
})

EM_JS(int, js_is_mod_data_loaded, (void), {
    return Module._modDataLoaded ? 1 : 0;
})

// --- Profile selector ---

EM_JS(void, js_show_profile_selector, (void), {
    Module._profileSelectionDone = false;
    Module._selectedMod = '';
    Module._selectedEngine = 0;
    if (Module.showProfileSelector) {
        Module.showProfileSelector(function(mod, engine) {
            Module._selectedMod = mod;
            Module._selectedEngine = engine;
            Module._profileSelectionDone = true;
        });
    }
})

EM_JS(int, js_is_profile_selection_done, (void), {
    return Module._profileSelectionDone ? 1 : 0;
})

EM_JS(void, js_get_selected_mod, (char *buf, int bufsize), {
    stringToUTF8(Module._selectedMod || '', buf, bufsize);
})

EM_JS(int, js_get_selected_engine, (void), {
    return Module._selectedEngine || 0;
})

// clang-format on

// setenv is POSIX but not declared under strict C standard modes.
int setenv(const char *name, const char *value, int overwrite);

void Shell_LoadModGameData(const char *const mod_name)
{
    js_load_mod_data(mod_name);
    while (!js_is_mod_data_loaded()) {
        Clock_Delay(10);
    }
}

// Parse /available_mods.txt written by the JS shell before callMain().
// Format: one entry per line, "mod_name|display_title".
static char *m_Manifest = nullptr;

static void M_EnsureManifestLoaded(void)
{
    if (m_Manifest != nullptr) {
        return;
    }
    FILE *f = fopen("/available_mods.txt", "r");
    if (f == nullptr) {
        m_Manifest = Memory_Alloc(1);
        m_Manifest[0] = '\0';
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    m_Manifest = Memory_Alloc(size + 1);
    fread(m_Manifest, 1, size, f);
    m_Manifest[size] = '\0';
    fclose(f);
}

// Find the start of a manifest line matching mod_name, returning a pointer
// to the beginning of that line, or nullptr if not found.
static const char *M_FindManifestLine(const char *const mod_name)
{
    M_EnsureManifestLoaded();
    const size_t name_len = strlen(mod_name);
    const char *p = m_Manifest;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        if (eol == nullptr) {
            eol = p + strlen(p);
        }
        // Check if line starts with "mod_name|"
        if ((size_t)(eol - p) > name_len && strncmp(p, mod_name, name_len) == 0
            && p[name_len] == '|') {
            return p;
        }
        p = *eol != '\0' ? eol + 1 : eol;
    }
    return nullptr;
}

bool Shell_IsModKnownAvailable(const char *const mod_name)
{
    return M_FindManifestLine(mod_name) != nullptr;
}

// Format: "mod_name|title|engine\n"
// Returns a pointer to the title field (between first and second '|'),
// or nullptr if not found.
const char *Shell_GetKnownModTitle(const char *const mod_name)
{
    const char *const line = M_FindManifestLine(mod_name);
    if (line == nullptr) {
        return nullptr;
    }
    const char *const pipe1 = strchr(line, '|');
    if (pipe1 == nullptr) {
        return nullptr;
    }
    const char *const title_start = pipe1 + 1;
    const char *title_end = strchr(title_start, '|');
    if (title_end == nullptr) {
        title_end = strchr(title_start, '\n');
    }
    if (title_end == nullptr) {
        title_end = title_start + strlen(title_start);
    }
    if (title_end == title_start) {
        return nullptr;
    }
    const size_t len = title_end - title_start;
    char *title = Memory_Alloc(len + 1);
    memcpy(title, title_start, len);
    title[len] = '\0';
    return title;
}

int32_t Shell_GetKnownModCount(void)
{
    M_EnsureManifestLoaded();
    int32_t count = 0;
    const char *p = m_Manifest;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        if (eol == nullptr) {
            eol = p + strlen(p);
        }
        if (eol > p && strchr(p, '|') != nullptr) {
            count++;
        }
        p = *eol != '\0' ? eol + 1 : eol;
    }
    return count;
}

const char *Shell_GetKnownModName(const int32_t index)
{
    M_EnsureManifestLoaded();
    int32_t cur = 0;
    const char *p = m_Manifest;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        if (eol == nullptr) {
            eol = p + strlen(p);
        }
        const char *pipe = strchr(p, '|');
        if (eol > p && pipe != nullptr && pipe < eol) {
            if (cur == index) {
                const size_t len = pipe - p;
                char *name = Memory_Alloc(len + 1);
                memcpy(name, p, len);
                name[len] = '\0';
                return name;
            }
            cur++;
        }
        p = *eol != '\0' ? eol + 1 : eol;
    }
    return nullptr;
}

int32_t Shell_GetKnownModEngine(const char *const mod_name)
{
    const char *const line = M_FindManifestLine(mod_name);
    if (line == nullptr) {
        return 0;
    }
    // Find the second '|' to get the engine field
    const char *const pipe1 = strchr(line, '|');
    if (pipe1 == nullptr) {
        return 0;
    }
    const char *const pipe2 = strchr(pipe1 + 1, '|');
    if (pipe2 == nullptr) {
        return 0;
    }
    return atoi(pipe2 + 1);
}

void Shell_InitIDBFS(void)
{
    const SHELL_ARGS *const args = Shell_GetArgs();
    const char *mod_name = args->mod != nullptr ? args->mod->name : "trx";

    char mount_path[64];
    snprintf(mount_path, sizeof(mount_path), "/persist/%s", mod_name);
    js_init_idbfs(mount_path);

    js_start_idbfs_sync_from_db();
    while (!js_is_idbfs_sync_done()) {
        Clock_Delay(10);
    }
    WEBGL_LOG("[WEBGL] IDBFS ready at /persist/%s", mod_name);

    // Redirect saves directory to IDBFS-backed path.
    char saves_dir[64];
    snprintf(saves_dir, sizeof(saves_dir), "/persist/%s/saves", mod_name);
    setenv("TRX_SAVES_DIR", saves_dir, 1);

    // Restore persisted user config to /cfg/ (before Config_Read runs).
    const int ver = args->engine_version;
    char persist_cfg[64];
    char cfg_path[64];
    snprintf(
        persist_cfg, sizeof(persist_cfg), "/persist/%s/cfg/TR%dX.json5",
        mod_name, ver);
    snprintf(cfg_path, sizeof(cfg_path), "/cfg/TR%dX.json5", ver);
    js_restore_config(persist_cfg, cfg_path);
}

void Shell_WaitForUserInput(void)
{
    WEBGL_LOG("[WEBGL] Showing start gate, waiting for user interaction...");
    js_show_start_gate();
    while (!js_is_start_gate_dismissed()) {
        Clock_Delay(50);
    }
    WEBGL_LOG("[WEBGL] User interaction received, proceeding.");
}

static void M_WaitForProfileSelection(void)
{
    js_show_profile_selector();
    while (!js_is_profile_selection_done()) {
        Clock_Delay(50);
    }
}

void Shell_ShowProfileSelector(
    char *mod_buf, const int32_t mod_buf_size, int32_t *engine_out)
{
    M_WaitForProfileSelection();
    js_get_selected_mod(mod_buf, mod_buf_size);
    *engine_out = js_get_selected_engine();
}

void Shell_PersistConfigToIDBFS(void)
{
    const SHELL_ARGS *const args = Shell_GetArgs();
    const char *mod_name = args->mod != nullptr ? args->mod->name : "trx";
    const int ver = args->engine_version;
    char src[64];
    char dst[64];
    snprintf(src, sizeof(src), "/cfg/TR%dX.json5", ver);
    snprintf(dst, sizeof(dst), "/persist/%s/cfg/TR%dX.json5", mod_name, ver);
    js_persist_file_and_sync(src, dst);
}

void Shell_PersistSavesToIDBFS(void)
{
    js_sync_idbfs_to_db();
}

bool Shell_HasTouchSupport(void)
{
    return js_has_touch_support();
}

void Shell_SetTouchControlsVisible(const bool visible)
{
    js_set_touch_controls_visible(visible);
}

uint32_t Shell_GetWindowExtraFlags(void)
{
    return SDL_WINDOW_ALLOW_HIGHDPI;
}

void Shell_PostSDLInit(void)
{
    // Restrict SDL keyboard handling to the canvas element so that HTML
    // form inputs (e.g. profile creation dialog) receive keystrokes
    // normally.  A document-level forwarder in shell.html re-dispatches
    // keyboard events to the canvas when the game is running.
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
}

void Shell_SetupGLContextVersion(void)
{
    // WebGL 2.0 = OpenGL ES 3.0
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
}
