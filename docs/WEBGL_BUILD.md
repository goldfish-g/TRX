# TRX WebGL Build Guide

This document describes how to build TRX (Tomb Raider engine) for the web using
Emscripten, producing a WebAssembly (WASM) + WebGL 2.0 build that runs in
modern browsers.

## Prerequisites

### 1. Emscripten SDK

Install and activate the Emscripten SDK (emsdk):

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh   # activate in current shell
```

### 2. Build Tools

- **Meson** >= 1.3.0
- **Ninja** (build backend)
- **Pillow** (Python, for PWA icon generation)

```bash
pip install meson ninja Pillow
```

### 3. Dependencies

The WebGL build uses Emscripten's built-in ports for most dependencies:

| Dependency  | Status               | Notes                                   |
|-------------|----------------------|-----------------------------------------|
| SDL2        | Emscripten port      | `-sUSE_SDL=2`                           |
| zlib        | Emscripten port      | `-sUSE_ZLIB=1`                          |
| OpenGL ES 3 | Built-in            | `-sFULL_ES3=1` (WebGL 2.0)             |
| GLEW        | Not needed           | GL ES headers provided by Emscripten    |
| Lua         | Must provide headers | Build from source with `emcc` or skip   |
| PCRE2       | Optional             | Build from source with `emcc` or skip   |
| FFmpeg      | Not available        | Replaced by lightweight C decoders + HTML5 video |

## Building

### Quick Build (local Emscripten SDK)

```bash
source /path/to/emsdk/emsdk_env.sh

./tools/build_webgl.sh release --tr1                # TR1 only
./tools/build_webgl.sh release --tr1 --tr2          # TR1 + TR2
./tools/build_webgl.sh release --tr1 --ub --tr2 --gm --tr3 --la  # all games
./tools/build_webgl.sh debug                        # upload-only (no bundled data)
./tools/build_webgl.sh debug --eruda                # with eruda mobile debugger
```

Usage: `./tools/build_webgl.sh [debug|release|debugoptim] [flags]`

Game data flags:

| Flag     | Game                  | Data source    |
|----------|-----------------------|----------------|
| `--tr1`  | Tomb Raider I         | `tr1_data/`    |
| `--ub`   | Unfinished Business   | `ub_data/`     |
| `--tr2`  | Tomb Raider II        | `tr2_data/`    |
| `--gm`   | The Golden Mask       | `gm_data/`     |
| `--tr3`  | Tomb Raider III       | `tr3_data/`    |
| `--la`   | The Lost Artifact     | `la_data/`     |

Other flags:

| Flag      | Effect                                             |
|-----------|----------------------------------------------------|
| `--eruda` | Inject eruda mobile debugger into the HTML          |

Each game data flag bundles user game data (levels, SFX, music, FMV) from the
corresponding data directory and creates a default profile for that game.
Builds with no flags produce an upload-only deployment where users create
profiles and upload their own game files.

### Docker Build (recommended for CI / reproducibility)

A Docker image based on `emscripten/emsdk` provides a self-contained build
environment:

```bash
docker run --rm -v "$PWD:/app" --user "$(id -u):$(id -g)" \
    rrdash/trx-webgl build --target release --tr1 --tr2
```

The Docker entrypoint forwards all game data flags (`--tr1`, `--ub`, `--tr2`,
`--gm`, `--tr3`, `--la`) and `--eruda` directly to `build_webgl.sh`.

### Manual Build

```bash
source /path/to/emsdk/emsdk_env.sh

# First create the VFS staging directory (normally done by build_webgl.sh)
# Then pass it to meson:
meson setup \
  --cross-file tools/shared/emscripten/emscripten_cross.ini \
  --buildtype debug \
  -Dstaticdeps=false \
  -Dwebgl_vfs_stage=/path/to/vfs_stage \
  build/webgl \
  src/

meson compile -C build/webgl TRX
```

The `webgl_vfs_stage` option points to the staging directory that the build
script populates with the `games/` directory layout.

## Output Files

After a successful build:

```
build/webgl/
├── TRX.html              # Main HTML page
├── TRX.js                # Emscripten JavaScript glue code
├── TRX.wasm              # WebAssembly binary
├── TRX.data              # Preloaded assets (config, shaders, game data)
├── gamedata.js           # Game data upload/mapping manager
├── profiles.js           # Profile manager (IndexedDB-backed)
├── shell.css             # Stylesheet
├── vendor/
│   └── fflate.min.js     # ZIP/gzip decompression library
├── manifest.webmanifest  # PWA manifest
├── sw.js                 # Service worker for offline support
├── icon-192.png          # PWA icon (192x192)
└── icon-512.png          # PWA icon (512x512)
```

The build script applies cache-busting query strings (based on the WASM hash)
to all `<script>` tags in `TRX.html`, ensuring browsers never serve stale
assets after a rebuild.

## Profile System

The WebGL build uses a profile system to manage multiple games from a single
deployment. Profiles map to game mods (tr1, tr1-ub, tr2, etc.) and store
user game data in IndexedDB.

### Default profiles

When game data flags are used at build time, default profiles are created
automatically. The `profiles_defaults.json` manifest in the VFS staging
directory specifies which default profiles to create:

```json
[
  {
    "id": "tr1",
    "name": "Tomb Raider I",
    "mod": "tr1",
    "engine": 1,
    "description": "The original adventure. Explore ancient ruins from Peru to Atlantis."
  }
]
```

### Auto-start

If exactly one default profile with bundled game data exists, the game
auto-starts without showing the profile selector (matching the single-game
build behavior).

### User profiles

Users can create profiles for any supported game, including custom levels
(TRLEs). Known mods (tr1, tr1-ub, etc.) only need user game data uploaded
(levels, SFX, music, FMV) because ship config (gameflow, strings, injections)
is always bundled. Custom level profiles need all files uploaded.

### Profile selector

The profile selector appears when:
- Multiple default profiles exist (e.g., `--tr1 --tr2` build)
- No default profiles have bundled data
- The user exits a game via the passport's "Exit Game" option

### Mod switching

When a user exits a game and picks a different profile, the engine performs
a full mod switch (cleanup, session free, restart loop) using the existing
`Shell_RequestModSwitch()` infrastructure.

## Running Locally

WebGL builds require a web server with COOP/COEP headers for SharedArrayBuffer
(needed by pthreads):

```bash
cd build/webgl
python3 -c "
import http.server
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy','same-origin')
        self.send_header('Cross-Origin-Embedder-Policy','require-corp')
        super().end_headers()
http.server.HTTPServer(('',8080),H).serve_forever()"
# Open http://localhost:8080/TRX.html
```

## Game Data

### VFS directory layout

The build creates a `games/` directory structure in the VFS:

```
games/
├── tr1/                    # Base game — config + data + user game data
│   ├── gameflow.json5
│   ├── strings*.json5
│   ├── catalog_*.csv
│   ├── inv_ring.json5
│   ├── weapons.json5
│   ├── injections/
│   ├── images/
│   ├── scripts/
│   ├── levels/             # User game data (if --tr1 flag used)
│   ├── music/
│   └── fmv/
├── tr1-ub/                 # Expansion — mod config only (falls back to tr1/)
│   ├── gameflow.json5
│   └── strings*.json5
├── tr2/                    # Same pattern as tr1/
│   └── ...
├── tr2-gm/                 # Same pattern as tr1-ub/
│   └── ...
├── tr3/
│   └── ...
└── tr3-la/
    └── ...
```

Base game directories always contain ship config + data (catalogs, injections,
images, scripts). Expansion directories only contain mod-specific config
(gameflow, strings) — they find shared data via the base game fallback in the
path resolver.

### What users must provide

These are the copyrighted game files from GOG or Steam:

- **TR1**: `data/*.phd` (levels), `data/main.sfx`, `music/Track*.flac`
- **TR2**: `data/*.tr2` (levels), `data/main.sfx`, `music/*.mp3`
- **TR3**: `data/*.tr2` (levels), `data/main.sfx`, `music/*.wav`, `audio/cdaudio.wad`

FMV cutscenes (`.mp4`) are optional; the game skips missing cutscenes
gracefully.

### Upload formats

The upload UI (`gamedata.js`) accepts:

- **Folder** — via the browser's directory picker
- **ZIP archive** — extracted with fflate (~30KB, MIT license)
- **tar.gz archive** — extracted with fflate (gzip) + minimal tar parser

The uploader auto-detects the archive root, normalizes paths to lowercase, and
maps files to the expected VFS locations (`games/<mod>/levels/`, etc.)
regardless of the original directory structure.

### IndexedDB persistence

Profile data is stored in an IndexedDB database named `trx-profiles` with
two object stores:

- `profiles` — keyed by profile ID, stores profile metadata (name, mod,
  engine, description, timestamps)
- `gamedata` — keyed by `profileId/vfsPath` (e.g., `tr1/games/tr1/levels/gym.phd`),
  values are ArrayBuffers

Save games and config are persisted via IDBFS at `/persist/<mod>/`.

## FMV Cutscenes

FFmpeg cannot be compiled for the web platform, so the desktop Video API is not
available. Instead, the WebGL build plays FMVs through the browser's native
HTML5 `<video>` element.

### Preparing FMV files

Convert original FMV files to H.264 MP4:

```bash
ffmpeg -i upscaled.ogv -i original.fmv \
  -map 0:v -map 1:a \
  -c:v libx264 -preset medium -crf 23 -profile:v main -pix_fmt yuv420p \
  -c:a aac -b:a 128k -movflags +faststart \
  output.mp4
```

Place the results in the appropriate `_data/fmv/` directory with lowercase
`.mp4` filenames.

### Runtime playback

The Emscripten FMV implementation (`fmv_emscripten.c`) creates a Blob URL from
the video data and plays it via a hidden `<video>` element. Each frame is
uploaded to a WebGL texture with `texImage2D(videoElement)` and rendered through
the `GFX_2D_Renderer` pipeline with letterbox fitting.

For builds without bundled game data, user-uploaded MP4 files are stored in
IndexedDB and loaded as blob URLs on demand. If no blob is available, the
player falls back to fetching via HTTP (for builds that include FMVs in the
`fmv/` directory).

`File_GuessExtension()` tries `.mp4` first, so the `.avi` paths in
`gameflow.json5` do not need to be changed.

## PWA Support

The build produces a Progressive Web App that can be installed on desktop and
mobile devices:

- **`manifest.webmanifest`** — generated with app name "TRX". Configures
  fullscreen landscape display.
- **`sw.js`** — service worker generated from `sw.js.in`. Precaches all static
  assets (HTML, JS, WASM, gamedata.js, profiles.js, fflate, icons). Caches
  `TRX.data` on first use for offline support. FMV streaming requests are
  passed through uncached.
- **Icons** — generated from `data/trx/icon.png` by `generate_icons.py`
  (requires Pillow). Produces 192x192 and 512x512 PNGs.

After the first successful load (whether from preloaded data or user upload),
the app works fully offline.

## Loading Screen

The loading screen shows a two-phase progress bar:

1. **Download phase (0-50%)** — downloading TRX.wasm and TRX.data
2. **Data loading phase (50-100%)** — loading game data from IndexedDB into
   the virtual filesystem

The loading screen remains visible until the engine is ready to display the
start gate ("press any key" splash), preventing any black screen gap.

## Debugging

### Eruda mobile debugger

The `--eruda` flag injects the [eruda](https://github.com/nicknisi/eruda)
mobile console into the build. This provides a floating developer tools panel
useful for debugging on mobile devices where browser DevTools are unavailable.

### Browser DevTools

Use the browser's built-in developer tools for debugging. The Sources panel
shows the WASM module and can set breakpoints in the JavaScript glue code.
Native C stack traces are not available in WebGL builds.

## Architecture Notes

### Audio

FFmpeg is not available on the web platform. The WebGL build uses four
lightweight, header-only C audio decoders that compile natively to
WebAssembly:

| Library        | Format    | License       | Purpose                     |
|----------------|-----------|---------------|-----------------------------|
| `dr_wav.h`     | WAV/RIFF  | Public domain | Sound effects + WAV music   |
| `dr_flac.h`    | FLAC      | Public domain | Music streaming             |
| `dr_mp3.h`     | MP3       | MIT           | Music streaming             |
| `stb_vorbis.c` | OGG Vorbis| Public domain | Music streaming             |

These live in `src/trx/engine/vendor/` and are compiled into two
Emscripten-specific source files:

- `audio_sample_emscripten.c` — decodes sound effects (WAV) via `dr_wav`,
  with `SDL_AudioStream` for resampling to 44100 Hz mono.
- `audio_stream_emscripten.c` — streams music in any of the four formats,
  auto-detected by magic bytes, resampled via `SDL_AudioStream`.

The SDL2 audio device and mixer callback (`audio.c`) work unchanged on
Emscripten.

### Images

The native build uses FFmpeg to decode images (title screen, loading screens,
credits). The WebGL build replaces this with:

| Library              | Formats                  | Notes                          |
|----------------------|--------------------------|--------------------------------|
| `stb_image.h`        | PNG, JPEG, BMP, GIF, TGA | Header-only, in `vendor/`      |
| `stb_image_resize2.h`| —                        | Image scaling (crop/letterbox) |
| `stb_image_write.h`  | PNG, JPEG                | Screenshot saving              |
| libwebp (decode)     | WebP                     | Pre-built static library       |

The game's background images are distributed as WebP files. Since `stb_image`
does not support WebP, the image loader tries `stb_image` first and falls back
to `libwebp` for WebP decoding.

The pre-built `libwebpdecoder.a` (compiled from Google's libwebp 1.5.0 source
with `emcc`) is checked into the repository at `src/trx/engine/vendor/`. To
rebuild it from source:

```bash
source /path/to/emsdk/emsdk_env.sh
wget https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.5.0.tar.gz
tar xzf libwebp-1.5.0.tar.gz && cd libwebp-1.5.0
mkdir build_wasm && cd build_wasm
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release \
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF \
  -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF \
  -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
  -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF \
  -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF
emmake make -j$(nproc) webpdecoder
cp libwebpdecoder.a /path/to/TRX/src/trx/engine/vendor/
```

### Graphics Pipeline

The WebGL build targets **WebGL 2.0** (OpenGL ES 3.0), which is the closest
match to the desktop OpenGL 3.3 Core Profile used by TRX.

Key adaptations:
- Shaders are compiled as **GLSL ES 3.00** (`#version 300 es`) instead of
  GLSL 3.30 (`#version 330 core`)
- `precision highp float/int` qualifiers are injected automatically
- Desktop-only calls (`glPolygonMode`, `glDrawBuffer`, `glMapBuffer`, etc.)
  are replaced with ES 3.0 equivalents or no-ops
- GLEW is replaced by direct GL ES 3 headers from Emscripten

### Compatibility Layer

The file `src/trx/gfx/gl/gl_webgl_compat.h` provides a compatibility shim
that maps desktop GL concepts to their WebGL/ES equivalents:

- `glClearDepth()` -> `glClearDepthf()`
- `glMapBuffer()` -> `glMapBufferRange()`
- `glBindFragDataLocation()` -> no-op (use `layout(location=0)` in shaders)
- `glPolygonMode()` -> no-op (wireframe not available in WebGL)
- GLEW `glewInit()` -> no-op

### Shader Compatibility

All GLSL shaders have been made compatible with both GLSL 3.30 (desktop) and
GLSL ES 3.00 (WebGL 2) by:

- Using `float()` casts for integer constants in `clamp()`/`max()` calls
- Ensuring `texture()` calls use consistent types
- Avoiding `int * float` implicit conversions
- The preprocessor in `program.c` automatically injects the correct version
  string and precision qualifiers based on the build target

## Key Source Files

| File | Purpose |
|------|---------|
| `tools/build_webgl.sh` | Main build script (unified, all games) |
| `tools/shared/emscripten/shell.html` | HTML shell template (loading, profile selector, upload, canvas) |
| `tools/shared/emscripten/profiles.js` | Profile manager (IndexedDB, mod definitions) |
| `tools/shared/emscripten/gamedata.js` | Game data upload, extraction, file mapping |
| `tools/shared/emscripten/shell.css` | Stylesheet |
| `tools/shared/emscripten/emscripten_cross.ini` | Meson cross-compilation file |
| `tools/shared/emscripten/vendor/fflate.min.js` | ZIP/gzip decompression (MIT) |
| `tools/shared/emscripten/sw.js.in` | Service worker template |
| `tools/shared/emscripten/manifest.webmanifest.in` | PWA manifest template |
| `tools/shared/emscripten/generate_icons.py` | PWA icon generator |
| `tools/shared/docker/game-webgl/Dockerfile` | Docker build image |
| `tools/shared/docker/game-webgl/entrypoint.sh` | Docker entrypoint |
| `src/trx/gfx/gl/gl_webgl_compat.h` | OpenGL ES compatibility shim |
| `src/trx/game/fmv_emscripten.c` | FMV playback via HTML5 video |
| `src/trx/game/shell/flow_emscripten.c` | Emscripten shell (IDBFS, profile selector EM_JS) |

## Known Limitations

1. **No wireframe mode** — `glPolygonMode` is not available in WebGL.
2. **Threading** — the build uses Emscripten's Asyncify for cooperative
   multitasking instead of true threads (see `docs/WEBGL_ASYNCIFY.md`).
3. **Backtraces** — native stack traces are not available; use browser
   developer tools for debugging.
4. **Large download** — music and FMV files add significant size. Building
   without game data flags avoids this by having users upload their own files.
5. **FMV format** — only H.264 MP4 is supported (browser-native decoding).
   Original `.rpl`/`.fmv` formats cannot be played.

## Browser Requirements

- **WebGL 2.0** support (OpenGL ES 3.0)
- **WebAssembly** support
- **IndexedDB** support (for game data and profile persistence)
- Modern browsers: Chrome 56+, Firefox 51+, Safari 15+, Edge 79+
