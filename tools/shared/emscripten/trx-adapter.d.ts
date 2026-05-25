// Type definitions for the TRX WebGL engine loader.

/**
 * Profile selection returned by the selectProfile adapter.
 *
 * A result with an empty `mod` string (and any `engine` value) signals
 * the engine should exit — the wrapper's `run()` promise resolves.
 */
export interface TrxProfileSelection {
    mod: string;
    engine: number;
}

/**
 * Callbacks the engine invokes at well-defined lifecycle points. All
 * async methods must return a Promise; the engine awaits them via
 * EM_ASYNC_JS.
 */
export interface TrxAdapters {
    /**
     * Resolve once the user has interacted with the page. Called before
     * any audio output to satisfy browser autoplay policies.
     */
    startGate: () => Promise<void>;

    /**
     * Mount a persistent filesystem (e.g. IDBFS) at the given path.
     * The engine will subsequently read/write saves and config under it.
     */
    mountPersistence: (mountPath: string) => Promise<void>;

    /**
     * Populate the mounted persistent filesystem with its current
     * backing-store contents. Called once after mountPersistence.
     */
    syncPersistenceIn: () => Promise<void>;

    /**
     * Flush a config/save file to the persistent store. Fire-and-forget;
     * the engine does not await this. Adapters may coalesce.
     */
    syncPersistenceOut: (srcPath: string, dstPath: string) => void;

    /**
     * Flush any pending persistent writes to the backing store.
     * Fire-and-forget; used after save-game writes.
     */
    flushPersistence: () => void;

    /**
     * Called on mid-session mod switches. Adapter stages the mod's game
     * data into Module.FS under games/<modName>/ and resolves. Typically
     * a no-op when the adapter pre-staged all data before engine start.
     */
    loadModData: (modName: string) => Promise<void>;

    /**
     * Called from the engine's Exit Game flow. Return `{mod:"",
     * engine:0}` to let the engine exit cleanly (wrapper's run()
     * promise resolves). Return a real selection to restart into a
     * different profile without tearing down the wasm instance.
     */
    selectProfile: () => Promise<TrxProfileSelection>;
}

export interface TrxCreateOptions {
    canvas: HTMLCanvasElement;
    adapters: TrxAdapters;

    /** Override for the .wasm URL (default: "TRX.wasm"). */
    wasmUrl?: string;
    /** Override for the .data URL (default: "TRX.data"). */
    dataUrl?: string;
    /** Override for the Emscripten JS URL (default: "trx-engine.js"). */
    jsUrl?: string;

    /** Optional stdout handler; default logs to console. */
    onStdout?: (text: string) => void;
    /** Optional stderr handler; default warns to console. */
    onStderr?: (text: string) => void;
}

export interface TrxEngine {
    canvas: HTMLCanvasElement;
    /** Direct access to Emscripten's FS object. */
    fs: unknown;
    /** Version string baked into the wasm at build time. */
    version: string;

    /**
     * Start the engine with argv. Resolves when main() returns (e.g.
     * after Exit Game with a `{mod:""}` selectProfile result).
     */
    run(args: string[]): Promise<number>;

    /**
     * Tell the engine to re-query the canvas dimensions. Call after any
     * external canvas resize.
     */
    syncViewport(): void;
}

export function createTRX(options: TrxCreateOptions): Promise<TrxEngine>;
