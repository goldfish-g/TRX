# TRX WebGL Engine Build Guide

This document covers how to build the **TRX WebGL engine** (WASM binary +
JS loader) that any web frontend can consume. The engine build itself is
deliberately headless: it ships no HTML shell, no profile UI, and no
game-data upload flow. Frontends build their own integrations against
the documented adapter contract.

For the adapter API, see
[`tools/shared/emscripten/trx-adapter.d.ts`](../tools/shared/emscripten/trx-adapter.d.ts).

## Prerequisites

### 1. Emscripten SDK

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

### 2. Build tools

- **Meson** >= 1.3.0
- **Ninja** (build backend)

```bash
pip install meson ninja
```

### 3. Dependencies

Most deps come from Emscripten ports:

| Dependency  | Status                 | Notes                                   |
|-------------|------------------------|-----------------------------------------|
| SDL2        | Emscripten port        | `-sUSE_SDL=2`                           |
| zlib        | Emscripten port        | `-sUSE_ZLIB=1`                          |
| OpenGL ES 3 | Built-in               | `-sFULL_ES3=1` (WebGL 2.0)              |
| GLEW        | Not needed             | GL ES headers provided by Emscripten    |
| Lua         | Build headers locally  | or skip                                 |
| PCRE2       | Optional               | Build from source or skip               |
| FFmpeg      | Not available          | Replaced by in-tree audio/image/video decoders |

## Building

```bash
source /path/to/emsdk/emsdk_env.sh
./tools/build_webgl.sh release      # optimized
./tools/build_webgl.sh debug        # with assertions
./tools/build_webgl.sh debugoptim   # optimized with symbols
```

Output lives in `build/webgl/`:

```
build/webgl/
├── trx-adapter.js    Public loader (exports createTRX)
├── trx-adapter.d.ts  TypeScript types for the adapter contract
├── trx-engine.js     Emscripten JS glue (loaded internally by the adapter)
├── TRX.wasm          WebAssembly binary
└── TRX.data          Engine-authored ship assets (gameflow, catalogs,
                      injections, shaders, images) for TR1/TR2/TR3
```

**User game data** (level files, music, FMV, audio WADs) is never
bundled into the engine build. Frontends are responsible for sourcing
and staging it into the WASM runtime's filesystem. For a reference
React frontend that consumes this build, see the companion
`trx-webgl-shell` repo.

### Manual build

```bash
source /path/to/emsdk/emsdk_env.sh
meson setup \
    --cross-file tools/shared/emscripten/emscripten_cross.ini \
    --buildtype release \
    -Dstaticdeps=false \
    -Dwebgl_vfs_stage=/path/to/vfs_stage \
    build/webgl src/
meson compile -C build/webgl TRX
```

The `webgl_vfs_stage` directory must contain engine ship assets
(`cfg/`, `cfg/shaders/`, and `games/<mod>/` trees).
`tools/build_webgl.sh` populates this automatically.

## The adapter contract

The engine calls into the frontend via a single `Module.trxAdapters`
object installed at load time. All async methods return Promises; the C
side parks a per-call `done` flag on `Module._trxState`, kicks the
Promise via `EM_JS`, and polls the flag from a `Clock_Delay` loop that
yields through Asyncify. (Why not `EM_ASYNC_JS`? Its rewind machinery is
fragile in the presence of our function-pointer-heavy game-flow
dispatch; polling on top of the already-whitelisted Asyncify yield is
more robust.)

| Method                        | When called                                   | Purpose |
|-------------------------------|-----------------------------------------------|---------|
| `startGate()`                 | Before audio output                           | Resolve on first user gesture (browser autoplay policy). |
| `mountPersistence(mountPath)` | Shell boot                                    | Frontend mounts IDBFS (or equivalent) at `/persist/<mod>`. |
| `syncPersistenceIn()`         | After mount                                   | Frontend populates the mount with its backing-store contents. |
| `syncPersistenceOut(src,dst)` | On config write                               | Frontend copies `src` → `dst` in the mount, then flushes. |
| `flushPersistence()`          | On save-game write                            | Frontend flushes the mount to the backing store. |
| `loadModData(modName)`        | On mid-session mod switch                     | Frontend stages the target mod's files under `games/<modName>/`. |
| `selectProfile()`             | On Exit Game                                  | Return `{mod:"", engine:0}` to let the engine exit. |

The frontend writes files directly into `Module.FS` at engine-expected
paths — there is no wrapper API hiding the filesystem.

Mod discovery is a desktop-style filesystem scan: the engine looks at
`games/<mod>/gameflow.json5` under the VFS. Any mod the frontend stages
before calling `engine.run()` is picked up automatically. There is no
separate "known mods" manifest.

## Running the engine

The engine build is headless — it expects a frontend to provide the
adapter implementations described in `trx-adapter.d.ts`. To smoke-test a build,
drop the artifact files into a frontend that consumes the adapter API
(the `trx-webgl-shell` repo is the reference implementation) and run
that frontend's dev server.

Any custom frontend must serve its pages with
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` — pthreads + SharedArrayBuffer
require cross-origin isolation.

## FMV cutscenes

FFmpeg cannot be compiled for the web. FMV playback routes through the
browser's `<video>` element (`src/trx/game/fmv_emscripten.c`). Convert
original FMV files to H.264 MP4 before packaging:

```bash
ffmpeg -i upscaled.ogv -i original.fmv \
  -map 0:v -map 1:a \
  -c:v libx264 -preset medium -crf 23 -profile:v main -pix_fmt yuv420p \
  -c:a aac -b:a 128k -movflags +faststart \
  output.mp4
```

## Architecture notes

### Audio

FFmpeg is unavailable. The WebGL build uses header-only C decoders:

| Library        | Format    | License       | Purpose                     |
|----------------|-----------|---------------|-----------------------------|
| `dr_wav.h`     | WAV/RIFF  | Public domain | Sound effects + WAV music   |
| `dr_flac.h`    | FLAC      | Public domain | Music streaming             |
| `dr_mp3.h`     | MP3       | MIT           | Music streaming             |
| `stb_vorbis.c` | OGG Vorbis| Public domain | Music streaming             |

These live under `src/trx/engine/vendor/`.

### Images

| Library              | Formats                  | Notes                          |
|----------------------|--------------------------|--------------------------------|
| `stb_image.h`        | PNG, JPEG, BMP, GIF, TGA | Header-only                    |
| `stb_image_resize2.h`| —                        | Image scaling                  |
| `stb_image_write.h`  | PNG, JPEG                | Screenshot saving              |
| `libwebpdecoder.a`   | WebP                     | Pre-built, in `vendor/`        |

### Graphics pipeline

WebGL 2.0 = OpenGL ES 3.0. GLSL shaders are compiled as `#version 300
es`; `precision` qualifiers are injected automatically by
`program.c`. The compatibility shim at `src/trx/gl/gl_webgl_compat.h`
maps desktop GL concepts to ES equivalents
(`glMapBuffer` → `glMapBufferRange`, `glPolygonMode` → no-op, etc.).

### Asyncify

The engine's synchronous control flow is preserved via Emscripten's
Asyncify feature. See [WEBGL_ASYNCIFY.md](WEBGL_ASYNCIFY.md) for
details, including the `ASYNCIFY_ADD` whitelist needed for
function-pointer dispatch.

## Key source files

| File | Purpose |
|------|---------|
| `tools/build_webgl.sh` | Engine build script |
| `tools/shared/emscripten/emscripten_cross.ini` | Meson cross-compilation file |
| `tools/shared/emscripten/trx-adapter.js` | Hand-written JS loader / `createTRX()` |
| `tools/shared/emscripten/trx-adapter.d.ts` | Adapter contract (TS types) |
| `src/trx/game/shell/flow_emscripten.c` | Adapter EM_JS stubs + Clock_Delay polling |
| `src/trx/game/shell/mod.c` | Mod filesystem scan (same code as desktop) |
| `src/trx/gl/gl_webgl_compat.h` | OpenGL ES compatibility shim |
| `src/trx/game/fmv_emscripten.c` | FMV playback via HTML5 video |

## Browser requirements

- **WebGL 2.0** support (OpenGL ES 3.0)
- **WebAssembly** support
- **SharedArrayBuffer** (requires COOP/COEP headers)
- **IndexedDB** (if the frontend's persistence adapter uses IDBFS)
- Modern browsers: Chrome 56+, Firefox 51+, Safari 15+, Edge 79+
