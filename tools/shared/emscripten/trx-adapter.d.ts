// Type definitions for the TRX browser engine loader (trx-adapter.js).

/**
 * What the engine asks of the page that hosts it. The engine waits for every
 * returned promise to settle before it carries on; a rejected one is logged
 * and the engine carries on as if it had resolved.
 */
export interface TrxAdapters {
    /**
     * Resolves once the player has interacted with the page, before the game
     * plays any sound, as browsers allow sound only after that. Called once
     * per page, not again when the player switches games.
     */
    startGate: () => Promise<void>;

    /**
     * Mounts storage that outlives the session, such as IDBFS, at the given
     * path, which is `/persist/<game>`. The engine keeps its saves and its
     * settings for that game under it.
     */
    mountPersistence: (mountPath: string) => Promise<void>;

    /** Fills the mounted storage with what it held in earlier sessions. */
    syncPersistenceIn: () => Promise<void>;

    /**
     * Writes the mounted storage back to where it outlives the session. The
     * engine calls it after every save and every change of settings without
     * waiting for it, so it may batch the writes.
     */
    flushPersistence: () => void;
}

export interface TrxCreateOptions {
    canvas: HTMLCanvasElement;
    adapters: TrxAdapters;

    /** Where to load the engine's code from (default: "trx-engine.js"). */
    jsUrl?: string;
    /** Where to load the WebAssembly binary from (default: "TRX.wasm"). */
    wasmUrl?: string;
    /** Where to load the files the game ships with from (default: "TRX.data"). */
    dataUrl?: string;

    /** Receives the engine's standard output (default: console.log). */
    onStdout?: (text: string) => void;
    /** Receives the engine's standard error (default: console.warn). */
    onStderr?: (text: string) => void;
}

export interface TrxEngine {
    canvas: HTMLCanvasElement;
    /**
     * Emscripten's file system. The page stages the player's game data under
     * `games/<game>/` before calling run().
     */
    fs: unknown;
    /**
     * Emscripten's IndexedDB file system, for mountPersistence to mount with
     * `fs.mount(idbfs, {}, mountPath)`.
     */
    idbfs: unknown;
    /** The version of the engine. */
    version: string;

    /**
     * Starts the engine with the given command line, such as
     * `['--mod', 'tr1']`. Resolves with the exit code once the player quits.
     */
    run(args: string[]): Promise<number>;

    /** Tells the engine that the canvas changed size. */
    syncViewport(): void;
}

export function createTRX(options: TrxCreateOptions): Promise<TrxEngine>;
