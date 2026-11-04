## Building for the web

TRX can be built for web browsers, where it runs as WebAssembly and draws
through WebGL 2. The build is the engine alone: it brings no page of its own,
and the page that hosts it supplies the game data and answers what the engine
asks of it. [trx-webgl-shell](https://github.com/goldfish-g/trx-webgl-shell)
is a page that does so.

## Building TRX

Install `docker` and [just](https://github.com/casey/just), then build and
package the web target:

```bash
just trx-package-webgl target='release' -o build/web --no-zip
```

The package holds:

| File               | What it is                                                  |
|--------------------|-------------------------------------------------------------|
| `trx-adapter.js`   | The loader the page imports; it exports `createTRX()`.      |
| `trx-adapter.d.ts` | The types of the loader and of the adapters the page gives. |
| `trx-engine.js`    | The engine's JavaScript, which the loader loads.            |
| `TRX.wasm`         | The engine.                                                 |
| `TRX.data`         | The files TRX ships with, as in `data/trx/ship/`.           |

The player's own game files are never part of the build.

## Hosting the engine

The page:

1. Serves the files with `Cross-Origin-Opener-Policy: same-origin` and
   `Cross-Origin-Embedder-Policy: require-corp`. The engine decodes video and
   mixes sound on threads of its own, which need `SharedArrayBuffer`, and the
   browser offers that only to a page isolated this way.
2. Gives the engine a canvas with the id `canvas`, so that the keys the player
   types into the rest of the page stay with the page.
3. Calls `createTRX()` with the canvas and its adapters (see
   `trx-adapter.d.ts`), stages the player's game files under `games/<game>/`
   through `engine.fs`, and starts the game with `engine.run()`, such as with
   `['--mod', 'tr1']`. The engine finds the games there as the desktop game
   finds them next to itself.

The engine keeps the saves and the settings of each game under
`/persist/<game>`, where the page mounts storage that outlives the session,
such as IDBFS, which the engine hands over as `engine.idbfs`.

## How the engine runs in the browser

- The game runs a loop of its own, and the browser does not let a page wait.
  Emscripten's Asyncify unwinds the game whenever it waits for its next frame
  and rewinds it once the page has had its turn, so the loop runs unchanged.
  `Clock_Delay` is where the game waits.
- Where the engine differs by platform, it builds from `<module>_emscripten.c`
  in place of `<module>_generic.c`, such as `game/shell/host_*.c`, which
  answers what the page owns: the window, the storage and when to start.
- The renderer reaches OpenGL through `trx/gl/api.h`. WebGL 2 is OpenGL ES 3.0,
  which lacks some of what the desktop renderer uses, so in the browser:
  - instances draw one at a time rather than in batches;
  - affine mapping has no effect, as WebGL interpolates every texture
    coordinate with perspective and has no geometry stage to subdivide with;
  - the picture is not antialiased with multisampling;
  - there are no GPU timings in the performance overlay.
